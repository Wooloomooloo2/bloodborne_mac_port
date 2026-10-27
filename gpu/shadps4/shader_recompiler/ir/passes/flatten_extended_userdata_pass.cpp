// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 Mark Hall
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <boost/container/flat_map.hpp>
#include <queue>
#include "bbport_threads.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/srt_gvn_table.h"
#include "shader_recompiler/ir/value.h"

// bbport: upstream JIT-compiles the SRT walker to x86-64 (Xbyak) and patches loads that fault
// into "zero the register" (Zydis, SIGSEGV handler). Here the walker is a portable bytecode run
// by an interpreter, on x86-64 and on the native arm64 GPU host alike. A load that faults is
// marked zeroed permanently, as the patch did; the fault returns through runtime_fault_recover.
// Instructions are three words: opcode (bit 31 = zeroed load), a, b. Offsets are in dwords.

namespace {

constexpr u32 SrtMagic = 0x31545253; // "SRT1"
constexpr u32 SrtZeroed = 1u << 31;
constexpr u32 SrtMaxStack = 64;

enum class SrtOp : u32 {
    End,
    Imm,         // push a
    LoadFlat,    // push flat[a] (user data registers and already-flattened values)
    Add,
    Sub,
    Mul,
    Shl,
    Shr,
    And,
    Or,
    Xor,
    Not,
    UMin,
    UMax,
    Bfe,         // pop count, offset, base: (base >> offset) & mask(count)
    PushPtrImm,  // push the current pointer; pointer = *(u64*)(pointer + a * 4) & 48 bits
    PushPtrExpr, // as PushPtrImm with a popped offset
    CopyImm,     // flat[b] = *(u32*)(pointer + a * 4)
    CopyExpr,    // flat[b] = *(u32*)(pointer + popped offset * 4)
    PopPtr,
};

constexpr const char* SrtOpName(SrtOp op) {
    constexpr const char* names[] = {"end", "imm", "load_flat", "add", "sub", "mul", "shl",
                                     "shr", "and", "or", "xor", "not", "umin", "umax", "bfe",
                                     "push_ptr_imm", "push_ptr_expr", "copy_imm", "copy_expr",
                                     "pop_ptr"};
    return static_cast<u32>(op) < std::size(names) ? names[static_cast<u32>(op)] : "?";
}

std::mutex g_walker_mutex;
std::deque<std::unique_ptr<u32[]>> g_walkers;

/// Whether a program is well formed and stays within the interpreter's stacks.
bool CheckProgram(const u32* code, size_t words) {
    if (words < 4 || code[0] != SrtMagic || (words - 1) % 3 != 0) {
        return false;
    }
    int values = 0, ptrs = 0;
    for (size_t i = 1; i < words; i += 3) {
        const auto op = static_cast<SrtOp>(code[i] & ~SrtZeroed);
        switch (op) {
        case SrtOp::End:
            return i + 3 == words && values == 0 && ptrs == 0;
        case SrtOp::Imm:
        case SrtOp::LoadFlat:
            ++values;
            break;
        case SrtOp::Not:
            if (values < 1) {
                return false;
            }
            break;
        case SrtOp::Bfe:
            if (values < 3) {
                return false;
            }
            values -= 2;
            break;
        case SrtOp::PushPtrImm:
            ++ptrs;
            break;
        case SrtOp::PushPtrExpr:
            --values;
            ++ptrs;
            break;
        case SrtOp::CopyImm:
            break;
        case SrtOp::CopyExpr:
            --values;
            break;
        case SrtOp::PopPtr:
            --ptrs;
            break;
        default:
            if (op > SrtOp::PopPtr || values < 2) {
                return false;
            }
            --values; // binary operators
            break;
        }
        if (values < 0 || ptrs < 0 || values > int(SrtMaxStack) || ptrs > int(SrtMaxStack)) {
            return false;
        }
    }
    return false;
}

Shader::SrtWalkerCode StoreWalker(const u32* code, size_t words) {
    ASSERT_MSG(CheckProgram(code, words), "Malformed SRT walker program");
    auto copy = std::make_unique<u32[]>(words);
    std::memcpy(copy.get(), code, words * sizeof(u32));
    std::scoped_lock lock{g_walker_mutex};
    return g_walkers.emplace_back(std::move(copy)).get();
}

template <typename T>
T LoadGuest(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

void DumpSrtProgram(const Shader::Info& info, const u32* code, size_t words) {
    using namespace Common::FS;

    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}_{:#018x}.srtprogram.txt", info.hw_stage, info.pgm_hash);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create, FileType::TextFile};
    for (size_t i = 1; i + 2 < words; i += 3) {
        file.WriteString(fmt::format("{:4}: {} {} {}\n", (i - 1) / 3,
                                     SrtOpName(static_cast<SrtOp>(code[i] & ~SrtZeroed)),
                                     code[i + 1], code[i + 2]));
    }
}

} // namespace

namespace Shader {

SrtWalkerCode RegisterWalkerCode(const u8* ptr, size_t size) {
    ASSERT(size % sizeof(u32) == 0);
    std::vector<u32> code(size / sizeof(u32));
    std::memcpy(code.data(), ptr, size);
    return StoreWalker(code.data(), code.size());
}

SrtReadHook srt_read_hook = nullptr;

void RunSrtWalker(SrtWalkerCode code, const u32* user_data, u32* flat) {
    // Reads through guest pointers may fault (stale or unmapped tables): the faulting load is
    // marked zeroed for good and the walk restarts. It only reads guest memory and rewrites
    // the same flat words, so restarting is safe; each restart zeroes one more load.
    u32* const ops = const_cast<u32*>(code) + 1;
    BbRecoverBuf* const outer = runtime_fault_recover;
    volatile u32 pc = 0;
    BbRecoverBuf recover;
    if (BB_RECOVER_SET(recover)) {
        std::atomic_ref<u32>(ops[pc]).fetch_or(SrtZeroed, std::memory_order_relaxed);
        LOG_WARNING(Render_Recompiler, "SRT walker load {} faulted; it now reads zero", pc / 3);
    }
    runtime_fault_recover = &recover;

    u32 values[SrtMaxStack];
    u64 ptrs[SrtMaxStack];
    u32 nv = 0, np = 0;
    u64 cur = reinterpret_cast<u64>(user_data);
    for (u32 i = 0;; i += 3) {
        const u32 word = std::atomic_ref<u32>(ops[i]).load(std::memory_order_relaxed);
        const bool zeroed = word & SrtZeroed;
        const auto op = static_cast<SrtOp>(word & ~SrtZeroed);
        const u32 a = ops[i + 1], b = ops[i + 2];
        u32 rhs;
        switch (op) {
        case SrtOp::End:
            runtime_fault_recover = outer;
            return;
        case SrtOp::Imm:
            values[nv++] = a;
            continue;
        case SrtOp::LoadFlat:
            values[nv++] = flat[a];
            continue;
        case SrtOp::Not:
            values[nv - 1] = ~values[nv - 1];
            continue;
        case SrtOp::Bfe: {
            const u32 count = values[--nv], offset = values[--nv];
            const u32 mask = count >= 32 ? ~0u : (1u << count) - 1;
            values[nv - 1] = (values[nv - 1] >> (offset & 31)) & mask;
            continue;
        }
        case SrtOp::PushPtrImm:
        case SrtOp::PushPtrExpr: {
            const u32 off = op == SrtOp::PushPtrImm ? a : values[--nv];
            ptrs[np++] = cur;
            pc = i;
            // A null table reads zero for this walk only: marking the load zeroed for good
            // would keep it zero after the game fills the table in again (#74).
            const bool null = cur < Info::MinTableAddress;
            if (srt_read_hook && !zeroed && !null) {
                srt_read_hook(cur + u32(off << 2), 8);
            }
            cur = zeroed || null ? 0 : LoadGuest<u64>(cur + u32(off << 2)) & 0xFFFFFFFFFFFFULL;
            continue;
        }
        case SrtOp::CopyImm:
        case SrtOp::CopyExpr: {
            const u32 off = op == SrtOp::CopyImm ? a : values[--nv];
            pc = i;
            const bool null = cur < Info::MinTableAddress;
            if (srt_read_hook && !zeroed && !null) {
                srt_read_hook(cur + u32(off << 2), 4);
            }
            flat[b] = zeroed || null ? 0 : LoadGuest<u32>(cur + u32(off << 2));
            continue;
        }
        case SrtOp::PopPtr:
            cur = ptrs[--np];
            continue;
        default:
            break;
        }
        rhs = values[--nv];
        u32& lhs = values[nv - 1];
        switch (op) {
        case SrtOp::Add:
            lhs += rhs;
            break;
        case SrtOp::Sub:
            lhs -= rhs;
            break;
        case SrtOp::Mul:
            lhs *= rhs;
            break;
        case SrtOp::Shl:
            lhs <<= rhs & 31;
            break;
        case SrtOp::Shr:
            lhs >>= rhs & 31;
            break;
        case SrtOp::And:
            lhs &= rhs;
            break;
        case SrtOp::Or:
            lhs |= rhs;
            break;
        case SrtOp::Xor:
            lhs ^= rhs;
            break;
        case SrtOp::UMin:
            lhs = std::min(lhs, rhs);
            break;
        case SrtOp::UMax:
            lhs = std::max(lhs, rhs);
            break;
        default:
            UNREACHABLE_MSG("Bad SRT walker opcode {}", static_cast<u32>(op));
        }
    }
}

} // namespace Shader

namespace {

using namespace Shader;

struct PassInfo {
    struct PointerListCompare {
        bool operator()(const IR::Value& a, const IR::Value& b) const {
            // Keep order with the following criteria:
            // 1. If immediate, sort by immediate value
            // 2. If not immediate, sort by instruction order
            // 3. Immediates come first, then instructions
            if (a.IsImmediate() && b.IsImmediate()) {
                return a.U32() < b.U32();
            } else if (!a.IsImmediate() && !b.IsImmediate()) {
                auto a_inst = a.Inst();
                auto b_inst = b.Inst();
                auto a_block = a_inst->GetParent();
                auto b_block = b_inst->GetParent();
                if (a_block != b_block) {
                    return a_block < b_block; // Sort by block address
                }

                auto it = a_block->Instructions().iterator_to(*a_inst);
                auto end = a_block->Instructions().end();
                for (; it != end; ++it) {
                    if (&*it == b_inst) {
                        return true;
                    }
                }
                return false;
            } else {
                return a.IsImmediate(); // Immediates come first
            }
        }
    };

    // map offset to inst
    using PtrUserList =
        boost::container::flat_map<IR::Value, Shader::IR::Inst*, PointerListCompare>;

    Optimization::SrtGvnTable gvn_table;
    // keys are GetUserData, ReadConst or ReadConstBuffer instructions that are used as pointers
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    // GetUserData instructions corresponding to sgpr_base of SRT roots
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;

    // pick a single inst for a given value number
    std::unordered_map<u32, IR::Inst*> vn_to_inst;

    // Bumped during codegen to assign offsets to readconsts
    u16 dst_off_dw;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        if (it != pointer_uses.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Return a single instruction that this instruction is identical to, according
    // to value number
    // The "original" is arbitrary. Here it's the first instruction found for a given value number
    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};
} // namespace

namespace Shader::Optimization {

namespace {

static inline u16 GetFlatbufOffset(const IR::Inst* inst) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        return inst_info.flatbuf_off_dw;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        return inst->Flags<u16>();
    }
    UNREACHABLE_MSG("Instruction not supported");
}

static inline void SetFlatbufOffset(IR::Inst* inst, u16 offset) {
    if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
        auto inst_info = inst->Flags<IR::BufferInstInfo>();
        inst_info.flatbuf_off_dw.Assign(offset);
        inst->SetFlags(inst_info);
        return;
    }
    if (inst->GetOpcode() == IR::Opcode::ReadConst) {
        inst->SetFlags(offset);
        return;
    }
    UNREACHABLE_MSG("Instruction not supported");
}

using Code = std::vector<u32>;

static void Emit(Code& c, SrtOp op, u32 a = 0, u32 b = 0) {
    c.insert(c.end(), {static_cast<u32>(op), a, b});
}

static bool ComputeOffset(Code& c, PassInfo& pass_info, const IR::Value& off_dw);

#define ABORT_ON_FAILURE(expr)                                                                     \
    if (!(expr)) {                                                                                 \
        return false;                                                                              \
    }

static bool EmitOperand(Code& c, PassInfo& pass_info, const IR::Value& value) {
    if (value.IsImmediate()) {
        Emit(c, SrtOp::Imm, value.U32());
        return true;
    }
    return ComputeOffset(c, pass_info, value);
}

// Operands are pushed in argument order; the operator combines them as arg0 op arg1.
static bool EmitOperator(Code& c, PassInfo& pass_info, IR::Inst* inst, SrtOp op) {
    ASSERT(!inst->AreAllArgsImmediates());
    for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
        ABORT_ON_FAILURE(EmitOperand(c, pass_info, inst->Arg(arg)));
    }
    Emit(c, op);
    return true;
}

static bool IsAllowedOffsetInstruction(const IR::Inst* inst) {
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
    case IR::Opcode::IAdd32:
    case IR::Opcode::ISub32:
    case IR::Opcode::IMul32:
    case IR::Opcode::ShiftLeftLogical32:
    case IR::Opcode::ShiftRightLogical32:
    case IR::Opcode::BitwiseAnd32:
    case IR::Opcode::BitwiseOr32:
    case IR::Opcode::BitwiseXor32:
    case IR::Opcode::BitwiseNot32:
    case IR::Opcode::UMin32:
    case IR::Opcode::UMax32:
    case IR::Opcode::BitFieldUExtract:
        return true;
    default:
        return false;
    }
}

static bool ComputeOffset(Code& c, PassInfo& pass_info, const IR::Value& off_dw) {
    auto inst = off_dw.Inst();
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
        Emit(c, SrtOp::LoadFlat, static_cast<u32>(inst->Arg(0).ScalarReg()));
        return true;
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
        if (u16 offset = GetFlatbufOffset(pass_info.DeduplicateInstruction(inst)); offset != 0) {
            Emit(c, SrtOp::LoadFlat, offset);
            return true;
        }
        return false;
    case IR::Opcode::IAdd32:
        return EmitOperator(c, pass_info, inst, SrtOp::Add);
    case IR::Opcode::ISub32:
        return EmitOperator(c, pass_info, inst, SrtOp::Sub);
    case IR::Opcode::IMul32:
        return EmitOperator(c, pass_info, inst, SrtOp::Mul);
    case IR::Opcode::ShiftLeftLogical32:
        return EmitOperator(c, pass_info, inst, SrtOp::Shl);
    case IR::Opcode::ShiftRightLogical32:
        return EmitOperator(c, pass_info, inst, SrtOp::Shr);
    case IR::Opcode::BitwiseAnd32:
        return EmitOperator(c, pass_info, inst, SrtOp::And);
    case IR::Opcode::BitwiseOr32:
        return EmitOperator(c, pass_info, inst, SrtOp::Or);
    case IR::Opcode::BitwiseXor32:
        return EmitOperator(c, pass_info, inst, SrtOp::Xor);
    case IR::Opcode::BitwiseNot32:
        return EmitOperator(c, pass_info, inst, SrtOp::Not);
    case IR::Opcode::UMin32:
        return EmitOperator(c, pass_info, inst, SrtOp::UMin);
    case IR::Opcode::UMax32:
        return EmitOperator(c, pass_info, inst, SrtOp::UMax);
    case IR::Opcode::BitFieldUExtract:
        return EmitOperator(c, pass_info, inst, SrtOp::Bfe);
    default:
        LOG_ERROR(Render_Recompiler, "Unexpected instruction for offset computation, {}",
                  magic_enum::enum_name(inst->GetOpcode()));
        return false;
    }
}

// An offset expression is built aside and appended only when complete.
static bool EmitOffset(Code& c, PassInfo& pass_info, const IR::Value& off_dw) {
    Code expr;
    ABORT_ON_FAILURE(ComputeOffset(expr, pass_info, off_dw));
    c.insert(c.end(), expr.begin(), expr.end());
    return true;
}

static void VisitPointer(const IR::Value& off_dw, IR::Inst* subtree, PassInfo& pass_info,
                         Code& c) {
    if (subtree->GetOpcode() == IR::Opcode::ReadConst && subtree->Flags<u16>() == 0 ||
        subtree->GetOpcode() == IR::Opcode::ReadConstBuffer &&
            subtree->Flags<IR::BufferInstInfo>().flatbuf_off_dw == 0) {
        return;
    }

    if (off_dw.IsImmediate()) {
        Emit(c, SrtOp::PushPtrImm, off_dw.U32());
    } else if (EmitOffset(c, pass_info, off_dw)) {
        Emit(c, SrtOp::PushPtrExpr);
    } else {
        LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
        return;
    }
    PassInfo::PtrUserList* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);

    // First copy all the src data from this tree level
    // That way, all data that was contiguous in the guest SRT is also contiguous in the
    // flattened buffer.
    for (auto [src_off_dw, use] : *use_list) {
        if (src_off_dw.IsImmediate()) {
            Emit(c, SrtOp::CopyImm, src_off_dw.U32(), pass_info.dst_off_dw);
        } else if (EmitOffset(c, pass_info, src_off_dw)) {
            Emit(c, SrtOp::CopyExpr, 0, pass_info.dst_off_dw);
        } else {
            LOG_ERROR(Render_Recompiler, "Failed to compute offset for SRT walker");
            continue;
        }

        SetFlatbufOffset(use, pass_info.dst_off_dw);
        pass_info.dst_off_dw++;
    }

    // Then visit any children used as pointers
    for (const auto [src_off_dw, use] : *use_list) {
        if (pass_info.GetUsesAsPointer(use)) {
            VisitPointer(src_off_dw, use, pass_info, c);
        }
    }

    Emit(c, SrtOp::PopPtr);
}

static void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    if (pass_info.srt_roots.empty()) {
        return;
    }

    Code c{SrtMagic};
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);

    for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
        VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, c);
    }
    Emit(c, SrtOp::End);

    info.srt_info.walker_func = StoreWalker(c.data(), c.size());
    info.srt_info.walker_func_size = c.size() * sizeof(u32);

    if (EmulatorSettings.IsDumpShaders()) {
        DumpSrtProgram(info, c.data(), c.size());
    }

    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

static bool IsReadConstSource(const IR::Value base) {
    auto* inst = base.TryInst();
    if (!inst) {
        return false;
    }
    return inst->GetOpcode() == IR::Opcode::GetUserData ||
           inst->GetOpcode() == IR::Opcode::ReadConst ||
           inst->GetOpcode() == IR::Opcode::ReadConstBuffer;
}

void SimplifyReadConstAddressAdd(IR::Inst& inst) {
    // This handles the following pattern by combining the addition with the offset
    // s_add_u32    s12, s2, <offset>
    // s_addc_u32   s13, s3, 0
    // s_load_dword s16, s[12:13], 0x0

    if (inst.GetOpcode() != IR::Opcode::ReadConst) {
        return;
    }
    IR::Inst* addr = inst.Arg(0).Inst();
    IR::Inst* hi = addr->Arg(1).TryInst();
    IR::Inst* lo = addr->Arg(0).TryInst();
    if (!hi || !lo) {
        return;
    }

    if (lo->GetOpcode() != IR::Opcode::IAdd32 || hi->GetOpcode() != IR::Opcode::IAdd32) {
        return;
    }

    IR::Value add_offset;
    IR::Value base;
    if (base = lo->Arg(0); IsReadConstSource(base)) {
        add_offset = lo->Arg(1);
    } else if (base = lo->Arg(1); IsReadConstSource(base)) {
        add_offset = lo->Arg(0);
    } else {
        return;
    }

    IR::Inst* sel = hi->Arg(1).TryInst();
    if (!sel || sel->GetOpcode() != IR::Opcode::SelectU32) {
        return;
    }

    IR::Value sel_true = sel->Arg(1);
    IR::Value sel_false = sel->Arg(2);
    if (!sel_true.IsImmediate() || !sel_false.IsImmediate() || sel_true.U32() != 1 ||
        sel_false.U32() != 0) {
        return;
    }

    addr->SetArg(0, base);
    addr->SetArg(1, hi->Arg(0));
    for (auto [user, operand] : addr->Uses()) {
        ASSERT(user->GetOpcode() == IR::Opcode::ReadConst && operand == 0);
        IR::IREmitter ir{*user->GetParent(), IR::Block::InstructionList::s_iterator_to(*user)};
        IR::U32 offset = IR::U32{user->Arg(1)};
        IR::U32 dw_add_offset =
            add_offset.IsImmediate()
                ? ir.Imm32(add_offset.U32() >> 2u)
                : IR::U32{ir.ShiftRightLogical(IR::U32{add_offset}, ir.Imm32(2u))};
        if (offset.IsImmediate() && dw_add_offset.IsImmediate()) {
            user->SetArg(1, ir.Imm32(offset.U32() + dw_add_offset.U32()));
        } else {
            user->SetArg(1, ir.IAdd(offset, dw_add_offset));
        }
    }
}

} // Anonymous namespace

void FlattenExtendedUserdataPass(IR::Program& program) {
    auto& post_order = program.post_order_blocks;
    PassInfo pass_info;

    // traverse at end and assign offsets to duplicate readconsts, using
    // vn_to_inst as the source
    boost::container::small_vector<IR::Inst*, 32> all_readconsts, visited;
    std::queue<IR::Inst*> queue;

    for (auto it = post_order.rbegin(); it != post_order.rend(); it++) {
        IR::Block* block{*it};
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::ReadConst &&
                inst.GetOpcode() != IR::Opcode::ReadConstBuffer) {
                continue;
            }

            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer) {
                // Only flatten ReadConstBuffer if it was marked as a sharp source in the
                // resource discovery pass
                auto inst_info = inst.Flags<IR::BufferInstInfo>();
                if (!inst_info.sharp_source) {
                    continue;
                }
            }

            SimplifyReadConstAddressAdd(inst);

            all_readconsts.push_back(&inst);

            auto offset = inst.Arg(1);
            if (offset.IsImmediate()) {
                continue;
            }

            queue.push(offset.Inst());
            while (!queue.empty()) {
                IR::Inst* inst{queue.front()};
                queue.pop();

                if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                    auto buffer_inst_info = inst->Flags<IR::BufferInstInfo>();
                    if (!buffer_inst_info.sharp_source.Value()) {
                        all_readconsts.push_back(inst);
                        auto offset = inst->Arg(1);
                        if (!offset.IsImmediate()) {
                            auto arg_inst = offset.Inst();
                            if (std::ranges::find(visited, arg_inst) == visited.end()) {
                                visited.push_back(arg_inst);
                                queue.push(arg_inst);
                            }
                        }
                    }
                    continue;
                }
                if (inst->GetOpcode() == IR::Opcode::GetUserData ||
                    inst->GetOpcode() == IR::Opcode::ReadConst ||
                    !IsAllowedOffsetInstruction(inst)) {
                    continue;
                }
                for (size_t arg = inst->NumArgs(); arg--;) {
                    const IR::Value arg_value = inst->Arg(arg);
                    if (arg_value.IsImmediate()) {
                        continue;
                    }
                    IR::Inst* arg_inst = arg_value.Inst();
                    if (std::ranges::find(visited, arg_inst) == visited.end()) {
                        visited.push_back(arg_inst);
                        queue.push(arg_inst);
                    }
                }
            }
        }
    }

    for (auto inst : all_readconsts) {
        if (pass_info.DeduplicateInstruction(inst) != inst) {
            // This is a duplicate of a readconst we've already visited
            continue;
        }

        IR::Inst* base = inst->Arg(0).Inst();
        if (auto* inst = base->Arg(0).TryInst();
            inst && inst->GetOpcode() == IR::Opcode::ReadFirstLane) {
            continue;
        }
        ASSERT_MSG(IsReadConstSource(base->Arg(0)), "ReadConst base low not from constant memory");
        ASSERT_MSG(IsReadConstSource(base->Arg(1)), "ReadConst base high not from constant memory");

        IR::Inst* ptr_lo = base->Arg(0).Inst();
        ptr_lo = pass_info.DeduplicateInstruction(ptr_lo);

        auto ptr_uses_kv = pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
        PassInfo::PtrUserList& user_list = ptr_uses_kv.first->second;

        user_list[inst->Arg(1)] = inst;

        if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
            IR::ScalarReg ud_reg = ptr_lo->Arg(0).ScalarReg();
            pass_info.srt_roots[ud_reg] = ptr_lo;
        }
    }

    GenerateSrtProgram(program.info, pass_info);

    // Assign offsets to duplicate readconsts
    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        SetFlatbufOffset(readconst, GetFlatbufOffset(original));
    }

    program.info.RefreshFlatBuf();
}

} // namespace Shader::Optimization
