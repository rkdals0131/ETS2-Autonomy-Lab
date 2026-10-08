#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace bridge {
using json=nlohmann::json;
using Bytes=std::vector<uint8_t>;
#ifdef _WIN32
using SocketId=SOCKET;
constexpr SocketId invalid_socket=INVALID_SOCKET;
#else
using SocketId=int;
constexpr SocketId invalid_socket=-1;
#endif
struct Socket {
    SocketId id=invalid_socket;
    Socket()=default;
    explicit Socket(SocketId value):id(value) {}
    Socket(const Socket&)=delete;
    Socket& operator=(const Socket&)=delete;
    Socket(Socket&& other) noexcept:id(std::exchange(other.id,invalid_socket)) {}
    ~Socket() {
        if(id==invalid_socket) return;
#ifdef _WIN32
        closesocket(id);
#else
        close(id);
#endif
    }
    void interrupt() const {if(id!=invalid_socket) shutdown(id,2);}
    void configure(bool state) const {
        int yes=1;
        if(state) setsockopt(id,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<char*>(&yes),sizeof(yes));
#ifdef _WIN32
        DWORD timeout=2000;
#else
        timeval timeout{2,0};
#endif
        setsockopt(id,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
        setsockopt(id,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<char*>(&timeout),sizeof(timeout));
    }
};
inline Socket connect_to(const std::string& ip,uint16_t port,bool state) {
    Socket s(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));s.configure(state);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);
    if(inet_pton(AF_INET,ip.c_str(),&address.sin_addr)!=1 ||
       connect(s.id,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0)
        throw std::runtime_error("Cannot connect to WSL "+ip+":"+std::to_string(port));
    return s;
}
inline void transfer(const Socket& s,void* data,size_t size,bool write) {
    auto* p=static_cast<char*>(data);
    while(size) {
        const int count=static_cast<int>(std::min<size_t>(size,1024*1024));
#ifdef _WIN32
        const int flags=0;
#else
        const int flags=write?MSG_NOSIGNAL:0;
#endif
        const auto n=write?send(s.id,p,count,flags):recv(s.id,p,count,flags);
        if(n<=0) throw std::runtime_error(write?"TCP send ended or timed out":"TCP receive ended or timed out");
        p+=n;size-=n;
    }
}
struct Packet {json meta;Bytes data;};
// Little-endian transport framing is independent of the ROS CDR payload.
// 16 bytes: OTR1, uint32 JSON size, uint64 blob size. One bulk packet is one bundle.
inline void send_packet(const Socket& s,const json& meta,std::span<const uint8_t> data={}) {
    const auto text=meta.dump();
    if(text.size()>65536 || data.size()>64*1024*1024) throw std::runtime_error("Transport packet exceeds capacity");
    std::array<uint8_t,16> header{'O','T','R','1'};
    uint32_t m=static_cast<uint32_t>(text.size());uint64_t n=data.size();
    std::memcpy(header.data()+4,&m,4);std::memcpy(header.data()+8,&n,8);
    transfer(s,header.data(),header.size(),true);
    transfer(s,const_cast<char*>(text.data()),text.size(),true);
    if(n) transfer(s,const_cast<uint8_t*>(data.data()),data.size(),true);
}
inline Packet receive_packet(const Socket& s) {
    std::array<uint8_t,16> header{};transfer(s,header.data(),header.size(),false);
    uint32_t m;uint64_t n;std::memcpy(&m,header.data()+4,4);std::memcpy(&n,header.data()+8,8);
    if(std::memcmp(header.data(),"OTR1",4) || m>65536 || n>64*1024*1024)
        throw std::runtime_error("Invalid transport header");
    std::string text(m,'\0');transfer(s,text.data(),text.size(),false);
    Packet p{json::parse(text),Bytes(static_cast<size_t>(n))};
    if(n) transfer(s,p.data.data(),p.data.size(),false);
    return p;
}
struct Topic {std::string name,type;bool state=false,latched=false;};
inline std::vector<Topic> topics() {
    std::vector<Topic> result{
        {"/clock","rosgraph_msgs/msg/Clock",true},
        {"/ets2/vehicle/state","ets2_msgs/msg/VehicleState",true},
        {"/ets2/ground_truth/ego/pose","geometry_msgs/msg/PoseStamped",true},
        {"/diagnostics","diagnostic_msgs/msg/DiagnosticArray",true},
        {"/tf_static","tf2_msgs/msg/TFMessage",true,true},
        {"/tf","tf2_msgs/msg/TFMessage"},
        {"/ets2/frame_info","ets2_msgs/msg/FrameInfo"}};
    for(const auto* name:{"C_FN","C_FW","C_RL","C_RR"}) {
        const std::string base=std::string("/ets2/camera/")+name;
        result.push_back({base+"/image_raw","sensor_msgs/msg/Image"});
        result.push_back({base+"/depth/image_raw","sensor_msgs/msg/Image"});
        result.push_back({base+"/camera_info","sensor_msgs/msg/CameraInfo"});
        result.push_back({base+"/preview/image/compressed","sensor_msgs/msg/CompressedImage"});
        result.push_back({base+"/preview/camera_info","sensor_msgs/msg/CameraInfo"});
        result.push_back({std::string("/ets2/ground_truth/")+name+"/objects","vision_msgs/msg/Detection3DArray"});
    }
    for(const auto* name:{"L_F","L_PL","L_PR"})
        result.push_back({std::string("/ets2/lidar/")+name+"/points","sensor_msgs/msg/PointCloud2"});
    return result;
}
inline void add_message(Packet& packet,const std::string& topic,Bytes bytes) {
    packet.meta["messages"].push_back({{"topic",topic},{"offset",packet.data.size()},{"length",bytes.size()}});
    packet.data.insert(packet.data.end(),bytes.begin(),bytes.end());
}
}
