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
        if(response.find('\n')!=std::string::npos) {auto r=json::parse(response);if(!r.at("ok").get<bool>()) throw CoreError(r.at("error").get<std::string>());return r.at("result");}
    }
    throw std::runtime_error("Game reply too large");
}
static std::array<int,3> release(std::string_view text) {
    std::array<int,3> parts{-1,-1,-1};size_t at=0;
    for(size_t i=0;i<parts.size();++i) {
        const auto end=i+1<parts.size()?text.find('.',at):text.size();
        if(end==std::string_view::npos || end==at) return {-1,-1,-1};
        int value=0;
        for(const auto c:text.substr(at,end-at)) {if(c<'0' || c>'9') return {-1,-1,-1};value=value*10+(c-'0');}
        parts[i]=value;at=end+1;
    }
    return parts;
}
std::string require_core_version() {
    const auto hint=". Replace plugins/ot_runtime/ot_core.dll as described in ot/README.md";
    std::string installed;
    try {installed=command({{"cmd","version"}}).at("plugin_version").get<std::string>();}
    catch(const CoreError& e) {throw std::runtime_error(std::string("Installed core does not support the version command (")+e.what()+")"+hint);}
    const auto found=release(installed);
    if(found[0]<0) throw std::runtime_error("Installed core reports an unrecognized version '"+installed+"'"+hint);
    if(found<release(minimum_core_version))
        throw std::runtime_error("Installed core "+installed+" is older than the required "+std::string(minimum_core_version)+hint);
    return installed;
}
}
