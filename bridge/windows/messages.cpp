#include "messages.hpp"
#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>

namespace bridge {
using eprosima::fastcdr::Cdr;
using V=std::array<double,3>;
using M=std::array<double,9>;
using Q=std::array<double,4>;
template<class F> Bytes cdr(size_t capacity,F write) {
    Bytes bytes(capacity);eprosima::fastcdr::FastBuffer buffer(reinterpret_cast<char*>(bytes.data()),bytes.size());
    Cdr c(buffer,Cdr::LITTLE_ENDIANNESS,eprosima::fastcdr::CdrVersion::XCDRv1);
    c.set_encoding_flag(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR);
    c.serialize_encapsulation();write(c);bytes.resize(c.get_serialized_data_length());return bytes;
}
static void stamp(Cdr& c,uint64_t us) {c<<static_cast<int32_t>(us/1000000)<<static_cast<uint32_t>((us%1000000)*1000);}
static void header(Cdr& c,uint64_t us,const std::string& frame) {stamp(c,us);c<<frame;}
static M transpose(M a) {return {a[0],a[3],a[6],a[1],a[4],a[7],a[2],a[5],a[8]};}
static V mul(M a,V b) {V r{};for(int i=0;i<3;++i) for(int j=0;j<3;++j) r[i]+=a[i*3+j]*b[j];return r;}
static M mul(M a,M b) {M r{};for(int i=0;i<3;++i) for(int j=0;j<3;++j) for(int k=0;k<3;++k) r[3*i+j]+=a[3*i+k]*b[3*k+j];return r;}
static V add(V a,V b) {return {a[0]+b[0],a[1]+b[1],a[2]+b[2]};}
static V sub(V a,V b) {return {a[0]-b[0],a[1]-b[1],a[2]-b[2]};}
static M matrix(const json& a) {return {a.at(0),a.at(1),a.at(2),a.at(4),a.at(5),a.at(6),a.at(8),a.at(9),a.at(10)};}
static M from_quat(Q q) {
    const auto w=q[0],x=q[1],y=q[2],z=q[3];
    return {1-2*(y*y+z*z),2*(x*y-w*z),2*(x*z+w*y),2*(x*y+w*z),1-2*(x*x+z*z),2*(y*z-w*x),2*(x*z-w*y),2*(y*z+w*x),1-2*(x*x+y*y)};
}
static Q quaternion(M a) { // ROS x,y,z,w, matrix maps child vectors into parent.
    Q q{};const auto trace=a[0]+a[4]+a[8];
    if(trace>0) {const auto s=std::sqrt(trace+1)*2;q={(a[7]-a[5])/s,(a[2]-a[6])/s,(a[3]-a[1])/s,s/4};}
    else {
        int i=0;if(a[4]>a[0]) i=1;if(a[8]>a[i*3+i]) i=2;
        const int j=(i+1)%3,k=(i+2)%3;const auto s=std::sqrt(1+a[i*3+i]-a[j*3+j]-a[k*3+k])*2;
        q[i]=s/4;q[j]=(a[j*3+i]+a[i*3+j])/s;q[k]=(a[k*3+i]+a[i*3+k])/s;q[3]=(a[k*3+j]-a[j*3+k])/s;
    }
    double norm=0;for(double v:q) norm+=v*v;
    for(auto& v:q) v/=std::sqrt(norm);
    return q;
}
static void pose(Cdr& c,V p,Q q) {c.serialize_array(p.data(),3);c.serialize_array(q.data(),4);}
static const M enu{1,0,0,0,0,-1,0,1,0},optical{1,0,0,0,-1,0,0,0,-1},base_to_model{0,-1,0,0,0,1,-1,0,0};
Packet static_messages(const json& rig,const json& patterns,const std::string& session) {
    struct Mount {std::string name;V p;Q q;};std::vector<Mount> mounts;
    const std::map<int,std::string> names{{0,"C_FN"},{1,"C_FW"},{2,"C_RL"},{5,"C_RR"}};
    for(const auto& view:rig.at("views")) {
        const int slot=view.at("slot");
        if(view.at("basis")!="cabin") throw std::runtime_error("ROS mounting tree requires cabin mounts");
        const auto p=mul(transpose(base_to_model),view.at("position").get<V>());
        const auto r=mul(transpose(base_to_model),from_quat(view.at("quaternion_wxyz").get<Q>()));
        mounts.push_back({names.at(slot)+"_optical",p,quaternion(mul(r,optical))});
        const auto source="mirror"+std::to_string(slot);
        if(patterns.contains(source) && patterns.at(source).at("axis_camera")==source)
            mounts.push_back({patterns.at(source).at("name"),p,quaternion(mul(r,base_to_model))});
    }
    Packet packet{{{"session",session}}, {}};
    add_message(packet,"/tf_static",cdr(512+mounts.size()*256,[&](Cdr& c){
        c<<uint32_t(mounts.size());for(const auto& m:mounts) {header(c,0,"cabin");c<<m.name;pose(c,m.p,m.q);}
    }));return packet;
}
template<class F> void append_cdr(Packet& packet,const std::string& topic,size_t capacity,F write) {
    const auto offset=packet.data.size();
    // Value initialization also clears CDR alignment padding before it crosses
    // the process boundary. Serialize into the final packet, without a copy.
    packet.data.resize(offset+capacity);
    eprosima::fastcdr::FastBuffer buffer(reinterpret_cast<char*>(packet.data.data()+offset),capacity);
    Cdr c(buffer,Cdr::LITTLE_ENDIANNESS,eprosima::fastcdr::CdrVersion::XCDRv1);
    c.set_encoding_flag(eprosima::fastcdr::EncodingAlgorithmFlag::PLAIN_CDR);
    c.serialize_encapsulation();write(c);
    const auto length=c.get_serialized_data_length();packet.data.resize(offset+length);
    packet.meta["messages"].push_back({{"topic",topic},{"offset",offset},{"length",length}});
}
void add_diagnostics(Packet& packet,const json& values) {
    add_message(packet,"/diagnostics",cdr(8192,[&](Cdr& c){
        header(c,values.value("stamp_us",uint64_t{0}),"base_link");c<<uint32_t{1}<<uint8_t{0}
            <<std::string("ets2_bridge")<<std::string("Streaming")<<std::string("Windows relay / WSL Jazzy")<<uint32_t(values.size());
        for(const auto& item:values.items()) c<<item.key()<<item.value().dump();
    }));
}
json lidar_patterns(const json& rig,const json& profile) {
    std::map<std::string,json> views;
    for(const auto& view:rig.at("views")) views["mirror"+std::to_string(view.at("slot").get<int>())]=view;
    json patterns=json::object();
    for(const auto& sensor:profile.at("sensors")) {
        const auto axis=sensor.at("axis_camera").get<std::string>();
        bool complete=views.contains(axis);
        for(const auto& name:sensor.at("sources")) complete&=views.contains(name.get<std::string>());
        if(!complete) continue;
        const auto& a=views.at(axis);const auto axis_rotation=from_quat(a.at("quaternion_wxyz").get<Q>());
        for(const auto& name:sensor.at("sources")) {
            const auto& source=views.at(name.get<std::string>());
            const auto delta=sub(source.at("position").get<V>(),a.at("position").get<V>());
            if(source.at("basis")!=a.at("basis") || std::hypot(delta[0],delta[1],delta[2])>1e-4)
                throw std::runtime_error("LiDAR source mounts must share an optical origin and parent");
            auto spec=sensor;
            spec["camera_from_sensor"]=mul(mul(transpose(from_quat(source.at("quaternion_wxyz").get<Q>())),axis_rotation),base_to_model);
            patterns[name.get<std::string>()]=std::move(spec);
        }
    }
    return patterns;
}
static double channel(const json& state,const char* name) {
    const auto& sdk=state.at("sdk");
    if(!sdk.contains(name) || !sdk.at(name).value("available",false)) return std::numeric_limits<double>::quiet_NaN();
    return sdk.at(name).at("value").get<double>();
}
static V vector_channel(const json& state,const char* name) {
    const auto& sdk=state.at("sdk");const auto nan=std::numeric_limits<double>::quiet_NaN();
    if(!sdk.contains(name) || !sdk.at(name).value("available",false)) return {nan,nan,nan};
    return mul(transpose(base_to_model),sdk.at(name).at("value").get<V>());
}
Packet state_messages(const json& state,const std::string& session,const V& base) {
    const auto us=state.at("paused_simulation_time_us").get<uint64_t>();
    Packet packet{{{"session",session}}, {}};
    add_message(packet,"/clock",cdr(32,[&](Cdr& c){stamp(c,us);}));
    add_message(packet,"/ets2/vehicle/state",cdr(512,[&](Cdr& c){
        header(c,us,"base_link");c<<state.at("frame_id").get<uint64_t>()<<state.at("paused").get<bool>();
        for(const auto* key:{"render_time_us","simulation_time_us","paused_simulation_time_us"}) c<<state.at(key).get<uint64_t>();
        for(const auto* key:{"truck.speed","truck.engine.rpm","truck.input.steering","truck.input.throttle","truck.input.brake"}) c<<channel(state,key);
        for(const auto* key:{"truck.local.velocity.linear","truck.local.velocity.angular","truck.local.acceleration.linear"}) {
            auto value=vector_channel(state,key);c.serialize_array(value.data(),3);
        }
    }));
    if(state.contains("engine") && state["engine"].contains("vehicle") && state["engine"]["vehicle"].value("available",false)) {
        const auto& p=state["engine"]["vehicle"]["pose_physics"];
        const M rotation=from_quat(p.at("quaternion_wxyz").get<Q>());
        const auto world=mul(enu,add(p.at("position_m").get<V>(),mul(rotation,base)));
        const auto q=quaternion(mul(mul(enu,rotation),base_to_model));
        add_message(packet,"/ets2/ground_truth/ego/pose",cdr(256,[&](Cdr& c){header(c,us,"world");pose(c,world,q);}));
    }
    return packet;
}
static Bytes jpeg(std::span<const uint8_t> rgba,uint32_t width,uint32_t height,uint32_t out_width,uint32_t out_height) {
    using Microsoft::WRL::ComPtr;
    auto check=[](HRESULT hr){if(FAILED(hr)) throw std::runtime_error("WIC JPEG encoding failed");};
    ComPtr<IWICImagingFactory> factory;check(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICBitmap> bitmap;check(factory->CreateBitmapFromMemory(width,height,GUID_WICPixelFormat32bppRGBA,width*4,
        static_cast<UINT>(rgba.size()),const_cast<BYTE*>(rgba.data()),&bitmap));
    ComPtr<IWICBitmapScaler> scaled;check(factory->CreateBitmapScaler(&scaled));check(scaled->Initialize(bitmap.Get(),out_width,out_height,WICBitmapInterpolationModeFant));
    ComPtr<IWICFormatConverter> converter;check(factory->CreateFormatConverter(&converter));
    check(converter->Initialize(scaled.Get(),GUID_WICPixelFormat24bppBGR,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom));
    ComPtr<IStream> stream;check(CreateStreamOnHGlobal(nullptr,TRUE,&stream));
    ComPtr<IWICBitmapEncoder> encoder;check(factory->CreateEncoder(GUID_ContainerFormatJpeg,nullptr,&encoder));check(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;check(encoder->CreateNewFrame(&frame,&properties));
    PROPBAG2 option{};option.pstrName=const_cast<wchar_t*>(L"ImageQuality");VARIANT quality{};quality.vt=VT_R4;quality.fltVal=.8f;check(properties->Write(1,&option,&quality));
    check(frame->Initialize(properties.Get()));check(frame->SetSize(out_width,out_height));WICPixelFormatGUID format=GUID_WICPixelFormat24bppBGR;
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
Packet sensor_messages(std::span<const uint8_t> input,const std::string& session,const Demand& demand,uint64_t dropped,uint64_t stream_id,const json& rig) {
    uint64_t length{};if(input.size()<8) throw std::runtime_error("Truncated bundle");std::memcpy(&length,input.data(),8);
    if(length>input.size()-8) throw std::runtime_error("Invalid bundle manifest length");
    const auto manifest=json::parse(input.begin()+8,input.begin()+8+length);const auto blobs=input.subspan(8+length);
    if(manifest.at("stream_id")!=stream_id) return {}; // Ready slots can survive an earlier stream.
    std::map<std::string,std::span<const uint8_t>> files;
    for(const auto& f:manifest.at("files")) {
        const auto offset=f.at("offset").get<size_t>(),n=f.at("length").get<size_t>();
        if(offset>blobs.size() || n>blobs.size()-offset) throw std::runtime_error("Invalid bundle blob bounds");
        files.emplace(f.at("file").get<std::string>(),blobs.subspan(offset,n));
    }
    Packet packet{{{"session",session},{"frame",manifest.at("render_frame_id")},{"native_stream",stream_id}}, {}};
    size_t capacity=input.size();
    for(const auto& [file,bytes]:files) if(file.ends_with("_lidar.bin")) capacity+=bytes.size()/16*(36-16);
    packet.data.reserve(capacity);
    const auto& first=manifest.at("views").at(0).at("metadata").at("geometry_pass").at("sdk_at_compile");
    // A menu transition can finish render work after the SDK has paused and
    // stopped supplying the ego pose. State/clock continue on their own socket;
    // these sensor samples do not describe a running simulation frame.
    if(first.at("paused").get<bool>()) return {};
    const auto us=first.at("paused_simulation_time_us").get<uint64_t>();
    json cameras=json::array();
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
        uint32_t width{};M world_from_eye{};std::array<double,16> projection{};std::array<double,4> viewport{};
    };
    std::map<std::string,std::vector<LidarSource>> lidars;
    for(const auto& view:manifest.at("views")) {
        const std::string mirror=view.at("camera");
        static const std::map<std::string,std::string> names{{"mirror0","C_FN"},{"mirror1","C_FW"},{"mirror2","C_RL"},{"mirror5","C_RR"}};
        const auto name=names.at(mirror),base="/ets2/camera/"+name,frame=name+"_optical";
        const auto& meta=view.at("metadata");const auto& pass=meta.at("geometry_pass");const auto& camera=pass.at("camera_at_compile");
        if(pass.at("sdk_at_compile").at("frame_id")!=first.at("frame_id")) throw std::runtime_error("Different SDK associations within one sensor bundle");
        if(camera.at("projection_modifier_flag").get<unsigned>()!=0) throw std::runtime_error("Unsupported modified camera projection");
        cameras.push_back(name);
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
            const auto topic=base+(depth?"/depth/image_raw":"/image_raw");
            if(!preview && demand.contains(topic)) {
                const size_t size=static_cast<size_t>(width)*height*(color?3:4);
                append_cdr(packet,topic,size+512,[&](Cdr& c){
                    header(c,us,frame);c<<height<<width<<std::string(color?"rgb8":"32FC1")<<uint8_t{0}<<uint32_t(width*(color?3:4))<<uint32_t(size);
                    if(color) {
                        auto* rgb=c.get_current_position();
                        if(!c.jump(size)) throw std::runtime_error("Image exceeds CDR buffer");
                        for(size_t i=0,j=0;i<data.size();i+=4,j+=3) std::memcpy(rgb+j,data.data()+i,3);
                    } else c.serialize_array(data.data(),data.size());
                });
            }
            if(preview && demand.contains(base+"/preview/image/compressed")) {
                auto bytes=jpeg(data,width,height,width,height);
                append_cdr(packet,base+"/preview/image/compressed",bytes.size()+512,[&](Cdr& c){header(c,us,frame);c<<std::string("rgb8; jpeg compressed bgr8")<<uint32_t(bytes.size());c.serialize_array(bytes.data(),bytes.size());});
                add_message(packet,base+"/preview/camera_info",camera_info(us,frame,width,height,camera.at("projection_row_major"),vp,.5));
            }
        }
        if(demand.contains(base+"/preview/camera_info") && !demand.contains(base+"/preview/image/compressed"))
            add_message(packet,base+"/preview/camera_info",camera_info(us,frame,width/2,height/2,camera.at("projection_row_major"),vp,.5));
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
            source.width=source.description.at("source_width");source.world_from_eye=transpose(matrix(camera.at("camera_rotation_row_major")));
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
            const double angle=(az[0]+(beam%columns)*az[2])*std::numbers::pi/180,elevation=elevations[beam/columns]*std::numbers::pi/180;
            const V direction{std::cos(elevation)*std::cos(angle),std::cos(elevation)*std::sin(angle),std::sin(elevation)};
            std::array<float,4> xyzr{float(direction[0]*sample.range),float(direction[1]*sample.range),float(direction[2]*sample.range),sample.range};
            auto* point=cloud+size_t(beam)*step;std::memcpy(point,xyzr.data(),16);std::memcpy(point+16,&beam,4);
            point[20]=static_cast<uint8_t>(sample.status);point[21]=source?static_cast<uint8_t>(source->camera.back()-'0'):255;
            uint32_t x=UINT32_MAX,y=UINT32_MAX;float error=nan;
            if(source) {
                const auto width=source->width;x=sample.pixel%width;y=sample.pixel/width;
                if(std::isfinite(sample.depth)) {
                    const auto& p=source->projection;const auto& vp=source->viewport;
                    const double nx=2*(x+.5-vp[0])/vp[2]-1,ny=1-2*(y+.5-vp[1])/vp[3];
                    auto actual=mul(source->world_from_eye,V{(p[2]-nx*p[14])/p[0],(p[6]-ny*p[14])/p[5],-1});
                    const auto requested=mul(world_from_sensor,direction);const auto norm=std::hypot(actual[0],actual[1],actual[2]);
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
    add_message(packet,"/ets2/frame_info",cdr(8192,[&](Cdr& c){
        header(c,us,"world");c<<session<<manifest.at("render_frame_id").get<uint64_t>()<<first.at("frame_id").get<uint64_t>();
        for(const auto* key:{"render_time_us","simulation_time_us","paused_simulation_time_us"}) c<<first.at(key).get<uint64_t>();
        c<<uint32_t(cameras.size());for(const auto& name:cameras) c<<name.get<std::string>();
        c<<uint32_t(packet.meta["messages"].size());for(const auto& message:packet.meta["messages"]) c<<message.at("topic").get<std::string>();c<<dropped;
    }));
    return packet;
}
}
