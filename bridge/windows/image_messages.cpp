#include "ros_cdr.hpp"
#include <wincodec.h>
#include <wrl/client.h>
#include <limits>
#include <map>
#include <immintrin.h>
#include <intrin.h>

namespace bridge {
static void pack_rgb(uint8_t* rgb,std::span<const uint8_t> rgba) {
    static const bool ssse3=[] {int registers[4];__cpuid(registers,1);return (registers[2]&(1<<9))!=0;}();
    size_t i=0,j=0;
    if(ssse3) {
        const auto mask=_mm_setr_epi8(0,1,2,4,5,6,8,9,10,12,13,14,-1,-1,-1,-1);
        for(;i+16<=rgba.size();i+=16,j+=12) {
            const auto packed=_mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(rgba.data()+i)),mask);
            _mm_storel_epi64(reinterpret_cast<__m128i*>(rgb+j),packed);
            const auto tail=_mm_cvtsi128_si32(_mm_srli_si128(packed,8));std::memcpy(rgb+j+8,&tail,4);
        }
    }
    for(;i<rgba.size();i+=4,j+=3) std::memcpy(rgb+j,rgba.data()+i,3);
}
static Bytes jpeg(IWICImagingFactory* factory,std::span<const uint8_t> rgba,uint32_t width,uint32_t height) {
    using Microsoft::WRL::ComPtr;
    auto check=[](HRESULT hr){if(FAILED(hr)) throw std::runtime_error("WIC JPEG encoding failed");};
    ComPtr<IWICBitmap> bitmap;check(factory->CreateBitmapFromMemory(width,height,GUID_WICPixelFormat32bppRGBA,width*4,
        static_cast<UINT>(rgba.size()),const_cast<BYTE*>(rgba.data()),&bitmap));
    ComPtr<IWICFormatConverter> converter;check(factory->CreateFormatConverter(&converter));
    check(converter->Initialize(bitmap.Get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    ComPtr<IStream> stream;check(CreateStreamOnHGlobal(nullptr,TRUE,&stream));
    ComPtr<IWICBitmapEncoder> encoder;check(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;check(encoder->CreateNewFrame(&frame,&properties));
    PROPBAG2 option{};option.pstrName=const_cast<wchar_t*>(L"ImageQuality");VARIANT quality{};quality.vt=VT_R4;quality.fltVal=.8f;check(properties->Write(1,&option,&quality));
    check(frame->Initialize(properties.Get()));check(frame->SetSize(width,height));WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;
    check(frame->SetPixelFormat(&format));check(frame->WriteSource(converter.Get(),nullptr));check(frame->Commit());check(encoder->Commit());
    STATSTG stat{};check(stream->Stat(&stat,STATFLAG_NONAME));Bytes data(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};check(stream->Seek(zero,STREAM_SEEK_SET,nullptr));ULONG read{};check(stream->Read(data.data(),static_cast<ULONG>(data.size()),&read));
    if(read!=data.size()) throw std::runtime_error("Incomplete JPEG output");return data;
}
static Bytes camera_info(uint64_t us,const std::string& frame,uint32_t width,uint32_t height,const json& projection,const json& vp,double scale) {
    const auto p=projection.get<std::array<double,16>>();
    if(p[12]!=0 || p[13]!=0 || p[15]!=0 || p[14]==0 || p[1]!=0 || p[4]!=0) throw std::runtime_error("CameraInfo requires a pinhole projection without skew");
    const double w=vp.at("width"),h=vp.at("height"),x=vp.at("x"),y=vp.at("y");
    const double fx=-p[0]/p[14]*w*.5*scale,fy=-p[5]/p[14]*h*.5*scale;
    const double cx=(x+w*.5*(1+p[2]/p[14]))*scale,cy=(y+h*.5*(1-p[6]/p[14]))*scale;
    const std::array<double,9> k{fx,0,cx,0,fy,cy,0,0,1},r{1,0,0,0,1,0,0,0,1};
    const std::array<double,12> project{fx,0,cx,0,0,fy,cy,0,0,0,1,0};
    return cdr(1024,[&](Cdr& c){header(c,us,frame);c<<height<<width<<std::string("plumb_bob")<<uint32_t{5};
        for(int i=0;i<5;++i) c<<double{0};c.serialize_array(k.data(),9);c.serialize_array(r.data(),9);c.serialize_array(project.data(),12);
        c<<uint32_t{0}<<uint32_t{0}<<uint32_t{0}<<uint32_t{0}<<uint32_t{0}<<uint32_t{0}<<false;
    });
}
SensorBundle decode_bundle(Bytes input) {
    uint64_t length{};if(input.size()<8) throw std::runtime_error("Truncated bundle");std::memcpy(&length,input.data(),8);
    if(length>input.size()-8) throw std::runtime_error("Invalid bundle manifest length");
    auto manifest=json::parse(input.begin()+8,input.begin()+8+length);
    const auto size=input.size()-8-length;
    for(const auto& f:manifest.at("files")) {
        const auto offset=f.at("offset").get<size_t>(),n=f.at("length").get<size_t>();
        if(offset>size || n>size-offset) throw std::runtime_error("Invalid bundle blob bounds");
    }
    return {std::move(manifest),std::move(input),static_cast<size_t>(8+length)};
}
Packet sensor_messages(const SensorBundle& bundle,const std::string& session,const Demand& demand,uint64_t dropped,uint64_t stream_id,const json& rig,IWICImagingFactory* imaging) {
    const auto& manifest=bundle.manifest;const auto blobs=std::span(bundle.data).subspan(bundle.blob_offset);
    if(manifest.at("stream_id")!=stream_id) return {}; // Ready slots can survive an earlier stream.
    std::map<std::string,std::span<const uint8_t>> files;
    for(const auto& f:manifest.at("files")) {
        const auto offset=f.at("offset").get<size_t>(),n=f.at("length").get<size_t>();
        files.emplace(f.at("file").get<std::string>(),blobs.subspan(offset,n));
    }
    Packet packet{{{"session",session},{"frame",manifest.at("render_frame_id")},{"native_stream",stream_id}}, {}};
    size_t capacity=bundle.data.size();
    for(const auto& [file,bytes]:files) if(file.ends_with("_lidar.bin")) capacity+=bytes.size()/16*(36-16);
    packet.data.reserve(capacity);
    const auto& first=manifest.at("views").at(0).at("metadata").at("geometry_pass").at("sdk_at_compile");
    // A menu transition can finish render work after the SDK has paused and
    // stopped supplying the ego pose. State/clock continue on their own socket;
    // these sensor samples do not describe a running simulation frame.
    if(first.at("paused").get<bool>()) return {};
    const auto us=first.at("paused_simulation_time_us").get<uint64_t>();
    json cameras=json::array();std::vector<float> gains;std::vector<bool> automatic;
    struct Transform {std::string parent,frame;V position;Q rotation;};std::vector<Transform> transforms;
    const auto& reference=manifest.at("views").at(0);
    const auto& pass=reference.at("metadata").at("geometry_pass");
    const auto& camera=pass.at("camera_at_compile");
    const auto& body=pass.at("ego_at_compile").at("pose_physics");
    const auto reference_name=reference.at("camera").get<std::string>();
    const auto mount=std::find_if(rig.at("views").begin(),rig.at("views").end(),[&](const auto& v){return "mirror"+std::to_string(v.at("slot").template get<int>())==reference_name;});
    if(mount==rig.at("views").end()) throw std::runtime_error("Unconfigured render camera");
    // Derive the moving cabin parent from the actual rendered view and its fixed
    // mount. This includes render interpolation/suspension and excludes head pose.
    const M cabin_rotation=mul(transpose(matrix(camera.at("camera_rotation_row_major"))),transpose(from_quat(mount->at("quaternion_wxyz").get<Q>())));
    const V cabin_position=sub(camera.at("camera_world_xyz").get<V>(),mul(cabin_rotation,mount->at("position").get<V>()));
    const M body_rotation=from_quat(body.at("quaternion_wxyz").get<Q>());
    const V base_world=mul(enu,add(body.at("position_m").get<V>(),mul(body_rotation,rig.at("base_origin").get<V>())));
    const M world_from_base=mul(mul(enu,body_rotation),base_to_model);
    transforms.push_back({"world","base_link",base_world,quaternion(world_from_base)});
    transforms.push_back({"base_link","cabin",mul(transpose(world_from_base),sub(mul(enu,cabin_position),base_world)),
        quaternion(mul(mul(transpose(world_from_base),enu),mul(cabin_rotation,base_to_model)))});
    struct LidarSource {
        std::string camera;json description;const json* meta;std::span<const uint8_t> data;
        uint32_t width{};M eye_from_sensor{};std::array<double,16> projection{};std::array<double,4> viewport{};
    };
    std::map<std::string,std::vector<LidarSource>> lidars;
    for(const auto& view:manifest.at("views")) {
        const std::string mirror=view.at("camera");
        const auto mount=std::find_if(rig.at("views").begin(),rig.at("views").end(),[&](const json& v){return "mirror"+std::to_string(v.at("slot").get<int>())==mirror;});
        if(mount==rig.at("views").end()) throw std::runtime_error("Bundle camera is not in the configured rig");
        const auto name=mount->at("camera_id").get<std::string>(),base="/ets2/camera/"+name,frame=name+"_optical";
        const auto& meta=view.at("metadata");const auto& pass=meta.at("geometry_pass");const auto& camera=pass.at("camera_at_compile");
        if(pass.at("sdk_at_compile").at("frame_id")!=first.at("frame_id")) throw std::runtime_error("Different SDK associations within one sensor bundle");
        if(camera.at("projection_modifier_flag").get<unsigned>()!=0) throw std::runtime_error("Unsupported modified camera projection");
        cameras.push_back(name);
        float gain=std::numeric_limits<float>::quiet_NaN();
        for(const auto& image:meta.at("images")) if(image.contains("linear_gain")) {gain=image.at("linear_gain");break;}
        gains.push_back(gain);automatic.push_back(meta.contains("color_exposure") && meta.at("color_exposure").value("automatic",false));
        const auto& vp=meta.at("geometry_gpu").at("viewports").at(0);
        const auto rotation=matrix(camera.at("camera_rotation_row_major"));const auto origin=camera.at("camera_world_xyz").get<V>();
        const auto gt_topic="/ets2/ground_truth/"+name+"/objects";
        const auto marker_topic="/ets2/ground_truth/"+name+"/markers";
        if((demand.contains(gt_topic) || demand.contains(marker_topic)) && pass.contains("vehicles_at_compile") && pass.at("vehicles_at_compile").value("available",false)) {
            const auto& vehicles=pass.at("vehicles_at_compile").at("vehicles");
            struct Box {V p,size;Q q;std::string id;};std::vector<Box> boxes;
            for(const auto& v:vehicles) {
                    const auto bounds=v.at("actor_observation").at("aabb_raw").get<std::array<double,6>>();
                    V center{},size{};for(size_t i=0;i<3;++i) {center[i]=(bounds[i]+bounds[i+3])*.5;size[i]=bounds[i+3]-bounds[i];}
                    const auto model=matrix(v.at("model_rotation_row_major"));
                    const auto world=add(v.at("model_world_xyz").get<V>(),mul(model,sub(center,v.at("model_reference_offset_raw").get<V>())));
                    boxes.push_back({mul(optical,mul(rotation,sub(world,origin))),size,quaternion(mul(mul(optical,rotation),model)),session+":"+std::to_string(v.at("actor_address").get<uint64_t>())});
            }
            if(demand.contains(gt_topic)) add_message(packet,gt_topic,cdr(1024+boxes.size()*512,[&](Cdr& c){
                header(c,us,frame);c<<uint32_t(boxes.size());
                for(const auto& b:boxes) {
                    header(c,us,frame);c<<uint32_t{0}; // No invented semantic class/confidence.
                    pose(c,b.p,b.q);c.serialize_array(b.size.data(),3);c<<b.id;
                }
            }));
            if(demand.contains(marker_topic)) add_message(packet,marker_topic,cdr(1024+boxes.size()*512,[&](Cdr& c){
                c<<uint32_t(boxes.size()+1);
                auto marker=[&](int32_t id,int32_t action,const Box& b) {
                    header(c,us,frame);c<<name<<id<<int32_t{1}<<action;pose(c,b.p,b.q);c.serialize_array(b.size.data(),3);
                    c<<.2f<<1.f<<.3f<<.35f<<int32_t{0}<<uint32_t{300000000}<<false; // 300 ms lifetime
                    c<<uint32_t{0}<<uint32_t{0}<<std::string{}; // points, colors, texture_resource
                    header(c,0,"");c<<std::string{}<<uint32_t{0}; // empty CompressedImage texture
                    c<<uint32_t{0}<<std::string{}<<std::string{}<<std::string{}<<uint32_t{0}<<false; // UV, text, mesh resource/file, materials
                };
                marker(0,3,{{0,0,0},{1,1,1},{0,0,0,1},""});
                for(size_t i=0;i<boxes.size();++i) marker(static_cast<int32_t>(i),0,boxes[i]);
            }));
        }
        uint32_t width=meta.at("sensor_dimensions").at(0),height=meta.at("sensor_dimensions").at(1);
        for(const auto& desc:meta.at("images")) {
            const std::string file=desc.at("file");
            if(file==mirror+"_lidar.bin") {
                const auto bytes=files.at(file);const size_t count=desc.at("beam_count");
                if(desc.at("encoding")!="range_f32_status_u32_pixel_u32_depth_f32" || bytes.size()!=count*16)
                    throw std::runtime_error("Invalid GPU LiDAR return layout");
                lidars[desc.at("name").get<std::string>()].push_back({mirror,desc,&meta,bytes});continue;
            }
            const bool color=file==mirror+"_color_ldr.bin",depth=file==mirror+"_depth_f32.bin",preview=file==mirror+"_preview_ldr.bin";
            if(!color && !depth && !preview) continue;
            const uint32_t width=desc.at("width"),height=desc.at("height");const auto data=files.at(file);
            if(!width || !height || data.size()!=static_cast<size_t>(width)*height*4 || desc.at("row_bytes")!=width*4)
                throw std::runtime_error("Invalid packed image dimensions");
            if(depth && desc.at("encoding")!="optical_depth_m_nan_invalid") throw std::runtime_error("ROS depth requires metric GPU output");
            const bool rgb=!depth;
            const auto topic=base+(depth?"/depth/image_raw":preview?"/perception/image_raw":"/image_raw");
            if(demand.contains(topic)) {
                const size_t size=static_cast<size_t>(width)*height*(rgb?3:4);
                append_cdr(packet,topic,size+512,[&](Cdr& c){
                    header(c,us,frame);c<<height<<width<<std::string(rgb?"rgb8":"32FC1")<<uint8_t{0}<<uint32_t(width*(rgb?3:4))<<uint32_t(size);
                    if(rgb) {
                        auto* rgb=c.get_current_position();
                        if(!c.jump(size)) throw std::runtime_error("Image exceeds CDR buffer");
                        pack_rgb(reinterpret_cast<uint8_t*>(rgb),data);
                    } else c.serialize_array(data.data(),data.size());
                });
            }
            if(preview && demand.contains(base+"/preview/image/compressed")) {
                auto bytes=jpeg(imaging,data,width,height);
                append_cdr(packet,base+"/preview/image/compressed",bytes.size()+512,[&](Cdr& c){header(c,us,frame);c<<std::string("rgb8; jpeg compressed bgr8")<<uint32_t(bytes.size());c.serialize_array(bytes.data(),bytes.size());});
                add_message(packet,base+"/preview/camera_info",camera_info(us,frame,width,height,camera.at("projection_row_major"),vp,.5));
            }
        }
        if(demand.contains(base+"/preview/camera_info") && !demand.contains(base+"/preview/image/compressed"))
            add_message(packet,base+"/preview/camera_info",camera_info(us,frame,width/2,height/2,camera.at("projection_row_major"),vp,.5));
        if(demand.contains(base+"/perception/image_raw") || demand.contains(base+"/perception/camera_info"))
            add_message(packet,base+"/perception/camera_info",camera_info(us,frame,width/2,height/2,camera.at("projection_row_major"),vp,.5));
        if(width && (demand.contains(base+"/camera_info") || demand.contains(base+"/image_raw") || demand.contains(base+"/depth/image_raw")))
            add_message(packet,base+"/camera_info",camera_info(us,frame,width,height,camera.at("projection_row_major"),vp,1));
    }
    for(auto& [name,sources]:lidars) {
        const auto topic="/ets2/lidar/"+name+"/points";if(!demand.contains(topic)) continue;
        const auto& desc=sources.front().description;const auto order=desc.at("sources").get<std::vector<std::string>>();
        if(sources.size()!=order.size()) continue; // A source was not captured; never publish a partial LiDAR as complete.
        std::sort(sources.begin(),sources.end(),[&](const auto& a,const auto& b){return std::find(order.begin(),order.end(),a.camera)<std::find(order.begin(),order.end(),b.camera);});
        const auto axis=desc.at("axis_camera").get<std::string>();
        const auto a=std::find_if(sources.begin(),sources.end(),[&](const auto& s){return s.camera==axis;});
        if(a==sources.end()) throw std::runtime_error("LiDAR axis camera missing");
        const auto& axis_camera=a->meta->at("geometry_pass").at("camera_at_compile");
        const auto origin=axis_camera.at("camera_world_xyz").get<V>();
        const M world_from_sensor=mul(transpose(matrix(axis_camera.at("camera_rotation_row_major"))),base_to_model);
        for(auto& source:sources) {
            const auto& camera=source.meta->at("geometry_pass").at("camera_at_compile");
            const auto p=camera.at("camera_world_xyz").get<V>();
            source.width=source.description.at("source_width");
            source.eye_from_sensor=mul(matrix(camera.at("camera_rotation_row_major")),world_from_sensor);
            source.projection=camera.at("projection_row_major").get<std::array<double,16>>();
            const auto& vp=source.meta->at("geometry_gpu").at("viewports").at(0);
            source.viewport={vp.at("x"),vp.at("y"),vp.at("width"),vp.at("height")};
            const auto delta=sub(p,origin);
            if(std::hypot(delta[0],delta[1],delta[2])>1e-4 || source.description.at("beam_count")!=desc.at("beam_count"))
                throw std::runtime_error("Captured LiDAR sources are not co-located/aligned");
        }
        const uint32_t columns=desc.at("columns");const auto elevations=desc.at("elevations_deg").get<std::vector<double>>();
        const auto az=desc.at("azimuth_deg").get<std::array<double,3>>();const uint32_t count=desc.at("beam_count"),step=36;
        if(size_t(columns)*elevations.size()!=count) throw std::runtime_error("LiDAR beam grid does not match returns");
        // Beam directions are sensor configuration, independent of the truck's
        // pose. Keep trigonometry out of the per-frame PointCloud2 loop.
        struct Directions {std::array<double,3> az{};std::vector<double> elevations;std::vector<V> beams;};
        thread_local std::map<std::string,Directions> beam_cache;
        auto& directions=beam_cache[name];
        if(directions.az!=az || directions.elevations!=elevations || directions.beams.size()!=count) {
            directions.az=az;directions.elevations=elevations;directions.beams.resize(count);
            for(uint32_t beam=0;beam<count;++beam) {
                const double angle=(az[0]+(beam%columns)*az[2])*std::numbers::pi/180,elevation=elevations[beam/columns]*std::numbers::pi/180;
                directions.beams[beam]={std::cos(elevation)*std::cos(angle),std::cos(elevation)*std::sin(angle),std::sin(elevation)};
            }
        }
        append_cdr(packet,topic,size_t(count)*step+2048,[&](Cdr& c){
        header(c,us,name);c<<uint32_t(elevations.size())<<columns<<uint32_t{10};
        auto field=[&](const char* label,uint32_t offset,uint8_t datatype){c<<std::string(label)<<offset<<datatype<<uint32_t{1};};
        field("x",0,7);field("y",4,7);field("z",8,7);field("range",12,7);field("beam_index",16,6);field("status",20,2);
        field("source_camera",21,2);field("source_pixel_x",24,6);field("source_pixel_y",28,6);field("source_ray_error_deg",32,7);
        c<<false<<step<<uint32_t(columns*step)<<uint32_t(count*step);
        auto* cloud=reinterpret_cast<uint8_t*>(c.get_current_position());
        if(!c.jump(size_t(count)*step)) throw std::runtime_error("PointCloud2 exceeds CDR buffer");
        const float nan=std::numeric_limits<float>::quiet_NaN();
        struct Sample {float range;uint32_t status,pixel;float depth;};static_assert(sizeof(Sample)==16);
        for(uint32_t beam=0;beam<count;++beam) {
            const LidarSource* source=nullptr;Sample sample{nan,1,UINT32_MAX,nan};
            for(const auto& candidate:sources) {std::memcpy(&sample,candidate.data.data()+beam*16,16);if(sample.status!=1) {source=&candidate;break;}}
            const auto& direction=directions.beams[beam];
            std::array<float,4> xyzr{float(direction[0]*sample.range),float(direction[1]*sample.range),float(direction[2]*sample.range),sample.range};
            auto* point=cloud+size_t(beam)*step;std::memcpy(point,xyzr.data(),16);std::memcpy(point+16,&beam,4);
            point[20]=static_cast<uint8_t>(sample.status);point[21]=source?static_cast<uint8_t>(source->camera.back()-'0'):255;
            uint32_t x=UINT32_MAX,y=UINT32_MAX;float error=nan;
            if(source) {
                const auto width=source->width;x=sample.pixel%width;y=sample.pixel/width;
                if(std::isfinite(sample.depth)) {
                    const auto& p=source->projection;const auto& vp=source->viewport;
                    const double nx=2*(x+.5-vp[0])/vp[2]-1,ny=1-2*(y+.5-vp[1])/vp[3];
                    const V actual{(p[2]-nx*p[14])/p[0],(p[6]-ny*p[14])/p[5],-1};
                    const auto requested=mul(source->eye_from_sensor,direction);const auto norm=std::hypot(actual[0],actual[1],actual[2]);
                    double dot=0;for(size_t i=0;i<3;++i) dot+=actual[i]*requested[i]/norm;
                    error=static_cast<float>(std::acos(std::clamp(dot,-1.0,1.0))*180/std::numbers::pi);
                }
            }
            std::memcpy(point+24,&x,4);std::memcpy(point+28,&y,4);std::memcpy(point+32,&error,4);
        }
        c<<false;
        });
    }
    add_message(packet,"/tf",cdr(512+transforms.size()*256,[&](Cdr& c){c<<uint32_t(transforms.size());for(const auto& t:transforms) {header(c,us,t.parent);c<<t.frame;pose(c,t.position,t.rotation);}}));
    add_message(packet,"/ets2/frame_info/exposure",cdr(1024,[&](Cdr& c){
        header(c,us,"world");c<<manifest.at("render_frame_id").get<uint64_t>();
        c<<uint32_t(cameras.size());for(const auto& name:cameras) c<<name.get<std::string>();
        c<<uint32_t(gains.size());c.serialize_array(gains.data(),gains.size());
        c<<uint32_t(automatic.size());for(bool value:automatic) c<<value;
    }));
    add_message(packet,"/ets2/frame_info",cdr(8192,[&](Cdr& c){
        header(c,us,"world");c<<session<<manifest.at("render_frame_id").get<uint64_t>()<<first.at("frame_id").get<uint64_t>();
        for(const auto* key:{"render_time_us","simulation_time_us","paused_simulation_time_us"}) c<<first.at(key).get<uint64_t>();
        c<<uint32_t(cameras.size());for(const auto& name:cameras) c<<name.get<std::string>();
        c<<uint32_t(packet.meta["messages"].size());for(const auto& message:packet.meta["messages"]) c<<message.at("topic").get<std::string>();c<<dropped;
    }));
    return packet;
}
}
