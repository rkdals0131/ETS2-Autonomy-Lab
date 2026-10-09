#pragma once
#include "wire.hpp"
#include "windows_support.hpp"
#include "../../ot/native/ipc_layout.hpp"

namespace bridge {
json command(json request);
class Mapping {
    Handle file_;void* base_=nullptr;uint32_t capacity_=0,producer_=0;bool bundles_;uint64_t sequence_=0;
public:
    explicit Mapping(bool bundles):file_(OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,bundles?L"Local\\OT_Bundles":L"Local\\OT_State")),bundles_(bundles) {
        if(!file_.h) return;
        auto* header=static_cast<ot::RingHeader*>(MapViewOfFile(file_.h,FILE_MAP_ALL_ACCESS,0,0,64));
        if(!header) throw std::runtime_error("Cannot map IPC header");
        const auto valid=header->abi==1 && header->slots==(bundles?ot::bundle_slots:ot::ring_slots) &&
            std::memcmp(header->magic,bundles?"OTBNDL01":"OTSTATE1",8)==0;
        capacity_=header->bytes_per_slot;producer_=header->producer_pid;UnmapViewOfFile(header);
        if(!valid || !capacity_ || capacity_>(bundles?ot::bundle_max_bytes:ot::slot_bytes) || (!bundles && capacity_!=ot::slot_bytes))
            throw std::runtime_error("Unsupported game shared-memory ABI");
        const size_t size=64+(bundles?ot::bundle_slots:ot::ring_slots)*(size_t{64}+capacity_);
        base_=MapViewOfFile(file_.h,FILE_MAP_ALL_ACCESS,0,0,size);
        if(!base_) throw std::runtime_error("Cannot map IPC slots");
        // The caller owns OT_Bundles_Reader for this mapping's whole lifetime.
        // No other live consumer can own state 3; recover an interrupted copy.
        if(bundles_) for(uint32_t i=0;i<ot::bundle_slots;++i)
            InterlockedCompareExchange(&ot::bundle_slot(base_,capacity_,i)->state,0,3);
    }
    ~Mapping(){if(base_) UnmapViewOfFile(base_);}
    bool available() const {return base_!=nullptr;}
    DWORD producer() const {return producer_;}
    bool read(Bytes& bytes) {
        if(bundles_) {
            ot::BundleSlot* oldest=nullptr;
            for(uint32_t i=0;i<ot::bundle_slots;++i) {
                auto* slot=ot::bundle_slot(base_,capacity_,i);
                if(InterlockedCompareExchange(&slot->state,2,2)==2 && (!oldest || slot->sequence<oldest->sequence)) oldest=slot;
            }
            if(!oldest || InterlockedCompareExchange(&oldest->state,3,2)!=2) return false;
            struct Return {ot::BundleSlot* s;~Return(){InterlockedExchange(&s->state,0);}} release{oldest};
            if(oldest->length>capacity_) throw std::runtime_error("Invalid shared-memory slot length");
            bytes.resize(oldest->length);std::memcpy(bytes.data(),oldest+1,bytes.size());return true;
        }
        bool copied=false;
        for(uint32_t i=0;i<(bundles_?ot::bundle_slots:ot::ring_slots);++i) {
            auto* slot=ot::bundle_slot(base_,capacity_,i);
            if(InterlockedCompareExchange(&slot->state,3,2)!=2) continue;
            struct Return {ot::BundleSlot* s;~Return(){InterlockedExchange(&s->state,0);}} release{slot};
            if(slot->length>capacity_) throw std::runtime_error("Invalid shared-memory slot length");
            if(slot->sequence<=sequence_) continue;
            // Latest ready bundle wins. The copied bytes are owned before the slot is returned.
            bytes.resize(slot->length);std::memcpy(bytes.data(),slot+1,bytes.size());sequence_=slot->sequence;copied=true;
        }
        return copied;
    }
};
}
