#pragma once
#include "../native/ipc_layout.hpp"
#include <stdexcept>

namespace ot {
namespace detail {
struct ClaimedSlot {
    BundleSlot* slot{};
    ~ClaimedSlot() {if(slot) InterlockedExchange(&slot->state,0);}
};
template<class Copy>
int read_ring(void* ring,uint32_t capacity,uint32_t count,uint64_t after,uint64_t& sequence,bool latest,Copy&& copy) {
    sequence=after;
    ClaimedSlot selected;
    for(uint32_t i=0;i<count;++i) {
        auto* slot=bundle_slot(ring,capacity,i);
        if(InterlockedCompareExchange(&slot->state,3,2)!=2) continue;
        if(slot->length>capacity) {
            InterlockedExchange(&slot->state,0);
            if(selected.slot) {InterlockedExchange(&selected.slot->state,2);selected.slot=nullptr;}
            throw std::runtime_error("Invalid shared-memory slot length");
        }
        if(slot->sequence<=after) {InterlockedExchange(&slot->state,0);continue;}
        if(!selected.slot || (latest?slot->sequence>selected.slot->sequence:slot->sequence<selected.slot->sequence)) {
            if(selected.slot) InterlockedExchange(&selected.slot->state,latest?0:2);
            selected.slot=slot;
        } else InterlockedExchange(&slot->state,latest?0:2);
    }
    if(!selected.slot) return 0;
    const auto length=selected.slot->length;
    copy(reinterpret_cast<const char*>(selected.slot+1),length);
    sequence=selected.slot->sequence;
    return static_cast<int>(length);
}
}
template<class Copy>
int read_state(Ring* ring,uint64_t after,uint64_t& sequence,Copy&& copy) {
    return detail::read_ring(ring,slot_bytes,ring_slots,after,sequence,true,copy);
}
template<class Copy>
int read_bundle(void* ring,uint32_t capacity,uint64_t after,uint64_t& sequence,Copy&& copy) {
    return detail::read_ring(ring,capacity,bundle_slots,after,sequence,false,copy);
}
}
