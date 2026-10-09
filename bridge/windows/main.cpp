#include "messages.hpp"
#include "gpu_readback.hpp"
#include "game_ipc.hpp"
#include "relay_workers.hpp"
#include "state_source.hpp"
#include "render_capture.hpp"
#include <filesystem>
#include <fstream>
#include <wincodec.h>
#include <wrl/client.h>

using namespace bridge;
using namespace std::chrono_literals;
namespace fs=std::filesystem;
static std::atomic<bool> stopped{false};
static BOOL WINAPI signal_handler(DWORD) {stopped=true;return TRUE;}
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
int main(int argc,char** argv) {
    // The game renders synchronously; relay queues can discard old bundles.
    // Yield CPU scheduling priority without pinning the user's WSL or desktop.
    if(SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS))
        std::cout<<"Relay CPU priority: below normal"<<std::endl;
    else std::cerr<<"Could not lower relay CPU priority: "<<GetLastError()<<std::endl;
    SetConsoleCtrlHandler(signal_handler,TRUE);WSADATA winsock{};
    if(WSAStartup(MAKEWORD(2,2),&winsock)) return 1;
    struct WinsockEnd {~WinsockEnd(){WSACleanup();}} end;
    try {
        const bool input_only=argc>2 && std::string_view(argv[argc-1])=="--input-only";
        const auto arguments=argc-(input_only?1:0);
        if(arguments!=2 && arguments!=3) throw std::runtime_error("Usage: ets2_relay.exe path/to/bridge.local.json [stop-event] [--input-only]");
        Handle stop_event(arguments==3?OpenEventA(SYNCHRONIZE,FALSE,argv[2]):nullptr);
        if(arguments==3 && !stop_event.h) throw std::runtime_error("Launcher stop event unavailable");
        std::jthread stop_watch([&](std::stop_token stop){while(!stop.stop_requested()) {
            if(stop_event.h && WaitForSingleObject(stop_event.h,100)==WAIT_OBJECT_0) {stopped=true;break;}
            if(!stop_event.h) std::this_thread::sleep_for(100ms);
        }});
        const fs::path path=fs::absolute(argv[1]);std::ifstream file(path);json config;file>>config;
        const auto sensor_stage=config.value("diagnostic_sensor_stage",std::string("publish"));
        if(sensor_stage!="gpu_copy" && sensor_stage!="readback" && sensor_stage!="encode" && sensor_stage!="publish")
            throw std::runtime_error("diagnostic_sensor_stage must be gpu_copy, readback, encode or publish");
        if(sensor_stage=="gpu_copy" && !config.value("shared_gpu",true))
            throw std::runtime_error("gpu_copy profiling requires shared_gpu");
        const bool map_images=sensor_stage!="gpu_copy";
        const bool encode_sensors=sensor_stage=="encode" || sensor_stage=="publish";
        const bool publish_sensors=sensor_stage=="publish";
        const auto token=config.at("token").get<std::string>();if(token.size()<32) throw std::runtime_error("Missing pairing token");
        std::ifstream preset(path.parent_path()/config.at("rig").get<std::string>());json rig;preset>>rig;
        const auto selected=input_only?json::array():config.value("slots",json::array({0}));
        if(!selected.empty()) (void)rig.at("truck_id").get<std::string>();
        const auto truck=command({{"cmd","truck_config"}});
        const auto vehicle=resolve_vehicle(rig,truck,selected);
        rig=vehicle.rig;
        rig["ego_full_model"]=true;
        const auto base=rig.at("base_origin").get<std::array<double,3>>();
        std::ifstream lidar_file(path.parent_path()/config.value("lidar",std::string("../../ot/presets/phase1-lidar.json")));json lidar_profile;lidar_file>>lidar_profile;
        const auto patterns=lidar_patterns(rig,lidar_profile);
        const auto duration=config.value("duration_s",60.0);
        if(!std::isfinite(duration) || duration<=0) throw std::runtime_error("duration_s must be positive");
        const auto camera_hz=config.value("camera_hz",30.0),lidar_hz=config.value("lidar_hz",10.0);
        const auto preview_hz=config.value("preview_hz",10.0);
        const auto preview_stride=config.value("lidar_preview_stride",4u);
        if(!std::isfinite(preview_hz) || preview_hz<=0 || !preview_stride)
            throw std::runtime_error("Preview rate and LiDAR preview stride must be positive");
        if(!std::isfinite(camera_hz) || camera_hz<=0 || !std::isfinite(lidar_hz) || lidar_hz<=0 || lidar_hz>camera_hz)
            throw std::runtime_error("Rates require 0 < lidar_hz <= camera_hz");
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
        owned({{"cmd","tier"},{"value",1}});
        StateSource source(vehicle,config);
        const auto run=[&](double remaining) {
        struct DriveEnd {std::function<json(json)> control;~DriveEnd(){try{control({{"cmd","drive"},{"action","disconnect"}});}catch(...) {}}} drive_end{owned};
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
        RenderCapture capture(owned,rig,patterns,config);
        std::atomic<uint64_t> stream_id{0};
        std::atomic<bool> capture_wanted{welcome.meta.value("capture",true)},capture_active{false};
        std::atomic<std::shared_ptr<const Demand>> demand{std::make_shared<const Demand>()};
        std::atomic<uint64_t> dropped{0},sent{0},bytes_sent{0},received{0},last_ack{0};Latency latency,copy_time,encode_time,send_time;
        std::atomic<uint64_t> consumed{0},encoded{0},encoded_bytes{0};
        LatestQueue<SensorBundle> read_queue;LatestQueue<Packet> send_queue;ReadBuffers read_buffers;
        std::mutex capture_access;
        std::mutex state_tx;
        const auto state_send=[&](const json& meta,std::span<const uint8_t> bytes=std::span<const uint8_t>{}) {std::lock_guard lock(state_tx);send_packet(state,meta,bytes);};
        Workers workers(state,bulk);
        workers.start([&]{while(workers.alive) {
            const auto p=receive_packet(state);if(p.meta.at("session")!=session) throw std::runtime_error("Stale control session");
            if(p.meta.contains("demand")) demand.store(std::make_shared<const Demand>(p.meta.at("demand").get<Demand>()));
            if(p.meta.contains("capture")) capture_wanted=p.meta.at("capture").get<bool>();
            if(p.meta.contains("drive_request")) {
                const auto& incoming=p.meta.at("drive_request");json reply={{"id",incoming.at("id")},{"accepted",false}};
                try {auto request=incoming;request.erase("id");request["cmd"]="drive";reply["state"]=owned(std::move(request));reply["accepted"]=reply["state"].value("accepted",false);}
                catch(const std::exception& e) {reply["error"]=e.what();}
                state_send({{"session",session},{"drive_reply",reply}});
            }
            if(p.meta.contains("received_bundles")) received=p.meta.at("received_bundles").get<uint64_t>();
            const auto echo=p.meta.value("echo_us",uint64_t{0});if(echo) {latency.add(echo);last_ack=ticks();}
        }});
        workers.start([&]{
            auto fixed=static_messages(rig,patterns,session,source.configuration());add_sensor_configuration(fixed,source.configuration());state_send(fixed.meta,fixed.data);
            uint64_t diagnostic_time=0,last_stamp=0,last_frame=0,last_send=0,last_gnss_us=0;
            while(workers.alive) {Packet packet{{{"session",session}}, {}};
                if(const auto sample=source.latest()) {
                    const auto& telemetry=sample->telemetry;
                    const auto frame=telemetry.at("frame_id").get<uint64_t>();
                    if(frame!=last_frame) {
                        last_frame=frame;last_stamp=telemetry.at("paused_simulation_time_us").get<uint64_t>();
                        packet=state_messages(telemetry,session,base);append_motion_messages(packet,sample->motion,*demand.load(),last_gnss_us);
                    }
                }
                if(ticks()-diagnostic_time>=1000) {add_diagnostics(packet,{{"stamp_us",last_stamp},{"sent_bundles",sent.load()},{"bytes",bytes_sent.load()},
                    {"queue_dropped",dropped.load()},{"capture_active",capture_active.load()},{"status_publish_roundtrip",latency.snapshot()},
                    {"copy_elapsed",copy_time.snapshot()},{"encode_elapsed",encode_time.snapshot()},{"send_elapsed",send_time.snapshot()}});diagnostic_time=ticks();}
                if(!packet.data.empty() || ticks()-last_send>=20) {
                    packet.meta["ping_us"]=microseconds();state_send(packet.meta,packet.data);last_send=ticks();
                }
                std::this_thread::sleep_for(2ms);}
        });
        workers.start([&]{std::unique_ptr<Mapping> mapping;Bytes bytes;GpuReadback gpu;while(workers.alive) {
            std::unique_lock access(capture_access);
            if(!mapping || !mapping->available()) mapping=std::make_unique<Mapping>(true);
            if(mapping->available()) {if(bytes.empty()) bytes=read_buffers.take();const auto begin=microseconds();
                if(mapping->read(bytes)) {
                    auto bundle=decode_bundle(std::move(bytes));
                    if(bundle.manifest.at("stream_id")==stream_id.load()) {
                        gpu.read(bundle,mapping->producer(),map_images);copy_time.add(begin);++consumed;
                        if(encode_sensors) {
                            SensorBundle retired;
                            if(read_queue.push(std::move(bundle),&retired)) ++dropped;read_buffers.put(std::move(retired.data));
                        } else read_buffers.put(std::move(bundle.data));
                    } else read_buffers.put(std::move(bundle.data));
                }}
            access.unlock();std::this_thread::sleep_for(2ms);
        }});
        if(encode_sensors) workers.start([&]{const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);if(FAILED(hr)) throw std::runtime_error("COM worker initialization failed");
            struct ComEnd{~ComEnd(){CoUninitialize();}} com;
            Microsoft::WRL::ComPtr<IWICImagingFactory> imaging;
            if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&imaging)))) throw std::runtime_error("WIC initialization failed");
            while(workers.alive) {SensorBundle bundle;if(read_queue.pop(bundle)) {
                const auto begin=microseconds();auto packet=sensor_messages(bundle,session,*demand.load(),dropped,stream_id,rig,imaging.Get(),preview_stride);
                encode_time.add(begin);read_buffers.put(std::move(bundle.data));
                if(!packet.data.empty()) {++encoded;encoded_bytes+=packet.data.size();
                    if(publish_sensors && send_queue.push(std::move(packet))) ++dropped;}
            }else std::this_thread::sleep_for(2ms);}
        });
        workers.start([&]{uint64_t heartbeat=0;while(workers.alive) {
            Packet packet;if(send_queue.pop(packet)) {if(packet.meta.at("native_stream")!=stream_id.load()) continue;const auto begin=microseconds();send_packet(bulk,packet.meta,packet.data);send_time.add(begin);++sent;bytes_sent+=packet.data.size();heartbeat=ticks();}
            else {if(ticks()-heartbeat>250) {send_packet(bulk,{{"session",session}});heartbeat=ticks();}std::this_thread::sleep_for(2ms);}
        }});
        const auto start=ticks();uint64_t report=0;Demand previous_demand;json outputs=json::object();
        while(workers.alive && !stopped && lease_ok && ticks()-start<remaining*1000) {
            const auto requested=demand.load();
            if(*requested!=previous_demand) {
                outputs=capture_outputs(rig,patterns,*requested);
                owned({{"cmd","traffic_observation"},{"enabled",requested->contains("/ets2/ground_truth/traffic")}});
                previous_demand=*requested;
            }
            {
                std::lock_guard access(capture_access);
                const auto next=capture.reconcile(capture_wanted,outputs,remaining-(ticks()-start)/1000.0);
                if(next!=stream_id) {stream_id=next;read_queue.clear();send_queue.clear();}
                capture_active=capture.active();
            }
            if(ticks()-report>=1000) {std::cout<<json{{"elapsed_ms",ticks()-start},{"sent_bundles",sent.load()},{"bytes",bytes_sent.load()},
                {"session",session},{"wsl_ip",ip},{"capture_active",capture_active.load()},{"demand_topics",requested->size()},
                {"diagnostic_sensor_stage",sensor_stage},{"consumed_bundles",consumed.load()},
                {"encoded_bundles",encoded.load()},{"encoded_bytes",encoded_bytes.load()},
                {"camera_hz",camera_hz},{"lidar_hz",lidar_hz},
                {"ros_received_bundles",received.load()},{"ros_ack_age_ms",last_ack.load()?json(ticks()-last_ack.load()):json(nullptr)},
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
            source.latest(); // Surface game/clock errors even while no WSL peer is available.
            try {if(!run((deadline-ticks())/1000.0)) break;}
            catch(const TransportError& e) {std::cerr<<e.what()<<std::endl;}
            if(stopped || !lease_ok || ticks()>=deadline) break;
            std::cout<<"Network disconnected; render capture is off, motion sensors continue. Resolving Ubuntu eth0 again."<<std::endl;
            for(int i=0;i<20 && !stopped && lease_ok;++i) std::this_thread::sleep_for(50ms);
        }
        return lease_ok?0:1;
    } catch(const SensorMountMismatch& e) {std::cerr<<e.what()<<std::endl;return 2;}
    catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
}
