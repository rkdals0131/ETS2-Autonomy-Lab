#pragma once
// docs/28 §5 environment gate: compare the game's live .text with the
// executable file relocated to the live base, and classify every difference
// as our hook, a patch the user accepted in ot_config.json, or foreign.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace ot::gate {
// Differing byte ranges (offset, size); ranges closer than `gap` merge.
inline std::vector<std::pair<size_t,size_t>> diff_ranges(const uint8_t* live,const uint8_t* file,size_t size,size_t gap=16) {
    std::vector<std::pair<size_t,size_t>> out;
    constexpr size_t block=4096;
    for(size_t at=0;at<size;at+=block) {
        const auto n=size-at<block?size-at:block;
        if(!std::memcmp(live+at,file+at,n)) continue;
        for(size_t i=at;i<at+n;++i) {
            if(live[i]==file[i]) continue;
            if(!out.empty() && i<=out.back().first+out.back().second+gap) out.back().second=i+1-out.back().first;
            else out.push_back({i,1});
        }
    }
    return out;
}
// Applies IMAGE_REL_BASED_DIR64 entries of a .reloc directory to a copy of one
// section (raw bytes at section_rva). Entries that straddle the section end are
// skipped; returns the number applied.
inline size_t apply_dir64(std::span<uint8_t> section,uint32_t section_rva,std::span<const uint8_t> relocations,int64_t delta) {
    size_t applied=0,at=0;
    while(at+8<=relocations.size()) {
        uint32_t page{},block{};
        std::memcpy(&page,relocations.data()+at,4);std::memcpy(&block,relocations.data()+at+4,4);
        if(block<8 || at+block>relocations.size()) break;
        for(size_t i=8;i+2<=block;i+=2) {
            uint16_t entry{};std::memcpy(&entry,relocations.data()+at+i,2);
            if((entry>>12)!=10) continue;                       // DIR64
            const uint64_t rva=page+(entry&0xFFF);
            if(rva<section_rva || rva+8>section_rva+section.size()) continue;
            uint64_t value{};std::memcpy(&value,section.data()+(rva-section_rva),8);
            value+=static_cast<uint64_t>(delta);
            std::memcpy(section.data()+(rva-section_rva),&value,8);
            ++applied;
        }
        at+=block;
    }
    return applied;
}
inline bool overlaps(uint64_t a,uint64_t a_size,uint64_t b,uint64_t b_size) noexcept {return a<b+b_size && b<a+a_size;}
}
