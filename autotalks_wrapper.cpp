#include "autotalks_wrapper.h"

#include <iostream>
#include <cstring>
#include <unistd.h>

// Autotalks SDK headers (C headers)
extern "C" {
#include <atlk/sdk.h>
#include <atlk/cv2x.h>
#include <atlk/cv2x_service.h>
#include <atlk/ddm.h>
#include <atlk/ddm_service.h>
#include <atlk/wdm.h>
#include <atlk/wdm_service.h>
#include <atlk/v2x_service.h>
#include <extern/time_sync.h>
#include <extern/ref_cv2x_sys.h>
}

#define RSVP_MILLIS 100

AutotalksCV2X::AutotalksCV2X()
    : cv2x_service_(nullptr)
    , tx_socket_(nullptr)
    , rx_socket_(nullptr)
    , message_id_(0)
    , initialized_(false)
    , ue_id_(0)
{
}

AutotalksCV2X::~AutotalksCV2X()
{
    shutdown();
}

bool AutotalksCV2X::initialize(const char* eth_interface)
{
    if (initialized_) {
        std::cerr << "Already initialized" << std::endl;
        return false;
    }
    
    atlk_rc_t rc;
    cv2x_configuration_t initial_cv2x_config;
    memset(&initial_cv2x_config, 0, sizeof(initial_cv2x_config));
    
    cv2x_rrc_pre_config_t rrc_config;
    unsigned ue_id = 0;
    
    std::cout << "[AutotalksCV2X] Starting initialization..." << std::endl;
    
    // Set passthrough mode
    rc = v2x_sockets_passthrough_set(1);
    if (atlk_error(rc)) {
        std::cerr << "v2x_sockets_passthrough_set failed: " << rc << std::endl;
        return false;
    }
    
    // Use the provided interface or default to /dev/spidev0.0
    const char* interface_to_use = (eth_interface != nullptr && strlen(eth_interface) > 0) 
                                    ? eth_interface 
                                    : "/dev/spidev0.0";
    
    std::cout << "[AutotalksCV2X] Using interface: " << interface_to_use << std::endl;
    
    // Initialize CV2X reference system
    rc = ref_cv2x_sys_init(interface_to_use, nullptr, &initial_cv2x_config, &rrc_config);
    if (atlk_error(rc)) {
        std::cerr << "ref_cv2x_sys_init failed: " << rc << std::endl;
        return false;
    }
    
    ue_id = ref_cv2x_ue_id_get();
    std::cout << "[AutotalksCV2X] UE ID: 0x" << std::hex << ue_id << std::dec << std::endl;
    
    // Store UE ID for later use in transmit
    ue_id_ = ue_id;
    
    // Get services
    ddm_service_t* ddm_service_ptr = nullptr;
    rc = ddm_service_get(nullptr, &ddm_service_ptr);
    if (atlk_error(rc)) {
        std::cerr << "ddm_service_get failed: " << rc << std::endl;
        goto cleanup_ref_sys;
    }
    
    // Wait for device to be ready
    std::cout << "[AutotalksCV2X] Waiting for device to be ready..." << std::endl;
    ddm_state_t state;
    do {
        rc = ddm_state_get(ddm_service_ptr, &state);
        if (atlk_error(rc)) {
            std::cerr << "ddm_state_get failed: " << rc << std::endl;
            goto cleanup_ref_sys;
        }
        if (state != DDM_STATE_READY) {
            sleep(1);
        }
    } while (state != DDM_STATE_READY);
    
    std::cout << "[AutotalksCV2X] Device ready" << std::endl;
    
    // Initialize time sync
    rc = time_sync_init(ddm_service_ptr);
    if (atlk_error(rc)) {
        std::cerr << "time_sync_init failed: " << rc << std::endl;
        goto cleanup_ref_sys;
    }
    
    // Get CV2X service
    rc = cv2x_service_get(nullptr, &cv2x_service_);
    if (atlk_error(rc)) {
        std::cerr << "cv2x_service_get failed: " << rc << std::endl;
        goto cleanup_time_sync;
    }
    
    // Set configuration
    rc = cv2x_configuration_set(&initial_cv2x_config, &rrc_config);
    if (atlk_error(rc)) {
        std::cerr << "cv2x_configuration_set failed: " << rc << std::endl;
        goto cleanup_service;
    }
    
    // Enable service
    rc = cv2x_service_enable(cv2x_service_);
    if (atlk_error(rc)) {
        std::cerr << "cv2x_service_enable failed: " << rc << std::endl;
        goto cleanup_service;
    }
    
    // Set source L2 ID
    rc = cv2x_src_l2id_set(cv2x_service_, ue_id);
    if (atlk_error(rc)) {
        std::cerr << "cv2x_src_l2id_set failed: " << rc << std::endl;
        goto cleanup_service;
    }
    
    // Create TX socket (SPS mode)
    {
        cv2x_socket_config_t socket_config = CV2X_SOCKET_CONFIG_INIT;
        rc = cv2x_socket_create(cv2x_service_, 
                               CV2X_SOCKET_TYPE_SEMI_PERSISTENT_TX,
                               &socket_config,
                               &tx_socket_);
        if (atlk_error(rc)) {
            std::cerr << "cv2x_socket_create (TX) failed: " << rc << std::endl;
            goto cleanup_service;
        }
        
        // Set socket policy for SPS
        cv2x_socket_policy_t policy;
        policy.control_interval_ms = RSVP_MILLIS;
        policy.size = 400;  // Max CAM size
        policy.priority = CV2X_PPPP_1;
        
        rc = cv2x_socket_policy_set(tx_socket_, &policy);
        if (atlk_error(rc)) {
            std::cerr << "cv2x_socket_policy_set failed: " << rc << std::endl;
            goto cleanup_tx_socket;
        }
    }
    
    // Create RX socket
    {
        cv2x_socket_config_t socket_config = CV2X_SOCKET_CONFIG_INIT;
        rc = cv2x_socket_create(cv2x_service_,
                               CV2X_SOCKET_TYPE_RX,
                               &socket_config,
                               &rx_socket_);
        if (atlk_error(rc)) {
            std::cerr << "cv2x_socket_create (RX) failed: " << rc << std::endl;
            goto cleanup_tx_socket;
        }
    }
    
    std::cout << "[AutotalksCV2X] Initialization complete!" << std::endl;
    initialized_ = true;
    return true;

cleanup_tx_socket:
    if (tx_socket_) {
        cv2x_socket_delete(tx_socket_);
        tx_socket_ = nullptr;
    }
    
cleanup_service:
    if (cv2x_service_) {
        cv2x_service_disable(cv2x_service_);
        cv2x_service_deinit(cv2x_service_);
        cv2x_service_ = nullptr;
    }
    
cleanup_time_sync:
    time_sync_deinit();
    
cleanup_ref_sys:
    v2x_sockets_passthrough_set(0);
    ref_cv2x_sys_deinit();
    
    return false;
}

bool AutotalksCV2X::transmit(const uint8_t* data, size_t length, uint8_t priority)
{
    if (!initialized_ || !tx_socket_) {
        std::cerr << "Not initialized" << std::endl;
        return false;
    }
    
    if (!is_tx_ready()) {
        std::cerr << "TX not ready" << std::endl;
        return false;
    }
    
    cv2x_send_params_t send_params;
    memset(&send_params, 0, sizeof(send_params));
    send_params.message_id = ++message_id_;
    send_params.src_l2id = ue_id_;  // Set source L2 ID!
    
    std::cout << "[TX] Sending with src_l2id: 0x" << std::hex 
              << send_params.src_l2id << std::dec << std::endl;
    
    atlk_rc_t rc = cv2x_send(tx_socket_, data, length, &send_params);
    if (atlk_error(rc)) {
        std::cerr << "cv2x_send failed: " << rc << std::endl;
        return false;
    }
    
    return true;
}

bool AutotalksCV2X::receive(uint8_t* buffer, size_t* buffer_size,
                           uint64_t* timestamp, int16_t* rssi, uint32_t* src_l2id,
                           uint32_t timeout_ms)
{
    if (!initialized_ || !rx_socket_) {
        std::cerr << "[RX] Not initialized" << std::endl;
        return false;
    }
    
    cv2x_receive_params_t rx_params;
    memset(&rx_params, 0, sizeof(rx_params));
    
    atlk_wait_t receive_timeout;
    receive_timeout.wait_type = ATLK_WAIT_TYPE_INTERVAL;
    receive_timeout.wait_usec = timeout_ms * 1000;
    
    atlk_rc_t rc = cv2x_receive(rx_socket_, buffer, buffer_size, 
                                &rx_params, &receive_timeout);
    
    if (rc == ATLK_E_TIMEOUT) {
        return false;
    }
    
    if (atlk_error(rc)) {
        std::cerr << "[RX] cv2x_receive failed: " << rc << std::endl;
        return false;
    }
    
    if (timestamp != nullptr) {
        *timestamp = rx_params.receive_time;
    }
    
    // Extract RSSI from first antenna (index 0)
    if (rssi != nullptr) {
        // CV2X RSSI is in dbm8 format (dBm * 8)
        // Convert to dBm by dividing by 8
        *rssi = static_cast<int16_t>(rx_params.rssi[0] / 8);
        std::cout << "[RX] RSSI: " << (int)*rssi << " dBm (raw: " 
                  << rx_params.rssi[0] << ")" << std::endl;
    }
    
    // Extract source L2 ID
    if (src_l2id != nullptr) {
        *src_l2id = rx_params.l2id_src;
        std::cout << "[RX] Source L2 ID: 0x" << std::hex << rx_params.l2id_src 
                  << std::dec << std::endl;
    }
    
    return true;
}

bool AutotalksCV2X::is_tx_ready()
{
    return cv2x_tx_is_ready();
}

void AutotalksCV2X::shutdown()
{
    if (!initialized_) {
        return;
    }
    
    std::cout << "[AutotalksCV2X] Shutting down..." << std::endl;
    
    if (rx_socket_) {
        cv2x_socket_delete(rx_socket_);
        rx_socket_ = nullptr;
    }
    
    if (tx_socket_) {
        cv2x_socket_delete(tx_socket_);
        tx_socket_ = nullptr;
    }
    
    if (cv2x_service_) {
        cv2x_service_disable(cv2x_service_);
        cv2x_service_deinit(cv2x_service_);
        cv2x_service_ = nullptr;
    }
    
    time_sync_deinit();
    v2x_sockets_passthrough_set(0);
    ref_cv2x_sys_deinit();
    
    initialized_ = false;
    std::cout << "[AutotalksCV2X] Shutdown complete" << std::endl;
}