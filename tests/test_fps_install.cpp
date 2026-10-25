// SPDX-License-Identifier: GPL-3.0-or-later
// Admission and actual timing writes in private copies. Never starts the game.
#include "src/fps/timing.h"
#include "src/fps/patch_guard.h"
#include "src/fps/timing_verify.h"
#include "src/fps/sprj_flipper.h"
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

static void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
template<class T> static T get(const std::vector<unsigned char>& data, std::size_t offset) {
    require(offset <= data.size() && sizeof(T) <= data.size() - offset, "truncated ELF");
    T v; std::memcpy(&v,data.data()+offset,sizeof(v)); return v;
}
static std::vector<unsigned char> load(const char* path) {
    std::ifstream f(path,std::ios::binary);
    std::vector<unsigned char> file(std::istreambuf_iterator<char>(f),{});
    require(file.size()>=64 && !std::memcmp(file.data(),"\177ELF\2\1",6),"expected ELF64");
    const auto ph=get<std::uint64_t>(file,32);
    const auto stride=get<std::uint16_t>(file,54), count=get<std::uint16_t>(file,56);
    require(stride==56 && count && ph<=file.size() && count<=(file.size()-ph)/stride,"ELF headers");
    std::vector<unsigned char> image;
    for (unsigned i=0;i<count;++i) {
        const auto p=ph+i*stride; const auto kind=get<std::uint32_t>(file,p);
        if (kind!=1 && kind!=0x61000010) continue;
        const auto off=get<std::uint64_t>(file,p+8), va=get<std::uint64_t>(file,p+16);
        const auto files=get<std::uint64_t>(file,p+32), memory=get<std::uint64_t>(file,p+40);
        require(files<=memory && off<=file.size() && files<=file.size()-off &&
                va<512u*1024*1024 && memory<512u*1024*1024-va,"ELF segment");
        image.resize(std::max<std::size_t>(image.size(),va+memory));
        std::memcpy(image.data()+va,file.data()+off,files);
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(image.data(),CC_LONG(image.size()),digest);
    char hex[65]{}; for (int i=0;i<32;++i) std::snprintf(hex+i*2,3,"%02x",digest[i]);
    require(std::string(hex)=="071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a","unsupported ELF fixture");
    return image;
}
static void check_patch_set(const std::string& path, int expected) {
    FILE* f=std::fopen(path.c_str(),"rb"); require(f!=nullptr,"patch set missing");
    struct Close { FILE* f; ~Close(){std::fclose(f);} } close{f};
    char magic[8]; std::uint64_t base=0, count=0;
    require(std::fread(magic,1,8,f)==8 && !std::memcmp(magic,"BBPATCH2",8) &&
            std::fread(&base,8,1,f)==1 && base==0x400000 &&
            std::fread(&count,8,1,f)==1,"invalid patch fixture header");
    const auto entries=std::ftell(f);
    require(bb_fps_validate_patches(f,count)==expected,"unexpected actual patch-set admission");
    require(std::ftell(f)==entries,"actual patch validation consumed the stream");
}
int main(int argc,char** argv) {
    try {
        const char* elf=nullptr; int cap=60; std::uint64_t mutation=0;
        std::vector<std::pair<std::string,int>> patch_sets;
        for (int i=1;i<argc;++i) {
            if (!std::strcmp(argv[i],"--elf") && i+1<argc) elf=argv[++i];
            else if (!std::strcmp(argv[i],"--cap") && i+1<argc) cap=std::stoi(argv[++i]);
            else if (!std::strcmp(argv[i],"--mutate") && i+1<argc) mutation=std::stoull(argv[++i],nullptr,0);
            else if (!std::strcmp(argv[i],"--allow-patch") && i+1<argc) patch_sets.emplace_back(argv[++i],0);
            else if (!std::strcmp(argv[i],"--reject-patch") && i+1<argc) patch_sets.emplace_back(argv[++i],2);
            else throw std::runtime_error("unknown fixture argument");
        }
        auto image=elf ? load(elf) : std::vector<unsigned char>(4096);
        if (mutation) { require(mutation<image.size(),"mutation outside image"); image[mutation]^=1; }
        const auto page=std::size_t(getpagesize()), rounded=(image.size()+page-1)&~(page-1);
        const auto size=rounded+bb_fps_tail_size();
        void* p=mmap(nullptr,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        require(p!=MAP_FAILED,"fixture mapping");
        struct Release { void* p; std::size_t size; ~Release(){munmap(p,size);} } release{p,size};
        auto* bytes=static_cast<unsigned char*>(p); std::memcpy(bytes,image.data(),image.size());
        const auto result=bb_fps_install(bytes,image.size(),cap);
        for (const auto& [path,expected] : patch_sets) check_patch_set(path,expected);
        if (!elf || mutation || cap==30) {
            require(result==(cap==30 ? 0 : -1),"expected refusal/unmodified 30 FPS");
            require(!std::memcmp(bytes,image.data(),image.size()),"refused install changed image");
            require(MacTimingVerify::OwnedRanges().empty(),"refused install acquired timing sites");
            std::puts("PASS: timing refusal/original 30 FPS changes no bytes"); return 0;
        }
        require(result==1,"installation refused");
        // REA shows that the legacy timed-wait RET bypasses the epilogue.
        // Adaptive must retain the complete original body, including its gaps.
        constexpr std::size_t condition=0x2483e80-0x400000;
        require(!std::memcmp(bytes+condition,image.data()+condition,208),"condition wait changed");
        // Retain the original camera defaults. This does not establish live
        // camera-speed equivalence; the full decompiled camera is not installed.
        constexpr std::size_t camera=0x183a4c3-0x400000;
        require(!std::memcmp(bytes+camera,image.data()+camera,0x183aa5f-0x183a4c3),"camera defaults changed");
        for (const auto [va,length] : {std::pair<std::size_t,std::size_t>{0x183dc97,2},
                {0x183dcfe,1}, {0x183ff29,71}, {0x1840799,82}}) {
            const auto offset=va-0x400000;
            require(!std::memcmp(bytes+offset,image.data()+offset,length),"camera parameter store changed");
        }
        // This is a VMOVSS wind load with an intentionally redirected rel32,
        // rather than the CALL written by the legacy Uncap XML revision.
        constexpr std::size_t wind=0x2715d71-0x400000;
        require(!std::memcmp(bytes+wind,image.data()+wind,4),"wind-load opcode changed");
        require(bb_fps_conflicts(0x2434770-0x400000,2077),"limiter body not protected");
        require(bb_fps_conflicts(0x1914c24-0x400000,8),"sprint timing not protected");
        require(bb_fps_conflicts(0x1914c24-0x400000,1),"redirected-load opcode not protected");
        require(!bb_fps_conflicts(0x128e0e3-0x400000,1),"unrelated patch rejected");
        require(bb_fps_conflicts(UINT64_MAX,8),"overflow admitted");
        // The production loader scans the complete set before applying its
        // first write, including a conflicting write after an unrelated one.
        for (bool conflict : {false,true}) {
            FILE* patch=std::tmpfile(); require(patch!=nullptr,"patch fixture");
            const std::uint64_t one=1, ordinary=0x1000, timing=0x2434770-0x400000;
            const unsigned char byte=0x90;
            std::fwrite(&ordinary,8,1,patch); std::fwrite(&one,8,1,patch); std::fwrite(&byte,1,1,patch);
            const auto offset=conflict ? timing : ordinary;
            std::fwrite(&offset,8,1,patch); std::fwrite(&one,8,1,patch); std::fwrite(&byte,1,1,patch);
            std::rewind(patch);
            require(bb_fps_validate_patches(patch,2)==(conflict ? 2 : 0),"patch set conflict missed");
            require(std::ftell(patch)==0,"patch validation consumed the stream");
            require(bb_fps_validate_patches(patch,3)!=0,"truncated patch set admitted");
            std::fclose(patch);
        }
        const float target=1.f/(cap ? cap : 240);
        std::int64_t ms=0; double expected_ms=0;
        for (int k=0;k<1000;++k) {
            const float elapsed=k%3==0 ? .2f : k%3==1 ? .020f : .001f;
            const float expected=std::min(std::max(elapsed,target),1.f/30);
            const auto step=MacTimingVerify::TickInstalled(elapsed);
            require(step.seconds==expected && step.wind_seconds==expected*.5f,"installed time-step drift");
            ms+=step.play_ms; expected_ms+=double(expected)*1000;
        }
        require(std::abs(double(ms)-expected_ms)<1.01,"installed play clock loses milliseconds");
        // These authored event-frame conversions must stay 1/30 at every rate.
        for (const auto va : {0x4caf1c0u,0x4d27284u})
            require(!std::memcmp(bytes+va-0x400000,image.data()+va-0x400000,4),"authored duration changed");
        require(!mprotect(bytes,rounded,PROT_READ|PROT_EXEC),"fixture protections");
        require(!bb_fps_protect(),"dynamic timing data could not be made writable");
        MacTimingVerify::TickInstalled(.025f); // exercises real writes after segment protection
        // Execute only the installed absolute jump into our host replacement.
        // No original game instructions or startup function run.
        SprjFlipper flipper{}; flipper.force_no_sleep=1;
        reinterpret_cast<void(*)(SprjFlipper*)>(bytes+0x2434770-0x400000)(&flipper);
        require(flipper.target_frame_seconds==target && flipper.force_no_sleep==0,
                "installed entry does not reach the SysV replacement");
        std::printf("PASS: private 1.09 image at %d FPS, %zu owned sites, 1001 variable frames\n",
                    cap,MacTimingVerify::OwnedRanges().size());
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
