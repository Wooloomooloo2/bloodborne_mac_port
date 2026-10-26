# bbport online module: libbbnet (.so on Linux, .dylib on macOS), the PS4 network and PSN
# libraries with the shadNet client, loaded by the runtime only for online play
# (src/runtime_net.c). shadps4/ is shadPS4 v0.20.0's (529ceb32, GPL-2.0) src/core/libraries/net,
# network and np, src/core/net (the P2P transport: one UDP socket, FramingCodec, P2P crypto,
# the TCP-over-UDP streams) and src/shadnet, unchanged except where marked "bbport"; its
# host networking has a kqueue path, so macOS needs no epoll shim. Its headers (include/)
# extend the port's shim for these libraries alone; everything in it is hidden and bound to
# itself except bbnet.h's two calls, so its copies of shadPS4's kernel shims and symbol table
# never meet libbbgpu's (Linux: -Bsymbolic and --exclude-libs; macOS: an export list, and
# ld64's two-level namespace already binds a library's own symbols to itself).
# Needs protobuf, nlohmann_json, OpenSSL, zlib and miniupnpc (shell.nix; macOS: deps/x86_64 from
# scripts/macos/bootstrap_deps.sh); without them the port builds without it and plays offline
# (gpu/CMakeLists.txt: BB_NET).

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
file(GLOB_RECURSE BBNET_SOURCES CONFIGURE_DEPENDS shadps4/core/libraries/net/*.cpp
    shadps4/core/libraries/network/*.cpp shadps4/core/libraries/np/*.cpp shadps4/core/net/*.cpp
    shadps4/shadnet/*.cpp)
# Trophies and the system dialogs stay in the C runtime; their shadPS4 versions need the
# emulator's UI layers.
list(FILTER BBNET_SOURCES EXCLUDE REGEX
    "np_trophy|trophy_ui|np_sns_facebook_dialog|np_profile_dialog/|np_commerce/")

# The Hunter's Dream backend (thd/, from droogie's bbhost through the Windows port, GPL-3.0):
# BB_NET_BACKEND=thd puts its NP and sceNet code in front of shadPS4's.
file(GLOB BBNET_THD_SOURCES CONFIGURE_DEPENDS thd/*.cpp)
add_library(bbnet SHARED ${BBNET_SOURCES} ${BBNET_THD_SOURCES} "${SHADNET_PROTO_OUT}/shadnet.pb.cc"
    bbnet_glue.cpp)
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
    # miniupnpc is a static library. httplib takes the HTTPS anchors from the keychain
    # (Security.framework) on macOS.
    target_link_libraries(bbnet PRIVATE "-framework Security" "-framework CoreFoundation")
    target_compile_definitions(bbnet PRIVATE MINIUPNP_STATICLIB)
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

# The P2P wire format, byte for byte (tests/test_p2p_codec.cpp; build_macos.sh --test). The
# codec sources are compiled again here: the module exports nothing but bbnet.h's two calls.
add_executable(bbnet-codec-test EXCLUDE_FROM_ALL ../../tests/test_p2p_codec.cpp
    shadps4/core/net/p2p_codec.cpp shadps4/core/net/p2p_crypto.cpp shadps4/core/net/p2p_tcp.cpp
    shadps4/core/net/p2p_transport.cpp shadps4/core/net/host_net.cpp)
target_include_directories(bbnet-codec-test PRIVATE shadps4 ../shadps4)
target_link_libraries(bbnet-codec-test PRIVATE OpenSSL::Crypto)
set_target_properties(bbnet-codec-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    INTERPROCEDURAL_OPTIMIZATION OFF)

# Loopback HTTP regression coverage; no guest code or public service is contacted.
add_executable(bbnet-session-test EXCLUDE_FROM_ALL ../../tests/test_thd_session.cpp
    thd/thd_session.cpp)
target_include_directories(bbnet-session-test PRIVATE . ../third_party/httplib ../shadps4)
target_compile_options(bbnet-session-test PRIVATE -UNDEBUG)
target_compile_definitions(bbnet-session-test PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT CPPHTTPLIB_ZLIB_SUPPORT)
target_link_libraries(bbnet-session-test PRIVATE nlohmann_json::nlohmann_json OpenSSL::SSL
    OpenSSL::Crypto ZLIB::ZLIB)
if (APPLE)
    target_link_libraries(bbnet-session-test PRIVATE "-framework Security" "-framework CoreFoundation")
endif()
set_target_properties(bbnet-session-test PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    INTERPROCEDURAL_OPTIMIZATION OFF)
