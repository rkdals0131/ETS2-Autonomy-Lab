#include "ot.hpp"
#include "module_api.hpp"
#include "build_identity.hpp"
#include "input_device.hpp"
#include <map>
#include <stdexcept>

namespace ot {
class Loader;
static Loader* active_loader{};

class Loader {
public:
    Loader(scs_u32_t version,const scs_telemetry_init_params_v100_t& api)
        : version_(version),sdk_(api),bridge_(api),sdk_thread_(GetCurrentThreadId()),
          game_name_(api.common.game_name?api.common.game_name:""),game_id_(api.common.game_id),
          path_(module_path(module).parent_path()/L"ot_runtime"/L"ot_core.dll") {
        bridge_.common.game_name=game_name_.c_str();bridge_.common.game_id=game_id_.c_str();
        bridge_.register_for_event=register_event;bridge_.unregister_from_event=unregister_event;
    }
    ~Loader() {shutdown();}
    void start();
    void shutdown() noexcept;
    json command(const json& request);
private:
    struct Subscription {scs_telemetry_event_callback_t callback{};scs_context_t context{};};
    struct ConfigField {scs_named_value_t value;std::string name,text;};
    static scs_result_t SCSAPIFUNC register_event(scs_event_t id,scs_telemetry_event_callback_t callback,scs_context_t context);
    static scs_result_t SCSAPIFUNC unregister_event(scs_event_t id);
    static void SCSAPIFUNC event(scs_event_t id,const void* data,scs_context_t context) noexcept;
    void on_event(scs_event_t id,const void* data);
    void load();
    void unload();
    void expire();
    json status() const;

    scs_u32_t version_;
    scs_telemetry_init_params_v100_t sdk_,bridge_;
    DWORD sdk_thread_;
    std::string game_name_,game_id_;
    fs::path path_;
    // Only the SDK thread touches callback registrations and invokes module code.
    std::map<scs_event_t,Subscription> subscriptions_;
    std::map<std::string,std::vector<ConfigField>> configurations_;
    std::atomic<bool> paused_{true};
    std::atomic<uint64_t> frames_{0};
    std::mutex mutex_;
    std::unique_ptr<Transport> transport_;
    HMODULE payload_{};
    const OtModuleApi* api_{};
    std::string state_="unloaded",module_version_,error_;
    uint64_t generation_=0,deadline_=0;
    json operation_=nullptr;
};

void Loader::start() {
    // Keep these forwarding addresses resident, including frame_end itself.
    // SDK forbids registering the currently executing event from its callback.
    for(auto id:{SCS_TELEMETRY_EVENT_frame_start,SCS_TELEMETRY_EVENT_frame_end,
                SCS_TELEMETRY_EVENT_started,SCS_TELEMETRY_EVENT_paused,SCS_TELEMETRY_EVENT_configuration}) {
        if(sdk_.register_for_event(id,event,this)!=SCS_RESULT_ok)
            throw std::runtime_error("Loader SDK event registration failed");
        subscriptions_.emplace(id,Subscription{});
    }
    transport_=std::make_unique<Transport>([this](const json& r){return command(r);},[]{input_release(OtDriveReason::panic);},VK_F11,L"\\\\.\\pipe\\ot_loader");
    transport_->start(false);
    std::lock_guard lock(mutex_);
    try {load();}
    catch(const std::exception& e) {error_=e.what();log("Loader startup: "+error_);}
    log("Resident loader initialized; control pipe ready");
}
scs_result_t SCSAPIFUNC Loader::register_event(scs_event_t id,scs_telemetry_event_callback_t callback,scs_context_t context) {
    auto& self=*active_loader;
    if(!callback) return SCS_RESULT_invalid_parameter;
    auto found=self.subscriptions_.find(id);
    if(found==self.subscriptions_.end()) {
        const auto result=self.sdk_.register_for_event(id,event,&self);
        if(result!=SCS_RESULT_ok) return result;
        found=self.subscriptions_.emplace(id,Subscription{}).first;
    }
    if(found->second.callback) return SCS_RESULT_already_registered;
    found->second={callback,context};return SCS_RESULT_ok;
}
scs_result_t SCSAPIFUNC Loader::unregister_event(scs_event_t id) {
    auto& self=*active_loader;
    auto found=self.subscriptions_.find(id);
    if(found==self.subscriptions_.end() || !found->second.callback) return SCS_RESULT_not_found;
    found->second={};return SCS_RESULT_ok;
}
void SCSAPIFUNC Loader::event(scs_event_t id,const void* data,scs_context_t context) noexcept {
    auto& self=*static_cast<Loader*>(context);
    try {self.on_event(id,data);}
    catch(const std::exception& e) {log(std::string("Loader SDK event failed: ")+e.what());}
    catch(...) {log("Loader SDK event failed");}
}
void Loader::on_event(scs_event_t id,const void* data) {
    if(id==SCS_TELEMETRY_EVENT_started) {paused_=false;input_pause(false);}
    if(id==SCS_TELEMETRY_EVENT_paused) {paused_=true;input_pause(true);}
    if(id==SCS_TELEMETRY_EVENT_configuration && data) {
        const auto& source=*static_cast<const scs_telemetry_configuration_t*>(data);
        std::vector<ConfigField> fields;
        for(auto* field=source.attributes;field->name;++field) {
            ConfigField owned{*field,field->name,{}};
            if(field->value.type==SCS_VALUE_TYPE_string && field->value.value_string.value)
                owned.text=field->value.value_string.value;
            fields.push_back(std::move(owned));
        }
        configurations_.insert_or_assign(source.id,std::move(fields));
    }
    const auto found=subscriptions_.find(id);
    if(found!=subscriptions_.end()) {
        const auto subscriber=found->second;
        if(subscriber.callback) subscriber.callback(id,data,subscriber.context);
    }
    if(id!=SCS_TELEMETRY_EVENT_frame_end) return;
    ++frames_;
    // The module's frame callback has returned. No module SDK callback is on
    // this stack; channel unregistration is legal inside this event callback.
    std::lock_guard lock(mutex_);expire();
    if(operation_.is_null() || operation_["state"]!="queued") return;
    operation_["state"]="running";
    const auto action=operation_["command"].get<std::string>();
    try {
        if(action=="unload" || action=="reload") unload();
        if(action=="load" || action=="reload") load();
        operation_["state"]="done";error_.clear();
    } catch(const std::exception& e) {
        error_=e.what();operation_["state"]="failed";operation_["error"]=error_;
        log("Loader "+action+" failed: "+error_);
    }
}
void Loader::load() {
    if(payload_) {
        if(state_=="loaded") return;
        throw std::runtime_error("Previous module remains resident; unload must finish before loading");
    }
    state_="loading";
    payload_=LoadLibraryExW(path_.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!payload_) {state_="unloaded";throw std::runtime_error("LoadLibraryExW failed: "+std::to_string(GetLastError()));}
    try {
        const auto get=reinterpret_cast<OtGetModuleApi>(GetProcAddress(payload_,"ot_get_module_api"));
        if(!get) throw std::runtime_error("Module does not export ot_get_module_api");
        const auto candidate=get(1);
        if(!candidate || candidate->abi!=1 || candidate->size<ot_module_v1_prefix || !candidate->initialize || !candidate->stop || !candidate->version)
            throw std::runtime_error("Unsupported module ABI");
        api_=candidate;
        module_version_=api_->version;
        if(api_->size>=sizeof(OtModuleApi) && api_->attach_drive) api_->attach_drive(drive_host());
        const auto result=api_->initialize(version_,&bridge_);
        if(result!=SCS_RESULT_ok) throw std::runtime_error("Module initialization failed: "+std::to_string(result));
        // SDK configuration is normally sent only after real plugin init. Own
        // its strings and replay current configuration before hot-load events.
        for(const auto& [id,fields]:configurations_) {
            const auto found=subscriptions_.find(SCS_TELEMETRY_EVENT_configuration);
            if(found==subscriptions_.end() || !found->second.callback) break;
            std::vector<scs_named_value_t> attributes;
            attributes.reserve(fields.size()+1);
            for(const auto& field:fields) {
                auto value=field.value;value.name=field.name.c_str();
                if(value.value.type==SCS_VALUE_TYPE_string && value.value.value_string.value)
                    value.value.value_string.value=field.text.c_str();
                attributes.push_back(value);
            }
            attributes.push_back({});
            const scs_telemetry_configuration_t configuration{id.c_str(),attributes.data()};
            found->second.callback(SCS_TELEMETRY_EVENT_configuration,&configuration,found->second.context);
        }
        if(frames_.load()) {
            const auto id=paused_?SCS_TELEMETRY_EVENT_paused:SCS_TELEMETRY_EVENT_started;
            const auto found=subscriptions_.find(id);
            if(found!=subscriptions_.end() && found->second.callback)
                found->second.callback(id,nullptr,found->second.context);
        }
        state_="loaded";++generation_;
        log("Module loaded: "+module_version_+" generation="+std::to_string(generation_));
    } catch(...) {
        // Initialization failures still clean any module-owned callbacks/worker.
        if(api_ && !api_->stop()) {state_="retained";throw;}
        for(auto& [id,subscriber]:subscriptions_) subscriber={};
        api_=nullptr;
        if(FreeLibrary(payload_)) {payload_=nullptr;state_="unloaded";module_version_.clear();}
        else state_="retained";
        throw;
    }
}
void Loader::unload() {
    input_release(OtDriveReason::unloaded);
    if(!payload_) return;
    state_="unloading";
    if(api_ && !api_->stop()) {
        state_="retained";
        throw std::runtime_error("Module hooks have not drained; DLL retained. Retry unload or exit game normally");
    }
    for(auto& [id,subscriber]:subscriptions_) subscriber={};
    if(!FreeLibrary(payload_)) {
        state_="retained";throw std::runtime_error("FreeLibrary failed: "+std::to_string(GetLastError()));
    }
    payload_=nullptr;api_=nullptr;state_="unloaded";module_version_.clear();
    log("Module unloaded; resident loader remains available");
}
void Loader::expire() {
    if(!operation_.is_null() && operation_["state"]=="queued" && GetTickCount64()>=deadline_) {
        operation_["state"]="expired";
        operation_["error"]="No SDK frame-end callback within 5 seconds; no module change was made";
    }
}
json Loader::status() const {
    return {{"loader_version",OT_VERSION},{"module_abi",1},{"pid",GetCurrentProcessId()},
        {"sdk_thread_id",sdk_thread_},{"module_state",state_},{"module_version",module_version_},
        {"module_path",path_.generic_string()},{"module_handle",reinterpret_cast<uintptr_t>(payload_)},
        {"generation",generation_},{"sdk_frames",frames_.load()},{"simulation_paused",paused_.load()},
        {"operation",operation_},{"last_error",error_}};
}
json Loader::command(const json& request) {
    const auto action=request.at("cmd").get<std::string>();
    std::lock_guard lock(mutex_);expire();
    if(action=="status") return status();
    if(action!="load" && action!="unload" && action!="reload") throw std::runtime_error("Unknown loader command");
    const auto id=request.at("request_id").get<std::string>();
    if(id.empty() || id.size()>128) throw std::runtime_error("request_id must be 1-128 bytes");
    if(!operation_.is_null()) {
        if(operation_["request_id"]==id) {
            if(operation_["command"]!=action) throw std::runtime_error("request_id already used for a different command");
            return status();
        }
        if(operation_["state"]=="queued" || operation_["state"]=="running")
            throw std::runtime_error("A loader operation is already pending");
    }
    operation_={{"request_id",id},{"command",action},{"state","queued"}};
    deadline_=GetTickCount64()+5000;return status();
}
void Loader::shutdown() noexcept {
    if(transport_) {transport_->stop();transport_.reset();}
    try {
        std::lock_guard lock(mutex_);
        if(!operation_.is_null() && operation_["state"]=="queued") operation_["state"]="cancelled";
        try {unload();} catch(const std::exception& e) {log(std::string("Loader shutdown: ")+e.what());}
        for(auto& [id,subscriber]:subscriptions_) sdk_.unregister_from_event(id);
        subscriptions_.clear();
        log("Resident loader stopped; control pipe and SDK bridges released");
    } catch(...) {}
}
static std::unique_ptr<Loader> loader;
}

SCSAPI_RESULT scs_telemetry_init(scs_u32_t version,const scs_telemetry_init_params_t* params) {
    if(version!=SCS_TELEMETRY_VERSION_1_00 && version!=SCS_TELEMETRY_VERSION_1_01) return SCS_RESULT_unsupported;
    if(!params) return SCS_RESULT_invalid_parameter;
    if(ot::loader) return SCS_RESULT_already_registered;
    const auto& api=*static_cast<const scs_telemetry_init_params_v100_t*>(params);
    if(!api.common.game_id || std::strcmp(api.common.game_id,"eut2")!=0) return SCS_RESULT_unsupported;
    try {
        auto created=std::make_unique<ot::Loader>(version,api);
        ot::active_loader=created.get();created->start();ot::loader=std::move(created);
        return SCS_RESULT_ok;
    } catch(const std::exception& e) {ot::active_loader=nullptr;ot::log(std::string("Loader initialization failed: ")+e.what());return SCS_RESULT_generic_error;}
}
SCSAPI_VOID scs_telemetry_shutdown() {ot::loader.reset();ot::active_loader=nullptr;}
SCSAPI_RESULT scs_input_init(scs_u32_t version,const scs_input_init_params_t* params) {return ot::input_initialize(version,params);}
SCSAPI_VOID scs_input_shutdown() {ot::input_shutdown();}
