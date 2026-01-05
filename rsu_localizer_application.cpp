#include "rsu_localizer_application.hpp"
#include <vanetza/btp/ports.hpp>
#include <vanetza/asn1/cam.hpp>
#include <vanetza/asn1/packet_visitor.hpp>
#include <iostream>
#include <cmath>

using namespace vanetza;

RsuLocalizerApplication::RsuLocalizerApplication(PositionProvider& positioning)
    : positioning_(positioning)
{
}

RsuLocalizerApplication::PortType RsuLocalizerApplication::port()
{
    return host_cast<uint16_t>(0);  // Promiscuous - receive all packets
}

Application::PromiscuousHook* RsuLocalizerApplication::promiscuous_hook()
{
    return this;
}

void RsuLocalizerApplication::set_rsu_position(double lat, double lon)
{
    std::cout << "RSU fixed position set to: " << lat << ", " << lon << std::endl;
}

void RsuLocalizerApplication::tap_packet(const DataIndication& indication, 
                                         const UpPacket& packet)
{
    // Extract source MAC address
    vanetza::MacAddress source_addr = indication.source;
    
    // TODO: Extract RSSI from indication once propagated through Vanetza
    // For now, using placeholder - you'll need to modify Vanetza's DataIndication
    int16_t rssi_dbm8 = -600;  // Placeholder: -75 dBm * 8
    
    // Try to decode as CAM to get vehicle GPS position
    asn1::PacketVisitor<asn1::Cam> visitor;
    std::shared_ptr<const asn1::Cam> cam = boost::apply_visitor(visitor, packet);
    
    if (cam) {
        VehicleObservation obs;
        obs.source_addr = source_addr;
        obs.rssi_dbm8 = rssi_dbm8;
        obs.timestamp = std::chrono::steady_clock::now();
        
        // Extract GPS position from CAM
        const BasicContainer_t& basic = cam->cam.camParameters.basicContainer;
        obs.latitude = basic.referencePosition.latitude * 1e-7 * units::degree;
        obs.longitude = basic.referencePosition.longitude * 1e-7 * units::degree;
        
        observations_[source_addr] = obs;
        
        double rssi_dbm = rssi_dbm8 / 8.0;
        double distance = rssi_to_distance(rssi_dbm8);
        
        std::cout << "Vehicle " << source_addr 
                  << " | RSSI: " << rssi_dbm << " dBm"
                  << " | Est. Distance: " << distance << " m"
                  << " | GPS: " << obs.latitude.value() << ", " 
                  << obs.longitude.value() << std::endl;
    }
}

double RsuLocalizerApplication::rssi_to_distance(int16_t rssi_dbm8)
{
    double rssi_dbm = rssi_dbm8 / 8.0;
    
    // Log-distance path loss model: RSSI = RSSI_ref - 10*n*log10(d/d_ref)
    // Solving for d: d = d_ref * 10^((RSSI_ref - RSSI)/(10*n))
    
    double distance = 1.0 * pow(10.0, (rssi_ref_ - rssi_dbm) / (10.0 * path_loss_exponent_));
    
    return distance;
}

void RsuLocalizerApplication::indicate(const DataIndication& indication, UpPacketPtr packet)
{
    // Not used in promiscuous mode
}