#pragma once
#include "ot.hpp"

namespace ot {
// x64 stack walk of a suspended thread of this process. RtlLookupFunctionEntry
// can take the function-table lock and RtlVirtualUnwind can fault on a bad
// frame; either could deadlock against a target suspended inside the loader.
// This walker reads only the unwind data and code of images that the table
// holds a reference to (so none can be unmapped during a run) and the target
// stack between its RSP and StackBase, so it takes no lock and cannot fault.
struct UnwindImage {
    uintptr_t base=0,end=0;
    const RUNTIME_FUNCTION* functions=nullptr;size_t count=0;
    std::string name,path;
};
class UnwindTable {
public:
    UnwindTable()=default;
    ~UnwindTable() {release();}
    UnwindTable(UnwindTable&& other) noexcept;
    UnwindTable& operator=(UnwindTable&& other) noexcept;
    UnwindTable(const UnwindTable&)=delete;
    UnwindTable& operator=(const UnwindTable&)=delete;
    // Lists and references every module and pre-reads its unwind data so the
    // walk does not page-fault while the target is suspended. Allocates and
    // takes the loader lock; call while no sampled thread is suspended.
    void build();
    // Drops the module references; images stay listed for address lookups only.
    void release() noexcept;
    const UnwindImage* find(uintptr_t address) const noexcept;
    const std::vector<UnwindImage>& images() const noexcept {return images_;}
private:
    std::vector<UnwindImage> images_;
    std::vector<HMODULE> pins_;
};
enum class StackEnd:uint8_t {top,depth,unknown_code,bad_frame,count};
const char* stack_end_name(StackEnd end) noexcept;
// TEB of a thread opened with THREAD_QUERY_LIMITED_INFORMATION; its NT_TIB
// gives the current stack bounds (they move with fibers, so read per sample).
const NT_TIB* thread_tib(HANDLE thread);
// frames[0] is RIP, the rest are return addresses. Returns the frame count.
size_t walk_stack(const UnwindTable& table,const CONTEXT& context,const NT_TIB* tib,
                  uint64_t* frames,size_t depth,StackEnd& end) noexcept;
// One frame of walk_stack within [stack_low, stack_high), updating RIP, RSP
// and the integer registers. For tests against RtlVirtualUnwind.
bool unwind_one(const UnwindImage& image,CONTEXT& context,uintptr_t stack_low,uintptr_t stack_high,bool sampled) noexcept;
}
