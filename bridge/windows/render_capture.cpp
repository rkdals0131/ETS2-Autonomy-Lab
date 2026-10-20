#include "render_capture.hpp"
#include "game_ipc.hpp"
#include <iostream>

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
        const bool pose=demand.contains(topic+"/camera_info") || demand.contains(topic+"/preview/camera_info") || demand.contains(topic+"/perception/camera_info") ||
           demand.contains("/ets2/frame_info") || demand.contains("/ets2/frame_info/exposure");
        const auto mirror="mirror"+std::to_string(slot);
        if(patterns.contains(mirror)) {
            const auto lidar="/ets2/lidar/"+patterns.at(mirror).at("name").get<std::string>();
            if(demand.contains(lidar+"/points") || demand.contains(lidar+"/preview/points")) selected.push_back("lidar");
        }
        // A /tf listener alone never starts rendering; the cabin frame rides along
        // with views captured for another output.
        if(pose || (!selected.empty() && demand.contains("/tf"))) selected.push_back("pose");
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
    rig_["native_mirrors"]=settings.value("native_mirrors",true);
}
RenderCapture::~RenderCapture() {try{stop();}catch(...) {}}
void RenderCapture::stop() {
    if(hooks_owned_) {
        control_({{"cmd","render_probe"},{"enabled",false}});
        hooks_owned_=false;
    }
    stream_id_=0;outputs_=json::object();overlay_on_=false;
}
void RenderCapture::stop_stream() {
    if(!stream_id_) return;
    // The decal stays: drop to the overlay-only hooks instead of turning all off.
    if(overlay_on_) control_({{"cmd","render_probe"},{"enabled",true},{"mode","overlay"}});
    else stop();
    stream_id_=0;outputs_=json::object();
}
uint64_t RenderCapture::reconcile(bool enabled,const json& outputs,double remaining_seconds,bool overlay) {
    if(!enabled || outputs.empty()) stop_stream();
    else if(!stream_id_ || outputs!=outputs_) {
        auto request=options_;request["cmd"]="stream";request["outputs"]=outputs;
        if(!stream_id_) {
            // Mark ownership before enabling so partially installed hooks are
            // released if either enabling or rig setup throws.
            hooks_owned_=true;
            control_({{"cmd","render_probe"},{"enabled",true},{"vehicle_metadata",true},{"draw_metadata",false},{"overlay",overlay_on_}});
            control_(rig_);
            request["action"]="start";request["duration"]=remaining_seconds;
            stream_id_=control_(std::move(request)).at("stream_id").get<uint64_t>();
        } else {request["action"]="update";control_(std::move(request));}
        outputs_=outputs;
    }
    if(overlay!=overlay_on_ && !overlay_unsupported_) {
        try {
            if(overlay) hooks_owned_=true;
            // Enable maps Local\OT_Overlay, then keeps the stream's hooks or uses overlay mode.
            control_({{"cmd","overlay"},{"action",overlay?"enable":"disable"}});
            overlay_on_=overlay;
        } catch(const CoreError& e) {
            // An older core or missing permission: the relay keeps running without the decal.
            overlay_unsupported_=true;
            std::cerr<<"HUD overlay unavailable: "<<e.what()<<std::endl;
        }
    }
    return stream_id_;
}
}
