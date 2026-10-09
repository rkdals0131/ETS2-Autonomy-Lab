#include "render_capture.hpp"

namespace bridge {
json capture_outputs(const json& rig,const json& patterns,const Demand& demand) {
    json outputs=json::object();
    for(const auto& view:rig.at("views")) {
        const int slot=view.at("slot");const auto name=view.at("camera_id").get<std::string>();
        const auto topic="/ets2/camera/"+name;auto selected=json::array();
        if(demand.contains(topic+"/image_raw")) selected.push_back("color");
        if(demand.contains(topic+"/depth/image_raw")) selected.push_back("depth");
        if(demand.contains(topic+"/perception/image_raw")) selected.push_back("preview");
        if(demand.contains(topic+"/preview/image/compressed")) selected.push_back("display");
        if(demand.contains("/ets2/ground_truth/"+name+"/objects") || demand.contains("/ets2/ground_truth/"+name+"/markers")) selected.push_back("metadata");
        if(demand.contains(topic+"/camera_info") || demand.contains(topic+"/preview/camera_info") || demand.contains(topic+"/perception/camera_info") ||
           demand.contains("/tf") || demand.contains("/ets2/frame_info") || demand.contains("/ets2/frame_info/exposure")) selected.push_back("pose");
        const auto mirror="mirror"+std::to_string(slot);
        if(patterns.contains(mirror)) {
            const auto lidar="/ets2/lidar/"+patterns.at(mirror).at("name").get<std::string>();
            if(demand.contains(lidar+"/points") || demand.contains(lidar+"/preview/points")) selected.push_back("lidar");
        }
        if(!selected.empty()) outputs[mirror]=std::move(selected);
    }
    return outputs;
}
RenderCapture::RenderCapture(Control control,const json& rig,const json& patterns,const json& settings):
    control_(std::move(control)),rig_(rig),options_{{"format","ros"},{"shared_gpu",settings.value("shared_gpu",true)},
        {"hz",settings.value("camera_hz",30.0)},{"lidar_hz",settings.value("lidar_hz",10.0)},
        {"preview_hz",settings.value("preview_hz",10.0)},
        {"auto_exposure",settings.value("auto_exposure",true)},{"color_gain",settings.value("color_gain",1.0)},{"lidars",patterns}} {
    rig_["capture_warmup"]=settings.value("capture_warmup",true);
}
RenderCapture::~RenderCapture() {try{stop();}catch(...) {}}
void RenderCapture::stop() {
    if(hooks_owned_) {
        control_({{"cmd","render_probe"},{"enabled",false}});
        hooks_owned_=false;
    }
    stream_id_=0;outputs_=json::object();
}
uint64_t RenderCapture::reconcile(bool enabled,const json& outputs,double remaining_seconds) {
    if(!enabled || outputs.empty()) {stop();return 0;}
    if(stream_id_ && outputs==outputs_) return stream_id_;
    auto request=options_;request["cmd"]="stream";request["outputs"]=outputs;
    if(!stream_id_) {
        // Mark ownership before enabling so partially installed hooks are
        // released if either enabling or rig setup throws.
        hooks_owned_=true;
        control_({{"cmd","render_probe"},{"enabled",true},{"vehicle_metadata",true},{"draw_metadata",false}});
        control_(rig_);
        request["action"]="start";request["duration"]=remaining_seconds;
        stream_id_=control_(std::move(request)).at("stream_id").get<uint64_t>();
    } else {request["action"]="update";control_(std::move(request));}
    outputs_=outputs;return stream_id_;
}
}
