# bbport GPU library: shadPS4 video core + shader recompiler + Gnm/VideoOut
# drivers, adapted to the native_probe runtime through shims in shim/.
cmake_minimum_required(VERSION 3.24)
project(bbgpu LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
# Frame pointers keep the loader's thread dumps (watchdog, SIGUSR2 sampling) walkable.
add_compile_options(-fno-omit-frame-pointer)

# bbport: link-time optimization (BB_LTO, default on) and profile-guided optimization
# (BB_PGO: off, generate = instrumented build writing into BB_PGO_DIR, use = build with that
# profile). Covers bbgpu and the static libraries linked into it. No -march: the same build
# runs on the Steam Deck.
option(BB_LTO "Link-time optimization of bbgpu" ON)
set(BB_PGO "off" CACHE STRING "Profile-guided optimization: off, generate, use")
set(BB_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" CACHE PATH "Profile directory")
if (BB_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT bb_ipo_ok OUTPUT bb_ipo_error)
    if (bb_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
    else()
        message(WARNING "LTO unavailable: ${bb_ipo_error}")
    endif()
endif()
if (BB_PGO STREQUAL "generate")
    # Atomic counters: the GPU, recording, preparation and copy threads run the same code.
    add_compile_options(-fprofile-generate=${BB_PGO_DIR} -fprofile-update=atomic)
    add_link_options(-fprofile-generate=${BB_PGO_DIR})
    add_compile_definitions(BB_PGO_GENERATE)
elseif (BB_PGO STREQUAL "use" AND APPLE)
    # bbport (macOS): Apple clang takes .profdata profiles, not GCC's .gcda files.
    message(WARNING "BB_PGO=use is not supported on macOS yet; building without a profile")
elseif (BB_PGO STREQUAL "use")
    # Partial training keeps unprofiled code optimized normally; counters from concurrent
    # threads can be slightly inconsistent (correction). Functions changed since the profile
    # (or built differently for it, bbgpu_init) are compiled without it.
    add_compile_options(-fprofile-use=${BB_PGO_DIR} -fprofile-partial-training
                        -fprofile-correction -Wno-missing-profile -Wno-coverage-mismatch)
endif()

# bbport: the native arm64 GPU host (docs/DESIGN_NATIVE_GPU_PROCESS.md) builds this library for
# arm64 (scripts/macos/build_gpu_arm64.sh). Zydis and Xbyak only serve the x86-64 SRT-walker JIT
# in the shader recompiler, so they are x86-64 only.
if (CMAKE_OSX_ARCHITECTURES)
    set(BB_TARGET_ARCH "${CMAKE_OSX_ARCHITECTURES}")
else()
    set(BB_TARGET_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()
if (BB_TARGET_ARCH MATCHES "^(arm64|aarch64|ARM64)$")
    set(BB_ARM64 ON)
else()
    set(BB_ARM64 OFF)
endif()
message(STATUS "bbgpu target architecture: ${BB_TARGET_ARCH}")

find_package(Vulkan REQUIRED)
find_package(fmt REQUIRED)
find_package(Boost REQUIRED)
find_package(magic_enum REQUIRED)
find_package(tsl-robin-map REQUIRED)
if (NOT TARGET GPUOpen::VulkanMemoryAllocator)
    add_library(GPUOpen::VulkanMemoryAllocator INTERFACE IMPORTED)
    find_path(VMA_INC NAMES vk_mem_alloc.h)
    if (VMA_INC)
        target_include_directories(GPUOpen::VulkanMemoryAllocator INTERFACE "${VMA_INC}")
    endif()
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(XXHASH REQUIRED IMPORTED_TARGET libxxhash)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
if (NOT APPLE)
    pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
endif()
find_package(miniz REQUIRED)
set(BB_ZYDIS "")
if (NOT BB_ARM64)
    find_package(Zydis REQUIRED)
    set(BB_ZYDIS Zydis::Zydis)
endif()
find_program(GLSLANG_COMPILER NAMES glslang glslangValidator REQUIRED)

set(SIRIT_USE_SYSTEM_SPIRV_HEADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/sirit EXCLUDE_FROM_ALL)

# bbport: FSR 3.1 temporal upscaling / frame generation, native Vulkan (MIT, FireBurn/FSR-Vulkan).
set(FFX_VK_PORTABLE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FFX_VK_PORTABLE_BUILD_PROBE OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)

# Host shaders -> headers (same scheme as shadPS4's host_shaders/CMakeLists.txt).
set(SHADER_SRC ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/host_shaders)
set(SHADER_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/include)
set(SHADER_DIR ${SHADER_INCLUDE}/video_core/host_shaders)
set(SHADER_HEADERS "")
# FILENAME: relative to host_shaders/, or an absolute path (test kernels).
function(add_host_shader FILENAME SHADER_NAME)
    set(OUT ${SHADER_DIR}/${SHADER_NAME}.h)
    if (IS_ABSOLUTE ${FILENAME})
        set(SOURCE ${FILENAME})
    else()
        set(SOURCE ${SHADER_SRC}/${FILENAME})
    endif()
    string(TOUPPER ${SHADER_NAME} SYMBOL)
    set(DEFINES ${ARGN})
    list(TRANSFORM DEFINES PREPEND "-D")
    add_custom_command(OUTPUT ${OUT}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
        COMMAND ${GLSLANG_COMPILER} --target-env vulkan1.3 -o ${OUT} --vn ${SYMBOL} ${DEFINES} ${SOURCE}
        COMMAND ${CMAKE_COMMAND} "-DHEADER_FILE=${OUT}" -P ${SHADER_SRC}/AddShaderHeaderInclude.cmake
        DEPENDS ${SOURCE} VERBATIM)
    set(SHADER_HEADERS ${SHADER_HEADERS} ${OUT} PARENT_SCOPE)
endfunction()
add_host_shader(color_to_ms_depth.frag color_to_ms_depth_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_frag)
add_host_shader(ms_image_blit.frag ms_image_blit_msaa_frag SRC_MSAA=1)
add_host_shader(fault_buffer_process.comp fault_buffer_process_comp)
add_host_shader(fsr.comp fsr_easu_comp SAMPLE_EASU=1)
add_host_shader(fsr.comp fsr_rcas_comp SAMPLE_RCAS=1)
add_host_shader(camera_motion.comp camera_motion_comp)
add_host_shader(upscale_merge.comp upscale_merge_comp)
add_host_shader(upscale_reactive.comp upscale_reactive_comp)
add_host_shader(taa.comp taa_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_comp)
add_host_shader(taa_sharpen.comp taa_sharpen_ldr_comp LDR_OUTPUT=1)
add_host_shader(buffer_multi_copy.comp buffer_multi_copy_comp)
add_host_shader(depth_resample.frag depth_resample_frag)
add_host_shader(depth_resample.frag depth_stencil_resample_frag HAS_STENCIL=1)
add_host_shader(depth_resample.frag depth_stencil_bits_frag STENCIL_BITS=1)
add_host_shader(camera_motion.comp camera_motion_debug_comp DEBUG_OVERLAY=1)
foreach(BPP IN ITEMS 8 16 32 64 96 128)
    add_host_shader(tiling.comp tiling_micro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=0)
    add_host_shader(tiling.comp tiling_macro_${BPP}_comp BITS_PER_PIXEL=${BPP} IS_MACRO_TILED=1)
endforeach()
# Test kernels (host-kernel-test).
add_host_shader(${CMAKE_CURRENT_SOURCE_DIR}/../tests/shaders/host_kernel_sampler.comp host_kernel_sampler_comp)
add_custom_target(host_shaders DEPENDS ${SHADER_HEADERS})

file(GLOB_RECURSE VIDEO_CORE CONFIGURE_DEPENDS shadps4/video_core/*.cpp)
file(GLOB_RECURSE RECOMPILER CONFIGURE_DEPENDS shadps4/shader_recompiler/*.cpp)
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderdoc\\.cpp$")
# The Metal renderer (renderer_metal/) is added in the APPLE block below.
list(FILTER VIDEO_CORE EXCLUDE REGEX "renderer_metal/")
# Metal-native renderer (docs/superpowers/plans/2026-10-16-metal-native-roadmap.md): the milestone
# the renderer sources are converted to. Files of later milestones leave the build.
set(BB_METAL_NATIVE_STAGE 5 CACHE STRING "Metal-native renderer milestone the sources are converted to (5-7)")
if (BB_METAL_NATIVE_STAGE LESS 5)
    message(FATAL_ERROR "BB_METAL_NATIVE_STAGE below 5 is gone (M5 converted every renderer file)")
endif()
set(BB_UNCONVERTED_6 host_passes/fsr_pass)
set(BB_UNCONVERTED_7 vk_instance vk_platform)
foreach(stage 6 7)
    if (BB_METAL_NATIVE_STAGE LESS ${stage})
        foreach(source IN LISTS BB_UNCONVERTED_${stage})
            list(FILTER VIDEO_CORE EXCLUDE REGEX "renderer_vulkan/${source}\\.cpp$")
        endforeach()
    endif()
endforeach()
set(BB_DECODER "")
if (NOT BB_ARM64)
    set(BB_DECODER shadps4/common/decoder.cpp)
endif()
set(COMMON
    ${BB_DECODER} shadps4/common/error.cpp
    shadps4/common/io_file.cpp shadps4/common/native_clock.cpp shadps4/common/number_utils.cpp
    shadps4/common/path_util.cpp shadps4/common/rdtsc.cpp shadps4/common/spin_lock.cpp shadps4/common/recursive_lock.cpp
    shadps4/common/string_util.cpp shadps4/common/thread.cpp shadps4/common/signal_context.cpp
    shim/common/logging/log.cpp)
file(GLOB AVPLAYER CONFIGURE_DEPENDS shadps4/core/libraries/avplayer/*.cpp)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
file(GLOB SHIM_SOURCES CONFIGURE_DEPENDS shim/*.cpp shim/*/*.cpp)
add_library(bbgpu SHARED ${VIDEO_CORE} ${RECOMPILER} ${COMMON} ${SHIM_SOURCES}
    shadps4/core/libraries/gnmdriver/gnmdriver.cpp
    shadps4/core/libraries/videoout/driver.cpp shadps4/core/libraries/videoout/video_out.cpp
    shadps4/core/libraries/kernel/equeue.cpp ${AVPLAYER}
    shadps4/core/libraries/videodec/video_utils.cpp)
add_dependencies(bbgpu host_shaders)
# In-game settings menu: Dear ImGui (third_party/imgui, MIT) with its Metal backend (APPLE block
# below), and an embedded Cyrillic font (third_party/fonts).
target_sources(bbgpu PRIVATE third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp
    third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp)
# PUBLIC: the gated headers must see the same stage in the tests that include them.
target_compile_definitions(bbgpu PUBLIC BB_METAL_NATIVE_STAGE=${BB_METAL_NATIVE_STAGE})
target_include_directories(bbgpu PRIVATE third_party/imgui third_party/imgui/backends)
target_compile_definitions(bbgpu PRIVATE
    BB_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf")
set_source_files_properties(shim/bbport_overlay.cpp PROPERTIES
    OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/DejaVuSans.ttf)
target_include_directories(bbgpu PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include third_party/half/include ..)
target_compile_definitions(bbgpu PRIVATE BOOST_ASIO_STANDALONE NDEBUG)
target_compile_options(bbgpu PRIVATE -Wno-deprecated-declarations -Wno-missing-field-initializers)
if (APPLE)
    # bbport: one-frame Metal GPU capture (shim/bbport_gpu_capture.h).
    enable_language(OBJCXX)
    target_sources(bbgpu PRIVATE shim/bbport_gpu_capture.mm)
    # MetalFX temporal upscaling on native textures (shim/bbport_metalfx.h), used by the upscaler.
    target_sources(bbgpu PRIVATE shim/bbport_metalfx.mm)
    # The settings overlay's Metal side and ImGui's Metal backend (both ARC).
    target_sources(bbgpu PRIVATE shim/bbport_overlay_metal.mm
                   third_party/imgui/backends/imgui_impl_metal.mm)
    set_source_files_properties(shim/bbport_gpu_capture.mm shim/bbport_metalfx.mm
                                shim/bbport_overlay_metal.mm
                                third_party/imgui/backends/imgui_impl_metal.mm
                                PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_definitions(bbgpu PRIVATE VK_USE_PLATFORM_METAL_EXT)
    # The runtime's symbols resolve against the executable at load time (bb-probe, src/runtime*.c;
    # bbgpu-host, host/host_runtime.c); anything else left undefined is a link error (ld's
    # default: it warns that an explicit -undefined error is deprecated).
    set(BB_HOST_RUNTIME_SYMBOLS
        runtime_disabled_optimizations runtime_errno runtime_fault_recover
        runtime_file_translate runtime_game_cpu_since_last runtime_memory_clamp
        runtime_memory_gpu_protect runtime_memory_read_backing runtime_memory_region
        runtime_memory_set_gpu_hooks runtime_memory_write_backing
        runtime_process_time_counter runtime_process_time_us runtime_restart
        runtime_thread_attach_host runtime_thread_clear_blocked runtime_thread_cpu_ns
        runtime_thread_set_blocked runtime_tsc_frequency)
    foreach(symbol IN LISTS BB_HOST_RUNTIME_SYMBOLS)
        target_link_options(bbgpu PRIVATE -Wl,-U,_${symbol})
    endforeach()
    # Classic lazy binding, as with -undefined dynamic_lookup before: a runtime function binds at
    # its first call, so test programs that define only the ones they use still load (chained
    # fixups bind every symbol at load time).
    target_link_options(bbgpu PRIVATE -Wl,-no_fixup_chains)
    # The Metal renderer: SPIR-V -> MSL through SPIRV-Cross (deps/<arch>, bootstrap_deps.sh). The
    # x86-64 library compiles it too (split mode never runs it there) until M7 trims that library
    # to the remote shim.
    find_package(spirv_cross_core CONFIG REQUIRED)
    find_package(spirv_cross_glsl CONFIG REQUIRED)
    find_package(spirv_cross_msl CONFIG REQUIRED)
    # Metal-native core (docs/superpowers/plans/2026-10-16-metal-native-m2-scheduler-caches.md):
    # device, encoder and helpers with no renderer dependencies; unit tests link it alone.
    set(METAL_CORE_SOURCES
        shadps4/video_core/renderer_metal/mtl_object.cpp
        shadps4/video_core/renderer_metal/mtl_buffer.mm
        shadps4/video_core/renderer_metal/mtl_compiled_shader.mm
        shadps4/video_core/renderer_metal/mtl_counters.mm
        shadps4/video_core/renderer_metal/mtl_device.mm
        shadps4/video_core/renderer_metal/mtl_draw_core.mm
        shadps4/video_core/renderer_metal/mtl_encoder.mm
        shadps4/video_core/renderer_metal/mtl_depth_stencil.mm
        shadps4/video_core/renderer_metal/mtl_present.mm
        shadps4/video_core/renderer_metal/mtl_render_pipeline.mm
        shadps4/video_core/renderer_metal/mtl_scale.mm
        shadps4/video_core/renderer_metal/mtl_specialize.cpp
        shadps4/video_core/renderer_metal/mtl_texture.mm
        shadps4/video_core/renderer_metal/mtl_timeline.cpp
        shadps4/video_core/renderer_metal/vk_to_mtl.mm)
    add_library(bbmetal_core STATIC ${METAL_CORE_SOURCES})
    target_include_directories(bbmetal_core PRIVATE shim shadps4 ${SHADER_INCLUDE})
    target_compile_features(bbmetal_core PRIVATE cxx_std_20)
    target_link_libraries(bbmetal_core PRIVATE fmt::fmt)
    set_target_properties(bbmetal_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    foreach(source IN LISTS METAL_CORE_SOURCES)
        if (source MATCHES "\\.mm$")
            set_source_files_properties(${source} TARGET_DIRECTORY bbmetal_core PROPERTIES
                COMPILE_OPTIONS "-fobjc-arc;-UVK_USE_PLATFORM_METAL_EXT;-Wno-unguarded-availability-new")
        endif()
    endforeach()
    file(GLOB METAL_SOURCES CONFIGURE_DEPENDS shadps4/video_core/renderer_metal/*.cpp
         shadps4/video_core/renderer_metal/*.mm)
    list(REMOVE_ITEM METAL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_object.cpp ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_buffer.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_compiled_shader.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_counters.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_device.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_draw_core.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_encoder.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_depth_stencil.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_present.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_render_pipeline.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_scale.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_specialize.cpp ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_texture.mm ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/mtl_timeline.cpp ${CMAKE_CURRENT_SOURCE_DIR}/shadps4/video_core/renderer_metal/vk_to_mtl.mm)
    target_sources(bbgpu PRIVATE ${METAL_SOURCES})
    target_link_libraries(bbgpu PRIVATE bbmetal_core "-framework Metal" "-framework QuartzCore"
        "-framework CoreGraphics" "-framework ImageIO" "-framework Foundation")
    foreach(source IN LISTS METAL_SOURCES)
        if (source MATCHES "\\.mm$")
            # No Vulkan Metal-surface types here: under ARC their CAMetalLayer pointer breaks
            # vulkan.hpp's constexpr templates.
            set_source_files_properties(${source} PROPERTIES COMPILE_OPTIONS
                "-fobjc-arc;-UVK_USE_PLATFORM_METAL_EXT;-Wno-unguarded-availability-new")
        endif()
    endforeach()
    target_link_libraries(bbgpu PRIVATE spirv-cross-msl spirv-cross-glsl spirv-cross-core)
    target_compile_definitions(bbgpu PRIVATE BB_METAL_BACKEND=1)
else()
    # Runtime symbols (runtime_memory_*, runtime_process_*) resolve against bb-probe.
    target_link_options(bbgpu PRIVATE -Wl,--allow-shlib-undefined)
endif()
if (NOT BB_METAL_NATIVE_STAGE LESS 6)
    # FSR 3.1 on Vulkan: not below stage 6 (M6 brings it back through ffx_metal); FSR 4 is gone.
    target_link_libraries(bbgpu PRIVATE ffx-vulkan::portable)
endif()
if (APPLE)
    target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
        tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::FFMPEG miniz::miniz ${BB_ZYDIS} sirit
        "-framework Metal" "-framework MetalFX" "-framework QuartzCore" "-framework Foundation")
else()
    target_link_libraries(bbgpu PUBLIC Vulkan::Headers fmt::fmt Boost::headers magic_enum::magic_enum
        tsl::robin_map GPUOpen::VulkanMemoryAllocator PkgConfig::XXHASH PkgConfig::SDL3 PkgConfig::X11 PkgConfig::FFMPEG miniz::miniz Zydis::Zydis sirit)
endif()

# Explicit test targets; neither launches the game nor needs a Vulkan device.
add_executable(motion-history-test EXCLUDE_FROM_ALL ../tests/test_motion_history.cpp)
target_compile_options(motion-history-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(motion-history-test PRIVATE ..)
add_executable(motion-shader-test EXCLUDE_FROM_ALL ../tests/test_motion_shaders.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(motion-shader-test PRIVATE shim shadps4 third_party/gcn/include)
target_link_libraries(motion-shader-test PRIVATE bbgpu)
add_executable(ui-composition-test EXCLUDE_FROM_ALL ../tests/test_ui_composition.cpp)
target_compile_options(ui-composition-test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
target_include_directories(ui-composition-test PRIVATE ..)

add_executable(scene-resolution-test EXCLUDE_FROM_ALL ../tests/test_scene_resolution.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(scene-resolution-test PRIVATE shim shadps4 third_party/gcn/include)
# The test's Vulkan-Hpp inline methods have a different set of platform extensions
# from vk_platform.cpp. Keep them local and use an explicit dispatcher in the test.
target_compile_options(scene-resolution-test PRIVATE -UNDEBUG -fPIC)
set_source_files_properties(../tests/test_scene_resolution.cpp PROPERTIES
    COMPILE_OPTIONS "-fvisibility=hidden;-fvisibility-inlines-hidden")
target_link_libraries(scene-resolution-test PRIVATE bbgpu)
# The library finds the runtime stand-ins (tests/test_gpu_runtime_stubs.cpp) in the executable:
# keep LTO from internalizing them.
target_link_options(scene-resolution-test PRIVATE -Wl,-export_dynamic)
target_link_options(motion-shader-test PRIVATE -Wl,-export_dynamic)
if (APPLE)
    # Metal::BuildHostKernel (SPIR-V -> MSL through the renderer's ShaderCompiler, in libbbgpu):
    # sampler slots and a linear sample through a combined image sampler.
    add_executable(host-kernel-test EXCLUDE_FROM_ALL ../tests/test_mtl_host_kernel.mm ../tests/test_gpu_runtime_stubs.cpp)
    target_include_directories(host-kernel-test PRIVATE shim shadps4 ${SHADER_INCLUDE})
    target_compile_options(host-kernel-test PRIVATE -UNDEBUG)
    set_source_files_properties(../tests/test_mtl_host_kernel.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(host-kernel-test PRIVATE bbgpu "-framework Metal" "-framework Foundation")
    target_link_options(host-kernel-test PRIVATE -Wl,-export_dynamic)
    add_dependencies(host-kernel-test host_shaders)
    # The settings overlay drawn through ImGui's Metal backend into an offscreen BGRA8 target.
    add_executable(overlay-test EXCLUDE_FROM_ALL ../tests/test_mtl_overlay.mm ../tests/test_gpu_runtime_stubs.cpp)
    target_include_directories(overlay-test PRIVATE shim shadps4)
    target_compile_options(overlay-test PRIVATE -UNDEBUG)
    set_source_files_properties(../tests/test_mtl_overlay.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(overlay-test PRIVATE bbgpu "-framework Metal" "-framework Foundation")
    target_link_options(overlay-test PRIVATE -Wl,-export_dynamic)
endif()
# These two run the TAA and camera motion kernels on Metal, recorded by the renderer's own
# Taa::Record and CameraMotion::Record (M5 Tasks 7 and 5).
add_executable(taa-shader-test EXCLUDE_FROM_ALL ../tests/test_taa_shader.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(taa-shader-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(taa-shader-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(taa-shader-test PRIVATE bbgpu)
target_link_options(taa-shader-test PRIVATE -Wl,-export_dynamic)
add_dependencies(taa-shader-test host_shaders)
add_executable(camera-motion-test EXCLUDE_FROM_ALL ../tests/test_camera_motion.cpp ../tests/test_gpu_runtime_stubs.cpp)
target_include_directories(camera-motion-test PRIVATE shim shadps4 ${SHADER_INCLUDE} third_party/gcn/include)
target_compile_options(camera-motion-test PRIVATE -UNDEBUG -fPIC)
target_link_libraries(camera-motion-test PRIVATE bbgpu)
target_link_options(camera-motion-test PRIVATE -Wl,-export_dynamic)
add_dependencies(camera-motion-test host_shaders)

# The depth attachment rule shared by the render pass and the pipeline key (header only).
add_executable(depth-attachment-test EXCLUDE_FROM_ALL ../tests/test_depth_attachment.cpp)
target_include_directories(depth-attachment-test PRIVATE shim shadps4)
target_compile_options(depth-attachment-test PRIVATE -UNDEBUG)
target_link_libraries(depth-attachment-test PRIVATE fmt::fmt)

add_executable(upscaler-support-test EXCLUDE_FROM_ALL ../tests/test_upscaler_support.cpp
    shim/bbport_settings.cpp)
target_include_directories(upscaler-support-test PRIVATE shim)
target_compile_options(upscaler-support-test PRIVATE -UNDEBUG)


# bbport online module (gpu/bbnet, libbbnet.dylib / libbbnet.so): built when its libraries are
# there (macOS: deps/x86_64 from scripts/macos/bootstrap_deps.sh; Linux: shell.nix); BB_NET=OFF
# leaves it out. Without it the port plays offline. x86-64 only: it runs in the game process.
option(BB_NET "Build the online module (shadNet)" ON)
if (BB_NET AND NOT BB_ARM64)
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    find_package(Protobuf CONFIG QUIET)
    find_package(nlohmann_json CONFIG QUIET)
    find_package(OpenSSL QUIET)
    find_package(ZLIB QUIET)
    find_path(MINIUPNPC_INCLUDE miniupnpc.h PATH_SUFFIXES miniupnpc)
    find_library(MINIUPNPC_LIBRARY NAMES miniupnpc)
    set(bb_net_platform_ok TRUE)
    if (APPLE)
        find_package(epoll-shim CONFIG QUIET)
        if (NOT epoll-shim_FOUND)
            set(bb_net_platform_ok FALSE)
        endif()
    endif()
    if (Protobuf_FOUND AND nlohmann_json_FOUND AND OpenSSL_FOUND AND ZLIB_FOUND AND
        MINIUPNPC_INCLUDE AND MINIUPNPC_LIBRARY AND bb_net_platform_ok)
        add_subdirectory(bbnet)
        message(STATUS "Online module: on (libbbnet)")
    else()
        message(STATUS "Online module: off (protobuf, nlohmann_json, OpenSSL, zlib, miniupnpc or epoll-shim missing)")
    endif()
endif()
