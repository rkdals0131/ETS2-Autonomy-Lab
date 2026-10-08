#pragma once
#include <windows.h>
#include "ipc_layout.hpp"
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ot {
using json = nlohmann::json;
namespace fs = std::filesystem;
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE v): h(v == INVALID_HANDLE_VALUE ? nullptr : v) {}
    ~Handle() { if (h) CloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return h != nullptr; }
};
extern HMODULE module;
fs::path module_path(HMODULE mod);
fs::path log_directory();
void log(const std::string& message) noexcept;
std::string sha256_file(const fs::path& path);
json dump_process();
uint64_t qpc_now() noexcept;
uint64_t qpc_frequency() noexcept;
bool copy_memory(uintptr_t address, void* data, size_t size) noexcept;
json read_vehicle_physics(uintptr_t actor, const json& schema);
template<class T> bool read_memory(uintptr_t address, T& value) noexcept {
    return address && copy_memory(address, &value, sizeof(value));
}
struct BundleBlob {std::string camera,file;const uint8_t* data;size_t bytes;};

class Transport {
public:
    Transport(std::function<json(const json&)> handler, std::function<void()> panic, int panic_key,
              std::wstring pipe_name=L"\\\\.\\pipe\\ot",std::function<void()> poll={});
    ~Transport();
    void start(bool shared_state);
    void stop() noexcept;
    void publish(const std::string& text, uint64_t sequence) noexcept;
    json status() const;
    json publish_bundle(json manifest,const std::vector<BundleBlob>& blobs);
    json bundle_status() const;
private:
    json bundle_status_locked() const;
    mutable std::mutex bundle_mutex_;
    void serve() noexcept;
    bool io(HANDLE pipe, bool write, void* buffer, DWORD size, DWORD& transferred);
    bool wait_io(HANDLE event, DWORD timeout);
    void poll_panic();
    std::function<json(const json&)> handler_;
    std::function<void()> panic_,poll_;
    int panic_key_;
    std::wstring pipe_name_;
    Handle stop_event_, pipe_, mapping_;
    Ring* ring_ = nullptr;
    Handle bundle_mapping_;
    RingHeader* bundles_ = nullptr;
    uint32_t bundle_capacity_=0,bundle_cursor_=0;
    PSECURITY_DESCRIPTOR security_ = nullptr;
    std::thread worker_;
    uint32_t cursor_ = 0;
    bool key_was_down_ = false;
};
}
