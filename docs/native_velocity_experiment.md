# Native velocity reuse experiment

Extracted from PR #90 onto current upstream master. This branch contains only
native object velocity reuse; it does not add MetalFX Spatial or change the
upscaler defaults. It is disabled unless `BB_METAL_NATIVE_MOTION=1` is set.

The adapter recognizes exact original rigid/skinned velocity producers. Canonical
clear, consecutive camera matrices, signed scale, format, extent, frame provenance
and successful draw recording gate admission. Rejected texels retain the existing
camera/recompiler motion vectors. Unknown filter/dilation passes are not trusted.

Do not propose this as ready for merge until a matched gameplay comparison shows
less ghosting or smearing:

1. Use the same scene, camera path, settings, assets and copied save for both runs.
2. Change only `BB_METAL_NATIVE_MOTION=0` versus `BB_METAL_NATIVE_MOTION=1`.
3. Capture moving enemies or characters in screenshots or a short video; a
   stationary image does not validate temporal reconstruction.
4. Compare ghosting and smearing, confirm the native producers actually supplied
   vectors, and include any remaining artifacts and measured overhead.

No gameplay improvement is established by the extraction. The existing
`native-motion-test` policy fixture and `camera-motion-test` GPU fixture cover
admission/fallback mechanics, not visual benefit in play.

Ported from `evill33t/bloodborne_mac` at
`bbacb95c3338efcd627c0edaec05ff7534e555da`. No game shader binaries, saves or captures
are distributed.
