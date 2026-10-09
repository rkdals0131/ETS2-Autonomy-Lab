#include "state_source.hpp"
#include "game_ipc.hpp"
#include "windows_support.hpp"
#include <chrono>

namespace bridge {
StateSource::StateSource(const VehicleProfile& vehicle,const json& settings):
    sensors_(vehicle,settings),configuration_(sensors_.configuration()) {
    configuration_["sensor_session"]=uuid_string();
    configuration_["vehicle"]=vehicle.configuration;
    worker_=std::jthread([this](std::stop_token stop) {
        try {
            Mapping mapping(false);
            if(!mapping.available()) throw std::runtime_error("SDK shared state unavailable");
            uint64_t last_stamp=0,last_frame=0;
            Bytes bytes;
            while(!stop.stop_requested()) {
                if(mapping.read(bytes)) {
                    auto telemetry=json::parse(bytes);
                    const auto time=telemetry.at("paused_simulation_time_us").get<uint64_t>();
                    const auto frame=telemetry.at("frame_id").get<uint64_t>();
                    if(last_frame && (time<last_stamp || (telemetry.at("timer_flags").get<uint32_t>()&1)))
                        throw std::runtime_error("SDK clock restarted; restart relay for a new sensor session");
                    if(frame!=last_frame) {
                        auto sample=std::make_shared<StateSample>();
                        sample->motion=sensors_.update(telemetry);
                        sample->telemetry=std::move(telemetry);
                        last_frame=frame;last_stamp=time;
                        std::lock_guard lock(mutex_);latest_=std::move(sample);
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        } catch(...) {std::lock_guard lock(mutex_);error_=std::current_exception();}
    });
}
StateSource::~StateSource() {
    worker_.request_stop();
    if(worker_.joinable()) worker_.join();
}
std::shared_ptr<const StateSample> StateSource::latest() const {
    std::lock_guard lock(mutex_);
    if(error_) std::rethrow_exception(error_);
    return latest_;
}
}
