#include <capnp/ez-rpc.h>
#include <kj/debug.h>
#include <iostream>
#include <thread>
#include <queue>
#include <mutex>
#include <atomic>

#include "vanetza.capnp.h"
#include "autotalks_wrapper.h"

// Thread-safe queue for received packets
struct ReceivedPacket {
    std::vector<uint8_t> data;
    uint64_t timestamp;
    int16_t rssi;
    uint32_t src_l2id;
};

class PacketQueue {
public:
    void push(const ReceivedPacket& packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(packet);
    }
    
    bool pop(ReceivedPacket& packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty()) {
            return false;
        }
        packet = queue_.front();
        queue_.pop();
        return true;
    }

private:
    std::queue<ReceivedPacket> queue_;
    std::mutex mutex_;
};

class LinkLayerImpl : public vanetza::rpc::LinkLayer::Server {
public:
    LinkLayerImpl() 
        : autotalks_()
        , rx_thread_running_(false)
        , listener_set_(false)
    {
    }
    
    ~LinkLayerImpl() {
        stopRxThread();
    }
    
    bool initialize(const char* eth_interface = nullptr) {
        return autotalks_.initialize(eth_interface);
    }
    
    kj::Promise<void> identify(IdentifyContext context) override {
        std::cout << "[RPC] identify() called" << std::endl;
        
        auto results = context.getResults();
        results.setId(0x12345678);
        results.setVersion(5019001);
        results.setInfo("Autotalks SECTON CV2X via RPC");
        
        // Process queue whenever we get an RPC call
        processPacketQueue();
        
        return kj::READY_NOW;
    }
    
    kj::Promise<void> transmitData(TransmitDataContext context) override {
        auto params = context.getParams();
        auto frame = params.getFrame();
        auto payload = frame.getPayload();
        
        std::cout << "[RPC] transmitData() called, size=" << payload.size() << " bytes" << std::endl;
        
        uint8_t priority = 1;
        auto txParams = params.getTxParams();
        if (txParams.isCv2x()) {
            priority = txParams.getCv2x().getPriority();
        }
        
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
        
        // Process queue on every RPC call!
        processPacketQueue();
        
        return kj::READY_NOW;
    }
    
    kj::Promise<void> subscribeData(SubscribeDataContext context) override {
        std::cout << "[RPC] subscribeData() called" << std::endl;
        
        auto listener = context.getParams().getListener();
        
        {
            std::lock_guard<std::mutex> lock(listener_mutex_);
            listener_ = kj::heap<vanetza::rpc::LinkLayer::DataListener::Client>(kj::mv(listener));
            listener_set_ = true;
        }
        
        if (!rx_thread_running_) {
            startRxThread();
        }
        
        processPacketQueue();
        
        return kj::READY_NOW;
    }
    
    kj::Promise<void> setSourceAddress(SetSourceAddressContext context) override {
        std::cout << "[RPC] setSourceAddress() called" << std::endl;
        
        auto results = context.getResults();
        results.setError(vanetza::rpc::LinkLayer::ErrorCode::OK);
        
        processPacketQueue();
        
        return kj::READY_NOW;
    }
    
    kj::Promise<void> subscribeCbr(SubscribeCbrContext context) override {
        std::cout << "[RPC] subscribeCbr() called - not implemented" << std::endl;
        
        processPacketQueue();
        
        return kj::READY_NOW;
    }
    
    // Process packets from queue - called on every RPC method
    void processPacketQueue() {
        ReceivedPacket packet;
        
        int processed = 0;
        while (processed < 20 && packet_queue_.pop(packet)) {
            std::lock_guard<std::mutex> lock(listener_mutex_);
            
            if (!listener_set_ || !listener_) {
                continue;
            }
            
            try {
                auto request = listener_->onDataIndicationRequest();
                
                // Set frame data with real source address from L2 ID
                auto frame = request.initFrame();
                
                // Convert L2 ID to MAC address (32-bit L2 ID padded to 48-bit MAC)
                uint8_t source_mac[6] = {
                    0x00, 0x00,  // Padding
                    static_cast<uint8_t>((packet.src_l2id >> 24) & 0xFF),
                    static_cast<uint8_t>((packet.src_l2id >> 16) & 0xFF),
                    static_cast<uint8_t>((packet.src_l2id >> 8) & 0xFF),
                    static_cast<uint8_t>(packet.src_l2id & 0xFF)
                };
                frame.setSourceAddress(kj::arrayPtr(source_mac, 6));
                
                // Destination is broadcast for CAMs
                uint8_t dest_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                frame.setDestinationAddress(kj::arrayPtr(dest_mac, 6));
                
                frame.setPayload(kj::arrayPtr(packet.data.data(), packet.data.size()));
                
                // Set RX parameters with CV2X-specific data
                auto rxParams = request.initRxParams();
                
                // Set CV2X parameters including RSSI
                auto cv2xParams = rxParams.initCv2x();
                // RSSI from wrapper is in dBm; capnp field is dbm8 (dBm * 8)
                cv2xParams.setRssi(static_cast<int16_t>(packet.rssi * 8));
                
                // Set timestamp
                auto timestamp = rxParams.initTimestamp();
                timestamp.setHardware(packet.timestamp);
                
                // Send (fire and forget)
                auto promise = request.send();
                
                std::cout << "[RPC] Sent onDataIndication, size=" << packet.data.size() 
                          << " bytes, RSSI=" << (int)packet.rssi << " dBm, "
                          << "L2ID=0x" << std::hex << packet.src_l2id << std::dec << std::endl;
                          
            } catch (const kj::Exception& e) {
                if (e.getType() != kj::Exception::Type::DISCONNECTED) {
                    std::cerr << "[RPC] Error sending indication: " 
                              << e.getDescription().cStr() << std::endl;
                }
            }
            
            processed++;
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
                int16_t rssi = 0;
                uint32_t src_l2id = 0;
                
                bool received = autotalks_.receive(buffer, &buffer_size, 
                                                   &timestamp, &rssi, &src_l2id, 1000);
                
                if (received) {
                    std::cout << "[RX Thread] Received " << buffer_size << " bytes, "
                              << "RSSI: " << (int)rssi << " dBm, "
                              << "Src L2ID: 0x" << std::hex << src_l2id << std::dec 
                              << std::endl;
                    
                    ReceivedPacket packet;
                    packet.data.assign(buffer, buffer + buffer_size);
                    packet.timestamp = timestamp;
                    packet.rssi = rssi;
                    packet.src_l2id = src_l2id;
                    packet_queue_.push(packet);
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
    
    std::thread rx_thread_;
    std::atomic<bool> rx_thread_running_;
    
    PacketQueue packet_queue_;
    
    kj::Own<vanetza::rpc::LinkLayer::DataListener::Client> listener_;
    std::mutex listener_mutex_;
    bool listener_set_;
};

int main(int argc, char* argv[]) {
    std::cout << "================================================" << std::endl;
    std::cout << "  CV2X RPC Server for Autotalks SECTON" << std::endl;
    std::cout << "================================================" << std::endl;
    
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
    
    try {
        auto linkLayer = kj::heap<LinkLayerImpl>();
        
        std::cout << "Initializing Autotalks CV2X..." << std::endl;
        if (!linkLayer->initialize(eth_interface)) {
            std::cerr << "Failed to initialize Autotalks CV2X" << std::endl;
            return 1;
        }
        std::cout << "Autotalks CV2X initialized successfully!" << std::endl;
        std::cout << std::endl;
        
        std::cout << "Starting RPC server on " << listen_addr << "..." << std::endl;
        
        capnp::EzRpcServer server(kj::mv(linkLayer), listen_addr);
        auto& waitScope = server.getWaitScope();
        
        std::cout << "RPC server ready! Waiting for connections..." << std::endl;
        std::cout << "Press Ctrl+C to stop" << std::endl;
        
        // Just wait - queue is processed on every RPC call
        kj::NEVER_DONE.wait(waitScope);
        
    } catch (const kj::Exception& e) {
        std::cerr << "RPC server error: " << e.getDescription().cStr() << std::endl;
        return 1;
    }
    
    return 0;
}