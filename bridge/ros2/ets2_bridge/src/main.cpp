#include "wire.hpp"
#include <rclcpp/rclcpp.hpp>
#include <std_srvs/srv/set_bool.hpp>
#include <ifaddrs.h>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace bridge;
using namespace std::chrono_literals;
static std::string eth0() {
    ifaddrs* addresses=nullptr;
    if(getifaddrs(&addresses)) throw std::runtime_error("Cannot inspect eth0");
    std::string ip;
    for(auto* a=addresses;a;a=a->ifa_next) if(a->ifa_addr && a->ifa_addr->sa_family==AF_INET && std::string(a->ifa_name)=="eth0") {
        char value[INET_ADDRSTRLEN];inet_ntop(AF_INET,&reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr,value,sizeof(value));ip=value;break;
    }
    freeifaddrs(addresses);
    if(ip.empty()) throw std::runtime_error("No eth0 IPv4 address");
    return ip;
}
static Socket listener(const std::string& ip,uint16_t port) {
    Socket s(socket(AF_INET,SOCK_STREAM,IPPROTO_TCP));
    int yes=1;setsockopt(s.id,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes));
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);inet_pton(AF_INET,ip.c_str(),&address.sin_addr);
    if(bind(s.id,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(s.id,2))
        throw std::runtime_error("Cannot listen on "+ip+":"+std::to_string(port));
    return s;
}
static Socket accept_client(const Socket& server,bool state,bool paired=false) {
    const auto deadline=std::chrono::steady_clock::now()+3s;
    while(rclcpp::ok()) {
        if(paired && std::chrono::steady_clock::now()>=deadline) throw TransportError("Bulk connection did not arrive");
        fd_set reads;FD_ZERO(&reads);FD_SET(server.id,&reads);timeval timeout{0,250000};
        if(select(server.id+1,&reads,nullptr,nullptr,&timeout)>0) {
            Socket s(accept(server.id,nullptr,nullptr));s.configure(state);return s;
        }
    }
    throw std::runtime_error("Stopped");
}
int main(int argc,char** argv) {
    rclcpp::init(argc,argv);
    try {
        auto node=std::make_shared<rclcpp::Node>("ets2_bridge");
        std::atomic<bool> capture_enabled{true};
        auto capture_service=node->create_service<std_srvs::srv::SetBool>("/ets2/capture",
            [&](const std_srvs::srv::SetBool::Request::SharedPtr request,std_srvs::srv::SetBool::Response::SharedPtr response) {
                capture_enabled=request->data;response->success=true;
                response->message="Capture request updated; F11 still requires restarting the Windows relay";
            });
        rclcpp::executors::SingleThreadedExecutor executor;executor.add_node(node);
        struct Spin {rclcpp::Executor& executor;std::jthread thread;
            explicit Spin(rclcpp::Executor& e):executor(e),thread([&e]{e.spin();}) {}
            ~Spin(){executor.cancel();if(thread.joinable()) thread.join();}
        } spin(executor);
        const auto config_path=node->declare_parameter<std::string>("config","");
        std::ifstream file(config_path);json config;file>>config;
        const auto token=config.at("token").get<std::string>();
        if(token.size()<32) throw std::runtime_error("Bridge requires a local pairing token");
        const auto ip=eth0();
        auto state_listener=listener(ip,config.value("state_port",17401));
        auto bulk_listener=listener(ip,config.value("bulk_port",17400));
        struct Publisher {Topic topic;rclcpp::GenericPublisher::SharedPtr publisher;};
        std::unordered_map<std::string,Publisher> pubs;
        for(const auto& topic:topics()) {
            auto qos=rclcpp::QoS(rclcpp::KeepLast(topic.latched?1:3));
            if(topic.latched) qos.reliable().transient_local();
            else if(topic.state) qos.reliable();
            else qos.best_effort();
            pubs.emplace(topic.name,Publisher{topic,node->create_generic_publisher(topic.name,topic.type,qos)});
        }
        std::cout<<"ETS2 serialized bridge listening at "<<ip<<std::endl;
        auto receive_publish=[&](const Socket& socket,bool state,const std::string& session,
                                 std::vector<rclcpp::SerializedMessage>& storage) {
            auto [meta,size]=receive_header(socket);
            if(meta.at("session")!=session) throw std::runtime_error("Wrong transport session");
            struct Part {size_t offset,length,index;Publisher* publisher;};
            std::vector<Part> parts;
            for(const auto& message:meta.value("messages",json::array())) {
                const auto found=pubs.find(message.at("topic").get<std::string>());
                if(found==pubs.end() || found->second.topic.state!=state) throw std::runtime_error("Unknown topic or wrong connection");
                const auto offset=message.at("offset").get<size_t>(),length=message.at("length").get<size_t>();
                if(length<4 || length>8*1024*1024 || offset>size || length>size-offset)
                    throw std::runtime_error("Invalid XCDRv1 message bounds");
                parts.push_back({offset,length,parts.size(),&found->second});
            }
            std::sort(parts.begin(),parts.end(),[](const auto& a,const auto& b){return a.offset<b.offset;});
            size_t cursor=0;
            for(const auto& part:parts) {
                if(part.offset<cursor) throw std::runtime_error("Overlapping message bodies");
                cursor=part.offset+part.length;
            }
            while(storage.size()<parts.size()) storage.emplace_back();
            std::array<uint8_t,4096> discard{};cursor=0;
            auto skip=[&](size_t end) {while(cursor<end) {
                const auto count=std::min(end-cursor,discard.size());transfer(socket,discard.data(),count,false);cursor+=count;
            }};
            for(const auto& part:parts) {
                skip(part.offset);
                auto& serialized=storage[part.index];serialized.reserve(part.length);
                auto& bytes=serialized.get_rcl_serialized_message();
                transfer(socket,bytes.buffer,part.length,false);bytes.buffer_length=part.length;cursor+=part.length;
                if(std::memcmp(bytes.buffer,"\x00\x01\x00\x00",4)) throw std::runtime_error("Invalid XCDRv1 encapsulation");
            }
            skip(size);
            // Receive the whole bundle before publishing any of it. Each receiver
            // reuses its own ROS buffers, with no intermediate bulk allocation/copy.
            for(const auto& part:parts) part.publisher->publisher->publish(storage[part.index]);
            return meta;
        };
        while(rclcpp::ok()) {
            try {
                auto state=accept_client(state_listener,true);
                auto hello=receive_packet(state);
                if(hello.meta.value("token","")!=token || hello.meta.value("channel","")!="state") throw std::runtime_error("Pairing rejected");
                const auto session=hello.meta.at("session").get<std::string>();
                send_packet(state,{{"session",session},{"ready",true},{"capture",capture_enabled.load()}});
                auto bulk=accept_client(bulk_listener,false,true);
                hello=receive_packet(bulk);
                if(hello.meta.value("token","")!=token || hello.meta.value("session","")!=session || hello.meta.value("channel","")!="bulk")
                    throw std::runtime_error("Bulk pairing rejected");
                std::atomic<bool> alive{true};
                std::atomic<uint64_t> received{0};std::mutex state_tx;
                auto stop=[&]{alive=false;state.interrupt();bulk.interrupt();};
                auto receive=[&](const Socket& socket,bool is_state) {
                    std::vector<rclcpp::SerializedMessage> storage;
                    try {while(alive && rclcpp::ok()) {
                        auto meta=receive_publish(socket,is_state,session,storage);
                        if(is_state && meta.contains("ping_us")) {
                            std::lock_guard lock(state_tx);
                            send_packet(state,{{"session",session},{"echo_us",meta.at("ping_us")}});
                        }
                        else if(meta.contains("messages")) ++received;
                    }} catch(const std::exception& e) {if(alive) std::cerr<<e.what()<<std::endl;}
                    stop();
                };
                std::jthread state_rx([&]{receive(state,true);});
                std::jthread bulk_rx([&]{receive(bulk,false);});
                try {
                    while(alive && rclcpp::ok()) {
                        json demand=json::array();
                        for(const auto& [name,p]:pubs) if(p.publisher->get_subscription_count()>0) demand.push_back(name);
                        {std::lock_guard lock(state_tx);send_packet(state,{{"session",session},{"demand",demand},{"capture",capture_enabled.load()},{"received_bundles",received.load()}});}
                        std::this_thread::sleep_for(200ms);
                    }
                } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;}
                stop(); // Both receivers unblock before the sockets and publishers can be destroyed.
            } catch(const std::exception& e) {if(rclcpp::ok()) std::cerr<<e.what()<<std::endl;}
        }
    } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;rclcpp::shutdown();return 1;}
    rclcpp::shutdown();return 0;
}
