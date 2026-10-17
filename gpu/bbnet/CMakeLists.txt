# bbport online module: libbbnet (.so on Linux, .dylib on macOS), the PS4 network and PSN
# libraries with the shadNet client (shadp2p, a shadPS4 fork, GPL-2.0), loaded by the runtime
# only for online play (src/runtime_net.c). Its headers (include/) extend the port's shim for
# these libraries alone; everything in it is hidden and bound to itself except bbnet.h's two
# calls, so its copies of shadPS4's kernel shims and symbol table never meet libbbgpu's
# (Linux: -Bsymbolic and --exclude-libs; macOS: an export list, and ld64's two-level namespace
# already binds a library's own symbols to itself).
# Needs protobuf, nlohmann_json, OpenSSL, zlib and miniupnpc (shell.nix; macOS: deps/x86_64 from
# scripts/macos/bootstrap_deps.sh, plus epoll-shim for epoll and eventfd on kqueue); without
# them the port builds without it and plays offline (gpu/CMakeLists.txt: BB_NET).

set(SHADNET_PROTO_OUT "${CMAKE_CURRENT_BINARY_DIR}/shadnet_proto_gen")
file(MAKE_DIRECTORY "${SHADNET_PROTO_OUT}")
# BB_PROTOC: a protoc for the build machine (macOS: deps/host/bin/protoc, arm64) instead of the
# target architecture's one (which runs under Rosetta).
if (BB_PROTOC)
    set(BBNET_PROTOC "${BB_PROTOC}")
else()
    set(BBNET_PROTOC protobuf::protoc)
endif()
add_custom_command(
    OUTPUT "${SHADNET_PROTO_OUT}/shadnet.pb.cc" "${SHADNET_PROTO_OUT}/shadnet.pb.h"
    COMMAND "${BBNET_PROTOC}" "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet"
            "--cpp_out=${SHADNET_PROTO_OUT}" "${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet/shadnet.proto"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shadps4/shadnet/shadnet.proto")
file(GLOB_RECURSE BBNET_SOURCES CONFIGURE_DEPENDS shadps4/core/libraries/network/*.cpp
    shadps4/core/libraries/np/*.cpp shadps4/shadnet/*.cpp)
# Trophies and the system dialogs stay in the C runtime; their shadPS4 versions need the
# emulator's UI layers.
list(FILTER BBNET_SOURCES EXCLUDE REGEX
    "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")

add_library(bbnet SHARED ${BBNET_SOURCES} "${SHADNET_PROTO_OUT}/shadnet.pb.cc" bbnet_glue.cpp)
# The module's own headers first, then the port's shim and shadPS4 (common/, logging).
target_include_directories(bbnet PRIVATE include shadps4 shadps4/shadnet "${SHADNET_PROTO_OUT}"
    ${CMAKE_CURRENT_SOURCE_DIR} ../shim ../shadps4 ../third_party/imgui ../third_party/httplib)
target_compile_definitions(bbnet PRIVATE BOOST_ASIO_STANDALONE NDEBUG
    CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT)
target_compile_options(bbnet PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden
    -Wno-deprecated-declarations -Wno-missing-field-initializers)
# Generated protobuf code: not held to the port's warning policy.
set_source_files_properties("${SHADNET_PROTO_OUT}/shadnet.pb.cc" PROPERTIES COMPILE_OPTIONS "-w")
if (APPLE)
    # ld64: only bbnet_init and bbnet_resolve leave the library (bbnet.exports). The module is
    # dlopen'ed from out/gpu beside libbbgpu.dylib (@rpath/libbbgpu.dylib: @loader_path finds it).
    target_link_options(bbnet PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/bbnet.exports")
    set_property(TARGET bbnet APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bbnet.exports")
    set_target_properties(bbnet PROPERTIES BUILD_RPATH "@loader_path" INSTALL_RPATH "@loader_path"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}" # beside libbbgpu.dylib, where the runtime looks
        INTERPROCEDURAL_OPTIMIZATION OFF) # third-party network code: LTO buys nothing here
    # epoll, eventfd (sys/epoll.h, sys/eventfd.h) on kqueue; miniupnpc is a static library.
    # httplib takes the HTTPS anchors from the keychain (Security.framework) on macOS.
    target_link_libraries(bbnet PRIVATE epoll-shim::epoll-shim "-framework Security" "-framework CoreFoundation")
    target_compile_definitions(bbnet PRIVATE MINIUPNP_STATICLIB EPOLL_SHIM_DISABLE_WRAPPER_MACROS)
else()
    target_link_options(bbnet PRIVATE -Wl,-Bsymbolic -Wl,--exclude-libs,ALL)
    set_target_properties(bbnet PROPERTIES BUILD_RPATH "\$ORIGIN" INSTALL_RPATH "\$ORIGIN"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}") # beside libbbgpu.so, where the runtime looks
endif()
# libbbgpu (already in the process) provides shadPS4's common code: logging, threads' names.
target_link_libraries(bbnet PRIVATE bbgpu nlohmann_json::nlohmann_json protobuf::libprotobuf
    OpenSSL::SSL OpenSSL::Crypto ZLIB::ZLIB "${MINIUPNPC_LIBRARY}" Vulkan::Headers fmt::fmt
    Boost::headers magic_enum::magic_enum)
target_include_directories(bbnet PRIVATE "${MINIUPNPC_INCLUDE}")
