#pragma once
#include "wire.hpp"
#include "windows_support.hpp"
#include "../../ot/include/ipc_reader.hpp"

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
        const auto copy=[&](const char* data,uint32_t length) {bytes.assign(data,data+length);};
        return (bundles_?ot::read_bundle(base_,capacity_,sequence_,sequence_,copy):
            ot::read_state(static_cast<ot::Ring*>(base_),sequence_,sequence_,copy))>0;
    }
};
}
