#include "drive_control.hpp"
#include "manual_input.hpp"
#include <fstream>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>

static void check(bool value,std::source_location at=std::source_location::current()) {
    if(!value) throw std::runtime_error("Driving contract failed at line "+std::to_string(at.line()));
}
int main(int argc,char** argv) try {
    using namespace ot;
    DriveControl control;OtDriveStatus state;
    OtDriveRequest arm;arm.action=OtDriveAction::arm;arm.owner="controller_a";
    check(!control.request(arm,1000,state));
    control.configure(true,true,true);control.paused(false);control.frame(1000,true,{},false);
    check(control.request(arm,1000,state));const auto first_epoch=state.epoch;
    auto other=arm;other.owner="controller_b";check(!control.request(other,1001,state));
    OtDriveRequest command;command.action=OtDriveAction::command;command.owner=arm.owner;
    command.epoch=first_epoch;command.sequence=1;command.deadline_ms=1190;
    command.steering=.4f;command.throttle=.3f;command.brake=.2f;
    check(control.request(command,1010,state));
    const auto values=control.frame(1010,true,{},false);check(values==std::array<float,3>{-.4f,.3f,.2f});
    DriveFrame frame;frame.begin(values);uint32_t index;float value;
    check(frame.next(index,value) && index==0 && value==-.4f);
    command.sequence=2;command.throttle=.9f;check(control.request(command,1020,state));
    check(!control.request(arm,1021,state));control.release(OtDriveReason::panic);
    check(frame.next(index,value) && index==1 && value==.3f);
    check(frame.next(index,value) && index==2 && value==.2f);
    for(uint32_t i=3;i<DriveFrame::kInputs;++i) check(frame.next(index,value) && index==i && value==0 && DriveFrame::is_button(i));
    check(!frame.next(index,value));
    frame.begin(control.frame(1022,true,{},false));check(frame.next(index,value) && value==0);
    check(!control.request(command,1022,state));check(!state.armed);
    check(control.request(arm,1023,state));check(state.epoch>first_epoch);
    command.epoch=first_epoch;command.sequence=3;check(!control.request(command,1024,state));
    command.epoch=state.epoch;command.deadline_ms=1200;command.sequence=1;check(control.request(command,1030,state));
    check(!control.request(command,1040,state)); // Replay does not refresh expiry.
    command.sequence=2;command.deadline_ms=1300;check(!control.request(command,1050,state)); // Cannot extend beyond the issued 200 ms window.
    command.deadline_ms=1050;check(!control.request(command,1050,state));
    control.frame(1200,true,{},false);check(!control.request(command,1200,state));check(state.reason==OtDriveReason::expired);
    // Combined manual values may cancel; activity still revokes ownership.
    control.frame(1201,true,{},false);check(control.request(arm,1201,state));
    control.frame(1202,true,{},true);check(!control.request(command,1202,state));check(state.reason==OtDriveReason::manual);
    check(!control.request(arm,1203,state));control.frame(1204,true,{},false);check(control.request(arm,1204,state));
    control.paused(true);control.paused(false);check(!control.request(command,1205,state));check(!state.armed);
    check(control.request(arm,1206,state));control.frame(1207,false,{},false);check(!control.request(command,1207,state));
    check(state.reason==OtDriveReason::input_unavailable);
    // ACC leaves human steering untouched; LCC leaves human pedals untouched.
    control.frame(1300,true,{.5f,0,0},ot_drive_steering);
    check(control.request(arm,1300,state,ot_drive_pedals));
    command.epoch=state.epoch;command.sequence=1;command.deadline_ms=1490;
    uint32_t owned=0;check(control.request(command,1310,state,ot_drive_pedals,&owned));check(owned==ot_drive_pedals);
    check(control.frame(1311,true,{.5f,0,0},ot_drive_steering)==std::array<float,3>{0,.9f,.2f});
    control.frame(1312,true,{},ot_drive_pedals);check(!control.request(command,1313,state));check(state.reason==OtDriveReason::manual);
    control.frame(1400,true,{0,.5f,0},ot_drive_pedals);check(control.request(arm,1400,state,ot_drive_steering));
    command.epoch=state.epoch;command.sequence=1;command.deadline_ms=1590;
    check(control.request(command,1410,state,ot_drive_steering));
    check(control.frame(1411,true,{0,.5f,0},ot_drive_pedals)==std::array<float,3>{-.4f,0,0});
    // Enabling an axis under physical intervention releases; stale modes cannot renew.
    command.sequence=2;check(!control.request(command,1412,state,ot_drive_all));check(state.reason==OtDriveReason::manual);
    control.frame(1500,true,{},0);check(control.request(arm,1500,state,ot_drive_pedals));
    command.epoch=state.epoch;command.sequence=1;command.deadline_ms=1690;
    check(control.request(command,1510,state,ot_drive_all));check(control.request(OtDriveRequest{},1511,state,ot_drive_all,&owned));check(owned==ot_drive_all);
    control.release(OtDriveReason::panic);check(control.request(OtDriveRequest{},1512,state,ot_drive_all,&owned));check(owned==0 && !state.armed);
    // Auxiliary brakes: only with the pedals and the extended host; the retarder step is
    // pulsed on change and refreshed, the engine brake held, and step 0 pulsed on release.
    {
        DriveControl aux;aux.configure(true,true,true);aux.paused(false);aux.frame(2000,true,{},0);
        check(!aux.request(arm,2000,state,ot_drive_auxiliary|ot_drive_steering,nullptr,nullptr,nullptr,ot_drive_all_aux));
        check(!aux.request(arm,2000,state,ot_drive_all_aux));  // old hosts allow axes up to 3 only
        check(aux.request(arm,2000,state,ot_drive_pedals|ot_drive_auxiliary,nullptr,nullptr,nullptr,ot_drive_all_aux));
        OtDriveRequest c;c.action=OtDriveAction::command;c.owner=arm.owner;c.epoch=state.epoch;c.sequence=1;c.deadline_ms=2190;c.throttle=0;c.brake=.05f;
        const uint32_t acc=ot_drive_pedals|ot_drive_auxiliary;
        check(!aux.request(c,2010,state,acc,nullptr,nullptr,nullptr,ot_drive_all_aux));  // aux owned but no aux values
        OtDriveAux bad{6,0};check(!aux.request(c,2010,state,acc,nullptr,&bad,nullptr,ot_drive_all_aux));
        OtDriveAux want{3,1},got{};uint32_t aux_owned=0;
        check(aux.request(c,2010,state,acc,&aux_owned,&want,&got,ot_drive_all_aux));check(aux_owned==acc && got.retarder==3 && got.engine_brake==1);
        check(aux.frame(2011,true,{},0)==std::array<float,3>{0,0,.05f});check((aux.aux_frame()==DriveAuxFrame{3,true}));
        aux.frame(2011+DriveControl::kRetarderPulseMs,true,{},0);check((aux.aux_frame()==DriveAuxFrame{-1,true}));
        DriveFrame f;f.begin({0,0,.05f},DriveAuxFrame{3,true});
        for(uint32_t i=0;i<DriveFrame::kInputs;++i) {check(f.next(index,value) && index==i);check(value==(i==2?.05f:(i==3+3 || i==DriveFrame::kMotorbrake)?1.f:0.f));}
        c.sequence=2;c.deadline_ms=2340;check(aux.request(c,2150,state,acc,nullptr,&want,nullptr,ot_drive_all_aux));
        c.sequence=3;c.deadline_ms=2520;check(aux.request(c,2330,state,acc,nullptr,&want,nullptr,ot_drive_all_aux));
        aux.frame(2400,true,{},0);check(aux.aux_frame().retarder_pulse==-1);  // same step: no new pulse yet
        aux.frame(2011+DriveControl::kRetarderRefreshMs,true,{},0);check(aux.aux_frame().retarder_pulse==3);  // refreshed
        want={1,0};c.sequence=4;c.deadline_ms=2600;check(aux.request(c,2512,state,acc,nullptr,&want,nullptr,ot_drive_all_aux));
        aux.frame(2513,true,{},0);check((aux.aux_frame()==DriveAuxFrame{1,false}));
        aux.frame(2590,true,{},ot_drive_pedals);  // the driver's pedal releases everything
        check((aux.aux_frame()==DriveAuxFrame{0,false}));  // step 0 pulsed once
        aux.frame(2590+DriveControl::kRetarderPulseMs,true,{},0);check((aux.aux_frame()==DriveAuxFrame{-1,false}));
        uint32_t after=9;OtDriveAux echo{};check(aux.request(OtDriveRequest{},2800,state,ot_drive_all,&after,nullptr,&echo,ot_drive_all_aux));
        check(after==0 && echo.retarder==-1 && state.reason==OtDriveReason::manual);
    }
    if(argc>1) {
        ManualInput profile{std::filesystem::path(argv[1])};PhysicalInput input{true};
        check(!profile.evaluate(input).active);
        input.keys[0]=input.keys[2]=true;auto sample=profile.evaluate(input);check(sample.values[0]==0 && sample.active);
        input.keys[2]=false;input.x=1;sample=profile.evaluate(input);check(sample.values[0]==0 && sample.active);
        input={true};input.x=.16f;input.right_trigger=.03f;input.left_trigger=.04f;check(!profile.evaluate(input).active);
        input.x=.16001f;check(profile.evaluate(input).active);input.x=0;input.right_trigger=.03001f;check(profile.evaluate(input).active);
        input.right_trigger=0;input.left_trigger=.04001f;check(profile.evaluate(input).active);
        input={false};input.keys[4]=true;sample=profile.evaluate(input);check(sample.active && sample.values[1]==1);
    }
    if(argc>2) { // Live profile: pedals gate on a custom modifier, deadzones 0/0.
        ManualInput profile{std::filesystem::path(argv[2])};PhysicalInput input{true};
        auto sample=profile.evaluate(input);check(!sample.active && sample.values==std::array<float,3>{});
        input.right_trigger=.001f;check(profile.evaluate(input).active); // Without a deadzone trigger noise is takeover.
        input.right_trigger=.5f;sample=profile.evaluate(input);check(sample.active && sample.values[1]==.5f);
        // Holding LB (joy.b5) freezes the game's throttle; the pull still counts as activity.
        input.buttons=0x0100;input.right_trigger=.8f;sample=profile.evaluate(input);check(sample.active && sample.values[1]==.5f);
        input.buttons=0;sample=profile.evaluate(input);check(sample.values[1]==.8f);
        input={false};input.keys[6]=true;sample=profile.evaluate(input);check(sample.active && sample.values[2]==1);
        // Doubled operators have no verified engine meaning and reject the profile.
        const auto path=std::filesystem::temp_directory_path()/"ot_drive_operator_test.sii";
        const auto loads=[&](const char* steering) {
            {std::ofstream file(path);file<<"SiiNunit\n{\ninput_config : _nameless.t {\n"
                " config_lines[0]: \"device keyboard `di8.keyboard`\"\n config_lines[1]: \"device joy `xinput.xinput_gamepad_1`\"\n"
                " config_lines[2]: \"mix dsteering `"<<steering<<"`\"\n"
                " config_lines[3]: \"mix steering `dsteering - semantical.steering?0`\"\n"
                " config_lines[4]: \"mix forward `keyboard.w?0 + semantical.aforward?0`\"\n"
                " config_lines[5]: \"mix backward `keyboard.s?0 + semantical.abackward?0`\"\n}\n}\n";}
            try {ManualInput accepted{path};return true;} catch(const std::exception&) {return false;}
        };
        check(loads("keyboard.a?0 & (! keyboard.d?0)"));check(!loads("keyboard.a?0 && keyboard.d?0"));check(!loads("keyboard.a?0 || keyboard.d?0"));
        std::filesystem::remove(path);
    }
    std::cout<<"Driving owner, epoch, expiry, takeover, auxiliary brake and SDK frame contracts passed\n";
} catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
