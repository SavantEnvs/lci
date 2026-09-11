/**
 * A register based virtual machine for LOLCODE.
 *
 * The tree walking interpreter in \ref interpreter.c resolves names, allocates
 * scopes and dispatches on node types every time it evaluates anything.  This
 * machine does that work once, ahead of time: \ref compileProgram lowers the
 * parse tree into procedures of fixed-width instructions over a flat register
 * file, and \ref callProto executes them.
 *
 * Compilation is best effort.  Anything the compiler does not understand
 * leaves the enclosing procedure uncompiled, and it continues to run through
 * the tree walking interpreter, so the two share one set of semantics and one
 * representation of values and scopes.
 *
 * \file   vm.h
 *
 * \author Justin J. Meza
 */

#ifndef __VM_H__
#define __VM_H__

#include "interpreter.h"
#include "parser.h"

/**
 * An instruction opcode.  Operands are named after the fields of \ref Instr
 * they occupy: \c A is always the destination register.
 */
typedef enum {
	/* Loads */
	OPC_LOADK,    /**< R[a] = K[b] */
	OPC_LOADI,    /**< R[a] = integer b */
	OPC_LOADB,    /**< R[a] = boolean b */
	OPC_LOADNIL,  /**< R[a] = nil */
	OPC_MOVE,     /**< R[a] = R[b] */

	/* Variables that live in a scope object */
	OPC_GETVAR,   /**< R[a] = the variable named K[b] */
	OPC_SETVAR,   /**< The variable named K[b] = R[a] */
	OPC_DECLVAR,  /**< Declare K[b] in the local scope, set it to R[a] */
	OPC_DECLARR,  /**< Declare K[b] in the local scope as a fresh bukkit */
	OPC_UNDECL,   /**< Remove K[b] from scope */
	OPC_GETSLOT,  /**< R[a] = the value the identifier at K[b] names */
	OPC_SETSLOT,  /**< The value the identifier at K[b] names = R[a] */
	OPC_STMTSLOW, /**< Run the statement at K[b] through the interpreter */

	/* Arithmetic, R[a] = R[b] op R[c] */
	OPC_ADD,
	OPC_SUB,
	OPC_MUL,
	OPC_DIV,
	OPC_MOD,
	OPC_MAX,
	OPC_MIN,

	/* Arithmetic against an integer literal, R[a] = R[b] op c */
	OPC_ADDI,
	OPC_SUBI,

	/* Logic, R[a] = R[b] op R[c] */
	OPC_AND,
	OPC_OR,
	OPC_XOR,
	OPC_NOT,      /**< R[a] = not R[b] */
	OPC_CONCAT,   /**< R[a] = the c registers at R[b] joined as a string */

	/* Comparison, R[a] = R[b] op R[c] */
	OPC_EQ,
	OPC_NEQ,
	OPC_EQI,      /**< R[a] = R[b] == integer c */
	OPC_SWEQ,     /**< R[a] = R[b] matches switch guard R[c] */

	/* Conversion */
	OPC_CAST,     /**< R[a] = R[b] cast to type c */

	/* Control flow */
	OPC_JMP,      /**< pc += b */
	OPC_JMPF,     /**< If R[a] is false, pc += b */
	OPC_JMPT,     /**< If R[a] is true, pc += b */
	OPC_TESTJMPF, /**< If R[b] is false, pc += c, else R[a] = R[b] */

	/* Procedures */
	OPC_CALL,     /**< R[a] = K[b] called with c arguments at R[a+1].. */
	OPC_RET,      /**< Return R[a] */
	OPC_RETNIL,   /**< Return nil */
	OPC_BREAK,    /**< Leave the enclosing loop or procedure */
	OPC_ENDPROC,  /**< Fall off the end of a procedure. */

	/* Statements with side effects */
	OPC_PRINT,    /**< Print the b registers at R[a], as directed by K[c] */
	OPC_INPUT,    /**< Read a line into the variable named K[b] */

	/* Scope management for nested blocks */
	OPC_PUSHSC,   /**< Enter a nested scope */
	OPC_POPSC,    /**< Leave a nested scope */

	OPC_NUM       /**< The number of opcodes. */
} Opcode;

/**
 * A single instruction.  Keeping this a fixed size struct rather than a packed
 * word keeps decoding to plain field loads.
 */
typedef struct {
	unsigned char op; /**< The \ref Opcode to run. */
	unsigned char a;  /**< The destination register. */
	short c;          /**< A register, count or small literal. */
	int b;            /**< A register, constant index or jump offset. */
} Instr;

/**
 * A compiled procedure: either a function body or the main block.
 */
typedef struct proto {
	Instr *code;           /**< The instruction stream. */
	unsigned int numcode;  /**< The number of instructions. */
	ValueObject **k;       /**< Constants, owned by the procedure. */
	unsigned int numk;     /**< The number of constants. */
	const Name **knames;   /**< Interned names, indexed alongside \a k. */
	void **kptrs;          /**< Parse tree pointers referenced by instructions. */
	ScopeObject **icscope; /**< Inline cache: the scope a name was found in. */
	ScopeObject **icfrom;  /**< Inline cache: the scope the search started from. */
	int *icslot;           /**< Inline cache: the slot the name was found in. */
	unsigned int *icver;   /**< Inline cache: the scope version when cached. */
	const Name **regnames; /**< The name each register holds, or NULL. */
	unsigned int numregs;  /**< The number of registers a frame needs. */
	unsigned int numargs;  /**< The number of parameters. */
	int itreg;             /**< The register holding the procedure's IT. */
	int needscope;         /**< Whether a frame must own a scope object. */
	void *jitcode;         /**< Native code for this procedure, or NULL. */
	int jitrettype;        /**< The \ref JitType the native code returns. */
	FuncDefStmtNode *def;  /**< The function this was compiled from. */
} Proto;

/**
 * Compiles what it can of a parse tree.
 *
 * \param [in,out] node The program to compile.
 *
 * \post Every function the compiler understood has a \ref Proto attached.
 */
void compileProgram(MainNode *node);

/**
 * Runs a compiled procedure.
 *
 * \param [in] proto The procedure to run.
 *
 * \param [in] args The argument values, which the frame takes ownership of.
 *
 * \param [in] numargs The number of arguments supplied.
 *
 * \param [in] scope The scope the procedure runs relative to.
 *
 * \param [in] caller The scope \c ME refers to, usually \a scope.
 *
 * \return The procedure's return value.
 *
 * \retval NULL Execution failed and an error has been reported.
 */
ValueObject *callProto(Proto *proto, ValueObject **args, unsigned int numargs, ScopeObject *scope, ScopeObject *caller);

/**
 * Releases every procedure compiled for a program.
 */
void freeProtos(void);

/**
 * The compiled main block, if the compiler managed to lower it.
 */
Proto *getMainProto(void);

/**
 * Prints a procedure's instructions, for debugging the compiler.
 */
void dumpProto(const Proto *, const char *);

/**
 * Finds the procedure a call to a name always reaches, if there is one.
 */
Proto *resolveStaticCallee(const Name *);

/**
 * Finds a register held variable by name in the running frame.
 */
ValueObject *lookupRegisterVar(const Name *);

#endif /* __VM_H__ */
