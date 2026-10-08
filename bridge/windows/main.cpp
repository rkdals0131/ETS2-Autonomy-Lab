#include "messages.hpp"
#include "../../ot/native/ipc_layout.hpp"
#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

using namespace bridge;
using namespace std::chrono_literals;
namespace fs=std::filesystem;
struct Handle {
    HANDLE h=nullptr;
    explicit Handle(HANDLE value=nullptr):h(value==INVALID_HANDLE_VALUE?nullptr:value) {}
    Handle(const Handle&)=delete;
    ~Handle(){if(h) CloseHandle(h);}
};
static std::atomic<bool> stopped{false};
static BOOL WINAPI signal_handler(DWORD) {stopped=true;return TRUE;}
static uint64_t ticks() {return GetTickCount64();}
static std::string wsl_address() {
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};HANDLE read_raw{},write_raw{};
    if(!CreatePipe(&read_raw,&write_raw,&sa,0)) throw std::runtime_error("Cannot query WSL address");
    Handle read(read_raw),write(write_raw);SetHandleInformation(read.h,HANDLE_FLAG_INHERIT,0);
    Handle job(CreateJobObjectW(nullptr,nullptr));JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!job.h || !SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits))) throw std::runtime_error("Cannot own WSL query");
    STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESTDHANDLES|STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
    startup.hStdOutput=write.h;startup.hStdError=write.h;startup.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};std::wstring command=L"wsl.exe -d Ubuntu --cd / --exec ip -j -4 addr show dev eth0";
    if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,nullptr,&startup,&process)) throw std::runtime_error("WSL query failed to start");
    Handle owner(process.hProcess),thread(process.hThread);
    if(!AssignProcessToJobObject(job.h,owner.h)) {TerminateProcess(owner.h,1);WaitForSingleObject(owner.h,3000);throw std::runtime_error("Cannot contain WSL query");}
    ResumeThread(thread.h);
    if(WaitForSingleObject(owner.h,30000)!=WAIT_OBJECT_0) {
        TerminateJobObject(job.h,1);WaitForSingleObject(owner.h,3000);throw std::runtime_error("WSL eth0 query timed out");
    }
    CloseHandle(std::exchange(write.h,nullptr));
    char buffer[8192];DWORD size{};if(!ReadFile(read.h,buffer,sizeof(buffer),&size,nullptr)) throw std::runtime_error("No WSL address response");
    auto result=json::parse(buffer,buffer+size);
    for(const auto& iface:result) for(const auto& address:iface.at("addr_info"))
        if(address.at("family")=="inet") return address.at("local");
    throw std::runtime_error("Ubuntu has no eth0 IPv4 address");
}
static json command(json request) {
    Handle pipe;
    const auto deadline=ticks()+3000;
    while(!pipe.h) {
        const auto handle=CreateFileW(L"\\\\.\\pipe\\ot",GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
        if(handle!=INVALID_HANDLE_VALUE) {pipe.h=handle;break;}
        if(GetLastError()!=ERROR_PIPE_BUSY || ticks()>=deadline) break;
        WaitNamedPipeW(L"\\\\.\\pipe\\ot",50);
    }
    if(!pipe.h) throw std::runtime_error("Game command pipe unavailable");
    auto io=[&](void* data,DWORD size,bool output) {
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));OVERLAPPED operation{};operation.hEvent=event.h;DWORD count{};
        const bool done=output?WriteFile(pipe.h,data,size,&count,&operation):ReadFile(pipe.h,data,size,&count,&operation);
        if(!done) {
            if(GetLastError()!=ERROR_IO_PENDING) throw std::runtime_error("Game pipe I/O failed");
            if(WaitForSingleObject(event.h,3000)!=WAIT_OBJECT_0) {
                CancelIoEx(pipe.h,&operation);GetOverlappedResult(pipe.h,&operation,&count,TRUE);throw std::runtime_error("Game pipe timed out");
            }
            if(!GetOverlappedResult(pipe.h,&operation,&count,FALSE)) throw std::runtime_error("Game pipe closed");
        }
        return count;
    };
    auto text=request.dump()+"\n";size_t sent=0;
    while(sent<text.size()) {auto n=io(text.data()+sent,static_cast<DWORD>(text.size()-sent),true);if(!n) throw std::runtime_error("Empty pipe write");sent+=n;}
    std::string response;char buffer[8192];
    while(response.size()<1024*1024) {
        const auto n=io(buffer,sizeof(buffer),false);if(!n) throw std::runtime_error("Empty pipe reply");response.append(buffer,n);
        if(response.find('\n')!=std::string::npos) {auto r=json::parse(response);if(!r.at("ok").get<bool>()) throw std::runtime_error(r.at("error").get<std::string>());return r.at("result");}
    }
    throw std::runtime_error("Game reply too large");
}
class Mapping {
    Handle file_;void* base_=nullptr;uint32_t capacity_=0;bool bundles_;uint64_t sequence_=0;
public:
    explicit Mapping(bool bundles):file_(OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,bundles?L"Local\\OT_Bundles":L"Local\\OT_State")),bundles_(bundles) {
        if(!file_.h) return;
        auto* header=static_cast<ot::RingHeader*>(MapViewOfFile(file_.h,FILE_MAP_ALL_ACCESS,0,0,64));
        if(!header) throw std::runtime_error("Cannot map IPC header");
        const auto valid=header->abi==1 && header->slots==(bundles?ot::bundle_slots:ot::ring_slots) &&
            std::memcmp(header->magic,bundles?"OTBNDL01":"OTSTATE1",8)==0;
        capacity_=header->bytes_per_slot;UnmapViewOfFile(header);
        if(!valid || !capacity_ || capacity_>(bundles?ot::bundle_max_bytes:ot::slot_bytes) || (!bundles && capacity_!=ot::slot_bytes))
            throw std::runtime_error("Unsupported game shared-memory ABI");
        const size_t size=64+(bundles?ot::bundle_slots:ot::ring_slots)*(size_t{64}+capacity_);
        base_=MapViewOfFile(file_.h,FILE_MAP_ALL_ACCESS,0,0,size);
        if(!base_) throw std::runtime_error("Cannot map IPC slots");
    }
    ~Mapping(){if(base_) UnmapViewOfFile(base_);}
    bool available() const {return base_!=nullptr;}
    Bytes read() {
        Bytes bytes;
        for(uint32_t i=0;i<(bundles_?ot::bundle_slots:ot::ring_slots);++i) {
            auto* slot=ot::bundle_slot(base_,capacity_,i);
            if(InterlockedCompareExchange(&slot->state,3,2)!=2) continue;
            struct Return {ot::BundleSlot* s;~Return(){InterlockedExchange(&s->state,0);}} release{slot};
            if(slot->length>capacity_) throw std::runtime_error("Invalid shared-memory slot length");
            if(slot->sequence<=sequence_) continue;
            // Latest ready bundle wins. The copied bytes are owned before the slot is returned.
            bytes.resize(slot->length);std::memcpy(bytes.data(),slot+1,bytes.size());sequence_=slot->sequence;
        }
        return bytes;
    }
};
template<class T> class LatestQueue {
    std::mutex mutex_;std::deque<T> queue_;
public:
    bool push(T value) {std::lock_guard lock(mutex_);bool dropped=queue_.size()==2;if(dropped) queue_.pop_front();queue_.push_back(std::move(value));return dropped;}
    bool pop(T& value) {std::lock_guard lock(mutex_);if(queue_.empty()) return false;value=std::move(queue_.front());queue_.pop_front();return true;}
};
static json resolve_rig(json rig,const json& truck,const json& selected) {
    std::map<std::pair<std::string,int>,json> attributes;
    for(const auto& a:truck.at("attributes")) attributes[{a.at("name"),a.at("index").is_null()?-1:a.at("index").get<int>()}]=a.at("value");
    if(attributes.at({"id",-1})!=rig.at("truck_id")) throw std::runtime_error("Truck differs from calibrated sensor mount");
    auto wheels=rig.value("base_link_wheels",std::vector<int>{});
    if(wheels.empty()) for(const auto& [key,value]:attributes)
        if(key.first=="wheel.powered" && value.get<bool>()) wheels.push_back(key.second);
    if(wheels.empty()) throw std::runtime_error("No SDK wheel reference for base_link");
    const double axle=attributes.at({"wheel.position",wheels.front()}).at(2);
    for(int index:wheels) if(std::abs(attributes.at({"wheel.position",index}).at(2).get<double>()-axle)>.001)
        throw std::runtime_error("Select one base_link axle in the mounting preset");
    std::array<double,3> origin{};
    for(int index:wheels) {auto p=attributes.at({"wheel.position",index}).get<std::array<double,3>>();p[1]-=attributes.at({"wheel.radius",index}).get<double>();for(int i=0;i<3;++i) origin[i]+=p[i]/wheels.size();}
    json views=json::array();
    for(auto v:rig.at("views")) if(std::find(selected.begin(),selected.end(),v.at("slot"))!=selected.end()) {
        const auto p=v.at("position_base_link").get<std::array<double,3>>();v["position"]={origin[0]-p[1],origin[1]+p[2],origin[2]-p[0]};v.erase("position_base_link");views.push_back(v);
    }
    if(views.empty()) throw std::runtime_error("No selected sensor views");
    rig["views"]=views;rig["base_origin"]=origin;rig["enabled"]=true;rig["cmd"]="camera_rig";return rig;
}
struct Workers {
    std::atomic<bool> alive{true};const Socket& state;const Socket& bulk;std::vector<std::jthread> threads;
    Workers(const Socket& a,const Socket& b):state(a),bulk(b) {}
    void stop(){alive=false;state.interrupt();bulk.interrupt();for(auto& t:threads) if(t.joinable()) t.join();}
    ~Workers(){stop();}
    template<class F> void start(F f) {threads.emplace_back([this,f]{try{f();}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;alive=false;state.interrupt();bulk.interrupt();}});}
};
int main(int argc,char** argv) {
    SetConsoleCtrlHandler(signal_handler,TRUE);WSADATA winsock{};
    if(WSAStartup(MAKEWORD(2,2),&winsock)) return 1;
    struct WinsockEnd {~WinsockEnd(){WSACleanup();}} end;
    try {
        if(argc!=2) throw std::runtime_error("Usage: ets2_relay.exe path/to/bridge.local.json");
        const fs::path path=fs::absolute(argv[1]);std::ifstream file(path);json config;file>>config;
        const auto token=config.at("token").get<std::string>();if(token.size()<32) throw std::runtime_error("Missing pairing token");
        std::ifstream preset(path.parent_path()/config.at("rig").get<std::string>());json rig;preset>>rig;
        rig=resolve_rig(rig,command({{"cmd","truck_config"}}),config.value("slots",json::array({0})));
        const auto base=rig.at("base_origin").get<std::array<double,3>>();
        const auto duration=config.value("duration_s",60.0);
        if(!std::isfinite(duration) || duration<=0) throw std::runtime_error("duration_s must be positive");
        GUID uuid{};CoCreateGuid(&uuid);wchar_t wide[40];StringFromGUID2(uuid,wide,40);std::wstring w(wide);const std::string session(w.begin(),w.end());
        const auto ip=wsl_address();std::cout<<"WSL direct IP "<<ip<<"; session "<<session<<std::endl;
        auto state=connect_to(ip,config.value("state_port",17401),true),bulk=connect_to(ip,config.value("bulk_port",17400),false);
        send_packet(state,{{"token",token},{"session",session},{"channel","state"}});
        const auto welcome=receive_packet(state);if(!welcome.meta.value("ready",false) || welcome.meta.at("session")!=session) throw std::runtime_error("WSL pairing failed");
        send_packet(bulk,{{"token",token},{"session",session},{"channel","bulk"}});
        Handle reader(CreateMutexW(nullptr,TRUE,L"Local\\OT_Bundles_Reader"));
        if(!reader.h || GetLastError()==ERROR_ALREADY_EXISTS) throw std::runtime_error("Another bundle reader is active");
        command({{"cmd","lease"},{"action","claim"},{"owner",session}});
        struct Lease {std::string owner;~Lease(){try{command({{"cmd","lease"},{"action","release"},{"owner",owner}});}catch(const std::exception& e){std::cerr<<"Lease cleanup: "<<e.what()<<std::endl;}}} lease{session};
        command({{"cmd","tier"},{"value",1}});
        command({{"cmd","render_probe"},{"enabled",true},{"vehicle_metadata",true}});command(rig);
        command({{"cmd","stream"},{"action","start"},{"format","ros"},{"hz",10},{"duration",duration},{"color_gain",config.value("color_gain",1.0)}});
        std::atomic<std::shared_ptr<const Demand>> demand{std::make_shared<const Demand>()};
        std::atomic<uint64_t> dropped{0},sent{0},bytes_sent{0},echo_ms{0};
        LatestQueue<Bytes> read_queue;LatestQueue<Packet> send_queue;
        Workers workers(state,bulk);
        workers.start([&]{while(workers.alive) {
            const auto p=receive_packet(state);if(p.meta.at("session")!=session) throw std::runtime_error("Stale control session");
            demand.store(std::make_shared<const Demand>(p.meta.at("demand").get<Demand>()));
            if(!p.meta.value("capture",true)) throw std::runtime_error("Capture stopped by ROS consumer");
            const auto echo=p.meta.value("echo",uint64_t{0});if(echo) echo_ms=ticks()-echo;
        }});
        workers.start([&]{Mapping mapping(false);if(!mapping.available()) throw std::runtime_error("SDK shared state unavailable");
            while(workers.alive) {auto bytes=mapping.read();auto packet=bytes.empty()?Packet{{{"session",session}}, {}}:state_messages(json::parse(bytes),session,base);
                packet.meta["ping"]=ticks();send_packet(state,packet.meta,packet.data);std::this_thread::sleep_for(20ms);}
        });
        workers.start([&]{std::unique_ptr<Mapping> mapping;while(workers.alive) {
            if(!mapping || !mapping->available()) mapping=std::make_unique<Mapping>(true);
            if(mapping->available()) {auto bytes=mapping->read();if(!bytes.empty() && read_queue.push(std::move(bytes))) ++dropped;}
            std::this_thread::sleep_for(2ms);
        }});
        workers.start([&]{const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr)) throw std::runtime_error("COM worker initialization failed");
            struct ComEnd{~ComEnd(){CoUninitialize();}} com;
            while(workers.alive) {Bytes bytes;if(read_queue.pop(bytes)) {auto packet=sensor_messages(bytes,session,*demand.load(),dropped);if(send_queue.push(std::move(packet))) ++dropped;}else std::this_thread::sleep_for(2ms);}
        });
        workers.start([&]{uint64_t heartbeat=0;while(workers.alive) {
            Packet packet;if(send_queue.pop(packet)) {send_packet(bulk,packet.meta,packet.data);++sent;bytes_sent+=packet.data.size();heartbeat=ticks();}
            else {if(ticks()-heartbeat>250) {send_packet(bulk,{{"session",session}});heartbeat=ticks();}std::this_thread::sleep_for(2ms);}
        }});
        const auto start=ticks();uint64_t heartbeat=0,report=0;
        while(workers.alive && !stopped && ticks()-start<duration*1000) {
            if(ticks()-heartbeat>=500) {command({{"cmd","lease"},{"action","heartbeat"},{"owner",session}});heartbeat=ticks();}
            if(ticks()-report>=1000) {std::cout<<json{{"elapsed_ms",ticks()-start},{"sent_bundles",sent.load()},{"bytes",bytes_sent.load()},
                {"queue_dropped",dropped.load()},{"status_echo_ms",echo_ms.load()}}.dump()<<std::endl;report=ticks();}
            std::this_thread::sleep_for(20ms);
        }
        workers.stop();return stopped || ticks()-start>=duration*1000?0:1;
    } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
}
