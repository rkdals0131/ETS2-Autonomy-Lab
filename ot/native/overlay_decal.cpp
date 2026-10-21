#include "overlay_decal.hpp"
#include "overlay_vs.hpp"
#include "overlay_ps.hpp"
#include <fstream>
#include <stdexcept>

namespace ot {
namespace {
void check(HRESULT hr,const char* operation) {
    if(FAILED(hr)) throw std::runtime_error(std::string(operation)+" HRESULT="+std::to_string(hr));
}
struct Constants {
    std::array<float,16> inverse_projection,eye_to_world;
    std::array<float,4> camera,depth,mask_u,mask_v,road,unused,target,bounds;
};
static_assert(sizeof(Constants)==256);
// The path itself for the pixel shader's exact distance (b1): the mask's 25 cm texels made a
// 0.6 m ribbon visibly stepped. Points relative to the mask origin, with their arc length.
struct PathConstants {
    std::array<float,4> info; // point count, stroke count, fade start, fade end (m of arc)
    std::array<std::array<float,4>,overlay_max_strokes> shape,color; // first, count, half width, minimum half px; rgba
    std::array<std::array<float,4>,overlay_max_points> points;       // x, z, arc from the stroke start
};
static_assert(sizeof(PathConstants)%16==0);
// overlay_decal.hlsl: the most a foreshortened mark widens on the road (its minimum pixel thickness).
constexpr double max_half_m=4;
// Widest half width a stroke can be drawn with (the shader widens strokes with a pixel minimum).
double widest_half(const OverlayContent& c) {
    double result=0;
    for(uint32_t i=0;i<c.stroke_count;++i) result=std::max(result,c.strokes[i].min_px>0?std::max<double>(c.strokes[i].width_m/2,max_half_m):c.strokes[i].width_m/2);
    return result;
}
// Depth formats the main DSV may use: typeless storage and the depth-plane view.
bool depth_formats(DXGI_FORMAT format,DXGI_FORMAT& typeless,DXGI_FORMAT& view) {
    if(format==DXGI_FORMAT_D32_FLOAT_S8X24_UINT || format==DXGI_FORMAT_R32G8X24_TYPELESS) {typeless=DXGI_FORMAT_R32G8X24_TYPELESS;view=DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;}
    else if(format==DXGI_FORMAT_D24_UNORM_S8_UINT || format==DXGI_FORMAT_R24G8_TYPELESS) {typeless=DXGI_FORMAT_R24G8_TYPELESS;view=DXGI_FORMAT_R24_UNORM_X8_TYPELESS;}
    else if(format==DXGI_FORMAT_D32_FLOAT || format==DXGI_FORMAT_R32_TYPELESS) {typeless=DXGI_FORMAT_R32_TYPELESS;view=DXGI_FORMAT_R32_FLOAT;}
    else return false;
    return true;
}
}

void OverlayDecal::start(OverlayBlock* block,const OverlayOptions& options) {
    if(!block) throw std::runtime_error("Overlay shared memory is not mapped");
    if(!(options.fade_end>options.fade_start) || !(options.z_tolerance>=0) || !(options.slope>=0) ||
       !(options.normal_min>=0 && options.normal_min<=1))
        throw std::runtime_error("Invalid overlay options");
    options_.store(std::make_shared<const OverlayOptions>(options));
    if(worker_.joinable() && block==block_) return;
    stop();
    block_=block;
    stop_event_.h=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if(!stop_event_) throw std::runtime_error("Cannot create overlay worker event");
    worker_=std::thread([this]{run();});
}
void OverlayDecal::stop() noexcept {
    if(stop_event_) SetEvent(stop_event_.h);
    if(worker_.joinable()) worker_.join();
    if(stop_event_) {CloseHandle(stop_event_.h);stop_event_.h=nullptr;}
}
void OverlayDecal::run() noexcept {
    // Poll the generation at 200 Hz; rasterize (<1 ms) only when it or the
    // baked fade changes, off the render thread (docs/24 §9).
    auto content=std::make_unique<OverlayContent>();
    LONG64 seen=-1;std::shared_ptr<const OverlayOptions> rastered;
    std::shared_ptr<const Frame> current;
    while(WaitForSingleObject(stop_event_.h,5)==WAIT_TIMEOUT) {
        try {
            LONG64 generation{};
            const auto options=options_.load();
            const auto result=overlay_read(block_,seen,*content,generation);
            if(result==-1) {++torn_;continue;}
            if(result==-2) {++rejected_;seen=generation;content->valid=false;content->point_count=content->stroke_count=0;}
            if(result==0 && options==rastered) continue;
            if(result==0 && current) *content=current->content;
            const auto begin=qpc_now();
            auto frame=std::make_shared<Frame>();
            frame->serial=++serial_;frame->generation=result==0?seen:generation;frame->content=*content;
            // The mask (origin, axis, extent) over the whole point pool; the shader draws each stroke.
            frame->mask=overlay::rasterize(content->points,content->point_count,std::max(2*widest_half(*content),.1),options->fade_start,options->fade_end);
            frame->raster_qpc=qpc_now();
            raster_ticks_+=frame->raster_qpc-begin;++rasters_;
            if(result!=0) seen=generation;
            rastered=options;current=frame;frame_.store(std::move(frame));
        } catch(const std::exception& e) {
            AcquireSRWLockExclusive(&state_lock_);error_=std::string("mask worker: ")+e.what();ReleaseSRWLockExclusive(&state_lock_);
        } catch(...) {}
    }
}
void OverlayDecal::release_gpu() noexcept {
    list_.Reset();deferred_.Reset();vs_.Reset();ps_.Reset();constants_.Reset();path_constants_.Reset();mask_.Reset();mask_view_.Reset();
    depth_view_.Reset();sampler_.Reset();blend_.Reset();raster_.Reset();depth_state_.Reset();depth_source_.Reset();
    depth_copy_.Reset();recorded_target_.Reset();recorded_depth_=nullptr;recorded_width_=recorded_height_=0;
    uploaded_serial_=0;failed_=false;device_.Reset();
    AcquireSRWLockExclusive(&capture_lock_);
    for(auto& c:captured_) c=Captured{};
    capture_requested_=0;
    ReleaseSRWLockExclusive(&capture_lock_);
}
void OverlayDecal::prepare(ID3D11Device* device) {
    if(device_.Get()==device && list_ && deferred_) return;
    if(device_.Get()!=device) {
        release_gpu();device_=device;
    }
    if(deferred_) return;
    // A device created single-threaded refuses deferred contexts; then nothing is drawn.
    check(device->CreateDeferredContext(0,&deferred_),"CreateDeferredContext");
    check(device->CreateVertexShader(ot_overlay_vs,sizeof(ot_overlay_vs),nullptr,&vs_),"CreateVertexShader(overlay)");
    check(device->CreatePixelShader(ot_overlay_ps,sizeof(ot_overlay_ps),nullptr,&ps_),"CreatePixelShader(overlay)");
    D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=sizeof(Constants);buffer.Usage=D3D11_USAGE_DEFAULT;buffer.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    check(device->CreateBuffer(&buffer,nullptr,&constants_),"CreateBuffer(overlay constants)");
    buffer.ByteWidth=sizeof(PathConstants);
    check(device->CreateBuffer(&buffer,nullptr,&path_constants_),"CreateBuffer(overlay path)");
    D3D11_TEXTURE2D_DESC mask{};mask.Width=overlay::mask_width;mask.Height=overlay::mask_height;mask.MipLevels=1;mask.ArraySize=1;
    mask.Format=DXGI_FORMAT_R8G8_UNORM;mask.SampleDesc.Count=1;mask.Usage=D3D11_USAGE_DEFAULT;mask.BindFlags=D3D11_BIND_SHADER_RESOURCE;
    check(device->CreateTexture2D(&mask,nullptr,&mask_),"CreateTexture2D(overlay mask)");
    check(device->CreateShaderResourceView(mask_.Get(),nullptr,&mask_view_),"CreateSRV(overlay mask)");
    D3D11_SAMPLER_DESC sampler{};sampler.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D11_TEXTURE_ADDRESS_BORDER; // outside the mask: no path
    sampler.ComparisonFunc=D3D11_COMPARISON_NEVER;sampler.MaxLOD=D3D11_FLOAT32_MAX;
    check(device->CreateSamplerState(&sampler,&sampler_),"CreateSamplerState(overlay)");
    D3D11_BLEND_DESC blend{};auto& b=blend.RenderTarget[0];
    // Dual-source paint (overlay_decal.hlsl): target = output 0 + target * output 1.
    b.BlendEnable=TRUE;b.SrcBlend=D3D11_BLEND_ONE;b.DestBlend=D3D11_BLEND_SRC1_COLOR;b.BlendOp=D3D11_BLEND_OP_ADD;
    b.SrcBlendAlpha=D3D11_BLEND_ZERO;b.DestBlendAlpha=D3D11_BLEND_ONE;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;
    b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_RED|D3D11_COLOR_WRITE_ENABLE_GREEN|D3D11_COLOR_WRITE_ENABLE_BLUE;
    check(device->CreateBlendState(&blend,&blend_),"CreateBlendState(overlay)");
    D3D11_RASTERIZER_DESC raster{};raster.FillMode=D3D11_FILL_SOLID;raster.CullMode=D3D11_CULL_NONE;raster.DepthClipEnable=TRUE;
    check(device->CreateRasterizerState(&raster,&raster_),"CreateRasterizerState(overlay)");
    D3D11_DEPTH_STENCIL_DESC depth{};depth.DepthEnable=FALSE;depth.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;depth.StencilEnable=FALSE;
    depth.DepthFunc=D3D11_COMPARISON_ALWAYS;
    check(device->CreateDepthStencilState(&depth,&depth_state_),"CreateDepthStencilState(overlay)");
    gpu_allocations_+=11;
}
void OverlayDecal::prepare_depth(ID3D11DeviceContext* context,ID3D11DepthStencilView* view,D3D11_TEXTURE2D_DESC& desc) {
    Com<ID3D11Resource> resource;view->GetResource(&resource);
    Com<ID3D11Texture2D> texture;check(resource.As(&texture),"Depth resource Texture2D");
    texture->GetDesc(&desc);
    if(resource.Get()!=depth_source_.Get()) {
        DXGI_FORMAT typeless{},format{};
        if(!depth_formats(desc.Format,typeless,format)) throw std::runtime_error("Unsupported main depth format");
        if(desc.SampleDesc.Count!=1 || desc.ArraySize!=1) throw std::runtime_error("Unsupported multisampled or array main depth");
        depth_view_.Reset();depth_copy_.Reset();
        D3D11_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=format;srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
        if(desc.BindFlags&D3D11_BIND_SHADER_RESOURCE) {
            check(device_->CreateShaderResourceView(texture.Get(),&srv,&depth_view_),"CreateSRV(main depth)");
        } else {
            // No SHADER_RESOURCE: a private copy each frame, as GpuPack::depth.
            auto copy=desc;copy.Format=typeless;copy.Usage=D3D11_USAGE_DEFAULT;copy.BindFlags=D3D11_BIND_SHADER_RESOURCE;
            copy.CPUAccessFlags=copy.MiscFlags=0;copy.MipLevels=1;
            check(device_->CreateTexture2D(&copy,nullptr,&depth_copy_),"CreateTexture2D(main depth copy)");
            check(device_->CreateShaderResourceView(depth_copy_.Get(),&srv,&depth_view_),"CreateSRV(main depth copy)");
        }
        gpu_allocations_+=depth_copy_?2:1;
        depth_source_=resource;
    }
    if(depth_copy_) {context->CopyResource(depth_copy_.Get(),depth_source_.Get());++depth_copies_;}
}
void OverlayDecal::record(ID3D11RenderTargetView* target,UINT width,UINT height) {
    list_.Reset();
    auto* c=deferred_.Get();
    c->ClearState();
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);c->IASetInputLayout(nullptr);
    c->VSSetShader(vs_.Get(),nullptr,0);c->VSSetConstantBuffers(0,1,constants_.GetAddressOf());
    ID3D11Buffer* buffers[]{constants_.Get(),path_constants_.Get()};
    c->PSSetShader(ps_.Get(),nullptr,0);c->PSSetConstantBuffers(0,2,buffers);
    ID3D11ShaderResourceView* views[]{depth_view_.Get(),mask_view_.Get()};
    c->PSSetShaderResources(0,2,views);c->PSSetSamplers(0,1,sampler_.GetAddressOf());
    const D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    c->RSSetState(raster_.Get());c->RSSetViewports(1,&viewport);
    c->OMSetRenderTargets(1,&target,nullptr); // never the DSV: its resource is read as t0
    c->OMSetBlendState(blend_.Get(),nullptr,0xffffffff);c->OMSetDepthStencilState(depth_state_.Get(),0);
    c->Draw(4,0);
    check(c->FinishCommandList(FALSE,&list_),"FinishCommandList(overlay)");
    recorded_target_=target;recorded_depth_=depth_view_.Get();recorded_width_=width;recorded_height_=height;
    ++records_;
}
void OverlayDecal::draw(ID3D11DeviceContext* context,ID3D11RenderTargetView* target,ID3D11DepthStencilView* depth,
                        const RenderCameraSample& camera,uint64_t sdk_time_us) noexcept {
    try {
        const auto frame=frame_.load();
        if(!frame) {++skips_[no_path];return;}
        const auto& path=frame->content;
        if(!path.valid || !path.stroke_count || path.point_count<2) {++skips_[invalid];return;}
        if(path.ttl_ms && sdk_time_us>path.stamp_us && sdk_time_us-path.stamp_us>path.ttl_ms*1000) {++skips_[expired];return;}
        if(!camera.available) {++skips_[Skip::camera];return;}
        if(!target) {++skips_[Skip::target];return;}
        if(!depth) {++skips_[Skip::depth];return;}
        if(failed_) {++skips_[failed];return;}
        const auto view=overlay::Camera::make(camera.projection,camera.rotation,camera.world,camera.viewport_depth);
        // A projection modifier (not decoded) is drawn anyway: at most a sub-pixel shift is expected.
        if(!view) {++skips_[Skip::camera];return;}
        const auto options=options_.load();
        Com<ID3D11Device> device;context->GetDevice(&device);
        prepare(device.Get());
        Com<ID3D11Resource> resource;target->GetResource(&resource);
        Com<ID3D11Texture2D> texture;
        if(FAILED(resource.As(&texture))) {++skips_[unsupported];return;}
        D3D11_TEXTURE2D_DESC desc{},depth_desc{};texture->GetDesc(&desc);
        if(desc.SampleDesc.Count!=1) {++skips_[unsupported];return;}
        prepare_depth(context,depth,depth_desc);
        const double width=desc.Width,height=desc.Height;
        const auto bounds=overlay::path_bounds(*view,path.points,path.point_count,path.ground_y,
            widest_half(path)+overlay::edge_m,options->z_tolerance,options->slope);
        if(!bounds) {++skips_[offscreen];return;}
        if(uploaded_serial_!=frame->serial) {
            context->UpdateSubresource(mask_.Get(),0,nullptr,frame->mask.texels.data(),overlay::mask_width*2,0);
            auto p=std::make_unique<PathConstants>();
            const auto used=std::min<uint32_t>(path.point_count,overlay_max_points);
            p->info={float(used),float(path.stroke_count),float(options->fade_start),float(options->fade_end)};
            for(uint32_t k=0;k<path.stroke_count;++k) {
                const auto& stroke=path.strokes[k];
                p->shape[k]={float(stroke.first),float(stroke.count),stroke.width_m/2,stroke.min_px};
                p->color[k]={stroke.rgba[0],stroke.rgba[1],stroke.rgba[2],stroke.rgba[3]};
                double s=0;
                for(uint32_t i=stroke.first;i<stroke.first+stroke.count && i<used;++i) {
                    if(i>stroke.first) s+=std::hypot(path.points[i][0]-path.points[i-1][0],path.points[i][1]-path.points[i-1][1]);
                    p->points[i]={float(path.points[i][0]-frame->mask.origin[0]),float(path.points[i][1]-frame->mask.origin[1]),float(s),0};
                }
            }
            context->UpdateSubresource(path_constants_.Get(),0,nullptr,p.get(),0,0);
            uploaded_serial_=frame->serial;
        }
        Constants k{};
        for(int i=0;i<16;++i) k.inverse_projection[i]=float(view->eye_from_clip[i]);
        for(int i=0;i<3;++i) for(int j=0;j<3;++j) k.eye_to_world[4*i+j]=float(view->world_from_eye[3*i+j]);
        k.eye_to_world[15]=1;
        const bool ground=std::isfinite(path.ground_y);
        // Double before float: the mask origin and camera are absolute world positions.
        k.camera={float(camera.world[0]-frame->mask.origin[0]),float(ground?camera.world[1]-path.ground_y:0),
                  float(camera.world[2]-frame->mask.origin[1]),ground?1.f:0.f};
        k.depth={camera.viewport_depth[0],camera.viewport_depth[1],float(depth_desc.Width/width),float(depth_desc.Height/height)};
        const auto a=frame->mask.affine();
        k.mask_u={float(a[0]),float(a[1]),float(a[2]),0};k.mask_v={float(a[3]),float(a[4]),float(a[5]),0};
        k.road={float(options->z_tolerance),float(options->slope),float(options->normal_min),0};
        k.target={float(width),float(height),float(1/width),float(1/height)};
        k.bounds={float((*bounds)[0]),float((*bounds)[1]),float((*bounds)[2]),float((*bounds)[3])};
        context->UpdateSubresource(constants_.Get(),0,nullptr,&k,0,0);
        if(!list_ || recorded_target_.Get()!=target || recorded_depth_!=depth_view_.Get() ||
           recorded_width_!=desc.Width || recorded_height_!=desc.Height) record(target,desc.Width,desc.Height);
        context->ExecuteCommandList(list_.Get(),TRUE);
        ++drawn_;
        if(capture_busy_.load(std::memory_order_relaxed)) capture_frame(context,texture.Get(),desc,*frame,*view,sdk_time_us);
        if(drawn_.load(std::memory_order_relaxed)%64==1 && TryAcquireSRWLockExclusive(&state_lock_)) {
            last_draw_={{"target",{desc.Width,desc.Height,static_cast<unsigned>(desc.Format)}},
                {"depth",{depth_desc.Width,depth_desc.Height,static_cast<unsigned>(depth_desc.Format)}},
                {"depth_path",depth_copy_?"copy":"srv"},{"bounds_ndc",*bounds},{"viewport_depth",camera.viewport_depth},
                {"camera_world",camera.world},{"projection_modifier_flag",camera.projection_modifier_flag},
                {"generation",frame->generation},{"ground_known",ground}};
            ReleaseSRWLockExclusive(&state_lock_);
        }
    } catch(const std::exception& e) {
        failed_=true;++skips_[failed];
        AcquireSRWLockExclusive(&state_lock_);error_=e.what();ReleaseSRWLockExclusive(&state_lock_);
    } catch(...) {failed_=true;++skips_[failed];}
}
void OverlayDecal::capture_frame(ID3D11DeviceContext* context,ID3D11Texture2D* target,const D3D11_TEXTURE2D_DESC& desc,
                                 const Frame& frame,const overlay::Camera& camera,uint64_t sdk_time_us) {
    if(!TryAcquireSRWLockExclusive(&capture_lock_)) return;
    struct Unlock {SRWLOCK* l;~Unlock(){ReleaseSRWLockExclusive(l);}} unlock{&capture_lock_};
    // Map frames copied on earlier blits without waiting for the GPU.
    for(auto& c:captured_) if(c.copied && !c.mapped) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const auto hr=context->Map(c.staging.Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(hr==DXGI_ERROR_WAS_STILL_DRAWING) continue;
        check(hr,"Map(overlay capture)");
        const auto row=c.metadata.at("row_bytes").get<size_t>();
        c.pixels.resize(row*c.desc.Height);
        for(UINT y=0;y<c.desc.Height;++y) std::memcpy(c.pixels.data()+y*row,static_cast<const uint8_t*>(mapped.pData)+y*mapped.RowPitch,row);
        context->Unmap(c.staging.Get(),0);c.mapped=true;
    }
    if(!capture_requested_) return;
    auto& c=captured_[captured_.size()-capture_requested_];
    auto staging=desc;staging.Usage=D3D11_USAGE_STAGING;staging.BindFlags=0;staging.CPUAccessFlags=D3D11_CPU_ACCESS_READ;staging.MiscFlags=0;
    D3D11_TEXTURE2D_DESC previous{};if(c.staging) c.staging->GetDesc(&previous);
    if(!c.staging || previous.Width!=desc.Width || previous.Height!=desc.Height || previous.Format!=desc.Format) {
        c.staging.Reset();check(device_->CreateTexture2D(&staging,nullptr,&c.staging),"CreateTexture2D(overlay capture)");
    }
    context->CopyResource(c.staging.Get(),target);
    c.desc=desc;c.copied=true;c.mapped=false;
    const auto bytes=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8u:4u;
    // Screen positions of the path start, the fade start and the drawn end, with this frame's camera.
    json marks=json::array();
    const auto& path=frame.content;const auto& mask=frame.mask;
    const auto opts=options_.load();
    double s=0;uint32_t fade=0;
    for(uint32_t i=1;i<mask.points_used;++i) {
        s+=std::hypot(path.points[i][0]-path.points[i-1][0],path.points[i][1]-path.points[i-1][1]);
        if(!fade && s>=opts->fade_start) fade=i;
    }
    const double y=std::isfinite(path.ground_y)?path.ground_y:camera.origin[1];
    for(const auto& [name,index]:{std::pair{"start",0u},{"fade_start",fade},{"end",mask.points_used-1}}) {
        if(index>=mask.points_used) continue;
        const auto pixel=camera.pixel({path.points[index][0],y,path.points[index][1]},desc.Width,desc.Height);
        marks.push_back({{"name",name},{"point_index",index},{"world_xyz",{path.points[index][0],y,path.points[index][1]}},
            {"pixel",pixel?json(std::array<double,2>{(*pixel)[0],(*pixel)[1]}):json(nullptr)},
            {"viewport_depth",pixel?json((*pixel)[2]):json(nullptr)}});
    }
    c.metadata={{"width",desc.Width},{"height",desc.Height},{"dxgi_format",static_cast<unsigned>(desc.Format)},
        {"row_bytes",desc.Width*bytes},{"copy_qpc",qpc_now()},{"sdk_time_us",sdk_time_us},{"generation",frame.generation},
        {"camera_world",camera.origin},{"marks",marks},{"scope","main0/composition_out after the decal, before blit_to_target"}};
    --capture_requested_;
}
json OverlayDecal::capture(const json& request) {
    if(capture_busy_.exchange(true)) throw std::runtime_error("An overlay capture is already running");
    struct Done {std::atomic<bool>& b;~Done(){b=false;}} done{capture_busy_};
    AcquireSRWLockExclusive(&capture_lock_);
    for(auto& c:captured_) {c.copied=c.mapped=false;c.pixels.clear();c.metadata=nullptr;}
    capture_requested_=static_cast<unsigned>(captured_.size());
    ReleaseSRWLockExclusive(&capture_lock_);
    const auto deadline=GetTickCount64()+3000;
    for(;;) {
        AcquireSRWLockShared(&capture_lock_);
        const bool ready=std::all_of(captured_.begin(),captured_.end(),[](const Captured& c){return c.mapped;});
        ReleaseSRWLockShared(&capture_lock_);
        if(ready) break;
        if(GetTickCount64()>deadline) {
            AcquireSRWLockExclusive(&capture_lock_);capture_requested_=0;ReleaseSRWLockExclusive(&capture_lock_);
            throw std::runtime_error("No two drawn overlay frames within 3 s; see overlay status skips");
        }
        Sleep(5);
    }
    const auto directory=request.contains("directory")?fs::path(request.at("directory").get<std::string>()):
        log_directory()/(L"overlay-capture-"+std::to_wstring(GetTickCount64()));
    if(!directory.is_absolute()) throw std::runtime_error("Capture directory must be absolute");
    fs::create_directories(directory);
    json frames=json::array();
    AcquireSRWLockShared(&capture_lock_);
    try {
        for(size_t i=0;i<captured_.size();++i) {
            const auto file=directory/(L"frame"+std::to_wstring(i)+L".bin");
            std::ofstream output(file,std::ios::binary);
            output.write(reinterpret_cast<const char*>(captured_[i].pixels.data()),static_cast<std::streamsize>(captured_[i].pixels.size()));
            if(!output) throw std::runtime_error("Cannot write overlay capture");
            auto metadata=captured_[i].metadata;metadata["file"]=file.generic_string();frames.push_back(std::move(metadata));
        }
    } catch(...) {ReleaseSRWLockShared(&capture_lock_);throw;}
    ReleaseSRWLockShared(&capture_lock_);
    json result={{"directory",directory.generic_string()},{"frames",frames},{"qpc_frequency",qpc_frequency()}};
    std::ofstream(directory/L"capture.json")<<result.dump(2);
    return result;
}
json OverlayDecal::status() {
    const char* names[]={"no_path","invalid","expired","camera","target","depth","offscreen","unsupported","failed"};
    json skips=json::object();
    for(size_t i=0;i<skips_.size();++i) skips[names[i]]=skips_[i].load();
    json result={{"worker",worker_.joinable()},{"options",options_.load()->describe()},
        {"rasters",rasters_.load()},{"raster_ticks",raster_ticks_.load()},{"torn_reads",torn_.load()},{"rejected_blocks",rejected_.load()},
        {"drawn",drawn_.load()},{"skips",skips},{"command_list_records",records_.load()},{"depth_copies",depth_copies_.load()},
        {"gpu_allocations",gpu_allocations_.load()}};
    if(const auto frame=frame_.load()) {
        const auto& p=frame->content;
        result["path"]={{"generation",frame->generation},{"valid",p.valid},{"point_count",p.point_count},{"points_used",frame->mask.points_used},
            {"length_m",frame->mask.length},{"clipped",frame->mask.clipped},{"stamp_us",p.stamp_us},{"ttl_ms",p.ttl_ms},
            {"strokes",[&]{json strokes=json::array();for(uint32_t i=0;i<p.stroke_count;++i) strokes.push_back({{"first",p.strokes[i].first},{"count",p.strokes[i].count},
                {"width_m",p.strokes[i].width_m},{"min_px",p.strokes[i].min_px},{"rgba",p.strokes[i].rgba}});return strokes;}()},{"ground_y",std::isfinite(p.ground_y)?json(p.ground_y):json(nullptr)},
            {"mask_origin_xz",frame->mask.origin},{"mask_axis_xz",frame->mask.axis}};
    }
    AcquireSRWLockShared(&state_lock_);
    result["error"]=error_;result["last_draw"]=last_draw_;
    ReleaseSRWLockShared(&state_lock_);
    return result;
}
}
