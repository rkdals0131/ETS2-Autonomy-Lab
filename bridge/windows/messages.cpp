#include "messages.hpp"
#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <immintrin.h>
#include <intrin.h>

namespace bridge {
using eprosima::fastcdr::Cdr;
using V=std::array<double,3>;
using M=std::array<double,9>;
using Q=std::array<double,4>;
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
Packet static_messages(const json& rig,const json& patterns,const std::string& session,const json& settings) {
    struct Mount {std::string name;V p;Q q;std::string parent="cabin";};std::vector<Mount> mounts;
    for(const auto& view:rig.at("views")) {
        const int slot=view.at("slot");
        if(view.at("basis")!="cabin") throw std::runtime_error("ROS mounting tree requires cabin mounts");
        const auto p=mul(transpose(base_to_model),view.at("position").get<V>());
        const auto r=mul(transpose(base_to_model),from_quat(view.at("quaternion_wxyz").get<Q>()));
        mounts.push_back({view.at("camera_id").get<std::string>()+"_optical",p,quaternion(mul(r,optical))});
        const auto source="mirror"+std::to_string(slot);
        if(patterns.contains(source) && patterns.at(source).at("axis_camera")==source)
            mounts.push_back({patterns.at(source).at("name"),p,quaternion(mul(r,base_to_model))});
    }
    mounts.push_back({"imu_link",settings.value("imu_mount_base_m",V{0,0,1}),{0,0,0,1},"base_link"});
    mounts.push_back({"gnss_link",settings.value("gnss_mount_base_m",V{0,0,1}),{0,0,0,1},"base_link"});
    Packet packet{{{"session",session}}, {}};
    add_message(packet,"/tf_static",cdr(512+mounts.size()*256,[&](Cdr& c){
        c<<uint32_t(mounts.size());for(const auto& m:mounts) {header(c,0,m.parent);c<<m.name;pose(c,m.p,m.q);}
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
            auto value=vector_channel(state,key);
            if(std::string_view(key)=="truck.local.velocity.angular") for(auto& v:value) v*=2*std::numbers::pi;
            c.serialize_array(value.data(),3);
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
static V scale(V a,double s) {for(auto& v:a) v*=s;return a;}
static V cross(V a,V b) {return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};}
static bool finite(V a) {return std::all_of(a.begin(),a.end(),[](double x){return std::isfinite(x);});}
MotionSensors::MotionSensors(const json& rig,const json& truck,const json& settings):
    base_(rig.at("base_origin").get<V>()),
    mount_(settings.value("imu_mount_base_m",V{0,0,1})),
    gnss_mount_(settings.value("gnss_mount_base_m",V{0,0,1})),
    reference_(settings.value("gnss_reference_lla",V{0,0,0})),generation_(truck.at("truck_generation")) {
    if(!finite(mount_) || !finite(gnss_mount_) || !finite(reference_) || std::abs(reference_[0])>=90 || std::abs(reference_[1])>180)
        throw std::runtime_error("Invalid motion sensor mounts or WGS84 reference");
    std::map<uint32_t,V> positions;std::map<uint32_t,double> radii;
    for(const auto& a:truck.at("attributes")) {
        if(a.at("name")=="wheel.position") positions[a.at("index")]=a.at("value").get<V>();
        if(a.at("name")=="wheel.radius") radii[a.at("index")]=a.at("value").get<double>();
    }
    for(const auto& [i,p]:positions) if(radii.contains(i) && radii[i]>0)
        wheels_.push_back({i,mul(transpose(base_to_model),sub(p,base_)),radii[i]});
}
void MotionSensors::configuration(Packet& packet) const {
    const json value={{"core_version","0.23.0"},{"model","ideal"},{"angular_velocity_unit","rad/s"},
        {"imu_mount_base_m",mount_},{"gnss_mount_base_m",gnss_mount_},{"gnss_reference_lla",reference_},
        {"gnss_reference","virtual ENU at first observed base_link; WGS84 ellipsoid"},
        {"gnss_hz",10},{"motion_rate","one fresh SDK frame; no interpolation"},
        {"wheel_odometry","rolling constraints, zero initial pose, no GT correction; slip covariance unmodelled"}};
    add_message(packet,"/ets2/sensors/config",cdr(4096,[&](Cdr& c){c<<value.dump();}));
}
static V wgs84(V local,V reference) {
    constexpr double a=6378137.,e2=6.6943799901413165e-3;
    const auto lat=reference[0]*std::numbers::pi/180,lon=reference[1]*std::numbers::pi/180;
    const auto s=std::sin(lat),c=std::cos(lat),sl=std::sin(lon),cl=std::cos(lon),n=a/std::sqrt(1-e2*s*s);
    V ecef{(n+reference[2])*c*cl,(n+reference[2])*c*sl,(n*(1-e2)+reference[2])*s};
    ecef=add(ecef,mul(M{-sl,-s*cl,c*cl,cl,-s*sl,c*sl,0,c,s},local));
    const auto p=std::hypot(ecef[0],ecef[1]);double phi=std::atan2(ecef[2],p*(1-e2)),height=0;
    for(int i=0;i<8;++i) {const auto sp=std::sin(phi),np=a/std::sqrt(1-e2*sp*sp);height=p/std::cos(phi)-np;phi=std::atan2(ecef[2]+e2*np*sp,p);}
    height=p/std::cos(phi)-a/std::sqrt(1-e2*std::sin(phi)*std::sin(phi));
    return {phi*180/std::numbers::pi,std::atan2(ecef[1],ecef[0])*180/std::numbers::pi,height};
}
void MotionSensors::append(Packet& packet,const json& state,const Demand& demand) {
    if(state.at("truck_generation").get<uint64_t>()!=generation_)
        throw std::runtime_error("Truck configuration changed; restart bridge to update sensor mounts and wheel geometry");
    if(state.at("paused").get<bool>()) {previous_us_=0;return;}
    const auto us=state.at("paused_simulation_time_us").get<uint64_t>();
    if(us==previous_us_) return;
    const auto dt=previous_us_ && us>previous_us_?(us-previous_us_)*1e-6:0;
    const bool contiguous=dt>0; // Pause and unavailable-state paths reset the previous observation.
    const auto& sdk=state.at("sdk");constexpr double tau=2*std::numbers::pi;
    std::vector<uint32_t> indices;std::vector<double> angular,steering,radius;std::vector<bool> ground;
    double aa=0,ab=0,bb=0,as=0,bs=0;
    for(const auto& wheel:wheels_) {
        const auto suffix="["+std::to_string(wheel.index)+"]";
        const auto velocity="truck.wheel.angular_velocity"+suffix,angle="truck.wheel.steering"+suffix,contact="truck.wheel.on_ground"+suffix;
        if(!sdk.contains(contact) || !sdk.at(contact).value("available",false)) continue;
        const auto omega=channel(state,velocity.c_str())*tau,delta=channel(state,angle.c_str())*tau;
        if(!std::isfinite(omega) || !std::isfinite(delta)) continue;
        const bool on_ground=sdk.at(contact).at("value").get<bool>();
        indices.push_back(wheel.index);angular.push_back(omega);steering.push_back(delta);radius.push_back(wheel.radius);ground.push_back(on_ground);
        if(on_ground) {const auto a=std::cos(delta),b=-wheel.position[1]*a+wheel.position[0]*std::sin(delta),speed=omega*wheel.radius;
            aa+=a*a;ab+=a*b;bb+=b*b;as+=a*speed;bs+=b*speed;}
    }
    if(!indices.empty() && demand.contains("/ets2/wheels/state"))
        add_message(packet,"/ets2/wheels/state",cdr(256+indices.size()*32,[&](Cdr& c){header(c,us,"base_link");c<<indices<<angular<<steering<<radius<<ground;}));
    const auto determinant=aa*bb-ab*ab;
    if(determinant>1e-10) {
        const auto speed=(as*bb-bs*ab)/determinant,rate=(bs*aa-as*ab)/determinant;
        if(contiguous) {const auto theta=rate*dt;const auto distance=std::abs(theta)>1e-8?speed*dt*std::sin(theta*.5)/(theta*.5):speed*dt;
            x_+=distance*std::cos(yaw_+theta*.5);y_+=distance*std::sin(yaw_+theta*.5);yaw_+=theta;}
        if(demand.contains("/ets2/wheels/odometry")) add_message(packet,"/ets2/wheels/odometry",cdr(1024,[&](Cdr& c){
            header(c,us,"wheel_odom");c<<std::string("base_link");pose(c,{x_,y_,0},{0,0,std::sin(yaw_/2),std::cos(yaw_/2)});
            for(int i=0;i<36;++i) c<<double{0};const V v{speed,0,0},w{0,0,rate};c.serialize_array(v.data(),3);c.serialize_array(w.data(),3);
            for(int i=0;i<36;++i) c<<double{0};}));
    }
    if(state.contains("engine") && state.at("engine").contains("vehicle") && state.at("engine").at("vehicle").value("available",false)) {
        const auto& vehicle=state.at("engine").at("vehicle");const auto& p=vehicle.at("pose_physics");
        const auto rotation=from_quat(p.at("quaternion_wxyz").get<Q>()),sensor_rotation=mul(rotation,base_to_model);
        const auto velocity=mul(sensor_rotation,vector_channel(state,"truck.local.velocity.linear"));
        const auto omega=mul(sensor_rotation,scale(vector_channel(state,"truck.local.velocity.angular"),tau));
        if(finite(velocity) && finite(omega)) {
            if(contiguous && demand.contains("/ets2/imu/data_raw")) {
                const auto r=mul(rotation,sub(add(base_,mul(base_to_model,mount_)),vehicle.at("mass_center_local_m").get<V>()));
                const auto acceleration=add(scale(sub(velocity,previous_velocity_),1/dt),add(cross(scale(sub(omega,previous_omega_),1/dt),r),cross(omega,cross(omega,r))));
                const auto force=mul(transpose(sensor_rotation),sub(acceleration,V{0,-9.8100004196167,0}));
                const auto gyro=mul(transpose(sensor_rotation),omega);
                add_message(packet,"/ets2/imu/data_raw",cdr(512,[&](Cdr& c){header(c,us,"imu_link");
                    const Q q{0,0,0,1};c.serialize_array(q.data(),4);for(int i=0;i<9;++i) c<<double(i==0?-1:0);
                    c.serialize_array(gyro.data(),3);for(int i=0;i<9;++i) c<<double{0};
                    c.serialize_array(force.data(),3);for(int i=0;i<9;++i) c<<double{0};}));
            }
            previous_velocity_=velocity;previous_omega_=omega;
        } else {previous_us_=0;return;}
        const auto origin=p.at("position_m").get<V>();
        if(!anchored_) {anchor_=mul(enu,add(origin,mul(rotation,base_)));anchored_=true;}
        if(demand.contains("/ets2/gnss/fix") && us>=next_gnss_us_) {
            const auto antenna=mul(enu,add(origin,mul(rotation,add(base_,mul(base_to_model,gnss_mount_)))));
            const auto lla=wgs84(sub(antenna,anchor_),reference_);
            add_message(packet,"/ets2/gnss/fix",cdr(256,[&](Cdr& c){header(c,us,"gnss_link");c<<int8_t{0}<<uint16_t{1};
                c.serialize_array(lla.data(),3);for(int i=0;i<9;++i) c<<double{0};c<<uint8_t{2};}));
            next_gnss_us_=(us/100000+1)*100000;
        }
    } else {previous_us_=0;return;}
    previous_us_=us;
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
