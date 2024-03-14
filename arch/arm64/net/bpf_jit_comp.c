// SPDX-License-Identifier: GPL-2.0-only
/*
 * BPF JIT compiler for ARM64
 *
 * Copyright (C) 2014-2016 Zi Shen Lim <zlim.lnx@gmail.com>
 */

#define pr_fmt(fmt) "bpf_jit: " fmt

#include <linux/bitfield.h>
#include <linux/bpf.h>
#include <linux/filter.h>
#include <linux/memory.h>
#include <linux/printk.h>
#include <linux/slab.h>

#include <asm/asm-extable.h>
#include <asm/byteorder.h>
#include <asm/cacheflush.h>
#include <asm/debug-monitors.h>
#include <asm/insn.h>
#include <asm/patching.h>
#include <asm/set_memory.h>

#include "bpf_jit.h"

#define TMP_REG_1 (MAX_BPF_JIT_REG + 0)
#define TMP_REG_2 (MAX_BPF_JIT_REG + 1)
#define TCALL_CNT (MAX_BPF_JIT_REG + 2)
#define TMP_REG_3 (MAX_BPF_JIT_REG + 3)
#define FP_BOTTOM (MAX_BPF_JIT_REG + 4)

#define check_imm(bits, imm) do {				\
	if ((((imm) > 0) && ((imm) >> (bits))) ||		\
	    (((imm) < 0) && (~(imm) >> (bits)))) {		\
		pr_info("[%2d] imm=%d(0x%x) out of range\n",	\
			i, imm, imm);				\
		return -EINVAL;					\
	}							\
} while (0)
#define check_imm19(imm) check_imm(19, imm)
#define check_imm26(imm) check_imm(26, imm)

/* Map BPF registers to A64 registers */
static const int bpf2a64[] = {
	/* return value from in-kernel function, and exit value from eBPF */
	[BPF_REG_0] = A64_R(7),
	/* arguments from eBPF program to in-kernel function */
	[BPF_REG_1] = A64_R(0),
	[BPF_REG_2] = A64_R(1),
	[BPF_REG_3] = A64_R(2),
	[BPF_REG_4] = A64_R(3),
	[BPF_REG_5] = A64_R(4),
	/* callee saved registers that in-kernel function will preserve */
	[BPF_REG_6] = A64_R(19),
	[BPF_REG_7] = A64_R(20),
	[BPF_REG_8] = A64_R(21),
	[BPF_REG_9] = A64_R(22),
	/* read-only frame pointer to access stack */
	[BPF_REG_FP] = A64_R(25),
	/* temporary registers for BPF JIT */
	[TMP_REG_1] = A64_R(10),
	[TMP_REG_2] = A64_R(11),
	[TMP_REG_3] = A64_R(12),
	/* tail_call_cnt */
	[TCALL_CNT] = A64_R(26),
	/* temporary register for blinding constants */
	[BPF_REG_AX] = A64_R(9),
	[FP_BOTTOM] = A64_R(27),
};

struct jit_ctx {
	const struct bpf_prog *prog;
	int idx;
	int epilogue_offset;
	int *offset;
	int exentry_idx;
	__le32 *image;
	u32 stack_size;
	int fpb_offset;
};

struct bpf_plt {
	u32 insn_ldr; /* load target */
	u32 insn_br;  /* branch to target */
	u64 target;   /* target value */
};

#define PLT_TARGET_SIZE   sizeof_field(struct bpf_plt, target)
#define PLT_TARGET_OFFSET offsetof(struct bpf_plt, target)

static inline void emit(const u32 insn, struct jit_ctx *ctx)
{
	if (ctx->image != NULL)
		ctx->image[ctx->idx] = cpu_to_le32(insn);

	ctx->idx++;
}

static inline void emit_a64_mov_i(const int is64, const int reg,
				  const s32 val, struct jit_ctx *ctx)
{
	u16 hi = val >> 16;
	u16 lo = val & 0xffff;

	if (hi & 0x8000) {
		if (hi == 0xffff) {
			emit(A64_MOVN(is64, reg, (u16)~lo, 0), ctx);
		} else {
			emit(A64_MOVN(is64, reg, (u16)~hi, 16), ctx);
			if (lo != 0xffff)
				emit(A64_MOVK(is64, reg, lo, 0), ctx);
		}
	} else {
		emit(A64_MOVZ(is64, reg, lo, 0), ctx);
		if (hi)
			emit(A64_MOVK(is64, reg, hi, 16), ctx);
	}
}

static int i64_i16_blocks(const u64 val, bool inverse)
{
	return (((val >>  0) & 0xffff) != (inverse ? 0xffff : 0x0000)) +
	       (((val >> 16) & 0xffff) != (inverse ? 0xffff : 0x0000)) +
	       (((val >> 32) & 0xffff) != (inverse ? 0xffff : 0x0000)) +
	       (((val >> 48) & 0xffff) != (inverse ? 0xffff : 0x0000));
}

static inline void emit_a64_mov_i64(const int reg, const u64 val,
				    struct jit_ctx *ctx)
{
	u64 nrm_tmp = val, rev_tmp = ~val;
	bool inverse;
	int shift;

	if (!(nrm_tmp >> 32))
		return emit_a64_mov_i(0, reg, (u32)val, ctx);

	inverse = i64_i16_blocks(nrm_tmp, true) < i64_i16_blocks(nrm_tmp, false);
	shift = max(round_down((inverse ? (fls64(rev_tmp) - 1) :
					  (fls64(nrm_tmp) - 1)), 16), 0);
	if (inverse)
		emit(A64_MOVN(1, reg, (rev_tmp >> shift) & 0xffff, shift), ctx);
	else
		emit(A64_MOVZ(1, reg, (nrm_tmp >> shift) & 0xffff, shift), ctx);
	shift -= 16;
	while (shift >= 0) {
		if (((nrm_tmp >> shift) & 0xffff) != (inverse ? 0xffff : 0x0000))
			emit(A64_MOVK(1, reg, (nrm_tmp >> shift) & 0xffff, shift), ctx);
		shift -= 16;
	}
}

static inline void emit_bti(u32 insn, struct jit_ctx *ctx)
{
	if (IS_ENABLED(CONFIG_ARM64_BTI_KERNEL))
		emit(insn, ctx);
}

/*
 * Kernel addresses in the vmalloc space use at most 48 bits, and the
 * remaining bits are guaranteed to be 0x1. So we can compose the address
 * with a fixed length movn/movk/movk sequence.
 */
static inline void emit_addr_mov_i64(const int reg, const u64 val,
				     struct jit_ctx *ctx)
{
	u64 tmp = val;
	int shift = 0;

	emit(A64_MOVN(1, reg, ~tmp & 0xffff, shift), ctx);
	while (shift < 32) {
		tmp >>= 16;
		shift += 16;
		emit(A64_MOVK(1, reg, tmp & 0xffff, shift), ctx);
	}
}

static inline void emit_call(u64 target, struct jit_ctx *ctx)
{
	u8 tmp = bpf2a64[TMP_REG_1];

	emit_addr_mov_i64(tmp, target, ctx);
	emit(A64_BLR(tmp), ctx);
}

static inline int bpf2a64_offset(int bpf_insn, int off,
				 const struct jit_ctx *ctx)
{
	/* BPF JMP offset is relative to the next instruction */
	bpf_insn++;
	/*
	 * Whereas arm64 branch instructions encode the offset
	 * from the branch itself, so we must subtract 1 from the
	 * instruction offset.
	 */
	return ctx->offset[bpf_insn + off] - (ctx->offset[bpf_insn] - 1);
}

static void jit_fill_hole(void *area, unsigned int size)
{
	__le32 *ptr;
	/* We are guaranteed to have aligned memory. */
	for (ptr = area; size >= sizeof(u32); size -= sizeof(u32))
		*ptr++ = cpu_to_le32(AARCH64_BREAK_FAULT);
}

static inline int epilogue_offset(const struct jit_ctx *ctx)
{
	int to = ctx->epilogue_offset;
	int from = ctx->idx;

	return to - from;
}

static bool is_addsub_imm(u32 imm)
{
	/* Either imm12 or shifted imm12. */
	return !(imm & ~0xfff) || !(imm & ~0xfff000);
}

/*
 * There are 3 types of AArch64 LDR/STR (immediate) instruction:
 * Post-index, Pre-index, Unsigned offset.
 *
 * For BPF ldr/str, the "unsigned offset" type is sufficient.
 *
 * "Unsigned offset" type LDR(immediate) format:
 *
 *    3                   2                   1                   0
 *  1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |x x|1 1 1 0 0 1 0 1|         imm12         |    Rn   |    Rt   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * scale
 *
 * "Unsigned offset" type STR(immediate) format:
 *    3                   2                   1                   0
 *  1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |x x|1 1 1 0 0 1 0 0|         imm12         |    Rn   |    Rt   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * scale
 *
 * The offset is calculated from imm12 and scale in the following way:
 *
 * offset = (u64)imm12 << scale
 */
static bool is_lsi_offset(int offset, int scale)
{
	if (offset < 0)
		return false;

	if (offset > (0xFFF << scale))
		return false;

	if (offset & ((1 << scale) - 1))
		return false;

	return true;
}

/* generated prologue:
 *      bti c // if CONFIG_ARM64_BTI_KERNEL
 *      mov x9, lr
 *      nop  // POKE_OFFSET
 *      paciasp // if CONFIG_ARM64_PTR_AUTH_KERNEL
 *      stp x29, lr, [sp, #-16]!
 *      mov x29, sp
 *      stp x19, x20, [sp, #-16]!
 *      stp x21, x22, [sp, #-16]!
 *      stp x25, x26, [sp, #-16]!
 *      stp x27, x28, [sp, #-16]!
 *      mov x25, sp
 *      mov tcc, #0
 *      // PROLOGUE_OFFSET
 */

#define BTI_INSNS (IS_ENABLED(CONFIG_ARM64_BTI_KERNEL) ? 1 : 0)
#define PAC_INSNS (IS_ENABLED(CONFIG_ARM64_PTR_AUTH_KERNEL) ? 1 : 0)

/* Offset of nop instruction in bpf prog entry to be poked */
#define POKE_OFFSET (BTI_INSNS + 1)

/* Tail call offset to jump into */
#define PROLOGUE_OFFSET (BTI_INSNS + 2 + PAC_INSNS + 8)

static int build_prologue(struct jit_ctx *ctx, bool ebpf_from_cbpf)
{
	const struct bpf_prog *prog = ctx->prog;
	const bool is_main_prog = prog->aux->func_idx == 0;
	const u8 r6 = bpf2a64[BPF_REG_6];
	const u8 r7 = bpf2a64[BPF_REG_7];
	const u8 r8 = bpf2a64[BPF_REG_8];
	const u8 r9 = bpf2a64[BPF_REG_9];
	const u8 fp = bpf2a64[BPF_REG_FP];
	const u8 tcc = bpf2a64[TCALL_CNT];
	const u8 fpb = bpf2a64[FP_BOTTOM];
	const int idx0 = ctx->idx;
	int cur_offset;

	/*
	 * BPF prog stack layout
	 *
	 *                         high
	 * original A64_SP =>   0:+-----+ BPF prologue
	 *                        |FP/LR|
	 * current A64_FP =>  -16:+-----+
	 *                        | ... | callee saved registers
	 * BPF fp register => -64:+-----+ <= (BPF_FP)
	 *                        |     |
	 *                        | ... | BPF prog stack
	 *                        |     |
	 *                        +-----+ <= (BPF_FP - prog->aux->stack_depth)
	 *                        |RSVD | padding
	 * current A64_SP =>      +-----+ <= (BPF_FP - ctx->stack_size)
	 *                        |     |
	 *                        | ... | Function call stack
	 *                        |     |
	 *                        +-----+
	 *                          low
	 *
	 */

	/* bpf function may be invoked by 3 instruction types:
	 * 1. bl, attached via freplace to bpf prog via short jump
	 * 2. br, attached via freplace to bpf prog via long jump
	 * 3. blr, working as a function pointer, used by emit_call.
	 * So BTI_JC should used here to support both br and blr.
	 */
	emit_bti(A64_BTI_JC, ctx);

	emit(A64_MOV(1, A64_R(9), A64_LR), ctx);
	emit(A64_NOP, ctx);

	/* Sign lr */
	if (IS_ENABLED(CONFIG_ARM64_PTR_AUTH_KERNEL))
		emit(A64_PACIASP, ctx);

	/* Save FP and LR registers to stay align with ARM64 AAPCS */
	emit(A64_PUSH(A64_FP, A64_LR, A64_SP), ctx);
	emit(A64_MOV(1, A64_FP, A64_SP), ctx);

	/* Save callee-saved registers */
	emit(A64_PUSH(r6, r7, A64_SP), ctx);
	emit(A64_PUSH(r8, r9, A64_SP), ctx);
	emit(A64_PUSH(fp, tcc, A64_SP), ctx);
	emit(A64_PUSH(fpb, A64_R(28), A64_SP), ctx);

	/* Set up BPF prog stack base register */
	emit(A64_MOV(1, fp, A64_SP), ctx);

	if (!ebpf_from_cbpf && is_main_prog) {
		/* Initialize tail_call_cnt */
		emit(A64_MOVZ(1, tcc, 0, 0), ctx);

		cur_offset = ctx->idx - idx0;
		if (cur_offset != PROLOGUE_OFFSET) {
			pr_err_once("PROLOGUE_OFFSET = %d, expected %d!\n",
				    cur_offset, PROLOGUE_OFFSET);
			return -1;
		}

		/* BTI landing pad for the tail call, done with a BR */
		emit_bti(A64_BTI_J, ctx);
	}

	emit(A64_SUB_I(1, fpb, fp, ctx->fpb_offset), ctx);

	/* Stack must be multiples of 16B */
	ctx->stack_size = round_up(prog->aux->stack_depth, 16);

	/* Set up function call stack */
	emit(A64_SUB_I(1, A64_SP, A64_SP, ctx->stack_size), ctx);
	return 0;
}

static int out_offset = -1; /* initialized on the first pass of build_body() */
static int emit_bpf_tail_call(struct jit_ctx *ctx)
{
	/* bpf_tail_call(void *prog_ctx, struct bpf_array *array, u64 index) */
	const u8 r2 = bpf2a64[BPF_REG_2];
	const u8 r3 = bpf2a64[BPF_REG_3];

	const u8 tmp = bpf2a64[TMP_REG_1];
	const u8 prg = bpf2a64[TMP_REG_2];
	const u8 tcc = bpf2a64[TCALL_CNT];
	const int idx0 = ctx->idx;
#define cur_offset (ctx->idx - idx0)
#define jmp_offset (out_offset - (cur_offset))
	size_t off;

	/* if (index >= array->map.max_entries)
	 *     goto out;
	 */
	off = offsetof(struct bpf_array, map.max_entries);
	emit_a64_mov_i64(tmp, off, ctx);
	emit(A64_LDR32(tmp, r2, tmp), ctx);
	emit(A64_MOV(0, r3, r3), ctx);
	emit(A64_CMP(0, r3, tmp), ctx);
	emit(A64_B_(A64_COND_CS, jmp_offset), ctx);

	/*
	 * if (tail_call_cnt >= MAX_TAIL_CALL_CNT)
	 *     goto out;
	 * tail_call_cnt++;
	 */
	emit_a64_mov_i64(tmp, MAX_TAIL_CALL_CNT, ctx);
	emit(A64_CMP(1, tcc, tmp), ctx);
	emit(A64_B_(A64_COND_CS, jmp_offset), ctx);
	emit(A64_ADD_I(1, tcc, tcc, 1), ctx);

	/* prog = array->ptrs[index];
	 * if (prog == NULL)
	 *     goto out;
	 */
	off = offsetof(struct bpf_array, ptrs);
	emit_a64_mov_i64(tmp, off, ctx);
	emit(A64_ADD(1, tmp, r2, tmp), ctx);
	emit(A64_LSL(1, prg, r3, 3), ctx);
	emit(A64_LDR64(prg, tmp, prg), ctx);
	emit(A64_CBZ(1, prg, jmp_offset), ctx);

	/* goto *(prog->bpf_func + prologue_offset); */
	off = offsetof(struct bpf_prog, bpf_func);
	emit_a64_mov_i64(tmp, off, ctx);
	emit(A64_LDR64(tmp, prg, tmp), ctx);
	emit(A64_ADD_I(1, tmp, tmp, sizeof(u32) * PROLOGUE_OFFSET), ctx);
	emit(A64_ADD_I(1, A64_SP, A64_SP, ctx->stack_size), ctx);
	emit(A64_BR(tmp), ctx);

	/* out: */
	if (out_offset == -1)
		out_offset = cur_offset;
	if (cur_offset != out_offset) {
		pr_err_once("tail_call out_offset = %d, expected %d!\n",
			    cur_offset, out_offset);
		return -1;
	}
	return 0;
#undef cur_offset
#undef jmp_offset
}

#ifdef CONFIG_ARM64_LSE_ATOMICS
static int emit_lse_atomic(const struct bpf_insn *insn, struct jit_ctx *ctx)
{
	const u8 code = insn->code;
	const u8 dst = bpf2a64[insn->dst_reg];
	const u8 src = bpf2a64[insn->src_reg];
	const u8 tmp = bpf2a64[TMP_REG_1];
	const u8 tmp2 = bpf2a64[TMP_REG_2];
	const bool isdw = BPF_SIZE(code) == BPF_DW;
	const s16 off = insn->off;
	u8 reg;

	if (!off) {
		reg = dst;
	} else {
		emit_a64_mov_i(1, tmp, off, ctx);
		emit(A64_ADD(1, tmp, tmp, dst), ctx);
		reg = tmp;
	}

	switch (insn->imm) {
	/* lock *(u32/u64 *)(dst_reg + off) <op>= src_reg */
	case BPF_ADD:
		emit(A64_STADD(isdw, reg, src), ctx);
		break;
	case BPF_AND:
		emit(A64_MVN(isdw, tmp2, src), ctx);
		emit(A64_STCLR(isdw, reg, tmp2), ctx);
		break;
	case BPF_OR:
		emit(A64_STSET(isdw, reg, src), ctx);
		break;
	case BPF_XOR:
		emit(A64_STEOR(isdw, reg, src), ctx);
		break;
	/* src_reg = atomic_fetch_<op>(dst_reg + off, src_reg) */
	case BPF_ADD | BPF_FETCH:
		emit(A64_LDADDAL(isdw, src, reg, src), ctx);
		break;
	case BPF_AND | BPF_FETCH:
		emit(A64_MVN(isdw, tmp2, src), ctx);
		emit(A64_LDCLRAL(isdw, src, reg, tmp2), ctx);
		break;
	case BPF_OR | BPF_FETCH:
		emit(A64_LDSETAL(isdw, src, reg, src), ctx);
		break;
	case BPF_XOR | BPF_FETCH:
		emit(A64_LDEORAL(isdw, src, reg, src), ctx);
		break;
	/* src_reg = atomic_xchg(dst_reg + off, src_reg); */
	case BPF_XCHG:
		emit(A64_SWPAL(isdw, src, reg, src), ctx);
		break;
	/* r0 = atomic_cmpxchg(dst_reg + off, r0, src_reg); */
	case BPF_CMPXCHG:
		emit(A64_CASAL(isdw, src, reg, bpf2a64[BPF_REG_0]), ctx);
		break;
	default:
		pr_err_once("unknown atomic op code %02x\n", insn->imm);
		return -EINVAL;
	}

	return 0;
}
#else
static inline int emit_lse_atomic(const struct bpf_insn *insn, struct jit_ctx *ctx)
{
	return -EINVAL;
}
#endif

static int emit_ll_sc_atomic(const struct bpf_insn *insn, struct jit_ctx *ctx)
{
	const u8 code = insn->code;
	const u8 dst = bpf2a64[insn->dst_reg];
	const u8 src = bpf2a64[insn->src_reg];
	const u8 tmp = bpf2a64[TMP_REG_1];
	const u8 tmp2 = bpf2a64[TMP_REG_2];
	const u8 tmp3 = bpf2a64[TMP_REG_3];
	const int i = insn - ctx->prog->insnsi;
	const s32 imm = insn->imm;
	const s16 off = insn->off;
	const bool isdw = BPF_SIZE(code) == BPF_DW;
	u8 reg;
	s32 jmp_offset;

	if (!off) {
		reg = dst;
	} else {
		emit_a64_mov_i(1, tmp, off, ctx);
		emit(A64_ADD(1, tmp, tmp, dst), ctx);
		reg = tmp;
	}

	if (imm == BPF_ADD || imm == BPF_AND ||
	    imm == BPF_OR || imm == BPF_XOR) {
		/* lock *(u32/u64 *)(dst_reg + off) <op>= src_reg */
		emit(A64_LDXR(isdw, tmp2, reg), ctx);
		if (imm == BPF_ADD)
			emit(A64_ADD(isdw, tmp2, tmp2, src), ctx);
		else if (imm == BPF_AND)
			emit(A64_AND(isdw, tmp2, tmp2, src), ctx);
		else if (imm == BPF_OR)
			emit(A64_ORR(isdw, tmp2, tmp2, src), ctx);
		else
			emit(A64_EOR(isdw, tmp2, tmp2, src), ctx);
		emit(A64_STXR(isdw, tmp2, reg, tmp3), ctx);
		jmp_offset = -3;
		check_imm19(jmp_offset);
		emit(A64_CBNZ(0, tmp3, jmp_offset), ctx);
	} else if (imm == (BPF_ADD | BPF_FETCH) ||
		   imm == (BPF_AND | BPF_FETCH) ||
		   imm == (BPF_OR | BPF_FETCH) ||
		   imm == (BPF_XOR | BPF_FETCH)) {
		/* src_reg = atomic_fetch_<op>(dst_reg + off, src_reg) */
		const u8 ax = bpf2a64[BPF_REG_AX];

		emit(A64_MOV(isdw, ax, src), ctx);
		emit(A64_LDXR(isdw, src, reg), ctx);
		if (imm == (BPF_ADD | BPF_FETCH))
			emit(A64_ADD(isdw, tmp2, src, ax), ctx);
		else if (imm == (BPF_AND | BPF_FETCH))
			emit(A64_AND(isdw, tmp2, src, ax), ctx);
		else if (imm == (BPF_OR | BPF_FETCH))
			emit(A64_ORR(isdw, tmp2, src, ax), ctx);
		else
			emit(A64_EOR(isdw, tmp2, src, ax), ctx);
		emit(A64_STLXR(isdw, tmp2, reg, tmp3), ctx);
		jmp_offset = -3;
		check_imm19(jmp_offset);
		emit(A64_CBNZ(0, tmp3, jmp_offset), ctx);
		emit(A64_DMB_ISH, ctx);
	} else if (imm == BPF_XCHG) {
		/* src_reg = atomic_xchg(dst_reg + off, src_reg); */
		emit(A64_MOV(isdw, tmp2, src), ctx);
		emit(A64_LDXR(isdw, src, reg), ctx);
		emit(A64_STLXR(isdw, tmp2, reg, tmp3), ctx);
		jmp_offset = -2;
		check_imm19(jmp_offset);
		emit(A64_CBNZ(0, tmp3, jmp_offset), ctx);
		emit(A64_DMB_ISH, ctx);
	} else if (imm == BPF_CMPXCHG) {
		/* r0 = atomic_cmpxchg(dst_reg + off, r0, src_reg); */
		const u8 r0 = bpf2a64[BPF_REG_0];

		emit(A64_MOV(isdw, tmp2, r0), ctx);
		emit(A64_LDXR(isdw, r0, reg), ctx);
		emit(A64_EOR(isdw, tmp3, r0, tmp2), ctx);
		jmp_offset = 4;
		check_imm19(jmp_offset);
		emit(A64_CBNZ(isdw, tmp3, jmp_offset), ctx);
		emit(A64_STLXR(isdw, src, reg, tmp3), ctx);
		jmp_offset = -4;
		check_imm19(jmp_offset);
		emit(A64_CBNZ(0, tmp3, jmp_offset), ctx);
		emit(A64_DMB_ISH, ctx);
	} else {
		pr_err_once("unknown atomic op code %02x\n", imm);
		return -EINVAL;
	}

	return 0;
}

void dummy_tramp(void);

asm (
"	.pushsection .text, \"ax\", @progbits\n"
"	.global dummy_tramp\n"
"	.type dummy_tramp, %function\n"
"dummy_tramp:"
#if IS_ENABLED(CONFIG_ARM64_BTI_KERNEL)
"	bti j\n" /* dummy_tramp is called via "br x10" */
#endif
"	mov x10, x30\n"
"	mov x30, x9\n"
"	ret x10\n"
"	.size dummy_tramp, .-dummy_tramp\n"
"	.popsection\n"
);

/* build a plt initialized like this:
 *
 * plt:
 *      ldr tmp, target
 *      br tmp
 * target:
 *      .quad dummy_tramp
 *
 * when a long jump trampoline is attached, target is filled with the
 * trampoline address, and when the trampoline is removed, target is
 * restored to dummy_tramp address.
 */
static void build_plt(struct jit_ctx *ctx)
{
	const u8 tmp = bpf2a64[TMP_REG_1];
	struct bpf_plt *plt = NULL;

	/* make sure target is 64-bit aligned */
	if ((ctx->idx + PLT_TARGET_OFFSET / AARCH64_INSN_SIZE) % 2)
		emit(A64_NOP, ctx);

	plt = (struct bpf_plt *)(ctx->image + ctx->idx);
	/* plt is called via bl, no BTI needed here */
	emit(A64_LDR64LIT(tmp, 2 * AARCH64_INSN_SIZE), ctx);
	emit(A64_BR(tmp), ctx);

	if (ctx->image)
		plt->target = (u64)&dummy_tramp;
}

static void build_epilogue(struct jit_ctx *ctx)
{
	const u8 r0 = bpf2a64[BPF_REG_0];
	const u8 r6 = bpf2a64[BPF_REG_6];
	const u8 r7 = bpf2a64[BPF_REG_7];
	const u8 r8 = bpf2a64[BPF_REG_8];
	const u8 r9 = bpf2a64[BPF_REG_9];
	const u8 fp = bpf2a64[BPF_REG_FP];
	const u8 fpb = bpf2a64[FP_BOTTOM];

	/* We're done with BPF stack */
	emit(A64_ADD_I(1, A64_SP, A64_SP, ctx->stack_size), ctx);

	/* Restore x27 and x28 */
	emit(A64_POP(fpb, A64_R(28), A64_SP), ctx);
	/* Restore fs (x25) and x26 */
	emit(A64_POP(fp, A64_R(26), A64_SP), ctx);

	/* Restore callee-saved register */
	emit(A64_POP(r8, r9, A64_SP), ctx);
	emit(A64_POP(r6, r7, A64_SP), ctx);

	/* Restore FP/LR registers */
	emit(A64_POP(A64_FP, A64_LR, A64_SP), ctx);

	/* Set return value */
	emit(A64_MOV(1, A64_R(0), r0), ctx);

	/* Authenticate lr */
	if (IS_ENABLED(CONFIG_ARM64_PTR_AUTH_KERNEL))
		emit(A64_AUTIASP, ctx);

	emit(A64_RET(A64_LR), ctx);
}

#define BPF_FIXUP_OFFSET_MASK	GENMASK(26, 0)
#define BPF_FIXUP_REG_MASK	GENMASK(31, 27)

bool ex_handler_bpf(const struct exception_table_entry *ex,
		    struct pt_regs *regs)
{
	off_t offset = FIELD_GET(BPF_FIXUP_OFFSET_MASK, ex->fixup);
	int dst_reg = FIELD_GET(BPF_FIXUP_REG_MASK, ex->fixup);

	regs->regs[dst_reg] = 0;
	regs->pc = (unsigned long)&ex->fixup - offset;
	return true;
}

/* For accesses to BTF pointers, add an entry to the exception table */
static int add_exception_handler(const struct bpf_insn *insn,
				 struct jit_ctx *ctx,
				 int dst_reg)
{
	off_t offset;
	unsigned long pc;
	struct exception_table_entry *ex;

	if (!ctx->image)
		/* First pass */
		return 0;

	if (BPF_MODE(insn->code) != BPF_PROBE_MEM &&
		BPF_MODE(insn->code) != BPF_PROBE_MEMSX)
		return 0;

	if (!ctx->prog->aux->extable ||
	    WARN_ON_ONCE(ctx->exentry_idx >= ctx->prog->aux->num_exentries))
		return -EINVAL;

	ex = &ctx->prog->aux->extable[ctx->exentry_idx];
	pc = (unsigned long)&ctx->image[ctx->idx - 1];

	offset = pc - (long)&ex->insn;
	if (WARN_ON_ONCE(offset >= 0 || offset < INT_MIN))
		return -ERANGE;
	ex->insn = offset;

	/*
	 * Since the extable follows the program, the fixup offset is always
	 * negative and limited to BPF_JIT_REGION_SIZE. Store a positive value
	 * to keep things simple, and put the destination register in the upper
	 * bits. We don't need to worry about buildtime or runtime sort
	 * modifying the upper bits because the table is already sorted, and
	 * isn't part of the main exception table.
	 */
	offset = (long)&ex->fixup - (pc + AARCH64_INSN_SIZE);
	if (!FIELD_FIT(BPF_FIXUP_OFFSET_MASK, offset))
		return -ERANGE;

	ex->fixup = FIELD_PREP(BPF_FIXUP_OFFSET_MASK, offset) |
		    FIELD_PREP(BPF_FIXUP_REG_MASK, dst_reg);

	ex->type = EX_TYPE_BPF;

	ctx->exentry_idx++;
	return 0;
}

/* JITs an eBPF instruction.
 * Returns:
 * 0  - successfully JITed an 8-byte eBPF instruction.
 * >0 - successfully JITed a 16-byte eBPF instruction.
 * <0 - failed to JIT.
 */
static int build_insn(const struct bpf_insn *insn, struct jit_ctx *ctx,
		      bool extra_pass)
{
	const u8 code = insn->code;
	const u8 dst = bpf2a64[insn->dst_reg];
	const u8 src = bpf2a64[insn->src_reg];
	const u8 tmp = bpf2a64[TMP_REG_1];
	const u8 tmp2 = bpf2a64[TMP_REG_2];
	const u8 fp = bpf2a64[BPF_REG_FP];
	const u8 fpb = bpf2a64[FP_BOTTOM];
	const s16 off = insn->off;
	const s32 imm = insn->imm;
	const int i = insn - ctx->prog->insnsi;
	const bool is64 = BPF_CLASS(code) == BPF_ALU64 ||
			  BPF_CLASS(code) == BPF_JMP;
	u8 jmp_cond;
	s32 jmp_offset;
	u32 a64_insn;
	u8 src_adj;
	u8 dst_adj;
	int off_adj;
	int ret;
	bool sign_extend;

	switch (code) {
	/* dst = src */
	case BPF_ALU | BPF_MOV | BPF_X:
	case BPF_ALU64 | BPF_MOV | BPF_X:
		switch (insn->off) {
		case 0:
			emit(A64_MOV(is64, dst, src), ctx);
			break;
		case 8:
			emit(A64_SXTB(is64, dst, src), ctx);
			break;
		case 16:
			emit(A64_SXTH(is64, dst, src), ctx);
			break;
		case 32:
			emit(A64_SXTW(is64, dst, src), ctx);
			break;
		}
		break;
	/* dst = dst OP src */
	case BPF_ALU | BPF_ADD | BPF_X:
	case BPF_ALU64 | BPF_ADD | BPF_X:
		emit(A64_ADD(is64, dst, dst, src), ctx);
		break;
	case BPF_ALU | BPF_SUB | BPF_X:
	case BPF_ALU64 | BPF_SUB | BPF_X:
		emit(A64_SUB(is64, dst, dst, src), ctx);
		break;
	case BPF_ALU | BPF_AND | BPF_X:
	case BPF_ALU64 | BPF_AND | BPF_X:
		emit(A64_AND(is64, dst, dst, src), ctx);
		break;
	case BPF_ALU | BPF_OR | BPF_X:
	case BPF_ALU64 | BPF_OR | BPF_X:
		emit(A64_ORR(is64, dst, dst, src), ctx);
		break;
	case BPF_ALU | BPF_XOR | BPF_X:
	case BPF_ALU64 | BPF_XOR | BPF_X:
		emit(A64_EOR(is64, dst, dst, src), ctx);
		break;
	case BPF_ALU | BPF_MUL | BPF_X:
	case BPF_ALU64 | BPF_MUL | BPF_X:
		emit(A64_MUL(is64, dst, dst, src), ctx);
		break;
	case BPF_ALU | BPF_DIV | BPF_X:
	case BPF_ALU64 | BPF_DIV | BPF_X:
		if (!off)
			emit(A64_UDIV(is64, dst, dst, src), ctx);
		else
			emit(A64_SDIV(is64, dst, dst, src), ctx);
		break;
	case BPF_ALU | BPF_MOD | BPF_X:
	case BPF_ALU64 | BPF_MOD | BPF_X:
		if (!off)
			emit(A64_UDIV(is64, tmp, dst, src), ctx);
		else
			emit(A64_SDIV(is64, tmp, dst, src), ctx);
		emit(A64_MSUB(is64, dst, dst, tmp, src), ctx);
		break;
	case BPF_ALU | BPF_LSH | BPF_X:
	case BPF_ALU64 | BPF_LSH | BPF_X:
		emit(A64_LSLV(is64, dst, dst, src), ctx);
		break;
	case BPF_ALU | BPF_RSH | BPF_X:
	case BPF_ALU64 | BPF_RSH | BPF_X:
		emit(A64_LSRV(is64, dst, dst, src), ctx);
		break;
	case BPF_ALU | BPF_ARSH | BPF_X:
	case BPF_ALU64 | BPF_ARSH | BPF_X:
		emit(A64_ASRV(is64, dst, dst, src), ctx);
		break;
	/* dst = -dst */
	case BPF_ALU | BPF_NEG:
	case BPF_ALU64 | BPF_NEG:
		emit(A64_NEG(is64, dst, dst), ctx);
		break;
	/* dst = BSWAP##imm(dst) */
	case BPF_ALU | BPF_END | BPF_FROM_LE:
	case BPF_ALU | BPF_END | BPF_FROM_BE:
	case BPF_ALU64 | BPF_END | BPF_FROM_LE:
#ifdef CONFIG_CPU_BIG_ENDIAN
		if (BPF_CLASS(code) == BPF_ALU && BPF_SRC(code) == BPF_FROM_BE)
			goto emit_bswap_uxt;
#else /* !CONFIG_CPU_BIG_ENDIAN */
		if (BPF_CLASS(code) == BPF_ALU && BPF_SRC(code) == BPF_FROM_LE)
			goto emit_bswap_uxt;
#endif
		switch (imm) {
		case 16:
			emit(A64_REV16(is64, dst, dst), ctx);
			/* zero-extend 16 bits into 64 bits */
			emit(A64_UXTH(is64, dst, dst), ctx);
			break;
		case 32:
			emit(A64_REV32(is64, dst, dst), ctx);
			/* upper 32 bits already cleared */
			break;
		case 64:
			emit(A64_REV64(dst, dst), ctx);
			break;
		}
		break;
emit_bswap_uxt:
		switch (imm) {
		case 16:
			/* zero-extend 16 bits into 64 bits */
			emit(A64_UXTH(is64, dst, dst), ctx);
			break;
		case 32:
			/* zero-extend 32 bits into 64 bits */
			emit(A64_UXTW(is64, dst, dst), ctx);
			break;
		case 64:
			/* nop */
			break;
		}
		break;
	/* dst = imm */
	case BPF_ALU | BPF_MOV | BPF_K:
	case BPF_ALU64 | BPF_MOV | BPF_K:
		emit_a64_mov_i(is64, dst, imm, ctx);
		break;
	/* dst = dst OP imm */
	case BPF_ALU | BPF_ADD | BPF_K:
	case BPF_ALU64 | BPF_ADD | BPF_K:
		if (is_addsub_imm(imm)) {
			emit(A64_ADD_I(is64, dst, dst, imm), ctx);
		} else if (is_addsub_imm(-imm)) {
			emit(A64_SUB_I(is64, dst, dst, -imm), ctx);
		} else {
			emit_a64_mov_i(is64, tmp, imm, ctx);
			emit(A64_ADD(is64, dst, dst, tmp), ctx);
		}
		break;
	case BPF_ALU | BPF_SUB | BPF_K:
	case BPF_ALU64 | BPF_SUB | BPF_K:
		if (is_addsub_imm(imm)) {
			emit(A64_SUB_I(is64, dst, dst, imm), ctx);
		} else if (is_addsub_imm(-imm)) {
			emit(A64_ADD_I(is64, dst, dst, -imm), ctx);
		} else {
			emit_a64_mov_i(is64, tmp, imm, ctx);
			emit(A64_SUB(is64, dst, dst, tmp), ctx);
		}
		break;
	case BPF_ALU | BPF_AND | BPF_K:
	case BPF_ALU64 | BPF_AND | BPF_K:
		a64_insn = A64_AND_I(is64, dst, dst, imm);
		if (a64_insn != AARCH64_BREAK_FAULT) {
			emit(a64_insn, ctx);
		} else {
			emit_a64_mov_i(is64, tmp, imm, ctx);
			emit(A64_AND(is64, dst, dst, tmp), ctx);
		}
		break;
	case BPF_ALU | BPF_OR | BPF_K:
	case BPF_ALU64 | BPF_OR | BPF_K:
		a64_insn = A64_ORR_I(is64, dst, dst, imm);
		if (a64_insn != AARCH64_BREAK_FAULT) {
			emit(a64_insn, ctx);
		} else {
			emit_a64_mov_i(is64, tmp, imm, ctx);
			emit(A64_ORR(is64, dst, dst, tmp), ctx);
		}
		break;
	case BPF_ALU | BPF_XOR | BPF_K:
	case BPF_ALU64 | BPF_XOR | BPF_K:
		a64_insn = A64_EOR_I(is64, dst, dst, imm);
		if (a64_insn != AARCH64_BREAK_FAULT) {
			emit(a64_insn, ctx);
		} else {
			emit_a64_mov_i(is64, tmp, imm, ctx);
			emit(A64_EOR(is64, dst, dst, tmp), ctx);
		}
		break;
	case BPF_ALU | BPF_MUL | BPF_K:
	case BPF_ALU64 | BPF_MUL | BPF_K:
		emit_a64_mov_i(is64, tmp, imm, ctx);
		emit(A64_MUL(is64, dst, dst, tmp), ctx);
		break;
	case BPF_ALU | BPF_DIV | BPF_K:
	case BPF_ALU64 | BPF_DIV | BPF_K:
		emit_a64_mov_i(is64, tmp, imm, ctx);
		if (!off)
			emit(A64_UDIV(is64, dst, dst, tmp), ctx);
		else
			emit(A64_SDIV(is64, dst, dst, tmp), ctx);
		break;
	case BPF_ALU | BPF_MOD | BPF_K:
	case BPF_ALU64 | BPF_MOD | BPF_K:
		emit_a64_mov_i(is64, tmp2, imm, ctx);
		if (!off)
			emit(A64_UDIV(is64, tmp, dst, tmp2), ctx);
		else
			emit(A64_SDIV(is64, tmp, dst, tmp2), ctx);
		emit(A64_MSUB(is64, dst, dst, tmp, tmp2), ctx);
		break;
	case BPF_ALU | BPF_LSH | BPF_K:
	case BPF_ALU64 | BPF_LSH | BPF_K:
		emit(A64_LSL(is64, dst, dst, imm), ctx);
		break;
	case BPF_ALU | BPF_RSH | BPF_K:
	case BPF_ALU64 | BPF_RSH | BPF_K:
		emit(A64_LSR(is64, dst, dst, imm), ctx);
		break;
	case BPF_ALU | BPF_ARSH | BPF_K:
	case BPF_ALU64 | BPF_ARSH | BPF_K:
		emit(A64_ASR(is64, dst, dst, imm), ctx);
		break;

	/* JUMP off */
	case BPF_JMP | BPF_JA:
	case BPF_JMP32 | BPF_JA:
		if (BPF_CLASS(code) == BPF_JMP)
			jmp_offset = bpf2a64_offset(i, off, ctx);
		else
			jmp_offset = bpf2a64_offset(i, imm, ctx);
		check_imm26(jmp_offset);
		emit(A64_B(jmp_offset), ctx);
		break;
	/* IF (dst COND src) JUMP off */
	case BPF_JMP | BPF_JEQ | BPF_X:
	case BPF_JMP | BPF_JGT | BPF_X:
	case BPF_JMP | BPF_JLT | BPF_X:
	case BPF_JMP | BPF_JGE | BPF_X:
	case BPF_JMP | BPF_JLE | BPF_X:
	case BPF_JMP | BPF_JNE | BPF_X:
	case BPF_JMP | BPF_JSGT | BPF_X:
	case BPF_JMP | BPF_JSLT | BPF_X:
	case BPF_JMP | BPF_JSGE | BPF_X:
	case BPF_JMP | BPF_JSLE | BPF_X:
	case BPF_JMP32 | BPF_JEQ | BPF_X:
	case BPF_JMP32 | BPF_JGT | BPF_X:
	case BPF_JMP32 | BPF_JLT | BPF_X:
	case BPF_JMP32 | BPF_JGE | BPF_X:
	case BPF_JMP32 | BPF_JLE | BPF_X:
	case BPF_JMP32 | BPF_JNE | BPF_X:
	case BPF_JMP32 | BPF_JSGT | BPF_X:
	case BPF_JMP32 | BPF_JSLT | BPF_X:
	case BPF_JMP32 | BPF_JSGE | BPF_X:
	case BPF_JMP32 | BPF_JSLE | BPF_X:
		emit(A64_CMP(is64, dst, src), ctx);
emit_cond_jmp:
		jmp_offset = bpf2a64_offset(i, off, ctx);
		check_imm19(jmp_offset);
		switch (BPF_OP(code)) {
		case BPF_JEQ:
			jmp_cond = A64_COND_EQ;
			break;
		case BPF_JGT:
			jmp_cond = A64_COND_HI;
			break;
		case BPF_JLT:
			jmp_cond = A64_COND_CC;
			break;
		case BPF_JGE:
			jmp_cond = A64_COND_CS;
			break;
		case BPF_JLE:
			jmp_cond = A64_COND_LS;
			break;
		case BPF_JSET:
		case BPF_JNE:
			jmp_cond = A64_COND_NE;
			break;
		case BPF_JSGT:
			jmp_cond = A64_COND_GT;
			break;
		case BPF_JSLT:
			jmp_cond = A64_COND_LT;
			break;
		case BPF_JSGE:
			jmp_cond = A64_COND_GE;
			break;
		case BPF_JSLE:
			jmp_cond = A64_COND_LE;
			break;
		default:
			return -EFAULT;
		}
		emit(A64_B_(jmp_cond, jmp_offset), ctx);
		break;
	case BPF_JMP | BPF_JSET | BPF_X:
	case BPF_JMP32 | BPF_JSET | BPF_X:
		emit(A64_TST(is64, dst, src), ctx);
		goto emit_cond_jmp;
	/* IF (dst COND imm) JUMP off */
	case BPF_JMP | BPF_JEQ | BPF_K:
	case BPF_JMP | BPF_JGT | BPF_K:
	case BPF_JMP | BPF_JLT | BPF_K:
	case BPF_JMP | BPF_JGE | BPF_K:
	case BPF_JMP | BPF_JLE | BPF_K:
	case BPF_JMP | BPF_JNE | BPF_K:
	case BPF_JMP | BPF_JSGT | BPF_K:
	case BPF_JMP | BPF_JSLT | BPF_K:
	case BPF_JMP | BPF_JSGE | BPF_K:
	case BPF_JMP | BPF_JSLE | BPF_K:
	case BPF_JMP32 | BPF_JEQ | BPF_K:
	case BPF_JMP32 | BPF_JGT | BPF_K:
	case BPF_JMP32 | BPF_JLT | BPF_K:
	case BPF_JMP32 | BPF_JGE | BPF_K:
	case BPF_JMP32 | BPF_JLE | BPF_K:
	case BPF_JMP32 | BPF_JNE | BPF_K:
	case BPF_JMP32 | BPF_JSGT | BPF_K:
	case BPF_JMP32 | BPF_JSLT | BPF_K:
	case BPF_JMP32 | BPF_JSGE | BPF_K:
	case BPF_JMP32 | BPF_JSLE | BPF_K:
		if (is_addsub_imm(imm)) {
			emit(A64_CMP_I(is64, dst, imm), ctx);
		} else if (is_addsub_imm(-imm)) {
			emit(A64_CMN_I(is64, dst, -imm), ctx);
		} else {
			emit_a64_mov_i(is64, tmp, imm, ctx);
			emit(A64_CMP(is64, dst, tmp), ctx);
		}
		goto emit_cond_jmp;
	case BPF_JMP | BPF_JSET | BPF_K:
	case BPF_JMP32 | BPF_JSET | BPF_K:
		a64_insn = A64_TST_I(is64, dst, imm);
		if (a64_insn != AARCH64_BREAK_FAULT) {
			emit(a64_insn, ctx);
		} else {
			emit_a64_mov_i(is64, tmp, imm, ctx);
			emit(A64_TST(is64, dst, tmp), ctx);
		}
		goto emit_cond_jmp;
	/* function call */
	case BPF_JMP | BPF_CALL:
	{
		const u8 r0 = bpf2a64[BPF_REG_0];
		bool func_addr_fixed;
		u64 func_addr;

		ret = bpf_jit_get_func_addr(ctx->prog, insn, extra_pass,
					    &func_addr, &func_addr_fixed);
		if (ret < 0)
			return ret;
		emit_call(func_addr, ctx);
		emit(A64_MOV(1, r0, A64_R(0)), ctx);
		break;
	}
	/* tail call */
	case BPF_JMP | BPF_TAIL_CALL:
		if (emit_bpf_tail_call(ctx))
			return -EFAULT;
		break;
	/* function return */
	case BPF_JMP | BPF_EXIT:
		/* Optimization: when last instruction is EXIT,
		   simply fallthrough to epilogue. */
		if (i == ctx->prog->len - 1)
			break;
		jmp_offset = epilogue_offset(ctx);
		check_imm26(jmp_offset);
		emit(A64_B(jmp_offset), ctx);
		break;

	/* dst = imm64 */
	case BPF_LD | BPF_IMM | BPF_DW:
	{
		const struct bpf_insn insn1 = insn[1];
		u64 imm64;

		imm64 = (u64)insn1.imm << 32 | (u32)imm;
		if (bpf_pseudo_func(insn))
			emit_addr_mov_i64(dst, imm64, ctx);
		else
			emit_a64_mov_i64(dst, imm64, ctx);

		return 1;
	}

	/* LDX: dst = (u64)*(unsigned size *)(src + off) */
	case BPF_LDX | BPF_MEM | BPF_W:
	case BPF_LDX | BPF_MEM | BPF_H:
	case BPF_LDX | BPF_MEM | BPF_B:
	case BPF_LDX | BPF_MEM | BPF_DW:
	case BPF_LDX | BPF_PROBE_MEM | BPF_DW:
	case BPF_LDX | BPF_PROBE_MEM | BPF_W:
	case BPF_LDX | BPF_PROBE_MEM | BPF_H:
	case BPF_LDX | BPF_PROBE_MEM | BPF_B:
	/* LDXS: dst_reg = (s64)*(signed size *)(src_reg + off) */
	case BPF_LDX | BPF_MEMSX | BPF_B:
	case BPF_LDX | BPF_MEMSX | BPF_H:
	case BPF_LDX | BPF_MEMSX | BPF_W:
	case BPF_LDX | BPF_PROBE_MEMSX | BPF_B:
	case BPF_LDX | BPF_PROBE_MEMSX | BPF_H:
	case BPF_LDX | BPF_PROBE_MEMSX | BPF_W:
		if (ctx->fpb_offset > 0 && src == fp) {
			src_adj = fpb;
			off_adj = off + ctx->fpb_offset;
		} else {
			src_adj = src;
			off_adj = off;
		}
		sign_extend = (BPF_MODE(insn->code) == BPF_MEMSX ||
				BPF_MODE(insn->code) == BPF_PROBE_MEMSX);
		switch (BPF_SIZE(code)) {
		case BPF_W:
			if (is_lsi_offset(off_adj, 2)) {
				if (sign_extend)
					emit(A64_LDRSWI(dst, src_adj, off_adj), ctx);
				else
					emit(A64_LDR32I(dst, src_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp, off, ctx);
				if (sign_extend)
					emit(A64_LDRSW(dst, src_adj, off_adj), ctx);
				else
					emit(A64_LDR32(dst, src, tmp), ctx);
			}
			break;
		case BPF_H:
			if (is_lsi_offset(off_adj, 1)) {
				if (sign_extend)
					emit(A64_LDRSHI(dst, src_adj, off_adj), ctx);
				else
					emit(A64_LDRHI(dst, src_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp, off, ctx);
				if (sign_extend)
					emit(A64_LDRSH(dst, src, tmp), ctx);
				else
					emit(A64_LDRH(dst, src, tmp), ctx);
			}
			break;
		case BPF_B:
			if (is_lsi_offset(off_adj, 0)) {
				if (sign_extend)
					emit(A64_LDRSBI(dst, src_adj, off_adj), ctx);
				else
					emit(A64_LDRBI(dst, src_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp, off, ctx);
				if (sign_extend)
					emit(A64_LDRSB(dst, src, tmp), ctx);
				else
					emit(A64_LDRB(dst, src, tmp), ctx);
			}
			break;
		case BPF_DW:
			if (is_lsi_offset(off_adj, 3)) {
				emit(A64_LDR64I(dst, src_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp, off, ctx);
				emit(A64_LDR64(dst, src, tmp), ctx);
			}
			break;
		}

		ret = add_exception_handler(insn, ctx, dst);
		if (ret)
			return ret;
		break;

	/* speculation barrier */
	case BPF_ST | BPF_NOSPEC:
		/*
		 * Nothing required here.
		 *
		 * In case of arm64, we rely on the firmware mitigation of
		 * Speculative Store Bypass as controlled via the ssbd kernel
		 * parameter. Whenever the mitigation is enabled, it works
		 * for all of the kernel code with no need to provide any
		 * additional instructions.
		 */
		break;

	/* ST: *(size *)(dst + off) = imm */
	case BPF_ST | BPF_MEM | BPF_W:
	case BPF_ST | BPF_MEM | BPF_H:
	case BPF_ST | BPF_MEM | BPF_B:
	case BPF_ST | BPF_MEM | BPF_DW:
		if (ctx->fpb_offset > 0 && dst == fp) {
			dst_adj = fpb;
			off_adj = off + ctx->fpb_offset;
		} else {
			dst_adj = dst;
			off_adj = off;
		}
		/* Load imm to a register then store it */
		emit_a64_mov_i(1, tmp, imm, ctx);
		switch (BPF_SIZE(code)) {
		case BPF_W:
			if (is_lsi_offset(off_adj, 2)) {
				emit(A64_STR32I(tmp, dst_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp2, off, ctx);
				emit(A64_STR32(tmp, dst, tmp2), ctx);
			}
			break;
		case BPF_H:
			if (is_lsi_offset(off_adj, 1)) {
				emit(A64_STRHI(tmp, dst_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp2, off, ctx);
				emit(A64_STRH(tmp, dst, tmp2), ctx);
			}
			break;
		case BPF_B:
			if (is_lsi_offset(off_adj, 0)) {
				emit(A64_STRBI(tmp, dst_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp2, off, ctx);
				emit(A64_STRB(tmp, dst, tmp2), ctx);
			}
			break;
		case BPF_DW:
			if (is_lsi_offset(off_adj, 3)) {
				emit(A64_STR64I(tmp, dst_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp2, off, ctx);
				emit(A64_STR64(tmp, dst, tmp2), ctx);
			}
			break;
		}
		break;

	/* STX: *(size *)(dst + off) = src */
	case BPF_STX | BPF_MEM | BPF_W:
	case BPF_STX | BPF_MEM | BPF_H:
	case BPF_STX | BPF_MEM | BPF_B:
	case BPF_STX | BPF_MEM | BPF_DW:
		if (ctx->fpb_offset > 0 && dst == fp) {
			dst_adj = fpb;
			off_adj = off + ctx->fpb_offset;
		} else {
			dst_adj = dst;
			off_adj = off;
		}
		switch (BPF_SIZE(code)) {
		case BPF_W:
			if (is_lsi_offset(off_adj, 2)) {
				emit(A64_STR32I(src, dst_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp, off, ctx);
				emit(A64_STR32(src, dst, tmp), ctx);
			}
			break;
		case BPF_H:
			if (is_lsi_offset(off_adj, 1)) {
				emit(A64_STRHI(src, dst_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp, off, ctx);
				emit(A64_STRH(src, dst, tmp), ctx);
			}
			break;
		case BPF_B:
			if (is_lsi_offset(off_adj, 0)) {
				emit(A64_STRBI(src, dst_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp, off, ctx);
				emit(A64_STRB(src, dst, tmp), ctx);
			}
			break;
		case BPF_DW:
			if (is_lsi_offset(off_adj, 3)) {
				emit(A64_STR64I(src, dst_adj, off_adj), ctx);
			} else {
				emit_a64_mov_i(1, tmp, off, ctx);
				emit(A64_STR64(src, dst, tmp), ctx);
			}
			break;
		}
		break;

	case BPF_STX | BPF_ATOMIC | BPF_W:
	case BPF_STX | BPF_ATOMIC | BPF_DW:
		if (cpus_have_cap(ARM64_HAS_LSE_ATOMICS))
			ret = emit_lse_atomic(insn, ctx);
		else
			ret = emit_ll_sc_atomic(insn, ctx);
		if (ret)
			return ret;
		break;

	default:
		pr_err_once("unknown opcode %02x\n", code);
		return -EINVAL;
	}

	return 0;
}

/*
 * Return 0 if FP may change at runtime, otherwise find the minimum negative
 * offset to FP, converts it to positive number, and align down to 8 bytes.
 */
static int find_fpb_offset(struct bpf_prog *prog)
{
	int i;
	int offset = 0;

	for (i = 0; i < prog->len; i++) {
		const struct bpf_insn *insn = &prog->insnsi[i];
		const u8 class = BPF_CLASS(insn->code);
		const u8 mode = BPF_MODE(insn->code);
		const u8 src = insn->src_reg;
		const u8 dst = insn->dst_reg;
		const s32 imm = insn->imm;
		const s16 off = insn->off;

		switch (class) {
		case BPF_STX:
		case BPF_ST:
			/* fp holds atomic operation result */
			if (class == BPF_STX && mode == BPF_ATOMIC &&
			    ((imm == BPF_XCHG ||
			      imm == (BPF_FETCH | BPF_ADD) ||
			      imm == (BPF_FETCH | BPF_AND) ||
			      imm == (BPF_FETCH | BPF_XOR) ||
			      imm == (BPF_FETCH | BPF_OR)) &&
			     src == BPF_REG_FP))
				return 0;

			if (mode == BPF_MEM && dst == BPF_REG_FP &&
			    off < offset)
				offset = insn->off;
			break;

		case BPF_JMP32:
		case BPF_JMP:
			break;

		case BPF_LDX:
		case BPF_LD:
			/* fp holds load result */
			if (dst == BPF_REG_FP)
				return 0;

			if (class == BPF_LDX && mode == BPF_MEM &&
			    src == BPF_REG_FP && off < offset)
				offset = off;
			break;

		case BPF_ALU:
		case BPF_ALU64:
		default:
			/* fp holds ALU result */
			if (dst == BPF_REG_FP)
				return 0;
		}
	}

	if (offset < 0) {
		/*
		 * safely be converted to a positive 'int', since insn->off
		 * is 's16'
		 */
		offset = -offset;
		/* align down to 8 bytes */
		offset = ALIGN_DOWN(offset, 8);
	}

	return offset;
}

static int build_body(struct jit_ctx *ctx, bool extra_pass)
{
	const struct bpf_prog *prog = ctx->prog;
	int i;

	/*
	 * - offset[0] offset of the end of prologue,
	 *   start of the 1st instruction.
	 * - offset[1] - offset of the end of 1st instruction,
	 *   start of the 2nd instruction
	 * [....]
	 * - offset[3] - offset of the end of 3rd instruction,
	 *   start of 4th instruction
	 */
	for (i = 0; i < prog->len; i++) {
		const struct bpf_insn *insn = &prog->insnsi[i];
		int ret;

		if (ctx->image == NULL)
			ctx->offset[i] = ctx->idx;
		ret = build_insn(insn, ctx, extra_pass);
		if (ret > 0) {
			i++;
			if (ctx->image == NULL)
				ctx->offset[i] = ctx->idx;
			continue;
		}
		if (ret)
			return ret;
	}
	/*
	 * offset is allocated with prog->len + 1 so fill in
	 * the last element with the offset after the last
	 * instruction (end of program)
	 */
	if (ctx->image == NULL)
		ctx->offset[i] = ctx->idx;

	return 0;
}

static int validate_code(struct jit_ctx *ctx)
{
	int i;

	for (i = 0; i < ctx->idx; i++) {
		u32 a64_insn = le32_to_cpu(ctx->image[i]);

		if (a64_insn == AARCH64_BREAK_FAULT)
			return -1;
	}
	return 0;
}

static int validate_ctx(struct jit_ctx *ctx)
{
	if (validate_code(ctx))
		return -1;

	if (WARN_ON_ONCE(ctx->exentry_idx != ctx->prog->aux->num_exentries))
		return -1;

	return 0;
}

static inline void bpf_flush_icache(void *start, void *end)
{
	flush_icache_range((unsigned long)start, (unsigned long)end);
}

struct arm64_jit_data {
	struct bpf_binary_header *header;
	u8 *image;
	struct jit_ctx ctx;
};

struct bpf_prog *bpf_int_jit_compile(struct bpf_prog *prog)
{
	int image_size, prog_size, extable_size, extable_align, extable_offset;
	struct bpf_prog *tmp, *orig_prog = prog;
	struct bpf_binary_header *header;
	struct arm64_jit_data *jit_data;
	bool was_classic = bpf_prog_was_classic(prog);
	bool tmp_blinded = false;
	bool extra_pass = false;
	struct jit_ctx ctx;
	u8 *image_ptr;

	if (!prog->jit_requested)
		return orig_prog;

	tmp = bpf_jit_blind_constants(prog);
	/* If blinding was requested and we failed during blinding,
	 * we must fall back to the interpreter.
	 */
	if (IS_ERR(tmp))
		return orig_prog;
	if (tmp != prog) {
		tmp_blinded = true;
		prog = tmp;
	}

	jit_data = prog->aux->jit_data;
	if (!jit_data) {
		jit_data = kzalloc(sizeof(*jit_data), GFP_KERNEL);
		if (!jit_data) {
			prog = orig_prog;
			goto out;
		}
		prog->aux->jit_data = jit_data;
	}
	if (jit_data->ctx.offset) {
		ctx = jit_data->ctx;
		image_ptr = jit_data->image;
		header = jit_data->header;
		extra_pass = true;
		prog_size = sizeof(u32) * ctx.idx;
		goto skip_init_ctx;
	}
	memset(&ctx, 0, sizeof(ctx));
	ctx.prog = prog;

	ctx.offset = kvcalloc(prog->len + 1, sizeof(int), GFP_KERNEL);
	if (ctx.offset == NULL) {
		prog = orig_prog;
		goto out_off;
	}

	ctx.fpb_offset = find_fpb_offset(prog);

	/*
	 * 1. Initial fake pass to compute ctx->idx and ctx->offset.
	 *
	 * BPF line info needs ctx->offset[i] to be the offset of
	 * instruction[i] in jited image, so build prologue first.
	 */
	if (build_prologue(&ctx, was_classic)) {
		prog = orig_prog;
		goto out_off;
	}

	if (build_body(&ctx, extra_pass)) {
		prog = orig_prog;
		goto out_off;
	}

	ctx.epilogue_offset = ctx.idx;
	build_epilogue(&ctx);
	build_plt(&ctx);

	extable_align = __alignof__(struct exception_table_entry);
	extable_size = prog->aux->num_exentries *
		sizeof(struct exception_table_entry);

	/* Now we know the actual image size. */
	prog_size = sizeof(u32) * ctx.idx;
	/* also allocate space for plt target */
	extable_offset = round_up(prog_size + PLT_TARGET_SIZE, extable_align);
	image_size = extable_offset + extable_size;
	header = bpf_jit_binary_alloc(image_size, &image_ptr,
				      sizeof(u32), jit_fill_hole);
	if (header == NULL) {
		prog = orig_prog;
		goto out_off;
	}

	/* 2. Now, the actual pass. */

	ctx.image = (__le32 *)image_ptr;
	if (extable_size)
		prog->aux->extable = (void *)image_ptr + extable_offset;
skip_init_ctx:
	ctx.idx = 0;
	ctx.exentry_idx = 0;

	build_prologue(&ctx, was_classic);

	if (build_body(&ctx, extra_pass)) {
		bpf_jit_binary_free(header);
		prog = orig_prog;
		goto out_off;
	}

	build_epilogue(&ctx);
	build_plt(&ctx);

	/* 3. Extra pass to validate JITed code. */
	if (validate_ctx(&ctx)) {
		bpf_jit_binary_free(header);
		prog = orig_prog;
		goto out_off;
	}

	/* And we're done. */
	if (bpf_jit_enable > 1)
		bpf_jit_dump(prog->len, prog_size, 2, ctx.image);

	bpf_flush_icache(header, ctx.image + ctx.idx);

	if (!prog->is_func || extra_pass) {
		if (extra_pass && ctx.idx != jit_data->ctx.idx) {
			pr_err_once("multi-func JIT bug %d != %d\n",
				    ctx.idx, jit_data->ctx.idx);
			bpf_jit_binary_free(header);
			prog->bpf_func = NULL;
			prog->jited = 0;
			prog->jited_len = 0;
			goto out_off;
		}
		bpf_jit_binary_lock_ro(header);
	} else {
		jit_data->ctx = ctx;
		jit_data->image = image_ptr;
		jit_data->header = header;
	}
	prog->bpf_func = (void *)ctx.image;
	prog->jited = 1;
	prog->jited_len = prog_size;

	if (!prog->is_func || extra_pass) {
		int i;

		/* offset[prog->len] is the size of program */
		for (i = 0; i <= prog->len; i++)
			ctx.offset[i] *= AARCH64_INSN_SIZE;
		bpf_prog_fill_jited_linfo(prog, ctx.offset + 1);
out_off:
		kvfree(ctx.offset);
		kfree(jit_data);
		prog->aux->jit_data = NULL;
	}
out:
	if (tmp_blinded)
		bpf_jit_prog_release_other(prog, prog == orig_prog ?
					   tmp : orig_prog);
	return prog;
}

bool bpf_jit_supports_kfunc_call(void)
{
	return true;
}

u64 bpf_jit_alloc_exec_limit(void)
{
	return VMALLOC_END - VMALLOC_START;
}

void *bpf_jit_alloc_exec(unsigned long size)
{
	/* Memory is intended to be executable, reset the pointer tag. */
	return kasan_reset_tag(vmalloc(size));
}

void bpf_jit_free_exec(void *addr)
{
	return vfree(addr);
}

/* Indicate the JIT backend supports mixing bpf2bpf and tailcalls. */
bool bpf_jit_supports_subprog_tailcalls(void)
{
	return true;
}

static void invoke_bpf_prog(struct jit_ctx *ctx, struct bpf_tramp_link *l,
			    int args_off, int retval_off, int run_ctx_off,
			    bool save_ret)
{
	__le32 *branch;
	u64 enter_prog;
	u64 exit_prog;
	struct bpf_prog *p = l->link.prog;
	int cookie_off = offsetof(struct bpf_tramp_run_ctx, bpf_cookie);

	enter_prog = (u64)bpf_trampoline_enter(p);
	exit_prog = (u64)bpf_trampoline_exit(p);

	if (l->cookie == 0) {
		/* if cookie is zero, one instruction is enough to store it */
		emit(A64_STR64I(A64_ZR, A64_SP, run_ctx_off + cookie_off), ctx);
	} else {
		emit_a64_mov_i64(A64_R(10), l->cookie, ctx);
		emit(A64_STR64I(A64_R(10), A64_SP, run_ctx_off + cookie_off),
		     ctx);
	}

	/* save p to callee saved register x19 to avoid loading p with mov_i64
	 * each time.
	 */
	emit_addr_mov_i64(A64_R(19), (const u64)p, ctx);

	/* arg1: prog */
	emit(A64_MOV(1, A64_R(0), A64_R(19)), ctx);
	/* arg2: &run_ctx */
	emit(A64_ADD_I(1, A64_R(1), A64_SP, run_ctx_off), ctx);

	emit_call(enter_prog, ctx);

	/* if (__bpf_prog_enter(prog) == 0)
	 *         goto skip_exec_of_prog;
	 */
	branch = ctx->image + ctx->idx;
	emit(A64_NOP, ctx);

	/* save return value to callee saved register x20 */
	emit(A64_MOV(1, A64_R(20), A64_R(0)), ctx);

	emit(A64_ADD_I(1, A64_R(0), A64_SP, args_off), ctx);
	if (!p->jited)
		emit_addr_mov_i64(A64_R(1), (const u64)p->insnsi, ctx);

	emit_call((const u64)p->bpf_func, ctx);

	if (save_ret)
		emit(A64_STR64I(A64_R(0), A64_SP, retval_off), ctx);

	if (ctx->image) {
		int offset = &ctx->image[ctx->idx] - branch;
		*branch = cpu_to_le32(A64_CBZ(1, A64_R(0), offset));
	}

	/* arg1: prog */
	emit(A64_MOV(1, A64_R(0), A64_R(19)), ctx);
	/* arg2: start time */
	emit(A64_MOV(1, A64_R(1), A64_R(20)), ctx);
	/* arg3: &run_ctx */
	emit(A64_ADD_I(1, A64_R(2), A64_SP, run_ctx_off), ctx);

	emit_call(exit_prog, ctx);
}

static void invoke_bpf_mod_ret(struct jit_ctx *ctx, struct bpf_tramp_links *tl,
			       int args_off, int retval_off, int run_ctx_off,
			       __le32 **branches)
{
	int i;

	/* The first fmod_ret program will receive a garbage return value.
	 * Set this to 0 to avoid confusing the program.
	 */
	emit(A64_STR64I(A64_ZR, A64_SP, retval_off), ctx);
	for (i = 0; i < tl->nr_links; i++) {
		invoke_bpf_prog(ctx, tl->links[i], args_off, retval_off,
				run_ctx_off, true);
		/* if (*(u64 *)(sp + retval_off) !=  0)
		 *	goto do_fexit;
		 */
		emit(A64_LDR64I(A64_R(10), A64_SP, retval_off), ctx);
		/* Save the location of branch, and generate a nop.
		 * This nop will be replaced with a cbnz later.
		 */
		branches[i] = ctx->image + ctx->idx;
		emit(A64_NOP, ctx);
	}
}

static void save_args(struct jit_ctx *ctx, int args_off, int nregs)
{
	int i;

	for (i = 0; i < nregs; i++) {
		emit(A64_STR64I(i, A64_SP, args_off), ctx);
		args_off += 8;
	}
}

static void restore_args(struct jit_ctx *ctx, int args_off, int nregs)
{
	int i;

	for (i = 0; i < nregs; i++) {
		emit(A64_LDR64I(i, A64_SP, args_off), ctx);
		args_off += 8;
	}
}

/* Based on the x86's implementation of arch_prepare_bpf_trampoline().
 *
 * bpf prog and function entry before bpf trampoline hooked:
 *   mov x9, lr
 *   nop
 *
 * bpf prog and function entry after bpf trampoline hooked:
 *   mov x9, lr
 *   bl  <bpf_trampoline or plt>
 *
 */
static int prepare_trampoline(struct jit_ctx *ctx, struct bpf_tramp_image *im,
			      struct bpf_tramp_links *tlinks, void *orig_call,
			      int nregs, u32 flags)
{
	int i;
	int stack_size;
	int retaddr_off;
	int regs_off;
	int retval_off;
	int args_off;
	int nregs_off;
	int ip_off;
	int run_ctx_off;
	struct bpf_tramp_links *fentry = &tlinks[BPF_TRAMP_FENTRY];
	struct bpf_tramp_links *fexit = &tlinks[BPF_TRAMP_FEXIT];
	struct bpf_tramp_links *fmod_ret = &tlinks[BPF_TRAMP_MODIFY_RETURN];
	bool save_ret;
	__le32 **branches = NULL;

	/* trampoline stack layout:
	 *                  [ parent ip         ]
	 *                  [ FP                ]
	 * SP + retaddr_off [ self ip           ]
	 *                  [ FP                ]
	 *
	 *                  [ padding           ] align SP to multiples of 16
	 *
	 *                  [ x20               ] callee saved reg x20
	 * SP + regs_off    [ x19               ] callee saved reg x19
	 *
	 * SP + retval_off  [ return value      ] BPF_TRAMP_F_CALL_ORIG or
	 *                                        BPF_TRAMP_F_RET_FENTRY_RET
	 *
	 *                  [ arg reg N         ]
	 *                  [ ...               ]
	 * SP + args_off    [ arg reg 1         ]
	 *
	 * SP + nregs_off   [ arg regs count    ]
	 *
	 * SP + ip_off      [ traced function   ] BPF_TRAMP_F_IP_ARG flag
	 *
	 * SP + run_ctx_off [ bpf_tramp_run_ctx ]
	 */

	stack_size = 0;
	run_ctx_off = stack_size;
	/* room for bpf_tramp_run_ctx */
	stack_size += round_up(sizeof(struct bpf_tramp_run_ctx), 8);

	ip_off = stack_size;
	/* room for IP address argument */
	if (flags & BPF_TRAMP_F_IP_ARG)
		stack_size += 8;

	nregs_off = stack_size;
	/* room for args count */
	stack_size += 8;

	args_off = stack_size;
	/* room for args */
	stack_size += nregs * 8;

	/* room for return value */
	retval_off = stack_size;
	save_ret = flags & (BPF_TRAMP_F_CALL_ORIG | BPF_TRAMP_F_RET_FENTRY_RET);
	if (save_ret)
		stack_size += 8;

	/* room for callee saved registers, currently x19 and x20 are used */
	regs_off = stack_size;
	stack_size += 16;

	/* round up to multiples of 16 to avoid SPAlignmentFault */
	stack_size = round_up(stack_size, 16);

	/* return address locates above FP */
	retaddr_off = stack_size + 8;

	/* bpf trampoline may be invoked by 3 instruction types:
	 * 1. bl, attached to bpf prog or kernel function via short jump
	 * 2. br, attached to bpf prog or kernel function via long jump
	 * 3. blr, working as a function pointer, used by struct_ops.
	 * So BTI_JC should used here to support both br and blr.
	 */
	emit_bti(A64_BTI_JC, ctx);

	/* frame for parent function */
	emit(A64_PUSH(A64_FP, A64_R(9), A64_SP), ctx);
	emit(A64_MOV(1, A64_FP, A64_SP), ctx);

	/* frame for patched function */
	emit(A64_PUSH(A64_FP, A64_LR, A64_SP), ctx);
	emit(A64_MOV(1, A64_FP, A64_SP), ctx);

	/* allocate stack space */
	emit(A64_SUB_I(1, A64_SP, A64_SP, stack_size), ctx);

	if (flags & BPF_TRAMP_F_IP_ARG) {
		/* save ip address of the traced function */
		emit_addr_mov_i64(A64_R(10), (const u64)orig_call, ctx);
		emit(A64_STR64I(A64_R(10), A64_SP, ip_off), ctx);
	}

	/* save arg regs count*/
	emit(A64_MOVZ(1, A64_R(10), nregs, 0), ctx);
	emit(A64_STR64I(A64_R(10), A64_SP, nregs_off), ctx);

	/* save arg regs */
	save_args(ctx, args_off, nregs);

	/* save callee saved registers */
	emit(A64_STR64I(A64_R(19), A64_SP, regs_off), ctx);
	emit(A64_STR64I(A64_R(20), A64_SP, regs_off + 8), ctx);

	if (flags & BPF_TRAMP_F_CALL_ORIG) {
		emit_addr_mov_i64(A64_R(0), (const u64)im, ctx);
		emit_call((const u64)__bpf_tramp_enter, ctx);
	}

	for (i = 0; i < fentry->nr_links; i++)
		invoke_bpf_prog(ctx, fentry->links[i], args_off,
				retval_off, run_ctx_off,
				flags & BPF_TRAMP_F_RET_FENTRY_RET);

	if (fmod_ret->nr_links) {
		branches = kcalloc(fmod_ret->nr_links, sizeof(__le32 *),
				   GFP_KERNEL);
		if (!branches)
			return -ENOMEM;

		invoke_bpf_mod_ret(ctx, fmod_ret, args_off, retval_off,
				   run_ctx_off, branches);
	}

	if (flags & BPF_TRAMP_F_CALL_ORIG) {
		restore_args(ctx, args_off, nregs);
		/* call original func */
		emit(A64_LDR64I(A64_R(10), A64_SP, retaddr_off), ctx);
		emit(A64_ADR(A64_LR, AARCH64_INSN_SIZE * 2), ctx);
		emit(A64_RET(A64_R(10)), ctx);
		/* store return value */
		emit(A64_STR64I(A64_R(0), A64_SP, retval_off), ctx);
		/* reserve a nop for bpf_tramp_image_put */
		im->ip_after_call = ctx->image + ctx->idx;
		emit(A64_NOP, ctx);
	}

	/* update the branches saved in invoke_bpf_mod_ret with cbnz */
	for (i = 0; i < fmod_ret->nr_links && ctx->image != NULL; i++) {
		int offset = &ctx->image[ctx->idx] - branches[i];
		*branches[i] = cpu_to_le32(A64_CBNZ(1, A64_R(10), offset));
	}

	for (i = 0; i < fexit->nr_links; i++)
		invoke_bpf_prog(ctx, fexit->links[i], args_off, retval_off,
				run_ctx_off, false);

	if (flags & BPF_TRAMP_F_CALL_ORIG) {
		im->ip_epilogue = ctx->image + ctx->idx;
		emit_addr_mov_i64(A64_R(0), (const u64)im, ctx);
		emit_call((const u64)__bpf_tramp_exit, ctx);
	}

	if (flags & BPF_TRAMP_F_RESTORE_REGS)
		restore_args(ctx, args_off, nregs);

	/* restore callee saved register x19 and x20 */
	emit(A64_LDR64I(A64_R(19), A64_SP, regs_off), ctx);
	emit(A64_LDR64I(A64_R(20), A64_SP, regs_off + 8), ctx);

	if (save_ret)
		emit(A64_LDR64I(A64_R(0), A64_SP, retval_off), ctx);

	/* reset SP  */
	emit(A64_MOV(1, A64_SP, A64_FP), ctx);

	/* pop frames  */
	emit(A64_POP(A64_FP, A64_LR, A64_SP), ctx);
	emit(A64_POP(A64_FP, A64_R(9), A64_SP), ctx);

	if (flags & BPF_TRAMP_F_SKIP_FRAME) {
		/* skip patched function, return to parent */
		emit(A64_MOV(1, A64_LR, A64_R(9)), ctx);
		emit(A64_RET(A64_R(9)), ctx);
	} else {
		/* return to patched function */
		emit(A64_MOV(1, A64_R(10), A64_LR), ctx);
		emit(A64_MOV(1, A64_LR, A64_R(9)), ctx);
		emit(A64_RET(A64_R(10)), ctx);
	}

	if (ctx->image)
		bpf_flush_icache(ctx->image, ctx->image + ctx->idx);

	kfree(branches);

	return ctx->idx;
}

int arch_prepare_bpf_trampoline(struct bpf_tramp_image *im, void *image,
				void *image_end, const struct btf_func_model *m,
				u32 flags, struct bpf_tramp_links *tlinks,
				void *orig_call)
{
	int i, ret;
	int nregs = m->nr_args;
	int max_insns = ((long)image_end - (long)image) / AARCH64_INSN_SIZE;
	struct jit_ctx ctx = {
		.image = NULL,
		.idx = 0,
	};

	/* extra registers needed for struct argument */
	for (i = 0; i < MAX_BPF_FUNC_ARGS; i++) {
		/* The arg_size is at most 16 bytes, enforced by the verifier. */
		if (m->arg_flags[i] & BTF_FMODEL_STRUCT_ARG)
			nregs += (m->arg_size[i] + 7) / 8 - 1;
	}

	/* the first 8 registers are used for arguments */
	if (nregs > 8)
		return -ENOTSUPP;

	ret = prepare_trampoline(&ctx, im, tlinks, orig_call, nregs, flags);
	if (ret < 0)
		return ret;

	if (ret > max_insns)
		return -EFBIG;

	ctx.image = image;
	ctx.idx = 0;

	jit_fill_hole(image, (unsigned int)(image_end - image));
	ret = prepare_trampoline(&ctx, im, tlinks, orig_call, nregs, flags);

	if (ret > 0 && validate_code(&ctx) < 0)
		ret = -EINVAL;

	if (ret > 0)
		ret *= AARCH64_INSN_SIZE;

	return ret;
}

static bool is_long_jump(void *ip, void *target)
{
	long offset;

	/* NULL target means this is a NOP */
	if (!target)
		return false;

	offset = (long)target - (long)ip;
	return offset < -SZ_128M || offset >= SZ_128M;
}

static int gen_branch_or_nop(enum aarch64_insn_branch_type type, void *ip,
			     void *addr, void *plt, u32 *insn)
{
	void *target;

	if (!addr) {
		*insn = aarch64_insn_gen_nop();
		return 0;
	}

	if (is_long_jump(ip, addr))
		target = plt;
	else
		target = addr;

	*insn = aarch64_insn_gen_branch_imm((unsigned long)ip,
					    (unsigned long)target,
					    type);

	return *insn != AARCH64_BREAK_FAULT ? 0 : -EFAULT;
}

/* Replace the branch instruction from @ip to @old_addr in a bpf prog or a bpf
 * trampoline with the branch instruction from @ip to @new_addr. If @old_addr
 * or @new_addr is NULL, the old or new instruction is NOP.
 *
 * When @ip is the bpf prog entry, a bpf trampoline is being attached or
 * detached. Since bpf trampoline and bpf prog are allocated separately with
 * vmalloc, the address distance may exceed 128MB, the maximum branch range.
 * So long jump should be handled.
 *
 * When a bpf prog is constructed, a plt pointing to empty trampoline
 * dummy_tramp is placed at the end:
 *
 *      bpf_prog:
 *              mov x9, lr
 *              nop // patchsite
 *              ...
 *              ret
 *
 *      plt:
 *              ldr x10, target
 *              br x10
 *      target:
 *              .quad dummy_tramp // plt target
 *
 * This is also the state when no trampoline is attached.
 *
 * When a short-jump bpf trampoline is attached, the patchsite is patched
 * to a bl instruction to the trampoline directly:
 *
 *      bpf_prog:
 *              mov x9, lr
 *              bl <short-jump bpf trampoline address> // patchsite
 *              ...
 *              ret
 *
 *      plt:
 *              ldr x10, target
 *              br x10
 *      target:
 *              .quad dummy_tramp // plt target
 *
 * When a long-jump bpf trampoline is attached, the plt target is filled with
 * the trampoline address and the patchsite is patched to a bl instruction to
 * the plt:
 *
 *      bpf_prog:
 *              mov x9, lr
 *              bl plt // patchsite
 *              ...
 *              ret
 *
 *      plt:
 *              ldr x10, target
 *              br x10
 *      target:
 *              .quad <long-jump bpf trampoline address> // plt target
 *
 * The dummy_tramp is used to prevent another CPU from jumping to unknown
 * locations during the patching process, making the patching process easier.
 */
int bpf_arch_text_poke(void *ip, enum bpf_text_poke_type poke_type,
		       void *old_addr, void *new_addr)
{
	int ret;
	u32 old_insn;
	u32 new_insn;
	u32 replaced;
	struct bpf_plt *plt = NULL;
	unsigned long size = 0UL;
	unsigned long offset = ~0UL;
	enum aarch64_insn_branch_type branch_type;
	char namebuf[KSYM_NAME_LEN];
	void *image = NULL;
	u64 plt_target = 0ULL;
	bool poking_bpf_entry;

	if (!__bpf_address_lookup((unsigned long)ip, &size, &offset, namebuf))
		/* Only poking bpf text is supported. Since kernel function
		 * entry is set up by ftrace, we reply on ftrace to poke kernel
		 * functions.
		 */
		return -ENOTSUPP;

	image = ip - offset;
	/* zero offset means we're poking bpf prog entry */
	poking_bpf_entry = (offset == 0UL);

	/* bpf prog entry, find plt and the real patchsite */
	if (poking_bpf_entry) {
		/* plt locates at the end of bpf prog */
		plt = image + size - PLT_TARGET_OFFSET;

		/* skip to the nop instruction in bpf prog entry:
		 * bti c // if BTI enabled
		 * mov x9, x30
		 * nop
		 */
		ip = image + POKE_OFFSET * AARCH64_INSN_SIZE;
	}

	/* long jump is only possible at bpf prog entry */
	if (WARN_ON((is_long_jump(ip, new_addr) || is_long_jump(ip, old_addr)) &&
		    !poking_bpf_entry))
		return -EINVAL;

	if (poke_type == BPF_MOD_CALL)
		branch_type = AARCH64_INSN_BRANCH_LINK;
	else
		branch_type = AARCH64_INSN_BRANCH_NOLINK;

	if (gen_branch_or_nop(branch_type, ip, old_addr, plt, &old_insn) < 0)
		return -EFAULT;

	if (gen_branch_or_nop(branch_type, ip, new_addr, plt, &new_insn) < 0)
		return -EFAULT;

	if (is_long_jump(ip, new_addr))
		plt_target = (u64)new_addr;
	else if (is_long_jump(ip, old_addr))
		/* if the old target is a long jump and the new target is not,
		 * restore the plt target to dummy_tramp, so there is always a
		 * legal and harmless address stored in plt target, and we'll
		 * never jump from plt to an unknown place.
		 */
		plt_target = (u64)&dummy_tramp;

	if (plt_target) {
		/* non-zero plt_target indicates we're patching a bpf prog,
		 * which is read only.
		 */
		if (set_memory_rw(PAGE_MASK & ((uintptr_t)&plt->target), 1))
			return -EFAULT;
		WRITE_ONCE(plt->target, plt_target);
		set_memory_ro(PAGE_MASK & ((uintptr_t)&plt->target), 1);
		/* since plt target points to either the new trampoline
		 * or dummy_tramp, even if another CPU reads the old plt
		 * target value before fetching the bl instruction to plt,
		 * it will be brought back by dummy_tramp, so no barrier is
		 * required here.
		 */
	}

	/* if the old target and the new target are both long jumps, no
	 * patching is required
	 */
	if (old_insn == new_insn)
		return 0;

	mutex_lock(&text_mutex);
	if (aarch64_insn_read(ip, &replaced)) {
		ret = -EFAULT;
		goto out;
	}

	if (replaced != old_insn) {
		ret = -EFAULT;
		goto out;
	}

	/* We call aarch64_insn_patch_text_nosync() to replace instruction
	 * atomically, so no other CPUs will fetch a half-new and half-old
	 * instruction. But there is chance that another CPU executes the
	 * old instruction after the patching operation finishes (e.g.,
	 * pipeline not flushed, or icache not synchronized yet).
	 *
	 * 1. when a new trampoline is attached, it is not a problem for
	 *    different CPUs to jump to different trampolines temporarily.
	 *
	 * 2. when an old trampoline is freed, we should wait for all other
	 *    CPUs to exit the trampoline and make sure the trampoline is no
	 *    longer reachable, since bpf_tramp_image_put() function already
	 *    uses percpu_ref and task-based rcu to do the sync, no need to call
	 *    the sync version here, see bpf_tramp_image_put() for details.
	 */
	ret = aarch64_insn_patch_text_nosync(ip, new_insn);
out:
	mutex_unlock(&text_mutex);

	return ret;
}
