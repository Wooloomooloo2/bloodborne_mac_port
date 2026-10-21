# FSR 3 golden outputs (synthetic frames)

Reference outputs for `Fsr3MatchesVulkan` (`tests/test_ffx_metal.mm`): what the Vulkan path the
game used before the Metal renderer produces from the synthetic frames of `tests/fsr3_synthetic.h`.
Synthetic data, no game content.

- Producer: `tests/fsr3_golden_vk.cpp` (CMake target `fsr3-golden-vk`; removed with MoltenVK in
  0.5.3, see the `mac-v0.5.2.2` tag), through FSR-Vulkan's
  `ffx-vulkan::portable` (submodule `gpu/third_party/fsr-vulkan` at c64f093, patched per its
  `UPSTREAM.md`): AMD FidelityFX SDK 1.1.4, FSR 3.1.4 upscaler, AMD's `ffx_vk.cpp` back end and
  SPIR-V blobs, on MoltenVK 1.4.2 (arm64), Apple M4 Max.
- Blob set: **fp16** (the `_16bit` permutations: the device enables `shaderFloat16`), wave32 (no
  subgroup size control), no Lanczos LUT. The Metal back end's default (`BB_FFX_FP16` unset) picks
  the same blobs; run the test with that default.
- Command line (from the repository root, at `mac-v0.5.2.2`, with MoltenVK in deps/arm64):

      cmake -S gpu -B out/gpu-arm64 -DBB_FSR3_GOLDEN=ON
      ninja -C out/gpu-arm64 fsr3-golden-vk
      out/gpu-arm64/fsr3-golden-vk tests/data/fsr3
      cmake -S gpu -B out/gpu-arm64 -DBB_FSR3_GOLDEN=OFF

  Two runs give identical files.
- Files: `<case>_f<frame>.bin`, the output texture after frame 4 and frame 7 (of 8, resets at
  frames 0 and 5), raw RGBA16F, row-major, no header.

| Case | Render | Output | Flags | Sharpening |
|---|---|---|---|---|
| `quality_hdr` | 256×144 | 384×216 | HDR, auto exposure | on, 0.3 |
| `odd_up` | 171×96 | 512×288 | none | off |

Exposure, reactive and transparency/composition masks are null; dispatch constants (frame time,
camera, jitter, motion) are in `tests/fsr3_synthetic.h`.
