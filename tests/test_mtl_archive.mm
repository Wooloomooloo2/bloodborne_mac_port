// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Metal-native M4: which pipeline configurations an MTLBinaryArchive can serialize.
//
// The renderer compiles every shader into its own MTLLibrary and adds each pipeline to one binary
// archive (PsoCache). With SPIRV-Cross' default entry name every function was `main0`, and
// serializing failed with "expecting 'fragment' stage" / "missing 'vertex' stage in pipeline
// no. N". Root cause (cases k, l): an archive that was serialized and then gets pipelines reusing
// functions it already holds (the same function, or an identical one from another library) fails
// every later serialization; the renderer wrote its archive every 32 pipelines. An archive
// serialized once, after all its additions, works: the renderer writes a fresh archive of every
// pipeline it recorded (cases m and m2, asserted). Each case builds a fresh archive, adds
// pipelines and serializes it to a temporary file. Cases the renderer relies on must PASS;
// configurations Metal refuses are printed as INFO (the renderer avoids them). Reloaded archives
// are checked with MTLPipelineOptionFailOnBinaryArchiveMiss to prove the pipelines come from them.
//
// Pure Metal: no renderer library.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <string>
#include <unistd.h>

namespace {

int g_failures = 0;

std::string ErrorText(NSError* error) {
    if (!error) {
        return "no error";
    }
    std::string text = error.localizedDescription.UTF8String;
    const size_t eol = text.find('\n');
    if (eol != std::string::npos) {
        text.resize(eol);
    }
    // The archiver's messages start with temporary paths; the reason is at the end.
    const size_t reason = text.rfind("': ");
    return reason == std::string::npos ? text : "..." + text.substr(reason + 1);
}

void Report(bool required, bool ok, const char* name, const std::string& detail) {
    if (ok) {
        std::printf("PASS: %s\n", name);
    } else if (required) {
        std::printf("FAIL: %s: %s\n", name, detail.c_str());
        ++g_failures;
    } else {
        std::printf("INFO: %s: Metal refuses it (%s)\n", name, detail.c_str());
    }
}

/// One library per function, as the renderer builds them.
id<MTLFunction> Function(id<MTLDevice> device, NSString* source, NSString* name) {
    MTLCompileOptions* options = [MTLCompileOptions new];
    options.languageVersion = MTLLanguageVersion3_1;
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    if (!library) {
        std::printf("FAIL: compiling %s: %s\n", name.UTF8String, ErrorText(error).c_str());
        ++g_failures;
        return nil;
    }
    id<MTLFunction> function = [library newFunctionWithName:name];
    if (!function) {
        std::printf("FAIL: function %s not found\n", name.UTF8String);
        ++g_failures;
    }
    return function;
}

NSString* VertexSource(NSString* name, float scale) {
    return [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
                                       "struct Out { float4 position [[position]]; float4 color [[user(locn0)]]; };\n"
                                       "vertex Out %@(uint vid [[vertex_id]]) {\n"
                                       "  Out o; o.position = float4(float(vid & 1) * %f, float(vid >> 1), 0, 1);\n"
                                       "  o.color = float4(%f); return o; }\n",
                                      name, scale, scale];
}

NSString* FragmentSource(NSString* name, float value) {
    return [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
                                       "struct In { float4 color [[user(locn0)]]; };\n"
                                       "struct FOut { float4 c0 [[color(0)]]; };\n"
                                       "fragment FOut %@(In in [[stage_in]]) {\n"
                                       "  FOut o; o.c0 = in.color * %f; return o; }\n",
                                      name, value];
}

MTLRenderPipelineDescriptor* ColorPipeline(id<MTLFunction> vs, id<MTLFunction> fs) {
    MTLRenderPipelineDescriptor* desc = [MTLRenderPipelineDescriptor new];
    desc.vertexFunction = vs;
    desc.fragmentFunction = fs;
    desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    return desc;
}

id<MTLBinaryArchive> NewArchive(id<MTLDevice> device, NSURL* url) {
    MTLBinaryArchiveDescriptor* descriptor = [MTLBinaryArchiveDescriptor new];
    descriptor.url = url;
    NSError* error = nil;
    id<MTLBinaryArchive> archive = [device newBinaryArchiveWithDescriptor:descriptor error:&error];
    if (!archive) {
        std::printf("FAIL: newBinaryArchiveWithDescriptor: %s\n", ErrorText(error).c_str());
        ++g_failures;
    }
    return archive;
}

NSURL* TempURL(const char* name) {
    NSString* path = [NSString stringWithFormat:@"%@bb-archive-test-%d-%s.binaryarchive",
                                                NSTemporaryDirectory(), getpid(), name];
    [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    return [NSURL fileURLWithPath:path];
}

/// Creates each pipeline (as the renderer does, with the archive in `binaryArchives`), adds it
/// to a fresh archive and serializes the archive. Returns the error text, empty on success.
std::string AddAndSerialize(id<MTLDevice> device, NSArray* descriptors, NSURL* url,
                            bool archive_in_descriptor, bool serialize_each = false) {
    id<MTLBinaryArchive> archive = NewArchive(device, nil);
    if (!archive) {
        return "no archive";
    }
    for (id desc in descriptors) {
        NSError* error = nil;
        if (archive_in_descriptor) {
            [desc setBinaryArchives:@[ archive ]];
        }
        if ([desc isKindOfClass:[MTLRenderPipelineDescriptor class]]) {
            if (![device newRenderPipelineStateWithDescriptor:desc error:&error]) {
                return "pipeline creation: " + ErrorText(error);
            }
            if (![archive addRenderPipelineFunctionsWithDescriptor:desc error:&error]) {
                return "add: " + ErrorText(error);
            }
        } else {
            if (![device newComputePipelineStateWithDescriptor:desc
                                                       options:MTLPipelineOptionNone
                                                    reflection:nil
                                                         error:&error]) {
                return "pipeline creation: " + ErrorText(error);
            }
            if (![archive addComputePipelineFunctionsWithDescriptor:desc error:&error]) {
                return "add: " + ErrorText(error);
            }
        }
        if (serialize_each && ![archive serializeToURL:url error:&error]) {
            return "serialize after " + std::to_string([descriptors indexOfObject:desc]) + ": " +
                   ErrorText(error);
        }
    }
    NSError* error = nil;
    if (![archive serializeToURL:url error:&error]) {
        return "serialize: " + ErrorText(error);
    }
    return {};
}

void Case(id<MTLDevice> device, bool required, const char* name, NSArray* descriptors,
          bool archive_in_descriptor = true, bool serialize_each = false) {
    NSURL* url = TempURL(name);
    const std::string error =
        AddAndSerialize(device, descriptors, url, archive_in_descriptor, serialize_each);
    Report(required, error.empty(), name, error);
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
}

/// The game's mix at the size where the renderer's archive failed (pipeline 24 of the stored
/// cache): colour pipelines, depth-only pipelines whose vertex function is shared with a colour
/// pipeline, and compute kernels; each function in its own library. `unique`: every function
/// has its own name (<stage>_<n>), else all are `main0`.
NSArray* GameMix(id<MTLDevice> device, bool unique) {
    NSMutableArray* descriptors = [NSMutableArray array];
    auto name = [&](const char* stage, int n) {
        return unique ? [NSString stringWithFormat:@"%s_%016x", stage, n] : @"main0";
    };
    for (int i = 0; i < 40; ++i) {
        id<MTLFunction> vs = Function(device, VertexSource(name("vertex", i), 1.0f + i), name("vertex", i));
        id<MTLFunction> fs =
            Function(device, FragmentSource(name("fragment", i), 1.0f + i), name("fragment", i));
        [descriptors addObject:ColorPipeline(vs, fs)];
        if (i % 8 == 3) {
            MTLRenderPipelineDescriptor* depth_only = [MTLRenderPipelineDescriptor new];
            depth_only.vertexFunction = vs;
            depth_only.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
            [descriptors addObject:depth_only];
        }
        if (i % 4 == 1) {
            NSString* cs_name = name("compute", i);
            id<MTLFunction> cs = Function(
                device,
                [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
                                            "kernel void %@(device float* b [[buffer(0)]], uint i "
                                            "[[thread_position_in_grid]]) { b[i] *= %d.0f; }\n",
                                           cs_name, i + 2],
                cs_name);
            MTLComputePipelineDescriptor* compute = [MTLComputePipelineDescriptor new];
            compute.computeFunction = cs;
            [descriptors addObject:compute];
        }
    }
    return descriptors;
}

/// What the renderer does per pipeline: `binaryArchives` = @[archive] (when `attach`), create
/// the pipeline state, add the descriptor to the archive.
std::string CreateAndAdd(id<MTLDevice> device, id<MTLBinaryArchive> archive, id desc, bool attach) {
    NSError* error = nil;
    [desc setBinaryArchives:attach ? @[ archive ] : @[]];
    if ([desc isKindOfClass:[MTLRenderPipelineDescriptor class]]) {
        if (![device newRenderPipelineStateWithDescriptor:desc error:&error]) {
            return "pipeline creation: " + ErrorText(error);
        }
        if (![archive addRenderPipelineFunctionsWithDescriptor:desc error:&error]) {
            return "add: " + ErrorText(error);
        }
    } else {
        if (![device newComputePipelineStateWithDescriptor:desc
                                                   options:MTLPipelineOptionNone
                                                reflection:nil
                                                     error:&error]) {
            return "pipeline creation: " + ErrorText(error);
        }
        if (![archive addComputePipelineFunctionsWithDescriptor:desc error:&error]) {
            return "add: " + ErrorText(error);
        }
    }
    return {};
}

/// Adds `first` to a fresh archive, serializes it, adds `second`, serializes again (the
/// renderer's periodic Flush). `fresh_second`: `second` goes to a new archive instead.
std::string SerializeBetween(id<MTLDevice> device, NSArray* first, NSArray* second,
                             bool fresh_second = false, bool attach = true) {
    NSURL* url = TempURL("between");
    id<MTLBinaryArchive> archive = NewArchive(device, nil);
    std::string error;
    NSError* ns_error = nil;
    for (id desc in first) {
        if (error.empty()) {
            error = CreateAndAdd(device, archive, desc, attach);
        }
    }
    if (error.empty() && ![archive serializeToURL:url error:&ns_error]) {
        error = "first write: " + ErrorText(ns_error);
    }
    if (error.empty() && fresh_second) {
        archive = NewArchive(device, nil);
    }
    for (id desc in second) {
        if (error.empty()) {
            error = CreateAndAdd(device, archive, desc, attach);
        }
    }
    if (error.empty() && ![archive serializeToURL:url error:&ns_error]) {
        error = "second write: " + ErrorText(ns_error);
    }
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    return error;
}

/// PsoCache::CreateWithArchive: from `lookup` (FailOnBinaryArchiveMiss) when given and it has the
/// pipeline (`hit`), else compiled; `binaryArchives` is cleared afterwards. Nil on failure.
id CreateState(id<MTLDevice> device, id desc, id<MTLBinaryArchive> lookup, bool& hit) {
    const bool render = [desc isKindOfClass:[MTLRenderPipelineDescriptor class]];
    auto create = [&](MTLPipelineOption options) -> id {
        NSError* error = nil;
        return render ? (id)[device newRenderPipelineStateWithDescriptor:desc
                                                                 options:options
                                                              reflection:nil
                                                                   error:&error]
                      : (id)[device newComputePipelineStateWithDescriptor:desc
                                                                  options:options
                                                               reflection:nil
                                                                    error:&error];
    };
    id state = nil;
    hit = false;
    if (lookup) {
        [desc setBinaryArchives:@[ lookup ]];
        state = create(MTLPipelineOptionFailOnBinaryArchiveMiss);
        hit = state != nil;
    }
    if (!state) {
        state = create(MTLPipelineOptionNone);
    }
    [desc setBinaryArchives:nil];
    return state;
}

/// PsoCache::WriteArchive: a fresh archive of every recorded descriptor, serialized once.
std::string WriteFresh(id<MTLDevice> device, NSArray* recorded, NSURL* url) {
    id<MTLBinaryArchive> fresh = NewArchive(device, nil);
    if (!fresh) {
        return "no archive";
    }
    for (id desc in recorded) {
        NSError* error = nil;
        const BOOL added = [desc isKindOfClass:[MTLRenderPipelineDescriptor class]]
                               ? [fresh addRenderPipelineFunctionsWithDescriptor:desc error:&error]
                               : [fresh addComputePipelineFunctionsWithDescriptor:desc error:&error];
        if (!added) {
            return "add: " + ErrorText(error);
        }
    }
    NSError* error = nil;
    if (![fresh serializeToURL:url error:&error]) {
        return "write of " + std::to_string(recorded.count) + ": " + ErrorText(error);
    }
    return {};
}

/// How many of `descriptors` the archive file at `url` has (FailOnBinaryArchiveMiss).
int CountHits(id<MTLDevice> device, NSArray* descriptors, NSURL* url) {
    id<MTLBinaryArchive> loaded = NewArchive(device, url);
    int hits = 0;
    for (id desc in descriptors) {
        bool hit = false;
        if (loaded && CreateState(device, desc, loaded, hit) && hit) {
            ++hits;
        }
    }
    return hits;
}

} // namespace

int main() {
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            std::printf("FAIL: no Metal device\n");
            return 1;
        }

        // (a) One vertex + fragment pair, unique names, one library each.
        id<MTLFunction> vs_a = Function(device, VertexSource(@"vs_a", 1.0f), @"vs_a");
        id<MTLFunction> fs_a = Function(device, FragmentSource(@"fs_a", 1.0f), @"fs_a");
        Case(device, true, "a: one VS+FS pair, unique names", @[ ColorPipeline(vs_a, fs_a) ]);

        // (b) Two pipelines from four libraries whose functions are all `main0` (SPIRV-Cross'
        // default entry name: the renderer's configuration before the fix).
        id<MTLFunction> vs_b1 = Function(device, VertexSource(@"main0", 1.0f), @"main0");
        id<MTLFunction> fs_b1 = Function(device, FragmentSource(@"main0", 1.0f), @"main0");
        id<MTLFunction> vs_b2 = Function(device, VertexSource(@"main0", 2.0f), @"main0");
        id<MTLFunction> fs_b2 = Function(device, FragmentSource(@"main0", 2.0f), @"main0");
        Case(device, false, "b: two pipelines, four libraries, all named main0",
             @[ ColorPipeline(vs_b1, fs_b1), ColorPipeline(vs_b2, fs_b2) ]);

        Case(device, false, "b2: the game's mix (40 colour, depth-only, compute), all main0",
             GameMix(device, false));

        // (c) As (b) with unique names: the renderer's configuration after the fix.
        id<MTLFunction> vs_c1 = Function(device, VertexSource(@"vertex_c1", 1.0f), @"vertex_c1");
        id<MTLFunction> fs_c1 = Function(device, FragmentSource(@"fragment_c1", 1.0f), @"fragment_c1");
        id<MTLFunction> vs_c2 = Function(device, VertexSource(@"vertex_c2", 2.0f), @"vertex_c2");
        id<MTLFunction> fs_c2 = Function(device, FragmentSource(@"fragment_c2", 2.0f), @"fragment_c2");
        Case(device, true, "c: two pipelines, four libraries, unique names",
             @[ ColorPipeline(vs_c1, fs_c1), ColorPipeline(vs_c2, fs_c2) ]);

        Case(device, true, "c2: the game's mix (40 colour, depth-only, compute), unique names",
             GameMix(device, true));

        // (i) Identical sources in separate libraries (the game has shaders whose guest hashes
        // differ but whose SPIR-V, and so MSL, is the same; each gets its own library).
        id<MTLFunction> vs_i1 = Function(device, VertexSource(@"main0", 3.0f), @"main0");
        id<MTLFunction> vs_i2 = Function(device, VertexSource(@"main0", 4.0f), @"main0");
        id<MTLFunction> fs_i1 = Function(device, FragmentSource(@"main0", 5.0f), @"main0");
        id<MTLFunction> fs_i2 = Function(device, FragmentSource(@"main0", 5.0f), @"main0");
        Case(device, false, "i1: identical fragment sources in two libraries, all main0",
             @[ ColorPipeline(vs_i1, fs_i1), ColorPipeline(vs_i2, fs_i2) ]);

        // (j) The renderer flushes the archive every few new pipelines: serialized after every
        // addition, the way the live archive is.
        Case(device, false, "j1: the game's mix serialized after every addition, all main0",
             GameMix(device, false), true, true);
        Case(device, true, "j2: the game's mix serialized after every addition, unique names",
             GameMix(device, true), true, true);

        // (k) The game's failure, minimised from its pipeline cache: two pipelines whose fragment
        // shaders have different guest hashes but the same SPIR-V, so the same MSL, each in its own
        // library; the archive is serialized after each addition (the renderer flushes every few
        // pipelines). With SPIRV-Cross' `main0` the two functions are identical and the second
        // serialization fails with "missing 'vertex' stage in pipeline no. N" (a tessellation
        // pipeline first, as in the game, is not needed). Named per shader (stage, guest hash), the
        // functions differ and this case serializes; reuse of one function remains (l).
        for (int unique = 0; unique < 2; ++unique) {
            auto nm = [&](NSString* n) { return unique ? n : @"main0"; };
            id<MTLFunction> vs1 = Function(device, VertexSource(nm(@"vertex_k1"), 1.0f), nm(@"vertex_k1"));
            id<MTLFunction> vs2 = Function(device, VertexSource(nm(@"vertex_k2"), 2.0f), nm(@"vertex_k2"));
            id<MTLFunction> fs1 = Function(device, FragmentSource(nm(@"fragment_k1"), 9.0f), nm(@"fragment_k1"));
            id<MTLFunction> fs2 = Function(device, FragmentSource(nm(@"fragment_k2"), 9.0f), nm(@"fragment_k2"));
            Case(device, unique,
                 unique ? "k2: same fragment source in two libraries, unique names, serialized after each addition"
                        : "k1: same fragment source in two libraries, all main0, serialized after each addition",
                 @[ ColorPipeline(vs1, fs1), ColorPipeline(vs2, fs2) ], true, true);
        }

        // (l) The root cause, unique names or not: once an archive was serialized, adding a
        // pipeline that reuses a function the archive already holds can break every later
        // serialization ("expecting 'fragment' stage" / "missing 'vertex' stage in pipeline no.
        // N"). Which reuse breaks depends on the functions (here and in the renderer it was the
        // vertex function in some runs, the fragment function in others), so l1-l3 are not
        // asserted (PASS or INFO). The game reuses shaders across many pipelines, so the renderer's
        // archive, written every 32 pipelines, failed. An archive serialized once, after all its
        // additions, works (l4, l5).
        {
            id<MTLFunction> vs = Function(device, VertexSource(@"vertex_l", 1.0f), @"vertex_l");
            id<MTLFunction> vs2 = Function(device, VertexSource(@"vertex_l2", 2.0f), @"vertex_l2");
            id<MTLFunction> fs = Function(device, FragmentSource(@"fragment_l", 1.0f), @"fragment_l");
            id<MTLFunction> fs2 = Function(device, FragmentSource(@"fragment_l2", 2.0f), @"fragment_l2");
            MTLRenderPipelineDescriptor* blended = ColorPipeline(vs, fs);
            blended.colorAttachments[0].blendingEnabled = YES;
            std::string error = SerializeBetween(device, @[ ColorPipeline(vs, fs) ], @[ ColorPipeline(vs, fs2) ]);
            Report(false, error.empty(), "l1: vertex function reused after a serialization", error);
            error = SerializeBetween(device, @[ ColorPipeline(vs, fs) ], @[ ColorPipeline(vs, fs2) ], false, false);
            Report(false, error.empty(), "l1b: as l1, binaryArchives empty", error);
            error = SerializeBetween(device, @[ ColorPipeline(vs, fs) ], @[ blended ]);
            Report(false, error.empty(), "l2: same functions, other state after a serialization", error);
            error = SerializeBetween(device, @[ ColorPipeline(vs, fs) ], @[ ColorPipeline(vs2, fs) ]);
            Report(false, error.empty(), "l3: fragment function reused after a serialization", error);
            error = SerializeBetween(device, @[ ColorPipeline(vs, fs) ], @[ ColorPipeline(vs, fs2), blended ], true);
            Report(true, error.empty(), "l4: vertex function reused in a new archive", error);
            Case(device, true, "l5: vertex function reused, serialized once",
                 @[ ColorPipeline(vs, fs), ColorPipeline(vs, fs2), blended ]);

            // (m) The renderer's configuration (PsoCache): pipelines are created (looked up in the
            // loaded archive, or compiled), their descriptors recorded, and every write builds a
            // fresh archive of all recorded descriptors, serialized once. Two writes, the second
            // batch reusing the first batch's functions (what fails in l1-l3), with a compute
            // pipeline; then the file is reloaded and every pipeline must be an archive hit.
            id<MTLFunction> cs = Function(
                device,
                @"#include <metal_stdlib>\nusing namespace metal;\n"
                 "kernel void compute_m(device float* b [[buffer(0)]], uint i "
                 "[[thread_position_in_grid]]) { b[i] *= 3.0f; }\n",
                @"compute_m");
            MTLComputePipelineDescriptor* compute = [MTLComputePipelineDescriptor new];
            compute.computeFunction = cs;
            NSMutableArray* recorded = [NSMutableArray array];
            NSArray* batches[2] = {@[ ColorPipeline(vs, fs), compute ],
                                   @[ ColorPipeline(vs2, fs), ColorPipeline(vs, fs2), blended ]};
            NSURL* url = TempURL("renderer");
            error.clear();
            for (NSArray* batch : batches) {
                for (id desc in batch) {
                    bool hit = false;
                    if (error.empty() && !CreateState(device, desc, nil, hit)) {
                        error = "pipeline creation failed";
                    }
                }
                [recorded addObjectsFromArray:batch];
                if (error.empty()) {
                    error = WriteFresh(device, recorded, url);
                }
            }
            Report(true, error.empty(), "m: renderer's writes (fresh archive of all recorded pipelines)", error);
            if (error.empty()) {
                const int hits = CountHits(device, recorded, url);
                Report(true, hits == static_cast<int>(recorded.count),
                       "m: reloaded, every pipeline is an archive hit",
                       std::to_string(hits) + " of " + std::to_string(recorded.count));
            }

            // (m2) The next session: m's file is loaded, its pipelines come from it (hits), a new
            // pipeline reusing their functions is compiled, and the fresh write of all of them
            // reloads with every pipeline a hit. Before (INFO): the new pipeline added to the
            // loaded archive itself and that archive serialized again.
            if (error.empty()) {
                id<MTLBinaryArchive> loaded = NewArchive(device, url);
                NSMutableArray* session = [NSMutableArray array];
                int hits = 0;
                for (id desc in recorded) {
                    bool hit = false;
                    if (CreateState(device, desc, loaded, hit)) {
                        hits += hit;
                        [session addObject:desc];
                    }
                }
                // Functions of the hits, in a state the file has no pipeline for.
                MTLRenderPipelineDescriptor* added = ColorPipeline(vs2, fs2);
                added.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
                added.colorAttachments[0].blendingEnabled = YES;
                bool new_hit = true;
                if (!CreateState(device, added, loaded, new_hit)) {
                    error = "pipeline creation failed";
                }
                [session addObject:added];
                Report(true, hits == static_cast<int>(recorded.count) && !new_hit,
                       "m2: next session, the loaded archive's pipelines are hits, the new one is not",
                       std::to_string(hits) + " of " + std::to_string(recorded.count) +
                           (new_hit ? ", the new one too" : ""));

                NSError* ns_error = nil;
                std::string old_way;
                if (![loaded addRenderPipelineFunctionsWithDescriptor:added error:&ns_error]) {
                    old_way = "add: " + ErrorText(ns_error);
                } else if (![loaded serializeToURL:TempURL("old-way") error:&ns_error]) {
                    old_way = "write: " + ErrorText(ns_error);
                }
                Report(false, old_way.empty(), "m2 before: added to the loaded archive, written again", old_way);
                [[NSFileManager defaultManager] removeItemAtURL:TempURL("old-way") error:nil];

                NSURL* next = TempURL("renderer-next");
                if (error.empty()) {
                    error = WriteFresh(device, session, next);
                }
                Report(true, error.empty(), "m2: next session's fresh write", error);
                if (error.empty()) {
                    const int next_hits = CountHits(device, session, next);
                    Report(true, next_hits == static_cast<int>(session.count),
                           "m2: reloaded, every pipeline is an archive hit (the new one too)",
                           std::to_string(next_hits) + " of " + std::to_string(session.count));
                }
                [[NSFileManager defaultManager] removeItemAtURL:next error:nil];
            }
            [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
        }

        // (d) Without a fragment function: rasterization off, and a depth-only pipeline.
        id<MTLFunction> vs_d = Function(device, VertexSource(@"vertex_d", 1.0f), @"vertex_d");
        id<MTLFunction> vs_void = Function(
            device,
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "vertex void vertex_void(device float* b [[buffer(0)]], uint vid [[vertex_id]]) {\n"
             "  b[vid] = 1.0f; }\n",
            @"vertex_void");
        MTLRenderPipelineDescriptor* no_raster = [MTLRenderPipelineDescriptor new];
        no_raster.vertexFunction = vs_void;
        no_raster.rasterizationEnabled = NO;
        MTLRenderPipelineDescriptor* depth_only = [MTLRenderPipelineDescriptor new];
        depth_only.vertexFunction = vs_d;
        depth_only.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        Case(device, true, "d1: vertex-only pipeline, rasterization off", @[ no_raster ]);
        Case(device, true, "d2: depth-only pipeline, no fragment function", @[ depth_only ]);
        Case(device, true, "d3: depth-only and colour pipelines in one archive",
             @[ depth_only, ColorPipeline(vs_c1, fs_c1) ]);

        // (e) A post-tessellation vertex function.
        id<MTLFunction> tese = Function(
            device,
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct Out { float4 position [[position]]; float4 color [[user(locn0)]]; };\n"
             "[[patch(triangle, 3)]] vertex Out vertex_tese(uint pid [[patch_id]],\n"
             "    float3 bc [[position_in_patch]]) {\n"
             "  Out o; o.position = float4(bc, 1); o.color = float4(float(pid)); return o; }\n",
            @"vertex_tese");
        MTLRenderPipelineDescriptor* tess = ColorPipeline(tese, fs_c1);
        tess.tessellationPartitionMode = MTLTessellationPartitionModeInteger;
        tess.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
        tess.tessellationOutputWindingOrder = MTLWindingCounterClockwise;
        tess.tessellationFactorFormat = MTLTessellationFactorFormatHalf;
        tess.maxTessellationFactor = 16;
        Case(device, true, "e: post-tessellation vertex function", @[ tess ]);

        // (f) Vertex descriptor with dynamic strides (all the renderer's vertex buffers).
        id<MTLFunction> vs_f = Function(
            device,
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "struct In { float4 a0 [[attribute(0)]]; float4 a1 [[attribute(1)]]; };\n"
             "struct Out { float4 position [[position]]; float4 color [[user(locn0)]]; };\n"
             "vertex Out vertex_f(In in [[stage_in]]) {\n"
             "  Out o; o.position = in.a0; o.color = in.a1; return o; }\n",
            @"vertex_f");
        MTLRenderPipelineDescriptor* dynamic = ColorPipeline(vs_f, fs_c2);
        MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat4;
        vd.attributes[0].bufferIndex = 30;
        vd.layouts[30].stride = MTLBufferLayoutStrideDynamic;
        vd.layouts[30].stepFunction = MTLVertexStepFunctionPerVertex;
        vd.attributes[1].format = MTLVertexFormatFloat4;
        vd.attributes[1].bufferIndex = 29;
        vd.layouts[29].stride = MTLBufferLayoutStrideDynamic;
        vd.layouts[29].stepFunction = MTLVertexStepFunctionConstant;
        vd.layouts[29].stepRate = 0;
        dynamic.vertexDescriptor = vd;
        Case(device, true, "f: vertex descriptor with dynamic strides", @[ dynamic ]);

        // (g) The descriptor's binaryArchives holds the archive when added (the renderer's way),
        // against a descriptor without it.
        Case(device, true, "g1: binaryArchives holds the archive when added",
             @[ ColorPipeline(vs_c1, fs_c1), ColorPipeline(vs_c2, fs_c2) ], true);
        Case(device, true, "g2: binaryArchives empty when added",
             @[ ColorPipeline(vs_c1, fs_c1), ColorPipeline(vs_c2, fs_c2) ], false);
        Case(device, false, "g3: all main0, binaryArchives empty when added",
             @[ ColorPipeline(vs_b1, fs_b1), ColorPipeline(vs_b2, fs_b2) ], false);

        // (h) Compute kernels next to render pipelines, unique names.
        id<MTLFunction> cs = Function(
            device,
            @"#include <metal_stdlib>\nusing namespace metal;\n"
             "kernel void compute_h(device float* b [[buffer(0)]], uint i [[thread_position_in_grid]]) {\n"
             "  b[i] *= 2.0f; }\n",
            @"compute_h");
        MTLComputePipelineDescriptor* compute = [MTLComputePipelineDescriptor new];
        compute.computeFunction = cs;
        Case(device, true, "h: compute and render pipelines, unique names",
             @[ compute, ColorPipeline(vs_c1, fs_c1), depth_only, tess, dynamic ]);

        // Reload: everything the renderer adds, serialized, then created from the loaded archive
        // only (MTLPipelineOptionFailOnBinaryArchiveMiss).
        NSArray* all = @[ ColorPipeline(vs_c1, fs_c1), ColorPipeline(vs_c2, fs_c2), depth_only,
                          tess, dynamic, compute ];
        NSURL* url = TempURL("reload");
        std::string error = AddAndSerialize(device, all, url, true);
        Report(true, error.empty(), "reload: serialize", error);
        if (error.empty()) {
            id<MTLBinaryArchive> loaded = NewArchive(device, url);
            if (loaded) {
                int hits = 0;
                for (id desc in all) {
                    [desc setBinaryArchives:@[ loaded ]];
                    NSError* ns_error = nil;
                    id state = nil;
                    if ([desc isKindOfClass:[MTLRenderPipelineDescriptor class]]) {
                        state = [device
                            newRenderPipelineStateWithDescriptor:desc
                                                         options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                      reflection:nil
                                                           error:&ns_error];
                    } else {
                        state = [device
                            newComputePipelineStateWithDescriptor:desc
                                                          options:MTLPipelineOptionFailOnBinaryArchiveMiss
                                                       reflection:nil
                                                            error:&ns_error];
                    }
                    if (state) {
                        ++hits;
                    } else {
                        std::printf("  miss: %s\n", ErrorText(ns_error).c_str());
                    }
                }
                const int total = static_cast<int>(all.count);
                Report(true, hits == total, "reload: every pipeline is an archive hit",
                       std::to_string(hits) + " of " + std::to_string(total));
            }
        }
        [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    }
    if (g_failures) {
        std::printf("mtl-archive-test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mtl-archive-test: all required cases pass\n");
    return 0;
}
