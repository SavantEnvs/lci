/**
 * A type specialising native code generator for the virtual machine.
 *
 * Where \ref vm.c removes the cost of walking a parse tree, this removes the
 * cost of dispatching instructions.  It compiles a procedure to AArch64
 * machine code when a static analysis can prove that every value the procedure
 * handles is an integer or a boolean, which lets the generated code work on
 * raw 64 bit payloads with no tags, no boxes and no allocation.
 *
 * Anything the analysis cannot prove is simply not compiled, and the procedure
 * continues to run on the virtual machine.  The machine remains the definition
 * of what a procedure means; this only makes some of them faster.
 *
 * \file   jit.h
 *
 * \author Justin J. Meza
 */

#ifndef __JIT_H__
#define __JIT_H__

#include "vm.h"

/**
 * Whether native code generation is available on this build.
 */
#ifndef JIT_SUPPORTED
#if (defined(__aarch64__) || defined(__x86_64__)) \
		&& (defined(__APPLE__) || defined(__linux__))
#define JIT_SUPPORTED 1
#else
#define JIT_SUPPORTED 0
#endif
#endif

/**
 * The static type of a value, as far as the analysis can tell.
 */
typedef enum {
	JT_BOTTOM,  /**< Not reached. */
	JT_NIL,     /**< Always nil. */
	JT_BOOLEAN, /**< Always a boolean. */
	JT_INTEGER, /**< Always an integer. */
	JT_UNKNOWN  /**< Anything at all. */
} JitType;

/**
 * Compiles what it can of the program's procedures to native code.
 *
 * \param [in] protos The procedures to consider.
 *
 * \param [in] num The number of procedures.
 *
 * \post Procedures that could be compiled have \a jitcode set.
 */
void jitCompileAll(Proto **protos, unsigned int num);

/**
 * Calls a procedure's native code.
 *
 * \param [in] proto The procedure, which must have native code.
 *
 * \param [in] args The argument values, which must all be integers.
 *
 * \param [in] numargs The number of arguments.
 *
 * \return The procedure's return value.
 */
ValueObject *jitCall(Proto *proto, ValueObject **args, unsigned int numargs);

/**
 * Releases the memory holding generated code.
 */
void jitFree(void);

#endif /* __JIT_H__ */
