#include "ot.hpp"
#include "build_identity.hpp"
#include "render_probe.hpp"
#include "module_api.hpp"
#include <scssdk_telemetry.h>
#include <array>
#include <fstream>
#include <stdexcept>
#include <cstring>

namespace ot {
class Runtime;
struct Channel { Runtime* owner; std::string name; scs_value_type_t type; scs_result_t registration=SCS_RESULT_not_found; };
struct Placement { float x,y,z; int16_t cx,cz; float w,qx,qy,qz; };
static_assert(sizeof(Placement)==32);
struct ArrayHeader { uintptr_t vtable,data; uint64_t size,capacity; };

class Runtime {
public:
    explicit Runtime(const scs_telemetry_init_params_v100_t& api):api_(api),schema_(json::parse(OT_SCHEMA)) {}
    ~Runtime() { shutdown(); }
    void initialize();
    bool shutdown() noexcept;
    void panic() noexcept;
    void event(scs_event_t event,const void* data);
    void value(const Channel& channel,const scs_value_t* value);
    json command(const json& request);
    json reload_permissions();
    void poll_lease();
    static void SCSAPIFUNC event_callback(scs_event_t event,const void* data,scs_context_t context) noexcept {
        auto& self=*static_cast<Runtime*>(context);
        try {self.event(event,data);} catch(const std::exception& e) {self.panic();log(std::string("SDK event error: ")+e.what());}
        catch(...) {self.panic();}
    }
    static void SCSAPIFUNC channel_callback(scs_string_t,scs_u32_t,const scs_value_t* value,scs_context_t context) noexcept {
        auto& channel=*static_cast<Channel*>(context);
        try {channel.owner->value(channel,value);} catch(...) {channel.owner->panic();}
    }
private:
    json engine_snapshot();
    json snapshot() const;
    scs_telemetry_init_params_v100_t api_;
    json schema_,config_=json::object(),values_=json::object(),registration_=json::object();
    std::atomic<std::shared_ptr<const json>> truck_config_;
    std::string executable_hash_,gate_error_;
    bool gate_ok_=false,allow_tier1_=false,allow_render_probe_=false,allow_camera_rig_=false,paused_=true;
    std::mutex control_;
    std::string lease_;
    uint64_t lease_deadline_=0;
    std::atomic<int> tier_{0};
    uint64_t frame_=0,generation_=0;
    scs_telemetry_frame_start_t clock_{};
    std::vector<Channel> channels_;
    std::vector<scs_event_t> events_;
    std::atomic<std::shared_ptr<const json>> latest_;
    std::unique_ptr<Transport> transport_;
    std::unique_ptr<RenderProbe> render_probe_;
};

void Runtime::initialize() {
    const auto directory=module_path(module).parent_path();
    std::ifstream input(directory/L"ot_config.json");
    if(input) {input>>config_;if(!config_.is_object()) throw std::runtime_error("ot_config.json must be an object");}
    int initial=config_.value("initial_tier",0),key=config_.value("panic_virtual_key",VK_F11);
    if(initial<0 || initial>1 || key<1 || key>254) throw std::runtime_error("Invalid initial_tier or panic_virtual_key");
    allow_tier1_=config_.value("allow_tier1",false) && config_.value("singleplayer_research",false);
    allow_render_probe_=config_.value("allow_render_probe",false);
    allow_camera_rig_=config_.value("allow_camera_rig",false);
    render_probe_=std::make_unique<RenderProbe>();
    try {executable_hash_=sha256_file(module_path(nullptr)); gate_ok_=executable_hash_==OT_GAME_SHA256;}
    catch(const std::exception& e) {gate_error_=e.what();}
    if(!gate_ok_) log("Version gate closed; SDK-only. Observed hash="+executable_hash_+" "+gate_error_);
    tier_=(gate_ok_ && allow_tier1_)?initial:0;
    const auto& fields=schema_.at("sdk_fields");
    channels_.reserve(fields.size());
    for(const auto& field:fields) {
        std::string type=field.at("type"),name=field.at("name");
        auto kind=type=="dplacement"?SCS_VALUE_TYPE_dplacement:type=="fvector"?SCS_VALUE_TYPE_fvector:SCS_VALUE_TYPE_float;
        channels_.push_back({this,name,kind});
    }
    for(auto& channel:channels_) {
        channel.registration=api_.register_for_channel(channel.name.c_str(),SCS_U32_NIL,channel.type,
            SCS_TELEMETRY_CHANNEL_FLAG_each_frame|SCS_TELEMETRY_CHANNEL_FLAG_no_value,channel_callback,&channel);
        registration_[channel.name]=channel.registration;
    }
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
bool Runtime::shutdown() noexcept {
    tier_=0;
    for(auto event:events_) api_.unregister_from_event(event);
    events_.clear();
    for(auto& channel:channels_) {
        if(channel.registration==SCS_RESULT_ok) api_.unregister_from_channel(channel.name.c_str(),SCS_U32_NIL,channel.type);
        channel.registration=SCS_RESULT_not_found;
    }
    if(transport_) transport_->stop();
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
    try {
        std::lock_guard lock(control_);
        lease_.clear();lease_deadline_=0;
        tier_=0;
        if(render_probe_) render_probe_->disable();
        log("Panic: Tier 0; SDK remains active; render hook disable requested");
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
        else if(value->type==SCS_VALUE_TYPE_fvector) {
            const auto& v=value->value_fvector; item["value"]={v.x,v.y,v.z};
        } else if(value->type==SCS_VALUE_TYPE_dplacement) {
            const auto& p=value->value_dplacement;
            item["value"]={{"position_m",{p.position.x,p.position.y,p.position.z}},
                {"euler_rotations",{p.orientation.heading,p.orientation.pitch,p.orientation.roll}}};
        }
    }
    values_[channel.name]=std::move(item);
}
void Runtime::event(scs_event_t event,const void* data) {
    if(event==SCS_TELEMETRY_EVENT_started) paused_=false;
    else if(event==SCS_TELEMETRY_EVENT_paused) paused_=true;
    else if(event==SCS_TELEMETRY_EVENT_configuration && data) {
        const auto& config=*static_cast<const scs_telemetry_configuration_t*>(data);
        if(config.id && std::strcmp(config.id,"truck")==0) {
            ++generation_;values_=json::object();
            json attributes=json::array();
            for(const auto* attribute=config.attributes;attribute->name;++attribute) {
                const std::string_view name=attribute->name;
                if(name!="id" && name!="brand" && name!="name" && name!="cabin.position" &&
                   name!="head.position" && name!="hook.position" && name!="wheels.count" &&
                   !name.starts_with("wheel.")) continue;
                const auto& value=attribute->value;json decoded;
                switch(value.type) {
                    case SCS_VALUE_TYPE_bool:decoded=value.value_bool.value!=0;break;
                    case SCS_VALUE_TYPE_u32:decoded=value.value_u32.value;break;
                    case SCS_VALUE_TYPE_float:decoded=value.value_float.value;break;
                    case SCS_VALUE_TYPE_string:decoded=value.value_string.value;break;
                    case SCS_VALUE_TYPE_fvector: {
                        const auto& v=value.value_fvector;decoded={v.x,v.y,v.z};break;
                    }
                    default:continue;
                }
                attributes.push_back({{"name",name},{"index",attribute->index==SCS_U32_NIL?json(nullptr):json(attribute->index)},
                    {"value",std::move(decoded)}});
            }
            truck_config_.store(std::make_shared<const json>(json{{"source","SDK truck configuration"},
                {"truck_generation",generation_},{"attributes",std::move(attributes)}}));
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
            {"tier",tier_.load()},{"sdk",values_}};
        if(tier_>=1 && !paused_) state["engine"]=engine_snapshot();
        if(tier_==0) state.erase("engine");
        auto stored=std::make_shared<const json>(std::move(state));
        latest_.store(stored);
        if(render_probe_) render_probe_->sdk_frame(stored);
        if(transport_) transport_->publish(stored->dump(),frame_);
    }
}
json Runtime::reload_permissions() {
    // Only the pipe worker calls this after initialization. Engine callbacks
    // consume the atomic tier; permissions never grant a higher tier implicitly.
    panic();allow_tier1_=false;allow_render_probe_=false;allow_camera_rig_=false;
    std::ifstream input(module_path(module).parent_path()/L"ot_config.json");
    if(!input) throw std::runtime_error("Cannot open ot_config.json; returned to Tier 0");
    json settings;input>>settings;
    if(!settings.is_object()) throw std::runtime_error("ot_config.json must be an object");
    allow_tier1_=settings.value("allow_tier1",false) && settings.value("singleplayer_research",false);
    allow_render_probe_=settings.value("allow_render_probe",false);
    allow_camera_rig_=settings.value("allow_camera_rig",false);
    log("Permissions reloaded; tier=0, allow_tier1="+std::to_string(allow_tier1_));
    return {{"tier",0},{"internal_access_allowed",gate_ok_ && allow_tier1_},
        {"render_probe_allowed",gate_ok_ && allow_tier1_ && allow_render_probe_}};
}
json Runtime::engine_snapshot() {
    const auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t controller{},actor{},interior{};
    if(!read_memory(base+schema_.at("controller_pointer_rva").get<uintptr_t>(),controller) || !controller ||
       !read_memory(controller+schema_.at("player_offset").get<uintptr_t>(),actor) || !actor)
        return {{"available",false},{"reason","player_unavailable"}};
    json engine={{"available",true},{"phase","sdk_frame_end_before_render_preparation"},
        {"vehicle",read_vehicle_physics(actor,schema_)},{"mirrors",json::array()}};
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
                result["pose"]={{"position_m",{static_cast<double>(p.cx)*512+p.x,static_cast<double>(p.y),static_cast<double>(p.cz)*512+p.z}},
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
    // canceled lease cannot race F11 and turn capture back on.
    const auto require_owner=[&] {
        if((!lease_.empty() || request.contains("owner")) && request.value("owner",std::string{})!=lease_)
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
        if(action=="release") panic();
        return {{"active",action!="release"},{"expires_in_ms",5000}};
    }
    if(cmd=="ping") return {{"plugin","ot_core"},{"pid",GetCurrentProcessId()}};
    if(cmd=="version") return {{"plugin_version",OT_VERSION},{"schema_game_version",schema_.at("game_version")},
        {"sdk_game_version",api_.common.game_version},{"expected_exe_sha256",OT_GAME_SHA256},
        {"observed_exe_sha256",executable_hash_},{"internal_access_allowed",gate_ok_ && allow_tier1_},
        {"gate_error",gate_error_},{"tier",tier_.load()},{"capabilities",{"sdk","truck_config","pipe","state_ring","mirror_read","vehicle_physics_read","render_probe","frames","capture_mirror5","capture_mirrors","stream","manual_dump","panic"}},
        {"render_probe_allowed",gate_ok_ && allow_tier1_ && allow_render_probe_},
        {"overlay",false},{"gpu_capture",true},{"writes",render_probe_->status().at("active").get<int>()!=0},
        {"field_writes",false},{"camera_rig",render_probe_->camera_rig(json::object())},{"channels",registration_}};
    if(cmd=="schema") return schema_;
    if(cmd=="truck_config") {
        auto config=truck_config_.load();
        if(!config) throw std::runtime_error("No SDK truck configuration has been received");
        return *config;
    }
    if(cmd=="reload_permissions") return reload_permissions();
    if(cmd=="hooks") return render_probe_->status();
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
    if(cmd=="capture_mirror5" || cmd=="capture_mirrors" || cmd=="stream") {
        std::lock_guard lock(control_);
        const auto action=request.value("action",std::string("status"));
        if(action!="status") require_owner();
        if((action=="arm" || action=="start") && (tier_<1 || !gate_ok_ || !allow_tier1_ || !allow_render_probe_))
            throw std::runtime_error("Mirror5 capture requires the permitted Tier 1 render probe");
        CaptureOptions options;
        if(action=="arm" || action=="start" || action=="update") {
            options.format=request.value("format",std::string(cmd=="stream"?"rgbd8":"raw"));
            options.color_gain=request.value("color_gain",1.0f);
            if(request.contains("lidars")) for(const auto& item:request.at("lidars").items()) {
                const auto& name=item.key();
                if(name.size()!=7 || !name.starts_with("mirror") || name.back()<'0' || name.back()>'5')
                    throw std::runtime_error("Invalid LiDAR source camera");
                options.lidar_patterns[name.back()-'0']=make_lidar_pattern(item.value());
            }
            if(request.contains("outputs")) {
                if(!options.metric()) throw std::runtime_error("Output selection requires ROS capture format");
                options.selective=true;options.outputs.fill(0);
                for(const auto& entry:request.at("outputs").items()) {
                    const auto& name=entry.key();
                    if(name.size()!=7 || !name.starts_with("mirror") || name.back()<'0' || name.back()>'5')
                        throw std::runtime_error("Invalid output camera");
                    for(const auto& output:entry.value()) {
                        uint8_t flag=output=="color"?1:output=="depth"?2:output=="preview"?4:output=="lidar"?8:output=="metadata"?16:0;
                        if(!flag) throw std::runtime_error("Unknown sensor output");
                        options.outputs[name.back()-'0']|=flag;
                    }
                }
            }
            for(size_t i=0;i<6;++i) if((options.outputs[i]&8) && !options.lidar_patterns[i])
                throw std::runtime_error("LiDAR demand requires a beam pattern");
            if(options.format!="raw" && options.format!="rgbd8" && options.format!="raw+rgbd8" && options.format!="ros" && options.format!="raw+ros")
                throw std::runtime_error("Capture format must be raw, rgbd8, raw+rgbd8 ros or raw+ros");
            if(!std::isfinite(options.color_gain) || options.color_gain<=0)
                throw std::runtime_error("Color gain must be finite and positive");
        }
        if(cmd=="stream") return render_probe_->stream(request,*transport_,options);
        return cmd=="capture_mirrors"?render_probe_->capture_views(action,transport_.get(),request.value("metadata",true),options):render_probe_->capture(action,options);
    }
    if(cmd=="render_probe") {
        std::lock_guard lock(control_);
        if(request.contains("enabled")) {
            require_owner();
            if(request.at("enabled").get<bool>()) {
                if(tier_<1 || !gate_ok_ || !allow_tier1_ || !allow_render_probe_)
                    throw std::runtime_error("Render probe requires Tier 1, matching EXE, singleplayer_research, allow_tier1 and allow_render_probe");
                try {
                    render_probe_->enable(request.value("vehicle_metadata",false),
                        request.value("mode",std::string("observe")),request.value("frame_timing",false));
                } catch(...) {
                    if(tier_==2 && !render_probe_->camera_rig(json::object()).at("enabled").get<bool>()) tier_=1;
                    throw;
                }
            } else {render_probe_->disable();if(tier_==2) tier_=1;}
        }
        return render_probe_->status();
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
        if(registration_.contains(field)) return {{"available",false},{"registration_result",registration_[field]}};
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
    static const OtModuleApi api{1,sizeof(OtModuleApi),OT_VERSION,scs_telemetry_init,module_stop};
    return abi==api.abi?&api:nullptr;
}
