#include "ot.hpp"
#include <bcrypt.h>
#include <dbghelp.h>
#include <fstream>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace ot {
HMODULE module{};
fs::path module_path(HMODULE mod) {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(mod, path.data(), static_cast<DWORD>(path.size()));
    if (!n || n == path.size()) throw std::runtime_error("GetModuleFileNameW failed");
    path.resize(n); return path;
}
fs::path log_directory() {
    wchar_t path[32768]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", path, 32768)) throw std::runtime_error("LOCALAPPDATA missing");
    fs::path result = fs::path(path) / L"ETS2AutonomyLab" / L"ot" / std::to_wstring(GetCurrentProcessId());
    fs::create_directories(result); return result;
}
void log(const std::string& message) noexcept {
    try {
        static std::mutex lock;
        std::lock_guard guard(lock);
        std::ofstream out(log_directory() / L"ot_core.log", std::ios::app);
        SYSTEMTIME time{}; GetSystemTime(&time);
        out << time.wYear << '-' << time.wMonth << '-' << time.wDay << 'T'
            << time.wHour << ':' << time.wMinute << ':' << time.wSecond << "Z " << message << '\n';
    } catch (...) {}
}
std::string sha256_file(const fs::path& path) {
    struct Crypto {
        BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_HASH_HANDLE hash{};
        ~Crypto() { if(hash) BCryptDestroyHash(hash); if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0); }
    } c;
    if (BCryptOpenAlgorithmProvider(&c.algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA256 provider failed");
    if (BCryptCreateHash(c.algorithm,&c.hash,nullptr,0,nullptr,0,0)<0) throw std::runtime_error("SHA256 initialization failed");
    std::ifstream input(path,std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open executable for version gate");
    std::array<unsigned char,65536> buffer{};
    while (input.read(reinterpret_cast<char*>(buffer.data()),buffer.size()) || input.gcount()) {
        if(BCryptHashData(c.hash,buffer.data(),static_cast<ULONG>(input.gcount()),0)<0) throw std::runtime_error("SHA256 read failed");
    }
    if(input.bad()) throw std::runtime_error("Executable read error");
    std::array<unsigned char,32> digest{};
    if(BCryptFinishHash(c.hash,digest.data(),static_cast<ULONG>(digest.size()),0)<0) throw std::runtime_error("SHA256 finish failed");
    std::ostringstream out; out << std::hex << std::setfill('0');
    for(auto b:digest) out << std::setw(2) << static_cast<int>(b);
    return out.str();
}
json dump_process() {
    // Explicit diagnostic command. Does not replace the game's exception handler.
    auto path=log_directory()/(L"manual-"+std::to_wstring(GetTickCount64())+L".dmp");
    Handle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file || !MiniDumpWriteDump(GetCurrentProcess(),GetCurrentProcessId(),file.h,MiniDumpNormal,nullptr,nullptr,nullptr))
        throw std::runtime_error("MiniDumpWriteDump failed: "+std::to_string(GetLastError()));
    return {{"path",path.generic_string()},{"kind","manual_minidump"}};
}
bool copy_memory(uintptr_t address,void* data,size_t size) noexcept {
    SIZE_T done=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(address),data,size,&done) && done==size;
}
}
BOOL WINAPI DllMain(HINSTANCE instance,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) ot::module=instance;
    return TRUE;
}
