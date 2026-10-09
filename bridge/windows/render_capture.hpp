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
    uint64_t reconcile(bool enabled,const json& outputs,double remaining_seconds);
    void stop();
    bool active() const {return stream_id_!=0;}
private:
    Control control_;
    json rig_,options_,outputs_=json::object();
    uint64_t stream_id_=0;
    bool hooks_owned_=false;
};
}
