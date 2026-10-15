/* SIMD (VEX/AVX) instructions for lift4.c, lane by lane in C with x86's own rules where they
 * differ from ARM's (min/max operand order, compares with NaN, float->int out of range, ucomiss
 * flags, dpps summation order). Vector registers are c->v[i][0..7] (32-bit lanes of ymm i);
 * VEX.128 writes zero lanes 4..7. Approximations (rcpps, rsqrtps) are not supported: their x86
 * tables are not reproduced yet. Included by lift4.c. */

static int vreg(ZydisRegister r) {
    if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM15) return (int)(r - ZYDIS_REGISTER_XMM0);
    if (r >= ZYDIS_REGISTER_YMM0 && r <= ZYDIS_REGISTER_YMM15) return (int)(r - ZYDIS_REGISTER_YMM0);
    return -1;
}

static const ZydisMnemonic simd_ok[] = {
    ZYDIS_MNEMONIC_VMOVAPS, ZYDIS_MNEMONIC_VMOVUPS, ZYDIS_MNEMONIC_VMOVDQA, ZYDIS_MNEMONIC_VMOVDQU,
    ZYDIS_MNEMONIC_VMOVAPD, ZYDIS_MNEMONIC_VMOVUPD, ZYDIS_MNEMONIC_VMOVSS, ZYDIS_MNEMONIC_VMOVSD,
    ZYDIS_MNEMONIC_VMOVD, ZYDIS_MNEMONIC_VMOVQ, ZYDIS_MNEMONIC_VADDPS, ZYDIS_MNEMONIC_VSUBPS,
    ZYDIS_MNEMONIC_VMULPS, ZYDIS_MNEMONIC_VDIVPS, ZYDIS_MNEMONIC_VADDSS, ZYDIS_MNEMONIC_VSUBSS,
    ZYDIS_MNEMONIC_VMULSS, ZYDIS_MNEMONIC_VDIVSS, ZYDIS_MNEMONIC_VSQRTPS, ZYDIS_MNEMONIC_VSQRTSS,
    ZYDIS_MNEMONIC_VMAXPS, ZYDIS_MNEMONIC_VMINPS, ZYDIS_MNEMONIC_VMAXSS, ZYDIS_MNEMONIC_VMINSS,
    ZYDIS_MNEMONIC_VXORPS, ZYDIS_MNEMONIC_VANDPS, ZYDIS_MNEMONIC_VANDNPS, ZYDIS_MNEMONIC_VORPS,
    ZYDIS_MNEMONIC_VPXOR, ZYDIS_MNEMONIC_VPAND, ZYDIS_MNEMONIC_VPANDN, ZYDIS_MNEMONIC_VPOR,
    ZYDIS_MNEMONIC_VPSHUFD, ZYDIS_MNEMONIC_VSHUFPS, ZYDIS_MNEMONIC_VUNPCKLPS, ZYDIS_MNEMONIC_VUNPCKHPS,
    ZYDIS_MNEMONIC_VMOVHLPS, ZYDIS_MNEMONIC_VMOVLHPS, ZYDIS_MNEMONIC_VINSERTPS, ZYDIS_MNEMONIC_VBLENDPS,
    ZYDIS_MNEMONIC_VBLENDVPS, ZYDIS_MNEMONIC_VMOVMSKPS, ZYDIS_MNEMONIC_VEXTRACTPS, ZYDIS_MNEMONIC_VBROADCASTSS,
    ZYDIS_MNEMONIC_VHADDPS, ZYDIS_MNEMONIC_VDPPS, ZYDIS_MNEMONIC_VCMPPS, ZYDIS_MNEMONIC_VCMPSS,
    ZYDIS_MNEMONIC_VUCOMISS, ZYDIS_MNEMONIC_VCOMISS, ZYDIS_MNEMONIC_VCVTSI2SS, ZYDIS_MNEMONIC_VCVTTSS2SI,
    ZYDIS_MNEMONIC_VCVTDQ2PS, ZYDIS_MNEMONIC_VCVTTPS2DQ, ZYDIS_MNEMONIC_VCVTSS2SD, ZYDIS_MNEMONIC_VPADDD,
    ZYDIS_MNEMONIC_VPSUBD, ZYDIS_MNEMONIC_VPADDQ, ZYDIS_MNEMONIC_VPSUBQ, ZYDIS_MNEMONIC_VPSLLD,
    ZYDIS_MNEMONIC_VPSRLD, ZYDIS_MNEMONIC_VPSRAD, ZYDIS_MNEMONIC_VPSLLQ, ZYDIS_MNEMONIC_VPSRLQ,
    ZYDIS_MNEMONIC_VPCMPEQD, ZYDIS_MNEMONIC_VPCMPGTD, ZYDIS_MNEMONIC_VPSHUFB, ZYDIS_MNEMONIC_VZEROUPPER,
};

static int simd_supported(const Insn *x) {
    const ZydisDecodedInstruction *in = &x->in;
    int known = 0;
    for (size_t i = 0; i < sizeof(simd_ok) / sizeof(simd_ok[0]); ++i) if (simd_ok[i] == in->mnemonic) known = 1;
    if (!known) return 0;
    for (int i = 0; i < in->operand_count_visible; ++i) {
        const ZydisDecodedOperand *o = &x->ops[i];
        if (o->type == ZYDIS_OPERAND_TYPE_REGISTER) {
            if (vreg(o->reg.value) < 0 && (gpr_index(o->reg.value) < 0 || o->size < 32)) return 0;
        } else if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) {
            if (!mem_ok(x, o)) return 0;
        } else if (o->type != ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            return 0;
        }
    }
    if ((in->mnemonic == ZYDIS_MNEMONIC_VCMPPS || in->mnemonic == ZYDIS_MNEMONIC_VCMPSS) &&
        x->ops[in->operand_count_visible - 1].imm.value.u > 7)
        return 0;
    /* shifts by an xmm count, vpshufd/vpsll.. 256-bit integer forms (AVX2) do not occur */
    if ((in->mnemonic == ZYDIS_MNEMONIC_VPSLLD || in->mnemonic == ZYDIS_MNEMONIC_VPSRLD || in->mnemonic == ZYDIS_MNEMONIC_VPSRAD ||
         in->mnemonic == ZYDIS_MNEMONIC_VPSLLQ || in->mnemonic == ZYDIS_MNEMONIC_VPSRLQ) &&
        x->ops[2].type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
        return 0;
    if (in->mnemonic == ZYDIS_MNEMONIC_VMOVQ && x->ops[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
        vreg(x->ops[0].reg.value) < 0 && x->ops[1].type == ZYDIS_OPERAND_TYPE_REGISTER && vreg(x->ops[1].reg.value) < 0)
        return 0;
    return 1;
}

/* Loads operand i of x as `lanes` 32-bit lanes into the C array named arr. */
static void vload(const Insn *x, int i, const char *arr, int lanes) {
    const ZydisDecodedOperand *o = &x->ops[i];
    if (o->type == ZYDIS_OPERAND_TYPE_MEMORY) {
        char a[200];
        addr_expr(x, o, a, sizeof(a));
        fprintf(out, "      memcpy(%s, (const void *)(uintptr_t)%s, %d);\n", arr, a, lanes * 4);
    } else if (vreg(o->reg.value) >= 0) {
        fprintf(out, "      memcpy(%s, c->v[%d], %d);\n", arr, vreg(o->reg.value), lanes * 4);
    } else { /* a general register: its low 32 or 64 bits */
        fprintf(out, "      { uint64_t g = %s; memcpy(%s, &g, %d); }\n", cname(gpr_index(o->reg.value)), arr, o->size / 8);
    }
}
/* Writes R[0..lanes) to vector register operand i, zeroing the lanes above (VEX). */
static void vstore_reg(const Insn *x, int i, int lanes) {
    int r = vreg(x->ops[i].reg.value);
    fprintf(out, "      memcpy(c->v[%d], R, %d); memset(&c->v[%d][%d], 0, %d);\n", r, lanes * 4, r, lanes, (8 - lanes) * 4);
}
static void vstore_mem(const Insn *x, int i, int bytes) {
    char a[200];
    addr_expr(x, &x->ops[i], a, sizeof(a));
    fprintf(out, "      memcpy((void *)(uintptr_t)%s, R, %d);\n", a, bytes);
}

static int emit_simd(const Insn *x) {
    const ZydisDecodedInstruction *in = &x->in;
    const ZydisDecodedOperand *o = x->ops;
    const int nv = in->operand_count_visible;
    const int L = o[0].size == 256 ? 8 : 4; /* lanes of a packed operation */
    const uint64_t imm = o[nv - 1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE ? o[nv - 1].imm.value.u : 0;
    const int dst_mem = o[0].type == ZYDIS_OPERAND_TYPE_MEMORY;
    const int dst_vec = o[0].type == ZYDIS_OPERAND_TYPE_REGISTER && vreg(o[0].reg.value) >= 0;
    fprintf(out, "    { uint32_t A[8] = {0}, B[8] = {0}, R[8] = {0}; (void)A; (void)B;\n");
    switch (in->mnemonic) {
    case ZYDIS_MNEMONIC_VZEROUPPER:
        fprintf(out, "      for (int i = 0; i < 16; ++i) memset(&c->v[i][4], 0, 16);\n");
        break;
    case ZYDIS_MNEMONIC_VMOVAPS: case ZYDIS_MNEMONIC_VMOVUPS: case ZYDIS_MNEMONIC_VMOVDQA: case ZYDIS_MNEMONIC_VMOVDQU:
    case ZYDIS_MNEMONIC_VMOVAPD: case ZYDIS_MNEMONIC_VMOVUPD: {
        const int l = (dst_mem ? o[1].size : o[0].size) == 256 ? 8 : 4;
        vload(x, 1, "R", l);
        if (dst_mem) vstore_mem(x, 0, l * 4); else vstore_reg(x, 0, l);
        break;
    }
    case ZYDIS_MNEMONIC_VMOVSS: case ZYDIS_MNEMONIC_VMOVSD: {
        const int l = in->mnemonic == ZYDIS_MNEMONIC_VMOVSS ? 1 : 2;
        if (dst_mem) { vload(x, 1, "R", l); vstore_mem(x, 0, l * 4); }
        else if (nv == 2) { vload(x, 1, "R", l); vstore_reg(x, 0, 4); } /* load: upper lanes zero */
        else { vload(x, 1, "R", 4); vload(x, 2, "B", l); fprintf(out, "      memcpy(R, B, %d);\n", l * 4); vstore_reg(x, 0, 4); }
        break;
    }
    case ZYDIS_MNEMONIC_VMOVD: case ZYDIS_MNEMONIC_VMOVQ: {
        const int bytes = in->mnemonic == ZYDIS_MNEMONIC_VMOVD ? 4 : 8;
        if (dst_vec) { vload(x, 1, "R", bytes / 4); fprintf(out, "      memset((uint8_t *)R + %d, 0, %d);\n", bytes, 32 - bytes); vstore_reg(x, 0, 4); }
        else if (dst_mem) { vload(x, 1, "R", bytes / 4); vstore_mem(x, 0, bytes); }
        else { vload(x, 1, "R", bytes / 4);
               fprintf(out, "      { uint64_t g = 0; memcpy(&g, R, %d); %s = g; }\n", bytes, cname(gpr_index(o[0].reg.value))); }
        break;
    }
    case ZYDIS_MNEMONIC_VADDPS: case ZYDIS_MNEMONIC_VSUBPS: case ZYDIS_MNEMONIC_VMULPS: case ZYDIS_MNEMONIC_VDIVPS:
    case ZYDIS_MNEMONIC_VADDSS: case ZYDIS_MNEMONIC_VSUBSS: case ZYDIS_MNEMONIC_VMULSS: case ZYDIS_MNEMONIC_VDIVSS:
    case ZYDIS_MNEMONIC_VMAXPS: case ZYDIS_MNEMONIC_VMINPS: case ZYDIS_MNEMONIC_VMAXSS: case ZYDIS_MNEMONIC_VMINSS: {
        const int scalar = in->mnemonic == ZYDIS_MNEMONIC_VADDSS || in->mnemonic == ZYDIS_MNEMONIC_VSUBSS ||
                           in->mnemonic == ZYDIS_MNEMONIC_VMULSS || in->mnemonic == ZYDIS_MNEMONIC_VDIVSS ||
                           in->mnemonic == ZYDIS_MNEMONIC_VMAXSS || in->mnemonic == ZYDIS_MNEMONIC_VMINSS;
        const char *op = NULL, *fn = NULL;
        switch (in->mnemonic) {
        case ZYDIS_MNEMONIC_VADDPS: case ZYDIS_MNEMONIC_VADDSS: op = "+"; break;
        case ZYDIS_MNEMONIC_VSUBPS: case ZYDIS_MNEMONIC_VSUBSS: op = "-"; break;
        case ZYDIS_MNEMONIC_VMULPS: case ZYDIS_MNEMONIC_VMULSS: op = "*"; break;
        case ZYDIS_MNEMONIC_VDIVPS: case ZYDIS_MNEMONIC_VDIVSS: op = "/"; break;
        case ZYDIS_MNEMONIC_VMAXPS: case ZYDIS_MNEMONIC_VMAXSS: fn = "XMAX"; break;
        default: fn = "XMIN"; break;
        }
        const int l = scalar ? 1 : L;
        vload(x, 1, "A", scalar ? 4 : L);
        vload(x, 2, "B", l);
        if (scalar) fprintf(out, "      memcpy(R, A, 16);\n");
        if (op) fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = XOP(A[i], B[i], %s);\n", l, op);
        else fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = %s(A[i], B[i]);\n", l, fn);
        vstore_reg(x, 0, scalar ? 4 : L);
        break;
    }
    case ZYDIS_MNEMONIC_VSQRTPS: vload(x, 1, "A", L);
        fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = XSQRT(A[i]);\n", L); vstore_reg(x, 0, L); break;
    case ZYDIS_MNEMONIC_VSQRTSS: vload(x, 1, "R", 4); vload(x, 2, "B", 1);
        fprintf(out, "      R[0] = XSQRT(B[0]);\n"); vstore_reg(x, 0, 4); break;
    case ZYDIS_MNEMONIC_VXORPS: case ZYDIS_MNEMONIC_VANDPS: case ZYDIS_MNEMONIC_VANDNPS: case ZYDIS_MNEMONIC_VORPS:
    case ZYDIS_MNEMONIC_VPXOR: case ZYDIS_MNEMONIC_VPAND: case ZYDIS_MNEMONIC_VPANDN: case ZYDIS_MNEMONIC_VPOR: {
        const char *e = in->mnemonic == ZYDIS_MNEMONIC_VXORPS || in->mnemonic == ZYDIS_MNEMONIC_VPXOR ? "A[i] ^ B[i]" :
                        in->mnemonic == ZYDIS_MNEMONIC_VANDPS || in->mnemonic == ZYDIS_MNEMONIC_VPAND ? "A[i] & B[i]" :
                        in->mnemonic == ZYDIS_MNEMONIC_VORPS || in->mnemonic == ZYDIS_MNEMONIC_VPOR ? "A[i] | B[i]" : "~A[i] & B[i]";
        vload(x, 1, "A", L); vload(x, 2, "B", L);
        fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = %s;\n", L, e);
        vstore_reg(x, 0, L);
        break;
    }
    case ZYDIS_MNEMONIC_VPSHUFD: vload(x, 1, "A", L);
        fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = A[(i & ~3) + ((0x%llxu >> (2 * (i & 3))) & 3)];\n", L, (unsigned long long)imm);
        vstore_reg(x, 0, L); break;
    case ZYDIS_MNEMONIC_VSHUFPS: vload(x, 1, "A", L); vload(x, 2, "B", L);
        fprintf(out, "      for (int h = 0; h < %d; h += 4) { R[h] = A[h + (0x%llxu & 3)]; R[h + 1] = A[h + ((0x%llxu >> 2) & 3)];"
                     " R[h + 2] = B[h + ((0x%llxu >> 4) & 3)]; R[h + 3] = B[h + ((0x%llxu >> 6) & 3)]; }\n",
                L, (unsigned long long)imm, (unsigned long long)imm, (unsigned long long)imm, (unsigned long long)imm);
        vstore_reg(x, 0, L); break;
    case ZYDIS_MNEMONIC_VUNPCKLPS: case ZYDIS_MNEMONIC_VUNPCKHPS: {
        const int hi = in->mnemonic == ZYDIS_MNEMONIC_VUNPCKHPS ? 2 : 0;
        vload(x, 1, "A", L); vload(x, 2, "B", L);
        fprintf(out, "      for (int h = 0; h < %d; h += 4) { R[h] = A[h + %d]; R[h + 1] = B[h + %d]; R[h + 2] = A[h + %d]; R[h + 3] = B[h + %d]; }\n",
                L, hi, hi, hi + 1, hi + 1);
        vstore_reg(x, 0, L);
        break;
    }
    case ZYDIS_MNEMONIC_VMOVHLPS: vload(x, 1, "A", 4); vload(x, 2, "B", 4);
        fprintf(out, "      R[0] = B[2]; R[1] = B[3]; R[2] = A[2]; R[3] = A[3];\n"); vstore_reg(x, 0, 4); break;
    case ZYDIS_MNEMONIC_VMOVLHPS: vload(x, 1, "A", 4); vload(x, 2, "B", 4);
        fprintf(out, "      R[0] = A[0]; R[1] = A[1]; R[2] = B[0]; R[3] = B[1];\n"); vstore_reg(x, 0, 4); break;
    case ZYDIS_MNEMONIC_VINSERTPS: {
        vload(x, 1, "R", 4);
        const int mem = o[2].type == ZYDIS_OPERAND_TYPE_MEMORY;
        vload(x, 2, "B", mem ? 1 : 4);
        fprintf(out, "      R[%d] = B[%d];\n", (int)((imm >> 4) & 3), mem ? 0 : (int)(imm >> 6));
        for (int i = 0; i < 4; ++i) if (imm & (1u << i)) fprintf(out, "      R[%d] = 0;\n", i);
        vstore_reg(x, 0, 4);
        break;
    }
    case ZYDIS_MNEMONIC_VBLENDPS: vload(x, 1, "A", L); vload(x, 2, "B", L);
        fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = (0x%llxu >> i) & 1 ? B[i] : A[i];\n", L, (unsigned long long)imm);
        vstore_reg(x, 0, L); break;
    case ZYDIS_MNEMONIC_VBLENDVPS: { uint32_t M[1]; (void)M;
        vload(x, 1, "A", L); vload(x, 2, "B", L);
        fprintf(out, "      { uint32_t M[8] = {0};\n"); vload(x, 3, "M", L);
        fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = (M[i] >> 31) ? B[i] : A[i]; }\n", L);
        vstore_reg(x, 0, L); break; }
    case ZYDIS_MNEMONIC_VMOVMSKPS: vload(x, 1, "A", L);
        fprintf(out, "      { uint64_t m = 0; for (int i = 0; i < %d; ++i) m |= (uint64_t)(A[i] >> 31) << i; %s = m; }\n", L,
                cname(gpr_index(o[0].reg.value)));
        break;
    case ZYDIS_MNEMONIC_VEXTRACTPS: vload(x, 1, "A", 4);
        fprintf(out, "      R[0] = A[%d];\n", (int)(imm & 3));
        if (dst_mem) vstore_mem(x, 0, 4); else fprintf(out, "      %s = R[0];\n", cname(gpr_index(o[0].reg.value)));
        break;
    case ZYDIS_MNEMONIC_VBROADCASTSS: vload(x, 1, "A", 1);
        fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = A[0];\n", L); vstore_reg(x, 0, L); break;
    case ZYDIS_MNEMONIC_VHADDPS: vload(x, 1, "A", L); vload(x, 2, "B", L);
        fprintf(out, "      for (int h = 0; h < %d; h += 4) { R[h] = XOP(A[h], A[h + 1], +); R[h + 1] = XOP(A[h + 2], A[h + 3], +);"
                     " R[h + 2] = XOP(B[h], B[h + 1], +); R[h + 3] = XOP(B[h + 2], B[h + 3], +); }\n", L);
        vstore_reg(x, 0, L); break;
    case ZYDIS_MNEMONIC_VDPPS: vload(x, 1, "A", L); vload(x, 2, "B", L);
        /* products masked by imm[7:4], summed (p0 + p1) + (p2 + p3), broadcast by imm[3:0] */
        fprintf(out, "      for (int h = 0; h < %d; h += 4) { uint32_t p[4]; for (int i = 0; i < 4; ++i) p[i] = (0x%llxu >> (4 + i)) & 1 ? XOP(A[h + i], B[h + i], *) : 0;\n"
                     "        uint32_t s = XOP(XOP(p[0], p[1], +), XOP(p[2], p[3], +), +);\n"
                     "        for (int i = 0; i < 4; ++i) R[h + i] = (0x%llxu >> i) & 1 ? s : 0; }\n",
                L, (unsigned long long)imm, (unsigned long long)imm);
        vstore_reg(x, 0, L); break;
    case ZYDIS_MNEMONIC_VCMPPS: case ZYDIS_MNEMONIC_VCMPSS: {
        const int scalar = in->mnemonic == ZYDIS_MNEMONIC_VCMPSS;
        vload(x, 1, "A", scalar ? 4 : L); vload(x, 2, "B", scalar ? 1 : L);
        if (scalar) fprintf(out, "      memcpy(R, A, 16);\n");
        fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = XCMP(A[i], B[i], %d) ? ~0u : 0;\n", scalar ? 1 : L, (int)imm);
        vstore_reg(x, 0, scalar ? 4 : L);
        break;
    }
    case ZYDIS_MNEMONIC_VUCOMISS: case ZYDIS_MNEMONIC_VCOMISS:
        vload(x, 0, "A", 1); vload(x, 1, "B", 1);
        fprintf(out, "      { float a = F(A[0]), b = F(B[0]); int un = a != a || b != b;\n"
                     "        zf = un || a == b; pf = un; cf = un || a < b; of = 0; sf = 0; }\n");
        break;
    case ZYDIS_MNEMONIC_VCVTSI2SS: vload(x, 1, "R", 4);
        if (o[2].type == ZYDIS_OPERAND_TYPE_MEMORY) { char a[200]; addr_expr(x, &o[2], a, sizeof(a));
            fprintf(out, "      R[0] = U((float)*(volatile int%u_t *)(uintptr_t)%s);\n", o[2].size, a); }
        else fprintf(out, "      R[0] = U((float)(int%u_t)%s);\n", o[2].size, cname(gpr_index(o[2].reg.value)));
        vstore_reg(x, 0, 4); break;
    case ZYDIS_MNEMONIC_VCVTTSS2SI: vload(x, 1, "A", 1);
        if (o[0].size == 64) fprintf(out, "      %s = XCVTT64(F(A[0]));\n", cname(gpr_index(o[0].reg.value)));
        else fprintf(out, "      %s = (uint32_t)XCVTT32(F(A[0]));\n", cname(gpr_index(o[0].reg.value)));
        break;
    case ZYDIS_MNEMONIC_VCVTDQ2PS: vload(x, 1, "A", L);
        fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = U((float)(int32_t)A[i]);\n", L); vstore_reg(x, 0, L); break;
    case ZYDIS_MNEMONIC_VCVTTPS2DQ: vload(x, 1, "A", L);
        fprintf(out, "      for (int i = 0; i < %d; ++i) R[i] = (uint32_t)XCVTT32(F(A[i]));\n", L); vstore_reg(x, 0, L); break;
    case ZYDIS_MNEMONIC_VCVTSS2SD: vload(x, 1, "R", 4); vload(x, 2, "B", 1);
        fprintf(out, "      { double d = (double)F(B[0]); memcpy(R, &d, 8); }\n"); vstore_reg(x, 0, 4); break;
    case ZYDIS_MNEMONIC_VPADDD: case ZYDIS_MNEMONIC_VPSUBD: vload(x, 1, "A", 4); vload(x, 2, "B", 4);
        fprintf(out, "      for (int i = 0; i < 4; ++i) R[i] = A[i] %c B[i];\n", in->mnemonic == ZYDIS_MNEMONIC_VPADDD ? '+' : '-');
        vstore_reg(x, 0, 4); break;
    case ZYDIS_MNEMONIC_VPADDQ: case ZYDIS_MNEMONIC_VPSUBQ: vload(x, 1, "A", 4); vload(x, 2, "B", 4);
        fprintf(out, "      for (int i = 0; i < 2; ++i) { uint64_t a, b, r; memcpy(&a, &A[2 * i], 8); memcpy(&b, &B[2 * i], 8); r = a %c b; memcpy(&R[2 * i], &r, 8); }\n",
                in->mnemonic == ZYDIS_MNEMONIC_VPADDQ ? '+' : '-');
        vstore_reg(x, 0, 4); break;
    case ZYDIS_MNEMONIC_VPSLLD: case ZYDIS_MNEMONIC_VPSRLD: case ZYDIS_MNEMONIC_VPSRAD: vload(x, 1, "A", 4);
        if (imm > 31) fprintf(out, "      for (int i = 0; i < 4; ++i) R[i] = %s;\n", in->mnemonic == ZYDIS_MNEMONIC_VPSRAD ? "(uint32_t)((int32_t)A[i] >> 31)" : "0");
        else fprintf(out, in->mnemonic == ZYDIS_MNEMONIC_VPSLLD ? "      for (int i = 0; i < 4; ++i) R[i] = A[i] << %u;\n" :
                          in->mnemonic == ZYDIS_MNEMONIC_VPSRLD ? "      for (int i = 0; i < 4; ++i) R[i] = A[i] >> %u;\n" :
                          "      for (int i = 0; i < 4; ++i) R[i] = (uint32_t)((int32_t)A[i] >> %u);\n", (unsigned)imm);
        vstore_reg(x, 0, 4); break;
    case ZYDIS_MNEMONIC_VPSLLQ: case ZYDIS_MNEMONIC_VPSRLQ: vload(x, 1, "A", 4);
        if (imm > 63) fprintf(out, "      memset(R, 0, 16);\n");
        else fprintf(out, "      for (int i = 0; i < 2; ++i) { uint64_t a, r; memcpy(&a, &A[2 * i], 8); r = a %s %u; memcpy(&R[2 * i], &r, 8); }\n",
                     in->mnemonic == ZYDIS_MNEMONIC_VPSLLQ ? "<<" : ">>", (unsigned)imm);
        vstore_reg(x, 0, 4); break;
    case ZYDIS_MNEMONIC_VPCMPEQD: case ZYDIS_MNEMONIC_VPCMPGTD: vload(x, 1, "A", 4); vload(x, 2, "B", 4);
        fprintf(out, "      for (int i = 0; i < 4; ++i) R[i] = %s ? ~0u : 0;\n",
                in->mnemonic == ZYDIS_MNEMONIC_VPCMPEQD ? "A[i] == B[i]" : "(int32_t)A[i] > (int32_t)B[i]");
        vstore_reg(x, 0, 4); break;
    case ZYDIS_MNEMONIC_VPSHUFB: vload(x, 1, "A", 4); vload(x, 2, "B", 4);
        fprintf(out, "      { const uint8_t *a = (const uint8_t *)A, *b = (const uint8_t *)B; uint8_t *r = (uint8_t *)R;\n"
                     "        for (int i = 0; i < 16; ++i) r[i] = (b[i] & 0x80) ? 0 : a[b[i] & 15]; }\n");
        vstore_reg(x, 0, 4); break;
    default:
        fprintf(out, "    }\n");
        return 0;
    }
    fprintf(out, "    }\n");
    return 1;
}

/* Prelude of the generated file: lane helpers with x86 semantics. */
static const char simd_prelude[] =
    "#include <math.h>\n#include <string.h>\n"
    "static inline float F(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }\n"
    "static inline uint32_t U(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }\n"
    "/* x86 picks the NaN of the first operand (quieted) unless only the second is a NaN; ARM\n"
    " * returns a NaN by its own rules: do it by hand. */\n"
    "static inline uint32_t XNAN(uint32_t a, uint32_t b) {\n"
    "    int an = (a & 0x7fffffffu) > 0x7f800000u, bn = (b & 0x7fffffffu) > 0x7f800000u;\n"
    "    return (an ? a : b) | 0x00400000u;\n"
    "}\n"
    "/* a NaN created by an invalid operation (inf - inf, 0 * inf, sqrt(-1)) is x86's \"real\n"
    " * indefinite\" 0xffc00000; ARM's default NaN is 0x7fc00000 */\n"
    "static inline uint32_t XDEF(uint32_t r) { return (r & 0x7fffffffu) > 0x7f800000u ? 0xffc00000u : r; }\n"
    "#define XOP(a, b, op) ({ uint32_t _a = (a), _b = (b); (((_a & 0x7fffffffu) > 0x7f800000u) || ((_b & 0x7fffffffu) > 0x7f800000u)) ? XNAN(_a, _b) : XDEF(U(F(_a) op F(_b))); })\n"
    "static inline uint32_t XSQRT(uint32_t a) { return (a & 0x7fffffffu) > 0x7f800000u ? (a | 0x00400000u) : XDEF(U(sqrtf(F(a)))); }\n"
    "/* maxps/minps: the second operand when either is a NaN or both are zero */\n"
    "static inline uint32_t XMAX(uint32_t a, uint32_t b) { return F(a) > F(b) ? a : b; }\n"
    "static inline uint32_t XMIN(uint32_t a, uint32_t b) { return F(a) < F(b) ? a : b; }\n"
    "static inline int XCMP(uint32_t ua, uint32_t ub, int p) {\n"
    "    float a = F(ua), b = F(ub); int un = a != a || b != b;\n"
    "    switch (p) { case 0: return !un && a == b; case 1: return !un && a < b; case 2: return !un && a <= b;\n"
    "    case 3: return un; case 4: return un || a != b; case 5: return un || !(a < b); case 6: return un || !(a <= b);\n"
    "    default: return !un; }\n"
    "}\n"
    "/* cvttss2si: out of range and NaN give the \"integer indefinite\" value */\n"
    "static inline int32_t XCVTT32(float f) { return (f != f || f >= 2147483648.0f || f < -2147483648.0f) ? INT32_MIN : (int32_t)f; }\n"
    "static inline uint64_t XCVTT64(float f) { return (f != f || f >= 9223372036854775808.0f || f < -9223372036854775808.0f) ? 0x8000000000000000ull : (uint64_t)(int64_t)f; }\n"
    ;
