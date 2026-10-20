#pragma once
#include "messages.hpp"
#include <functional>

namespace bridge {
json capture_outputs(const json& rig,const json& patterns,const Demand& demand);

// Owns only render hooks and a GPU stream. Physics observation belongs to the
// relay's longer-lived lease and remains active when this object is stopped.
class RenderCapture {
public:
    using Control=std::function<json(json)>;
    RenderCapture(Control control,const json& rig,const json& patterns,const json& settings);
    ~RenderCapture();
    // overlay: keep the HUD decal on (docs/24 §7), with or without a sensor stream.
    uint64_t reconcile(bool enabled,const json& outputs,double remaining_seconds,bool overlay=false);
    void stop();
    bool active() const {return stream_id_!=0;}
    bool overlay_active() const {return overlay_on_;}
private:
    Control control_;
    json rig_,options_,outputs_=json::object();
    uint64_t stream_id_=0;
    bool hooks_owned_=false,overlay_on_=false,overlay_unsupported_=false;
    void stop_stream();
};
}
