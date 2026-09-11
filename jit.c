/**
 * \file jit.c
 *
 * \author Justin J. Meza
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "jit.h"
#include "error.h"

#if JIT_SUPPORTED

/**
 * How many machine registers each target lends to a procedure's values:
 * callee saved ones for values that outlive a call, scratch ones for the rest.
 */
#if defined(__aarch64__)
#define JIT_NUM_SAVED   9
#define JIT_NUM_SCRATCH 8
#elif defined(__x86_64__)
#define JIT_NUM_SAVED   6
#define JIT_NUM_SCRATCH 7
#endif

/**
 * The most registers a procedure may use and still be compiled.
 */
#define JIT_MAX_REGS (JIT_NUM_SAVED + JIT_NUM_SCRATCH)

/**
 * The most parameters a compiled procedure may take.  Every target passes at
 * least this many in registers.
 */
#define JIT_MAX_ARGS 4

#include <sys/mman.h>
#include <setjmp.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <libkern/OSCacheControl.h>
#endif

/**
 * \name Type analysis
 *
 * A forward pass over a procedure's instructions works out, for every point in
 * it, what type each register holds.  Where every read of every register has a
 * known scalar type, the generated code needs no tags and no checks: the
 * compiler knows statically what each raw 64 bit payload means.
 *
 * Parameters are assumed to be integers.  The caller checks that assumption
 * once, on entry, and falls back to the virtual machine when it does not hold.
 */
/**@{*/

/**
 * Merges two types at a point where control flow joins.
 */
static JitType mergeType(JitType a, JitType b)
{
	if (a == JT_BOTTOM) return b;
	if (b == JT_BOTTOM) return a;
	if (a == b) return a;
	return JT_UNKNOWN;
}

/**
 * The state of the analysis: the type of every register at every instruction.
 */
typedef struct {
	JitType *types;    /**< numcode by numregs, row major. */
	char *reached;     /**< Whether each instruction is reachable. */
	unsigned int numregs;
	unsigned int numcode;
} TypeMap;

#define TYPE_AT(m, pc, r) ((m)->types[(pc) * (m)->numregs + (r)])

/**
 * The type an instruction leaves in its destination register, given the types
 * of its operands.
 *
 * \retval JT_UNKNOWN The instruction is not one the generator handles, or its
 * result cannot be pinned down.
 */
static JitType resultType(const Instr *i, const JitType *in, Proto **callee)
{
	switch ((Opcode)i->op) {
		case OPC_LOADI:    return JT_INTEGER;
		case OPC_LOADB:    return JT_BOOLEAN;
		case OPC_LOADNIL:  return JT_NIL;
		case OPC_MOVE:     return in[i->b];
		case OPC_ADD: case OPC_SUB: case OPC_MUL:
		case OPC_MAX: case OPC_MIN:
			/* Integer arithmetic stays integral; anything else
			 * needs the interpreter's casting rules. */
			return (in[i->b] == JT_INTEGER && in[i->c] == JT_INTEGER)
					? JT_INTEGER : JT_UNKNOWN;
		case OPC_ADDI: case OPC_SUBI:
			return in[i->b] == JT_INTEGER ? JT_INTEGER : JT_UNKNOWN;
		case OPC_EQI:
			return in[i->b] == JT_INTEGER ? JT_BOOLEAN : JT_UNKNOWN;
		case OPC_EQ: case OPC_NEQ:
			/* Comparing unlike types has its own rules. */
			return (in[i->b] == in[i->c]
					&& (in[i->b] == JT_INTEGER || in[i->b] == JT_BOOLEAN))
					? JT_BOOLEAN : JT_UNKNOWN;
		case OPC_AND: case OPC_OR: case OPC_XOR: case OPC_NOT:
			return JT_BOOLEAN;
		case OPC_CALL:
			return (callee && *callee && (*callee)->jitcode)
					? (JitType)(*callee)->jitrettype : JT_UNKNOWN;
		default:
			return JT_UNKNOWN;
	}
}

/**
 * Whether an instruction writes to its \c a register.
 */
static int writesDest(Opcode op)
{
	switch (op) {
		case OPC_LOADK: case OPC_LOADI: case OPC_LOADB: case OPC_LOADNIL:
		case OPC_MOVE: case OPC_GETVAR:
		case OPC_ADD: case OPC_SUB: case OPC_MUL: case OPC_DIV:
		case OPC_MOD: case OPC_MAX: case OPC_MIN:
		case OPC_ADDI: case OPC_SUBI:
		case OPC_AND: case OPC_OR: case OPC_XOR: case OPC_NOT:
		case OPC_EQ: case OPC_NEQ: case OPC_EQI:
		case OPC_CAST: case OPC_CALL: case OPC_INPUT:
			return 1;
		default:
			return 0;
	}
}
/**@}*/

/**
 * \name Analysis driver
 */
/**@{*/

/**
 * Finds the procedure a call instruction reaches, if it always reaches one.
 */
static Proto *calleeOf(Proto *p, const Instr *i)
{
	if ((Opcode)i->op != OPC_CALL) return NULL;
	if (i->b < 0 || (unsigned)i->b >= p->numk) return NULL;
	return resolveStaticCallee(p->knames[i->b]);
}

/**
 * Lists the instructions control can reach from \a pc.
 *
 * \return The number of successors written to \a out.
 */
static int successors(const Proto *p, unsigned int pc, unsigned int *out)
{
	const Instr *i = p->code + pc;
	switch ((Opcode)i->op) {
		case OPC_RET: case OPC_RETNIL: case OPC_BREAK: case OPC_ENDPROC:
			return 0;
		case OPC_JMP:
			out[0] = pc + 1 + (unsigned)i->b;
			return 1;
		case OPC_JMPF: case OPC_JMPT:
			out[0] = pc + 1;
			out[1] = pc + 1 + (unsigned)i->b;
			return 2;
		default:
			out[0] = pc + 1;
			return 1;
	}
	(void)p;
}

/**
 * Works out what type every register holds at every instruction.
 *
 * \return Whether the procedure can be compiled, and if so what type it
 * returns in \a rettype.
 */
static int analyseProc(Proto *p, TypeMap *m, JitType *rettype)
{
	unsigned int pc;
	int changed = 1;
	int guard = 0;
	JitType ret = JT_BOTTOM;

	memset(m->reached, 0, m->numcode);
	for (pc = 0; pc < m->numcode * m->numregs; pc++) m->types[pc] = JT_BOTTOM;

	/* Parameters are integers; the caller checks that before entering. */
	for (pc = 0; pc < p->numargs; pc++) TYPE_AT(m, 0, pc) = JT_INTEGER;
	m->reached[0] = 1;

	while (changed && guard++ < 1000) {
		changed = 0;
		for (pc = 0; pc < m->numcode; pc++) {
			const Instr *i = p->code + pc;
			unsigned int succ[2];
			int nsucc, n, r;
			JitType out[JIT_MAX_REGS];

			if (!m->reached[pc]) continue;

			for (r = 0; r < (int)m->numregs; r++)
				out[r] = TYPE_AT(m, pc, r);
			if (writesDest((Opcode)i->op)) {
				Proto *callee = calleeOf(p, i);
				out[i->a] = resultType(i, out, &callee);
			}

			nsucc = successors(p, pc, succ);
			for (n = 0; n < nsucc; n++) {
				unsigned int t = succ[n];
				if (t >= m->numcode) continue;
				if (!m->reached[t]) {
					m->reached[t] = 1;
					changed = 1;
				}
				for (r = 0; r < (int)m->numregs; r++) {
					JitType merged = mergeType(TYPE_AT(m, t, r), out[r]);
					if (merged != TYPE_AT(m, t, r)) {
						TYPE_AT(m, t, r) = merged;
						changed = 1;
					}
				}
			}
		}
	}
	if (guard >= 1000) return 0;

	/* Check every reachable instruction is one the generator handles and
	 * that its operands have types it can work with. */
	for (pc = 0; pc < m->numcode; pc++) {
		const Instr *i = p->code + pc;
		JitType tb, tc;
		if (!m->reached[pc]) continue;
		tb = (i->b >= 0 && i->b < (int)m->numregs) ? TYPE_AT(m, pc, i->b) : JT_UNKNOWN;
		tc = (i->c >= 0 && i->c < (int)m->numregs) ? TYPE_AT(m, pc, i->c) : JT_UNKNOWN;

		switch ((Opcode)i->op) {
			case OPC_LOADI: case OPC_LOADB: case OPC_LOADNIL:
			case OPC_JMP:
				break;
			case OPC_MOVE:
				if (tb == JT_BOTTOM || tb == JT_UNKNOWN) return 0;
				break;
			case OPC_ADD: case OPC_SUB: case OPC_MUL:
			case OPC_MAX: case OPC_MIN:
				if (tb != JT_INTEGER || tc != JT_INTEGER) return 0;
				break;
			case OPC_ADDI: case OPC_SUBI: case OPC_EQI:
				if (tb != JT_INTEGER) return 0;
				break;
			case OPC_EQ: case OPC_NEQ:
				if (tb != tc) return 0;
				if (tb != JT_INTEGER && tb != JT_BOOLEAN) return 0;
				break;
			case OPC_AND: case OPC_OR: case OPC_XOR:
				if (tb == JT_UNKNOWN || tb == JT_BOTTOM) return 0;
				if (tc == JT_UNKNOWN || tc == JT_BOTTOM) return 0;
				break;
			case OPC_NOT: case OPC_JMPF: case OPC_JMPT: {
				JitType t = ((Opcode)i->op == OPC_NOT)
						? tb : TYPE_AT(m, pc, i->a);
				if (t == JT_UNKNOWN || t == JT_BOTTOM) return 0;
				break;
			}
			case OPC_CALL: {
				Proto *callee = calleeOf(p, i);
				unsigned int n;
				if (!callee || !callee->jitcode) return 0;
				if (callee->numargs != (unsigned int)i->c) return 0;
				if ((unsigned int)i->c > JIT_MAX_ARGS) return 0;
				for (n = 0; n < (unsigned int)i->c; n++) {
					if (i->a + 1 + n >= m->numregs) return 0;
					if (TYPE_AT(m, pc, i->a + 1 + n) != JT_INTEGER) return 0;
				}
				if (callee->jitrettype == JT_UNKNOWN
						|| callee->jitrettype == JT_BOTTOM)
					return 0;
				break;
			}
			case OPC_RET: {
				JitType t = TYPE_AT(m, pc, i->a);
				if (t == JT_UNKNOWN || t == JT_BOTTOM) return 0;
				ret = mergeType(ret, t);
				break;
			}
			case OPC_ENDPROC: {
				JitType t = (p->itreg >= 0 && p->itreg < (int)m->numregs)
						? TYPE_AT(m, pc, p->itreg) : JT_UNKNOWN;
				if (t == JT_UNKNOWN || t == JT_BOTTOM) return 0;
				ret = mergeType(ret, t);
				break;
			}
			default:
				/* Everything else touches scopes, strings or
				 * the outside world. */
				return 0;
		}
	}

	if (ret == JT_UNKNOWN || ret == JT_BOTTOM) return 0;
	*rettype = ret;
	return 1;
}
/**@}*/

/**
 * Called from generated code when the stack guard trips.
 */
static void jitStackOverflow(void);

/**
 * \name Register assignment
 *
 * A procedure's registers are kept in machine registers.  Only those still
 * holding something across a call need to survive one, so only those go in
 * callee saved registers and have to be pushed on entry; the rest use scratch
 * registers that cost nothing to set up.  For a small recursive procedure this
 * is the difference between saving ten registers per call and saving two.
 */
/**@{*/

/**
 * The registers an instruction reads.
 *
 * \return The number written to \a out.
 */
static int sourceRegs(const Proto *p, const Instr *i, int *out)
{
	int n = 0;
	switch ((Opcode)i->op) {
		case OPC_MOVE:
		case OPC_ADDI: case OPC_SUBI: case OPC_EQI: case OPC_NOT:
			out[n++] = i->b;
			break;
		case OPC_ADD: case OPC_SUB: case OPC_MUL:
		case OPC_MAX: case OPC_MIN:
		case OPC_EQ: case OPC_NEQ:
		case OPC_AND: case OPC_OR: case OPC_XOR:
			out[n++] = i->b;
			out[n++] = i->c;
			break;
		case OPC_JMPF: case OPC_JMPT: case OPC_RET:
			out[n++] = i->a;
			break;
		case OPC_CALL: {
			int k;
			for (k = 0; k < i->c; k++) out[n++] = i->a + 1 + k;
			break;
		}
		case OPC_ENDPROC:
			out[n++] = p->itreg;
			break;
		default:
			break;
	}
	return n;
}

/**
 * Marks the registers that hold a value across a call.
 */
static void findCrossCall(const Proto *p, const char *reached, char *cross)
{
	char *live = calloc(p->numcode + 1, p->numregs);
	unsigned int pc;
	int changed = 1;
	int guard = 0;
	int src[JIT_MAX_REGS + 1];

	if (!live) {
		perror("calloc");
		/* Be conservative: assume everything must survive. */
		memset(cross, 1, p->numregs);
		return;
	}

	while (changed && guard++ < 1000) {
		changed = 0;
		for (pc = p->numcode; pc > 0; pc--) {
			unsigned int at = pc - 1;
			const Instr *i = p->code + at;
			unsigned int succ[2];
			int nsucc = successors(p, at, succ);
			char out[JIT_MAX_REGS];
			int n, r, nsrc;

			if (!reached[at]) continue;

			memset(out, 0, p->numregs);
			for (n = 0; n < nsucc; n++)
				if (succ[n] < p->numcode)
					for (r = 0; r < (int)p->numregs; r++)
						if (live[succ[n] * p->numregs + r]) out[r] = 1;

			if ((Opcode)i->op == OPC_CALL)
				for (r = 0; r < (int)p->numregs; r++)
					if (out[r] && r != i->a) cross[r] = 1;

			if (writesDest((Opcode)i->op)) out[i->a] = 0;
			nsrc = sourceRegs(p, i, src);
			for (n = 0; n < nsrc; n++)
				if (src[n] >= 0 && src[n] < (int)p->numregs) out[src[n]] = 1;

			for (r = 0; r < (int)p->numregs; r++)
				if (out[r] != live[at * p->numregs + r]) {
					live[at * p->numregs + r] = out[r];
					changed = 1;
				}
		}
	}
	free(live);
}
/**@}*/


/**
 * \name Instruction buffer
 *
 * Code is built up as plain bytes so that the same buffer serves a fixed width
 * instruction set and a variable width one.
 */
/**@{*/

/**
 * A block of machine code being assembled.
 */
typedef struct {
	unsigned char *code; /**< The bytes emitted so far. */
	unsigned int num;    /**< How many bytes are used. */
	unsigned int cap;    /**< The allocated length of \a code. */
	int failed;          /**< Set if the buffer could not grow. */
} Asm;

static void emitByte(Asm *a, unsigned int b)
{
	if (a->failed) return;
	if (a->num == a->cap) {
		unsigned int newcap = a->cap ? a->cap * 2 : 1024;
		void *mem = realloc(a->code, newcap);
		if (!mem) {
			perror("realloc");
			a->failed = 1;
			return;
		}
		a->code = mem;
		a->cap = newcap;
	}
	a->code[a->num++] = (unsigned char)b;
}

/**
 * Emits a little endian value of \a n bytes.
 */
static void emitLE(Asm *a, unsigned long long v, int n)
{
	int i;
	for (i = 0; i < n; i++) emitByte(a, (unsigned int)((v >> (i * 8)) & 0xFF));
}

/**
 * Reads back a little endian value the buffer already holds.
 */
static void pokeLE(Asm *a, unsigned int at, unsigned long long v, int n)
{
	int i;
	if (a->failed || at + (unsigned int)n > a->num) return;
	for (i = 0; i < n; i++) a->code[at + i] = (unsigned char)((v >> (i * 8)) & 0xFF);
}
/**@}*/

/**
 * A condition to test, named the same way on every target.
 */
typedef enum {
	JC_EQ, /**< Equal. */
	JC_NE, /**< Not equal. */
	JC_LT, /**< Signed less than. */
	JC_GT, /**< Signed greater than. */
	JC_AE  /**< Unsigned above or equal. */
} JitCond;

static void jitStackOverflow(void);

#if defined(__aarch64__)

/**
 * \name AArch64 back end
 *
 * Instructions are four bytes each, so a branch's displacement counts
 * instructions and every patch site is the address of the instruction itself.
 */
/**@{*/

#define XZR 31
#define SPR 31

/** The registers a procedure's values live in. */
static const int jitSaved[] = { 19, 20, 21, 22, 23, 24, 25, 26, 27 };
static const int jitScratch[] = { 8, 9, 10, 11, 12, 13, 14, 15 };

/** A register the generator may use between instructions. */
#define JIT_TMP  16
/** A second one, for the stack check. */
#define JIT_TMP2 17

/** Where the nth argument arrives, and where a result is left. */
static int jitArgReg(int n) { return n; }
#define JIT_RET 0

static void emitWord(Asm *a, unsigned int w)
{
	emitLE(a, w, 4);
}

static unsigned int condBits(JitCond c)
{
	switch (c) {
		case JC_EQ: return 0x0;
		case JC_NE: return 0x1;
		case JC_LT: return 0xB;
		case JC_GT: return 0xC;
		default:    return 0x2; /* HS */
	}
}

static void emitMoveReg(Asm *a, int d, int s)
{
	if (d == s) return;
	emitWord(a, 0xAA0003E0u | ((unsigned)s << 16) | (unsigned)d);
}

static void emitLoadImm(Asm *a, int d, long long value)
{
	unsigned long long v = (unsigned long long)value;
	int started = 0;
	int n;

	/* A negative value with one significant half word is shorter built
	 * from its complement. */
	if (value < 0) {
		unsigned long long inv = ~v;
		int words = 0;
		for (n = 0; n < 4; n++)
			if ((inv >> (n * 16)) & 0xFFFF) words++;
		if (words <= 1) {
			for (n = 0; n < 4; n++) {
				unsigned int part = (unsigned int)((inv >> (n * 16)) & 0xFFFF);
				if (part) {
					emitWord(a, 0x92800000u | ((unsigned)n << 21)
							| (part << 5) | (unsigned)d);
					return;
				}
			}
			emitWord(a, 0x92800000u | (unsigned)d);
			return;
		}
	}
	for (n = 0; n < 4; n++) {
		unsigned int part = (unsigned int)((v >> (n * 16)) & 0xFFFF);
		if (!part) continue;
		emitWord(a, (started ? 0xF2800000u : 0xD2800000u)
				| ((unsigned)n << 21) | (part << 5) | (unsigned)d);
		started = 1;
	}
	if (!started) emitWord(a, 0xD2800000u | (unsigned)d);
}

static void emitAdd(Asm *a, int d, int n, int m)
{
	emitWord(a, 0x8B000000u | ((unsigned)m << 16) | ((unsigned)n << 5) | (unsigned)d);
}
static void emitSub(Asm *a, int d, int n, int m)
{
	emitWord(a, 0xCB000000u | ((unsigned)m << 16) | ((unsigned)n << 5) | (unsigned)d);
}
static void emitMul(Asm *a, int d, int n, int m)
{
	emitWord(a, 0x9B007C00u | ((unsigned)m << 16) | ((unsigned)n << 5) | (unsigned)d);
}
static void emitOr(Asm *a, int d, int n, int m)
{
	emitWord(a, 0xAA000000u | ((unsigned)m << 16) | ((unsigned)n << 5) | (unsigned)d);
}
static void emitAnd(Asm *a, int d, int n, int m)
{
	emitWord(a, 0x8A000000u | ((unsigned)m << 16) | ((unsigned)n << 5) | (unsigned)d);
}
static void emitXor(Asm *a, int d, int n, int m)
{
	emitWord(a, 0xCA000000u | ((unsigned)m << 16) | ((unsigned)n << 5) | (unsigned)d);
}

static void emitAddConst(Asm *a, int d, int n, long long v)
{
	if (v >= 0 && v < 4096)
		emitWord(a, 0x91000000u | ((unsigned)v << 10) | ((unsigned)n << 5) | (unsigned)d);
	else if (v < 0 && -v < 4096)
		emitWord(a, 0xD1000000u | ((unsigned)(-v) << 10) | ((unsigned)n << 5) | (unsigned)d);
	else {
		emitLoadImm(a, JIT_TMP, v);
		emitAdd(a, d, n, JIT_TMP);
	}
}

static void emitCmpReg(Asm *a, int n, int m)
{
	emitWord(a, 0xEB00001Fu | ((unsigned)m << 16) | ((unsigned)n << 5));
}

static void emitCmpConst(Asm *a, int n, long long v)
{
	if (v >= 0 && v < 4096)
		emitWord(a, 0xF100001Fu | ((unsigned)v << 10) | ((unsigned)n << 5));
	else if (v < 0 && -v < 4096)
		/* CMN, which compares against the negated value. */
		emitWord(a, 0xB100001Fu | ((unsigned)(-v) << 10) | ((unsigned)n << 5));
	else {
		emitLoadImm(a, JIT_TMP, v);
		emitCmpReg(a, n, JIT_TMP);
	}
}

static void emitSetCond(Asm *a, int d, JitCond c)
{
	unsigned int inv = condBits(c) ^ 1u;
	emitWord(a, 0x9A800400u | ((unsigned)XZR << 16) | (inv << 12)
			| ((unsigned)XZR << 5) | (unsigned)d);
}

static void emitSelCond(Asm *a, int d, int n, int m, JitCond c)
{
	emitWord(a, 0x9A800000u | ((unsigned)m << 16) | (condBits(c) << 12)
			| ((unsigned)n << 5) | (unsigned)d);
}

static unsigned int emitJump(Asm *a)
{
	unsigned int at = a->num;
	emitWord(a, 0x14000000u);
	return at;
}

static unsigned int emitJumpCond(Asm *a, JitCond c)
{
	unsigned int at = a->num;
	emitWord(a, 0x54000000u | condBits(c));
	return at;
}

static unsigned int emitJumpZero(Asm *a, int r)
{
	unsigned int at = a->num;
	emitWord(a, 0xB4000000u | (unsigned)r);
	return at;
}

static unsigned int emitJumpNotZero(Asm *a, int r)
{
	unsigned int at = a->num;
	emitWord(a, 0xB5000000u | (unsigned)r);
	return at;
}

static void patchJump(Asm *a, unsigned int at, unsigned int target)
{
	int off;
	unsigned int word;
	if (a->failed || at + 4 > a->num) return;
	off = ((int)target - (int)at) / 4;
	word = (unsigned int)a->code[at]
			| ((unsigned int)a->code[at + 1] << 8)
			| ((unsigned int)a->code[at + 2] << 16)
			| ((unsigned int)a->code[at + 3] << 24);
	if ((word & 0xFC000000u) == 0x14000000u)
		word = 0x14000000u | ((unsigned int)off & 0x03FFFFFFu);
	else
		/* Conditional branches and the compare and branch forms all
		 * carry a nineteen bit displacement at bit five. */
		word = (word & 0xFF00001Fu) | (((unsigned int)off & 0x7FFFFu) << 5);
	pokeLE(a, at, word, 4);
}

static unsigned int emitCallRel(Asm *a)
{
	unsigned int at = a->num;
	emitWord(a, 0x94000000u);
	return at;
}

static void patchCallRel(unsigned char *base, unsigned int at, void *target)
{
	long long off = ((long long)(intptr_t)target
			- (long long)(intptr_t)(base + at)) / 4;
	unsigned int word = 0x94000000u | ((unsigned int)off & 0x03FFFFFFu);
	int i;
	for (i = 0; i < 4; i++) base[at + i] = (unsigned char)((word >> (i * 8)) & 0xFF);
}

static void emitProlog(Asm *a, const int *saved, int nsaved)
{
	int n;
	/* STP x29, x30, [sp, #-16]! */
	emitWord(a, 0xA9800000u | (0x7Eu << 15) | (30u << 10) | ((unsigned)SPR << 5) | 29u);
	for (n = 0; n < nsaved; n += 2) {
		int t2 = (n + 1 < nsaved) ? saved[n + 1] : XZR;
		emitWord(a, 0xA9800000u | (0x7Eu << 15) | ((unsigned)t2 << 10)
				| ((unsigned)SPR << 5) | (unsigned)saved[n]);
	}
}

static void emitEpilog(Asm *a, const int *saved, int nsaved)
{
	int n;
	for (n = ((nsaved + 1) & ~1) - 2; n >= 0; n -= 2) {
		int t2 = (n + 1 < nsaved) ? saved[n + 1] : XZR;
		emitWord(a, 0xA8C00000u | (2u << 15) | ((unsigned)t2 << 10)
				| ((unsigned)SPR << 5) | (unsigned)saved[n]);
	}
	/* LDP x29, x30, [sp], #16 */
	emitWord(a, 0xA8C00000u | (2u << 15) | (30u << 10) | ((unsigned)SPR << 5) | 29u);
	emitWord(a, 0xD65F03C0u); /* RET */
}

/**
 * Refuses to run when the stack is nearly gone.
 *
 * The floor is a fixed address by the time this runs, so it travels in a
 * literal placed after the code and costs a single load.
 */
static unsigned int emitStackCheck(Asm *a)
{
	unsigned int ldr = a->num;
	unsigned int ok;
	emitWord(a, 0x58000000u | (unsigned)JIT_TMP);          /* LDR x16, <literal> */
	emitWord(a, 0x91000000u | ((unsigned)SPR << 5) | (unsigned)JIT_TMP2); /* MOV x17, sp */
	emitCmpReg(a, JIT_TMP2, JIT_TMP);
	ok = emitJumpCond(a, JC_AE);
	emitLoadImm(a, JIT_TMP, (long long)(intptr_t)jitStackOverflow);
	emitWord(a, 0xD63F0000u | ((unsigned)JIT_TMP << 5));   /* BLR x16 */
	patchJump(a, ok, a->num);
	return ldr;
}

/**
 * Puts the stack floor where \ref emitStackCheck can load it.
 */
static void emitLiteralPool(Asm *a, unsigned int ldr, long long floor)
{
	unsigned int pool;
	while (a->num & 7) emitWord(a, 0xD503201Fu); /* NOP */
	pool = a->num;
	emitLE(a, (unsigned long long)floor, 8);
	pokeLE(a, ldr, 0x58000000u | ((((pool - ldr) / 4) & 0x7FFFFu) << 5)
			| (unsigned)JIT_TMP, 4);
}

/**
 * Rounds a procedure's length so that the next one stays aligned.
 */
static void emitAlignProc(Asm *a)
{
	while (a->num & 7) emitWord(a, 0xD503201Fu);
}
/**@}*/

#elif defined(__x86_64__)

/**
 * \name x86-64 back end
 *
 * Instructions vary in length, so a patch site is the address of the four byte
 * displacement itself, which is relative to the end of the instruction.
 *
 * Arithmetic here is two operand, so an instruction whose destination is not
 * also its first source needs a move in front of it, and one whose destination
 * collides with its second source borrows the scratch register.
 */
/**@{*/

/* Register numbers, in the order the encoding uses them. */
#define RAX 0
#define RCX 1
#define RDX 2
#define RBX 3
#define RSP 4
#define RBP 5
#define RSI 6
#define RDI 7

/** The registers a procedure's values live in. */
static const int jitSaved[] = { RBX, RBP, 12, 13, 14, 15 };
static const int jitScratch[] = { RCX, RDX, RSI, RDI, 8, 9, 10 };

/** Two registers the generator may use between instructions. */
#define JIT_TMP  RAX
#define JIT_TMP2 11

/** Where the nth argument arrives, and where a result is left. */
static int jitArgReg(int n)
{
	static const int args[] = { RDI, RSI, RDX, RCX, 8, 9 };
	return args[n];
}
#define JIT_RET RAX

static unsigned int condBits(JitCond c)
{
	switch (c) {
		case JC_EQ: return 0x4;
		case JC_NE: return 0x5;
		case JC_LT: return 0xC;
		case JC_GT: return 0xF;
		default:    return 0x3; /* AE */
	}
}

/**
 * Emits a one byte opcode taking two registers, with the operand size prefix.
 */
static void x86Op(Asm *a, unsigned int op, int reg, int rm)
{
	emitByte(a, 0x48u | ((reg >= 8) << 2) | (rm >= 8));
	emitByte(a, op);
	emitByte(a, 0xC0u | (((unsigned)reg & 7) << 3) | ((unsigned)rm & 7));
}

/**
 * Emits a two byte opcode taking two registers.
 */
static void x86Op0F(Asm *a, unsigned int op, int reg, int rm)
{
	emitByte(a, 0x48u | ((reg >= 8) << 2) | (rm >= 8));
	emitByte(a, 0x0F);
	emitByte(a, op);
	emitByte(a, 0xC0u | (((unsigned)reg & 7) << 3) | ((unsigned)rm & 7));
}

/**
 * Emits an opcode taking a register and a thirty two bit immediate.
 */
static void x86OpImm(Asm *a, unsigned int op, unsigned int ext, int rm, long long v)
{
	emitByte(a, 0x48u | (rm >= 8));
	emitByte(a, op);
	emitByte(a, 0xC0u | (ext << 3) | ((unsigned)rm & 7));
	emitLE(a, (unsigned long long)(unsigned int)(int)v, 4);
}

static void emitMoveReg(Asm *a, int d, int s)
{
	if (d == s) return;
	x86Op(a, 0x89, s, d);
}

static void emitLoadImm(Asm *a, int d, long long value)
{
	if (value == 0) {
		/* XOR clears without a displacement. */
		x86Op(a, 0x31, d, d);
		return;
	}
	if (value > 0 && value <= 0xFFFFFFFFLL) {
		/* Writing the low half of a register clears the high half. */
		if (d >= 8) emitByte(a, 0x41);
		emitByte(a, 0xB8u + ((unsigned)d & 7));
		emitLE(a, (unsigned long long)value, 4);
		return;
	}
	emitByte(a, 0x48u | (d >= 8));
	emitByte(a, 0xB8u + ((unsigned)d & 7));
	emitLE(a, (unsigned long long)value, 8);
}

/**
 * Emits a two operand instruction as though it had three.
 */
static void x86Binary(Asm *a, unsigned int op, int d, int n, int m, int commutes)
{
	if (d == n) {
		x86Op(a, op, m, d);
	}
	else if (d == m) {
		if (commutes) {
			x86Op(a, op, n, d);
		}
		else {
			emitMoveReg(a, JIT_TMP, n);
			x86Op(a, op, m, JIT_TMP);
			emitMoveReg(a, d, JIT_TMP);
		}
	}
	else {
		emitMoveReg(a, d, n);
		x86Op(a, op, m, d);
	}
}

static void emitAdd(Asm *a, int d, int n, int m) { x86Binary(a, 0x01, d, n, m, 1); }
static void emitSub(Asm *a, int d, int n, int m) { x86Binary(a, 0x29, d, n, m, 0); }
static void emitOr (Asm *a, int d, int n, int m) { x86Binary(a, 0x09, d, n, m, 1); }
static void emitAnd(Asm *a, int d, int n, int m) { x86Binary(a, 0x21, d, n, m, 1); }
static void emitXor(Asm *a, int d, int n, int m) { x86Binary(a, 0x31, d, n, m, 1); }

static void emitMul(Asm *a, int d, int n, int m)
{
	/* IMUL takes its destination as the first source. */
	if (d == n) {
		x86Op0F(a, 0xAF, d, m);
	}
	else if (d == m) {
		x86Op0F(a, 0xAF, d, n);
	}
	else {
		emitMoveReg(a, d, n);
		x86Op0F(a, 0xAF, d, m);
	}
}

static void emitAddConst(Asm *a, int d, int n, long long v)
{
	emitMoveReg(a, d, n);
	if (v == 0) return;
	if (v >= -2147483648LL && v <= 2147483647LL) {
		if (v > 0) x86OpImm(a, 0x81, 0, d, v);
		else x86OpImm(a, 0x81, 5, d, -v);
	}
	else {
		emitLoadImm(a, JIT_TMP, v);
		x86Op(a, 0x01, JIT_TMP, d);
	}
}

static void emitCmpReg(Asm *a, int n, int m)
{
	/* CMP r/m64, r64 computes r/m minus reg. */
	x86Op(a, 0x39, m, n);
}

static void emitCmpConst(Asm *a, int n, long long v)
{
	if (v >= -2147483648LL && v <= 2147483647LL) {
		x86OpImm(a, 0x81, 7, n, v);
	}
	else {
		emitLoadImm(a, JIT_TMP, v);
		emitCmpReg(a, n, JIT_TMP);
	}
}

static void emitSetCond(Asm *a, int d, JitCond c)
{
	/* SETcc writes one byte, so the rest of the register is cleared after. */
	emitByte(a, 0x40u | (d >= 8));
	emitByte(a, 0x0F);
	emitByte(a, 0x90u + condBits(c));
	emitByte(a, 0xC0u | ((unsigned)d & 7));
	x86Op0F(a, 0xB6, d, d); /* MOVZX r64, r8 */
}

static void emitSelCond(Asm *a, int d, int n, int m, JitCond c)
{
	/* CMOV does not disturb the flags the comparison set. */
	if (d == n) {
		x86Op0F(a, 0x40u + (condBits(c) ^ 1u), d, m);
	}
	else {
		emitMoveReg(a, d, m);
		x86Op0F(a, 0x40u + condBits(c), d, n);
	}
}

static unsigned int emitJump(Asm *a)
{
	unsigned int at;
	emitByte(a, 0xE9);
	at = a->num;
	emitLE(a, 0, 4);
	return at;
}

static unsigned int emitJumpCond(Asm *a, JitCond c)
{
	unsigned int at;
	emitByte(a, 0x0F);
	emitByte(a, 0x80u + condBits(c));
	at = a->num;
	emitLE(a, 0, 4);
	return at;
}

static unsigned int emitJumpZero(Asm *a, int r)
{
	x86Op(a, 0x85, r, r); /* TEST r, r */
	return emitJumpCond(a, JC_EQ);
}

static unsigned int emitJumpNotZero(Asm *a, int r)
{
	x86Op(a, 0x85, r, r);
	return emitJumpCond(a, JC_NE);
}

static void patchJump(Asm *a, unsigned int at, unsigned int target)
{
	/* A displacement counts from the end of the instruction. */
	pokeLE(a, at, (unsigned long long)(unsigned int)
			((int)target - (int)(at + 4)), 4);
}

static unsigned int emitCallRel(Asm *a)
{
	unsigned int at;
	emitByte(a, 0xE8);
	at = a->num;
	emitLE(a, 0, 4);
	return at;
}

static void patchCallRel(unsigned char *base, unsigned int at, void *target)
{
	long long off = (long long)(intptr_t)target
			- (long long)(intptr_t)(base + at + 4);
	int i;
	for (i = 0; i < 4; i++)
		base[at + i] = (unsigned char)(((unsigned long long)off >> (i * 8)) & 0xFF);
}

static void emitPushReg(Asm *a, int r)
{
	if (r >= 8) emitByte(a, 0x41);
	emitByte(a, 0x50u + ((unsigned)r & 7));
}

static void emitPopReg(Asm *a, int r)
{
	if (r >= 8) emitByte(a, 0x41);
	emitByte(a, 0x58u + ((unsigned)r & 7));
}

static void emitProlog(Asm *a, const int *saved, int nsaved)
{
	int n;
	for (n = 0; n < nsaved; n++) emitPushReg(a, saved[n]);
	/* A call arrives with the stack eight past a sixteen byte boundary, so
	 * an even number of pushes leaves it misaligned for the next call. */
	if ((nsaved & 1) == 0) x86OpImm(a, 0x81, 5, RSP, 8);
}

static void emitEpilog(Asm *a, const int *saved, int nsaved)
{
	int n;
	if ((nsaved & 1) == 0) x86OpImm(a, 0x81, 0, RSP, 8);
	for (n = nsaved - 1; n >= 0; n--) emitPopReg(a, saved[n]);
	emitByte(a, 0xC3); /* RET */
}

/**
 * Refuses to run when the stack is nearly gone.  The floor is loaded as an
 * immediate; there is no literal pool to fill in afterwards.
 */
static unsigned int emitStackCheck(Asm *a)
{
	unsigned int at = a->num;
	unsigned int ok;
	/* The floor is poked in later, so reserve the full ten byte form. */
	emitByte(a, 0x48);
	emitByte(a, 0xB8u + JIT_TMP);
	emitLE(a, 0, 8);
	emitCmpReg(a, RSP, JIT_TMP);
	ok = emitJumpCond(a, JC_AE);
	emitLoadImm(a, JIT_TMP, (long long)(intptr_t)jitStackOverflow);
	emitByte(a, 0xFF);
	emitByte(a, 0xD0u + JIT_TMP); /* CALL rax */
	patchJump(a, ok, a->num);
	return at;
}

static void emitLiteralPool(Asm *a, unsigned int at, long long floor)
{
	pokeLE(a, at + 2, (unsigned long long)floor, 8);
}

static void emitAlignProc(Asm *a)
{
	while (a->num & 7) emitByte(a, 0x90); /* NOP */
}
/**@}*/

#endif /* back end */

/**
 * \name Code generation
 */
/**@{*/

/**
 * A call that has to be pointed at its target once every procedure has been
 * laid out.
 */
typedef struct {
	unsigned int at; /**< The displacement to fill in. */
	Proto *callee;   /**< Where it should land. */
} CallSite;

/**
 * Everything gathered while generating one procedure.
 */
typedef struct {
	Asm a;                 /**< The instructions. */
	unsigned int *pcmap;   /**< Bytecode index to byte offset. */
	unsigned int *fixat;   /**< Branch sites to patch. */
	unsigned int *fixto;   /**< The bytecode index each one targets. */
	unsigned int numfix;   /**< The number of branches to patch. */
	unsigned int capfix;   /**< The allocated length of the fix arrays. */
	CallSite *calls;       /**< The calls to patch. */
	unsigned int numcalls; /**< The number of calls. */
	unsigned int capcalls; /**< The allocated length of \a calls. */
	unsigned int stackat;  /**< Where the stack floor goes. */
	int saved[JIT_NUM_SAVED]; /**< The machine registers the prologue pushes. */
	int nsaved;            /**< How many of them there are. */
	int map[JIT_MAX_REGS]; /**< Machine register for each procedure register. */
} Gen;

/** The machine register a procedure register lives in. */
#define REG(r) (g->map[(r)])

static void addFix(Gen *g, unsigned int at, unsigned int to)
{
	if (g->numfix == g->capfix) {
		unsigned int newcap = g->capfix ? g->capfix * 2 : 16;
		void *m1 = realloc(g->fixat, sizeof(unsigned int) * newcap);
		void *m2 = realloc(g->fixto, sizeof(unsigned int) * newcap);
		if (!m1 || !m2) {
			perror("realloc");
			g->a.failed = 1;
			free(m1);
			free(m2);
			return;
		}
		g->fixat = m1;
		g->fixto = m2;
		g->capfix = newcap;
	}
	g->fixat[g->numfix] = at;
	g->fixto[g->numfix] = to;
	g->numfix++;
}

static void addCall(Gen *g, unsigned int at, Proto *callee)
{
	if (g->numcalls == g->capcalls) {
		unsigned int newcap = g->capcalls ? g->capcalls * 2 : 8;
		void *mem = realloc(g->calls, sizeof(CallSite) * newcap);
		if (!mem) {
			perror("realloc");
			g->a.failed = 1;
			return;
		}
		g->calls = mem;
		g->capcalls = newcap;
	}
	g->calls[g->numcalls].at = at;
	g->calls[g->numcalls].callee = callee;
	g->numcalls++;
}

/**
 * Moves a set of values into a set of registers at once.
 *
 * The destinations may overlap the sources, which happens on a target whose
 * argument registers are also general ones, so a move is only emitted when its
 * destination is not still needed as a source.  A cycle is broken by parking
 * one value in the scratch register.
 */
static void emitParallelMove(Asm *a, const int *dst, const int *src, int n)
{
	int from[JIT_MAX_ARGS];
	int done[JIT_MAX_ARGS];
	int i, j, moved, pending = 0;

	for (i = 0; i < n; i++) {
		from[i] = src[i];
		done[i] = (dst[i] == src[i]);
		if (!done[i]) pending++;
	}

	while (pending > 0) {
		moved = 0;
		for (i = 0; i < n; i++) {
			int blocked = 0;
			if (done[i]) continue;
			for (j = 0; j < n; j++)
				if (!done[j] && j != i && from[j] == dst[i]) blocked = 1;
			if (blocked) continue;
			emitMoveReg(a, dst[i], from[i]);
			done[i] = 1;
			pending--;
			moved = 1;
		}
		if (moved) continue;
		/* Everything left is part of a cycle. */
		for (i = 0; i < n; i++) {
			if (done[i]) continue;
			emitMoveReg(a, JIT_TMP, from[i]);
			for (j = 0; j < n; j++)
				if (!done[j] && from[j] == from[i]) from[j] = JIT_TMP;
			break;
		}
	}
}

/**
 * Puts the truth of a value into \a dst as a zero or one.
 */
static void emitTruth(Asm *a, int dst, int src, JitType type)
{
	if (type == JT_NIL) {
		emitLoadImm(a, dst, 0);
	}
	else if (type == JT_BOOLEAN) {
		emitMoveReg(a, dst, src);
	}
	else {
		emitCmpConst(a, src, 0);
		emitSetCond(a, dst, JC_NE);
	}
}

/**
 * Decides which machine register each of a procedure's registers lives in.
 *
 * Only values that are still wanted after a call need a register the callee
 * will preserve, and only those have to be pushed on entry.
 *
 * \return Whether they all fit.
 */
static int assignRegs(const Proto *p, const char *reached, Gen *g)
{
	char cross[JIT_MAX_REGS];
	unsigned int r;
	int saved = 0;
	int scratch = 0;

	memset(cross, 0, sizeof(cross));
	findCrossCall(p, reached, cross);

	for (r = 0; r < p->numregs; r++) {
		if (!cross[r]) continue;
		if (saved >= JIT_NUM_SAVED) return 0;
		g->map[r] = jitSaved[saved++];
	}
	for (r = 0; r < p->numregs; r++) {
		if (cross[r]) continue;
		if (scratch < JIT_NUM_SCRATCH) g->map[r] = jitScratch[scratch++];
		else if (saved < JIT_NUM_SAVED) g->map[r] = jitSaved[saved++];
		else return 0;
	}
	for (r = 0; r < (unsigned int)saved; r++) g->saved[r] = jitSaved[r];
	g->nsaved = saved;
	return 1;
}

/**
 * Generates machine code for one procedure.
 *
 * \return Whether generation succeeded.
 */
static int generate(Proto *p, TypeMap *m, Gen *g)
{
	unsigned int pc;
	unsigned int n;
	int dst[JIT_MAX_ARGS], src[JIT_MAX_ARGS];

	if (!assignRegs(p, m->reached, g)) return 0;

	emitProlog(&g->a, g->saved, g->nsaved);
	g->stackat = emitStackCheck(&g->a);
	for (n = 0; n < p->numargs; n++) {
		dst[n] = REG((int)n);
		src[n] = jitArgReg((int)n);
	}
	emitParallelMove(&g->a, dst, src, (int)p->numargs);

	for (pc = 0; pc < p->numcode; pc++) {
		const Instr *i = p->code + pc;
		int d;
		g->pcmap[pc] = g->a.num;
		if (!m->reached[pc]) continue;
		d = REG(i->a);

		switch ((Opcode)i->op) {
			case OPC_LOADI:
			case OPC_LOADB:
				emitLoadImm(&g->a, d, i->b);
				break;
			case OPC_LOADNIL:
				emitLoadImm(&g->a, d, 0);
				break;
			case OPC_MOVE:
				emitMoveReg(&g->a, d, REG(i->b));
				break;
			case OPC_ADD:
				emitAdd(&g->a, d, REG(i->b), REG(i->c));
				break;
			case OPC_SUB:
				emitSub(&g->a, d, REG(i->b), REG(i->c));
				break;
			case OPC_MUL:
				emitMul(&g->a, d, REG(i->b), REG(i->c));
				break;
			case OPC_MAX:
				emitCmpReg(&g->a, REG(i->b), REG(i->c));
				emitSelCond(&g->a, d, REG(i->b), REG(i->c), JC_GT);
				break;
			case OPC_MIN:
				emitCmpReg(&g->a, REG(i->b), REG(i->c));
				emitSelCond(&g->a, d, REG(i->b), REG(i->c), JC_LT);
				break;
			case OPC_ADDI:
				emitAddConst(&g->a, d, REG(i->b), i->c);
				break;
			case OPC_SUBI:
				emitAddConst(&g->a, d, REG(i->b), -(long long)i->c);
				break;
			case OPC_EQI:
				emitCmpConst(&g->a, REG(i->b), i->c);
				emitSetCond(&g->a, d, JC_EQ);
				break;
			case OPC_EQ: case OPC_NEQ:
				emitCmpReg(&g->a, REG(i->b), REG(i->c));
				emitSetCond(&g->a, d,
						(Opcode)i->op == OPC_EQ ? JC_EQ : JC_NE);
				break;
			case OPC_AND: case OPC_OR: case OPC_XOR: {
				/* A boolean's payload is already zero or one, so
				 * only other types need reducing first. */
				int rb = REG(i->b), rc = REG(i->c);
				if (TYPE_AT(m, pc, i->b) != JT_BOOLEAN) {
					emitTruth(&g->a, JIT_TMP, rb, TYPE_AT(m, pc, i->b));
					rb = JIT_TMP;
				}
				if (TYPE_AT(m, pc, i->c) != JT_BOOLEAN) {
					emitTruth(&g->a, JIT_TMP2, rc, TYPE_AT(m, pc, i->c));
					rc = JIT_TMP2;
				}
				if ((Opcode)i->op == OPC_AND) emitAnd(&g->a, d, rb, rc);
				else if ((Opcode)i->op == OPC_OR) emitOr(&g->a, d, rb, rc);
				else emitXor(&g->a, d, rb, rc);
				break;
			}
			case OPC_NOT: {
				int rb = REG(i->b);
				if (TYPE_AT(m, pc, i->b) != JT_BOOLEAN) {
					emitTruth(&g->a, JIT_TMP, rb, TYPE_AT(m, pc, i->b));
					rb = JIT_TMP;
				}
				emitCmpConst(&g->a, rb, 0);
				emitSetCond(&g->a, d, JC_EQ);
				break;
			}
			case OPC_JMP:
				addFix(g, emitJump(&g->a), pc + 1 + (unsigned)i->b);
				break;
			case OPC_JMPF: case OPC_JMPT: {
				JitType t = TYPE_AT(m, pc, i->a);
				if (t == JT_NIL) {
					/* Nil is never true. */
					if ((Opcode)i->op == OPC_JMPF)
						addFix(g, emitJump(&g->a),
								pc + 1 + (unsigned)i->b);
					break;
				}
				if ((Opcode)i->op == OPC_JMPF)
					addFix(g, emitJumpZero(&g->a, d),
							pc + 1 + (unsigned)i->b);
				else
					addFix(g, emitJumpNotZero(&g->a, d),
							pc + 1 + (unsigned)i->b);
				break;
			}
			case OPC_CALL: {
				Proto *callee = calleeOf(p, i);
				for (n = 0; n < (unsigned int)i->c; n++) {
					dst[n] = jitArgReg((int)n);
					src[n] = REG(i->a + 1 + (int)n);
				}
				emitParallelMove(&g->a, dst, src, i->c);
				addCall(g, emitCallRel(&g->a), callee);
				emitMoveReg(&g->a, d, JIT_RET);
				break;
			}
			case OPC_RET:
				emitMoveReg(&g->a, JIT_RET, d);
				emitEpilog(&g->a, g->saved, g->nsaved);
				break;
			case OPC_ENDPROC:
				emitMoveReg(&g->a, JIT_RET, REG(p->itreg));
				emitEpilog(&g->a, g->saved, g->nsaved);
				break;
			default:
				return 0;
		}
	}

	if (g->a.failed) return 0;

	/* Point every branch at the code its bytecode target became. */
	for (n = 0; n < g->numfix; n++) {
		unsigned int to = g->fixto[n];
		patchJump(&g->a, g->fixat[n],
				to < p->numcode ? g->pcmap[to] : g->a.num);
	}
	emitLiteralPool(&g->a, g->stackat, (long long)(intptr_t)stackFloor);
	emitAlignProc(&g->a);
	return !g->a.failed;
}
/**@}*/

/**
 * \name Driver
 */
/**@{*/

/**
 * The regions holding generated code.  A process may compile more than once --
 * an interactive session compiles each statement it reads -- so they are kept
 * in a list rather than a single pointer.
 */
typedef struct jitregion {
	struct jitregion *next; /**< The previously allocated region. */
	void *mem;              /**< The region itself. */
	size_t size;            /**< Its length. */
} JitRegion;

static JitRegion *jitregions = NULL;

/**
 * Every procedure compiled to machine code, so that they can be freed.
 */

/**
 * Where to resume when generated code runs out of stack.  Generated frames
 * have nothing to clean up but the machine registers they saved, which
 * \c longjmp restores, so unwinding this way is safe.
 */
static jmp_buf jitbail;

static void jitStackOverflow(void)
{
	longjmp(jitbail, 1);
}

/**
 * Reserves memory that may hold executable code.
 */
static void *allocExecutable(size_t size)
{
	void *mem;
#if defined(__APPLE__)
	mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
			MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
	if (mem == MAP_FAILED)
		mem = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
				MAP_PRIVATE | MAP_ANON, -1, 0);
#else
	mem = mmap(NULL, size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANON, -1, 0);
#endif
	return mem == MAP_FAILED ? NULL : mem;
}

/**
 * Opens the region for writing, where the system keeps code and data apart.
 */
static void beginWriting(void)
{
#if defined(__APPLE__) && defined(__aarch64__)
	pthread_jit_write_protect_np(0);
#endif
}

/**
 * Makes written code executable and visible to the instruction fetcher.
 */
static int finishExecutable(void *mem, size_t size)
{
#if defined(__APPLE__)
#if defined(__aarch64__)
	pthread_jit_write_protect_np(1);
	sys_icache_invalidate(mem, size);
#endif
	return 1;
#else
	if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0) {
		perror("mprotect");
		return 0;
	}
	__builtin___clear_cache((char *)mem, (char *)mem + size);
	return 1;
#endif
}

void jitCompileAll(Proto **protos, unsigned int num)
{
	Gen *gens = NULL;
	TypeMap map;
	unsigned int n, i;
	unsigned int total = 0;
	unsigned int changed = 1;
	unsigned int guard = 0;
	unsigned int maxregs = 0, maxcode = 0;

	if (!num || getenv("LCI_NOJIT")) return;

	for (n = 0; n < num; n++) {
		Proto *p = protos[n];
		p->jitcode = NULL;
		p->jitrettype = JT_UNKNOWN;
		if (p->needscope) continue;
		if (p->numregs == 0 || p->numregs > JIT_MAX_REGS) continue;
		if (p->numargs > JIT_MAX_ARGS) continue;
		if (p->itreg < 0 || (unsigned int)p->itreg >= p->numregs) continue;
		/* Assume the best, then check the assumption below. */
		p->jitcode = (void *)1;
		p->jitrettype = JT_INTEGER;
		if (p->numregs > maxregs) maxregs = p->numregs;
		if (p->numcode > maxcode) maxcode = p->numcode;
	}
	if (!maxcode) return;

	map.numregs = maxregs;
	map.numcode = maxcode;
	map.types = malloc(sizeof(JitType) * maxcode * maxregs);
	map.reached = malloc(maxcode);
	if (!map.types || !map.reached) {
		perror("malloc");
		free(map.types);
		free(map.reached);
		return;
	}

	/* A procedure's return type depends on what it calls, which may include
	 * itself, so the whole set has to settle together. */
	while (changed && guard++ < 100) {
		changed = 0;
		for (n = 0; n < num; n++) {
			Proto *p = protos[n];
			JitType ret = JT_UNKNOWN;
			if (!p->jitcode) continue;
			map.numregs = p->numregs;
			map.numcode = p->numcode;
			if (!analyseProc(p, &map, &ret)) {
				p->jitcode = NULL;
				p->jitrettype = JT_UNKNOWN;
				changed = 1;
			}
			else if ((JitType)p->jitrettype != ret) {
				p->jitrettype = ret;
				changed = 1;
			}
		}
	}

	gens = calloc(num, sizeof(Gen));
	if (!gens) {
		perror("calloc");
		free(map.types);
		free(map.reached);
		return;
	}

	for (n = 0; n < num; n++) {
		Proto *p = protos[n];
		JitType ret = JT_UNKNOWN;
		if (!p->jitcode) continue;
		map.numregs = p->numregs;
		map.numcode = p->numcode;
		if (!analyseProc(p, &map, &ret)) {
			p->jitcode = NULL;
			continue;
		}
		gens[n].pcmap = calloc(p->numcode, sizeof(unsigned int));
		if (!gens[n].pcmap || !generate(p, &map, &gens[n])) {
			p->jitcode = NULL;
			continue;
		}
		total += gens[n].a.num;
	}

	if (total) {
		size_t size = (total + 4095) & ~(size_t)4095;
		void *jitmem = allocExecutable(size);
		if (!jitmem) {
			/* Without executable memory everything simply runs on the
			 * virtual machine. */
			for (n = 0; n < num; n++) protos[n]->jitcode = NULL;
		}
		else {
			unsigned char *out = jitmem;
			unsigned int off = 0;
			JitRegion *region = malloc(sizeof(JitRegion));
			if (region) {
				region->mem = jitmem;
				region->size = size;
				region->next = jitregions;
				jitregions = region;
			}
			beginWriting();
			for (n = 0; n < num; n++) {
				if (!protos[n]->jitcode) continue;
				protos[n]->jitcode = out + off;
				off += gens[n].a.num;
			}
			/* Copy each procedure in and point its calls at the
			 * addresses the targets ended up at. */
			off = 0;
			for (n = 0; n < num; n++) {
				Gen *g = gens + n;
				unsigned char *dst;
				if (!protos[n]->jitcode) continue;
				dst = out + off;
				memcpy(dst, g->a.code, g->a.num);
				for (i = 0; i < g->numcalls; i++) {
					Proto *callee = g->calls[i].callee;
					if (!callee || !callee->jitcode) continue;
					patchCallRel(dst, g->calls[i].at, callee->jitcode);
				}
				off += g->a.num;
			}
			if (!finishExecutable(jitmem, size))
				for (n = 0; n < num; n++) protos[n]->jitcode = NULL;
		}
	}

	/* Writing the generated code out lets a disassembler check it. */
	if (getenv("LCI_DUMP_ASM")) {
		FILE *f = fopen(getenv("LCI_DUMP_ASM"), "wb");
		if (f) {
			for (n = 0; n < num; n++)
				if (protos[n]->jitcode)
					fwrite(gens[n].a.code, 1, gens[n].a.num, f);
			fclose(f);
		}
		else perror(getenv("LCI_DUMP_ASM"));
	}
	if (getenv("LCI_DUMP"))
		for (n = 0; n < num; n++)
			fprintf(stderr, "; proto %u: %s\n", n,
					protos[n]->jitcode ? "native" : "interpreted");

	for (n = 0; n < num; n++) {
		free(gens[n].a.code);
		free(gens[n].pcmap);
		free(gens[n].fixat);
		free(gens[n].fixto);
		free(gens[n].calls);
	}
	free(gens);
	free(map.types);
	free(map.reached);
}

ValueObject *jitCall(Proto *proto, ValueObject **args, unsigned int numargs)
{
	long long a[JIT_MAX_ARGS];
	long long r;
	unsigned int n;
	ValueObject *ret;

	for (n = 0; n < JIT_MAX_ARGS; n++)
		a[n] = (n < numargs) ? args[n]->data.i : 0;

	if (setjmp(jitbail)) {
		error(IN_RECURSION_TOO_DEEP);
		return NULL;
	}

	switch (numargs) {
		case 0:
			r = ((long long (*)(void))proto->jitcode)();
			break;
		case 1:
			r = ((long long (*)(long long))proto->jitcode)(a[0]);
			break;
		case 2:
			r = ((long long (*)(long long, long long))proto->jitcode)(a[0], a[1]);
			break;
		case 3:
			r = ((long long (*)(long long, long long, long long))
					proto->jitcode)(a[0], a[1], a[2]);
			break;
		default:
			r = ((long long (*)(long long, long long, long long, long long))
					proto->jitcode)(a[0], a[1], a[2], a[3]);
			break;
	}

	ret = newValueObject();
	if (!ret) return NULL;
	switch ((JitType)proto->jitrettype) {
		case JT_BOOLEAN: ret->type = VT_BOOLEAN; ret->data.i = r; break;
		case JT_NIL:     ret->type = VT_NIL; break;
		default:         ret->type = VT_INTEGER; ret->data.i = r; break;
	}
	return ret;
}

void jitFree(void)
{
	while (jitregions) {
		JitRegion *next = jitregions->next;
		munmap(jitregions->mem, jitregions->size);
		free(jitregions);
		jitregions = next;
	}
}
/**@}*/

#else /* !JIT_SUPPORTED */

void jitCompileAll(Proto **protos, unsigned int num)
{
	unsigned int n;
	for (n = 0; n < num; n++) protos[n]->jitcode = NULL;
}

ValueObject *jitCall(Proto *proto, ValueObject **args, unsigned int numargs)
{
	(void)proto;
	(void)args;
	(void)numargs;
	return NULL;
}

void jitFree(void)
{
}

#endif /* JIT_SUPPORTED */
