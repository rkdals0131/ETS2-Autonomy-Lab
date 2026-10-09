#pragma once
#include <windows.h>
#include <objbase.h>
#include <chrono>
#include <string>
#include <stdexcept>
#include <utility>

namespace bridge {
struct Handle {
    HANDLE h=nullptr;
    explicit Handle(HANDLE value=nullptr):h(value==INVALID_HANDLE_VALUE?nullptr:value) {}
    Handle(const Handle&)=delete;
    ~Handle(){if(h) CloseHandle(h);}
};
inline std::string uuid_string() {
    GUID value{};if(FAILED(CoCreateGuid(&value))) throw std::runtime_error("Cannot create session ID");
    wchar_t text[40];StringFromGUID2(value,text,40);char ascii[40];
    if(!WideCharToMultiByte(CP_UTF8,0,text,-1,ascii,sizeof(ascii),nullptr,nullptr)) throw std::runtime_error("Cannot encode session ID");
    return ascii;
}
inline uint64_t ticks() {return GetTickCount64();}
inline uint64_t microseconds() {return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
}
