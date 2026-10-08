#include "ot.hpp"
#include <sddl.h>
#include <array>
#include <stdexcept>

namespace ot {
Transport::Transport(std::function<json(const json&)> handler,std::function<void()> panic,int key,std::wstring pipe_name)
    : handler_(std::move(handler)),panic_(std::move(panic)),panic_key_(key),pipe_name_(std::move(pipe_name)),
      stop_event_(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {}
Transport::~Transport() { stop(); if(ring_) UnmapViewOfFile(ring_); if(security_) LocalFree(security_); }
void Transport::start(bool shared) {
    if(!stop_event_) throw std::runtime_error("Stop event creation failed");
    HANDLE token_raw{};
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token_raw)) throw std::runtime_error("Cannot obtain current user SID");
    Handle token(token_raw); DWORD bytes=0;
    GetTokenInformation(token.h,TokenUser,nullptr,0,&bytes);
    std::vector<unsigned char> info(bytes);
    if(!GetTokenInformation(token.h,TokenUser,info.data(),bytes,&bytes)) throw std::runtime_error("TokenUser failed");
    LPWSTR sid{};
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(info.data())->User.Sid,&sid)) throw std::runtime_error("SID conversion failed");
    std::wstring acl=L"D:P(A;;GA;;;"+std::wstring(sid)+L")"; LocalFree(sid);
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(),SDDL_REVISION_1,&security_,nullptr)) throw std::runtime_error("IPC ACL creation failed");
    SECURITY_ATTRIBUTES sa{sizeof(sa),security_,FALSE};
    pipe_.h=CreateNamedPipeW(pipe_name_.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,65536,65536,0,&sa);
    if(pipe_.h==INVALID_HANDLE_VALUE) {pipe_.h=nullptr; throw std::runtime_error("Cannot own command pipe; another instance or access error");}
    if(shared) {
        mapping_.h=CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,sizeof(Ring),L"Local\\OT_State");
        if(!mapping_ || GetLastError()==ERROR_ALREADY_EXISTS) throw std::runtime_error("Cannot exclusively own Local\\OT_State");
        ring_=static_cast<Ring*>(MapViewOfFile(mapping_.h,FILE_MAP_ALL_ACCESS,0,0,sizeof(Ring)));
        if(!ring_) throw std::runtime_error("Cannot map Local\\OT_State");
        std::memset(ring_,0,sizeof(Ring));
        std::memcpy(ring_->header.magic,"OTSTATE1",8);
        ring_->header.abi=1; ring_->header.slots=ring_slots; ring_->header.bytes_per_slot=slot_bytes;
        ring_->header.producer_pid=GetCurrentProcessId();
    }
    worker_=std::thread(&Transport::serve,this);
}
void Transport::stop() noexcept {
    if(stop_event_) SetEvent(stop_event_.h);
    if(pipe_) CancelIoEx(pipe_.h,nullptr);
    if(worker_.joinable()) worker_.join();
}
void Transport::poll_panic() {
    if(!panic_key_) return;
    DWORD owner=0; GetWindowThreadProcessId(GetForegroundWindow(),&owner);
    bool down=owner==GetCurrentProcessId() && (GetAsyncKeyState(panic_key_)&0x8000);
    if(down && !key_was_down_) panic_();
    key_was_down_=down;
}
bool Transport::wait_io(HANDLE event,DWORD timeout) {
    HANDLE events[]={stop_event_.h,event}; const auto start=GetTickCount64();
    for(;;) {
        DWORD result=WaitForMultipleObjects(2,events,FALSE,50);
        if(result==WAIT_OBJECT_0+1) return true;
        if(result!=WAIT_TIMEOUT) return false;
        poll_panic();
        if(timeout!=INFINITE && GetTickCount64()-start>=timeout) return false;
    }
}
bool Transport::io(HANDLE pipe,bool write,void* buffer,DWORD size,DWORD& transferred) {
    Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr)); if(!event) return false;
    OVERLAPPED op{}; op.hEvent=event.h;
    BOOL ok=write?WriteFile(pipe,buffer,size,&transferred,&op):ReadFile(pipe,buffer,size,&transferred,&op);
    if(ok) return true;
    if(GetLastError()!=ERROR_IO_PENDING) return false;
    if(!wait_io(event.h,2000)) {CancelIoEx(pipe,&op); GetOverlappedResult(pipe,&op,&transferred,TRUE); return false;}
    return GetOverlappedResult(pipe,&op,&transferred,FALSE)!=FALSE;
}
void Transport::serve() noexcept {
    try {
        while(WaitForSingleObject(stop_event_.h,0)==WAIT_TIMEOUT) {
            Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr)); if(!event) break;
            OVERLAPPED op{}; op.hEvent=event.h;
            BOOL connected=ConnectNamedPipe(pipe_.h,&op);
            DWORD error=connected?ERROR_SUCCESS:GetLastError();
            if(error==ERROR_IO_PENDING) {
                if(!wait_io(event.h,INFINITE)) {DWORD ignored{}; CancelIoEx(pipe_.h,&op); GetOverlappedResult(pipe_.h,&op,&ignored,TRUE); break;}
                DWORD ignored{}; connected=GetOverlappedResult(pipe_.h,&op,&ignored,FALSE);
            } else connected=(error==ERROR_PIPE_CONNECTED || connected);
            if(!connected) break;
            std::string input; std::array<char,4096> buffer{}; bool complete=false;
            while(input.size()<65536) {
                DWORD n{}; if(!io(pipe_.h,false,buffer.data(),static_cast<DWORD>(buffer.size()),n) || !n) break;
                input.append(buffer.data(),n);
                auto end=input.find('\n');
                if(end!=std::string::npos) {input.resize(end); complete=true; break;}
            }
            json response;
            try {
                if(!complete || input.size()>65536) throw std::runtime_error("Request must be one JSON line, at most 65536 bytes");
                auto request=json::parse(input);
                if(!request.is_object()) throw std::runtime_error("Request must be a JSON object");
                response={{"ok",true},{"result",handler_(request)}};
            } catch(const std::exception& e) {response={{"ok",false},{"error",e.what()}};}
            std::string output=response.dump()+"\n";
            size_t offset=0;
            while(offset<output.size()) {
                DWORD n{}; auto size=static_cast<DWORD>(output.size()-offset);
                if(!io(pipe_.h,true,output.data()+offset,size,n) || !n) break;
                offset+=n;
            }
            // Let the client drain the reply and close. DisconnectNamedPipe would
            // otherwise discard buffered reply bytes. A stalled client times out.
            if(offset==output.size()) {char end{}; DWORD n{}; io(pipe_.h,false,&end,1,n);}
            DisconnectNamedPipe(pipe_.h);
            poll_panic();
        }
    } catch(const std::exception& e) {log(std::string("Command server stopped: ")+e.what()); panic_();}
    catch(...) {log("Command server stopped with unknown exception"); panic_();}
}
void Transport::publish(const std::string& text,uint64_t sequence) noexcept {
    if(!ring_) return;
    if(text.size()>slot_bytes) {InterlockedIncrement64(&ring_->header.dropped); return;}
    for(uint32_t n=0;n<ring_slots;++n) {
        auto& slot=ring_->slots[(cursor_+n)%ring_slots];
        LONG state=InterlockedCompareExchange(&slot.state,0,0);
        if(state!=0 && state!=2) continue;
        if(InterlockedCompareExchange(&slot.state,1,state)!=state) continue;
        slot.length=static_cast<uint32_t>(text.size()); slot.sequence=sequence;
        std::memcpy(slot.payload,text.data(),text.size());
        InterlockedExchange(&slot.state,2);
        InterlockedExchange64(&ring_->header.published,static_cast<LONG64>(sequence));
        cursor_=(cursor_+n+1)%ring_slots; return;
    }
    InterlockedIncrement64(&ring_->header.dropped);
}
json Transport::status() const {
    if(!ring_) return {{"enabled",false}};
    return {{"enabled",true},{"abi",1},{"slots",ring_slots},{"slot_bytes",slot_bytes},
        {"published",InterlockedCompareExchange64(&ring_->header.published,0,0)},
        {"dropped",InterlockedCompareExchange64(&ring_->header.dropped,0,0)}};
}
}
