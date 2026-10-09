#include "game_ipc.hpp"

namespace bridge {
json command(json request) {
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
}
