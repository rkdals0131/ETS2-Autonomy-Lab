#include "capture_stream.hpp"
#include <algorithm>
#include <stdexcept>

namespace ot {
CaptureStream::CaptureStream(uint32_t mask,const CaptureOptions& options,double hz,double duration,
        const std::atomic<uint64_t>& presents,uint64_t session,Transport& publisher):
    options_(options),hz_(hz),duration_(duration),presents_(presents),session_(session),publisher_(publisher),
    stop_event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    if(!stop_event_) throw std::runtime_error("Cannot create capture stream stop event");
    for(unsigned camera=0;camera<6;++camera) if(mask&(1u<<camera)) {
        const auto name="mirror"+std::to_string(camera);names_.push_back(name);camera_indices_.push_back(camera);
        for(auto& slot:slots_) slot.cameras.push_back(std::make_unique<GpuCapture>(name));
    }
}
CaptureStream::~CaptureStream() {stop();}
void CaptureStream::start() {
    running_=true;
    try {worker_=std::thread([this]{run();});}
    catch(...) {running_=false;throw;}
}
void CaptureStream::stop() noexcept {
    SetEvent(stop_event_.h);
    if(worker_.joinable()) worker_.join();
}
void CaptureStream::observe(ID3D11DeviceContext* context,uint32_t count,const uintptr_t* targets,
        uint64_t sequence,uint64_t sdk_frame,uint64_t render_frame,const json* pass) noexcept {
    for(auto& slot:slots_) if(slot.active.load())
        for(auto& camera:slot.cameras)
            camera->observe(context,count,targets,sequence,sdk_frame,render_frame,session_,pass);
}
void CaptureStream::finish_slot(Slot& slot) {
    bool pending=false,failed=false;
    for(size_t i=0;i<slot.cameras.size();++i) {
        if(!(slot.mask.load()&(1u<<camera_indices_[i]))) continue;
        auto& camera=slot.cameras[i];
        const auto phase=camera->phase();
        pending|=phase==GpuCapture::Phase::armed || phase==GpuCapture::Phase::waiting_gpu;
        failed|=phase==GpuCapture::Phase::error;
    }
    if(pending) return;
    slot.active=false;
    if(failed) {
        ++failed_;
        std::string errors;
        for(auto& camera:slot.cameras) if(camera->phase()==GpuCapture::Phase::error) {
            const auto state=camera->command("status",0,false);
            if(!errors.empty()) errors+="; ";
            errors+=state.at("error").get<std::string>();
        }
        std::lock_guard lock(result_mutex_);error_=errors;
        return;
    }
    ++completed_;
    json manifest={{"stream_id",id_},{"render_frame_id",slot.frame.load()},
        {"observation_session_qpc",session_},{"complete",true},{"requested_cameras",json::array()},
        {"missing_views",json::array()},{"views",json::array()}};
    std::vector<BundleBlob> blobs;
    for(size_t i=0;i<slot.cameras.size();++i) if(slot.mask.load()&(1u<<camera_indices_[i])) {
        manifest["requested_cameras"].push_back(names_[i]);slot.cameras[i]->append_bundle(manifest["views"],blobs);
    }
    // Completed captures are immutable until this worker rearms the slot.
    const auto result=publisher_.publish_bundle(std::move(manifest),blobs);
    if(result.at("published").get<bool>()) ++published_;else ++queue_dropped_;
}
void CaptureStream::run() noexcept {
    std::string reason="duration",error;
    try {
        const auto begin=qpc_now();const double frequency=static_cast<double>(qpc_frequency());
        double next=0;uint64_t previous_frame=0;size_t cursor=0;
        for(;;) {
            if(WaitForSingleObject(stop_event_.h,0)==WAIT_OBJECT_0) {reason="stopped";break;}
            for(auto& slot:slots_) if(slot.active.load()) finish_slot(slot);
            const double elapsed=static_cast<double>(qpc_now()-begin)/frequency;
            if(elapsed>=duration_) {
                const bool pending=std::any_of(slots_.begin(),slots_.end(),[](const auto& s){return s.active.load();});
                // Finish already requested frames, but do not wait indefinitely
                // when a menu or pause stops rendering.
                if(!pending || elapsed>=duration_+1.0) break;
            } else if(elapsed>=next) {
                if(auto changed=pending_options_.exchange({})) {std::lock_guard lock(result_mutex_);options_=*changed;}
                next+=1.0/hz_;
                if(next<=elapsed) next=elapsed+1.0/hz_; // No catch-up burst after a stall.
                const auto frame=presents_.load()+2;
                if(frame==previous_frame) ++same_frame_;
                else {
                    Slot* available=nullptr;
                    for(size_t n=0;n<slots_.size();++n) {
                        const auto index=(cursor+n)%slots_.size();
                        if(!slots_[index].active.load()) {available=&slots_[index];cursor=(index+1)%slots_.size();break;}
                    }
                    if(!available) ++ring_busy_;
                    else {
                        uint32_t mask=0;
                        for(size_t i=0;i<available->cameras.size();++i) {
                            auto options=options_;options.products=options.outputs[camera_indices_[i]];
                            if(options.selective && !options.products) continue;
                            available->cameras[i]->command("arm",frame,false,options);mask|=1u<<camera_indices_[i];
                        }
                        available->mask=mask;
                        if(mask) {available->frame=frame;available->active=true;previous_frame=frame;++armed_;}
                    }
                }
            }
            if(WaitForSingleObject(stop_event_.h,2)==WAIT_OBJECT_0) {reason="stopped";break;}
        }
    } catch(const std::exception& e) {reason="error";error=e.what();}
    catch(...) {reason="error";error="Capture stream failed";}
    for(auto& slot:slots_) {
        if(slot.active.exchange(false)) ++canceled_;
        for(auto& camera:slot.cameras) camera->cancel();
    }
    {std::lock_guard lock(result_mutex_);reason_=reason;if(!error.empty()) error_=error;}
    ended_=qpc_now();running_=false;
}
json CaptureStream::status() {
    json slots=json::array();
    for(auto& slot:slots_) {
        json cameras=json::array();
        for(auto& camera:slot.cameras) cameras.push_back(camera->command("status",0,false));
        slots.push_back({{"active",slot.active.load()},{"render_frame_id",slot.frame.load()},{"cameras",cameras}});
    }
    std::lock_guard lock(result_mutex_);
    return {{"running",running_.load()},{"stream_id",id_},{"requested_hz",hz_},{"duration_s",duration_},
        {"format",options_.format},{"color_gain",options_.color_gain},{"requested_cameras",names_},
        {"armed",armed_.load()},{"completed",completed_.load()},{"published",published_.load()},
        {"failed",failed_.load()},{"canceled",canceled_.load()},{"gpu_ring_busy",ring_busy_.load()},
        {"same_frame_skipped",same_frame_.load()},{"queue_dropped",queue_dropped_.load()},
        {"ended_qpc",ended_.load()},{"reason",reason_},{"last_error",error_},{"slots",slots}};
}
}
