// Game-free check of the M3a pieces that touch engine data: the deferred
// splice keeps [front blob][body][back blob] (also across a chunk boundary),
// storages with several records fall back, the shadow comparator ignores
// allocation pointers but catches content, token and size differences, and
// pass names normalize their object ids.
#include "pass_stream.hpp"
#include <cstdio>
#include <deque>

using namespace ot::passes;
namespace {
int failures=0;
void check(bool ok,const char* what) {std::printf("%s %s\n",ok?"PASS":"FAIL",what);if(!ok) ++failures;}

// A storage with small chunks; allocate() follows 0x28CE60 (strict "<" bound,
// a new record starts with used = n).
struct FakeStorage {
    static constexpr uint64_t chunk=64;
    CmdStorage storage{};
    std::deque<std::vector<uint32_t>> chunks;
    explicit FakeStorage(bool empty=false) {
        storage.records=storage.inline_records;storage.capacity=32;
        if(!empty) add_record(0);
    }
    uint32_t* add_record(uint64_t used) {
        chunks.emplace_back(chunk);
        auto& record=storage.records[storage.count++];
        record={chunks.back().data(),used,chunk};
        storage.current=&record;
        return record.data;
    }
    uint32_t* allocate(uint64_t n) {
        if(auto* current=storage.current;current && current->used+n<current->capacity) {
            auto* p=current->data+current->used;current->used+=n;return p;
        }
        return add_record(n);
    }
    void append(std::initializer_list<uint32_t> dwords) {
        auto* at=allocate(dwords.size());
        for(auto v:dwords) *at++=v;
    }
    auto allocator() {return [this](CmdStorage*,uint64_t n){return allocate(n);};}
    std::vector<uint32_t> stream() const {std::vector<uint32_t> out;gather(storage,0,0,out);return out;}
};
std::vector<uint32_t> blob(uint32_t tag) {return {0xD,0x1000+tag,0x2000+tag,0x3000+tag};}

void splice_order() {
    FakeStorage f;
    f.append({0xD,1,2,3});                                  // front blob, before 0x226960
    const auto splice=prepare_splice(f.storage);
    check(splice.usable && splice.k==4,"deferral records k after the front blob");
    f.append({0xD,7,8,9});                                  // back blob written by 0x227140
    std::vector<uint32_t> tail,observed;
    const bool used=run_spliced(f.storage,splice,tail,
        [&]{f.append({0x14,5});f.append({0x14,6});f.append({4});},
        f.allocator(),[&](const CmdStorage& s){gather(s,0,splice.k,observed);});
    const std::vector<uint32_t> expected={0xD,1,2,3,0x14,5,0x14,6,4,0xD,7,8,9};
    check(used && f.stream()==expected,"splice puts the body between the blobs");
    check(observed==std::vector<uint32_t>({0x14,5,0x14,6,4}),"observer sees only the body before the back blob returns");
}
void splice_across_chunks() {
    FakeStorage f;
    f.append({0xD,1,2,3});
    const auto splice=prepare_splice(f.storage);
    f.append({0xD,7,8,9});
    std::vector<uint32_t> tail;
    std::vector<uint32_t> body;
    for(uint32_t i=0;i<40;++i) body.insert(body.end(),{0x14,i});   // 80 dwords: crosses the 64-dword chunk
    run_spliced(f.storage,splice,tail,[&]{for(size_t i=0;i<body.size();i+=2) f.append({body[i],body[i+1]});},
        f.allocator(),[](const CmdStorage&){});
    std::vector<uint32_t> expected={0xD,1,2,3};
    expected.insert(expected.end(),body.begin(),body.end());
    expected.insert(expected.end(),{0xD,7,8,9});
    check(f.storage.count>1 && f.stream()==expected,"splice order holds when the body opens a new chunk");
}
void splice_fallback() {
    FakeStorage f;
    for(int i=0;i<20;++i) f.append({0x14,static_cast<uint32_t>(i),0x14,0});   // 80 dwords: two records
    check(!prepare_splice(f.storage).usable,"a storage with two records is not spliced");
    FakeStorage g;
    g.append({0xD,1,2,3});
    const auto splice=prepare_splice(g.storage);
    g.append({0xD,7,8,9});
    g.add_record(0);                                       // storage changed between deferral and join
    std::vector<uint32_t> tail;
    const bool used=run_spliced(g.storage,splice,tail,[&]{g.append({4});},g.allocator(),[](const CmdStorage&){});
    check(!used && g.stream()==std::vector<uint32_t>({0xD,1,2,3,0xD,7,8,9,4}),"a changed storage records after the blob instead");
}
void splice_empty_storage() {
    FakeStorage f(true);                                    // no front blob: no record yet
    const auto splice=prepare_splice(f.storage);
    check(splice.usable && splice.k==0,"an empty storage is spliced from offset 0");
    f.append({0xD,7,8,9});                                  // back blob opens record 0
    std::vector<uint32_t> tail;
    run_spliced(f.storage,splice,tail,[&]{f.append({0x14,5});f.append({4});},f.allocator(),[](const CmdStorage&){});
    check(f.stream()==std::vector<uint32_t>({0x14,5,4,0xD,7,8,9}),"the back blob follows the body of an empty storage");
    FakeStorage g(true);
    const auto none=prepare_splice(g.storage);
    const bool used=run_spliced(g.storage,none,tail,[&]{g.append({4});},g.allocator(),[](const CmdStorage&){});
    check(used && g.stream()==std::vector<uint32_t>({4}),"an empty storage without blobs records only the body");
}
void comparator() {
    // draw (10 dwords, binding pointer at 8-9), blob (pointer at 1-2), token 0x13 (pointer in payload bytes 1-8)
    std::vector<uint32_t> a={1,11,12,13,14,15,16,17,0xAAAA,0xBBBB, 0xD,0x111,0x222,3, 0x13,0x55667707,0x11223344,0x99887766, 0x14,9, 4};
    auto b=a;
    b[8]=0xCCCC;b[9]=0xDDDD;b[11]=0x333;b[12]=0x444;
    b[15]=0xFFEEDD07;b[16]=0x01020304;b[17]=0x998877AA;
    const auto sa=summarize(a.data(),a.size()),sb=summarize(b.data(),b.size());
    check(sa.parsed==a.size() && sa.stop_token==0 && sa.draws==1,"summary parses the fixed-length tokens");
    check(compare(sa,sb)==0,"allocation pointers do not count as differences");
    auto c=a;c[19]=10;
    check(compare(sa,summarize(c.data(),c.size()))==3,"changed token content is level 3");
    auto d=a;d[18]=3;                                      // token 0x14 -> token 3, both 2 dwords
    check(compare(sa,summarize(d.data(),d.size()))==2,"a different token of the same size is level 2");
    auto e=a;e.insert(e.end()-1,{0x14,1});
    check(compare(sa,summarize(e.data(),e.size()))==1,"an extra token is level 1");
    check(first_difference(a.data(),a.size(),c.data(),c.size())==18,"first difference points at the token");
    auto f=a;f[17]=0x99887700;                             // low byte of the third payload dword is pointer byte 8
    check(compare(sa,summarize(f.data(),f.size()))==0,"token 0x13 masks exactly payload bytes 1-8");
    auto g=a;g[17]=0x11887766;
    check(compare(sa,summarize(g.data(),g.size()))==3,"token 0x13 keeps payload bytes 9-11");
    const std::vector<uint32_t> variable={0x14,1,6,99,98};
    const auto sv=summarize(variable.data(),variable.size());
    check(sv.stop_token==6 && sv.stop_offset==2 && sv.parsed==2,"variable-length tokens stop parsing with their offset");
}
void names() {
    check(normalize_name("sun0000029B0FD0B558_cascade4")=="sun#_cascade4","object id becomes #");
    check(normalize_name("scene defattr")=="scene defattr","plain names are unchanged");
    check(normalize_name("a0123456789abcdef0")=="a0123456789abcdef0","17 hex digits are not an id");
    check(normalize_name("0123456789ABCDEF")=="#","an id alone becomes #");
}
}

int main() {
    splice_order();
    splice_across_chunks();
    splice_fallback();
    splice_empty_storage();
    comparator();
    names();
    std::printf(failures?"FAILED %d\n":"ALL PASSED\n",failures);
    return failures?1:0;
}
