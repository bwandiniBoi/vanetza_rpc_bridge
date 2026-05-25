#ifndef AUTOTALKS_WRAPPER_H
#define AUTOTALKS_WRAPPER_H

#include <cstdint>
#include <cstddef>

// Forward declarations for Autotalks types - use the CORRECT struct names from SDK
struct cv2x_service_st;
struct cv2x_socket_st;
typedef struct cv2x_service_st cv2x_service_t;
typedef struct cv2x_socket_st cv2x_socket_t;

class AutotalksCV2X {
public:
    AutotalksCV2X();
    ~AutotalksCV2X();
    
    // Initialize the Autotalks SDK and create sockets
    // eth_interface: e.g., "enx0002ccf00006" or NULL for auto-detect
    bool initialize(const char* eth_interface = nullptr);
    
    // Transmit data using SPS socket
    // data: pointer to payload
    // length: payload size in bytes
    // priority: CV2X PPPP priority (0-7)
    // Returns true on success
    bool transmit(const uint8_t* data, size_t length, uint8_t priority);
    
    // Receive data (blocking with timeout)
    // buffer: output buffer for received data
    // buffer_size: in: max buffer size, out: actual received size
    // timestamp: output timestamp (if not NULL)
    // rssi: output RSSI in dBm (if not NULL)
    // src_l2id: output source L2 ID (if not NULL)
    // timeout_ms: timeout in milliseconds
    // Returns true if data received, false on timeout or error
    bool receive(uint8_t* buffer, size_t* buffer_size,
                 uint64_t* timestamp, int16_t* rssi, uint32_t* src_l2id,
                 uint32_t timeout_ms);
    
    // Check if TX is ready
    bool is_tx_ready();
    
    // Shutdown and cleanup
    void shutdown();
    
    // Check if initialized
    bool is_initialized() const { return initialized_; }

private:
    cv2x_service_t* cv2x_service_;
    cv2x_socket_t* tx_socket_;
    cv2x_socket_t* rx_socket_;
    uint32_t message_id_;
    bool initialized_;
    uint32_t ue_id_;  // Store UE ID for use in transmit
};

#endif // AUTOTALKS_WRAPPER_H