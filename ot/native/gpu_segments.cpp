#include "gpu_segments.hpp"
#include <algorithm>

namespace ot {
json GpuSegments::control(const json& request) {
    const auto action=request.value("action",std::string("status"));
    if(action=="start") {clear_=true;enabled_=true;}
    else if(action=="stop") enabled_=false;
    else if(action=="clear") clear_=true;
    else if(action!="status") throw std::runtime_error("Unknown gpu_profile action");
    std::lock_guard lock(mutex_);
    std::vector<std::pair<std::string,Total>> rows(totals_.begin(),totals_.end());
    std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.second.ms>b.second.ms;});
    json segments=json::array();
    const auto per_frame=[&](double ms){return resolved_?ms/static_cast<double>(resolved_):0.0;};
    for(const auto& [name,total]:rows)
        segments.push_back({{"target",name},{"ms_per_frame",per_frame(total.ms)},{"max_frame_ms",total.max_frame_ms},
            {"segments_per_frame",resolved_?static_cast<double>(total.segments)/static_cast<double>(resolved_):0.0}});
    json worst=json::array();
    for(const auto& [name,ms]:worst_) worst.push_back({name,ms});
    return {{"enabled",enabled_.load()},{"frames",resolved_},{"worst_frame_ms",worst_ms_},{"worst_frame",std::move(worst)},{"gpu_frame_ms",per_frame(frame_ms_)},{"disjoint",disjoint_},
        {"overflow",overflow_},{"skipped",skipped_},{"segments",std::move(segments)},
        {"scope","GPU time from a game render-target bind to the next, by the opening bind's target; proxy work after a bind counts toward it"}};
}
std::string GpuSegments::label(uint32_t count,const uintptr_t* targets,uintptr_t depth,const RenderPassPtr& pass) {
    if(pass) {
        if(pass->main==MainPass::scene) return "main scene";
        if(pass->main==MainPass::composition) return "main composition";
        if(pass->main==MainPass::blit) return "main blit";
        if(pass->camera_slot<9) return "sensor mirror"+std::to_string(pass->camera_slot);
        if(pass->sensor_slot<9) return "sensor mirror"+std::to_string(pass->sensor_slot)+" (other targets)";
    }
    const auto view=count?targets[0]:depth;
    if(!view) return "no target";
    if(const auto found=sizes_.find(view);found!=sizes_.end()) return found->second;
    std::string name="?";
    Com<ID3D11Resource> resource;Com<ID3D11Texture2D> texture;
    reinterpret_cast<ID3D11View*>(view)->GetResource(&resource);
    if(resource && SUCCEEDED(resource.As(&texture))) {
        D3D11_TEXTURE2D_DESC desc{};texture->GetDesc(&desc);
        name=(count?"rt ":"depth ")+std::to_string(desc.Width)+"x"+std::to_string(desc.Height)+(count>1?" x"+std::to_string(count):"")+
            " f"+std::to_string(desc.Format)+(count && depth?" +depth":"");
    }
    sizes_.emplace(view,name);
    return name;
}
void GpuSegments::close(ID3D11DeviceContext* context,Frame& frame) noexcept {
    context->End(frame.stamps[frame.used].Get());frame.labels[frame.used].clear();++frame.used;
    context->End(frame.disjoint.Get());
    frame.open=false;frame.pending=true;
    sizes_.clear(); // views are recreated every frame (proxy CreateRenderTargetView)
}
void GpuSegments::resolve(ID3D11DeviceContext* context) noexcept {
    for(auto& frame:frames_) {
        if(!frame.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        if(context->GetData(frame.disjoint.Get(),&disjoint,sizeof(disjoint),D3D11_ASYNC_GETDATA_DONOTFLUSH)!=S_OK) continue;
        std::vector<uint64_t> times(frame.used);
        bool ready=true;
        for(size_t i=0;i<frame.used && ready;++i)
            ready=context->GetData(frame.stamps[i].Get(),&times[i],sizeof(uint64_t),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK;
        if(!ready) continue;
        frame.pending=false;
        std::lock_guard lock(mutex_);
        if(disjoint.Disjoint || !disjoint.Frequency || frame.used<2) {++disjoint_;continue;}
        const double scale=1000.0/static_cast<double>(disjoint.Frequency);
        std::map<std::string,double> per_frame;
        for(size_t i=0;i+1<frame.used;++i)
            if(times[i+1]>=times[i]) {per_frame[frame.labels[i]]+=static_cast<double>(times[i+1]-times[i])*scale;++totals_[frame.labels[i]].segments;}
        for(const auto& [name,ms]:per_frame) {auto& total=totals_[name];total.ms+=ms;total.max_frame_ms=std::max(total.max_frame_ms,ms);}
        if(times[frame.used-1]>=times[0]) {
            const auto ms=static_cast<double>(times[frame.used-1]-times[0])*scale;
            frame_ms_+=ms;
            if(ms>worst_ms_) {
                worst_ms_=ms;worst_.clear();
                for(size_t i=0;i+1<frame.used;++i)
                    worst_.push_back({frame.labels[i],times[i+1]>=times[i]?static_cast<double>(times[i+1]-times[i])*scale:0.0});
            }
        }
        ++resolved_;
    }
}
void GpuSegments::bind(ID3D11DeviceContext* context,uint64_t render_frame,uint32_t count,const uintptr_t* targets,
                       uintptr_t depth,const RenderPassPtr& pass) noexcept {
    if(!enabled_.load(std::memory_order_relaxed) || !context) return;
    try {
        if(clear_.exchange(false)) {
            std::lock_guard lock(mutex_);
            totals_.clear();resolved_=disjoint_=overflow_=skipped_=0;frame_ms_=worst_ms_=0;worst_.clear();
        }
        auto* frame=&frames_[current_];
        // A new Present id, or a full frame without one, closes the frame.
        if(frame->open && (frame->render_frame!=render_frame || frame->used+1>=stamps)) {
            if(frame->render_frame==render_frame) ++overflow_;
            close(context,*frame);
            current_=(current_+1)%frames;frame=&frames_[current_];
            resolve(context);
        }
        if(!frame->open) {
            if(frame->pending) resolve(context);
            if(frame->pending) {++skipped_;return;} // the GPU is more than three frames behind
            if(!device_) context->GetDevice(&device_);
            if(!frame->disjoint) {
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
                if(FAILED(device_->CreateQuery(&desc,&frame->disjoint))) return;
                desc.Query=D3D11_QUERY_TIMESTAMP;
                frame->stamps.resize(stamps);frame->labels.resize(stamps);
                for(auto& stamp:frame->stamps) if(FAILED(device_->CreateQuery(&desc,&stamp))) {frame->disjoint.Reset();return;}
            }
            context->Begin(frame->disjoint.Get());
            frame->open=true;frame->used=0;frame->render_frame=render_frame;
        }
        context->End(frame->stamps[frame->used].Get());
        frame->labels[frame->used]=label(count,targets,depth,pass);
        ++frame->used;
    } catch(...) {}
}
}
