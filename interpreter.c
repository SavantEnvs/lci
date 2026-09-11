#include <sys/time.h>
#include <sys/resource.h>

#include "interpreter.h"
#include "vm.h"

/**
 * Creates a new string by copying the contents of another string.
 *
 * \param [in] data The string to copy.
 *
 * \return A new string whose contents is a copy of \a data.
 *
 * \retval NULL Memory allocation failed.
 */
char *copyString(char *data)
{
	char *p = malloc(sizeof(char) * (strlen(data) + 1));
	if (!p) {
		perror("malloc");
		return NULL;
	}
	strcpy(p, data);
	return p;
}

/**
 * Checks if a string follows the format of a hexadecimal number.
 *
 * \param [in] data The characters to check the format of.
 *
 * \retval 0 The string is not a hexadecimal number.
 *
 * \retval 1 The string is a hexadecimal number.
 */
unsigned int isHexString(const char *data)
{
	size_t n;
	size_t len = strlen(data);

	/* Check for empty string */
	if (len == 0) return 0;

	/* Check for non-digit and non-A-through-F characters */
	for (n = 0; n < len; n++) {
		if (!isdigit(data[n])
				&& data[n] != 'A'
				&& data[n] != 'B'
				&& data[n] != 'C'
				&& data[n] != 'D'
				&& data[n] != 'E'
				&& data[n] != 'F'
				&& data[n] != 'a'
				&& data[n] != 'b'
				&& data[n] != 'c'
				&& data[n] != 'd'
				&& data[n] != 'e'
				&& data[n] != 'f')
			return 0;
	}

	return 1;
}

/**
 * Evaluates an identifier to produce its name as a string.
 *
 * \param [in] id The identifier to evaluate.
 *
 * \param [in] scope The scope to evaluate \a id under.
 *
 * \return A new string containing the evaluated name of the identifier.
 *
 * \retval NULL Memory allocation failed.
 */
char *resolveIdentifierName(IdentifierNode *id,
                            ScopeObject *scope)
{
	ValueObject *val = NULL;
	ValueObject *str = NULL;
	char *ret = NULL;

	if (!id) goto resolveIdentifierNameAbort;

	if (id->type == IT_DIRECT) {
		/* Just return a copy of the character array */
		const char *temp = (char *)(id->id);
		ret = malloc(sizeof(char) * (strlen(temp) + 1));
		strcpy(ret, temp);
	} else if (id->type == IT_INDIRECT) {
		ExprNode *expr = (ExprNode *)(id->id);

		/* Interpret the identifier expression */
		val = interpretExprNode(expr, scope);
		if (!val) goto resolveIdentifierNameAbort;

		/* Then cast it to a string */
		str = castStringExplicit(val, scope);
		if (!str) goto resolveIdentifierNameAbort;
		deleteValueObject(val);

		/* Copy the evaluated string */
		ret = copyString(getString(str));
		if (!ret) goto resolveIdentifierNameAbort;
		deleteValueObject(str);
	} else {
		char *name = resolveIdentifierName(id, scope);
		error(IN_INVALID_IDENTIFIER_TYPE, id->fname, id->line, name);
		free(name);
	}

	return ret;

resolveIdentifierNameAbort: /* Exception handline */

	/* Clean up any allocated structures */
	if (ret) free(ret);
	if (str) deleteValueObject(str);
	if (val) deleteValueObject(val);

	return NULL;
}

/**
 * \name Stack guard
 *
 * Recursion in a LOLCODE program becomes recursion in the interpreter, so a
 * runaway program used to take the process down with a segmentation fault.
 * Recording how much stack there is at start up lets every path -- the tree
 * walking interpreter, the virtual machine and generated code -- notice it is
 * running out and report the problem instead.
 */
/**@{*/

char *stackFloor = NULL;

/**
 * The margin left below the floor, enough to report an error and unwind.
 */
#define STACK_MARGIN (256 * 1024)

void initStackGuard(void)
{
	char marker;
	size_t size = 8 * 1024 * 1024;
#ifdef RLIMIT_STACK
	struct rlimit rl;
	if (getrlimit(RLIMIT_STACK, &rl) == 0
			&& rl.rlim_cur != RLIM_INFINITY
			&& rl.rlim_cur > STACK_MARGIN * 2)
		size = (size_t)rl.rlim_cur;
#endif
	if (size <= STACK_MARGIN * 2) size = STACK_MARGIN * 4;
	stackFloor = &marker - (size - STACK_MARGIN);
}
/**@}*/

/**
 * \name Object pools
 *
 * Values and return records are created and destroyed several times per
 * expression evaluated and used to be individually malloc'd, which dominated
 * run time.  They are uniformly sized, so they are carved out of block
 * allocations and recycled through free lists instead.
 */
/**@{*/

/**
 * The number of objects carved out of the system allocator at a time.
 */
#define POOL_BLOCK 1024

/**
 * Tracks a block allocation so that it can be released at exit.
 */
typedef struct poolblock {
	struct poolblock *next; /**< The previously allocated block. */
	void *mem;              /**< The block itself. */
} PoolBlock;

static PoolBlock *poolblocks = NULL;
unsigned int scopeVersion = 1;
ValueObject *valuepool = NULL;
static ReturnObject *returnpool = NULL;

/**
 * Records \a mem so that \ref freeObjectPools can release it.
 */
static void trackPoolBlock(void *mem)
{
	PoolBlock *b = malloc(sizeof(PoolBlock));
	if (!b) {
		perror("malloc");
		return;
	}
	b->mem = mem;
	b->next = poolblocks;
	poolblocks = b;
}

/**
 * Takes a value off of the value free list, refilling it if it is empty.
 */
static ValueObject *allocValueObject(void)
{
	ValueObject *p = valuepool;
	ValueObject *block;
	unsigned int n;

	if (p) {
		valuepool = *(ValueObject **)p;
		return p;
	}

	block = malloc(sizeof(ValueObject) * POOL_BLOCK);
	if (!block) {
		perror("malloc");
		return NULL;
	}
	trackPoolBlock(block);
	/* Hand back the first object and thread the rest onto the free list. */
	for (n = 1; n < POOL_BLOCK - 1; n++)
		*(ValueObject **)(block + n) = block + n + 1;
	*(ValueObject **)(block + POOL_BLOCK - 1) = NULL;
	valuepool = block + 1;
	return block;
}

/**
 * Allocates an uninitialised value with a single reference.
 *
 * \return A value whose type and data the caller must set.
 */
ValueObject *refillValuePool(void)
{
	return allocValueObject();
}

/**
 * Returns a value to the value free list.
 */
static void recycleValueObject(ValueObject *p)
{
	*(ValueObject **)p = valuepool;
	valuepool = p;
}

/**
 * Takes a return record off of the return free list, refilling it if it is
 * empty.
 */
static ReturnObject *allocReturnObject(void)
{
	ReturnObject *p = returnpool;
	ReturnObject *block;
	unsigned int n;

	if (p) {
		returnpool = *(ReturnObject **)p;
		return p;
	}

	block = malloc(sizeof(ReturnObject) * POOL_BLOCK);
	if (!block) {
		perror("malloc");
		return NULL;
	}
	trackPoolBlock(block);
	for (n = 1; n < POOL_BLOCK - 1; n++)
		*(ReturnObject **)(block + n) = block + n + 1;
	*(ReturnObject **)(block + POOL_BLOCK - 1) = NULL;
	returnpool = block + 1;
	return block;
}

/**
 * Returns a return record to the return free list.
 */
static void recycleReturnObject(ReturnObject *p)
{
	*(ReturnObject **)p = returnpool;
	returnpool = p;
}

/**
 * Releases every block the pools have taken from the system allocator.
 *
 * \post No value or return object remains valid.
 */
void freeObjectPools(void)
{
	while (poolblocks) {
		PoolBlock *next = poolblocks->next;
		free(poolblocks->mem);
		free(poolblocks);
		poolblocks = next;
	}
	valuepool = NULL;
	returnpool = NULL;
}
/**@}*/

/**
 * \name Scope name index
 *
 * A scope holds its values in insertion order in dense parallel arrays.  Small
 * scopes -- nearly all of them -- are searched by scanning those arrays and
 * comparing interned name pointers, which is quicker than hashing.  A scope
 * that grows past \ref SCOPE_LINEAR_MAX also maintains an open-addressed index
 * from name to slot so that lookups in large scopes stay constant time.
 */
/**@{*/

/**
 * Rebuilds \a scope's name index, sizing it for the current contents.
 */
static void reindexScope(ScopeObject *scope)
{
	unsigned int cap = 16;
	unsigned int n;

	while (cap < scope->numvals * 2) cap *= 2;
	free(scope->index);
	scope->index = malloc(sizeof(int) * cap);
	if (!scope->index) {
		perror("malloc");
		/* Fall back to scanning; correctness does not depend on the index. */
		scope->idxcap = 0;
		return;
	}
	memset(scope->index, -1, sizeof(int) * cap);
	scope->idxcap = cap;
	for (n = 0; n < scope->numvals; n++) {
		unsigned int i = scope->names[n]->hash & (cap - 1);
		while (scope->index[i] != -1) i = (i + 1) & (cap - 1);
		scope->index[i] = (int)n;
	}
}

/**
 * Finds the slot holding \a name in \a scope alone, ignoring its parents.
 *
 * \retval -1 \a name is not present in \a scope.
 */
int findScopeSlot(ScopeObject *scope, const Name *name)
{
	unsigned int n;

	if (scope->idxcap) {
		unsigned int mask = scope->idxcap - 1;
		unsigned int i = name->hash & mask;
		int slot;
		while ((slot = scope->index[i]) != -1) {
			if (scope->names[slot] == name) return slot;
			i = (i + 1) & mask;
		}
		return -1;
	}

	for (n = 0; n < scope->numvals; n++)
		if (scope->names[n] == name) return (int)n;
	return -1;
}

/**
 * Appends \a name to \a scope, bound to a new nil value.
 *
 * \return The slot the name was placed in.
 *
 * \retval -1 Memory allocation failed.
 */
int appendScopeSlot(ScopeObject *scope, const Name *name)
{
	if (scope->numvals == scope->cap) {
		unsigned int newcap = scope->cap ? scope->cap * 2 : 4;
		void *mem = realloc(scope->names, sizeof(Name *) * newcap);
		if (!mem) {
			perror("realloc");
			return -1;
		}
		scope->names = mem;
		mem = realloc(scope->values, sizeof(ValueObject *) * newcap);
		if (!mem) {
			perror("realloc");
			return -1;
		}
		scope->values = mem;
		scope->cap = newcap;
	}

	scopeVersion++;
	scope->names[scope->numvals] = name;
	scope->values[scope->numvals] = createNilValueObject();
	if (!scope->values[scope->numvals]) return -1;
	scope->numvals++;

	if (scope->idxcap) {
		if (scope->numvals * 2 > scope->idxcap) {
			reindexScope(scope);
		}
		else {
			unsigned int mask = scope->idxcap - 1;
			unsigned int i = name->hash & mask;
			while (scope->index[i] != -1) i = (i + 1) & mask;
			scope->index[i] = (int)(scope->numvals - 1);
		}
	}
	else if (scope->numvals > SCOPE_LINEAR_MAX) {
		reindexScope(scope);
	}

	return (int)(scope->numvals - 1);
}
/**@}*/

/**
 * The interned name of the calling object reference variable, resolved once.
 */
static const Name *nameME = NULL;

/**
 * Returns the interned name \c ME.
 */
static const Name *getNameME(void)
{
	if (!nameME) nameME = internName("ME");
	return nameME;
}

/**
 * Resolves an identifier to an interned name.
 *
 * Unlike \ref resolveIdentifierName this allocates nothing for the common
 * case of a direct identifier, which was interned when it was parsed.
 *
 * \param [in] id The identifier to resolve.
 *
 * \param [in] scope The scope to evaluate an indirect identifier under.
 *
 * \return The interned name \a id refers to.
 *
 * \retval NULL \a id could not be resolved.
 */
const Name *resolveIdentifierIName(IdentifierNode *id,
                                   ScopeObject *scope)
{
	ValueObject *val = NULL;
	ValueObject *str = NULL;
	const Name *ret = NULL;

	if (!id) return NULL;

	if (id->type == IT_DIRECT) return id->iname;

	if (id->type == IT_INDIRECT) {
		ExprNode *expr = (ExprNode *)(id->id);

		/* Interpret the identifier expression */
		val = interpretExprNode(expr, scope);
		if (!val) return NULL;

		/* Then cast it to a string */
		str = castStringExplicit(val, scope);
		deleteValueObject(val);
		if (!str) return NULL;

		ret = internName(getString(str));
		deleteValueObject(str);
		return ret;
	}

	{
		char *name = resolveIdentifierName(id, scope);
		error(IN_INVALID_IDENTIFIER_TYPE, id->fname, id->line, name);
		if (name) free(name);
	}
	return NULL;
}

/**
 * Creates a nil-type value.
 *
 * \return A new nil-type value.
 *
 * \retval NULL Memory allocation failed.
 */
ValueObject *createNilValueObject(void)
{
	ValueObject *p = allocValueObject();
	if (!p) {
		perror("malloc");
		return NULL;
	}
	p->type = VT_NIL;
	p->semaphore = 1;
	return p;
}

/**
 * Creates a boolean-type value.
 *
 * \param [in] data The boolean data to store.
 *
 * \return A boolean-type value equalling 0 if \a data equals 0 and 1 otherwise.
 *
 * \retval NULL Memory allocation failed.
 */
ValueObject *createBooleanValueObject(int data)
{
	ValueObject *p = allocValueObject();
	if (!p) {
		perror("malloc");
		return NULL;
	}
	p->type = VT_BOOLEAN;
	p->data.i = (data != 0);
	p->semaphore = 1;
	return p;
}

/**
 * Creates a integer-type value.
 *
 * \param [in] data The integer data to store.
 *
 * \return An integer-type value equalling \a data.
 *
 * \retval NULL Memory allocation failed.
 */
ValueObject *createIntegerValueObject(long long data)
{
	ValueObject *p = allocValueObject();
	if (!p) {
		perror("malloc");
		return NULL;
	}
	p->type = VT_INTEGER;
	p->data.i = data;
	p->semaphore = 1;
	return p;
}

/**
 * Creates a floating-point-type value.
 *
 * \param [in] data The floating-point data to store.
 *
 * \return A floating-point-type value equalling \a data.
 *
 * \retval NULL Memory allocation failed.
 */
ValueObject *createFloatValueObject(float data)
{
	ValueObject *p = allocValueObject();
	if (!p) {
		perror("malloc");
		return NULL;
	}
	p->type = VT_FLOAT;
	p->data.f = data;
	p->semaphore = 1;
	return p;
}

/**
 * Creates a string-type value.
 *
 * \param [in] data The string data to store.
 *
 * \note \a data is stored as-is; no copy of it is made.
 *
 * \return A string-type value equalling \a data.
 *
 * \retval NULL Memory allocation failed.
 */
ValueObject *createStringValueObject(char *data)
{
	ValueObject *p = allocValueObject();
	if (!p) {
		perror("malloc");
		return NULL;
	}
	p->type = VT_STRING;
	p->data.s = data;
	p->semaphore = 1;
	return p;
}

/**
 * Creates a function-type value.
 *
 * \param [in] def The function definition to store.
 *
 * \return A function-type value containing \a data.
 *
 * \retval NULL Memory allocation failed.
 */
ValueObject *createFunctionValueObject(FuncDefStmtNode *def)
{
	ValueObject *p = allocValueObject();
	if (!p) {
		perror("malloc");
		return NULL;
	}
	p->type = VT_FUNC;
	p->data.fn = def;
	p->semaphore = 1;
	return p;
}

/**
 * Creates an array-type value.
 *
 * \param [in] parent The optional parent scope to use.
 *
 * \note \a parent may be NULL, in which case this array is treated as the root.
 *
 * \return An empty array-type value with parent \a parent.
 *
 * \retval NULL Memory allocation failed.
 */
ValueObject *createArrayValueObject(ScopeObject *parent)
{
	ValueObject *p = allocValueObject();
	if (!p) {
		perror("malloc");
		return NULL;
	}
	p->type = VT_ARRAY;
	p->data.a = createScopeObject(parent);
	if (!p->data.a) {
		free(p);
		return NULL;
	}
	p->semaphore = 1;
	return p;
}

/**
 * Copies a value.
 *
 * Instead of actually performing a copy of memory, this function increments a
 * semaphore in \a value and returns \a value again.  The semaphore gets
 * decremented when \a value gets deleted.  This way, an immutable copy of a
 * value may be made without actually copying its blocks of memory; this reduces
 * the overhead associated with copying a value--a fairly common
 * operation--while still preserving its usability.
 *
 * \param [in,out] value The value to copy.
 *
 * \return A value with the same type and contents as \a value.
 *
 * \retval NULL The type of \a value is unrecognized.
 */

/**
 * Deletes a value.
 *
 * This function decrements a semaphore in \a value and deletes \a value if the
 * semaphore reaches 0 (no copies of this value are need anymore).  The
 * semaphore gets incremented when either the value is created or it gets
 * copied.  This way, an immutable copy of the value may be made without
 * actually copying its memory.
 *
 * \param [in,out] value The value to delete.
 *
 * \post The memory at \a value and any of its members will be freed (although
 * see note for full details).
 */
void freeValueObjectSlow(ValueObject *value)
{
	{
		if (value->type == VT_STRING)
			free(value->data.s);
		/* FuncDefStmtNode structures get freed with the parse tree */
		else if (value->type == VT_ARRAY)
			deleteScopeObject(value->data.a);
		recycleValueObject(value);
	}
}

/**
 * Creates a scope.
 *
 * Scopes are used to map identifiers to values.  Scopes are organized
 * hierarchically.
 *
 * \param [in] parent The optional parent scope to use.
 *
 * \return An empty scope with parent \a parent.
 *
 * \retval NULL Memory allocation failed.
 */
ScopeObject *createScopeObject(ScopeObject *parent)
{
	ScopeObject *p = malloc(sizeof(ScopeObject));
	if (!p) {
		perror("malloc");
		return NULL;
	}
	p->impvar = createNilValueObject();
	if (!p->impvar) {
		free(p);
		return NULL;
	}
	p->numvals = 0;
	p->names = NULL;
	p->values = NULL;
	p->cap = 0;
	p->idxcap = 0;
	p->index = NULL;
	p->parent = parent;
	if (parent) p->caller = parent->caller;
	else p->caller = NULL;
	return p;
}

/**
 * Creates a scope with a specific caller.
 *
 * \param [in] parent The optional parent scope to use.
 *
 * \param [in] caller The caller scope to use.
 *
 * \return An empty scope with parent \a parent and caller \a caller.
 *
 * \retval NULL Memory allocation failed.
 */
ScopeObject *createScopeObjectCaller(ScopeObject *parent,
                                     ScopeObject *caller)
{
	ScopeObject *p = createScopeObject(parent);
	if (!p) return NULL;
	if (caller) p->caller = caller;
	return p;
}

/**
 * Deletes a scope.
 *
 * \param [in,out] scope The scope to delete.
 *
 * \post The memory at \a scope and any of its members will be freed.
 */
void deleteScopeObject(ScopeObject *scope)
{
	unsigned int n;
	if (!scope) return;
	/* Names are interned and outlive every scope that mentions them. */
	for (n = 0; n < scope->numvals; n++)
		deleteValueObject(scope->values[n]);
	free(scope->names);
	free(scope->values);
	free(scope->index);
	deleteValueObject(scope->impvar);
	free(scope);
}

/**
 * Creates a new, nil-type value in a scope.
 *
 * \param [in] src The scope to evaluate \a target under.
 *
 * \param [in,out] dest The scope to create the new value in.
 *
 * \param [in] target The name of the value to create.
 *
 * \return The newly-created value.
 *
 * \retval NULL Memory allocation failed.
 */
ValueObject *createScopeValue(ScopeObject *src,
                              ScopeObject *dest,
                              IdentifierNode *target)
{
	ScopeObject *parent = dest;
	IdentifierNode *child = target;
	const Name *name = NULL;
	int slot;

	/* Traverse the target to the terminal child and parent */
	if (!resolveTerminalSlot(src, dest, target, &parent, &child))
		return NULL;

	/* Look up the identifier name */
	name = resolveIdentifierIName(target, src);
	if (!name) return NULL;

	/* Add value to local scope */
	slot = appendScopeSlot(dest, name);
	if (slot < 0) return NULL;

	return dest->values[slot];
}

/**
 * Updates a value in a scope.
 *
 * \param [in] src The scope to evaluate \a target under.
 *
 * \param [in,out] dest The scope to update the value in.
 *
 * \param [in] target The name of the value to create.
 *
 * \param [in] value The new value to assign.
 *
 * \return The updated value (will be the same as \a val).
 *
 * \retval NULL Either \a target could not be evaluated in \a src or \a target
 * could not be found in \a dest.
 */
ValueObject *updateScopeValue(ScopeObject *src,
                              ScopeObject *dest,
                              IdentifierNode *target,
                              ValueObject *value)
{
	ScopeObject *parent = dest;
	IdentifierNode *child = target;
	const Name *name = NULL;

	/* Traverse the target to the terminal child and parent */
	if (!resolveTerminalSlot(src, dest, target, &parent, &child))
		return NULL;

	/* Look up the identifier name */
	name = resolveIdentifierIName(child, src);
	if (!name) return NULL;

	/* Traverse upwards through scopes */
	do {
		int slot = findScopeSlot(parent, name);
		if (slot >= 0) {
			/* Wipe out the old value */
			deleteValueObject(parent->values[slot]);
			/* Assign the new value */
			if (value) parent->values[slot] = value;
			else parent->values[slot] = createNilValueObject();
			return parent->values[slot];
		}
	} while ((parent = parent->parent));

	{
		char *n = resolveIdentifierName(target, src);
		error(IN_UNABLE_TO_STORE_VARIABLE, target->fname, target->line, n);
		if (n) free(n);
	}

	return NULL;
}

/**
 * Gets a stored value in a scope.
 *
 * \param [in] src The scope to evaluate \a target under.
 *
 * \param [in,out] dest The scope to update the value in.
 *
 * \param [in] target The name of the value to get.
 *
 * \return The value in \a dest, named by evaluating \a target under \a src.
 *
 * \retval NULL Either \a target could not be evaluated in \a src or \a target
 * could not be found in \a dest.
 */
ValueObject *getScopeValue(ScopeObject *src,
                           ScopeObject *dest,
                           IdentifierNode *target)
{
	ScopeObject *parent = dest;
	IdentifierNode *child = target;
	const Name *name = NULL;

	/* Traverse the target to the terminal child and parent */
	if (!resolveTerminalSlot(src, dest, target, &parent, &child))
		return NULL;

	/* Look up the identifier name */
	name = resolveIdentifierIName(child, src);
	if (!name) return NULL;

	/* Traverse upwards through scopes */
	do {
		int slot = findScopeSlot(parent, name);
		if (slot >= 0) return parent->values[slot];
	} while ((parent = parent->parent));

	{
		char *n = resolveIdentifierName(child, src);
		error(IN_VARIABLE_DOES_NOT_EXIST, child->fname, child->line, n);
		if (n) free(n);
	}

	return NULL;
}

/**
 * Gets a scope without accessing any arrays.
 *
 * \param [in] src The scope to evaluate \a target under.
 *
 * \param [in,out] dest The scope to update the value in.
 *
 * \param [in] target The name of the value containing the scope to get.
 *
 * \return The scope contained in the value in \a dest, named by evaluating \a
 * target under \a src, without accessing any arrays.
 *
 * \retval NULL Either \a target could not be evaluated in \a src or \a target
 * could not be found in \a dest.
 */
/** \todo Add this definition to interpreter.h */
ScopeObject *getScopeObjectLocal(ScopeObject *src,
                                 ScopeObject *dest,
                                 IdentifierNode *target)
{
	ScopeObject *current = dest;
	const Name *name = NULL;

	/* Look up the identifier name */
	name = resolveIdentifierIName(target, src);
	if (!name) return NULL;

	/* Check for calling object reference variable */
	if (name == getNameME()) {
		/* Traverse upwards through callers */
		for (current = dest;
				current->caller;
				current = current->caller);
		return current;
	}

	/* Traverse upwards through scopes */
	do {
		int slot = findScopeSlot(current, name);
		if (slot >= 0) {
			if (current->values[slot]->type != VT_ARRAY) {
				error(IN_VARIABLE_NOT_AN_ARRAY, target->fname, target->line, name->str);
				return NULL;
			}
			return getArray(current->values[slot]);
		}
	} while ((current = current->parent));

	error(IN_VARIABLE_DOES_NOT_EXIST, target->fname, target->line, name->str);

	return NULL;
}

/**
 * Gets a scope (possibly by casting a function) without accessing any arrays.
 *
 * \param [in] src The scope to evaluate \a target under.
 *
 * \param [in,out] dest The scope to update the value in.
 *
 * \param [in] target The name of the value containing the scope to get.
 *
 * \return The scope contained in the value in \a dest, named by evaluating \a
 * target under \a src, without accessing any arrays.
 *
 * \retval NULL Either \a target could not be evaluated in \a src or \a target
 * could not be found in \a dest.
 */
/** \todo Add this definition to interpreter.h */
ScopeObject *getScopeObjectLocalCaller(ScopeObject *src,
                                 ScopeObject *dest,
                                 IdentifierNode *target)
{
	ScopeObject *current = dest;
	const Name *name = NULL;

	/* Look up the identifier name */
	name = resolveIdentifierIName(target, src);
	if (!name) return NULL;

	/* Check for calling object reference variable */
	if (name == getNameME()) {
		/* Traverse upwards through callers */
		for (current = dest;
				current->caller;
				current = current->caller);
		return current;
	}

	/* Traverse upwards through scopes */
	do {
		int slot = findScopeSlot(current, name);
		if (slot >= 0) {
			ValueObject *val = current->values[slot];
			if (val->type != VT_ARRAY && val->type != VT_FUNC) {
				error(IN_VARIABLE_NOT_AN_ARRAY, target->fname, target->line, name->str);
				return NULL;
			}
			if (val->type == VT_ARRAY) return getArray(val);
			else return dest;
		}
	} while ((current = current->parent));

	error(IN_VARIABLE_DOES_NOT_EXIST, target->fname, target->line, name->str);

	return NULL;
}

/**
 * Gets a value from a scope without accessing its ancestors.
 *
 * \param [in] src The scope to evaluate \a target under.
 *
 * \param [in,out] dest The scope to update the value in.
 *
 * \param [in] target The name of the value to get.
 *
 * \return The value in \a dest, named by evaluating \a target under \a src,
 * without accessing any ancestors of \a dest.
 *
 * \retval NULL Either \a target could not be evaluated in \a src or \a target
 * could not be found in \a dest.
 */
ValueObject *getScopeValueLocal(ScopeObject *src,
                                ScopeObject *dest,
                                IdentifierNode *target)
{
	const Name *name = NULL;
	ScopeObject *scope = NULL;
	int slot;

	/* Access any slots */
	while (target->slot) {
		/*
		 * Look up the target in the dest scope, using the src scope
		 * for resolving variables in indirect identifiers
		 */
		scope = getScopeObjectLocal(src, dest, target);
		if (!scope) return NULL;
		dest = scope;

		target = target->slot;
	}

	/* Look up the identifier name */
	name = resolveIdentifierIName(target, src);
	if (!name) return NULL;

	/* Check for value in current scope */
	slot = findScopeSlot(dest, name);
	if (slot >= 0) return dest->values[slot];

	return NULL;
}

/**
 * Gets a scope from within another scope.
 *
 * \param [in] src The scope to evaluate \a target under.
 *
 * \param [in,out] dest The scope to update the value in.
 *
 * \param [in] target The name of the scope to get.
 *
 * \return The value in \a dest, named by evaluating \a target under \a src,
 * without accessing any ancestors of \a dest.
 *
 * \retval NULL Either \a target could not be evaluated in \a src or \a target
 * could not be found in \a dest.
 */
ScopeObject *getScopeObject(ScopeObject *src,
                            ScopeObject *dest,
                            IdentifierNode *target)
{
	ValueObject *val = NULL;
	char *name = NULL;
	int status;
	int isI;
	int isME;
	ScopeObject *scope;
	
	/* Look up the identifier name */
	name = resolveIdentifierName(target, src);
	if (!name) goto getScopeObjectAbort;

	/* Check for targets with special meanings */
	isI = strcmp(name, "I");
	isME = strcmp(name, "ME");
	free(name);
	name = NULL;

	if (!isI) {
		/* The function scope variable */
		return src;
	}
	else if (!isME) {
		/* The calling object scope variable */
		scope = getScopeObjectLocal(src, dest, target);
		if (!scope) goto getScopeObjectAbort;
		return scope;
	}

	/* Access any slots */
	while (target->slot) {
		/*
		 * Look up the target in the dest scope, using the src scope
		 * for resolving variables in indirect identifiers
		 */
		scope = getScopeObjectLocal(src, dest, target);
		if (!scope) goto getScopeObjectAbort;
		dest = scope;

		target = target->slot;
	}

	val = getScopeValue(src, dest, target);
	if (!val) goto getScopeObjectAbort;
	if (val->type != VT_ARRAY) {
		char *name = resolveIdentifierName(target, src);
		error(IN_VARIABLE_NOT_AN_ARRAY, target->fname, target->line, name);
		free(name);
		goto getScopeObjectAbort;
	}

	return getArray(val);

getScopeObjectAbort: /* In case something goes wrong... */

	/* Clean up any allocated structures */
	if (name) free(name);

	return NULL;
}

/**
 * Deletes a value from a scope.
 *
 * \param [in] src The scope to evaluate \a target under.
 *
 * \param [in,out] dest The scope to update the value in.
 *
 * \param [in] target The name of the value to delete.
 */
void deleteScopeValue(ScopeObject *src,
                      ScopeObject *dest,
                      IdentifierNode *target)
{
	ScopeObject *current = NULL;
	ScopeObject *scope = NULL;
	const Name *name = NULL;

	/* Access any slots */
	while (target->slot) {
		/*
		 * Look up the target in the dest scope, using the src scope
		 * for resolving variables in indirect identifiers
		 */
		scope = getScopeObjectLocal(src, dest, target);
		if (!scope) return;
		dest = scope;
		target = target->slot;
	}
	current = dest;

	/* Look up the identifier name */
	name = resolveIdentifierIName(target, src);
	if (!name) return;

	/* Traverse upwards through scopes */
	do {
		int slot = findScopeSlot(current, name);
		if (slot >= 0) {
			unsigned int i;
			scopeVersion++;
			/* Wipe out the value */
			deleteValueObject(current->values[slot]);
			/* Close the hole left in the tables */
			for (i = (unsigned int)slot; i + 1 < current->numvals; i++) {
				current->names[i] = current->names[i + 1];
				current->values[i] = current->values[i + 1];
			}
			current->numvals--;
			/* Every index entry past the hole now points one slot high */
			if (current->idxcap) reindexScope(current);
			return;
		}
	} while ((current = current->parent));
}

/**
 * Creates a returned value.
 *
 * \param [in] type The type of returned value.
 *
 * \param [in] value An optional value to return.
 *
 * \return A pointer to a returned value with the desired properties.
 *
 * \retval NULL Memory allocation failed.
 */
ReturnObject *createReturnObject(ReturnType type,
                                 ValueObject *value)
{
	ReturnObject *p = allocReturnObject();
	if (!p) {
		perror("malloc");
		return NULL;
	}
	p->type = type;
	p->value = value;
	return p;
}

/**
 * Deletes a returned value.
 *
 * \param [in,out] object The returned value to be deleted.
 *
 * \post The memory at \a object and all of its members will be freed.
 */
void deleteReturnObject(ReturnObject *object)
{
	if (!object) return;
	if (object->type == RT_RETURN)
		deleteValueObject(object->value);
	recycleReturnObject(object);
}

/**
 * Starting from an initial parent scope and target identifier, traverses down
 * until the target identifier is not a scope.  Stores the value of the terminal
 * child identifier and its parent scope.
 *
 * \param [in] src The scope to resolve \a target in.
 *
 * \param [in] dest The scope to retrieve \a target from.
 *
 * \param [in] target The array slot to traverse.
 *
 * \param [out] parent The parent of the terminal child identifier of \a target.
 *
 * \param [out] child The terminal child identifier of \a target.
 *
 * \return A status code indicating success or failure.
 *
 * \retval 0 Failed to traverse array.
 *
 * \retval 1 Succeeded to traverse array.
 *
 * \post \a parent will point to the parent scope containing the terminal child.
 *
 * \post \a child will point to the terminal child.
 */
int resolveTerminalSlot(ScopeObject *src,
                        ScopeObject *dest,
                        IdentifierNode *target,
                        ScopeObject **parent,
                        IdentifierNode **child)
{
	ScopeObject *scope = NULL;

	/* Start with default values */
	*parent = dest;
	*child = target;

	/* Access any slots */
	while (target->slot) {
		/*
		 * Look up the target in the dest scope, using the src scope
		 * for resolving variables in indirect identifiers
		 */
		scope = getScopeObjectLocal(src, dest, target);
		if (!scope) goto resolveTerminalSlotAbort;
		dest = scope;

		/* Change the target to the old target's slot */
		target = target->slot;
	}

	/* Store the output values */
	*parent = dest;
	*child = target;

	return 1;

resolveTerminalSlotAbort: /* In case something goes wrong... */

	/* Clean up any allocated structures */
	if (scope) deleteScopeObject(scope);

	return 0;
}

/**
 * Casts the contents of a value to boolean type in an implicit way.  Casting is
 * not done directly to \a node, instead, it is performed on a copy which is
 * what is returned.
 *
 * \param [in] node The value to cast.
 * 
 * \param [in] scope The scope to use for variable interpolation.
 *
 * \return A pointer to a value with a copy of the contents of \a node, cast to
 * boolean type.
 *
 * \retval NULL An error occurred while casting.
 */
ValueObject *castBooleanImplicit(ValueObject *node,
                                 ScopeObject *scope)
{
	if (!node) return NULL;
	return castBooleanExplicit(node, scope);
}

/**
 * Casts the contents of a value to integer type in an implicit way.  Casting is
 * not done directly to \a node, instead, it is performed on a copy which is
 * what is returned.
 *
 * \param [in] node The value to cast.
 * 
 * \param [in] scope The scope to use for variable interpolation.
 *
 * \return A pointer to a value with a copy of the contents of \a node, cast to
 * integer type.
 *
 * \retval NULL An error occurred while casting.
 */
ValueObject *castIntegerImplicit(ValueObject *node,
                                 ScopeObject *scope)
{
	if (!node) return NULL;
	if (node->type == VT_NIL) {
		error(IN_CANNOT_IMPLICITLY_CAST_NIL);
		return NULL;
	}
	else return castIntegerExplicit(node, scope);
}

/**
 * Casts the contents of a value to decimal type in an implicit way.  Casting is
 * not done directly to \a node, instead, it is performed on a copy which is
 * what is returned.
 *
 * \param [in] node The value to cast.
 * 
 * \param [in] scope The scope to use for variable interpolation.
 *
 * \return A pointer to a value with a copy of the contents of \a node, cast to
 * decimal type.
 *
 * \retval NULL An error occurred while casting.
 */
ValueObject *castFloatImplicit(ValueObject *node,
                               ScopeObject *scope)
{
	if (!node) return NULL;
	if (node->type == VT_NIL) {
		error(IN_CANNOT_IMPLICITLY_CAST_NIL);
		return NULL;
	}
	else return castFloatExplicit(node, scope);
}

/**
 * Casts the contents of a value to string type in an implicit way.  Casting is
 * not done directly to \a node, instead, it is performed on a copy which is
 * what is returned.
 *
 * \param [in] node The value to cast.
 * 
 * \param [in] scope The scope to use for variable interpolation.
 *
 * \note \a scope is used to resolve variable interpolation within the string
 * before casting it.  Therefore, a simple way to interpolate the variables
 * within a string is to call this function with it.
 *
 * \return A pointer to a value with a copy of the contents of \a node, cast to
 * string type.
 *
 * \retval NULL An error occurred while casting.
 */
ValueObject *castStringImplicit(ValueObject *node,
                                ScopeObject *scope)
{
	if (!node) return NULL;
	if (node->type == VT_NIL) {
		error(IN_CANNOT_IMPLICITLY_CAST_NIL);
		return NULL;
	}
	else return castStringExplicit(node, scope);
}

/**
 * Casts the contents of a value to boolean type in an explicit way.  Casting is
 * not done directly to \a node, instead, it is performed on a copy which is
 * what is returned.
 *
 * \param [in] node The value to cast.
 * 
 * \param [in] scope The scope to use for variable interpolation.
 *
 * \return A pointer to a value with a copy of the contents of \a node, cast to
 * boolean type.
 *
 * \retval NULL An error occurred while casting.
 */
ValueObject *castBooleanExplicit(ValueObject *node,
                                 ScopeObject *scope)
{
	if (!node) return NULL;
	switch (node->type) {
		case VT_NIL:
			return createBooleanValueObject(0);
		case VT_BOOLEAN:
			return createBooleanValueObject(getInteger(node));
		case VT_INTEGER:
			return createBooleanValueObject(getInteger(node) != 0);
		case VT_FLOAT:
			return createBooleanValueObject(fabs(getFloat(node) - 0.0) > FLT_EPSILON);
		case VT_STRING:
			if (strstr(getString(node), ":{")) {
				/* Perform interpolation */
				ValueObject *ret = NULL;
				ValueObject *interp = castStringExplicit(node, scope);
				if (!interp) return NULL;
				ret = createBooleanValueObject(getString(interp)[0] != '\0');
				deleteValueObject(interp);
				return ret;
			}
			else
				return createBooleanValueObject(getString(node)[0] != '\0');
		case VT_FUNC:
			error(IN_CANNOT_CAST_FUNCTION_TO_BOOLEAN);
			return NULL;
		case VT_ARRAY:
			error(IN_CANNOT_CAST_ARRAY_TO_BOOLEAN);
			return NULL;
		default:
			error(IN_UNKNOWN_VALUE_DURING_BOOLEAN_CAST);
			return NULL;
	}
}

/**
 * Casts the contents of a value to integer type in an explicit way.  Casting is
 * not done directly to \a node, instead, it is performed on a copy which is
 * what is returned.
 *
 * \param [in] node The value to cast.
 * 
 * \param [in] scope The scope to use for variable interpolation.
 *
 * \return A pointer to a value with a copy of the contents of \a node, cast to
 * integer type.
 *
 * \retval NULL An error occurred while casting.
 */
ValueObject *castIntegerExplicit(ValueObject *node,
                                 ScopeObject *scope)
{
	if (!node) return NULL;
	switch (node->type) {
		case VT_NIL:
			return createIntegerValueObject(0);
		case VT_BOOLEAN:
		case VT_INTEGER:
			return createIntegerValueObject(getInteger(node));
		case VT_FLOAT:
			return createIntegerValueObject((long long)getFloat(node));
		case VT_STRING:
			if (strstr(getString(node), ":{")) {
				/* Perform interpolation */
				ValueObject *ret = NULL;
				ValueObject *interp = castStringExplicit(node, scope);
				if (!interp) return NULL;
				long long value = strtoll(getString(interp), NULL, 0);
				ret = createIntegerValueObject(value);
				deleteValueObject(interp);
				return ret;
			}
			else {
				long long value = strtoll(getString(node), NULL, 0);
				return createIntegerValueObject(value);
			}
		case VT_FUNC:
			error(IN_CANNOT_CAST_FUNCTION_TO_INTEGER);
			return NULL;
		case VT_ARRAY:
			error(IN_CANNOT_CAST_ARRAY_TO_INTEGER);
			return NULL;
		default:
			error(IN_UNKNOWN_VALUE_DURING_INTEGER_CAST);
			return NULL;
	}
}

/**
 * Casts the contents of a value to decimal type in an explicit way.  Casting is
 * not done directly to \a node, instead, it is performed on a copy which is
 * what is returned.
 *
 * \param [in] node The value to cast.
 * 
 * \param [in] scope The scope to use for variable interpolation.
 *
 * \return A pointer to a value with a copy of the contents of \a node, cast to
 * decimal type.
 *
 * \retval NULL An error occurred while casting.
 */
ValueObject *castFloatExplicit(ValueObject *node,
                               ScopeObject *scope)
{
	if (!node) return NULL;
	switch (node->type) {
		case VT_NIL:
			return createFloatValueObject(0.0);
		case VT_BOOLEAN:
		case VT_INTEGER:
			return createFloatValueObject((float)getInteger(node));
		case VT_FLOAT:
			return createFloatValueObject(getFloat(node));
		case VT_STRING:
			if (strstr(getString(node), ":{")) {
				/* Perform interpolation */
				ValueObject *ret = NULL;
				ValueObject *interp = castStringExplicit(node, scope);
				if (!interp) return NULL;
				float value = strtof(getString(interp), NULL);
				ret = createFloatValueObject(value);
				deleteValueObject(interp);
				return ret;
			}
			else {
				float value = strtof(getString(node), NULL);
				return createFloatValueObject(value);
			}
		case VT_FUNC:
			error(IN_CANNOT_CAST_FUNCTION_TO_DECIMAL);
			return NULL;
		case VT_ARRAY:
			error(IN_CANNOT_CAST_ARRAY_TO_DECIMAL);
			return NULL;
		default:
			error(IN_UNKNOWN_VALUE_DURING_DECIMAL_CAST);
			return NULL;
	}
}

/**
 * Casts the contents of a value to string type in an explicit way.  Casting is
 * not done directly to \a node, instead, it is performed on a copy which is
 * what is returned.
 *
 * \param [in] node The value to cast.
 * 
 * \param [in] scope The scope to use for variable interpolation.
 *
 * \note \a scope is used to resolve variable interpolation within the string
 * before casting it.  Therefore, a simple way to interpolate the variables
 * within a string is to call this function with it.
 *
 * \return A pointer to a value with a copy of the contents of \a node, cast to
 * string type.
 *
 * \retval NULL An error occurred while casting.
 */
ValueObject *castStringExplicit(ValueObject *node,
                                ScopeObject *scope)
{
	if (!node) return NULL;
	switch (node->type) {
		case VT_NIL: {
			char *str = copyString("");
			if (!str) return NULL;
			return createStringValueObject(str);
		}
		case VT_BOOLEAN: {
			/*
			 * \note The spec does not define how TROOFs may be cast
			 * to YARNs.
			 */
			error(IN_CANNOT_CAST_BOOLEAN_TO_STRING);
			return NULL;
		}
		case VT_INTEGER: {
			char *data = NULL;
			/*
			 * One character per integer bit plus one more for the
			 * null character
			 */
			size_t size = sizeof(long long) * 8 + 1;
			data = malloc(sizeof(char) * size);
			if (!data) return NULL;
			sprintf(data, "%lli", getInteger(node));
			return createStringValueObject(data);
		}
		case VT_FLOAT: {
			char *data = NULL;
			unsigned int precision = 2;
			/*
			 * One character per float bit plus one more for the
			 * null character
			 */
			size_t size = sizeof(float) * 8 + 1;
			data = malloc(sizeof(char) * size);
			if (!data) return NULL;
			sprintf(data, "%f", getFloat(node));
			/* Truncate to a certain number of decimal places */
			strchr(data, '.')[precision + 1] = '\0';
			return createStringValueObject(data);
		}
		case VT_STRING: {
			char *temp = NULL;
			char *data = NULL;
			char *str = getString(node);
			unsigned int a, b;
			size_t size;
			/* Perform interpolation */
			size = strlen(getString(node)) + 1;
			temp = malloc(sizeof(char) * size);
			for (a = 0, b = 0; str[b] != '\0'; ) {
				if (!strncmp(str + b, ":)", 2)) {
					temp[a] = '\n';
					a++, b += 2;
				}
				else if (!strncmp(str + b, ":>", 2)) {
					temp[a] = '\t';
					a++, b += 2;
				}
				else if (!strncmp(str + b, ":o", 2)) {
					temp[a] = '\a';
					a++, b += 2;
				}
				else if (!strncmp(str + b, ":\"", 2)) {
					temp[a] = '"';
					a++, b += 2;
				}
				else if (!strncmp(str + b, "::", 2)) {
					temp[a] = ':';
					a++, b += 2;
				}
				else if (!strncmp(str + b, ":(", 2)) {
					const char *start = str + b + 2;
					const char *end = strchr(start, ')');
					size_t len;
					char *image = NULL;
					long codepoint;
					/* A code point above U+FFFF needs four bytes. */
					char out[4];
					size_t num;
					void *mem = NULL;
					if (end < start) {
						error(IN_EXPECTED_CLOSING_PAREN);
						free(temp);
						return NULL;
					}
					len = (size_t)(end - start);
					image = malloc(sizeof(char) * (len + 1));
					strncpy(image, start, len);
					image[len] = '\0';
					if (!isHexString(image)) {
						error(IN_INVALID_HEX_NUMBER);
						free(temp);
						free(image);
						return NULL;
					}
					codepoint = strtol(image, NULL, 16);
					free(image);
					if (codepoint < 0) {
						error(IN_CODE_POINT_MUST_BE_POSITIVE);
						free(temp);
						return NULL;
					}
					num = convertCodePointToUTF8((unsigned int)codepoint, out);
					if (num == 0) {
						free(temp);
						return NULL;
					}
					size += num;
					mem = realloc(temp, size);
					if (!mem) {
						perror("realloc");
						free(temp);
						return NULL;
					}
					temp = mem;
					strncpy(temp + a, out, num);
					a += num, b += len + 3;
				}
				else if (!strncmp(str + b, ":[", 2)) {
					const char *start = str + b + 2;
					const char *end = strchr(start, ']');
					size_t len;
					char *image = NULL;
					long codepoint;
					/* A code point above U+FFFF needs four bytes. */
					char out[4];
					size_t num;
					void *mem = NULL;
					if (end < start) {
						error(IN_EXPECTED_CLOSING_SQUARE_BRACKET);
						free(temp);
						return NULL;
					}
					len = (size_t)(end - start);
					image = malloc(sizeof(char) * (len + 1));
					strncpy(image, start, len);
					strncpy(image, start, len);
					image[len] = '\0';
					codepoint = convertNormativeNameToCodePoint(image);
					free(image);
					if (codepoint < 0) {
						error(IN_CODE_POINT_MUST_BE_POSITIVE);
						free(temp);
						return NULL;
					}
					num = convertCodePointToUTF8((unsigned int)codepoint, out);
					size += num;
					mem = realloc(temp, size);
					if (!mem) {
						perror("realloc");
						free(temp);
						return NULL;
					}
					temp = mem;
					strncpy(temp + a, out, num);
					a += num, b += len + 3;
				}
				else if (!strncmp(str + b, ":{", 2)) {
					IdentifierNode *target = NULL;
					ValueObject *val = NULL, *use = NULL;
					/* Copy the variable name into image */
					const char *start = str + b + 2;
					const char *end = strchr(start, '}');
					size_t len;
					char *image = NULL;
					void *mem = NULL;
					if (end < start) {
						error(IN_EXPECTED_CLOSING_CURLY_BRACE);
						free(temp);
						return NULL;
					}
					len = (size_t)(end - start);
					image = malloc(sizeof(char) * (len + 1));
					strncpy(image, start, len);
					image[len] = '\0';
					if (!strcmp(image, "IT"))
						/* Lookup implicit variable */
						val = scope->impvar;
					else {
						/*
						 * Create a new IdentifierNode
						 * structure and look up its
						 * value
						 */
						target = createIdentifierNode(IT_DIRECT, image, NULL, NULL, 0);
						if (!target) {
							free(temp);
							return NULL;
						}
						val = getScopeValue(scope, scope, target);
						if (!val) {
							error(IN_VARIABLE_DOES_NOT_EXIST, target->fname, target->line, image);
							deleteIdentifierNode(target);
							free(temp);
							return NULL;
						}
						deleteIdentifierNode(target);
					}
					/* Cast the variable value to a string */
					if (!(use = castStringImplicit(val, scope))) {
						free(temp);
						return NULL;
					}
					/* Update the size of the new string */
					size += strlen(getString(use));
					mem = realloc(temp, size);
					if (!mem) {
						perror("realloc");
						free(temp);
					}
					temp = mem;
					/* Copy the variable string into the new string */
					strcpy(temp + a, getString(use));
					a += strlen(getString(use)), b += len + 3;
					deleteValueObject(use);
				}
				else {
					temp[a] = str[b];
					a++, b++;
				}
			}
			temp[a] = '\0';
			data = malloc(sizeof(char) * (strlen(temp) + 1));
			strcpy(data, temp);
			free(temp);
			return createStringValueObject(data);
		}
		case VT_FUNC: {
			error(IN_CANNOT_CAST_FUNCTION_TO_STRING);
			return NULL;
		}
		case VT_ARRAY:
			error(IN_CANNOT_CAST_ARRAY_TO_STRING);
			return NULL;
		default:
			error(IN_UNKNOWN_VALUE_DURING_STRING_CAST);
			return NULL;
	}
}

/**
 * Interprets an implicit variable.
 *
 * \param [in] node Not used (see note).
 *
 * \param [in] scope The scope from which to use the implicit variable.
 *
 * \note \a node is not used by this function but is still included in its
 * prototype to allow this function to be stored in a jump table for fast
 * execution.
 *
 * \return A pointer to the value of \a scope's implicit variable.
 */
ValueObject *interpretImpVarExprNode(ExprNode *node,
                                     ScopeObject *scope)
{
	node = NULL;
	return scope->impvar;
}

/**
 * Interprets a cast.
 *
 * \param [in] node A pointer to the expression to interpret.
 *
 * \param [in] scope A pointer to a scope to evaluate \a node under.
 *
 * \pre \a node contains a expression created by createCastExprNode().
 *
 * \return A pointer to the cast value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ValueObject *interpretCastExprNode(ExprNode *node,
                                   ScopeObject *scope)
{
	CastExprNode *expr = (CastExprNode *)node->expr;
	ValueObject *val = interpretExprNode(expr->target, scope);
	ValueObject *ret = NULL;
	if (!val) return NULL;
	switch(expr->newtype->type) {
		case CT_NIL:
			deleteValueObject(val);
			return createNilValueObject();
		case CT_BOOLEAN:
			ret = castBooleanExplicit(val, scope);
			deleteValueObject(val);
			return ret;
		case CT_INTEGER:
			ret = castIntegerExplicit(val, scope);
			deleteValueObject(val);
			return ret;
		case CT_FLOAT:
			ret = castFloatExplicit(val, scope);
			deleteValueObject(val);
			return ret;
		case CT_STRING:
			ret = castStringExplicit(val, scope);
			deleteValueObject(val);
			return ret;
		default:
			error(IN_UNKNOWN_CAST_TYPE);
			deleteValueObject(val);
			return NULL;
	}
}

/**
 * Interprets a function call.
 *
 * \param [in] node A pointer to the expression to interpret.
 *
 * \param [in,out] scope A pointer to a scope to evaluate \a node under.
 *
 * \pre \a node contains an expression created by createFuncCallExprNode().
 *
 * \return A pointer to the returned value.
 *
 * \retval NULL An error occurred during interpretation.
 */
/**
 * Calls a function with values that have already been evaluated.
 *
 * This is the path the virtual machine takes into a function the compiler
 * could not lower, and the path \ref interpretFuncCallExprNode takes once it
 * has evaluated the call's arguments.
 *
 * \param [in] def The function to call.
 *
 * \param [in] args The argument values, which are borrowed.
 *
 * \param [in] numargs The number of arguments.
 *
 * \param [in] scope The scope the call is made from.
 *
 * \return The function's return value.
 *
 * \retval NULL The call failed and an error has been reported.
 */
ValueObject *callFunctionValues(FuncDefStmtNode *def,
                                ValueObject **args,
                                unsigned int numargs,
                                ScopeObject *scope)
{
	ScopeObject *outer = NULL;
	ReturnObject *retval = NULL;
	ValueObject *ret = NULL;
	unsigned int n;

	if (stackExhausted()) {
		error(IN_RECURSION_TOO_DEEP);
		return NULL;
	}

	outer = createScopeObjectCaller(scope, scope);
	if (!outer) return NULL;

	for (n = 0; n < numargs; n++) {
		if (!createScopeValue(scope, outer, def->args->ids[n])
				|| !updateScopeValue(scope, outer, def->args->ids[n],
						copyValueObject(args[n]))) {
			deleteScopeObject(outer);
			return NULL;
		}
	}

	if (!(retval = interpretStmtNodeList(def->body->stmts, outer))) {
		deleteScopeObject(outer);
		return NULL;
	}
	switch (retval->type) {
		case RT_DEFAULT:
			/* Extract return value */
			ret = outer->impvar;
			outer->impvar = NULL;
			break;
		case RT_BREAK:
			ret = createNilValueObject();
			break;
		case RT_RETURN:
			/* Extract return value */
			ret = retval->value;
			retval->value = NULL;
			break;
		default:
			error(IN_INVALID_RETURN_TYPE);
			break;
	}
	deleteReturnObject(retval);
	deleteScopeObject(outer);
	return ret;
}

ValueObject *interpretFuncCallExprNode(ExprNode *node,
                                       ScopeObject *scope)
{
	FuncCallExprNode *expr = (FuncCallExprNode *)node->expr;
	unsigned int n;
	ScopeObject *outer = NULL;
	ValueObject *def = NULL;
	ReturnObject *retval = NULL;
	ValueObject *ret = NULL;
	ScopeObject *dest = NULL;
	ScopeObject *target = NULL;

	if (stackExhausted()) {
		error(IN_RECURSION_TOO_DEEP);
		return NULL;
	}

	dest = getScopeObject(scope, scope, expr->scope);

	target = getScopeObjectLocalCaller(scope, dest, expr->name);
	if (!target) return NULL;

	def = getScopeValue(scope, dest, expr->name);

	if (!def || def->type != VT_FUNC) {
		IdentifierNode *id = (IdentifierNode *)(expr->name);
		char *name = resolveIdentifierName(id, scope);
		if (name) {
			error(IN_UNDEFINED_FUNCTION, id->fname, id->line, name);
			free(name);
		}
		return NULL;
	}
	/* Check for correct supplied arity */
	if (getFunction(def)->args->num != expr->args->num) {
		IdentifierNode *id = (IdentifierNode *)(expr->name);
		char *name = resolveIdentifierName(id, scope);
		if (name) {
			error(IN_INCORRECT_NUMBER_OF_ARGUMENTS, id->fname, id->line, name);
			free(name);
		}
		return NULL;
	}

	/* A compiled function is entered directly, with its arguments in a
	 * plain array rather than a scope. */
	if (getFunction(def)->proto) {
		ValueObject **vals = NULL;
		if (expr->args->num) {
			vals = malloc(sizeof(ValueObject *) * expr->args->num);
			if (!vals) {
				perror("malloc");
				return NULL;
			}
		}
		for (n = 0; n < expr->args->num; n++) {
			vals[n] = interpretExprNode(expr->args->exprs[n], scope);
			if (!vals[n]) {
				unsigned int i;
				for (i = 0; i < n; i++) deleteValueObject(vals[i]);
				free(vals);
				return NULL;
			}
		}
		ret = callProto((Proto *)getFunction(def)->proto, vals,
				expr->args->num, scope, target);
		for (n = 0; n < expr->args->num; n++) deleteValueObject(vals[n]);
		free(vals);
		return ret;
	}

	outer = createScopeObjectCaller(scope, target);
	if (!outer) return NULL;

	for (n = 0; n < getFunction(def)->args->num; n++) {
		ValueObject *val = NULL;
		if (!createScopeValue(scope, outer, getFunction(def)->args->ids[n])) {
			deleteScopeObject(outer);
			return NULL;
		}
		if (!(val = interpretExprNode(expr->args->exprs[n], scope))) {
			deleteScopeObject(outer);
			return NULL;
		}
		if (!updateScopeValue(scope, outer, getFunction(def)->args->ids[n], val)) {
			deleteScopeObject(outer);
			deleteValueObject(val);
			return NULL;
		}
	}
	/**
	 * \note We use interpretStmtNodeList here because we want to have
	 * access to the function's scope as we may need to retrieve the
	 * implicit variable in the case of a default return.
	 */
	if (!(retval = interpretStmtNodeList(getFunction(def)->body->stmts, outer))) {
		deleteScopeObject(outer);
		return NULL;
	}
	switch (retval->type) {
		case RT_DEFAULT:
			/* Extract return value */
			ret = outer->impvar;
			outer->impvar = NULL;
			break;
		case RT_BREAK:
			ret = createNilValueObject();
			break;
		case RT_RETURN:
			/* Extract return value */
			ret = retval->value;
			retval->value = NULL;
			break;
		default:
			error(IN_INVALID_RETURN_TYPE);
			break;
	}
	deleteReturnObject(retval);
	deleteScopeObject(outer);
	return ret;
}

/**
 * Interprets an identifier.
 *
 * \param [in] node A pointer to the expression to interpret.
 *
 * \param [in,out] scope A pointer to a scope to evaluate \a node under.
 *
 * \pre \a node contains an identifier created by createIdentifierNode().
 *
 * \return A pointer to the cast value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ValueObject *interpretIdentifierExprNode(ExprNode *node,
                                         ScopeObject *scope)
{
	ValueObject *val = getScopeValue(scope, scope, node->expr);
	if (!val) return NULL;
	return copyValueObject(val);
}

/**
 * Interprets a constant.
 *
 * \param [in] node A pointer to the expression to interpret.
 *
 * \param [in] scope Not used (see note).
 *
 * \note \a node is not used by this function but is still included in its
 * prototype to allow this function to be stored in a jump table for fast
 * execution.
 *
 * \pre \a node contains a constant created by createXConstantNode(), where X is
 * either Boolean, Integer, Float, or String.
 *
 * \return A pointer to the constant value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ValueObject *interpretConstantExprNode(ExprNode *node,
                                       ScopeObject *scope)
{
	ConstantNode *expr = (ConstantNode *)node->expr;
	scope = NULL;
	switch (expr->type) {
		case CT_NIL:
			return createNilValueObject();
		case CT_BOOLEAN:
			return createBooleanValueObject(expr->data.i);
		case CT_INTEGER:
			return createIntegerValueObject(expr->data.i);
		case CT_FLOAT:
			return createFloatValueObject(expr->data.f);
		case CT_STRING: {
			/*
			 * \note For efficiency, string interpolation should be
			 * performed by caller because it only needs to be
			 * performed when necessary.
			 */
			char *str = copyString(expr->data.s);
			if (!str) return NULL;
			return createStringValueObject(str);
		}
		default:
			error(IN_UNKNOWN_CONSTANT_TYPE);
			return NULL;
	}
}

/**
 * Interprets a logical NOT operation.
 *
 * \param [in] expr A pointer to the expression to interpret.
 *
 * \param [in] scope A pointer to a scope to evaluate \a node under.
 *
 * \note Only the first element of \a args is used.
 *
 * \return A pointer to the value of the logical negation of the first element
 * of \a args.
 *
 * \retval NULL An error occurred during interpretation.
 */
ValueObject *interpretNotOpExprNode(OpExprNode *expr,
                                    ScopeObject *scope)
{
	ValueObject *val = interpretExprNode(expr->args->exprs[0], scope);
	ValueObject *use = val;
	int retval;
	unsigned short cast = 0;
	if (!val) return NULL;
	if (val->type != VT_BOOLEAN && val->type != VT_INTEGER) {
		use = castBooleanImplicit(val, scope);
		if (!use) {
			deleteValueObject(val);
			return NULL;
		}
		cast = 1;
	}
	retval = getInteger(use);
	if (cast) deleteValueObject(use);
	deleteValueObject(val);
	return createBooleanValueObject(!retval);
}

/**
 * Adds an integer to an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the sum of \a a and \a b.
 */
ValueObject *opAddIntegerInteger(ValueObject *a,
                                 ValueObject *b)
{
	return createIntegerValueObject(getInteger(a) + getInteger(b));
}

/**
 * Subtracts an integer from an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the difference of \a a and \a b.
 */
ValueObject *opSubIntegerInteger(ValueObject *a,
                                 ValueObject *b)
{
	return createIntegerValueObject(getInteger(a) - getInteger(b));
}

/**
 * Multiplies an integer by an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the product of \a a and \a b.
 */
ValueObject *opMultIntegerInteger(ValueObject *a,
                                  ValueObject *b)
{
	return createIntegerValueObject(getInteger(a) * getInteger(b));
}

/**
 * Divides an integer by an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the quotient of \a a and \a b.
 *
 * \retval NULL Division by zero.
 */
ValueObject *opDivIntegerInteger(ValueObject *a,
                                 ValueObject *b)
{
	if (getInteger(b) == 0) {
		error(IN_DIVISION_BY_ZERO);
		return NULL;
	}
	return createIntegerValueObject(getInteger(a) / getInteger(b));
}

/**
 * Finds the maximum of an integer and an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the maximum of \a a and \a b.
 */
ValueObject *opMaxIntegerInteger(ValueObject *a,
                                 ValueObject *b)
{
	return createIntegerValueObject(getInteger(a) > getInteger(b) ? getInteger(a) : getInteger(b));
}

/**
 * Finds the minimum of an integer and an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the minimum of \a a and \a b.
 */
ValueObject *opMinIntegerInteger(ValueObject *a,
                                 ValueObject *b)
{
	return createIntegerValueObject(getInteger(a) < getInteger(b) ? getInteger(a) : getInteger(b));
}

/**
 * Calculates the modulus of an integer and an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the modulus of \a a and \a b.
 */
ValueObject *opModIntegerInteger(ValueObject *a,
                                 ValueObject *b)
{
	if (getInteger(b) == 0) {
		error(IN_DIVISION_BY_ZERO);
		return NULL;
	}
	return createIntegerValueObject(getInteger(a) % getInteger(b));
}

/**
 * Adds an integer to a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the sum of \a a and \a b.
 */
ValueObject *opAddIntegerFloat(ValueObject *a,
                               ValueObject *b)
{
	return createFloatValueObject((float)(getInteger(a) + getFloat(b)));
}

/**
 * Subtracts an integer from a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the difference of \a a and \a b.
 */
ValueObject *opSubIntegerFloat(ValueObject *a,
                               ValueObject *b)
{
	return createFloatValueObject((float)(getInteger(a) - getFloat(b)));
}

/**
 * Multiplies an integer by a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the product of \a a and \a b.
 */
ValueObject *opMultIntegerFloat(ValueObject *a,
                                ValueObject *b)
{
	return createFloatValueObject((float)(getInteger(a) * getFloat(b)));
}

/**
 * Divides an integer by a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the quotient of \a a and \a b.
 *
 * \retval NULL Division by zero.
 */
ValueObject *opDivIntegerFloat(ValueObject *a,
                               ValueObject *b)
{
	if (fabs(getFloat(b) - 0.0) < FLT_EPSILON) {
		error(IN_DIVISION_BY_ZERO);
		return NULL;
	}
	return createFloatValueObject((float)(getInteger(a) / getFloat(b)));
}

/**
 * Finds the maximum of an integer and a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the maximum of \a a and \a b.
 */
ValueObject *opMaxIntegerFloat(ValueObject *a,
                               ValueObject *b)
{
	return createFloatValueObject((float)(getInteger(a)) > getFloat(b) ? (float)(getInteger(a)) : getFloat(b));
}

/**
 * Finds the minimum of an integer and a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the minimum of \a a and \a b.
 */
ValueObject *opMinIntegerFloat(ValueObject *a,
                               ValueObject *b)
{
	return createFloatValueObject((float)(getInteger(a)) < getFloat(b) ? (float)(getInteger(a)) : getFloat(b));
}

/**
 * Calculates the modulus of an integer and a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the modulus of \a a and \a b.
 */
ValueObject *opModIntegerFloat(ValueObject *a,
                               ValueObject *b)
{
	if (fabs(getFloat(b) - 0.0) < FLT_EPSILON) {
		error(IN_DIVISION_BY_ZERO);
		return NULL;
	}
	return createFloatValueObject((float)(fmod((double)(getInteger(a)), getFloat(b))));
}

/**
 * Adds a decimal to an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the sum of \a a and \a b.
 */
ValueObject *opAddFloatInteger(ValueObject *a,
                               ValueObject *b)
{
	return createFloatValueObject(getFloat(a) + getInteger(b));
}

/**
 * Subtracts a decimal from an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the difference of \a a and \a b.
 */
ValueObject *opSubFloatInteger(ValueObject *a,
                               ValueObject *b)
{
	return createFloatValueObject(getFloat(a) - getInteger(b));
}

/**
 * Multiplies a decimal by an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the product of \a a and \a b.
 */
ValueObject *opMultFloatInteger(ValueObject *a,
                                ValueObject *b)
{
	return createFloatValueObject(getFloat(a) * getInteger(b));
}

/**
 * Divides a decimal by an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the quotient of \a a and \a b.
 *
 * \retval NULL Division by zero.
 */
ValueObject *opDivFloatInteger(ValueObject *a,
                               ValueObject *b)
{
	if (getInteger(b) == 0) {
		error(IN_DIVISION_BY_ZERO);
		return NULL;
	}
	return createFloatValueObject(getFloat(a) / getInteger(b));
}

/**
 * Finds the maximum of a decimal and an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the maximum of \a a and \a b.
 */
ValueObject *opMaxFloatInteger(ValueObject *a,
                               ValueObject *b)
{
	return createFloatValueObject(getFloat(a) > (float)(getInteger(b)) ? getFloat(a) : (float)(getInteger(b)));
}

/**
 * Finds the minimum of a decimal and an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the minimum of \a a and \a b.
 */
ValueObject *opMinFloatInteger(ValueObject *a,
                               ValueObject *b)
{
	return createFloatValueObject(getFloat(a) < (float)(getInteger(b)) ? getFloat(a) : (float)(getInteger(b)));
}

/**
 * Calculates the modulus of a decimal and an integer.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the modulus of \a a and \a b.
 */
ValueObject *opModFloatInteger(ValueObject *a,
                               ValueObject *b)
{
	if (getInteger(b) == 0) {
		error(IN_DIVISION_BY_ZERO);
		return NULL;
	}
	return createFloatValueObject((float)(fmod(getFloat(a), (double)(getInteger(b)))));
}

/**
 * Adds a decimal to a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the sum of \a a and \a b.
 */
ValueObject *opAddFloatFloat(ValueObject *a,
                             ValueObject *b)
{
	return createFloatValueObject(getFloat(a) + getFloat(b));
}

/**
 * Subtracts a decimal from a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the difference of \a a and \a b.
 */
ValueObject *opSubFloatFloat(ValueObject *a,
                             ValueObject *b)
{
	return createFloatValueObject(getFloat(a) - getFloat(b));
}

/**
 * Multiplies a decimal by a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the product of \a a and \a b.
 */
ValueObject *opMultFloatFloat(ValueObject *a,
                              ValueObject *b)
{
	return createFloatValueObject(getFloat(a) * getFloat(b));
}

/**
 * Divides a decimal by a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the quotient of \a a and \a b.
 *
 * \retval NULL Division by zero.
 */
ValueObject *opDivFloatFloat(ValueObject *a,
                             ValueObject *b)
{
	if (fabs(getFloat(b) - 0.0) < FLT_EPSILON) {
		error(IN_DIVISION_BY_ZERO);
		return NULL;
	}
	return createFloatValueObject(getFloat(a) / getFloat(b));
}

/**
 * Finds the maximum of a decimal and a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the maximum of \a a and \a b.
 */
ValueObject *opMaxFloatFloat(ValueObject *a,
                             ValueObject *b)
{
	return createFloatValueObject(getFloat(a) > getFloat(b) ? getFloat(a) : getFloat(b));
}

/**
 * Finds the minimum of a decimal and a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the minimum of \a a and \a b.
 */
ValueObject *opMinFloatFloat(ValueObject *a,
                             ValueObject *b)
{
	return createFloatValueObject(getFloat(a) < getFloat(b) ? getFloat(a) : getFloat(b));
}

/**
 * Calculates the modulus of a decimal and a decimal.
 *
 * \param [in] a The first operand.
 *
 * \param [in] b The second operand.
 *
 * \return A pointer to the value of the modulus of \a a and \a b.
 */
ValueObject *opModFloatFloat(ValueObject *a,
                             ValueObject *b)
{
	if (fabs(getFloat(b) - 0.0) < FLT_EPSILON) {
		error(IN_DIVISION_BY_ZERO);
		return NULL;
	}
	return createFloatValueObject((float)(fmod(getFloat(a), getFloat(b))));
}

/*
 * A jump table for arithmetic operations.  The first index determines the
 * particular arithmetic operation to perform, the second index determines the
 * type of the first argument, and the third index determines the type of the
 * second object.
 */
static ValueObject *(*ArithOpJumpTable[7][2][2])(ValueObject *, ValueObject *) = {
	{ { opAddIntegerInteger, opAddIntegerFloat }, { opAddFloatInteger, opAddFloatFloat } },
	{ { opSubIntegerInteger, opSubIntegerFloat }, { opSubFloatInteger, opSubFloatFloat } },
	{ { opMultIntegerInteger, opMultIntegerFloat }, { opMultFloatInteger, opMultFloatFloat } },
	{ { opDivIntegerInteger, opDivIntegerFloat }, { opDivFloatInteger, opDivFloatFloat } },
	{ { opModIntegerInteger, opModIntegerFloat }, { opModFloatInteger, opModFloatFloat } },
	{ { opMaxIntegerInteger, opMaxIntegerFloat }, { opMaxFloatInteger, opMaxFloatFloat } },
	{ { opMinIntegerInteger, opMinIntegerFloat }, { opMinFloatInteger, opMinFloatFloat } }
};

/**
 * Interprets an arithmetic operation.
 *
 * \param [in] expr The operation to interpret.
 *
 * \param [in] scope The scope to evaluate \a expr under.
 *
 * \note Only supports binary arithmetic operations.
 *
 * \return A pointer to the value of the arithmetic operation.
 *
 * \retval NULL An error occurred during interpretation.
 */
ValueObject *applyArithOp(OpType type,
                          ValueObject *val1,
                          ValueObject *val2,
                          ScopeObject *scope)
{
	ValueObject *use1 = val1;
	ValueObject *use2 = val2;
	unsigned int cast1 = 0;
	unsigned int cast2 = 0;
	ValueObject *ret = NULL;

	/* Check if a floating point decimal string and cast */
	switch (val1->type) {
		case VT_NIL:
		case VT_BOOLEAN:
			use1 = castIntegerImplicit(val1, scope);
			if (!use1) return NULL;
			cast1 = 1;
			break;
		case VT_INTEGER:
		case VT_FLOAT:
			break;
		case VT_STRING: {
			/* Perform interpolation */
			ValueObject *interp = castStringExplicit(val1, scope);
			if (!interp) return NULL;
			if (strchr(getString(interp), '.'))
				use1 = castFloatImplicit(interp, scope);
			else
				use1 = castIntegerImplicit(interp, scope);
			deleteValueObject(interp);
			if (!use1) return NULL;
			cast1 = 1;
			break;
		}
		default:
			error(IN_INVALID_OPERAND_TYPE);
			return NULL;
	}
	switch (val2->type) {
		case VT_NIL:
		case VT_BOOLEAN:
			use2 = castIntegerImplicit(val2, scope);
			if (!use2) {
				if (cast1) deleteValueObject(use1);
				return NULL;
			}
			cast2 = 1;
			break;
		case VT_INTEGER:
		case VT_FLOAT:
			break;
		case VT_STRING: {
			/* Perform interpolation */
			ValueObject *interp = castStringExplicit(val2, scope);
			if (!interp) {
				if (cast1) deleteValueObject(use1);
				return NULL;
			}
			if (strchr(getString(interp), '.'))
				use2 = castFloatImplicit(interp, scope);
			else
				use2 = castIntegerImplicit(interp, scope);
			deleteValueObject(interp);
			if (!use2) {
				if (cast1) deleteValueObject(use1);
				return NULL;
			}
			cast2 = 1;
			break;
		}
		default:
			error(IN_INVALID_OPERAND_TYPE);
			if (cast1) deleteValueObject(use1);
			return NULL;
	}
	/* Do math depending on value types */
	ret = ArithOpJumpTable[type][use1->type][use2->type](use1, use2);
	/* Clean up after floating point decimal casts */
	if (cast1) deleteValueObject(use1);
	if (cast2) deleteValueObject(use2);
	return ret;
}

ValueObject *interpretArithOpExprNode(OpExprNode *expr,
                                      ScopeObject *scope)
{
	ValueObject *val1 = interpretExprNode(expr->args->exprs[0], scope);
	ValueObject *val2 = interpretExprNode(expr->args->exprs[1], scope);
	ValueObject *ret = NULL;
	if (!val1 || !val2) {
		deleteValueObject(val1);
		deleteValueObject(val2);
		return NULL;
	}
	ret = applyArithOp(expr->type, val1, val2, scope);
	deleteValueObject(val1);
	deleteValueObject(val2);
	return ret;
}

/**
 * Reduces a value to the truth of its contents.
 *
 * \param [in] val The value to test.
 *
 * \param [in] scope The scope to cast \a val under.
 *
 * \param [out] ok Set to zero if \a val could not be cast.
 *
 * \return Whether \a val is true.
 */
int valueIsTrue(ValueObject *val, ScopeObject *scope, int *ok)
{
	ValueObject *use = val;
	int ret;
	*ok = 1;
	if (val->type != VT_BOOLEAN && val->type != VT_INTEGER) {
		use = castBooleanImplicit(val, scope);
		if (!use) {
			*ok = 0;
			return 0;
		}
		ret = (getInteger(use) != 0);
		deleteValueObject(use);
		return ret;
	}
	return getInteger(use) != 0;
}

/**
 * Interprets a boolean operation.
 *
 * \param [in] expr The operation to interpret.
 *
 * \param [in] scope The scope to evaluate \a expr under.
 *
 * \return A pointer to the value of the boolean operation.
 *
 * \retval NULL An error occurred during interpretation.
 */
ValueObject *interpretBoolOpExprNode(OpExprNode *expr,
                                     ScopeObject *scope)
{
	unsigned int n;
	int acc = 0;
	/*
	 * Proceed to apply the same operation on the accumulator for the
	 * remaining arguments.
	 */
	for (n = 0; n < expr->args->num; n++) {
		ValueObject *val = interpretExprNode(expr->args->exprs[n], scope);
		ValueObject *use = val;
		int temp;
		unsigned int cast = 0;
		if (!val) return NULL;
		if (val->type != VT_BOOLEAN && val->type != VT_INTEGER) {
			use = castBooleanImplicit(val, scope);
			if (!use) {
				deleteValueObject(val);
				return NULL;
			}
			cast = 1;
		}
		temp = getInteger(use);
		if (cast) deleteValueObject(use);
		deleteValueObject(val);
		if (n == 0) acc = temp;
		else {
			switch (expr->type) {
				case OP_AND:
					acc &= temp;
					break;
				case OP_OR:
					acc |= temp;
					break;
				case OP_XOR:
					acc ^= temp;
					break;
				default:
					error(IN_INVALID_BOOLEAN_OPERATION_TYPE);
					return NULL;
			}
		}
		/**
		 * \note The specification does not say whether boolean logic
		 * short circuits or not.  Here, we assume it does.
		 */
		if (expr->type == OP_AND && acc == 0) break;
		else if (expr->type == OP_OR && acc == 1) break;
	}
	return createBooleanValueObject(acc);
}

/**
 * Checks if an integer value is equal to another integer value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is equal to \a b.
 */
ValueObject *opEqIntegerInteger(ValueObject *a,
                                ValueObject *b)
{
	return createBooleanValueObject(getInteger(a) == getInteger(b));
}

/**
 * Checks if an integer value is not equal to another integer value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is not equal to \a b.
 */
ValueObject *opNeqIntegerInteger(ValueObject *a,
                                 ValueObject *b)
{
	return createBooleanValueObject(getInteger(a) != getInteger(b));
}

/**
 * Checks if an integer value is equal to a decimal value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is equal to \a b.
 */
ValueObject *opEqIntegerFloat(ValueObject *a,
                              ValueObject *b)
{
	return createBooleanValueObject(fabs((float)(getInteger(a)) - getFloat(b)) < FLT_EPSILON);
}

/**
 * Checks if an integer value is not equal to a decimal value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is not equal to \a b.
 */
ValueObject *opNeqIntegerFloat(ValueObject *a,
                               ValueObject *b)
{
	return createBooleanValueObject(fabs((float)(getInteger(a)) - getFloat(b)) > FLT_EPSILON);
}

/**
 * Checks if a decimal value is equal to an integer value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is equal to \a b.
 */
ValueObject *opEqFloatInteger(ValueObject *a,
                              ValueObject *b)
{
	return createBooleanValueObject(fabs(getFloat(a) - (float)(getInteger(b))) < FLT_EPSILON);
}

/**
 * Checks if a decimal value is not equal to an integer value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is not equal to \a b.
 */
ValueObject *opNeqFloatInteger(ValueObject *a,
                               ValueObject *b)
{
	return createBooleanValueObject(fabs(getFloat(a) - (float)(getInteger(b))) > FLT_EPSILON);
}

/**
 * Checks if a decimal value is equal to another decimal value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is equal to \a b.
 */
ValueObject *opEqFloatFloat(ValueObject *a,
                            ValueObject *b)
{
	return createBooleanValueObject(fabs(getFloat(a) - getFloat(b)) < FLT_EPSILON);
}

/**
 * Checks if a decimal value is not equal to another decimal value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is not equal to \a b.
 */
ValueObject *opNeqFloatFloat(ValueObject *a,
                             ValueObject *b)
{
	return createBooleanValueObject(fabs(getFloat(a) - getFloat(b)) > FLT_EPSILON);
}

/**
 * Checks if a boolean value is equal to another boolean value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is equal to \a b.
 */
ValueObject *opEqBooleanBoolean(ValueObject *a,
                                ValueObject *b)
{
	return createBooleanValueObject(getInteger(a) == getInteger(b));
}

/**
 * Checks if a boolean value is not equal to another boolean value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is not equal to \a b.
 */
ValueObject *opNeqBooleanBoolean(ValueObject *a,
                                 ValueObject *b)
{
	return createBooleanValueObject(getInteger(a) != getInteger(b));
}

/**
 * Checks if a string value is equal to another string value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is equal to \a b.
 */
ValueObject *opEqStringString(ValueObject *a,
                              ValueObject *b)
{
	return createBooleanValueObject(strcmp(getString(a), getString(b)) == 0);
}

/**
 * Checks if a string value is not equal to another string value.
 *
 * \param [in] a The first value to check.
 *
 * \param [in] b The second value to check.
 *
 * \return A pointer to a boolean value indicating if \a is not equal to \a b.
 */
ValueObject *opNeqStringString(ValueObject *a,
                               ValueObject *b)
{
	return createBooleanValueObject(strcmp(getString(a), getString(b)) != 0);
}

/**
 * Returns true because two nil values are always equal.
 *
 * \param [in] a Not used.
 *
 * \param [in] b Not used.
 *
 * \return A true boolean value.
 */
ValueObject *opEqNilNil(ValueObject *a,
                        ValueObject *b)
{
	a = NULL;
	b = NULL;
	return createBooleanValueObject(1);
}

/**
 * Returns false because two nil values are never not equal.
 *
 * \param [in] a Not used.
 *
 * \param [in] b Not used.
 *
 * \return A false boolean value.
 */
ValueObject *opNeqNilNil(ValueObject *a,
                         ValueObject *b)
{
	a = NULL;
	b = NULL;
	return createBooleanValueObject(0);
}

/*
 * A jump table for boolean operations.  The first index determines the
 * particular boolean operation to perform, the second index determines the type
 * of the first argument, and the third index determines the type of the second
 * object.
 */
static ValueObject *(*BoolOpJumpTable[2][5][5])(ValueObject *, ValueObject *) = {
	{ /* OP_EQ */
	              /* Integer, Float, Boolean, String, Nil */
	/* Integer */ { opEqIntegerInteger, opEqIntegerFloat, NULL, NULL, NULL },
	/* Float   */ { opEqFloatInteger, opEqFloatFloat, NULL, NULL, NULL },
	/* Boolean */ { NULL, NULL, opEqBooleanBoolean, NULL, NULL },
	/* String  */ { NULL, NULL, NULL, opEqStringString, NULL },
	/* Nil     */ { NULL, NULL, NULL, NULL, opEqNilNil }
	},
	{ /* OP_NEQ */
	              /* Integer, Float, Boolean, String, Nil */
	/* Integer */ { opNeqIntegerInteger, opNeqIntegerFloat, NULL, NULL, NULL },
	/* Float   */ { opNeqFloatInteger, opNeqFloatFloat, NULL, NULL, NULL },
	/* Boolean */ { NULL, NULL, opNeqBooleanBoolean, NULL, NULL },
	/* String  */ { NULL, NULL, NULL, opNeqStringString, NULL },
	/* Nil     */ { NULL, NULL, NULL, NULL, opNeqNilNil }
	}
};

/**
 * Interprets an equality operation.
 *
 * \param [in] expr The operation to interpret.
 *
 * \param [in] scope The scope to evaluate \a expr under.
 *
 * \return A pointer to the resulting value of the equality operation.
 *
 * \retval NULL An error occurred during interpretation.
 */
ValueObject *applyEqualityOp(OpType type,
                             ValueObject *val1,
                             ValueObject *val2,
                             ScopeObject *scope)
{
	(void)scope;
	/*
	 * Since there is no automatic casting, an equality (inequality) test
	 * against a non-number type will always fail (succeed).
	 */
	if ((val1->type != val2->type)
			&& ((val1->type != VT_INTEGER && val1->type != VT_FLOAT)
			|| (val2->type != VT_INTEGER && val2->type != VT_FLOAT))) {
		switch (type) {
			case OP_EQ:
				return createBooleanValueObject(0);
			case OP_NEQ:
				return createBooleanValueObject(1);
			default:
				error(IN_INVALID_EQUALITY_OPERATION_TYPE);
				return NULL;
		}
	}
	return BoolOpJumpTable[type - OP_EQ][val1->type][val2->type](val1, val2);
}

ValueObject *interpretEqualityOpExprNode(OpExprNode *expr,
                                         ScopeObject *scope)
{
	ValueObject *val1 = interpretExprNode(expr->args->exprs[0], scope);
	ValueObject *val2 = interpretExprNode(expr->args->exprs[1], scope);
	ValueObject *ret = NULL;
	if (!val1 || !val2) {
		deleteValueObject(val1);
		deleteValueObject(val2);
		return NULL;
	}
	ret = applyEqualityOp(expr->type, val1, val2, scope);
	deleteValueObject(val1);
	deleteValueObject(val2);
	return ret;
}

/**
 * Interprets a concatenation operation.
 *
 * \param [in] expr The operation to interpret.
 *
 * \param [in] scope The scope to evaluate \a expr under.
 *
 * \return A pointer to the resulting value of the concatenation operation.
 *
 * \retval NULL An error occurred during interpretation.
 */
/**
 * Joins values end to end as a string.
 *
 * \param [in] vals The values to join.
 *
 * \param [in] num The number of values.
 *
 * \param [in] scope The scope to cast the values under.
 *
 * \return A string value holding the values one after another.
 *
 * \retval NULL A value could not be cast to a string.
 */
ValueObject *concatValues(ValueObject **vals,
                          unsigned int num,
                          ScopeObject *scope)
{
	unsigned int n;
	size_t len = 0;
	char *acc = malloc(1);

	if (!acc) {
		perror("malloc");
		return NULL;
	}
	acc[0] = '\0';

	for (n = 0; n < num; n++) {
		ValueObject *use = castStringImplicit(vals[n], scope);
		size_t add;
		void *mem;
		if (!use) {
			free(acc);
			return NULL;
		}
		add = strlen(getString(use));
		mem = realloc(acc, len + add + 1);
		if (!mem) {
			perror("realloc");
			deleteValueObject(use);
			free(acc);
			return NULL;
		}
		acc = mem;
		memcpy(acc + len, getString(use), add + 1);
		len += add;
		deleteValueObject(use);
	}
	return createStringValueObject(acc);
}

ValueObject *interpretConcatOpExprNode(OpExprNode *expr,
                                       ScopeObject *scope)
{
	unsigned int n;
	/* Start out with the first string to concatenate. */
	ValueObject *val = interpretExprNode(expr->args->exprs[0], scope);
	ValueObject *use = castStringImplicit(val, scope);
	char *acc = NULL;
	void *mem = NULL;
	if (!val || !use) {
		deleteValueObject(val);
		deleteValueObject(use);
		return NULL;
	}
	/* Start out an accumulator with the first string. */
	mem = realloc(acc, sizeof(char) * (strlen(getString(use)) + 1));
	if (!mem) {
		perror("realloc");
		deleteValueObject(val);
		deleteValueObject(use);
		free(acc);
		return NULL;
	}
	acc = mem;
	acc[0] = '\0';
	strcat(acc, getString(use));
	deleteValueObject(val);
	deleteValueObject(use);
	for (n = 1; n < expr->args->num; n++) {
		/* Grab the next string to concatenate. */
		val = interpretExprNode(expr->args->exprs[n], scope);
		use = castStringImplicit(val, scope);
		if (!val || !use) {
			deleteValueObject(val);
			deleteValueObject(use);
			free(acc);
			return NULL;
		}
		/* Add the next string to the accumulator. */
		mem = realloc(acc, sizeof(char) * (strlen(acc) + strlen(getString(use)) + 1));
		if (!mem) {
			perror("realloc");
			deleteValueObject(val);
			deleteValueObject(use);
			free(acc);
			return NULL;
		}
		acc = mem;
		strcat(acc, getString(use));
		deleteValueObject(val);
		deleteValueObject(use);
	}
	return createStringValueObject(acc);
}

/*
 * A jump table for operations.  The index of a function in the table is given
 * by its its index in the enumerated OpType type.
 */
static ValueObject *(*OpExprJumpTable[14])(OpExprNode *, ScopeObject *) = {
	interpretArithOpExprNode,
	interpretArithOpExprNode,
	interpretArithOpExprNode,
	interpretArithOpExprNode,
	interpretArithOpExprNode,
	interpretArithOpExprNode,
	interpretArithOpExprNode,
	interpretBoolOpExprNode,
	interpretBoolOpExprNode,
	interpretBoolOpExprNode,
	interpretNotOpExprNode,
	interpretEqualityOpExprNode,
	interpretEqualityOpExprNode,
	interpretConcatOpExprNode };

/**
 * Interprets an operation.
 *
 * \param [in] node The operation to interpret.
 *
 * \param [in] scope The scope to evaluate \a expr under.
 *
 * \return A pointer to the resulting value of the operation.
 *
 * \retval NULL An error occurred during interpretation.
 */
ValueObject *interpretOpExprNode(ExprNode *node,
                                 ScopeObject *scope)
{
	OpExprNode *expr = (OpExprNode *)node->expr;
	return OpExprJumpTable[expr->type](expr, scope);
}

/*
 * A jump table for expressions.  The index of a function in the table is given
 * by its its index in the enumerated ExprType type.
 */
static ValueObject *(*ExprJumpTable[6])(ExprNode *, ScopeObject *) = {
	interpretCastExprNode,
	interpretConstantExprNode,
	interpretIdentifierExprNode,
	interpretFuncCallExprNode,
	interpretOpExprNode,
	interpretImpVarExprNode };

/**
 * Interprets an expression.
 *
 * \param [in] node The expression to interpret.
 *
 * \param [in] scope The scope to evaluate \a expr under.
 *
 * \return A pointer to the value of \a expr evaluated under \a scope.
 *
 * \retval NULL An error occurred during interpretation.
 */
ValueObject *interpretExprNode(ExprNode *node,
                               ScopeObject *scope)
{
	return ExprJumpTable[node->type](node, scope);
}

/**
 * Interprets a cast statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createCastStmtNode().
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretCastStmtNode(StmtNode *node,
                                    ScopeObject *scope)
{
	CastStmtNode *stmt = (CastStmtNode *)node->stmt;
	ValueObject *val = getScopeValue(scope, scope, stmt->target);
	ValueObject *cast = NULL;
	if (!val) {
		IdentifierNode *id = (IdentifierNode *)(stmt->target);
		char *name = resolveIdentifierName(id, scope);
		if (name) {
			error(IN_VARIABLE_DOES_NOT_EXIST, id->fname, id->line, name);
			free(name);
		}
		return NULL;
	}
	switch(stmt->newtype->type) {
		case CT_NIL:
			if (!(cast = createNilValueObject())) return NULL;
			break;
		case CT_BOOLEAN:
			if (!(cast = castBooleanExplicit(val, scope))) return NULL;
			break;
		case CT_INTEGER:
			if (!(cast = castIntegerExplicit(val, scope))) return NULL;
			break;
		case CT_FLOAT:
			if (!(cast = castFloatExplicit(val, scope))) return NULL;
			break;
		case CT_STRING:
			if (!(cast = castStringExplicit(val, scope))) return NULL;
			break;
		case CT_ARRAY: {
			IdentifierNode *id = (IdentifierNode *)(stmt->target);
			char *name = resolveIdentifierName(id, scope);
			if (name) {
				error(IN_CANNOT_CAST_VALUE_TO_ARRAY, id->fname, id->line, name);
				free(name);
			}
			return NULL;
			break;
		}
	}
	if (!updateScopeValue(scope, scope, stmt->target, cast)) {
		deleteValueObject(cast);
		return NULL;
	}
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets a print statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createPrintStmtNode().
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretPrintStmtNode(StmtNode *node,
                                     ScopeObject *scope)
{
	PrintStmtNode *stmt = (PrintStmtNode *)node->stmt;
	unsigned int n;
	for (n = 0; n < stmt->args->num; n++) {
		ValueObject *val = interpretExprNode(stmt->args->exprs[n], scope);
		ValueObject *use = castStringImplicit(val, scope);
		if (!val || !use) {
			deleteValueObject(val);
			deleteValueObject(use);
			return NULL;
		}
		fprintf(stmt->file, "%s", getString(use));
		deleteValueObject(val);
		deleteValueObject(use);
	}
	if (!stmt->nonl)
		putc('\n', stmt->file);
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets an input statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createInputStmtNode().
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
/**
 * Reads one line from standard input.
 *
 * \note The specification is unclear as to the exact semantics of input.
 * Here, we read up until the first newline or EOF but do not store it.
 *
 * \return The line read, as a string value.
 *
 * \retval NULL Memory allocation failed.
 */
ValueObject *readLineValue(void)
{
	unsigned int size = 16;
	unsigned int cur = 0;
	char *temp = malloc(sizeof(char) * size);
	int c;
	void *mem = NULL;
	ValueObject *val = NULL;

	if (!temp) {
		perror("malloc");
		return NULL;
	}
	while ((c = getchar()) && !feof(stdin)) {
		if (c == EOF || c == (int)'\r' || c == (int)'\n') break;
		temp[cur] = (char)c;
		cur++;
		if (cur > size - 1) {
			/* Increasing buffer size. */
			size *= 2;
			mem = realloc(temp, sizeof(char) * size);
			if (!mem) {
				perror("realloc");
				free(temp);
				return NULL;
			}
			temp = mem;
		}
	}
	temp[cur] = '\0';
	val = createStringValueObject(temp);
	if (!val) {
		free(temp);
		return NULL;
	}
	return val;
}

ReturnObject *interpretInputStmtNode(StmtNode *node,
                                     ScopeObject *scope)
{
	InputStmtNode *stmt = (InputStmtNode *)node->stmt;
	ValueObject *val = readLineValue();
	if (!val) return NULL;
	if (!updateScopeValue(scope, scope, stmt->target, val)) {
		deleteValueObject(val);
		return NULL;
	}
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets an assignment statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createAssignmentStmtNode().
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretAssignmentStmtNode(StmtNode *node,
                                          ScopeObject *scope)
{
	AssignmentStmtNode *stmt = (AssignmentStmtNode *)node->stmt;
	ValueObject *val = interpretExprNode(stmt->expr, scope);
	if (!val) return NULL;
	/* interpolate assigned strings */
	if (val->type == VT_STRING) {
		ValueObject *use = castStringImplicit(val, scope);
		deleteValueObject(val);
		if (!use) return NULL;
		val = use;
	}
	if (!updateScopeValue(scope, scope, stmt->target, val)) {
		deleteValueObject(val);
		return NULL;
	}
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets a declaration statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createDeclarationStmtNode().
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretDeclarationStmtNode(StmtNode *node,
                                           ScopeObject *scope)
{
	DeclarationStmtNode *stmt = (DeclarationStmtNode *)node->stmt;
	ValueObject *init = NULL;
	ScopeObject *dest = NULL;
	dest = getScopeObject(scope, scope, stmt->scope);
	if (!dest) return NULL;
	if (getScopeValueLocal(scope, dest, stmt->target)) {
		IdentifierNode *id = (IdentifierNode *)(stmt->target);
		char *name = resolveIdentifierName(id, scope);
		if (name) {
			error(IN_REDEFINITION_OF_VARIABLE, id->fname, id->line, name);
			free(name);
		}
		return NULL;
	}
	if (stmt->expr)
		init = interpretExprNode(stmt->expr, scope);
	else if (stmt->type) {
		switch (stmt->type->type) {
			case CT_NIL:
				init = createNilValueObject();
				break;
			case CT_BOOLEAN:
				init = createBooleanValueObject(0);
				break;
			case CT_INTEGER:
				init = createIntegerValueObject(0);
				break;
			case CT_FLOAT:
				init = createFloatValueObject(0.0);
				break;
			case CT_STRING:
				init = createStringValueObject(copyString(""));
				break;
			case CT_ARRAY:
				init = createArrayValueObject(scope);
				break;
			default:
				error(IN_INVALID_DECLARATION_TYPE);
				return NULL;
		}
	}
	else if (stmt->parent) {
		ScopeObject *parent = getScopeObject(scope, scope, stmt->parent);
		if (!parent) return NULL;
		init = createArrayValueObject(parent);
	}
	else
		init = createNilValueObject();
	if (!init) return NULL;
	if (!createScopeValue(scope, dest, stmt->target)) {
		deleteValueObject(init);
		return NULL;
	}
	if (!updateScopeValue(scope, dest, stmt->target, init)) {
		deleteValueObject(init);
		return NULL;
	}
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets an if/then/else statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createIfThenElseStmtNode().
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretIfThenElseStmtNode(StmtNode *node,
                                          ScopeObject *scope)
{
	IfThenElseStmtNode *stmt = (IfThenElseStmtNode *)node->stmt;
	ValueObject *use1 = scope->impvar;
	int use1val;
	unsigned int cast1 = 0;
	BlockNode *path = NULL;
	if (scope->impvar->type != VT_BOOLEAN && scope->impvar->type != VT_INTEGER) {
		use1 = castBooleanImplicit(scope->impvar, scope);
		if (!use1) return NULL;
		cast1 = 1;
	}
	use1val = getInteger(use1);
	if (cast1) deleteValueObject(use1);
	/* Determine which block of code to execute */
	if (use1val)
		path = stmt->yes;
	else {
		unsigned int n;
		for (n = 0; n < stmt->guards->num; n++) {
			ValueObject *val = interpretExprNode(stmt->guards->exprs[n], scope);
			ValueObject *use2 = val;
			int use2val;
			unsigned int cast2 = 0;
			if (!val) return NULL;
			if (val->type != VT_BOOLEAN && val->type != VT_INTEGER) {
				use2 = castBooleanImplicit(val, scope);
				if (!use2) {
					deleteValueObject(val);
					return NULL;
				}
				cast2 = 1;
			}
			use2val = getInteger(use2);
			deleteValueObject(val);
			if (cast2) deleteValueObject(use2);
			if (use2val) {
				path = stmt->blocks->blocks[n];
				break;
			}
		}
		/* Reached the end without satisfying any guard */
		if (n == stmt->guards->num)
			path = stmt->no;
	}
	/* Interpret a path if one was reached */
	if (path) {
		ReturnObject *r = interpretBlockNode(path, scope);
		if (!r)
			return NULL;
		/* Pass this up to the outer block to handle. */
		else if (r->type == RT_BREAK || r->type == RT_RETURN)
			return r;
		else
			deleteReturnObject(r);
	}
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets a switch statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createSwitchStmtNode().
 *
 * \note The specification is unclear as to whether guards are implicitly cast
 * to the type of the implicit variable.  This only matters in the case that
 * mixed guard types are present, and in this code, the action that is performed
 * is the same as the comparison operator, that is, in order for a guard to
 * match, both its type and value must match the implicit variable.
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
/**
 * Compares a value against a switch guard.
 *
 * A switch does not cast: a guard only matches a value of its own type, and
 * nil never matches anything.
 *
 * \param [in] use1 The value being switched on.
 *
 * \param [in] use2 The guard to compare it against.
 *
 * \retval 1 They match.
 * \retval 0 They do not match.
 * \retval -1 The value has a type a switch cannot compare.
 */
int switchMatches(ValueObject *use1, ValueObject *use2)
{
	if (use1->type != use2->type) return 0;
	switch (use1->type) {
		case VT_NIL:
			return 0;
		case VT_BOOLEAN:
		case VT_INTEGER:
			return getInteger(use1) == getInteger(use2);
		case VT_FLOAT:
			return fabs(getFloat(use1) - getFloat(use2)) < FLT_EPSILON;
		case VT_STRING:
			/**
			 * \note Strings with interpolation should have already
			 * been checked for.
			 */
			return !strcmp(getString(use1), getString(use2));
		default:
			return -1;
	}
}

ReturnObject *interpretSwitchStmtNode(StmtNode *node,
                                      ScopeObject *scope)
{
	SwitchStmtNode *stmt = (SwitchStmtNode *)node->stmt;
	unsigned int n;
	/*
	 * Loop over each of the guards, checking if any match the implicit
	 * variable.
	 */
	for (n = 0; n < stmt->guards->num; n++) {
		ValueObject *use1 = scope->impvar;
		ValueObject *use2 = interpretExprNode(stmt->guards->exprs[n], scope);
		unsigned int done = 0;
		if (!use2) return NULL;
		{
			int m = switchMatches(use1, use2);
			if (m < 0) {
				error(IN_INVALID_TYPE);
				deleteValueObject(use2);
				return NULL;
			}
			done = (unsigned int)m;
		}
		deleteValueObject(use2);
		if (done) break;
	}
	/* If none of the guards match and a default block exists */
	if (n == stmt->blocks->num && stmt->def) {
		ReturnObject *r = interpretBlockNode(stmt->def, scope);
		if (!r)
			return NULL;
		else if (r->type == RT_RETURN)
			return r;
		else
			deleteReturnObject(r);
	}
	else {
		/*
		 * Keep interpreting blocks starting at n until a break or
		 * return is encountered.
		 */
		for (; n < stmt->blocks->num; n++) {
			ReturnObject *r = interpretBlockNode(stmt->blocks->blocks[n], scope);
			if (!r)
				return NULL;
			else if (r->type == RT_BREAK) {
				deleteReturnObject(r);
				break;
			}
			else if (r->type == RT_RETURN)
				return r;
			else
				deleteReturnObject(r);
		}
	}
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets a break statement.
 *
 * \param [in] node Not used (see note).
 *
 * \param [in] scope Not used (see note).
 *
 * \pre \a node contains a statement created by createStmtNode() with arguments
 * ST_BREAK and NULL.
 *
 * \note \a node and \a scope are not used by this function but are still
 * included in its prototype to allow this function to be stored in a jump table
 * for fast execution.
 *
 * \return A pointer to a break return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretBreakStmtNode(StmtNode *node,
                                     ScopeObject *scope)
{
	node = NULL;
	scope = NULL;
	return createReturnObject(RT_BREAK, NULL);
}

/**
 * Interprets a return statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createReturnStmtNode().
 *
 * \return A pointer to a return value of \a node interpreted under \a scope.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretReturnStmtNode(StmtNode *node,
                                      ScopeObject *scope)
{
	/* Evaluate and return the expression. */
	ReturnStmtNode *stmt = (ReturnStmtNode *)node->stmt;
	ValueObject *value = interpretExprNode(stmt->value, scope);
	if (!value) return NULL;
	return createReturnObject(RT_RETURN, value);
}

/**
 * Interprets a loop statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createLoopStmtNode().
 *
 * \return A pointer to a return value of \a node interpreted under \a scope.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretLoopStmtNode(StmtNode *node,
                                    ScopeObject *scope)
{
	LoopStmtNode *stmt = (LoopStmtNode *)node->stmt;
	ScopeObject *outer = createScopeObject(scope);
	ValueObject *var = NULL;
	if (!outer) return NULL;
	/* Create a temporary loop variable if required */
	if (stmt->var) {
		var = createScopeValue(scope, outer, stmt->var);
		if (!var) {
			deleteScopeObject(outer);
			return NULL;
		}
		var->type = VT_INTEGER;
		var->data.i = 0;
		var->semaphore = 1;
	}
	while (1) {
		if (stmt->guard) {
			ValueObject *val = interpretExprNode(stmt->guard, outer);
			ValueObject *use = val;
			unsigned short cast = 0;
			int guardval;
			if (val->type != VT_BOOLEAN && val->type != VT_INTEGER) {
				use = castBooleanImplicit(val, scope);
				if (!use) {
					deleteScopeObject(outer);
					deleteValueObject(val);
					return NULL;
				}
				cast = 1;
			}
			guardval = getInteger(use);
			if (cast) deleteValueObject(use);
			deleteValueObject(val);
			if (guardval == 0) break;
		}
		if (stmt->body) {
			ReturnObject *result = interpretBlockNode(stmt->body, outer);
			if (!result) {
				deleteScopeObject(outer);
				return NULL;
			}
			else if (result->type == RT_BREAK) {
				deleteReturnObject(result);
				break;
			}
			else if (result->type == RT_RETURN) {
				deleteScopeObject(outer);
				return result;
			}
			else
				deleteReturnObject(result);
		}
		if (stmt->update) {
			/*
			 * A little efficiency hack: if we know the operation to
			 * perform, don't bother evaluating the ExprNode
			 * structure, just go ahead and do it to the loop
			 * variable.
			 */
			if (stmt->update->type == ET_OP) {
				ValueObject *updated = NULL;
				var = getScopeValue(scope, outer, stmt->var);
				OpExprNode *op = (OpExprNode *)stmt->update->expr;
				if (op->type == OP_ADD)
					updated = createIntegerValueObject(var->data.i + 1);
				else if (op->type == OP_SUB)
					updated = createIntegerValueObject(var->data.i - 1);

				if (!updateScopeValue(scope, outer, stmt->var, updated)) {
					deleteValueObject(updated);
					deleteScopeObject(outer);
					return NULL;
				}
			}
			else {
				ValueObject *update = interpretExprNode(stmt->update, outer);
				if (!update) {
					deleteScopeObject(outer);
					return NULL;
				}
				if (!updateScopeValue(scope, outer, stmt->var, update)) {
					deleteScopeObject(outer);
					deleteValueObject(update);
					return NULL;
				}
			}
		}
	}
	deleteScopeObject(outer);
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets a deallocation statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createDeallocationStmtNode().
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretDeallocationStmtNode(StmtNode *node,
                                            ScopeObject *scope)
{
	DeallocationStmtNode *stmt = (DeallocationStmtNode *)node->stmt;
	if (!updateScopeValue(scope, scope, stmt->target, NULL)) return NULL;
	/* If we want to completely remove the variable, use:
	deleteScopeValue(scope, stmt->target);
	*/
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets a function definition statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createFuncDefStmtNode().
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretFuncDefStmtNode(StmtNode *node,
                                       ScopeObject *scope)
{
	/* Add the function to the current scope */
	FuncDefStmtNode *stmt = (FuncDefStmtNode *)node->stmt;
	ValueObject *init = NULL;
	ScopeObject *dest = NULL;

	dest = getScopeObject(scope, scope, stmt->scope);
	if (!dest) return NULL;
	if (getScopeValueLocal(scope, dest, stmt->name)) {
		IdentifierNode *id = (IdentifierNode *)(stmt->name);
		char *name = resolveIdentifierName(id, scope);
		if (name) {
			error(IN_FUNCTION_NAME_USED_BY_VARIABLE, id->fname, id->line, name);
			free(name);
		}
		return NULL;
	}
	init = createFunctionValueObject(stmt);
	if (!init) return NULL;
	if (!createScopeValue(scope, dest, stmt->name)) {
		deleteValueObject(init);
		return NULL;
	}
	if (!updateScopeValue(scope, dest, stmt->name, init)) {
		deleteValueObject(init);
		return NULL;
	}
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets an expression statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createExprNode().
 *
 * \post The implicit variable of \a scope will be set the the value of \a node
 * evaluated under \a scope.
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretExprStmtNode(StmtNode *node,
                                    ScopeObject *scope)
{
	/* Set the implicit variable to the result of the expression */
	ExprNode *expr = (ExprNode *)node->stmt;
	deleteValueObject(scope->impvar);
	scope->impvar = interpretExprNode(expr, scope);
	if (!scope->impvar) return NULL;
	return createReturnObject(RT_DEFAULT, NULL);
}

/**
 * Interprets an alternate array definition statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by createAltArrayDefNode().
 *
 * \return A pointer to a default return value.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretAltArrayDefStmtNode(StmtNode *node,
                                           ScopeObject *scope)
{
	AltArrayDefStmtNode *stmt = (AltArrayDefStmtNode *)node->stmt;
	ValueObject *init = NULL;
	ScopeObject *dest = scope;
	ReturnObject *ret = NULL;
	if (getScopeValueLocal(scope, dest, stmt->name)) {
		IdentifierNode *id = (IdentifierNode *)(stmt->name);
		char *name = resolveIdentifierName(id, scope);
		if (name) {
			fprintf(stderr, "%s:%u: redefinition of existing variable at: %s\n", id->fname, id->line, name);
			free(name);
		}
		return NULL;
	}
	if (stmt->parent) {
		ScopeObject *parent = getScopeObject(scope, scope, stmt->parent);
		if (!parent) return NULL;
		init = createArrayValueObject(parent);
	}
	else {
		init = createArrayValueObject(scope);
	}
	if (!init) return NULL;

	/* Populate the array body */
	ret = interpretStmtNodeList(stmt->body->stmts, getArray(init));
	if (!ret) {
		deleteValueObject(init);
		return NULL;
	}
	deleteReturnObject(ret);
	if (!createScopeValue(scope, dest, stmt->name)) {
		deleteValueObject(init);
		return NULL;
	}
	if (!updateScopeValue(scope, dest, stmt->name, init)) {
		deleteValueObject(init);
		return NULL;
	}
	return createReturnObject(RT_DEFAULT, NULL);
}

/*
 * A jump table for statements.  The index of a function in the table is given
 * by its its index in the enumerated StmtType type.
 */
static ReturnObject *(*StmtJumpTable[14])(StmtNode *, ScopeObject *) = {
	interpretCastStmtNode,
	interpretPrintStmtNode,
	interpretInputStmtNode,
	interpretAssignmentStmtNode,
	interpretDeclarationStmtNode,
	interpretIfThenElseStmtNode,
	interpretSwitchStmtNode,
	interpretBreakStmtNode,
	interpretReturnStmtNode,
	interpretLoopStmtNode,
	interpretDeallocationStmtNode,
	interpretFuncDefStmtNode,
	interpretExprStmtNode,
	interpretAltArrayDefStmtNode };

/**
 * Interprets a statement.
 *
 * \param [in] node The statement to interpret.
 *
 * \param [in] scope The scope to evaluate \a node under.
 *
 * \pre \a node contains a statement created by parseStmtNode().
 *
 * \return A pointer to a return value set appropriately depending on the
 * statement interpreted.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretStmtNode(StmtNode *node,
                                ScopeObject *scope)
{
	return StmtJumpTable[node->type](node, scope);
}

/**
 * Interprets a list of statements.
 *
 * \param [in] list The statements to interpret.
 *
 * \param [in] scope The scope to evaluate \a list under.
 *
 * \return A pointer to a return value set appropriately depending on the
 * statements interpreted.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretStmtNodeList(StmtNodeList *list,
                                    ScopeObject *scope)
{
	ReturnObject *ret = NULL;
	unsigned int n;
	for (n = 0; n < list->num; n++) {
		ret = interpretStmtNode(list->stmts[n], scope);
		if (!ret)
			return NULL;
		else if (ret->type == RT_BREAK || ret->type == RT_RETURN)
			return ret;
		else {
			deleteReturnObject(ret);
			ret = NULL;
		}
	}
	if (!ret) ret = createReturnObject(RT_DEFAULT, NULL);
	return ret;
}

/**
 * Interprets a block of code.
 *
 * \param [in] node The block of code to interpret.
 *
 * \param [in] scope The scope to evaluate \a block under.
 *
 * \pre \a block contains a block of code created by parseBlockNode().
 *
 * \return A pointer to a return value set appropriately depending on the
 * statements interpreted.
 *
 * \retval NULL An error occurred during interpretation.
 */
ReturnObject *interpretBlockNode(BlockNode *node,
                                 ScopeObject *scope)
{
	ReturnObject *ret = NULL;
	ScopeObject *inner = createScopeObject(scope);
	if (!inner) return NULL;
	ret = interpretStmtNodeList(node->stmts, inner);
	deleteScopeObject(inner);
	return ret;
}

/**
 * Interprets a the main block of code.
 *
 * \param [in] main The main block of code to interpret.
 *
 * \pre \a main contains a block of code created by parseMainNode().
 *
 * \return The final status of the program.
 *
 * \retval 0 \a main was interpreted without any errors.
 *
 * \retval 1 An error occurred while interpreting \a main.
 */
int interpretMainNode(MainNode *main)
{
	ReturnObject *ret = NULL;
	Proto *proto;
	if (!main) return 1;

	compileProgram(main);

	proto = getMainProto();
	if (proto) {
		ValueObject *val = callProto(proto, NULL, 0, NULL, NULL);
		if (!val) return 1;
		deleteValueObject(val);
		return 0;
	}

	ret = interpretBlockNode(main->block, NULL);
       	if (!ret) return 1;
	deleteReturnObject(ret);
	return 0;
}
