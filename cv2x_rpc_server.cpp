#include <capnp/ez-rpc.h>
#include <kj/debug.h>
#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unistd.h>  // for usleep

#include "vanetza.capnp.h"
#include "autotalks_wrapper.h"

// Thread-safe queue for received packets
struct ReceivedPacket {
    std::vector<uint8_t> data;
    uint64_t timestamp;
};

class PacketQueue {
public:
    void push(const ReceivedPacket& packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(packet);
        cv_.notify_one();
    }
    
    bool pop(ReceivedPacket& packet, uint32_t timeout_ms) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                        [this] { return !queue_.empty(); })) {
            packet = queue_.front();
            queue_.pop();
            return true;
        }
        return false;
    }
    
    bool empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    std::queue<ReceivedPacket> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
};

class LinkLayerImpl : public vanetza::rpc::LinkLayer::Server {
public:
    LinkLayerImpl() 
        : autotalks_()
        , rx_thread_running_(false)
        , listener_(nullptr)  // FIXED: Initialize as nullptr
    {
    }
    
    ~LinkLayerImpl() {
        stopRxThread();
    }
    
    bool initialize(const char* eth_interface = nullptr) {
        return autotalks_.initialize(eth_interface);
    }
    
    // RPC Method 1: identify
    kj::Promise<void> identify(IdentifyContext context) override {
        std::cout << "[RPC] identify() called" << std::endl;
        
        auto results = context.getResults();
        results.setId(0x12345678);
        results.setVersion(5019001);  // SDK version 5.19.1
        results.setInfo("Autotalks SECTON CV2X via RPC");
        
        return kj::READY_NOW;
    }
    
    // RPC Method 2: transmitData
    kj::Promise<void> transmitData(TransmitDataContext context) override {
        auto params = context.getParams();
        auto frame = params.getFrame();
        auto payload = frame.getPayload();
        
        std::cout << "[RPC] transmitData() called, size=" << payload.size() << " bytes" << std::endl;
        
        // Extract priority from CV2X parameters
        uint8_t priority = 1;  // Default PPPP_1
        auto txParams = params.getTxParams();
        if (txParams.isCv2x()) {
            priority = txParams.getCv2x().getPriority();
        }
        
        // Transmit using Autotalks SDK
        bool success = autotalks_.transmit(
            reinterpret_cast<const uint8_t*>(payload.begin()),
            payload.size(),
            priority
        );
        
        auto results = context.getResults();
        if (success) {
            results.setError(vanetza::rpc::LinkLayer::ErrorCode::OK);
            results.setMessage("Transmitted successfully");
        } else {
            results.setError(vanetza::rpc::LinkLayer::ErrorCode::INTERNAL_ERROR);
            results.setMessage("Transmission failed");
        }
        
        return kj::READY_NOW;
    }
    
    // RPC Method 3: subscribeData
    kj::Promise<void> subscribeData(SubscribeDataContext context) override {
        std::cout << "[RPC] subscribeData() called" << std::endl;
        
        auto listener = context.getParams().getListener();
        
        // Store the listener capability
        {
            std::lock_guard<std::mutex> lock(listener_mutex_);
            listener_ = listener;
        }
        
        // Start RX thread if not already running
        if (!rx_thread_running_) {
            startRxThread();
        }
        
        return kj::READY_NOW;
    }
    
    // RPC Method 4: setSourceAddress
    kj::Promise<void> setSourceAddress(SetSourceAddressContext context) override {
        std::cout << "[RPC] setSourceAddress() called" << std::endl;
        
        // For now, source address is set during initialization
        // We could add cv2x_src_l2id_set() call here if needed
        
        auto results = context.getResults();
        results.setError(vanetza::rpc::LinkLayer::ErrorCode::OK);
        
        return kj::READY_NOW;
    }
    
    // Process queued packets and send callbacks
   void processPacketQueue(kj::WaitScope& waitScope) {
    ReceivedPacket packet;
    while (packet_queue_.pop(packet, 100)) {  // 100ms timeout
        std::lock_guard<std::mutex> lock(listener_mutex_);
        try {
            auto request = listener_.onDataIndicationRequest();
            
            // Set frame data
            auto frame = request.initFrame();
            frame.setPayload(kj::arrayPtr(packet.data.data(), packet.data.size()));
            
            // Set RX parameters with timestamp
            auto rxParams = request.initRxParams();
            auto timestamp = rxParams.initTimestamp();
            timestamp.setHardware(packet.timestamp);
            
            // Send the indication (non-blocking)
            request.send().wait(waitScope);
            
            std::cout << "[RPC] Sent onDataIndication, size=" 
                      << packet.data.size() << " bytes" << std::endl;
                      
        } catch (const kj::Exception& e) {
            // Listener not available or send failed - silently skip
            // (Only log if it's not a disconnected client error)
            if (e.getType() != kj::Exception::Type::DISCONNECTED) {
                std::cerr << "[RPC] Error sending indication: " << e.getDescription().cStr() << std::endl;
            }
        }
    }
}

private:
    void startRxThread() {
        rx_thread_running_ = true;
        rx_thread_ = std::thread([this]() {
            std::cout << "[RX Thread] Started" << std::endl;
            
            uint8_t buffer[2000];
            
            while (rx_thread_running_) {
                size_t buffer_size = sizeof(buffer);
                uint64_t timestamp = 0;
                
                // Receive with 1 second timeout
                bool received = autotalks_.receive(buffer, &buffer_size, 
                                                   &timestamp, 1000);
                
                if (received) {
                    std::cout << "[RX Thread] Received " << buffer_size << " bytes" << std::endl;
                    
                    // Create packet and push to queue
                    ReceivedPacket packet;
                    packet.data.assign(buffer, buffer + buffer_size);
                    packet.timestamp = timestamp;
                    
                    packet_queue_.push(packet);
                } else {
                    // Timeout or error - just continue
                    // (cv2x_receive already logs errors)
                }
            }
            
            std::cout << "[RX Thread] Stopped" << std::endl;
        });
    }
    
    void stopRxThread() {
        if (rx_thread_running_) {
            std::cout << "[RPC] Stopping RX thread..." << std::endl;
            rx_thread_running_ = false;
            if (rx_thread_.joinable()) {
                rx_thread_.join();
            }
        }
    }

private:
    AutotalksCV2X autotalks_;
    
    // RX thread
    std::thread rx_thread_;
    std::atomic<bool> rx_thread_running_;
    
    // Packet queue
    PacketQueue packet_queue_;
    
    // Listener for callbacks - initialized as nullptr
    vanetza::rpc::LinkLayer::DataListener::Client listener_;
    std::mutex listener_mutex_;
};

int main(int argc, char* argv[]) {
    std::cout << "================================================" << std::endl;
    std::cout << "  CV2X RPC Server for Autotalks SECTON" << std::endl;
    std::cout << "================================================" << std::endl;
    
    // Parse command line arguments
    const char* listen_addr = "*:23057";
    const char* eth_interface = nullptr;
    
    if (argc > 1) {
        listen_addr = argv[1];
    }
    if (argc > 2) {
        eth_interface = argv[2];
    }
    
    std::cout << "Listen address: " << listen_addr << std::endl;
    if (eth_interface) {
        std::cout << "Ethernet interface: " << eth_interface << std::endl;
    }
    std::cout << std::endl;
    
    // Create and initialize the link layer
    auto linkLayer = kj::heap<LinkLayerImpl>();
    
    std::cout << "Initializing Autotalks CV2X..." << std::endl;
    if (!linkLayer->initialize(eth_interface)) {
        std::cerr << "Failed to initialize Autotalks CV2X" << std::endl;
        return 1;
    }
    std::cout << "Autotalks CV2X initialized successfully!" << std::endl;
    std::cout << std::endl;
    
    std::cout << "Starting RPC server on " << listen_addr << "..." << std::endl;
    
    try {
        capnp::EzRpcServer server(kj::mv(linkLayer), listen_addr);
        auto& waitScope = server.getWaitScope();
        
        std::cout << "RPC server ready! Waiting for connections..." << std::endl;
        std::cout << "Press Ctrl+C to stop" << std::endl;
        
        // Let the KJ event loop run - this is what accepts connections
        kj::NEVER_DONE.wait(waitScope);
        
    } catch (const kj::Exception& e) {
        std::cerr << "RPC server error: " << e.getDescription().cStr() << std::endl;
        return 1;
    }
    
    return 0;
}