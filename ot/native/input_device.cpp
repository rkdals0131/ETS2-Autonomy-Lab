#include "input_device.hpp"
#include "drive_control.hpp"
#include "manual_input.hpp"
#include <windows.h>
#include <Xinput.h>
#include <mutex>

namespace ot {
namespace {
class InputDevice {
public:
    void configure(bool permitted,const char* path) noexcept {
        std::lock_guard lock(mutex_);profile_.reset();error_.clear();
        try {if(permitted) profile_=std::make_unique<ManualInput>(std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path))));}
        catch(const std::exception& e) {error_=e.what();}
        permitted_=permitted;control_.configure(available_,permitted_,bool(profile_),error_.c_str());
        sample();
    }
    bool request(const OtDriveRequest& request,OtDriveStatus& result) noexcept {
        std::lock_guard lock(mutex_);
        if(request.action==OtDriveAction::arm) sample();
        return control_.request(request,GetTickCount64(),result);
    }
    void release(OtDriveReason reason) {std::lock_guard lock(mutex_);control_.release(reason);}
    void pause(bool paused) {std::lock_guard lock(mutex_);control_.paused(paused);}
    void available(bool value) {std::lock_guard lock(mutex_);available_=value;if(!value) active_=false;control_.configure(available_,permitted_,bool(profile_),error_.c_str());}
    static scs_result_t SCSAPIFUNC event(scs_input_event_t* event,scs_u32_t flags,scs_context_t context) noexcept {
        auto& self=*static_cast<InputDevice*>(context);std::lock_guard lock(self.mutex_);
        if(flags&SCS_INPUT_EVENT_CALLBACK_FLAG_first_in_frame) self.frame_.begin(self.sample());
        return self.frame_.next(event->input_index,event->value_float.value)?SCS_RESULT_ok:SCS_RESULT_not_found;
    }
    static void SCSAPIFUNC active(scs_u8_t active,scs_context_t context) noexcept {
        auto& self=*static_cast<InputDevice*>(context);std::lock_guard lock(self.mutex_);self.active_=active!=0;
        if(!active) self.control_.release(OtDriveReason::input_unavailable);
    }
private:
    std::array<float,3> sample() noexcept {
        PhysicalInput input;
        // SCS's first XInput gamepad is provisionally mapped to index 0. Verify
        // this mapping against disarmed game input before the first arm test.
        XINPUT_STATE state{};const auto pad_result=XInputGetState(0,&state);input.connected=pad_result==ERROR_SUCCESS;
        const bool source_known=pad_result==ERROR_SUCCESS || pad_result==ERROR_DEVICE_NOT_CONNECTED;
        if(connection_sampled_ && connected_!=input.connected) control_.release(OtDriveReason::input_unavailable);
        connected_=input.connected;connection_sampled_=true;
        if(input.connected) {const auto& pad=state.Gamepad;input.x=pad.sThumbLX/(pad.sThumbLX<0?32768.f:32767.f);input.right_trigger=pad.bRightTrigger/255.f;input.left_trigger=pad.bLeftTrigger/255.f;}
        constexpr int keys[]={'A',VK_LEFT,'D',VK_RIGHT,'W',VK_UP,'S',VK_DOWN};
        DWORD foreground{};GetWindowThreadProcessId(GetForegroundWindow(),&foreground);
        const bool in_game=foreground==GetCurrentProcessId();
        for(size_t i=0;i<8;++i) input.keys[i]=(GetAsyncKeyState(keys[i])&0x8000)!=0;
        const bool panic_down=in_game && (GetAsyncKeyState(VK_F11)&0x8000);
        std::array<float,3> result{};
        try {const auto manual=profile_?profile_->evaluate(input):ManualInput::Sample{};result=control_.frame(GetTickCount64(),active_ && source_known && bool(profile_) && !panic_down,manual.values,manual.active);}
        catch(...) {control_.release(OtDriveReason::profile_unsupported);}
        if(panic_down) {result={};control_.release(OtDriveReason::panic);}
        return result;
    }
    std::mutex mutex_;
    DriveControl control_;
    std::unique_ptr<ManualInput> profile_;
    std::string error_;
    bool available_=false,permitted_=false,active_=false;
    bool connected_=false,connection_sampled_=false;
    DriveFrame frame_;
};
InputDevice device;
const OtDriveHost host{1,sizeof(OtDriveHost),&device,
    [](void* context,bool allowed,const char* path){static_cast<InputDevice*>(context)->configure(allowed,path);},
    [](void* context,const OtDriveRequest* request,OtDriveStatus* result){return static_cast<InputDevice*>(context)->request(*request,*result);},
    [](void* context,OtDriveReason reason){static_cast<InputDevice*>(context)->release(reason);}};
}
const OtDriveHost* drive_host() {return &host;}
void input_release(OtDriveReason reason) {device.release(reason);}
void input_pause(bool value) {device.pause(value);}
void input_shutdown() {device.available(false);device.release(OtDriveReason::input_unavailable);}
scs_result_t input_initialize(scs_u32_t version,const scs_input_init_params_t* params) {
    if(version!=SCS_INPUT_VERSION_1_00) return SCS_RESULT_unsupported;
    if(!params) return SCS_RESULT_invalid_parameter;
    const auto& sdk=*static_cast<const scs_input_init_params_v100_t*>(params);
    if(!sdk.common.game_id || std::strcmp(sdk.common.game_id,"eut2")) return SCS_RESULT_unsupported;
    static const scs_input_device_input_t axes[]={{"steering","Steering",SCS_VALUE_TYPE_float},{"aforward","Throttle",SCS_VALUE_TYPE_float},{"abackward","Brake",SCS_VALUE_TYPE_float}};
    scs_input_device_t description{};description.name="ot_drive";description.display_name="OT Driving Commands";
    description.type=SCS_INPUT_DEVICE_TYPE_semantical;description.input_count=3;description.inputs=axes;
    description.callback_context=&device;description.input_event_callback=InputDevice::event;description.input_active_callback=InputDevice::active;
    const auto result=sdk.register_device(&description);device.available(result==SCS_RESULT_ok);return result;
}
}
