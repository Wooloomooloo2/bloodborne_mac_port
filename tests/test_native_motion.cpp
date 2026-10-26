// SPDX-License-Identifier: GPL-2.0-or-later
#include "bbport_native_motion.h"
#include "bbport_shader_fingerprint.h"
#include <cassert>
#include <limits>
#include <cstdio>
int main() {
    using namespace BbNativeMotion;
    assert(CanonicalClear({127.f/255,127.f/255,1,0}));
    assert(!CanonicalClear({.5f,.5f,1,0}));
    assert(!CanonicalClear({127.f/255,127.f/255,1,1}));
    std::array<float,2> uv{};
    assert(DecodeScale({4.444444656f,-2.5f},uv));
    assert(std::abs(uv[0]+.225f)<1e-6f && std::abs(uv[1]+.4f)<1e-6f);
    for (auto bad : {std::array<float,2>{0,-2.5f}, {4.4f,2.5f},
                     {std::numeric_limits<float>::quiet_NaN(),-2.5f}, {4.4f,-2048}})
        assert(!DecodeScale(bad,uv));
    const std::uint32_t no_end[]{1,2,3};
    assert(BbShader::Fingerprint(no_end)==0);
    const std::uint32_t end[]{0xbf810000,123};
    assert(BbShader::Fingerprint(end)==0x3063bcd15634b7dfull);
    assert(!Producer(0) && !Vertex(0));
    std::puts("Native motion: canonical clear, signed scale, invalid input and fingerprints passed");
}
