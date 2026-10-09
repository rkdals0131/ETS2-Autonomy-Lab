#pragma once
#include "messages.hpp"
#include <d3d11_4.h>
#include <wrl/client.h>
#include <map>

namespace bridge {
// The read worker owns this device, its immediate context and opened resources.
// No game context is accessed here, and no GPU wait runs in the game process.
class GpuReadback {
    template<class T> using Com=Microsoft::WRL::ComPtr<T>;
    struct Texture {Com<ID3D11Texture2D> source,staging;D3D11_TEXTURE2D_DESC desc{};};
    struct Slot {Com<ID3D11Fence> fence;std::map<std::string,std::pair<uint64_t,Texture>> textures;};
    Com<ID3D11Device5> device_;
    Com<ID3D11DeviceContext4> context_;
    std::map<uint64_t,Slot> slots_;
    uint64_t stream_=0;
    LUID adapter_{};
public:
    // GPU-only profiling still copies and returns the ownership fence, while
    // omitting staging Map and CPU image storage.
    void read(SensorBundle& bundle,DWORD producer_pid,bool map_to_cpu=true);
};
}
