#pragma once
#include "ot.hpp"
#include <span>
#include <utility>
#include <vector>

namespace ot {
// Address ranges of hook code: this DLL, detour stubs and trampolines.
using CodeRanges=std::vector<std::pair<uintptr_t,uintptr_t>>;
// True when no other thread of this process has an instruction pointer or a
// return address inside the ranges. Each thread is suspended only while its
// stack is read and is resumed in all cases.
bool threads_outside(const CodeRanges& ranges) noexcept;
// Address of the only match of bytes_to_find in the game's code plus adjustment;
// throws unless it is the inspected site at expected_rva.
uintptr_t find_target(std::span<const uint8_t> bytes_to_find,size_t adjustment,uintptr_t expected_rva);
}
