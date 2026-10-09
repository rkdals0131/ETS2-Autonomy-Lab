#include "drive_control.hpp"
#include "manual_input.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>

static void check(bool value) {if(!value) throw std::runtime_error("Driving contract failed");}
int main(int argc,char** argv) {
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
    check(frame.next(index,value) && index==2 && value==.2f);check(!frame.next(index,value));
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
    std::cout<<"Driving owner, epoch, expiry, takeover and SDK frame contracts passed\n";
}
