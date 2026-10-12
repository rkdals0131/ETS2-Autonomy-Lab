#include "messages.hpp"
#include "gpu_readback.hpp"
#include "game_ipc.hpp"
#include "relay_workers.hpp"
#include "state_source.hpp"
#include "render_capture.hpp"
#include "ros_endpoint.hpp"
#include <filesystem>
#include <fstream>
#include <wincodec.h>
#include <wrl/client.h>

using namespace bridge;
using namespace std::chrono_literals;
namespace fs=std::filesystem;
static std::atomic<bool> stopped{false};
static BOOL WINAPI signal_handler(DWORD) {stopped=true;return TRUE;}
int main(int argc,char** argv) {
    // The game renders synchronously; relay queues can discard old bundles.
    // Yield CPU scheduling priority to the game and the desktop.
    if(SetPriorityClass(GetCurrentProcess(),BELOW_NORMAL_PRIORITY_CLASS))
        std::cout<<"Relay CPU priority: below normal"<<std::endl;
    else std::cerr<<"Could not lower relay CPU priority: "<<GetLastError()<<std::endl;
    SetConsoleCtrlHandler(signal_handler,TRUE);
    try {
        // Relay arguments come first; anything from --ros-args on belongs to rclcpp.
        int positional=argc;
        for(int i=1;i<argc;++i) if(std::string_view(argv[i])=="--ros-args") {positional=i;break;}
        const bool input_only=positional>2 && std::string_view(argv[positional-1])=="--input-only";
        const auto arguments=positional-(input_only?1:0);
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
        std::ifstream preset(path.parent_path()/config.at("rig").get<std::string>());json rig;preset>>rig;
        const auto selected=input_only?json::array():config.value("slots",json::array({0}));
        if(!selected.empty()) (void)rig.at("truck_id").get<std::string>();
        const auto core_version=require_core_version();
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
        // true until a stack localization publishes odom->base_link (docs/19 D3).
        const bool gt_localization_tf=config.value("gt_localization_tf",true);
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
        StateSource source(vehicle,config,core_version);
        RosEndpoint ros(argc,argv,owned);
        const auto run=[&](double remaining) {
        struct DriveEnd {std::function<json(json)> control;~DriveEnd(){try{control({{"cmd","drive"},{"action","disconnect"}});}catch(...) {}}} drive_end{owned};
        const auto session=uuid_string();
        Handle reader(CreateMutexW(nullptr,FALSE,L"Local\\OT_Bundles_Reader"));
        const auto acquired=reader.h?WaitForSingleObject(reader.h,0):WAIT_FAILED;
        if(acquired!=WAIT_OBJECT_0 && acquired!=WAIT_ABANDONED) throw std::runtime_error("Another bundle reader is active");
        struct ReaderEnd {HANDLE h;~ReaderEnd(){ReleaseMutex(h);}} reader_end{reader.h};
        if(!lease_ok || stopped) return false;
        RenderCapture capture(owned,rig,patterns,config);
        std::atomic<uint64_t> stream_id{0};
        std::atomic<bool> capture_active{false};
        std::atomic<uint64_t> dropped{0},sent{0},bytes_sent{0};Latency copy_time,encode_time,send_time;
        std::atomic<uint64_t> consumed{0},encoded{0},encoded_bytes{0};
        LatestQueue<SensorBundle> read_queue;LatestQueue<Packet> send_queue;ReadBuffers read_buffers;
        std::mutex capture_access;
        Workers workers;
        workers.start([&]{
            auto fixed=static_messages(rig,patterns,session,source.configuration());add_sensor_configuration(fixed,source.configuration());ros.publish(fixed);
            uint64_t diagnostic_time=0,last_stamp=0,last_frame=0,last_gnss_us=0;
            while(workers.alive) {Packet packet{{{"session",session}}, {}};
                if(const auto sample=source.latest()) {
                    const auto& telemetry=sample->telemetry;
                    const auto frame=telemetry.at("frame_id").get<uint64_t>();
                    if(frame!=last_frame) {
                        last_frame=frame;last_stamp=telemetry.at("paused_simulation_time_us").get<uint64_t>();
                        packet=state_messages(telemetry,session,base,gt_localization_tf);append_motion_messages(packet,sample->motion,*ros.demand(),last_gnss_us);
                    }
                }
                if(ticks()-diagnostic_time>=1000) {add_diagnostics(packet,{{"stamp_us",last_stamp},{"sent_bundles",sent.load()},{"bytes",bytes_sent.load()},
                    {"queue_dropped",dropped.load()},{"capture_active",capture_active.load()},{"drive_commands",ros.diagnostics()},{"slow_publishes",ros.publish_stats()},
                    {"copy_elapsed",copy_time.snapshot()},{"encode_elapsed",encode_time.snapshot()},{"send_elapsed",send_time.snapshot()}});diagnostic_time=ticks();}
                if(!packet.data.empty()) ros.publish(packet);
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
                const auto begin=microseconds();auto packet=sensor_messages(bundle,session,*ros.demand(),dropped,stream_id,rig,imaging.Get(),preview_stride);
                encode_time.add(begin);read_buffers.put(std::move(bundle.data));
                if(!packet.data.empty()) {++encoded;encoded_bytes+=packet.data.size();
                    if(publish_sensors && send_queue.push(std::move(packet))) ++dropped;}
            }else std::this_thread::sleep_for(2ms);}
        });
        workers.start([&]{while(workers.alive) {
            Packet packet;if(send_queue.pop(packet)) {if(packet.meta.at("native_stream")!=stream_id.load()) continue;const auto begin=microseconds();ros.publish(packet);send_time.add(begin);++sent;bytes_sent+=packet.data.size();}
            else std::this_thread::sleep_for(2ms);
        }});
        const auto start=ticks();uint64_t report=0;Demand previous_demand;json outputs=json::object();
        while(workers.alive && !stopped && lease_ok && ticks()-start<remaining*1000) {
            const auto requested=ros.demand();
            if(*requested!=previous_demand) {
                outputs=capture_outputs(rig,patterns,*requested);
                owned({{"cmd","traffic_observation"},{"enabled",requested->contains("/ets2/ground_truth/traffic")}});
                previous_demand=*requested;
            }
            {
                std::lock_guard access(capture_access);
                const auto next=capture.reconcile(ros.capture_wanted(),outputs,remaining-(ticks()-start)/1000.0);
                if(next!=stream_id) {stream_id=next;read_queue.clear();send_queue.clear();}
                capture_active=capture.active();
            }
            if(ticks()-report>=1000) {
                // The launcher reads this line; ACC/LCC ownership comes from the same SDK sample.
                json drive=nullptr,paused=nullptr;
                if(const auto sample=source.latest()) {
                    paused=sample->telemetry.value("paused",false);
                    if(sample->telemetry.contains("drive")) {
                        const auto& d=sample->telemetry.at("drive");
                        drive={{"armed",d.value("armed",false)},{"axes",d.value("axes",0u)},{"reason",d.value("reason",std::string{})}};
                    }
                }
                std::cout<<json{{"elapsed_ms",ticks()-start},{"sent_bundles",sent.load()},{"bytes",bytes_sent.load()},
                {"session",session},{"capture_active",capture_active.load()},{"demand_topics",requested->size()},{"paused",paused},{"drive",drive},
                {"diagnostic_sensor_stage",sensor_stage},{"consumed_bundles",consumed.load()},
                {"encoded_bundles",encoded.load()},{"encoded_bytes",encoded_bytes.load()},
                {"camera_hz",camera_hz},{"lidar_hz",lidar_hz},
                {"queue_dropped",dropped.load()},{"drive_commands",ros.diagnostics()},{"slow_publishes",ros.publish_stats()},
                {"copy_elapsed",copy_time.snapshot()},{"encode_elapsed",encode_time.snapshot()},{"send_elapsed",send_time.snapshot()}}.dump()<<std::endl;report=ticks();}
            std::this_thread::sleep_for(20ms);
        }
        workers.stop();
        if(workers.fatal) throw std::runtime_error("Relay session ended on a data or game error; explicit restart required");
        };
        run(duration);
        return lease_ok?0:1;
    } catch(const SensorMountMismatch& e) {std::cerr<<e.what()<<std::endl;return 2;}
    catch(const std::exception& e) {std::cerr<<e.what()<<std::endl;return 1;}
}
