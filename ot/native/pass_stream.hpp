#pragma once
// Engine command storage (r_cmd_storage_t, pass+0x2E8) and token streams.
// Pure layout and parsing helpers shared by the deferred pass hook and
// ot_parallel_test; nothing here touches game memory by itself.
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ot::passes {
// 0x28CE60 r_cmd_storage_t::allocate: records are {data, used dwords, capacity};
// +0x320 is the record being appended. Inline records start at +0x20.
struct CmdRecord {
    uint32_t* data;
    uint64_t used;
    uint64_t capacity;
};
struct CmdStorage {
    void* vtable;
    CmdRecord* records;
    uint64_t count;
    uint64_t capacity;
    CmdRecord inline_records[32];
    CmdRecord* current;
};
static_assert(offsetof(CmdStorage,records)==0x8 && offsetof(CmdStorage,inline_records)==0x20);
static_assert(offsetof(CmdStorage,current)==0x320 && sizeof(CmdStorage)==0x328);
static_assert(sizeof(CmdRecord)==0x18);

// Dwords per token including the token itself (2B1A30 cursor advances).
// 0 = variable or unknown: parsing stops and the rest is compared by length.
constexpr std::array<uint8_t,0x1E> token_length={
    0,10,0,2,1,5,0,0,4,11,2,3,0,4,2,2,1,2,2,4,2,3,3,1,0,0,0,0,0,0};
constexpr uint32_t end_token=0x1D;

struct Summary {
    uint64_t dwords=0;          // total dwords in the stream
    uint64_t parsed=0;          // dwords covered by tokens of known length
    uint64_t draws=0;           // token 1
    uint32_t stop_token=0;      // first token parsing could not step over (0 = none)
    uint64_t stop_offset=0;
    std::array<uint32_t,0x1E> histogram{};
    uint64_t masked_hash=0;     // FNV-1a over parsed dwords with allocation pointers masked
};

// Pointer fields depend on allocation order, so the same pass recorded twice
// differs there: token 1 dwords 8-9 (binding record), token 0xD dwords 1-2
// (transition blob) and token 0x13 payload bytes 1-8 (packed pointer).
inline uint32_t masked_dword(uint32_t token,size_t index,uint32_t value) noexcept {
    if(token==1 && (index==8 || index==9)) return 0;
    if(token==0xD && (index==1 || index==2)) return 0;
    if(token==0x13) {
        if(index==1) return value&0xFFu;
        if(index==2) return 0;
        if(index==3) return value&0xFFFFFF00u;
    }
    return value;
}

inline Summary summarize(const uint32_t* data,size_t size) noexcept {
    Summary s;s.dwords=size;
    uint64_t hash=1469598103934665603ull;
    const auto mix=[&](uint32_t v) {for(int b=0;b<4;++b) {hash^=(v>>(8*b))&0xFF;hash*=1099511628211ull;}};
    size_t at=0;
    while(at<size) {
        const auto token=data[at];
        const auto length=token<token_length.size()?token_length[token]:0;
        if(!length || at+length>size) {s.stop_token=token?token:UINT32_MAX;s.stop_offset=at;break;}
        ++s.histogram[token];
        if(token==1) ++s.draws;
        for(size_t i=0;i<length;++i) mix(masked_dword(token,i,data[at+i]));
        at+=length;
    }
    s.parsed=at;s.masked_hash=hash;
    return s;
}

// A0: sizes and token counts; A1: masked hash. Returns 0 when equal, else the
// first differing level (1 size, 2 token counts, 3 masked content).
inline int compare(const Summary& a,const Summary& b) noexcept {
    if(a.dwords!=b.dwords || a.parsed!=b.parsed || a.stop_token!=b.stop_token) return 1;
    if(a.histogram!=b.histogram) return 2;
    if(a.masked_hash!=b.masked_hash) return 3;
    return 0;
}
// First token index whose masked dwords differ, for diagnostics.
inline uint64_t first_difference(const uint32_t* a,size_t na,const uint32_t* b,size_t nb) noexcept {
    size_t at=0;
    while(at<na && at<nb) {
        const auto token=a[at];
        const auto length=token<token_length.size()?token_length[token]:0;
        if(token!=b[at] || !length || at+length>na || at+length>nb) return at;
        for(size_t i=0;i<length;++i)
            if(masked_dword(token,i,a[at+i])!=masked_dword(token,i,b[at+i])) return at;
        at+=length;
    }
    return at;
}

// Copy every dword from record `first` at `offset` to the end of the storage.
inline void gather(const CmdStorage& storage,uint64_t first,uint64_t offset,std::vector<uint32_t>& out) {
    out.clear();
    for(uint64_t r=first;r<storage.count;++r) {
        const auto& record=storage.records[r];
        const auto from=r==first?offset:0;
        if(record.used>from) out.insert(out.end(),record.data+from,record.data+record.used);
    }
}

// Pass names carry 16-digit hexadecimal object ids (e.g. sun<id>_cascade4);
// each such token becomes '#', as research/live/2026-10-14-passes/pass_names.py.
inline std::string normalize_name(const std::string& raw) {
    std::string out;out.reserve(raw.size());
    const auto hex=[&](size_t i){return std::isxdigit(static_cast<unsigned char>(raw[i]))!=0;};
    for(size_t i=0;i<raw.size();) {
        size_t j=i;
        if(i==0 || !hex(i-1)) while(j<raw.size() && j-i<16 && hex(j)) ++j;
        if(j-i==16 && (j==raw.size() || !hex(j))) {out+='#';i=j;}
        else out+=raw[i++];
    }
    return out;
}

// Deferred recording keeps [front blob][pass body][back blob] in one storage.
// At deferral the front part is recorded (k dwords); 0x227140 then appends the
// back blob. The join cuts the blob, records the body, and appends the blob again.
struct Splice {
    bool usable=false;
    uint64_t k=0;
};
// An empty storage (no record yet) splices with k = 0: whatever 0x227140 adds
// before the join opens record 0 and is all back blob.
inline Splice prepare_splice(const CmdStorage& storage) noexcept {
    if(!storage.records) return {};
    if(storage.count==0) return {true,0};
    if(storage.count!=1 || storage.current!=storage.records) return {};
    return {true,storage.current->used};
}
inline uint64_t total_dwords(const CmdStorage& storage) noexcept {
    uint64_t total=0;
    for(uint64_t r=0;r<storage.count;++r) total+=storage.records[r].used;
    return total;
}
// record(): runs the pass into the storage. allocate(): 0x28CE60 semantics.
// observe(storage) sees the body (from record 0 offset k) before the blob is
// appended again. Returns false when the storage no longer matches the
// deferral state; the pass is then recorded after the blob (DX11 skips 0xD).
template<class Record,class Allocate,class Observe>
bool run_spliced(CmdStorage& storage,const Splice& splice,std::vector<uint32_t>& tail,Record&& record,Allocate&& allocate,Observe&& observe) {
    if(splice.usable && storage.count==0 && splice.k==0) {
        record();
        observe(static_cast<const CmdStorage&>(storage));
        return true;
    }
    if(!splice.usable || storage.count!=1 || storage.current!=storage.records || storage.current->used<splice.k) {
        record();
        return false;
    }
    auto& current=*storage.current;
    tail.assign(current.data+splice.k,current.data+current.used);
    current.used=splice.k;
    record();
    observe(static_cast<const CmdStorage&>(storage));
    if(!tail.empty()) {
        auto* destination=allocate(&storage,tail.size());
        std::memcpy(destination,tail.data(),tail.size()*sizeof(uint32_t));
    }
    return true;
}
}
