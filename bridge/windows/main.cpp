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
static std::string uuid_string() {
    GUID value{};if(FAILED(CoCreateGuid(&value))) throw std::runtime_error("Cannot create session ID");
    wchar_t text[40];StringFromGUID2(value,text,40);char ascii[40];
    if(!WideCharToMultiByte(CP_UTF8,0,text,-1,ascii,sizeof(ascii),nullptr,nullptr)) throw std::runtime_error("Cannot encode session ID");
    return ascii;
}
static uint64_t ticks() {return GetTickCount64();}
static uint64_t microseconds() {return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
struct Latency {
    std::mutex mutex;std::deque<double> values;
    void add(uint64_t ping) {const double ms=(microseconds()-ping)/1000.0;std::lock_guard lock(mutex);if(values.size()==4096) values.pop_front();values.push_back(ms);}
    json snapshot() {std::vector<double> v;{std::lock_guard lock(mutex);v.assign(values.begin(),values.end());}
        if(v.empty()) return nullptr;std::sort(v.begin(),v.end());
        return {{"samples",v.size()},{"p50_ms",v[(v.size()-1)/2]},{"p95_ms",v[(v.size()-1)*95/100]},{"p99_ms",v[(v.size()-1)*99/100]},{"max_ms",v.back()}};
    }
};
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
        // The caller owns OT_Bundles_Reader for this mapping's whole lifetime.
        // No other live consumer can own state 3; recover an interrupted copy.
        if(bundles_) for(uint32_t i=0;i<ot::bundle_slots;++i)
            InterlockedCompareExchange(&ot::bundle_slot(base_,capacity_,i)->state,0,3);
    }
    ~Mapping(){if(base_) UnmapViewOfFile(base_);}
    bool available() const {return base_!=nullptr;}
    bool read(Bytes& bytes) {
        if(bundles_) {
            ot::BundleSlot* oldest=nullptr;
            for(uint32_t i=0;i<ot::bundle_slots;++i) {
                auto* slot=ot::bundle_slot(base_,capacity_,i);
                if(InterlockedCompareExchange(&slot->state,2,2)==2 && (!oldest || slot->sequence<oldest->sequence)) oldest=slot;
            }
            if(!oldest || InterlockedCompareExchange(&oldest->state,3,2)!=2) return false;
            struct Return {ot::BundleSlot* s;~Return(){InterlockedExchange(&s->state,0);}} release{oldest};
            if(oldest->length>capacity_) throw std::runtime_error("Invalid shared-memory slot length");
            bytes.resize(oldest->length);std::memcpy(bytes.data(),oldest+1,bytes.size());return true;
        }
        bool copied=false;
        for(uint32_t i=0;i<(bundles_?ot::bundle_slots:ot::ring_slots);++i) {
            auto* slot=ot::bundle_slot(base_,capacity_,i);
            if(InterlockedCompareExchange(&slot->state,3,2)!=2) continue;
            struct Return {ot::BundleSlot* s;~Return(){InterlockedExchange(&s->state,0);}} release{slot};
            if(slot->length>capacity_) throw std::runtime_error("Invalid shared-memory slot length");
            if(slot->sequence<=sequence_) continue;
            // Latest ready bundle wins. The copied bytes are owned before the slot is returned.
            bytes.resize(slot->length);std::memcpy(bytes.data(),slot+1,bytes.size());sequence_=slot->sequence;copied=true;
        }
        return copied;
    }
};
template<class T> class LatestQueue {
    std::mutex mutex_;std::deque<T> queue_;
public:
    bool push(T value,T* retired=nullptr) {std::lock_guard lock(mutex_);bool dropped=queue_.size()==2;
        if(dropped) {if(retired) *retired=std::move(queue_.front());queue_.pop_front();}queue_.push_back(std::move(value));return dropped;}
    bool pop(T& value) {std::lock_guard lock(mutex_);if(queue_.empty()) return false;value=std::move(queue_.front());queue_.pop_front();return true;}
    void clear() {std::lock_guard lock(mutex_);queue_.clear();}
};
// At most one buffer is copying, two are queued, and one is encoding. Return
// their storage after use instead of zero-initializing a new 24 MB bundle.
class ReadBuffers {
    std::mutex mutex_;std::vector<Bytes> free_;
public:
    Bytes take() {std::lock_guard lock(mutex_);if(free_.empty()) return {};auto b=std::move(free_.back());free_.pop_back();return b;}
    void put(Bytes b) {if(!b.capacity()) return;std::lock_guard lock(mutex_);free_.push_back(std::move(b));}
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
    std::atomic<bool> alive{true},network_failed{false},fatal{false};const Socket& state;const Socket& bulk;std::vector<std::jthread> threads;
    Workers(const Socket& a,const Socket& b):state(a),bulk(b) {}
    void stop(){alive=false;state.interrupt();bulk.interrupt();for(auto& t:threads) if(t.joinable()) t.join();}
    ~Workers(){stop();}
    template<class F> void start(F f) {threads.emplace_back([this,f]{
        try{f();}
        catch(const TransportError& e) {if(alive.exchange(false)) {network_failed=true;std::cerr<<e.what()<<std::endl;}state.interrupt();bulk.interrupt();}
        catch(const std::exception& e) {if(alive.exchange(false)) {fatal=true;std::cerr<<e.what()<<std::endl;}state.interrupt();bulk.interrupt();}
    });}
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
        std::ifstream lidar_file(path.parent_path()/config.value("lidar",std::string("../../ot/presets/phase1-lidar.json")));json lidar_profile;lidar_file>>lidar_profile;
        const auto patterns=lidar_patterns(rig,lidar_profile);
        const auto duration=config.value("duration_s",60.0);
        if(!std::isfinite(duration) || duration<=0) throw std::runtime_error("duration_s must be positive");
        const auto owner=uuid_string();
        command({{"cmd","lease"},{"action","claim"},{"owner",owner}});
        struct Lease {std::string owner;~Lease(){try{command({{"cmd","lease"},{"action","release"},{"owner",owner}});}catch(const std::exception& e){std::cerr<<"Lease cleanup: "<<e.what()<<std::endl;}}} lease{owner};
        std::atomic<bool> lease_ok{true};
        std::jthread keepalive([&](std::stop_token stop){while(!stop.stop_requested()) {
            try {command({{"cmd","lease"},{"action","heartbeat"},{"owner",owner}});}
            catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;lease_ok=false;break;}
            for(int i=0;i<10 && !stop.stop_requested();++i) std::this_thread::sleep_for(50ms);
        }});
        const auto owned=[&](json request){request["owner"]=owner;return command(std::move(request));};
        const auto run=[&](double remaining) {
        const auto session=uuid_string();
        const auto ip=wsl_address();std::cout<<"WSL direct IP "<<ip<<"; session "<<session<<std::endl;
        auto state=connect_to(ip,config.value("state_port",17401),true),bulk=connect_to(ip,config.value("bulk_port",17400),false);
        send_packet(state,{{"token",token},{"session",session},{"channel","state"}});
        const auto welcome=receive_packet(state);if(!welcome.meta.value("ready",false) || welcome.meta.at("session")!=session) throw std::runtime_error("WSL pairing failed");
        send_packet(bulk,{{"token",token},{"session",session},{"channel","bulk"}});
        Handle reader(CreateMutexW(nullptr,FALSE,L"Local\\OT_Bundles_Reader"));
        const auto acquired=reader.h?WaitForSingleObject(reader.h,0):WAIT_FAILED;
        if(acquired!=WAIT_OBJECT_0 && acquired!=WAIT_ABANDONED) throw std::runtime_error("Another bundle reader is active");
        struct ReaderEnd {HANDLE h;~ReaderEnd(){ReleaseMutex(h);}} reader_end{reader.h};
        if(!lease_ok || stopped) return false;
        struct Idle {decltype(owned)& control;~Idle(){try{control({{"cmd","tier"},{"value",0}});}catch(...) {}}} idle{owned};
        std::atomic<uint64_t> stream_id{0};
        std::atomic<bool> capture_wanted{welcome.meta.value("capture",true)},capture_active{false};
        std::atomic<std::shared_ptr<const Demand>> demand{std::make_shared<const Demand>()};
        std::atomic<uint64_t> dropped{0},sent{0},bytes_sent{0};Latency latency,copy_time,encode_time,send_time;
        LatestQueue<Bytes> read_queue;LatestQueue<Packet> send_queue;ReadBuffers read_buffers;
        Workers workers(state,bulk);
        workers.start([&]{while(workers.alive) {
            const auto p=receive_packet(state);if(p.meta.at("session")!=session) throw std::runtime_error("Stale control session");
            if(p.meta.contains("demand")) demand.store(std::make_shared<const Demand>(p.meta.at("demand").get<Demand>()));
            if(p.meta.contains("capture")) capture_wanted=p.meta.at("capture").get<bool>();
            const auto echo=p.meta.value("echo_us",uint64_t{0});if(echo) latency.add(echo);
        }});
        workers.start([&]{Mapping mapping(false);if(!mapping.available()) throw std::runtime_error("SDK shared state unavailable");
            auto fixed=static_messages(rig,patterns,session);send_packet(state,fixed.meta,fixed.data);
            uint64_t diagnostic_time=0,last_stamp=0,last_frame=0;Bytes bytes;
            while(workers.alive) {Packet packet{{{"session",session}}, {}};
                if(mapping.read(bytes)) {
                    const auto sample=json::parse(bytes);const auto time=sample.at("paused_simulation_time_us").get<uint64_t>();
                    if(last_frame && (time<last_stamp || (sample.at("timer_flags").get<uint32_t>()&1)))
                        throw std::runtime_error("SDK clock restarted; restart relay for a new clock session");
                    last_frame=sample.at("frame_id");last_stamp=time;packet=state_messages(sample,session,base);
                }
                if(ticks()-diagnostic_time>=1000) {add_diagnostics(packet,{{"stamp_us",last_stamp},{"sent_bundles",sent.load()},{"bytes",bytes_sent.load()},
                    {"queue_dropped",dropped.load()},{"capture_active",capture_active.load()},{"status_publish_roundtrip",latency.snapshot()},
                    {"copy_elapsed",copy_time.snapshot()},{"encode_elapsed",encode_time.snapshot()},{"send_elapsed",send_time.snapshot()}});diagnostic_time=ticks();}
                packet.meta["ping_us"]=microseconds();send_packet(state,packet.meta,packet.data);std::this_thread::sleep_for(20ms);}
        });
        workers.start([&]{std::unique_ptr<Mapping> mapping;Bytes bytes;while(workers.alive) {
            if(!mapping || !mapping->available()) mapping=std::make_unique<Mapping>(true);
            if(mapping->available()) {if(bytes.empty()) bytes=read_buffers.take();const auto begin=microseconds();
                if(mapping->read(bytes)) {copy_time.add(begin);Bytes retired;if(read_queue.push(std::move(bytes),&retired)) ++dropped;read_buffers.put(std::move(retired));}}
            std::this_thread::sleep_for(2ms);
        }});
        workers.start([&]{const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr)) throw std::runtime_error("COM worker initialization failed");
            struct ComEnd{~ComEnd(){CoUninitialize();}} com;
            while(workers.alive) {Bytes bytes;if(read_queue.pop(bytes)) {const auto begin=microseconds();auto packet=sensor_messages(bytes,session,*demand.load(),dropped,stream_id,rig);encode_time.add(begin);read_buffers.put(std::move(bytes));if(!packet.data.empty() && send_queue.push(std::move(packet))) ++dropped;}else std::this_thread::sleep_for(2ms);}
        });
        workers.start([&]{uint64_t heartbeat=0;while(workers.alive) {
            Packet packet;if(send_queue.pop(packet)) {if(packet.meta.at("native_stream")!=stream_id.load()) continue;const auto begin=microseconds();send_packet(bulk,packet.meta,packet.data);send_time.add(begin);++sent;bytes_sent+=packet.data.size();heartbeat=ticks();}
            else {if(ticks()-heartbeat>250) {send_packet(bulk,{{"session",session}});heartbeat=ticks();}std::this_thread::sleep_for(2ms);}
        }});
        const auto start=ticks();uint64_t report=0;Demand previous_demand;
        while(workers.alive && !stopped && lease_ok && ticks()-start<remaining*1000) {
            if(capture_wanted.load()!=capture_active.load()) {
                if(capture_wanted) {
                    owned({{"cmd","tier"},{"value",1}});
                    owned({{"cmd","render_probe"},{"enabled",true},{"vehicle_metadata",true},{"draw_metadata",false}});owned(rig);
                    stream_id=owned({{"cmd","stream"},{"action","start"},{"format","ros"},{"hz",10},{"duration",remaining-(ticks()-start)/1000.0},
                        {"color_gain",config.value("color_gain",1.0)},{"outputs",json::object()},{"lidars",patterns}}).at("stream_id").get<uint64_t>();
                    previous_demand.clear();capture_active=true;
                } else {
                    stream_id=0;owned({{"cmd","tier"},{"value",0}});read_queue.clear();send_queue.clear();capture_active=false;
                }
            }
            const auto requested=demand.load();
            if(capture_active && *requested!=previous_demand) {
                json outputs=json::object();
                const std::map<int,std::string> camera_names{{0,"C_FN"},{1,"C_FW"},{2,"C_RL"},{5,"C_RR"}};
                for(const auto& view:rig.at("views")) {
                    const int slot=view.at("slot");const auto base_topic="/ets2/camera/"+camera_names.at(slot);
                    auto selected=json::array();
                    if(requested->contains(base_topic+"/image_raw")) selected.push_back("color");
                    if(requested->contains(base_topic+"/depth/image_raw")) selected.push_back("depth");
                    if(requested->contains(base_topic+"/preview/image/compressed")) selected.push_back("preview");
                    if(requested->contains(base_topic+"/camera_info") || requested->contains(base_topic+"/preview/camera_info") ||
                       requested->contains("/ets2/ground_truth/"+camera_names.at(slot)+"/objects") || requested->contains("/ets2/ground_truth/"+camera_names.at(slot)+"/markers") || requested->contains("/tf") || requested->contains("/ets2/frame_info")) selected.push_back("metadata");
                    const auto mirror="mirror"+std::to_string(slot);
                    if(patterns.contains(mirror) && requested->contains("/ets2/lidar/"+patterns.at(mirror).at("name").get<std::string>()+"/points")) selected.push_back("lidar");
                    if(!selected.empty()) outputs[mirror]=selected;
                }
                owned({{"cmd","stream"},{"action","update"},{"format","ros"},{"color_gain",config.value("color_gain",1.0)},{"outputs",outputs},{"lidars",patterns}});
                previous_demand=*requested;
            }
            if(ticks()-report>=1000) {std::cout<<json{{"elapsed_ms",ticks()-start},{"sent_bundles",sent.load()},{"bytes",bytes_sent.load()},
                {"queue_dropped",dropped.load()},{"status_publish_roundtrip",latency.snapshot()},
                {"copy_elapsed",copy_time.snapshot()},{"encode_elapsed",encode_time.snapshot()},{"send_elapsed",send_time.snapshot()}}.dump()<<std::endl;report=ticks();}
            std::this_thread::sleep_for(20ms);
        }
        workers.stop();
        if(workers.fatal) throw std::runtime_error("Relay session ended on a data or game error; explicit restart required");
        return workers.network_failed.load();
        };
        const auto deadline=ticks()+static_cast<uint64_t>(duration*1000);
        while(!stopped && lease_ok && ticks()<deadline) {
            try {if(!run((deadline-ticks())/1000.0)) break;}
            catch(const TransportError& e) {std::cerr<<e.what()<<std::endl;}
            if(stopped || !lease_ok || ticks()>=deadline) break;
            std::cout<<"Network disconnected; capture is off. Resolving Ubuntu eth0 again."<<std::endl;
            for(int i=0;i<20 && !stopped && lease_ok;++i) std::this_thread::sleep_for(50ms);
        }
        return lease_ok?0:1;
    } catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
}
