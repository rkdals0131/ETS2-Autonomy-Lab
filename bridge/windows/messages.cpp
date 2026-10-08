#include "messages.hpp"
#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cmath>
#include <limits>
#include <map>

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
    return q;
}
static void pose(Cdr& c,V p,Q q) {c.serialize_array(p.data(),3);c.serialize_array(q.data(),4);}
static const M enu{1,0,0,0,0,-1,0,1,0},optical{1,0,0,0,-1,0,0,0,-1},base_to_model{0,-1,0,0,0,1,-1,0,0};
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
Packet sensor_messages(std::span<const uint8_t> input,const std::string& session,const Demand& demand,uint64_t dropped) {
    uint64_t length{};if(input.size()<8) throw std::runtime_error("Truncated bundle");std::memcpy(&length,input.data(),8);
    if(length>input.size()-8) throw std::runtime_error("Invalid bundle manifest length");
    const auto manifest=json::parse(input.begin()+8,input.begin()+8+length);const auto blobs=input.subspan(8+length);
    std::map<std::string,std::span<const uint8_t>> files;
    for(const auto& f:manifest.at("files")) {
        const auto offset=f.at("offset").get<size_t>(),n=f.at("length").get<size_t>();
        if(offset>blobs.size() || n>blobs.size()-offset) throw std::runtime_error("Invalid bundle blob bounds");
        files.emplace(f.at("file").get<std::string>(),blobs.subspan(offset,n));
    }
    Packet packet{{{"session",session},{"frame",manifest.at("render_frame_id")}}, {}};
    const auto& first=manifest.at("views").at(0).at("metadata").at("geometry_pass").at("sdk_at_compile");
    const auto us=first.at("paused_simulation_time_us").get<uint64_t>();
    json cameras=json::array();
    struct Transform {std::string frame;V position;Q rotation;};std::vector<Transform> transforms;
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
        transforms.push_back({frame,mul(enu,origin),quaternion(mul(mul(enu,transpose(rotation)),optical))});
        uint32_t width=meta.at("sensor_dimensions").at(0),height=meta.at("sensor_dimensions").at(1);
        for(const auto& desc:meta.at("images")) {
            const std::string file=desc.at("file");
            const bool color=file==mirror+"_color_ldr.bin",depth=file==mirror+"_depth_f32.bin",preview=file==mirror+"_preview_ldr.bin";
            if(!color && !depth && !preview) continue;
            const uint32_t width=desc.at("width"),height=desc.at("height");const auto data=files.at(file);
            if(!width || !height || data.size()!=static_cast<size_t>(width)*height*4 || desc.at("row_bytes")!=width*4)
                throw std::runtime_error("Invalid packed image dimensions");
            if(depth && desc.at("encoding")!="optical_depth_m_nan_invalid") throw std::runtime_error("ROS depth requires metric GPU output");
            const auto topic=base+(depth?"/depth/image_raw":"/image_raw");
            if(!preview && demand.contains(topic)) {
                Bytes rgb;if(color) {rgb.resize(static_cast<size_t>(width)*height*3);for(size_t i=0,j=0;i<data.size();i+=4,j+=3) std::memcpy(rgb.data()+j,data.data()+i,3);}
                auto pixels=color?std::span<const uint8_t>(rgb):data;
                add_message(packet,topic,cdr(pixels.size()+512,[&](Cdr& c){header(c,us,frame);c<<height<<width<<std::string(color?"rgb8":"32FC1")<<uint8_t{0}<<uint32_t(width*(color?3:4))<<uint32_t(pixels.size());c.serialize_array(pixels.data(),pixels.size());}));
            }
            if(preview && demand.contains(base+"/preview/image/compressed")) {
                auto bytes=jpeg(data,width,height,width,height);
                add_message(packet,base+"/preview/image/compressed",cdr(bytes.size()+512,[&](Cdr& c){header(c,us,frame);c<<std::string("rgb8; jpeg compressed bgr8")<<uint32_t(bytes.size());c.serialize_array(bytes.data(),bytes.size());}));
                add_message(packet,base+"/preview/camera_info",camera_info(us,frame,width,height,camera.at("projection_row_major"),vp,.5));
            }
        }
        if(demand.contains(base+"/preview/camera_info") && !demand.contains(base+"/preview/image/compressed"))
            add_message(packet,base+"/preview/camera_info",camera_info(us,frame,width/2,height/2,camera.at("projection_row_major"),vp,.5));
        if(width && (demand.contains(base+"/camera_info") || demand.contains(base+"/image_raw") || demand.contains(base+"/depth/image_raw")))
            add_message(packet,base+"/camera_info",camera_info(us,frame,width,height,camera.at("projection_row_major"),vp,1));
    }
    add_message(packet,"/tf",cdr(512+transforms.size()*256,[&](Cdr& c){c<<uint32_t(transforms.size());for(const auto& t:transforms) {header(c,us,"world");c<<t.frame;pose(c,t.position,t.rotation);}}));
    add_message(packet,"/ets2/frame_info",cdr(8192,[&](Cdr& c){
        header(c,us,"world");c<<session<<manifest.at("render_frame_id").get<uint64_t>()<<first.at("frame_id").get<uint64_t>();
        for(const auto* key:{"render_time_us","simulation_time_us","paused_simulation_time_us"}) c<<first.at(key).get<uint64_t>();
        c<<uint32_t(cameras.size());for(const auto& name:cameras) c<<name.get<std::string>();
        c<<uint32_t(packet.meta["messages"].size());for(const auto& message:packet.meta["messages"]) c<<message.at("topic").get<std::string>();c<<dropped;
    }));
    return packet;
}
}
