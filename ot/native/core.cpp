#include "ot.hpp"
#include "../include/geometry.hpp"
#include "../include/mount_layout.hpp"
#include "build_identity.hpp"
#include "render_probe.hpp"
#include "parallel_passes.hpp"
#include "thread_sampler.hpp"
#include "module_api.hpp"
#include <scssdk_telemetry.h>
#include <array>
#include <fstream>
#include <stdexcept>
#include <cstring>
#include <deque>
#include <map>
#include <cmath>

namespace ot {
static const OtDriveHost* input_host{};
// A host advertises each optional tail by its size; older loaders end before request_aux.
static bool has_tail(size_t end) {return input_host && input_host->size>=end;}
static bool split_axes() {return has_tail(offsetof(OtDriveHost,request_axes)+sizeof(OtDriveHost::request_axes)) && input_host->request_axes;}
static bool aux_brakes() {return has_tail(offsetof(OtDriveHost,request_aux)+sizeof(OtDriveHost::request_aux)) && input_host->request_aux;}
static bool turn_signals() {return has_tail(offsetof(OtDriveHost,signal)+sizeof(OtDriveHost::signal)) && input_host->signal;}
static json drive_status(OtDriveStatus status) {
    const char* reasons[]={"disabled","disarmed","armed","command","expired","manual","paused","panic","disconnected","unloaded","input_unavailable","profile_unsupported"};
    // A newer resident loader may report reasons this core does not know.
    const auto reason=static_cast<size_t>(status.reason);
    return {{"available",bool(status.available)},{"permitted",bool(status.permitted)},{"profile_supported",bool(status.profile_supported)},
        {"armed",bool(status.armed)},{"active",bool(status.active)},{"reason",reason<std::size(reasons)?reasons[reason]:"unknown"},
        {"owner",status.owner},{"epoch",status.epoch},{"sequence",status.sequence},{"deadline_ms",status.deadline_ms},
        {"command_window_ms",status.command_window_ms},{"steering",status.steering},{"throttle",status.throttle},{"brake",status.brake},
        {"manual_steering",status.manual_steering},{"manual_throttle",status.manual_throttle},{"manual_brake",status.manual_brake},{"error",status.error}};
}
static json current_drive() {
    OtDriveStatus status;const OtDriveRequest request{};
    if(input_host) input_host->request(input_host->context,&request,&status);
    uint32_t axes=status.armed?ot_drive_all:0;
    OtDriveAux aux{-1,0};
    if(aux_brakes()) input_host->request_aux(input_host->context,&request,ot_drive_all,nullptr,&status,&axes,&aux);
    else if(split_axes()) input_host->request_axes(input_host->context,&request,ot_drive_all,&status,&axes);
    auto result=drive_status(status);result["axes"]=axes;result["independent_axes"]=split_axes();
    result["auxiliary_brakes"]=aux_brakes();result["retarder"]=aux.retarder;result["engine_brake"]=aux.engine_brake!=0;result["turn_signals"]=turn_signals();return result;
}
static void configure_drive(const json& settings) {
    if(!input_host) return;
    const auto path=settings.value("drive_controls_path",std::string{});
    input_host->configure(input_host->context,settings.value("allow_drive",false) && settings.value("singleplayer_research",false),path.c_str());
}
class Runtime;
// Schema field type -> SDK value type for channel registration.
static scs_value_type_t sdk_value_type(const std::string& type) {
    if(type=="dplacement") return SCS_VALUE_TYPE_dplacement;
    if(type=="fvector") return SCS_VALUE_TYPE_fvector;
    if(type=="s32") return SCS_VALUE_TYPE_s32;
    if(type=="u32") return SCS_VALUE_TYPE_u32;
    if(type=="bool") return SCS_VALUE_TYPE_bool;
    if(type=="float") return SCS_VALUE_TYPE_float;
    throw std::runtime_error("Unsupported SDK field type in schema: "+type);
}
struct Channel {
    Runtime* owner; std::string name; scs_value_type_t type;
    scs_u32_t index=SCS_U32_NIL; scs_result_t registration=SCS_RESULT_not_found;
    std::string key() const {return index==SCS_U32_NIL?name:name+"["+std::to_string(index)+"]";}
};
struct ArrayHeader { uintptr_t vtable,data; uint64_t size,capacity; };
// SDK configuration attribute value as JSON; false for types not used by any configuration.
bool decode_attribute(const scs_value_t& value,json& decoded) {
    switch(value.type) {
        case SCS_VALUE_TYPE_bool:decoded=value.value_bool.value!=0;return true;
        case SCS_VALUE_TYPE_s32:decoded=value.value_s32.value;return true;
        case SCS_VALUE_TYPE_u32:decoded=value.value_u32.value;return true;
        case SCS_VALUE_TYPE_u64:decoded=value.value_u64.value;return true;
        case SCS_VALUE_TYPE_float:decoded=value.value_float.value;return true;
        case SCS_VALUE_TYPE_double:decoded=value.value_double.value;return true;
        case SCS_VALUE_TYPE_string:decoded=value.value_string.value;return true;
        case SCS_VALUE_TYPE_fvector: {const auto& v=value.value_fvector;decoded={v.x,v.y,v.z};return true;}
        case SCS_VALUE_TYPE_dvector: {const auto& v=value.value_dvector;decoded={v.x,v.y,v.z};return true;}
        default:return false;
    }
}

class Runtime {
public:
    explicit Runtime(const scs_telemetry_init_params_v100_t& api):api_(api),schema_(json::parse(OT_SCHEMA)) {}
    ~Runtime() { shutdown(); }
    void initialize();
    bool shutdown() noexcept;
    void panic() noexcept;
    void release_runtime(OtDriveReason reason,const char* message) noexcept;
    void event(scs_event_t event,const void* data);
    void value(const Channel& channel,const scs_value_t* value);
    json command(const json& request);
    json reload_permissions();
    void poll_lease();
    static void SCSAPIFUNC event_callback(scs_event_t event,const void* data,scs_context_t context) noexcept {
        auto& self=*static_cast<Runtime*>(context);
        const auto start=qpc_now();
        if(event==SCS_TELEMETRY_EVENT_frame_start) self.main_thread_.store(GetCurrentThreadId(),std::memory_order_relaxed);
        try {self.event(event,data);} catch(const std::exception& e) {self.panic();log(std::string("SDK event error: ")+e.what());}
        catch(...) {self.panic();}
        auto& timing=event==SCS_TELEMETRY_EVENT_frame_end?self.frame_end_timing_:
            event==SCS_TELEMETRY_EVENT_frame_start?self.frame_start_timing_:self.other_event_timing_;
        timing.add(qpc_now()-start);
    }
    static void SCSAPIFUNC channel_callback(scs_string_t,scs_u32_t,const scs_value_t* value,scs_context_t context) noexcept {
        auto& channel=*static_cast<Channel*>(context);
        const auto start=qpc_now();
        try {channel.owner->value(channel,value);} catch(...) {channel.owner->panic();}
        channel.owner->channel_timing_.add(qpc_now()-start);
    }
    // Game-thread cost of the SDK callbacks (they run at the SDK frame rate,
    // which can be several times the render rate).
    struct CallbackTiming {
        std::atomic<uint64_t> count{0},total{0},maximum{0};
        void add(uint64_t ticks) noexcept {
            ++count;total+=ticks;
            for(auto seen=maximum.load();ticks>seen && !maximum.compare_exchange_weak(seen,ticks););
        }
        json snapshot() const {return {{"samples",count.load()},{"total_ticks",total.load()},{"max_ticks",maximum.load()}};}
    };
    CallbackTiming channel_timing_,frame_start_timing_,frame_end_timing_,other_event_timing_;
    std::atomic<DWORD> main_thread_{0}; // The game thread that runs SDK frames, render and present.
private:
    json engine_snapshot();
    json snapshot() const;
    scs_telemetry_init_params_v100_t api_;
    json schema_,config_=json::object(),values_=json::object(),registration_=json::object();
    std::atomic<std::shared_ptr<const json>> truck_config_;
    std::atomic<uint32_t> wheel_count_{0},trailer_wheel_count_{0};
    std::atomic<std::shared_ptr<const json>> registered_;
    std::string executable_hash_,gate_error_;
    bool gate_ok_=false,allow_tier1_=false,allow_render_probe_=false,allow_camera_rig_=false,allow_parallel_passes_=false,allow_proxy_coexistence_=false,paused_=true;
    std::mutex control_;
    std::string lease_;
    uint64_t lease_deadline_=0;
    std::atomic<int> tier_{0};
    std::atomic<bool> observe_traffic_{false};
    uint64_t frame_=0,generation_=0;
    scs_telemetry_frame_start_t clock_{};
    std::deque<Channel> channels_;
    // Per-wheel channels of the truck and the first trailer, re-registered on each configuration.
    std::map<std::string,std::deque<Channel>> wheel_channels_;
    void register_wheels(const std::string& owner,uint32_t count,const json& fields);
    // Other SDK configurations (trailer.N, job, controls, ...) by id.
    json configs_store_=json::object();
    std::atomic<std::shared_ptr<const json>> configs_;
    std::vector<scs_event_t> events_;
    std::atomic<std::shared_ptr<const json>> latest_;
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<RenderProbe> render_probe_;
    std::unique_ptr<ParallelPasses> parallel_passes_;
    std::vector<std::pair<uintptr_t,size_t>> accepted_text_patches_;
    void read_accepted_patches(const json& settings);
    ThreadSampler sampler_;
};

void Runtime::initialize() {
    const auto directory=module_path(module).parent_path();
    std::ifstream input(directory/L"ot_config.json");
    if(input) {input>>config_;if(!config_.is_object()) throw std::runtime_error("ot_config.json must be an object");}
    configure_drive(config_);
    int initial=config_.value("initial_tier",0),key=config_.value("panic_virtual_key",VK_F11);
    if(initial<0 || initial>1 || key<1 || key>254) throw std::runtime_error("Invalid initial_tier or panic_virtual_key");
    allow_tier1_=config_.value("allow_tier1",false) && config_.value("singleplayer_research",false);
    allow_render_probe_=config_.value("allow_render_probe",false);
    allow_camera_rig_=config_.value("allow_camera_rig",false);
    allow_parallel_passes_=config_.value("allow_parallel_passes",false);
    allow_proxy_coexistence_=config_.value("allow_proxy_sensor_coexistence",false);
    read_accepted_patches(config_);
    render_probe_=std::make_unique<RenderProbe>();
    parallel_passes_=std::make_unique<ParallelPasses>();
    try {executable_hash_=sha256_file(module_path(nullptr)); gate_ok_=executable_hash_==OT_GAME_SHA256;}
    catch(const std::exception& e) {gate_error_=e.what();}
    if(!gate_ok_) log("Version gate closed; SDK-only. Observed hash="+executable_hash_+" "+gate_error_);
    tier_=(gate_ok_ && allow_tier1_)?initial:0;
    const auto& fields=schema_.at("sdk_fields");
    for(const auto& field:fields) {
        channels_.push_back({this,field.at("name"),sdk_value_type(field.at("type"))});
    }
    for(auto& channel:channels_) {
        channel.registration=api_.register_for_channel(channel.name.c_str(),SCS_U32_NIL,channel.type,
            SCS_TELEMETRY_CHANNEL_FLAG_each_frame|SCS_TELEMETRY_CHANNEL_FLAG_no_value,channel_callback,&channel);
        registration_[channel.name]=channel.registration;
    }
    registered_.store(std::make_shared<const json>(registration_));
    for(auto id:{SCS_TELEMETRY_EVENT_frame_start,SCS_TELEMETRY_EVENT_frame_end,SCS_TELEMETRY_EVENT_started,
                SCS_TELEMETRY_EVENT_paused,SCS_TELEMETRY_EVENT_configuration}) {
        if(api_.register_for_event(id,event_callback,this)!=SCS_RESULT_ok) throw std::runtime_error("SDK event registration failed");
        events_.push_back(id);
    }
    transport_=std::make_unique<Transport>([this](const json& r){return command(r);},[this]{panic();},key,L"\\\\.\\pipe\\ot",[this]{poll_lease();});
    transport_->start(config_.value("publish_shared_state",true));
    log("ot_core initialized: SDK callbacks registered, pipe ready, tier="+std::to_string(tier_.load()));
    if(api_.common.log) api_.common.log(SCS_LOG_TYPE_message,"[ot_core] SDK plugin ready; local pipe \\\\.\\pipe\\ot");
}
// SDK event callbacks permit registration; the deque keeps callback contexts stable.
void Runtime::register_wheels(const std::string& owner,uint32_t count,const json& fields) {
    auto& list=wheel_channels_[owner];
    for(auto& c:list) {
        if(c.registration==SCS_RESULT_ok) api_.unregister_from_channel(c.name.c_str(),c.index,c.type);
        registration_.erase(c.key());
    }
    list.clear();
    for(uint32_t i=0;i<count;++i) for(const auto& field:fields) {
        list.push_back({this,field.at("name"),sdk_value_type(field.at("type")),i});
        auto& c=list.back();
        c.registration=api_.register_for_channel(c.name.c_str(),c.index,c.type,
            SCS_TELEMETRY_CHANNEL_FLAG_each_frame|SCS_TELEMETRY_CHANNEL_FLAG_no_value,channel_callback,&c);
        registration_[c.key()]=c.registration;
    }
    registered_.store(std::make_shared<const json>(registration_));
}
bool Runtime::shutdown() noexcept {
    if(input_host) input_host->release(input_host->context,OtDriveReason::unloaded);
    sampler_.stop();
    tier_=0;
    for(auto event:events_) api_.unregister_from_event(event);
    events_.clear();
    for(auto& channel:channels_) {
        if(channel.registration==SCS_RESULT_ok) api_.unregister_from_channel(channel.name.c_str(),channel.index,channel.type);
        channel.registration=SCS_RESULT_not_found;
    }
    for(auto& [owner,list]:wheel_channels_) for(auto& channel:list) {
        if(channel.registration==SCS_RESULT_ok) api_.unregister_from_channel(channel.name.c_str(),channel.index,channel.type);
        channel.registration=SCS_RESULT_not_found;
    }
    if(transport_) transport_->stop();
    if(parallel_passes_) {
        if(parallel_passes_->close()) parallel_passes_.reset();
        else {
            log("ot_core stopped but parallel_passes hooks are retained; DLL release refused");
            return false;
        }
    }
    if(render_probe_) {
        if(render_probe_->close()) render_probe_.reset();
        else {
            // Keep the runtime for a loader stop retry. Direct SDK shutdown
            // retains it until process exit instead of freeing active code.
            log("ot_core stopped but render observer is retained; DLL release refused");
            return false;
        }
    }
    // The stream publisher must finish before its shared mapping is destroyed.
    if(transport_) {transport_.reset();log("ot_core shutdown: workers joined, pipe and mapping released");}
    return true;
}
void Runtime::panic() noexcept {
    release_runtime(OtDriveReason::panic,"Panic: Tier 0; SDK remains active; render hook disable requested");
}
// F11, errors and lease expiry are panics. A controller returning its own lease
// ends the session normally, so the drive release reads as a disconnect.
void Runtime::release_runtime(OtDriveReason reason,const char* message) noexcept {
    try {
        std::lock_guard lock(control_);
        if(input_host) input_host->release(input_host->context,reason);
        lease_.clear();lease_deadline_=0;
        tier_=0;observe_traffic_=false;
        if(parallel_passes_) parallel_passes_->disable();
        if(render_probe_) render_probe_->disable();
        log(message);
    } catch(...) {tier_=0;}
}
void Runtime::poll_lease() {
    bool expired=false;
    {std::lock_guard lock(control_);expired=lease_deadline_ && GetTickCount64()>=lease_deadline_;}
    if(expired) {log("Bridge lease expired; releasing capture and rig");panic();}
}
void Runtime::value(const Channel& channel,const scs_value_t* value) {
    json item={{"available",value!=nullptr},{"observed_frame",frame_},{"value",nullptr}};
    if(value) {
        if(value->type!=channel.type) throw std::runtime_error("SDK channel type mismatch");
        if(value->type==SCS_VALUE_TYPE_float) item["value"]=value->value_float.value;
        else if(value->type==SCS_VALUE_TYPE_s32) item["value"]=value->value_s32.value;
        else if(value->type==SCS_VALUE_TYPE_u32) item["value"]=value->value_u32.value;
        else if(value->type==SCS_VALUE_TYPE_bool) item["value"]=value->value_bool.value!=0;
        else if(value->type==SCS_VALUE_TYPE_fvector) {
            const auto& v=value->value_fvector; item["value"]={v.x,v.y,v.z};
        } else if(value->type==SCS_VALUE_TYPE_dplacement) {
            const auto& p=value->value_dplacement;
            item["value"]={{"position_m",{p.position.x,p.position.y,p.position.z}},
                {"euler_rotations",{p.orientation.heading,p.orientation.pitch,p.orientation.roll}}};
        }
    }
    values_[channel.key()]=std::move(item);
}
void Runtime::event(scs_event_t event,const void* data) {
    if(event==SCS_TELEMETRY_EVENT_started) paused_=false;
    else if(event==SCS_TELEMETRY_EVENT_paused) paused_=true;
    else if(event==SCS_TELEMETRY_EVENT_configuration && data) {
        const auto& config=*static_cast<const scs_telemetry_configuration_t*>(data);
        if(config.id && std::strcmp(config.id,"truck")==0) {
            ++generation_;values_=json::object();
            json attributes=json::array();uint32_t wheel_count=0;
            for(const auto* attribute=config.attributes;attribute->name;++attribute) {
                const std::string_view name=attribute->name;
                // Identity, geometry and the drivetrain/brake constants a vehicle bus or spec sheet would carry.
                if(name!="id" && name!="brand" && name!="name" && name!="cabin.position" &&
                   name!="head.position" && name!="hook.position" && name!="wheels.count" &&
                   !name.starts_with("wheel.") && !name.starts_with("gear") && !name.starts_with("retarder.") &&
                   name!="differential.ratio" && name!="forward.ratio" && name!="reverse.ratio" && name!="rpm.limit" && name!="fuel.capacity" && name!="adblue.capacity" &&
                   name!="brake.air.pressure.warning" && name!="brake.air.pressure.emergency" && name!="shifter.type" &&
                   name!="selector.count" && name!="hshifter.slot.gear") continue;
                json decoded;
                if(!decode_attribute(attribute->value,decoded)) continue;
                if(name=="wheels.count") wheel_count=decoded.get<uint32_t>();
                attributes.push_back({{"name",name},{"index",attribute->index==SCS_U32_NIL?json(nullptr):json(attribute->index)},
                    {"value",std::move(decoded)}});
            }
            register_wheels("truck",wheel_count,schema_.at("sdk_wheel_fields"));
            wheel_count_=wheel_count;
            truck_config_.store(std::make_shared<const json>(json{{"source","SDK truck configuration"},
                {"truck_generation",generation_},{"attributes",std::move(attributes)}}));
        } else if(config.id) {
            const std::string id=config.id;
            json attributes=json::array();uint32_t wheel_count=0;
            for(const auto* attribute=config.attributes;attribute && attribute->name;++attribute) {
                json decoded;
                if(!decode_attribute(attribute->value,decoded)) continue;
                if(std::string_view(attribute->name)=="wheels.count") wheel_count=decoded.get<uint32_t>();
                attributes.push_back({{"name",attribute->name},{"index",attribute->index==SCS_U32_NIL?json(nullptr):json(attribute->index)},
                    {"value",std::move(decoded)}});
            }
            if(id=="trailer.0") {
                register_wheels(id,wheel_count,schema_.value("sdk_trailer_wheel_fields",json::array()));
                trailer_wheel_count_=wheel_count;
            }
            configs_store_[id]={{"truck_generation",generation_},{"attributes",std::move(attributes)}};
            configs_.store(std::make_shared<const json>(configs_store_));
        }
    } else if(event==SCS_TELEMETRY_EVENT_frame_start && data) {
        ++frame_; clock_=*static_cast<const scs_telemetry_frame_start_t*>(data);
        // Explicitly distinguish a missing callback in this frame from a fresh zero.
        for(auto& item:values_.items()) item.value()["available"]=false;
    } else if(event==SCS_TELEMETRY_EVENT_frame_end) {
        json state={{"frame_id",frame_},{"truck_generation",generation_},{"phase","sdk_frame_end"},
            {"render_frame_id",nullptr},{"render_coherent",false},{"paused",paused_},
            {"render_time_us",clock_.render_time},{"simulation_time_us",clock_.simulation_time},
            {"paused_simulation_time_us",clock_.paused_simulation_time},{"timer_flags",clock_.flags},
            {"tier",tier_.load()},{"sdk",values_},{"drive",current_drive()}};
        if(tier_>=1 && !paused_) state["engine"]=engine_snapshot();
        if(tier_==0) state.erase("engine");
        auto stored=std::make_shared<const json>(std::move(state));
        latest_.store(stored);
        if(render_probe_) render_probe_->sdk_frame(stored);
        if(transport_) transport_->publish(stored->dump(),frame_);
    }
    // SDK events run on the game's main thread, which also records render passes.
    if(parallel_passes_) parallel_passes_->sdk_frame(GetCurrentThreadId(),paused_);
}
json Runtime::reload_permissions() {
    // Only the pipe worker calls this after initialization. Engine callbacks
    // consume the atomic tier; permissions never grant a higher tier implicitly.
    panic();allow_tier1_=false;allow_render_probe_=false;allow_camera_rig_=false;allow_parallel_passes_=false;allow_proxy_coexistence_=false;
    configure_drive(json::object());
    std::ifstream input(module_path(module).parent_path()/L"ot_config.json");
    if(!input) throw std::runtime_error("Cannot open ot_config.json; returned to Tier 0");
    json settings;input>>settings;
    if(!settings.is_object()) throw std::runtime_error("ot_config.json must be an object");
    allow_tier1_=settings.value("allow_tier1",false) && settings.value("singleplayer_research",false);
    allow_render_probe_=settings.value("allow_render_probe",false);
    allow_camera_rig_=settings.value("allow_camera_rig",false);
    allow_parallel_passes_=settings.value("allow_parallel_passes",false);
    allow_proxy_coexistence_=settings.value("allow_proxy_sensor_coexistence",false);
    read_accepted_patches(settings);
    configure_drive(settings);
    log("Permissions reloaded; tier=0, allow_tier1="+std::to_string(allow_tier1_));
    return {{"tier",0},{"internal_access_allowed",gate_ok_ && allow_tier1_},
        {"render_probe_allowed",gate_ok_ && allow_tier1_ && allow_render_probe_},
        {"parallel_passes_allowed",gate_ok_ && allow_tier1_ && allow_parallel_passes_}};
}
// accepted_text_patches: [{"rva":"0x1D1890D","size":4}, ...] (docs/28 §5).
void Runtime::read_accepted_patches(const json& settings) {
    accepted_text_patches_.clear();
    for(const auto& item:settings.value("accepted_text_patches",json::array())) {
        const auto& rva=item.at("rva");
        const auto value=rva.is_string()?std::stoull(rva.get<std::string>(),nullptr,0):rva.get<uint64_t>();
        accepted_text_patches_.push_back({static_cast<uintptr_t>(value),item.value("size",size_t(1))});
    }
}
json Runtime::engine_snapshot() {
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t controller{},actor{},interior{};
    if(!read_memory(base+schema_.at("controller_pointer_rva").get<uintptr_t>(),controller) || !controller ||
       !read_memory(controller+schema_.at("player_offset").get<uintptr_t>(),actor) || !actor)
        return {{"available",false},{"reason","player_unavailable"}};
    json engine={{"available",true},{"phase","sdk_frame_end_before_render_preparation"},
        {"vehicle",read_vehicle_physics(actor,schema_)},{"drivetrain",read_vehicle_drivetrain(actor,schema_,wheel_count_,trailer_wheel_count_)},
        {"mirrors",json::array()}};
    if(observe_traffic_) engine["traffic"]=read_world_traffic(schema_);
    ArrayHeader array{};
    if(!read_memory(actor+schema_.at("interior_offset").get<uintptr_t>(),interior) || !interior ||
       !read_memory(interior+schema_.at("mirror_array_offset").get<uintptr_t>(),array) ||
       array.size>array.capacity || array.size>schema_.at("mirror_limit").get<uint64_t>() || (array.size && !array.data))
        {engine["mirror_error"]="mirror_array_unavailable";return engine;}
    json mirrors=json::array();
    for(uint64_t i=0;i<array.size;++i) {
        uintptr_t camera{}; json result={{"index",i},{"available",false}};
        if(read_memory(array.data+i*sizeof(uintptr_t),camera) && camera) {
            Placement p{}; std::array<float,16> projection{};
            auto po=schema_["mirror_fields"]["pose"]["offset"].get<uintptr_t>();
            auto mo=schema_["mirror_fields"]["projection"]["offset"].get<uintptr_t>();
            if(read_memory(camera+po,p) && read_memory(camera+mo,projection)) {
                result["available"]=true;
                result["pose"]={{"position_m",world_position(p)},
                    {"quaternion_wxyz",{p.w,p.qx,p.qy,p.qz}},{"coordinate_space","unknown"}};
                result["projection"]=projection;
            }
        }
        mirrors.push_back(std::move(result));
    }
    engine["mirrors"]=std::move(mirrors);
    return engine;
}
json Runtime::snapshot() const {
    auto state=latest_.load();
    if(!state) return {{"available",false},{"reason","no_sdk_frame_yet"}};
    json copy=*state;
    if(tier_==0) copy.erase("engine");
    return copy;
}
json Runtime::command(const json& request) {
    const auto cmd=request.at("cmd").get<std::string>();
    // Call under control_ at each mutating boundary. A queued command from a
    // canceled lease cannot race F11 and turn capture back on, and no capture
    // state outlives a controller that stopped sending heartbeats.
    const auto require_owner=[&] {
        if(lease_.empty())
            throw std::runtime_error("This command requires an active bridge lease; use the launcher, ot.cmd preview or ot.cmd camera_rig apply");
        if(request.value("owner",std::string{})!=lease_)
            throw std::runtime_error("Lease ended or belongs to another controller; explicit restart required");
    };
    if(cmd=="lease") {
        const auto action=request.at("action").get<std::string>();
        const auto owner=request.at("owner").get<std::string>();
        if(owner.empty() || owner.size()>128) throw std::runtime_error("Invalid lease owner");
        {
            std::lock_guard lock(control_);
            if(action=="claim") {
                if(!lease_.empty() || tier_!=0) throw std::runtime_error("Capture must be idle before claiming a bridge lease");
                lease_=owner;
            } else if(owner!=lease_) throw std::runtime_error("Lease ended; explicit restart required");
            if(action!="claim" && action!="heartbeat" && action!="release")
                throw std::runtime_error("Unknown lease action");
            lease_deadline_=GetTickCount64()+5000;
        }
        if(action=="release") release_runtime(OtDriveReason::disconnected,"Lease released: Tier 0; SDK remains active");
        return {{"active",action!="release"},{"expires_in_ms",5000}};
    }
    if(cmd=="ping") return {{"plugin","ot_core"},{"pid",GetCurrentProcessId()}};
    if(cmd=="traffic_observation") {
        std::lock_guard lock(control_);require_owner();
        const bool enabled=request.at("enabled").get<bool>();
        if(enabled && tier_<1) throw std::runtime_error("Traffic observation requires a Tier 1 bridge lease");
        observe_traffic_=enabled;return {{"enabled",enabled}};
    }
    if(cmd=="drive") {
        const auto action=request.value("action",std::string("status"));
        if(action=="status") return current_drive();
        std::lock_guard lock(control_);require_owner();
        if(!input_host) throw std::runtime_error("Restart the game with the input-capable resident loader");
        if(action=="disconnect") {input_host->release(input_host->context,OtDriveReason::disconnected);return current_drive();}
        const auto driver=request.at("drive_owner").get<std::string>();
        if(driver.empty() || driver.size()>128 || driver.find('\0')!=std::string::npos) throw std::runtime_error("Invalid driving owner");
        OtDriveRequest decoded;decoded.owner=driver.c_str();
        // One turn signal lever press by the armed owner (any axes).
        if(action=="signal") {
            if(!turn_signals()) throw std::runtime_error("Turn signal presses require the updated resident loader; exit and restart the game");
            const auto side=request.at("side").get<int32_t>();
            if(side!=-1 && side!=1) throw std::runtime_error("Turn signal side must be -1 (left) or 1 (right)");
            decoded.epoch=request.at("epoch").get<uint64_t>();
            OtDriveStatus status;
            const bool accepted=input_host->signal(input_host->context,&decoded,side,&status);
            auto result=drive_status(status);result["accepted"]=accepted;result["turn_signals"]=true;return result;
        }
        if(action=="arm") decoded.action=OtDriveAction::arm;
        else if(action=="disarm") decoded.action=OtDriveAction::disarm;
        else if(action=="command") decoded.action=OtDriveAction::command;
        else throw std::runtime_error("Unknown driving action");
        if(action!="arm") decoded.epoch=request.at("epoch").get<uint64_t>();
        if(action=="command") {
            decoded.sequence=request.at("sequence").get<uint64_t>();decoded.deadline_ms=request.at("deadline_ms").get<uint64_t>();
            decoded.steering=request.at("steering").get<float>();decoded.throttle=request.at("throttle").get<float>();decoded.brake=request.at("brake").get<float>();
            if(!std::isfinite(decoded.steering)||!std::isfinite(decoded.throttle)||!std::isfinite(decoded.brake)||
               std::abs(decoded.steering)>1 || decoded.throttle<0 || decoded.throttle>1 || decoded.brake<0 || decoded.brake>1)
                throw std::runtime_error("Driving axes must be finite steering [-1,1], pedals [0,1]");
        }
        const auto axes=request.value("axes",ot_drive_all);
        if(!axes || axes>ot_drive_all_aux || ((axes&ot_drive_auxiliary) && !(axes&ot_drive_pedals)))
            throw std::runtime_error("Driving axes must be steering=1, pedals=2, both=3, plus auxiliary brakes=4 with the pedals");
        if(!split_axes() && axes!=ot_drive_all) throw std::runtime_error("Independent ACC/LCC requires the updated resident loader; exit and restart the game");
        if((axes&ot_drive_auxiliary) && !aux_brakes()) throw std::runtime_error("Retarder and engine brake commands require the updated resident loader; exit and restart the game");
        OtDriveAux aux{-1,0},actual_aux{-1,0};
        if(action=="command" && (axes&ot_drive_auxiliary)) {
            aux.retarder=request.at("retarder").get<int32_t>();aux.engine_brake=request.at("engine_brake").get<bool>();
            if(aux.retarder<0 || aux.retarder>ot_drive_retarder_steps) throw std::runtime_error("Retarder step must be 0..5");
        }
        OtDriveStatus status;uint32_t actual=0;
        const bool accepted=aux_brakes()?input_host->request_aux(input_host->context,&decoded,axes,&aux,&status,&actual,&actual_aux):
            split_axes()?input_host->request_axes(input_host->context,&decoded,axes,&status,&actual):input_host->request(input_host->context,&decoded,&status);
        auto result=drive_status(status);result["axes"]=split_axes()?actual:(status.armed?ot_drive_all:0);result["independent_axes"]=split_axes();
        result["auxiliary_brakes"]=aux_brakes();result["retarder"]=actual_aux.retarder;result["engine_brake"]=actual_aux.engine_brake!=0;result["accepted"]=accepted;return result;
    }
    if(cmd=="version") return {{"plugin_version",OT_VERSION},{"schema_game_version",schema_.at("game_version")},
        {"sdk_game_version",api_.common.game_version},{"expected_exe_sha256",OT_GAME_SHA256},
        {"observed_exe_sha256",executable_hash_},{"internal_access_allowed",gate_ok_ && allow_tier1_},
        {"gate_error",gate_error_},{"tier",tier_.load()},{"capabilities",{"sdk","truck_config","configs","pipe","state_ring","mirror_read","vehicle_physics_read","drivetrain_read","render_probe","frames","stream","manual_dump","panic","drive","drive_auxiliary_brakes","drive_turn_signals","parallel_passes","overlay","proxy_sensor_coexistence"}},
        {"render_probe_allowed",gate_ok_ && allow_tier1_ && allow_render_probe_},
        {"parallel_passes_allowed",gate_ok_ && allow_tier1_ && allow_parallel_passes_},
        {"overlay",true},{"overlay_allowed",gate_ok_ && allow_tier1_ && allow_render_probe_},{"gpu_capture",true},{"writes",render_probe_->status().at("active").get<int>()!=0 || current_drive().value("active",false) || parallel_passes_->active()},
        {"drive",current_drive()},
        {"field_writes",false},{"camera_rig",render_probe_->camera_rig(json::object())},{"channels",*registered_.load()}};
    if(cmd=="schema") return schema_;
    if(cmd=="configs") {
        auto configs=configs_.load();
        return configs?*configs:json::object();
    }
    if(cmd=="truck_config") {
        auto config=truck_config_.load();
        if(!config) throw std::runtime_error("No SDK truck configuration has been received");
        return *config;
    }
    if(cmd=="resolve_layout") {
        const auto truck=truck_config_.load();
        if(!truck) throw std::runtime_error("No SDK truck configuration has been received");
        auto rig=request.at("rig");json selected=json::array();
        for(const auto& view:rig.at("views")) selected.push_back(view.at("slot"));
        return resolve_mount_layout(std::move(rig),*truck,selected).rig;
    }
    if(cmd=="reload_permissions") return reload_permissions();
    if(cmd=="hooks") return render_probe_->status();
    if(cmd=="traffic") {
        // Read-only count for performance runs (scene load changes with traffic).
        // Guarded reads from this pipe thread; needs the pinned executable.
        if(!gate_ok_ || !allow_tier1_) throw std::runtime_error("Traffic reads require the version gate and allow_tier1");
        return read_world_traffic(schema_);
    }
    if(cmd=="main_profile") {
        // Read-only observation, so the bridge's lease owner is not required;
        // runs are bounded and the target is suspended only for each sample.
        const auto action=request.value("action",std::string("status"));
        if(action=="start") {
            // Any thread of the game can be sampled; the default is the main thread.
            auto thread=request.value("thread_id",DWORD{0});
            if(!thread) thread=main_thread_.load();
            if(!thread) throw std::runtime_error("No SDK frame has run yet");
            {
                Handle handle(OpenThread(THREAD_QUERY_LIMITED_INFORMATION,FALSE,thread));
                if(!handle || GetProcessIdOfThread(handle.h)!=GetCurrentProcessId())
                    throw std::runtime_error("thread_id is not a thread of the game process");
            }
            const double hz=request.value("hz",250.0),seconds=request.value("seconds",20.0);
            const auto depth=request.value("depth",0);
            if(!std::isfinite(hz) || hz<10 || hz>1000 || !std::isfinite(seconds) || seconds<=0 || seconds>120 || depth<0 || depth>64)
                throw std::runtime_error("Profile hz must be 10-1000, seconds 0-120 and depth 0-64");
            sampler_.start(thread,hz,seconds,static_cast<unsigned>(depth));
        } else if(action=="stacks") return sampler_.stacks(request.value("offset",size_t(0)));
        else if(action=="stop") sampler_.stop();
        else if(action!="status") throw std::runtime_error("Unknown main_profile action");
        return sampler_.status();
    }
    if(cmd=="sdk_timing") return {{"qpc_frequency",qpc_frequency()},{"channel",channel_timing_.snapshot()},
        {"frame_start",frame_start_timing_.snapshot()},{"frame_end",frame_end_timing_.snapshot()},{"other_events",other_event_timing_.snapshot()}};
    if(cmd=="frames") return render_probe_->frames(request.value("after_id",uint64_t(0)));
    if(cmd=="camera_rig") {
        std::lock_guard lock(control_);
        if(request.contains("views") || request.contains("enabled")) {
            require_owner();
            const bool enabled=request.value("enabled",true);
            if(enabled && (tier_<1 || !gate_ok_ || !allow_tier1_ || !allow_render_probe_ || !allow_camera_rig_))
                throw std::runtime_error("Camera rig requires Tier 1 probe and allow_camera_rig permission");
            auto result=render_probe_->camera_rig(request);
            tier_=enabled?2:(tier_.load()?1:0);
            return result;
        }
        return render_probe_->camera_rig(request);
    }
    if(cmd=="stream") {
        std::lock_guard lock(control_);
        const auto action=request.value("action",std::string("status"));
        if(action!="status") require_owner();
        if((action=="arm" || action=="start") && (tier_<1 || !gate_ok_ || !allow_tier1_ || !allow_render_probe_))
            throw std::runtime_error("Capture requires the permitted Tier 1 render probe");
        CaptureOptions options;
        if(action=="arm" || action=="start" || action=="update") {
            options.format=request.value("format",std::string("rgbd8"));
            options.color_gain=request.value("color_gain",1.0f);
            options.shared_gpu=request.value("shared_gpu",false);
            options.auto_exposure=request.value("auto_exposure",false);
            options.lidar_hz=request.value("lidar_hz",0.0);
            options.preview_hz=request.value("preview_hz",0.0);
            if(!std::isfinite(options.preview_hz) || options.preview_hz<0)
                throw std::runtime_error("preview_hz must be finite and nonnegative");
            if(!std::isfinite(options.lidar_hz) || options.lidar_hz<0)
                throw std::runtime_error("lidar_hz must be finite and nonnegative");
            if(options.shared_gpu && options.format!="ros")
                throw std::runtime_error("Shared GPU output requires a ROS stream relay");
            if(request.contains("lidars")) for(const auto& item:request.at("lidars").items()) {
                const auto& name=item.key();
                if(name.size()!=7 || !name.starts_with("mirror") || name.back()<'0' || name.back()>'8')
                    throw std::runtime_error("Invalid LiDAR source camera");
                options.lidar_patterns[name.back()-'0']=make_lidar_pattern(item.value());
            }
            if(request.contains("outputs")) {
                if(!options.metric()) throw std::runtime_error("Output selection requires ROS capture format");
                options.selective=true;options.outputs.fill(0);
                for(const auto& entry:request.at("outputs").items()) {
                    const auto& name=entry.key();
                    if(name.size()!=7 || !name.starts_with("mirror") || name.back()<'0' || name.back()>'8')
                        throw std::runtime_error("Invalid output camera");
                    for(const auto& output:entry.value()) {
                        uint8_t flag=output=="color"?1:output=="depth"?2:output=="preview"?4:output=="lidar"?8:output=="metadata"?16:output=="pose"?32:output=="display"?64:0;
                        if(!flag) throw std::runtime_error("Unknown sensor output");
                        options.outputs[name.back()-'0']|=flag;
                    }
                }
            }
            for(size_t i=0;i<options.outputs.size();++i) if((options.outputs[i]&8) && !options.lidar_patterns[i])
                throw std::runtime_error("LiDAR demand requires a beam pattern");
            if(options.format!="raw" && options.format!="rgbd8" && options.format!="raw+rgbd8" && options.format!="ros" && options.format!="raw+ros")
                throw std::runtime_error("Capture format must be raw, rgbd8, raw+rgbd8 ros or raw+ros");
            if(!std::isfinite(options.color_gain) || options.color_gain<=0)
                throw std::runtime_error("Color gain must be finite and positive");
        }
        return render_probe_->stream(request,*transport_,options);
    }
    if(cmd=="render_probe") {
        std::lock_guard lock(control_);
        if(request.contains("enabled")) {
            require_owner();
            if(request.at("enabled").get<bool>()) {
                if(tier_<1 || !gate_ok_ || !allow_tier1_ || !allow_render_probe_)
                    throw std::runtime_error("Render probe requires Tier 1, matching EXE, singleplayer_research, allow_tier1 and allow_render_probe");
                try {
                    const auto mode=request.value("mode",std::string("observe"));
                    const bool overlay=mode=="overlay" || request.value("overlay",false);
                    if(overlay) render_probe_->overlay_mapping(transport_->overlay_block());
                    render_probe_->enable(request.value("vehicle_metadata",false),
                        mode,request.value("frame_timing",false),request.value("draw_metadata",true),overlay);
                } catch(...) {
                    if(tier_==2 && !render_probe_->camera_rig(json::object()).at("enabled").get<bool>()) tier_=1;
                    throw;
                }
            } else {render_probe_->disable();if(tier_==2) tier_=1;}
        }
        return render_probe_->status();
    }
    if(cmd=="proxy_sensor_coexistence") {
        // Hypothesis from static analysis, off by default (research/snowymoon/sensor_coexistence.md).
        // Needs render_probe observe or overlay mode; the pre-checks refuse and say why in the status.
        std::lock_guard lock(control_);
        if(request.contains("enabled")) require_owner();
        return render_probe_->proxy_coexistence(request,tier_>=1 && gate_ok_ && allow_tier1_ && allow_render_probe_ && allow_proxy_coexistence_);
    }
    if(cmd=="parallel_passes") {
        // docs/28 M3a. Independent of the bridge lease: it changes when passes
        // are recorded, not what is captured. F11, lease end and permission
        // reload return it to the original path (release_runtime).
        std::lock_guard lock(control_);
        if(request.contains("mode")) {
            const auto mode=request.at("mode").get<std::string>();
            if(mode=="off") parallel_passes_->disable();
            else if(mode=="serial" || mode=="parallel" || mode=="alternate") {
                if(!gate_ok_ || !allow_tier1_ || !allow_parallel_passes_)
                    throw std::runtime_error("parallel_passes requires matching EXE, singleplayer_research, allow_tier1 and allow_parallel_passes");
                ParallelPasses::GateInputs gate{render_probe_->patched_ranges(),accepted_text_patches_,{}};
                for(const auto& rva:schema_.at("render_parallel").at("closure_rvas")) gate.closure.push_back(rva.get<uintptr_t>());
                ParallelPasses::Options options;
                if(mode!="serial") {
                    // docs/28 §10: the order is always explicit for worker recording.
                    const auto order=request.at("order").get<std::string>();
                    if(order!="ticket" && order!="free") throw std::runtime_error("order must be ticket or free");
                    options.ticket=order=="ticket";
                    options.executors=request.value("executors",0u);
                    options.accept_foreign=request.value("accept_foreign_hooks",false);
                }
                parallel_passes_->enable(mode=="serial"?ParallelPasses::Mode::serial:mode=="parallel"?ParallelPasses::Mode::parallel:
                    ParallelPasses::Mode::alternate,gate,options);
            } else if(mode=="shadow") throw std::runtime_error("Recording a pass twice crashes: pass callbacks consume their blit pairs (0x151DDB0, docs/28)");
            else throw std::runtime_error("parallel_passes mode must be off, serial, parallel or alternate");
        }
        return parallel_passes_->status();
    }
    if(cmd=="overlay") {
        // HUD trajectory decal (docs/24). The path arrives in Local\OT_Overlay;
        // this command only switches it. status and capture are read-only.
        const auto action=request.value("action",std::string("status"));
        if(action=="status") return render_probe_->overlay_status();
        if(action=="capture") return render_probe_->overlay_capture(request);
        std::lock_guard lock(control_);require_owner();
        if(action=="enable") {
            if(tier_<1 || !gate_ok_ || !allow_tier1_ || !allow_render_probe_)
                throw std::runtime_error("The HUD overlay requires Tier 1, matching EXE, singleplayer_research, allow_tier1 and allow_render_probe");
            OverlayOptions options;
            options.z_tolerance=request.value("z_tolerance_m",options.z_tolerance);
            options.slope=std::tan(request.value("slope_deg",8.0)*3.14159265358979323846/180);
            options.normal_min=request.value("normal_min",options.normal_min);
            if(request.contains("fade_m")) {const auto fade=request.at("fade_m").get<std::array<double,2>>();options.fade_start=fade[0];options.fade_end=fade[1];}
            const auto detector=request.value("detector",std::string("compiled_span"));
            if(detector!="compiled_span" && detector!="backbuffer_usage") throw std::runtime_error("detector must be compiled_span or backbuffer_usage");
            options.backbuffer_usage=detector=="backbuffer_usage";
            return render_probe_->overlay(true,transport_->overlay_block(),options);
        }
        if(action=="disable") return render_probe_->overlay(false,nullptr,{});
        throw std::runtime_error("overlay action must be enable, disable, status or capture");
    }
    if(cmd=="panic") {panic();return {{"tier",0},{"active_hooks",render_probe_->status().at("active")}};}
    if(cmd=="tier") {
        std::lock_guard lock(control_);
        if(request.contains("value")) {
            require_owner();
            int tier=request.at("value").get<int>();
            if(tier<0 || tier>1) throw std::runtime_error("Only Tier 0 and Tier 1 observation are implemented");
            if(tier && (!gate_ok_ || !allow_tier1_)) throw std::runtime_error("Tier 1 requires matching EXE, allow_tier1 and singleplayer_research in ot_config.json");
            if(!tier) render_probe_->disable();
            else if(tier_==2) render_probe_->camera_rig({{"enabled",false}});
            tier_=tier;
        }
        return {{"tier",tier_.load()}};
    }
    if(cmd=="snapshot") return snapshot();
    if(cmd=="state") return transport_->status();
    if(cmd=="bundles") return transport_->bundle_status();
    if(cmd=="dump") return dump_process();
    if(cmd=="read") {
        std::string field=request.at("field"); auto state=snapshot();
        if(state.contains("sdk") && state["sdk"].contains(field)) return state["sdk"][field];
        if(field.starts_with("vehicle.")) {
            auto member=field.substr(8);
            if(!schema_["vehicle_fields"].contains(member)) throw std::runtime_error("Unknown vehicle field");
            if(tier_<1 || !state.contains("engine") || !state["engine"].contains("vehicle")) throw std::runtime_error("No Tier 1 vehicle sample available");
            const auto& vehicle=state["engine"]["vehicle"];
            if(!vehicle.value("available",false)) return vehicle;
            return {{"value",vehicle.at(member)},{"frame_id",state["frame_id"]},{"phase",vehicle["phase"]}};
        }
        const std::string prefix="mirror_camera[";
        if(field.starts_with(prefix)) {
            const auto close=field.find(']',prefix.size());
            if(close==std::string::npos || close+2>field.size() || field[close+1]!='.') throw std::runtime_error("Invalid mirror field");
            const auto digits=field.substr(prefix.size(),close-prefix.size());
            if(digits.empty() || digits.find_first_not_of("0123456789")!=std::string::npos) throw std::runtime_error("Invalid mirror index");
            const auto index=std::stoul(digits); const auto member=field.substr(close+2);
            if(!schema_["mirror_fields"].contains(member)) throw std::runtime_error("Unknown mirror field");
            if(tier_<1 || !state.contains("engine") || !state["engine"].value("available",false)) throw std::runtime_error("No Tier 1 engine sample available");
            const auto& mirrors=state["engine"]["mirrors"];
            if(index>=mirrors.size() || !mirrors[index].value("available",false)) throw std::runtime_error("Mirror not present");
            return {{"value",mirrors[index].at(member)},{"frame_id",state["frame_id"]},{"phase",state["engine"]["phase"]}};
        }
        const auto registered=registered_.load();
        if(registered->contains(field)) return {{"available",false},{"registration_result",registered->at(field)}};
        throw std::runtime_error("Field is not in the compiled read-only schema");
    }
    throw std::runtime_error("Command not implemented: "+cmd);
}
static std::unique_ptr<Runtime> runtime;
}

SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version,const scs_telemetry_init_params_t* const params) {
    if(version!=SCS_TELEMETRY_VERSION_1_00 && version!=SCS_TELEMETRY_VERSION_1_01) return SCS_RESULT_unsupported;
    if(!params) return SCS_RESULT_invalid_parameter;
    if(ot::runtime) return SCS_RESULT_already_registered;
    try {
        const auto& api=*static_cast<const scs_telemetry_init_params_v100_t*>(params);
        if(!api.common.game_id || std::strcmp(api.common.game_id,"eut2")!=0) return SCS_RESULT_unsupported;
        auto created=std::make_unique<ot::Runtime>(api); created->initialize(); ot::runtime=std::move(created);
        return SCS_RESULT_ok;
    } catch(const std::exception& e) {ot::log(std::string("Initialization failed: ")+e.what());return SCS_RESULT_generic_error;}
    catch(...) {ot::log("Initialization failed");return SCS_RESULT_generic_error;}
}
static int SCSAPIFUNC module_stop() {
    if(ot::runtime && !ot::runtime->shutdown()) return 0;
    ot::runtime.reset();return 1;
}
SCSAPI_VOID scs_telemetry_shutdown() {
    if(!module_stop()) ot::runtime.release();
}
extern "C" __declspec(dllexport) const OtModuleApi* SCSAPIFUNC ot_get_module_api(uint32_t abi) {
    static const OtModuleApi api{1,sizeof(OtModuleApi),OT_VERSION,scs_telemetry_init,module_stop,
        [](const OtDriveHost* host){ot::input_host=host && host->abi==1 && host->size>=offsetof(OtDriveHost,request_axes) && host->configure && host->request && host->release?host:nullptr;}};
    return abi==api.abi?&api:nullptr;
}
