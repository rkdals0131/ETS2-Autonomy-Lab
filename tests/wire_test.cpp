#include "wire.hpp"
#include <sys/socket.h>
#include <iostream>
#include <set>

using namespace bridge;
static void check(bool value,const char* what) {if(!value) throw std::runtime_error(std::string("Wire contract failed: ")+what);}
template<class F> static bool throws(F&& f) {try {f();} catch(const std::exception&) {return true;}return false;}
static std::pair<Socket,Socket> pair() {
    int fds[2];if(socketpair(AF_UNIX,SOCK_STREAM,0,fds)) throw std::runtime_error("socketpair");
    return {Socket(fds[0]),Socket(fds[1])};
}
static void raw_header(const Socket& s,const char* magic,uint32_t meta_size,uint64_t blob_size) {
    std::array<uint8_t,16> header{};std::memcpy(header.data(),magic,4);
    std::memcpy(header.data()+4,&meta_size,4);std::memcpy(header.data()+8,&blob_size,8);
    transfer(s,header.data(),header.size(),true);
}

int main() {
    {
        auto [a,b]=pair();Bytes blob(1000);for(size_t i=0;i<blob.size();++i) blob[i]=static_cast<uint8_t>(i*7);
        send_packet(a,{{"session","s"},{"n",1}},blob);send_packet(a,{{"empty",true}});
        const auto first=receive_packet(b);check(first.meta.at("n")==1 && first.data==blob,"round trip with blob");
        const auto second=receive_packet(b);check(second.meta.at("empty")==true && second.data.empty(),"round trip without blob");
    }
    {
        auto [a,b]=pair();raw_header(a,"OTR2",2,0);
        check(throws([&]{receive_header(b);}),"wrong magic rejected");
    }
    {
        auto [a,b]=pair();raw_header(a,"OTR1",65537,0);
        check(throws([&]{receive_header(b);}),"oversized metadata rejected");
    }
    {
        auto [a,b]=pair();raw_header(a,"OTR1",2,64ull*1024*1024+1);
        check(throws([&]{receive_header(b);}),"oversized blob rejected");
    }
    {
        auto [a,b]=pair();
        check(throws([&]{send_packet(a,{{"pad",std::string(65536,'x')}});}),"oversized metadata not sent");
        a.interrupt();
        bool transport=false;try {receive_packet(b);} catch(const TransportError&) {transport=true;} catch(...) {}
        check(transport,"closed peer is a transport error");
    }
    std::set<std::string> names;
    for(const auto& topic:topics()) check(names.insert(topic.name).second,"topic names are unique");
    std::cout<<"Transport framing and topic table contracts passed\n";
}
