/**
 * Structures and functions for interpreting a parse tree.  The interpreter
 * traverses a parse tree in a depth-first manner, interpreting each node it
 * reaches along the way.  This is the last stage of the processing of a source
 * code file.
 *
 * \file   interpreter.h
 *
 * \author Justin J. Meza
 *
 * \date   2010-2012
 */

#ifndef __INTERPRETER_H__
#define __INTERPRETER_H__

#include <stdio.h>
#include <ctype.h>
#include <math.h>

#include "parser.h"
#include "unicode.h"

/**
 * Retrieves a value's integer data.
 */
#define getInteger(value) (value->data.i)

/**
 * Retrieves a value's decimal data.
 */
#define getFloat(value) (value->data.f)

/**
 * Retrieves a value's string data.
 */
#define getString(value) (value->data.s)

/**
 * Retrieves a value's function data.
 */
#define getFunction(value) (value->data.fn)

/**
 * Retrieves a value's array data.
 */
#define getArray(value) (value->data.a)

/**
 * Retrieves a value's blob data.
 */
#define getBlob(value) (value->data.b)

/**
 * Represents a value type.
 */
typedef enum {
	VT_INTEGER, /**< An integer value. */
	VT_FLOAT,   /**< A decimal value. */
	VT_BOOLEAN, /**< A boolean value. */
	VT_STRING,  /**< A string value. */
	VT_NIL,     /**< Represents no value. */
	VT_FUNC,    /**< A function. */
	VT_ARRAY,   /**< An array. */
	VT_BLOB     /**< A binary blob of data. */
} ValueType;

/**
 * Stores value data.
 */
typedef union {
	long long i;                 /**< Integer data. */
	float f;               /**< Decimal data. */
	char *s;               /**< String data. */
	FuncDefStmtNode *fn;   /**< Function data. */
	struct scopeobject *a; /**< Array data. */
	void *b;               /**< Binary blob data. */
} ValueData;

/**
 * Increments a value's semaphore.
 */
#define V(value) (value->semaphore++)

/**
 * Decrements a value's semaphore.
 */
#define P(value) (value->semaphore--)

/**
 * Stores a value.
 */
typedef struct {
	ValueType type;           /**< The type of value stored. */
	ValueData data;           /**< The value data. */
	unsigned short semaphore; /**< A semaphore for value usage. */
} ValueObject;

/**
 * Represents the return type.
 */
typedef enum {
	RT_DEFAULT, /**< Code block completed successfully. */
	RT_BREAK,   /**< Broke out of a loop or switch statement. */
	RT_RETURN   /**< Returned from a function. */
} ReturnType;

/**
 * Stores return state.
 */
typedef struct returnobject {
	ReturnType type;    /**< The type of return encountered. */
	ValueObject *value; /**< The optional return value. */
} ReturnObject;

/**
 * Stores a set of variables hierarchically.
 */
/**
 * The number of values a scope may hold before it builds a hash index.  Below
 * this, a linear scan of interned name pointers is faster than hashing.
 */
#define SCOPE_LINEAR_MAX 8

typedef struct scopeobject {
	struct scopeobject *parent; /**< The parent scope. */
	struct scopeobject *caller; /**< The caller scope (if in a function). */
	ValueObject *impvar;        /**< The \ref impvar "implicit variable". */
	unsigned int numvals;       /**< The number of values in the scope. */
	const Name **names;         /**< The interned names of the values. */
	ValueObject **values;       /**< The values in the scope. */
	unsigned int cap;           /**< Allocated length of \a names and \a values. */
	unsigned int idxcap;        /**< Size of \a index; zero when unused. */
	int *index;                 /**< Open-addressed name to slot index. */
} ScopeObject;

/**
 * \name Utilities
 *
 * Functions for performing helper tasks.
 */
/**@{*/
void printInterpreterError(const char *, IdentifierNode *, ScopeObject *);
void freeObjectPools(void);
void initStackGuard(void);
extern char *stackFloor;

/**
 * Whether the C stack is close enough to exhausted that recursing again is
 * unsafe.
 */
static inline int stackExhausted(void)
{
	char marker;
	return stackFloor && &marker < stackFloor;
}
ValueObject *readLineValue(void);
ValueObject *callFunctionValues(FuncDefStmtNode *, ValueObject **, unsigned int, ScopeObject *);
extern unsigned int scopeVersion;
int findScopeSlot(ScopeObject *, const Name *);
int appendScopeSlot(ScopeObject *, const Name *);
char *copyString(char *);
unsigned int isHexString(const char *);
char *resolveIdentifierName(IdentifierNode *, ScopeObject *);
const Name *resolveIdentifierIName(IdentifierNode *, ScopeObject *);
int resolveTerminalSlot(ScopeObject *, ScopeObject *, IdentifierNode *, ScopeObject **, IdentifierNode **);
/**@}*/

/**
 * \name Value object modifiers
 *
 * Functions for creating, copying, and deleting value objects.
 */
/**@{*/
ValueObject *createNilValueObject(void);
ValueObject *createBooleanValueObject(int);
ValueObject *createIntegerValueObject(long long);
ValueObject *createFloatValueObject(float);
ValueObject *createStringValueObject(char *);
ValueObject *createFunctionValueObject(FuncDefStmtNode *);
ValueObject *createArrayValueObject(ScopeObject *);
ValueObject *createBlobValueObject(void *);
void freeValueObjectSlow(ValueObject *);
ValueObject *refillValuePool(void);
extern ValueObject *valuepool;
/**@}*/

/**
 * \name Scope object modifiers
 *
 * Functions for manipulating scope objects and their data.
 */
/**@{*/
ScopeObject *createScopeObject(ScopeObject *);
ScopeObject *createScopeObjectCaller(ScopeObject *, ScopeObject *);
void deleteScopeObject(ScopeObject *);
ValueObject *createScopeValue(ScopeObject *, ScopeObject *, IdentifierNode *);
ValueObject *updateScopeValue(ScopeObject *, ScopeObject *, IdentifierNode *, ValueObject *);
ValueObject *getScopeValue(ScopeObject *, ScopeObject *, IdentifierNode *);
ValueObject *getScopeValueLocal(ScopeObject *, ScopeObject *, IdentifierNode *);
ScopeObject *getScopeObject(ScopeObject *, ScopeObject *, IdentifierNode *);
ScopeObject *getScopeObjectLocal(ScopeObject *, ScopeObject *, IdentifierNode *);
void deleteScopeValue(ScopeObject *, ScopeObject *, IdentifierNode *);
/**@}*/

/**
 * \name Return object modifiers
 *
 * Functions for creating and deleting return objects.
 */
/**@{*/
ReturnObject *createReturnObject(ReturnType, ValueObject *);
void deleteReturnObject(ReturnObject *);
/**@}*/

/**
 * \name Casts
 *
 * Functions for performing casts between different types of values.
 */
/**@{*/
ValueObject *castBooleanImplicit(ValueObject *, ScopeObject *);
ValueObject *castIntegerImplicit(ValueObject *, ScopeObject *);
ValueObject *castFloatImplicit(ValueObject *, ScopeObject *);
ValueObject *castStringImplicit(ValueObject *, ScopeObject *);
ValueObject *castBooleanExplicit(ValueObject *, ScopeObject *);
ValueObject *castIntegerExplicit(ValueObject *, ScopeObject *);
ValueObject *castFloatExplicit(ValueObject *, ScopeObject *);
ValueObject *castStringExplicit(ValueObject *, ScopeObject *);
ValueObject *applyArithOp(OpType, ValueObject *, ValueObject *, ScopeObject *);
ValueObject *applyEqualityOp(OpType, ValueObject *, ValueObject *, ScopeObject *);
int valueIsTrue(ValueObject *, ScopeObject *, int *);
ValueObject *concatValues(ValueObject **, unsigned int, ScopeObject *);
int switchMatches(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Node interpreters
 *
 * Functions for interpreting basic parse tree nodes.
 */
/**@{*/
ValueObject *interpretExprNode(ExprNode *, ScopeObject *);
ReturnObject *interpretStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretStmtNodeList(StmtNodeList *, ScopeObject *);
ReturnObject *interpretBlockNode(BlockNode *, ScopeObject *);
int interpretMainNodeScope(MainNode *, ScopeObject *);
/**@}*/

/**
 * \name Expression interpreters
 *
 * Functions for interpreting expression parse tree nodes.
 */
/**@{*/
ValueObject *interpretImpVarExprNode(ExprNode *, ScopeObject *);
ValueObject *interpretCastExprNode(ExprNode *, ScopeObject *);
ValueObject *interpretFuncCallExprNode(ExprNode *, ScopeObject *);
ValueObject *interpretIdentifierExprNode(ExprNode *, ScopeObject *);
ValueObject *interpretConstantExprNode(ExprNode *, ScopeObject *);
ValueObject *interpretSystemCommandExprNode(ExprNode *, ScopeObject *);
/**@}*/

/**
 * \name Operation interpreters
 *
 * Functions for interpreting operation parse tree nodes.
 */
/**@{*/
ValueObject *interpretNotOpExprNode(OpExprNode *, ScopeObject *);
ValueObject *interpretArithOpExprNode(OpExprNode *, ScopeObject *);
ValueObject *interpretBoolOpExprNode(OpExprNode *, ScopeObject *);
ValueObject *interpretEqualityOpExprNode(OpExprNode *, ScopeObject *);
ValueObject *interpretConcatOpExprNode(OpExprNode *, ScopeObject *);
ValueObject *interpretOpExprNode(ExprNode *, ScopeObject *);
/**@}*/

/**
 * \name Statement interpreters
 *
 * Functions for interpreting statement parse tree nodes.
 */
/**@{*/
ReturnObject *interpretCastStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretPrintStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretInputStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretAssignmentStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretDeclarationStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretIfThenElseStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretSwitchStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretBreakStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretReturnStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretLoopStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretDeallocationStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretFuncDefStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretExprStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretAltArrayDefStmtNode(StmtNode *, ScopeObject *);
ReturnObject *interpretBindingStmtNode(StmtNode *, ScopeObject *);
/* Forward declaration of binding.h function (to break circular dependence) */
void loadLibrary(ScopeObject *, IdentifierNode *);
ReturnObject *interpretImportStmtNode(StmtNode *, ScopeObject *);
/**@}*/

/**
 * \name Arithmetic operations (integer-integer)
 *
 * Functions for performing integer-integer operations on values.
 */
/**@{*/
ValueObject *opAddIntegerInteger(ValueObject *, ValueObject *);
ValueObject *opSubIntegerInteger(ValueObject *, ValueObject *);
ValueObject *opMultIntegerInteger(ValueObject *, ValueObject *);
ValueObject *opDivIntegerInteger(ValueObject *, ValueObject *);
ValueObject *opMaxIntegerInteger(ValueObject *, ValueObject *);
ValueObject *opMinIntegerInteger(ValueObject *, ValueObject *);
ValueObject *opModIntegerInteger(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Arithmetic operations (integer-float)
 *
 * Functions for performing integer-float operations on values.
 */
/**@{*/
ValueObject *opAddIntegerFloat(ValueObject *, ValueObject *);
ValueObject *opSubIntegerFloat(ValueObject *, ValueObject *);
ValueObject *opMultIntegerFloat(ValueObject *, ValueObject *);
ValueObject *opDivIntegerFloat(ValueObject *, ValueObject *);
ValueObject *opMaxIntegerFloat(ValueObject *, ValueObject *);
ValueObject *opMinIntegerFloat(ValueObject *, ValueObject *);
ValueObject *opModIntegerFloat(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Arithmetic operations (float-integer)
 *
 * Functions for performing float-integer operations on values.
 */
/**@{*/
ValueObject *opAddFloatInteger(ValueObject *, ValueObject *);
ValueObject *opSubFloatInteger(ValueObject *, ValueObject *);
ValueObject *opMultFloatInteger(ValueObject *, ValueObject *);
ValueObject *opDivFloatInteger(ValueObject *, ValueObject *);
ValueObject *opMaxFloatInteger(ValueObject *, ValueObject *);
ValueObject *opMinFloatInteger(ValueObject *, ValueObject *);
ValueObject *opModFloatInteger(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Arithmetic operations (float-float)
 *
 * Functions for performing float-float operations on values.
 */
/**@{*/
ValueObject *opAddFloatFloat(ValueObject *, ValueObject *);
ValueObject *opSubFloatFloat(ValueObject *, ValueObject *);
ValueObject *opMultFloatFloat(ValueObject *, ValueObject *);
ValueObject *opDivFloatFloat(ValueObject *, ValueObject *);
ValueObject *opMaxFloatFloat(ValueObject *, ValueObject *);
ValueObject *opMinFloatFloat(ValueObject *, ValueObject *);
ValueObject *opModFloatFloat(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Equality operations (boolean-boolean)
 *
 * Functions for performing boolean-boolean operations on values.
 */
/**@{*/
ValueObject *opEqBooleanBoolean(ValueObject *, ValueObject *);
ValueObject *opNeqBooleanBoolean(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Equality operations (integer-integer)
 *
 * Functions for performing integer-integer operations on values.
 */
/**@{*/
ValueObject *opEqIntegerInteger(ValueObject *, ValueObject *);
ValueObject *opNeqIntegerInteger(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Equality operations (integer-float)
 *
 * Functions for performing integer-float operations on values.
 */
/**@{*/
ValueObject *opEqIntegerFloat(ValueObject *, ValueObject *);
ValueObject *opNeqIntegerFloat(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Equality operations (float-integer)
 *
 * Functions for performing float-integer operations on values.
 */
/**@{*/
ValueObject *opEqFloatInteger(ValueObject *, ValueObject *);
ValueObject *opNeqFloatInteger(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Equality operations (float-float)
 *
 * Functions for performing float-float operations on values.
 */
/**@{*/
ValueObject *opEqFloatFloat(ValueObject *, ValueObject *);
ValueObject *opNeqFloatFloat(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Equality operations (string-string)
 *
 * Functions for performing string-string operations on values.
 */
/**@{*/
ValueObject *opEqStringString(ValueObject *, ValueObject *);
ValueObject *opNeqStringString(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Equality operations (nil-nil)
 *
 * Functions for performing nil-nil operations on values.
 */
/**@{*/
ValueObject *opEqNilNil(ValueObject *, ValueObject *);
ValueObject *opNeqNilNil(ValueObject *, ValueObject *);
/**@}*/

/**
 * \name Value fast paths
 *
 * These run several times for every instruction the machine executes, so they
 * are defined here rather than being called across a translation unit.
 */
/**@{*/

/**
 * Takes another reference to a value.
 */
static inline ValueObject *copyValueObjectInline(ValueObject *value)
{
	value->semaphore++;
	return value;
}

/**
 * Drops a reference to a value, freeing it when the last one goes.
 */
static inline void deleteValueObjectInline(ValueObject *value)
{
	if (!value) return;
	if (--value->semaphore == 0) freeValueObjectSlow(value);
}

/**
 * Allocates an uninitialised value with a single reference.
 */
static inline ValueObject *newValueObject(void)
{
	ValueObject *p = valuepool;
	if (!p) {
		p = refillValuePool();
		if (!p) return NULL;
	}
	else valuepool = *(ValueObject **)p;
	p->semaphore = 1;
	return p;
}

/**
 * Reduces a value to the truth of its contents, without a call for the types
 * that need no conversion.
 */
static inline int valueIsTrueInline(ValueObject *val, ScopeObject *scope, int *ok)
{
	if (val->type == VT_BOOLEAN || val->type == VT_INTEGER) {
		*ok = 1;
		return val->data.i != 0;
	}
	return valueIsTrue(val, scope, ok);
}
/**@}*/

#define copyValueObject(v) copyValueObjectInline(v)
#define deleteValueObject(v) deleteValueObjectInline(v)

#endif /* __INTERPRETER_H__ */
