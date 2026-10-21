# Missing geometry: large Metal index offsets

On an M2 Max, native indexed draws with an offset above 4 GiB in the guest arena read from the wrapped low offset. The source mesh, uploaded bytes and draw submission were correct. Streaming/reallocation moved large mesh index buffers past this boundary, while small detail meshes already used the stream buffer. Reloading changed the allocation and could hide the fault.

`ObtainIndexBuffer` preserves the existing path for ranges below 4 GiB. High or boundary-crossing ranges use the existing fence-managed staging pool: CPU-owned data is uploaded; GPU-owned indices are copied in command order, without reading a stale CPU mirror.

A live missing prop reappeared by changing only its native index binding, without reloading. After the fix, about ten trips between areas kept walls, stairs and props visible on a Mac Studio M2 Max, 64 GB RAM, at 3840×1080: still 60 FPS, with no observed drop. Related reports: [#18](https://github.com/Wooloomooloo2/bloodborne_mac_port/issues/18) and [#35](https://github.com/Wooloomooloo2/bloodborne_mac_port/issues/35). This does not establish a fix for the separate black loading screen in #18.

## Opt-in regression test

After building the arm64 GPU library:

```sh
cmake --build out/gpu-arm64 --target index-offset-test
out/gpu-arm64/index-offset-test
MTL_DEBUG_LAYER=1 out/gpu-arm64/index-offset-test
```

Uses synthetic data only. Checks actual rasterized triangles with high CPU-owned and GPU-produced indices (including a deliberately stale CPU mirror), and preserves low-offset bindings. The original high-offset result is diagnostic, since other devices may handle it correctly. The target is excluded from normal builds; no new in-game diagnostics are enabled.

## Separate model-detail finding

`Model detail: Lower` (`model_lod=1`) caused permanent wall holes in both the older Vulkan port and the Metal port. Switching only this setting to `Game default` (`model_lod=0`) restored those walls without an observed FPS drop. Keep the existing upstream default of 0; users importing old settings should select Game default. This is independent of the streaming/index-offset fix.
