#ifndef RSU_LOCALIZER_APPLICATION_HPP
#define RSU_LOCALIZER_APPLICATION_HPP

#include "application.hpp"
#include <vanetza/common/position_provider.hpp>
#include <boost/asio/io_context.hpp>
#include <map>
#include <chrono>

struct VehicleObservation {
    vanetza::MacAddress source_addr;
    int16_t rssi_dbm8;  // RSSI in dBm * 8
    std::chrono::steady_clock::time_point timestamp;
    vanetza::units::GeoAngle latitude;
    vanetza::units::GeoAngle longitude;
};

class RsuLocalizerApplication : public Application, private Application::PromiscuousHook
{
public:
    RsuLocalizerApplication(vanetza::PositionProvider& positioning);
    PortType port() override;
    void indicate(const DataIndication&, UpPacketPtr) override;
    Application::PromiscuousHook* promiscuous_hook() override;
    
    // Set RSU's known fixed position
    void set_rsu_position(double lat, double lon);

private:
    void tap_packet(const DataIndication&, const vanetza::UpPacket&) override;
    void report_observations();
    double rssi_to_distance(int16_t rssi_dbm8);
    
    vanetza::PositionProvider& positioning_;
    std::map<vanetza::MacAddress, VehicleObservation> observations_;
    
    // RSSI-to-distance model parameters (calibrate these!)
    double rssi_ref_ = -40.0;  // RSSI at 1 meter reference distance
    double path_loss_exponent_ = 2.5;  // Path loss exponent (2.0-4.0 typical)
};

#endif