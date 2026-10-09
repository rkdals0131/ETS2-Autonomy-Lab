#include "capture_stream.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace ot {
CaptureStream::CaptureStream(uint32_t mask,const CaptureOptions& options,double hz,double duration,
        const std::atomic<uint64_t>& presents,uint64_t session,Transport& publisher):
    options_(options),hz_(hz),duration_(duration),presents_(presents),session_(session),publisher_(publisher),
    stop_event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    if(!stop_event_) throw std::runtime_error("Cannot create capture stream stop event");
    for(unsigned camera=0;camera<9;++camera) if(mask&(1u<<camera)) {
        exposure_[camera]=std::make_shared<ExposureState>();
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
bool CaptureStream::compiling() const noexcept {
    return compiling_mask()!=0;
}
uint32_t CaptureStream::compiling_mask() const noexcept {
    uint32_t mask=0;
    for(const auto& slot:slots_) if(slot.active.load() && slot.selected.load())
        for(size_t i=0;i<slot.cameras.size();++i)
            if((slot.mask.load()&(1u<<camera_indices_[i])) && slot.cameras[i]->phase()==GpuCapture::Phase::armed)
                mask|=1u<<camera_indices_[i];
    return mask;
}
uint32_t CaptureStream::select_pending() noexcept {
    if(selecting_.test_and_set()) return 0;
    struct Unlock {std::atomic_flag& flag;~Unlock(){flag.clear();}} unlock{selecting_};
    // Only one selected bundle may still be waiting for its render commands.
    // GPU readback of older bundles can overlap the next submission.
    if(compiling()) return 0;
    for(auto& slot:slots_) if(slot.active.load() && !slot.selected.exchange(true)) {
        ++selected_;
        return slot.mask.load();
    }
    return 0;
}
bool CaptureStream::pending() const noexcept {
    for(const auto& slot:slots_) if(slot.active.load()) return true;
    return false;
}
void CaptureStream::observe(ID3D11DeviceContext* context,uint32_t count,const uintptr_t* targets,
        uint64_t sequence,uint64_t sdk_frame,uint64_t render_frame,const json* pass) noexcept {
    for(auto& slot:slots_) if(slot.active.load() && slot.selected.load())
        for(size_t i=0;i<slot.cameras.size();++i)
            if(slot.mask.load()&(1u<<camera_indices_[i]))
                slot.cameras[i]->observe(context,count,targets,sequence,sdk_frame,render_frame,session_,pass);
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
    uint64_t frame=0;bool mixed_frames=false;
    if(!failed) for(size_t i=0;i<slot.cameras.size();++i) if(slot.mask.load()&(1u<<camera_indices_[i])) {
        const auto captured=slot.cameras[i]->captured_frame();
        if(frame && frame!=captured) mixed_frames=true;
        frame=captured;
    }
    failed|=mixed_frames;
    slot.frame=frame;slot.active=false;
    if(failed) {
        ++failed_;
        for(auto& camera:slot.cameras) camera->abandon_shared();
        std::string errors=mixed_frames?"Sensor views crossed a Present boundary":"";
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
    if(result.at("published").get<bool>()) ++published_;
    else {++queue_dropped_;for(auto& camera:slot.cameras) camera->abandon_shared();}
}
void CaptureStream::run() noexcept {
    std::string reason="duration",error;
    try {
        const auto begin=qpc_now();const double frequency=static_cast<double>(qpc_frequency());
        double next=0,next_lidar=0;uint64_t previous_frame=0;size_t cursor=0;
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
                const auto frame=presents_.load();
                if(frame==previous_frame) ++same_frame_;
                else {
                    Slot* available=nullptr;
                    for(size_t n=0;n<slots_.size();++n) {
                        const auto index=(cursor+n)%slots_.size();
                        if(!slots_[index].active.load() && std::all_of(slots_[index].cameras.begin(),slots_[index].cameras.end(),
                            [](const auto& camera){return camera->reusable();})) {
                            available=&slots_[index];cursor=(index+1)%slots_.size();break;
                        }
                    }
                    if(!available) ++ring_busy_;
                    else {
                        uint32_t mask=0;bool armed_lidar=false;
                        const bool lidar_due=options_.lidar_hz==0 || elapsed>=next_lidar;
                        for(size_t i=0;i<available->cameras.size();++i) {
                            auto options=options_;options.products=options.outputs[camera_indices_[i]];
                            if(!lidar_due) options.products&=~uint8_t{8};
                            options.exposure=options.auto_exposure?exposure_[camera_indices_[i]]:nullptr;
                            options.lidar_pattern=options.lidar()?options.lidar_patterns[camera_indices_[i]]:nullptr;
                            if(options.selective && !options.products) continue;
                            // The next selection claims this request exactly
                            // once. Actual execution determines its Present ID.
                            available->cameras[i]->command("arm",0,false,options);mask|=1u<<camera_indices_[i];
                            armed_lidar|=options.lidar();
                        }
                        if(armed_lidar && options_.lidar_hz>0)
                            next_lidar=(std::floor(elapsed*options_.lidar_hz)+1)/options_.lidar_hz;
                        available->mask=mask;
                        if(mask) {available->frame=0;available->selected=false;available->active=true;previous_frame=frame;++armed_;}
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
    for(auto& exposure:exposure_) if(exposure) *exposure=ExposureState{};
    {std::lock_guard lock(result_mutex_);reason_=reason;if(!error.empty()) error_=error;}
    ended_=qpc_now();running_=false;
}
json CaptureStream::status() {
    json slots=json::array();
    for(auto& slot:slots_) {
        json cameras=json::array();
        for(auto& camera:slot.cameras) cameras.push_back(camera->command("status",0,false));
        slots.push_back({{"active",slot.active.load()},{"selected",slot.selected.load()},{"render_frame_id",slot.frame.load()},{"cameras",cameras}});
    }
    std::lock_guard lock(result_mutex_);
    return {{"running",running_.load()},{"stream_id",id_},{"requested_hz",hz_},{"duration_s",duration_},
        {"format",options_.format},{"color_gain",options_.color_gain},{"requested_cameras",names_},
        {"lidar_hz",options_.lidar_hz>0?std::min(options_.lidar_hz,hz_):hz_},
        {"armed",armed_.load()},{"selected",selected_.load()},{"completed",completed_.load()},{"published",published_.load()},
        {"failed",failed_.load()},{"canceled",canceled_.load()},{"gpu_ring_busy",ring_busy_.load()},
        {"same_frame_skipped",same_frame_.load()},{"queue_dropped",queue_dropped_.load()},
        {"ended_qpc",ended_.load()},{"reason",reason_},{"last_error",error_},{"slots",slots}};
}
}
