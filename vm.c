/**
 * \file vm.c
 *
 * \author Justin J. Meza
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "vm.h"
#include "intern.h"
#include "error.h"
#include "jit.h"

/**
 * \name Name escape analysis
 *
 * LOLCODE is dynamically scoped: a function called with \c I \c IZ runs with
 * its caller's scope as its parent, so a callee can read its caller's
 * variables.  A local can therefore only be kept in a register if no procedure
 * anywhere in the program refers to that name without declaring it first.
 *
 * This pass collects, for the whole program, the set of names that are used
 * somewhere they are not declared.  Anything in that set has to live in a real
 * scope object; everything else is safe to keep in a register.
 */
/**@{*/

/**
 * A growable set of interned names.
 */
typedef struct {
	const Name **items; /**< The names in the set. */
	unsigned int num;   /**< The number of names. */
	unsigned int cap;   /**< The allocated length of \a items. */
} NameSet;

static void initNameSet(NameSet *set)
{
	set->items = NULL;
	set->num = 0;
	set->cap = 0;
}

static void freeNameSet(NameSet *set)
{
	free(set->items);
	initNameSet(set);
}

static int inNameSet(const NameSet *set, const Name *name)
{
	unsigned int n;
	for (n = 0; n < set->num; n++)
		if (set->items[n] == name) return 1;
	return 0;
}

static void addNameSet(NameSet *set, const Name *name)
{
	if (!name || inNameSet(set, name)) return;
	if (set->num == set->cap) {
		unsigned int newcap = set->cap ? set->cap * 2 : 8;
		void *mem = realloc(set->items, sizeof(Name *) * newcap);
		if (!mem) {
			perror("realloc");
			return;
		}
		set->items = mem;
		set->cap = newcap;
	}
	set->items[set->num++] = name;
}

/**
 * Names that must live in a scope object because something reads or writes
 * them from outside the procedure that declares them.
 */
static NameSet escaping;

/**
 * Set when the program contains something that defeats the analysis entirely,
 * such as a computed identifier.  Registers are disabled outright.
 */
static int analysisPoisoned = 0;

/**
 * Names that are bound by something other than a single function definition.
 * A call to a name outside this set always reaches the same function, which is
 * what lets generated code branch to it directly.
 */
static NameSet rebound;

/**
 * The function definitions seen, indexed alongside \a funcnames.
 */
static const Name **funcnames = NULL;
static FuncDefStmtNode **funcdefs = NULL;
static unsigned int numfuncs = 0;
static unsigned int capfuncs = 0;

/**
 * Records that \a name is bound to something.
 */
static void noteBinding(IdentifierNode *id)
{
	if (id && id->type == IT_DIRECT) addNameSet(&rebound, id->iname);
}

/**
 * The set of names declared by the procedure currently being scanned.
 */
static NameSet declaredHere;

static void scanBlockDeclare(BlockNode *block);
static void scanBlockUse(BlockNode *block);

/**
 * Records an identifier as used, marking it escaping if the procedure being
 * scanned does not declare it.
 */
static void useIdentifier(IdentifierNode *id)
{
	if (!id) return;
	if (id->type != IT_DIRECT) {
		/* A computed name could reach anything at all. */
		analysisPoisoned = 1;
		return;
	}
	if (!inNameSet(&declaredHere, id->iname))
		addNameSet(&escaping, id->iname);
	/* A slot access reaches into a bukkit's own scope, which is a real
	 * scope object; the names along the path cannot be registers. */
	if (id->slot) {
		IdentifierNode *s;
		addNameSet(&escaping, id->iname);
		for (s = id->slot; s; s = s->slot) {
			if (s->type != IT_DIRECT) analysisPoisoned = 1;
			else addNameSet(&escaping, s->iname);
		}
	}
}

/**
 * Records an identifier as declared by the procedure being scanned.
 */
static void declareIdentifier(IdentifierNode *id)
{
	if (!id) return;
	if (id->type != IT_DIRECT || id->slot) {
		analysisPoisoned = 1;
		return;
	}
	addNameSet(&declaredHere, id->iname);
}

/**
 * Whether the analysis below understands a statement.
 *
 * A node type it has never seen could read or write a variable in a way it
 * cannot account for, so meeting one has to disable register allocation rather
 * than be passed over.  Adding a statement to the language means adding it here
 * and to the scans, or programs using it quietly keep their variables in scope
 * objects.
 */
static int knownStmtType(StmtType t)
{
	switch (t) {
		case ST_CAST: case ST_PRINT: case ST_INPUT: case ST_ASSIGNMENT:
		case ST_DECLARATION: case ST_IFTHENELSE: case ST_SWITCH:
		case ST_BREAK: case ST_RETURN: case ST_LOOP: case ST_DEALLOCATION:
		case ST_FUNCDEF: case ST_EXPR: case ST_ALTARRAYDEF:
			return 1;
		default:
			return 0;
	}
}

/**
 * Whether the analysis below understands an expression.
 */
static int knownExprType(ExprType t)
{
	switch (t) {
		case ET_CAST: case ET_CONSTANT: case ET_IDENTIFIER:
		case ET_FUNCCALL: case ET_OP: case ET_IMPVAR:
			return 1;
		default:
			return 0;
	}
}

/**
 * A string that interpolates another variable has to be able to find it by
 * name at run time.
 */
static void scanConstant(ConstantNode *c)
{
	if (c && c->type == CT_STRING && c->data.s && strstr(c->data.s, ":{"))
		analysisPoisoned = 1;
}

static void scanExprUse(ExprNode *expr)
{
	if (!expr) return;
	if (!knownExprType(expr->type)) {
		analysisPoisoned = 1;
		return;
	}
	switch (expr->type) {
		case ET_CONSTANT:
			scanConstant((ConstantNode *)expr->expr);
			break;
		case ET_IDENTIFIER:
			useIdentifier((IdentifierNode *)expr->expr);
			break;
		case ET_CAST:
			scanExprUse(((CastExprNode *)expr->expr)->target);
			break;
		case ET_FUNCCALL: {
			FuncCallExprNode *e = (FuncCallExprNode *)expr->expr;
			unsigned int n;
			/* The scope identifier names a bukkit or the special
			 * names I and ME, none of which can be a register. */
			if (e->scope && e->scope->type == IT_DIRECT)
				addNameSet(&escaping, e->scope->iname);
			else if (e->scope)
				analysisPoisoned = 1;
			if (e->name && e->name->type == IT_DIRECT)
				addNameSet(&escaping, e->name->iname);
			else if (e->name)
				analysisPoisoned = 1;
			for (n = 0; n < e->args->num; n++)
				scanExprUse(e->args->exprs[n]);
			break;
		}
		case ET_OP: {
			OpExprNode *e = (OpExprNode *)expr->expr;
			unsigned int n;
			for (n = 0; n < e->args->num; n++)
				scanExprUse(e->args->exprs[n]);
			break;
		}
		default:
			break;
	}
}

/**
 * Collects the names a block declares, without descending into functions.
 */
static void scanStmtDeclare(StmtNode *node)
{
	if (!knownStmtType(node->type)) {
		analysisPoisoned = 1;
		return;
	}
	switch (node->type) {
		case ST_DECLARATION: {
			DeclarationStmtNode *s = (DeclarationStmtNode *)node->stmt;
			/* A declaration qualified by a scope lands in that
			 * scope, not this one. */
			if (s->scope && s->scope->type == IT_DIRECT
					&& s->scope->iname == internName("I")
					&& !s->scope->slot)
				declareIdentifier(s->target);
			else
				analysisPoisoned = 1;
			break;
		}
		case ST_LOOP: {
			LoopStmtNode *s = (LoopStmtNode *)node->stmt;
			if (s->var) declareIdentifier(s->var);
			if (s->body) scanBlockDeclare(s->body);
			break;
		}
		case ST_IFTHENELSE: {
			IfThenElseStmtNode *s = (IfThenElseStmtNode *)node->stmt;
			unsigned int n;
			if (s->yes) scanBlockDeclare(s->yes);
			if (s->no) scanBlockDeclare(s->no);
			for (n = 0; n < s->blocks->num; n++)
				scanBlockDeclare(s->blocks->blocks[n]);
			break;
		}
		case ST_SWITCH: {
			SwitchStmtNode *s = (SwitchStmtNode *)node->stmt;
			unsigned int n;
			for (n = 0; n < s->blocks->num; n++)
				scanBlockDeclare(s->blocks->blocks[n]);
			if (s->def) scanBlockDeclare(s->def);
			break;
		}
		default:
			break;
	}
}

static void scanBlockDeclare(BlockNode *block)
{
	unsigned int n;
	if (!block) return;
	for (n = 0; n < block->stmts->num; n++)
		scanStmtDeclare(block->stmts->stmts[n]);
}

static void scanStmtUse(StmtNode *node)
{
	if (!knownStmtType(node->type)) {
		analysisPoisoned = 1;
		return;
	}
	switch (node->type) {
		case ST_CAST:
			useIdentifier(((CastStmtNode *)node->stmt)->target);
			noteBinding(((CastStmtNode *)node->stmt)->target);
			break;
		case ST_PRINT: {
			PrintStmtNode *s = (PrintStmtNode *)node->stmt;
			unsigned int n;
			for (n = 0; n < s->args->num; n++)
				scanExprUse(s->args->exprs[n]);
			break;
		}
		case ST_INPUT:
			useIdentifier(((InputStmtNode *)node->stmt)->target);
			noteBinding(((InputStmtNode *)node->stmt)->target);
			break;
		case ST_ASSIGNMENT: {
			AssignmentStmtNode *s = (AssignmentStmtNode *)node->stmt;
			useIdentifier(s->target);
			noteBinding(s->target);
			scanExprUse(s->expr);
			break;
		}
		case ST_DECLARATION: {
			DeclarationStmtNode *s = (DeclarationStmtNode *)node->stmt;
			scanExprUse(s->expr);
			noteBinding(s->target);
			if (s->parent) useIdentifier(s->parent);
			/* A bukkit keeps its members in a scope object. */
			if (s->type && s->type->type == CT_ARRAY)
				addNameSet(&escaping, s->target->type == IT_DIRECT
						? s->target->iname : NULL);
			break;
		}
		case ST_IFTHENELSE: {
			IfThenElseStmtNode *s = (IfThenElseStmtNode *)node->stmt;
			unsigned int n;
			if (s->yes) scanBlockUse(s->yes);
			if (s->no) scanBlockUse(s->no);
			for (n = 0; n < s->guards->num; n++)
				scanExprUse(s->guards->exprs[n]);
			for (n = 0; n < s->blocks->num; n++)
				scanBlockUse(s->blocks->blocks[n]);
			break;
		}
		case ST_SWITCH: {
			SwitchStmtNode *s = (SwitchStmtNode *)node->stmt;
			unsigned int n;
			for (n = 0; n < s->guards->num; n++)
				scanExprUse(s->guards->exprs[n]);
			for (n = 0; n < s->blocks->num; n++)
				scanBlockUse(s->blocks->blocks[n]);
			if (s->def) scanBlockUse(s->def);
			break;
		}
		case ST_RETURN:
			scanExprUse(((ReturnStmtNode *)node->stmt)->value);
			break;
		case ST_LOOP: {
			LoopStmtNode *s = (LoopStmtNode *)node->stmt;
			if (s->var) noteBinding(s->var);
			if (s->guard) scanExprUse(s->guard);
			if (s->update) scanExprUse(s->update);
			if (s->body) scanBlockUse(s->body);
			break;
		}
		case ST_DEALLOCATION:
			useIdentifier(((DeallocationStmtNode *)node->stmt)->target);
			noteBinding(((DeallocationStmtNode *)node->stmt)->target);
			break;
		case ST_EXPR:
			scanExprUse((ExprNode *)node->stmt);
			break;
		case ST_ALTARRAYDEF: {
			AltArrayDefStmtNode *s = (AltArrayDefStmtNode *)node->stmt;
			useIdentifier(s->name);
			if (s->name->type == IT_DIRECT)
				addNameSet(&escaping, s->name->iname);
			if (s->parent) useIdentifier(s->parent);
			analysisPoisoned = 1;
			break;
		}
		case ST_FUNCDEF:
			/* Scanned separately as its own procedure. */
			break;
		default:
			break;
	}
}

static void scanBlockUse(BlockNode *block)
{
	unsigned int n;
	if (!block) return;
	for (n = 0; n < block->stmts->num; n++)
		scanStmtUse(block->stmts->stmts[n]);
}

/**
 * Records a function definition, noting a duplicate name as a rebinding.
 */
static void noteFuncDef(FuncDefStmtNode *def)
{
	unsigned int n;
	if (!def->name || def->name->type != IT_DIRECT) return;
	for (n = 0; n < numfuncs; n++)
		if (funcnames[n] == def->name->iname) {
			addNameSet(&rebound, def->name->iname);
			return;
		}
	if (numfuncs == capfuncs) {
		unsigned int newcap = capfuncs ? capfuncs * 2 : 8;
		void *m1 = realloc(funcnames, sizeof(Name *) * newcap);
		void *m2 = realloc(funcdefs, sizeof(FuncDefStmtNode *) * newcap);
		if (!m1 || !m2) {
			perror("realloc");
			return;
		}
		funcnames = m1;
		funcdefs = m2;
		capfuncs = newcap;
	}
	funcnames[numfuncs] = def->name->iname;
	funcdefs[numfuncs] = def;
	numfuncs++;
}

/**
 * Finds the procedure a call to \a name always reaches.
 *
 * \retval NULL The name is bound in more than one place, so the target can
 * change while the program runs.
 */
Proto *resolveStaticCallee(const Name *name)
{
	unsigned int n;
	if (inNameSet(&rebound, name)) return NULL;
	for (n = 0; n < numfuncs; n++)
		if (funcnames[n] == name)
			return (Proto *)funcdefs[n]->proto;
	return NULL;
}

/**
 * Runs the analysis over one procedure.
 */
static void scanProcedure(IdentifierNodeList *args, BlockNode *body)
{
	unsigned int n;
	declaredHere.num = 0;
	if (args)
		for (n = 0; n < args->num; n++)
			declareIdentifier(args->ids[n]);
	scanBlockDeclare(body);
	scanBlockUse(body);
}

/**
 * Collects every function definition in a block, including nested ones.
 */
static void collectFuncDefs(BlockNode *block, FuncDefStmtNode ***out,
                            unsigned int *num, unsigned int *cap);

static void collectFuncDefsStmt(StmtNode *node, FuncDefStmtNode ***out,
                                unsigned int *num, unsigned int *cap)
{
	if (!knownStmtType(node->type)) {
		analysisPoisoned = 1;
		return;
	}
	switch (node->type) {
		case ST_FUNCDEF: {
			FuncDefStmtNode *def = (FuncDefStmtNode *)node->stmt;
			if (*num == *cap) {
				unsigned int newcap = *cap ? *cap * 2 : 8;
				void *mem = realloc(*out, sizeof(FuncDefStmtNode *) * newcap);
				if (!mem) {
					perror("realloc");
					return;
				}
				*out = mem;
				*cap = newcap;
			}
			(*out)[(*num)++] = def;
			collectFuncDefs(def->body, out, num, cap);
			break;
		}
		case ST_IFTHENELSE: {
			IfThenElseStmtNode *s = (IfThenElseStmtNode *)node->stmt;
			unsigned int n;
			collectFuncDefs(s->yes, out, num, cap);
			collectFuncDefs(s->no, out, num, cap);
			for (n = 0; n < s->blocks->num; n++)
				collectFuncDefs(s->blocks->blocks[n], out, num, cap);
			break;
		}
		case ST_SWITCH: {
			SwitchStmtNode *s = (SwitchStmtNode *)node->stmt;
			unsigned int n;
			for (n = 0; n < s->blocks->num; n++)
				collectFuncDefs(s->blocks->blocks[n], out, num, cap);
			collectFuncDefs(s->def, out, num, cap);
			break;
		}
		case ST_LOOP:
			collectFuncDefs(((LoopStmtNode *)node->stmt)->body, out, num, cap);
			break;
		default:
			break;
	}
}

static void collectFuncDefs(BlockNode *block, FuncDefStmtNode ***out,
                            unsigned int *num, unsigned int *cap)
{
	unsigned int n;
	if (!block) return;
	for (n = 0; n < block->stmts->num; n++)
		collectFuncDefsStmt(block->stmts->stmts[n], out, num, cap);
}
/**@}*/

/**
 * \name Compiler
 *
 * Lowers a procedure's parse tree into instructions over a register file.
 * Every expression is compiled into a caller supplied register; temporaries
 * come from a stack whose high water mark becomes the frame size.
 *
 * Compilation gives up the moment it meets something it cannot lower.  The
 * caller then leaves the procedure to the tree walking interpreter.
 */
/**@{*/

/**
 * Binds a name to a register for the extent of a block.
 */
typedef struct {
	const Name *name; /**< The name bound. */
	int reg;          /**< The register it lives in. */
	int depth;        /**< The block nesting depth of the binding. */
} Local;

/**
 * Compiler state for one procedure.
 */
typedef struct {
	Proto *proto;          /**< The procedure being built. */
	Instr *code;           /**< The instructions so far. */
	unsigned int numcode;  /**< The number of instructions. */
	unsigned int capcode;  /**< The allocated length of \a code. */
	ValueObject **k;       /**< The constants so far. */
	const Name **knames;   /**< Names, parallel to \a k. */
	void **kptrs;          /**< Parse tree pointers, parallel to \a k. */
	unsigned int numk;     /**< The number of constants. */
	unsigned int capk;     /**< The allocated length of the constant arrays. */
	Local *locals;         /**< The bindings currently in scope. */
	unsigned int numlocals;/**< The number of bindings. */
	unsigned int caplocals;/**< The allocated length of \a locals. */
	int nreg;              /**< The next free register. */
	int maxreg;            /**< The largest register used. */
	int depth;             /**< The current block nesting depth. */
	int itreg;             /**< The register holding the current block's IT. */
	int failed;            /**< Set when something could not be compiled. */
	int bailline;          /**< Where in this file compilation gave up. */
	int loopstart;         /**< Where \c GTFO jumps from, or -1 outside a loop. */
	int needscope;         /**< Set when the frame must own a scope object. */
	int scopedepth;        /**< How many block scopes are open here. */
	int *breaks;           /**< Pending break jumps to patch. */
	unsigned int numbreaks;/**< The number of pending breaks. */
	unsigned int capbreaks;/**< The allocated length of \a breaks. */
} Compiler;

static void compileBlock(Compiler *c, BlockNode *block, int newit);
static void compileExpr(Compiler *c, ExprNode *expr, int dest);
static int findLocal(Compiler *c, const Name *name);
static int isRegisterName(IdentifierNode *id);

/**
 * Compiles an expression and says which register holds the result.
 *
 * A variable already in a register, or the implicit variable, is used where it
 * stands rather than being copied into \a hint.  Every instruction reads its
 * operands before writing its destination, so sharing a register this way is
 * safe even when the destination is one of the operands.
 *
 * \return The register holding the value.
 */
static int compileExprAny(Compiler *c, ExprNode *expr, int hint)
{
	if (expr->type == ET_IDENTIFIER) {
		IdentifierNode *id = (IdentifierNode *)expr->expr;
		if (isRegisterName(id)) {
			int reg = findLocal(c, id->iname);
			if (reg >= 0) return reg;
		}
	}
	else if (expr->type == ET_IMPVAR) {
		return c->itreg;
	}
	compileExpr(c, expr, hint);
	return hint;
}

/**
 * Marks the procedure uncompilable, remembering where it gave up so that
 * \c LCI_DUMP can say why.
 */
static void bailAt(Compiler *c, int line)
{
	if (!c->failed) {
		c->failed = 1;
		c->bailline = line;
	}
}

#define bail(c) bailAt((c), __LINE__)

static int emit(Compiler *c, Opcode op, int a, int b, int cc)
{
	if (c->failed) return 0;
	if (c->numcode == c->capcode) {
		unsigned int newcap = c->capcode ? c->capcode * 2 : 64;
		void *mem = realloc(c->code, sizeof(Instr) * newcap);
		if (!mem) {
			perror("realloc");
			bail(c);
			return 0;
		}
		c->code = mem;
		c->capcode = newcap;
	}
	c->code[c->numcode].op = (unsigned char)op;
	c->code[c->numcode].a = (unsigned char)a;
	c->code[c->numcode].b = b;
	c->code[c->numcode].c = (short)cc;
	return (int)c->numcode++;
}

/**
 * Reserves \a n consecutive registers.
 */
static int allocReg(Compiler *c, int n)
{
	int base = c->nreg;
	c->nreg += n;
	if (c->nreg > c->maxreg) c->maxreg = c->nreg;
	if (c->nreg > 240) bail(c);
	return base;
}

static void freeReg(Compiler *c, int n)
{
	c->nreg -= n;
}

/**
 * Adds an entry to the constant table.
 *
 * \return The index of the entry.
 */
static int addConst(Compiler *c, ValueObject *val, const Name *name, void *ptr)
{
	if (c->numk == c->capk) {
		unsigned int newcap = c->capk ? c->capk * 2 : 16;
		void *m1 = realloc(c->k, sizeof(ValueObject *) * newcap);
		void *m2 = realloc(c->knames, sizeof(Name *) * newcap);
		void *m3 = realloc(c->kptrs, sizeof(void *) * newcap);
		if (!m1 || !m2 || !m3) {
			perror("realloc");
			bail(c);
			return 0;
		}
		c->k = m1;
		c->knames = m2;
		c->kptrs = m3;
		c->capk = newcap;
	}
	c->k[c->numk] = val;
	c->knames[c->numk] = name;
	c->kptrs[c->numk] = ptr;
	return (int)c->numk++;
}

/**
 * Finds the register a name is bound to.
 *
 * \retval -1 The name is not held in a register.
 */
static int findLocal(Compiler *c, const Name *name)
{
	unsigned int n;
	for (n = c->numlocals; n > 0; n--)
		if (c->locals[n - 1].name == name) return c->locals[n - 1].reg;
	return -1;
}

/**
 * Whether \a name is already bound at the current depth.
 */
static int isLocalHere(Compiler *c, const Name *name)
{
	unsigned int n;
	for (n = c->numlocals; n > 0; n--) {
		if (c->locals[n - 1].depth != c->depth) break;
		if (c->locals[n - 1].name == name) return 1;
	}
	return 0;
}

/**
 * Binds \a name to a fresh register at the current depth.
 */
static int addLocal(Compiler *c, const Name *name)
{
	int reg;
	if (c->numlocals == c->caplocals) {
		unsigned int newcap = c->caplocals ? c->caplocals * 2 : 8;
		void *mem = realloc(c->locals, sizeof(Local) * newcap);
		if (!mem) {
			perror("realloc");
			bail(c);
			return 0;
		}
		c->locals = mem;
		c->caplocals = newcap;
	}
	reg = allocReg(c, 1);
	c->locals[c->numlocals].name = name;
	c->locals[c->numlocals].reg = reg;
	c->locals[c->numlocals].depth = c->depth;
	c->numlocals++;
	return reg;
}

/**
 * Whether \a id can be held in a register rather than a scope object.
 */
static int isRegisterName(IdentifierNode *id)
{
	return !analysisPoisoned
			&& id
			&& id->type == IT_DIRECT
			&& !id->slot
			&& !inNameSet(&escaping, id->iname);
}

/**
 * Compiles a read of \a id into \a dest.
 */
static void compileIdentifierRead(Compiler *c, IdentifierNode *id, int dest)
{
	if (isRegisterName(id)) {
		int reg = findLocal(c, id->iname);
		if (reg >= 0) {
			if (reg != dest) emit(c, OPC_MOVE, dest, reg, 0);
			return;
		}
	}
	if (!id) {
		bail(c);
		return;
	}
	if (id->type != IT_DIRECT || id->slot) {
		/* A bukkit member or a computed name; the interpreter already
		 * knows how to resolve these. */
		emit(c, OPC_GETSLOT, dest, addConst(c, NULL, NULL, id), 0);
		return;
	}
	emit(c, OPC_GETVAR, dest, addConst(c, NULL, id->iname, id), 0);
}

/**
 * Compiles a write of \a src into \a id.
 */
static void compileIdentifierWrite(Compiler *c, IdentifierNode *id, int src)
{
	if (isRegisterName(id)) {
		int reg = findLocal(c, id->iname);
		if (reg >= 0) {
			if (reg != src) emit(c, OPC_MOVE, reg, src, 0);
			return;
		}
	}
	if (!id) {
		bail(c);
		return;
	}
	if (id->type != IT_DIRECT || id->slot) {
		emit(c, OPC_SETSLOT, src, addConst(c, NULL, NULL, id), 0);
		return;
	}
	emit(c, OPC_SETVAR, src, addConst(c, NULL, id->iname, id), 0);
}

/**
 * The instruction that implements an operator, or \c OPC_NUM if there is none.
 */
static Opcode opcodeForOp(OpType t)
{
	switch (t) {
		case OP_ADD:  return OPC_ADD;
		case OP_SUB:  return OPC_SUB;
		case OP_MULT: return OPC_MUL;
		case OP_DIV:  return OPC_DIV;
		case OP_MOD:  return OPC_MOD;
		case OP_MAX:  return OPC_MAX;
		case OP_MIN:  return OPC_MIN;
		case OP_AND:  return OPC_AND;
		case OP_OR:   return OPC_OR;
		case OP_XOR:  return OPC_XOR;
		case OP_NOT:  return OPC_NOT;
		case OP_EQ:   return OPC_EQ;
		case OP_NEQ:  return OPC_NEQ;
		default:      return OPC_NUM;
	}
}

/**
 * Whether \a expr is an integer literal, and if so what its value is.
 */
static int isIntLiteral(ExprNode *expr, long long *out)
{
	ConstantNode *k;
	if (!expr || expr->type != ET_CONSTANT) return 0;
	k = (ConstantNode *)expr->expr;
	if (k->type != CT_INTEGER) return 0;
	*out = k->data.i;
	return 1;
}

static void compileConstant(Compiler *c, ConstantNode *k, int dest)
{
	switch (k->type) {
		case CT_INTEGER:
			if (k->data.i >= -2147483647LL && k->data.i <= 2147483647LL)
				emit(c, OPC_LOADI, dest, (int)k->data.i, 0);
			else
				emit(c, OPC_LOADK, dest,
						addConst(c, createIntegerValueObject(k->data.i), NULL, NULL), 0);
			break;
		case CT_BOOLEAN:
			emit(c, OPC_LOADB, dest, (int)k->data.i, 0);
			break;
		case CT_NIL:
			emit(c, OPC_LOADNIL, dest, 0, 0);
			break;
		case CT_FLOAT:
			emit(c, OPC_LOADK, dest,
					addConst(c, createFloatValueObject(k->data.f), NULL, NULL), 0);
			break;
		case CT_STRING: {
			char *copy = copyString(k->data.s);
			if (!copy) {
				bail(c);
				return;
			}
			emit(c, OPC_LOADK, dest,
					addConst(c, createStringValueObject(copy), NULL, NULL), 0);
			break;
		}
		default:
			bail(c);
			break;
	}
}

static void compileFuncCall(Compiler *c, FuncCallExprNode *e, int dest)
{
	unsigned int n;
	int base;
	int argc = (int)e->args->num;

	/* A function reached through a bukkit resolves differently; leave the
	 * whole procedure to the interpreter rather than guess. */
	if (!e->name || e->name->type != IT_DIRECT || e->name->slot) {
		bail(c);
		return;
	}
	/* Only plain calls, where the scope is the current one, are lowered. */
	if (!e->scope || e->scope->type != IT_DIRECT || e->scope->slot
			|| e->scope->iname != internName("I")) {
		bail(c);
		return;
	}

	/* Arguments are evaluated into a run of registers above the frame's
	 * live values so that the callee can take them as a block. */
	base = allocReg(c, argc + 1);
	for (n = 0; n < e->args->num; n++)
		compileExpr(c, e->args->exprs[n], base + 1 + (int)n);
	emit(c, OPC_CALL, base, addConst(c, NULL, e->name->iname, e), argc);
	freeReg(c, argc + 1);
	if (base != dest) emit(c, OPC_MOVE, dest, base, 0);
}

static void compileExpr(Compiler *c, ExprNode *expr, int dest)
{
	if (c->failed) return;
	switch (expr->type) {
		case ET_CONSTANT:
			compileConstant(c, (ConstantNode *)expr->expr, dest);
			break;
		case ET_IDENTIFIER:
			compileIdentifierRead(c, (IdentifierNode *)expr->expr, dest);
			break;
		case ET_IMPVAR:
			emit(c, OPC_MOVE, dest, c->itreg, 0);
			break;
		case ET_CAST: {
			CastExprNode *e = (CastExprNode *)expr->expr;
			int tmp = allocReg(c, 1);
			int r = compileExprAny(c, e->target, tmp);
			emit(c, OPC_CAST, dest, r, (int)e->newtype->type);
			freeReg(c, 1);
			break;
		}
		case ET_FUNCCALL:
			compileFuncCall(c, (FuncCallExprNode *)expr->expr, dest);
			break;
		case ET_OP: {
			OpExprNode *e = (OpExprNode *)expr->expr;
			Opcode op = opcodeForOp(e->type);
			long long imm;
			if (op == OPC_NUM) {
				if (e->type == OP_CAT) {
					unsigned int k;
					int base = allocReg(c, (int)e->args->num);
					for (k = 0; k < e->args->num; k++)
						compileExpr(c, e->args->exprs[k], base + (int)k);
					emit(c, OPC_CONCAT, dest, base, (int)e->args->num);
					freeReg(c, (int)e->args->num);
					break;
				}
				bail(c);
				return;
			}
			if (e->type == OP_NOT) {
				int tmp = allocReg(c, 1);
				int r = compileExprAny(c, e->args->exprs[0], tmp);
				emit(c, OPC_NOT, dest, r, 0);
				freeReg(c, 1);
				break;
			}
			if (e->args->num != 2) {
				bail(c);
				return;
			}
			/* Fold a literal right operand into the instruction. */
			if ((e->type == OP_ADD || e->type == OP_SUB || e->type == OP_EQ)
					&& isIntLiteral(e->args->exprs[1], &imm)
					&& imm >= -32768 && imm <= 32767) {
				int tmp = allocReg(c, 1);
				int r = compileExprAny(c, e->args->exprs[0], tmp);
				emit(c, e->type == OP_ADD ? OPC_ADDI
						: e->type == OP_SUB ? OPC_SUBI : OPC_EQI,
						dest, r, (int)imm);
				freeReg(c, 1);
				break;
			}
			{
				int t1 = allocReg(c, 2);
				int l = compileExprAny(c, e->args->exprs[0], t1);
				int r = compileExprAny(c, e->args->exprs[1], t1 + 1);
				emit(c, op, dest, l, r);
				freeReg(c, 2);
			}
			break;
		}
		default:
			bail(c);
			break;
	}
}
/**@}*/

/**
 * Whether a statement puts a name directly into the enclosing scope.
 */
static int stmtDeclaresInScope(StmtNode *node)
{
	switch (node->type) {
		case ST_DECLARATION: {
			DeclarationStmtNode *s = (DeclarationStmtNode *)node->stmt;
			return !isRegisterName(s->target);
		}
		case ST_ALTARRAYDEF:
			return 1;
		default:
			return 0;
	}
}

/**
 * Whether a block needs a scope object of its own.
 *
 * The interpreter gives every block a fresh scope, so a declaration in a loop
 * body happens again on each pass rather than clashing with itself.  The
 * machine only pays for that where a block really does declare something a
 * scope has to hold.
 */
static int blockNeedsScope(BlockNode *block)
{
	unsigned int n;
	if (!block) return 0;
	for (n = 0; n < block->stmts->num; n++)
		if (stmtDeclaresInScope(block->stmts->stmts[n])) return 1;
	return 0;
}

/**
 * Whether anything in an expression reads the implicit variable.
 */
static int exprReadsIT(ExprNode *expr)
{
	unsigned int n;
	if (!expr) return 0;
	switch (expr->type) {
		case ET_IMPVAR:
			return 1;
		case ET_CAST:
			return exprReadsIT(((CastExprNode *)expr->expr)->target);
		case ET_FUNCCALL: {
			FuncCallExprNode *e = (FuncCallExprNode *)expr->expr;
			for (n = 0; n < e->args->num; n++)
				if (exprReadsIT(e->args->exprs[n])) return 1;
			return 0;
		}
		case ET_OP: {
			OpExprNode *e = (OpExprNode *)expr->expr;
			for (n = 0; n < e->args->num; n++)
				if (exprReadsIT(e->args->exprs[n])) return 1;
			return 0;
		}
		default:
			return 0;
	}
}

/**
 * Whether a block reads the implicit variable belonging to its own scope.
 *
 * Nested blocks get an implicit variable of their own, so only the statements
 * directly in this block are considered.  When nothing reads it, the compiler
 * need not set it up at all.
 */
static int blockReadsIT(BlockNode *block)
{
	unsigned int n, i;
	if (!block) return 0;
	for (n = 0; n < block->stmts->num; n++) {
		StmtNode *node = block->stmts->stmts[n];
		switch (node->type) {
			/* Both of these test the implicit variable. */
			case ST_IFTHENELSE:
			case ST_SWITCH:
				return 1;
			case ST_EXPR:
				if (exprReadsIT((ExprNode *)node->stmt)) return 1;
				break;
			case ST_ASSIGNMENT:
				if (exprReadsIT(((AssignmentStmtNode *)node->stmt)->expr)) return 1;
				break;
			case ST_DECLARATION:
				if (exprReadsIT(((DeclarationStmtNode *)node->stmt)->expr)) return 1;
				break;
			case ST_RETURN:
				if (exprReadsIT(((ReturnStmtNode *)node->stmt)->value)) return 1;
				break;
			case ST_PRINT: {
				PrintStmtNode *st = (PrintStmtNode *)node->stmt;
				for (i = 0; i < st->args->num; i++)
					if (exprReadsIT(st->args->exprs[i])) return 1;
				break;
			}
			default:
				/* A loop makes a scope of its own, so its guard
				 * and update read that one, not this. */
				break;
		}
	}
	return 0;
}

/**
 * Records a pending jump out of the enclosing loop.
 */
static void addBreak(Compiler *c, int pc)
{
	if (c->numbreaks == c->capbreaks) {
		unsigned int newcap = c->capbreaks ? c->capbreaks * 2 : 4;
		void *mem = realloc(c->breaks, sizeof(int) * newcap);
		if (!mem) {
			perror("realloc");
			bail(c);
			return;
		}
		c->breaks = mem;
		c->capbreaks = newcap;
	}
	c->breaks[c->numbreaks++] = pc;
}

/**
 * Points a previously emitted jump at the current instruction.
 */
static void patch(Compiler *c, int pc)
{
	if (c->failed || pc < 0) return;
	c->code[pc].b = (int)c->numcode - pc - 1;
}

static void compileStmt(Compiler *c, StmtNode *node)
{
	if (c->failed) return;
	switch (node->type) {
		case ST_EXPR:
			/* A bare expression sets the block's implicit variable. */
			compileExpr(c, (ExprNode *)node->stmt, c->itreg);
			break;

		case ST_ASSIGNMENT: {
			AssignmentStmtNode *s = (AssignmentStmtNode *)node->stmt;
			int reg = isRegisterName(s->target) ? findLocal(c, s->target->iname) : -1;
			if (reg >= 0) {
				compileExpr(c, s->expr, reg);
			}
			else {
				int tmp = allocReg(c, 1);
				compileExpr(c, s->expr, tmp);
				compileIdentifierWrite(c, s->target, tmp);
				freeReg(c, 1);
			}
			break;
		}

		case ST_DECLARATION: {
			DeclarationStmtNode *s = (DeclarationStmtNode *)node->stmt;
			int reg;
			if (s->parent
					|| !s->scope || s->scope->type != IT_DIRECT
					|| s->scope->slot
					|| s->scope->iname != internName("I")
					|| !s->target || s->target->type != IT_DIRECT
					|| s->target->slot) {
				/* Declaring into a bukkit, or inheriting from one,
				 * is left to the interpreter; it needs a scope
				 * either way. */
				emit(c, OPC_STMTSLOW, 0, addConst(c, NULL, NULL, node), 0);
				c->needscope = 1;
				return;
			}
			if (isRegisterName(s->target)) {
				/* A redeclaration in a loop body gets a fresh
				 * binding each time round in the interpreter,
				 * so reuse the register rather than reporting
				 * a redefinition. */
				if (isLocalHere(c, s->target->iname)) {
					/* A redefinition, which the tree
					 * walking interpreter reports. */
					bail(c);
					return;
				}
				reg = findLocal(c, s->target->iname);
				if (reg < 0) reg = addLocal(c, s->target->iname);
				if (s->expr) compileExpr(c, s->expr, reg);
				else if (s->type) {
					switch (s->type->type) {
						case CT_NIL:     emit(c, OPC_LOADNIL, reg, 0, 0); break;
						case CT_BOOLEAN: emit(c, OPC_LOADB, reg, 0, 0); break;
						case CT_INTEGER: emit(c, OPC_LOADI, reg, 0, 0); break;
						case CT_FLOAT:
							emit(c, OPC_LOADK, reg,
									addConst(c, createFloatValueObject(0.0), NULL, NULL), 0);
							break;
						case CT_STRING:
							emit(c, OPC_LOADK, reg,
									addConst(c, createStringValueObject(copyString("")), NULL, NULL), 0);
							break;
						default: bail(c); return;
					}
				}
				else emit(c, OPC_LOADNIL, reg, 0, 0);
			}
			else {
				int tmp = allocReg(c, 1);
				if (s->expr) compileExpr(c, s->expr, tmp);
				else if (s->type) {
					switch (s->type->type) {
						case CT_NIL:     emit(c, OPC_LOADNIL, tmp, 0, 0); break;
						case CT_BOOLEAN: emit(c, OPC_LOADB, tmp, 0, 0); break;
						case CT_INTEGER: emit(c, OPC_LOADI, tmp, 0, 0); break;
						case CT_FLOAT:
							emit(c, OPC_LOADK, tmp,
									addConst(c, createFloatValueObject(0.0), NULL, NULL), 0);
							break;
						case CT_STRING:
							emit(c, OPC_LOADK, tmp,
									addConst(c, createStringValueObject(copyString("")), NULL, NULL), 0);
							break;
						case CT_ARRAY:
							emit(c, OPC_DECLARR, tmp,
									addConst(c, NULL, s->target->iname, s->target), 0);
							freeReg(c, 1);
							return;
						default: bail(c); return;
					}
				}
				else emit(c, OPC_LOADNIL, tmp, 0, 0);
				if (!s->target || s->target->type != IT_DIRECT) {
					bail(c);
					return;
				}
				emit(c, OPC_DECLVAR, tmp,
						addConst(c, NULL, s->target->iname, s->target), 0);
				freeReg(c, 1);
			}
			break;
		}

		case ST_CAST: {
			CastStmtNode *s = (CastStmtNode *)node->stmt;
			int tmp = allocReg(c, 1);
			compileIdentifierRead(c, s->target, tmp);
			emit(c, OPC_CAST, tmp, tmp, (int)s->newtype->type);
			compileIdentifierWrite(c, s->target, tmp);
			freeReg(c, 1);
			break;
		}

		case ST_PRINT: {
			PrintStmtNode *s = (PrintStmtNode *)node->stmt;
			unsigned int n;
			int base;
			if (s->args->num == 0) {
				bail(c);
				return;
			}
			base = allocReg(c, (int)s->args->num);
			for (n = 0; n < s->args->num; n++)
				compileExpr(c, s->args->exprs[n], base + (int)n);
			emit(c, OPC_PRINT, base, addConst(c, NULL, NULL, s), (int)s->args->num);
			freeReg(c, (int)s->args->num);
			break;
		}

		case ST_INPUT: {
			InputStmtNode *s = (InputStmtNode *)node->stmt;
			int tmp = allocReg(c, 1);
			emit(c, OPC_INPUT, tmp, 0, 0);
			compileIdentifierWrite(c, s->target, tmp);
			freeReg(c, 1);
			break;
		}

		case ST_RETURN: {
			ReturnStmtNode *s = (ReturnStmtNode *)node->stmt;
			int tmp = allocReg(c, 1);
			int r = compileExprAny(c, s->value, tmp);
			emit(c, OPC_RET, r, 0, 0);
			freeReg(c, 1);
			break;
		}

		case ST_BREAK:
			if (c->loopstart >= 0) addBreak(c, emit(c, OPC_JMP, 0, 0, 0));
			else emit(c, OPC_BREAK, 0, 0, 0);
			break;

		case ST_IFTHENELSE: {
			IfThenElseStmtNode *s = (IfThenElseStmtNode *)node->stmt;
			unsigned int n;
			int *ends = NULL;
			unsigned int numends = 0;
			int next;

			ends = malloc(sizeof(int) * (s->guards->num + 2));
			if (!ends) {
				perror("malloc");
				bail(c);
				return;
			}

			/* The condition is whatever the block's IT holds. */
			next = emit(c, OPC_JMPF, c->itreg, 0, 0);
			compileBlock(c, s->yes, 1);
			ends[numends++] = emit(c, OPC_JMP, 0, 0, 0);
			patch(c, next);

			for (n = 0; n < s->guards->num; n++) {
				int t = allocReg(c, 1);
				int g = compileExprAny(c, s->guards->exprs[n], t);
				next = emit(c, OPC_JMPF, g, 0, 0);
				freeReg(c, 1);
				compileBlock(c, s->blocks->blocks[n], 1);
				ends[numends++] = emit(c, OPC_JMP, 0, 0, 0);
				patch(c, next);
			}

			if (s->no) compileBlock(c, s->no, 1);
			for (n = 0; n < numends; n++) patch(c, ends[n]);
			free(ends);
			break;
		}

		case ST_LOOP: {
			LoopStmtNode *s = (LoopStmtNode *)node->stmt;
			int savedstart = c->loopstart;
			int *savedbreaks = c->breaks;
			unsigned int savednum = c->numbreaks, savedcap = c->capbreaks;
			unsigned int savedlocals = c->numlocals;
			int savedreg = c->nreg;
			int saveditreg = c->itreg;
			int varreg = -1;
			int start, guardjmp = -1;
			unsigned int n;

			c->breaks = NULL;
			c->numbreaks = 0;
			c->capbreaks = 0;

			c->depth++;
			/* The interpreter gives a loop a scope of its own, so
			 * its guard and update see a fresh implicit variable. */
			if (blockReadsIT(s->body)
					|| exprReadsIT(s->guard) || exprReadsIT(s->update)) {
				c->itreg = allocReg(c, 1);
				emit(c, OPC_LOADNIL, c->itreg, 0, 0);
			}
			if (s->var) {
				if (isRegisterName(s->var)) {
					varreg = addLocal(c, s->var->iname);
					emit(c, OPC_LOADI, varreg, 0, 0);
				}
				else {
					int t = allocReg(c, 1);
					emit(c, OPC_LOADI, t, 0, 0);
					emit(c, OPC_DECLVAR, t,
							addConst(c, NULL, s->var->iname, s->var), 0);
					freeReg(c, 1);
				}
			}

			c->loopstart = start = (int)c->numcode;
			if (s->guard) {
				int t = allocReg(c, 1);
				int g = compileExprAny(c, s->guard, t);
				guardjmp = emit(c, OPC_JMPF, g, 0, 0);
				freeReg(c, 1);
			}
			if (s->body) compileBlock(c, s->body, 1);

			if (s->update) {
				if (!s->var) {
					bail(c);
				}
				else if (s->update->type == ET_OP) {
					OpExprNode *op = (OpExprNode *)s->update->expr;
					if (op->type != OP_ADD && op->type != OP_SUB) {
						bail(c);
					}
					else if (varreg >= 0) {
						emit(c, OPC_ADDI, varreg, varreg,
								op->type == OP_ADD ? 1 : -1);
					}
					else {
						int t = allocReg(c, 1);
						compileIdentifierRead(c, s->var, t);
						emit(c, OPC_ADDI, t, t, op->type == OP_ADD ? 1 : -1);
						compileIdentifierWrite(c, s->var, t);
						freeReg(c, 1);
					}
				}
				else if (varreg >= 0) {
					compileExpr(c, s->update, varreg);
				}
				else {
					int t = allocReg(c, 1);
					compileExpr(c, s->update, t);
					compileIdentifierWrite(c, s->var, t);
					freeReg(c, 1);
				}
			}

			emit(c, OPC_JMP, 0, start - (int)c->numcode - 1, 0);
			patch(c, guardjmp);
			for (n = 0; n < c->numbreaks; n++) patch(c, c->breaks[n]);
			/* A break jumps straight here, past any block's own pop. */
			emit(c, OPC_POPSC, c->scopedepth, 0, 0);

			free(c->breaks);
			c->breaks = savedbreaks;
			c->numbreaks = savednum;
			c->capbreaks = savedcap;
			c->loopstart = savedstart;
			c->depth--;
			c->numlocals = savedlocals;
			c->nreg = savedreg;
			c->itreg = saveditreg;
			break;
		}

		case ST_FUNCDEF: {
			FuncDefStmtNode *s = (FuncDefStmtNode *)node->stmt;
			if (!s->scope || s->scope->type != IT_DIRECT || s->scope->slot
					|| s->scope->iname != internName("I")
					|| !s->name || s->name->type != IT_DIRECT) {
				bail(c);
				return;
			}
			{
				int tmp = allocReg(c, 1);
				emit(c, OPC_LOADK, tmp,
						addConst(c, createFunctionValueObject(s), NULL, NULL), 0);
				emit(c, OPC_DECLVAR, tmp,
						addConst(c, NULL, s->name->iname, s->name), 0);
				freeReg(c, 1);
			}
			break;
		}

		case ST_DEALLOCATION: {
			DeallocationStmtNode *s = (DeallocationStmtNode *)node->stmt;
			int reg = isRegisterName(s->target) ? findLocal(c, s->target->iname) : -1;
			if (reg >= 0) {
				emit(c, OPC_LOADNIL, reg, 0, 0);
			}
			else {
				int tmp = allocReg(c, 1);
				emit(c, OPC_LOADNIL, tmp, 0, 0);
				compileIdentifierWrite(c, s->target, tmp);
				freeReg(c, 1);
			}
			break;
		}

		case ST_SWITCH: {
			SwitchStmtNode *s = (SwitchStmtNode *)node->stmt;
			int savedstart = c->loopstart;
			int *savedbreaks = c->breaks;
			unsigned int savednum = c->numbreaks, savedcap = c->capbreaks;
			int *starts;
			int nomatch;
			unsigned int n;

			if (s->guards->num != s->blocks->num) {
				bail(c);
				return;
			}
			starts = malloc(sizeof(int) * (s->guards->num + 1));
			if (!starts) {
				perror("malloc");
				bail(c);
				return;
			}

			c->breaks = NULL;
			c->numbreaks = 0;
			c->capbreaks = 0;
			/* GTFO inside a switch leaves the switch. */
			c->loopstart = (int)c->numcode;

			/* Test the implicit variable against each guard in turn,
			 * jumping to that guard's block on the first match. */
			for (n = 0; n < s->guards->num; n++) {
				int t = allocReg(c, 2);
				int g = compileExprAny(c, s->guards->exprs[n], t);
				emit(c, OPC_SWEQ, t + 1, c->itreg, g);
				starts[n] = emit(c, OPC_JMPT, t + 1, 0, 0);
				freeReg(c, 2);
			}
			nomatch = emit(c, OPC_JMP, 0, 0, 0);

			/* Blocks run one after another until a break, as they
			 * do in the interpreter. */
			for (n = 0; n < s->blocks->num; n++) {
				patch(c, starts[n]);
				compileBlock(c, s->blocks->blocks[n], 1);
			}
			starts[s->blocks->num] = emit(c, OPC_JMP, 0, 0, 0);

			patch(c, nomatch);
			if (s->def) compileBlock(c, s->def, 1);
			patch(c, starts[s->blocks->num]);

			for (n = 0; n < c->numbreaks; n++) patch(c, c->breaks[n]);
			emit(c, OPC_POPSC, c->scopedepth, 0, 0);
			free(c->breaks);
			free(starts);
			c->breaks = savedbreaks;
			c->numbreaks = savednum;
			c->capbreaks = savedcap;
			c->loopstart = savedstart;
			break;
		}

		default:
			/* Alternate array definitions are left to the tree
			 * walking interpreter. */
			bail(c);
			break;
	}
}

/**
 * Compiles a block.
 *
 * \param [in] newit Whether the block gets its own implicit variable, which
 * every block but a procedure's outermost one does.
 */
static void compileBlock(Compiler *c, BlockNode *block, int newit)
{
	unsigned int n;
	unsigned int savedlocals = c->numlocals;
	int savedreg = c->nreg;
	int saveditreg = c->itreg;
	int saveddepth;
	int pushed = 0;

	if (c->failed || !block) return;
	saveddepth = c->scopedepth;

	c->depth++;
	if (blockNeedsScope(block)) {
		emit(c, OPC_PUSHSC, 0, 0, 0);
		c->scopedepth++;
		pushed = 1;
	}
	if (newit && blockReadsIT(block)) {
		c->itreg = allocReg(c, 1);
		emit(c, OPC_LOADNIL, c->itreg, 0, 0);
	}
	else if (newit) {
		/* Nothing reads it, so leave the enclosing one in place; no
		 * instruction in this block can observe the difference. */
		c->itreg = saveditreg;
	}
	for (n = 0; n < block->stmts->num; n++)
		compileStmt(c, block->stmts->stmts[n]);
	if (pushed) {
		emit(c, OPC_POPSC, saveddepth, 0, 0);
		c->scopedepth = saveddepth;
	}
	c->depth--;

	/* Bindings made inside the block go out of scope with it, but the
	 * registers stay reserved so that a later block does not reuse one
	 * that an enclosing loop still reads. */
	c->numlocals = savedlocals;
	c->itreg = saveditreg;
	c->nreg = savedreg;
}

/**
 * \name Execution
 *
 * Registers hold ordinary \ref ValueObject pointers, so every cast, operator
 * and library routine the tree walking interpreter uses works unchanged here.
 * The machine avoids most of the allocation that costs by writing results
 * straight into a register's existing value whenever that value has no other
 * owner.
 */
/**@{*/

/**
 * A running procedure.
 */
typedef struct frame {
	struct frame *prev;   /**< The calling frame. */
	Proto *proto;         /**< The procedure being run. */
	unsigned int base;    /**< Where this frame's registers start. */
	ScopeObject *scope;   /**< The scope names are resolved against. */
	int itreg;            /**< The register holding the current block's IT. */
	ScopeObject *owned;   /**< A scope this frame created, or NULL. */
	int pushed;           /**< How many block scopes are open. */
} Frame;

/**
 * The register file, shared by all frames as a stack.  It is reallocated as it
 * grows, so frames hold an index into it rather than a pointer.
 */
static ValueObject **regstack = NULL;
static unsigned int regstackcap = 0;
static unsigned int regstacktop = 0;

/**
 * A frame's registers.  Recompute this after anything that can make the
 * register stack grow, which means after a call.
 */
#define FRAMEREGS(f) (regstack + (f)->base)

/**
 * The innermost running frame, so that name based lookups from outside the
 * machine can still find register held variables.
 */
static Frame *currentFrame = NULL;

/**
 * Makes room for \a n more registers.
 */
static int reserveRegs(unsigned int n)
{
	if (regstacktop + n > regstackcap) {
		unsigned int newcap = regstackcap ? regstackcap * 2 : 4096;
		void *mem;
		while (regstacktop + n > newcap) newcap *= 2;
		mem = realloc(regstack, sizeof(ValueObject *) * newcap);
		if (!mem) {
			perror("realloc");
			return 0;
		}
		regstack = mem;
		regstackcap = newcap;
	}
	return 1;
}

/**
 * Returns a register's value ready to be overwritten.
 *
 * When the register is the only owner of a scalar value, that value is reused
 * in place; otherwise a fresh one is taken from the pool.  This is what keeps
 * arithmetic in a loop from allocating.
 */
static ValueObject *mutableReg(ValueObject **regs, unsigned int a)
{
	ValueObject *v = regs[a];
	if (v) {
		/* Named rather than excluded, so that a value type added later
		 * gets a fresh object instead of having whatever it points at
		 * quietly overwritten. */
		if (v->semaphore == 1
				&& (v->type == VT_INTEGER || v->type == VT_BOOLEAN
					|| v->type == VT_FLOAT || v->type == VT_NIL))
			return v;
		deleteValueObject(v);
	}
	return (regs[a] = newValueObject());
}

/**
 * Replaces a register's contents, taking ownership of \a val.
 */
static void setReg(ValueObject **regs, unsigned int a, ValueObject *val)
{
	ValueObject *old = regs[a];
	regs[a] = val;
	if (old) deleteValueObject(old);
}

/**
 * Looks a name up through the scope chain, remembering where it was found.
 */
static ValueObject *cachedLookup(Proto *p, int k, ScopeObject *from)
{
	const Name *name = p->knames[k];
	ScopeObject *s;

	if (p->icfrom[k] == from && p->icver[k] == scopeVersion) {
		ScopeObject *found = p->icscope[k];
		return found->values[p->icslot[k]];
	}
	for (s = from; s; s = s->parent) {
		int slot = findScopeSlot(s, name);
		if (slot >= 0) {
			p->icfrom[k] = from;
			p->icscope[k] = s;
			p->icslot[k] = slot;
			p->icver[k] = scopeVersion;
			return s->values[slot];
		}
	}
	{
		IdentifierNode *id = (IdentifierNode *)p->kptrs[k];
		error(IN_VARIABLE_DOES_NOT_EXIST, id ? id->fname : NULL,
				id ? id->line : 0, name->str);
	}
	return NULL;
}

/**
 * Finds a register held variable by name in the running frame.
 *
 * String interpolation resolves names at run time, so a variable the compiler
 * put in a register still has to be reachable by name.  The escape analysis
 * keeps interpolated names out of registers in the ordinary case; this covers
 * a string that only turns out to interpolate once it has been built.
 *
 * \param [in] name The name to look for.
 *
 * \return The value bound to \a name in the innermost frame that has one.
 *
 * \retval NULL No frame holds \a name in a register.
 */
ValueObject *lookupRegisterVar(const Name *name)
{
	Frame *f;
	for (f = currentFrame; f; f = f->prev) {
		Proto *p = f->proto;
		ValueObject **regs = FRAMEREGS(f);
		unsigned int n;
		for (n = 0; n < p->numregs; n++)
			if (p->regnames[n] == name && regs[n])
				return regs[n];
	}
	return NULL;
}
/**@}*/

/**
 * How a procedure stopped running.
 */
typedef enum {
	VM_OK,     /**< Ran off the end of the procedure. */
	VM_RETURN, /**< Returned a value. */
	VM_BREAK,  /**< Left the procedure early. */
	VM_ERROR   /**< Failed; an error has been reported. */
} VMStatus;

static ValueObject *callByName(Proto *p, int k, ValueObject **argv,
                               unsigned int argc, ScopeObject *scope);

/**
 * Runs the instructions of one procedure.
 */
static VMStatus run(Frame *f, ValueObject **out)
{
	Proto *p = f->proto;
	ValueObject **regs = FRAMEREGS(f);
	const Instr *pc = p->code;

	*out = NULL;

	for (;;) {
		const Instr i = *pc++;
		switch ((Opcode)i.op) {

		case OPC_LOADI: {
			ValueObject *v = mutableReg(regs, i.a);
			if (!v) return VM_ERROR;
			v->type = VT_INTEGER;
			v->data.i = i.b;
			break;
		}

		case OPC_LOADB: {
			ValueObject *v = mutableReg(regs, i.a);
			if (!v) return VM_ERROR;
			v->type = VT_BOOLEAN;
			v->data.i = i.b;
			break;
		}

		case OPC_LOADNIL: {
			ValueObject *v = mutableReg(regs, i.a);
			if (!v) return VM_ERROR;
			v->type = VT_NIL;
			break;
		}

		case OPC_LOADK:
			setReg(regs, i.a, copyValueObject(p->k[i.b]));
			break;

		case OPC_MOVE:
			if (i.a != i.b) setReg(regs, i.a, copyValueObject(regs[i.b]));
			break;

		case OPC_GETVAR: {
			ValueObject *v = cachedLookup(p, i.b, f->scope);
			if (!v) return VM_ERROR;
			setReg(regs, i.a, copyValueObject(v));
			break;
		}

		case OPC_SETVAR: {
			IdentifierNode *id = (IdentifierNode *)p->kptrs[i.b];
			ValueObject *cp = copyValueObject(regs[i.a]);
			if (!updateScopeValue(f->scope, f->scope, id, cp)) {
				deleteValueObject(cp);
				return VM_ERROR;
			}
			break;
		}

		case OPC_DECLVAR: {
			IdentifierNode *id = (IdentifierNode *)p->kptrs[i.b];
			if (findScopeSlot(f->scope, p->knames[i.b]) >= 0) {
				error(IN_REDEFINITION_OF_VARIABLE, id->fname, id->line,
						p->knames[i.b]->str);
				return VM_ERROR;
			}
			if (appendScopeSlot(f->scope, p->knames[i.b]) < 0) return VM_ERROR;
			{
				ValueObject *cp = copyValueObject(regs[i.a]);
				if (!updateScopeValue(f->scope, f->scope, id, cp)) {
					deleteValueObject(cp);
					return VM_ERROR;
				}
			}
			break;
		}

		case OPC_DECLARR: {
			IdentifierNode *id = (IdentifierNode *)p->kptrs[i.b];
			ValueObject *arr;
			if (findScopeSlot(f->scope, p->knames[i.b]) >= 0) {
				error(IN_REDEFINITION_OF_VARIABLE, id->fname, id->line,
						p->knames[i.b]->str);
				return VM_ERROR;
			}
			arr = createArrayValueObject(f->scope);
			if (!arr) return VM_ERROR;
			if (appendScopeSlot(f->scope, p->knames[i.b]) < 0) {
				deleteValueObject(arr);
				return VM_ERROR;
			}
			if (!updateScopeValue(f->scope, f->scope, id, arr)) return VM_ERROR;
			break;
		}

		case OPC_GETSLOT: {
			IdentifierNode *id = (IdentifierNode *)p->kptrs[i.b];
			ValueObject *v = getScopeValue(f->scope, f->scope, id);
			if (!v) return VM_ERROR;
			setReg(regs, i.a, copyValueObject(v));
			break;
		}

		case OPC_SETSLOT: {
			IdentifierNode *id = (IdentifierNode *)p->kptrs[i.b];
			ValueObject *cp = copyValueObject(regs[i.a]);
			if (!updateScopeValue(f->scope, f->scope, id, cp)) {
				deleteValueObject(cp);
				return VM_ERROR;
			}
			break;
		}

		case OPC_STMTSLOW: {
			ReturnObject *r = interpretStmtNode(
					(StmtNode *)p->kptrs[i.b], f->scope);
			if (!r) return VM_ERROR;
			deleteReturnObject(r);
			break;
		}

		case OPC_PUSHSC: {
			ScopeObject *inner = createScopeObject(f->scope);
			if (!inner) return VM_ERROR;
			f->scope = inner;
			f->pushed++;
			break;
		}

		case OPC_POPSC:
			while (f->pushed > i.a) {
				ScopeObject *parent = f->scope->parent;
				deleteScopeObject(f->scope);
				f->scope = parent;
				f->pushed--;
			}
			break;

		case OPC_UNDECL:
			deleteScopeValue(f->scope, f->scope,
					(IdentifierNode *)p->kptrs[i.b]);
			break;

		/* Arithmetic.  Two integers is by far the common case and is
		 * handled without leaving the loop or allocating. */
		case OPC_ADD: case OPC_SUB: case OPC_MUL:
		case OPC_DIV: case OPC_MOD: case OPC_MAX: case OPC_MIN: {
			ValueObject *x = regs[i.b], *y = regs[i.c];
			if (x->type == VT_INTEGER && y->type == VT_INTEGER) {
				long long l = x->data.i, r = y->data.i, res;
				switch ((Opcode)i.op) {
					case OPC_ADD: res = l + r; break;
					case OPC_SUB: res = l - r; break;
					case OPC_MUL: res = l * r; break;
					case OPC_MAX: res = l > r ? l : r; break;
					case OPC_MIN: res = l < r ? l : r; break;
					default:
						goto slowarith;
				}
				{
					ValueObject *v = mutableReg(regs, i.a);
					if (!v) return VM_ERROR;
					v->type = VT_INTEGER;
					v->data.i = res;
				}
				break;
			}
		slowarith: {
				ValueObject *res = applyArithOp(
						(OpType)(OP_ADD + (i.op - OPC_ADD)),
						x, y, f->scope);
				if (!res) return VM_ERROR;
				setReg(regs, i.a, res);
			}
			break;
		}

		case OPC_ADDI: case OPC_SUBI: {
			ValueObject *x = regs[i.b];
			long long imm = (i.op == OPC_ADDI) ? i.c : -(long long)i.c;
			if (x->type == VT_INTEGER) {
				long long res = x->data.i + imm;
				ValueObject *v = mutableReg(regs, i.a);
				if (!v) return VM_ERROR;
				v->type = VT_INTEGER;
				v->data.i = res;
			}
			else {
				ValueObject *lit = createIntegerValueObject(i.c);
				ValueObject *res;
				if (!lit) return VM_ERROR;
				res = applyArithOp(i.op == OPC_ADDI ? OP_ADD : OP_SUB,
						x, lit, f->scope);
				deleteValueObject(lit);
				if (!res) return VM_ERROR;
				setReg(regs, i.a, res);
			}
			break;
		}

		case OPC_EQI: {
			ValueObject *x = regs[i.b];
			if (x->type == VT_INTEGER) {
				int res = (x->data.i == i.c);
				ValueObject *v = mutableReg(regs, i.a);
				if (!v) return VM_ERROR;
				v->type = VT_BOOLEAN;
				v->data.i = res;
			}
			else {
				ValueObject *lit = createIntegerValueObject(i.c);
				ValueObject *res;
				if (!lit) return VM_ERROR;
				res = applyEqualityOp(OP_EQ, x, lit, f->scope);
				deleteValueObject(lit);
				if (!res) return VM_ERROR;
				setReg(regs, i.a, res);
			}
			break;
		}

		case OPC_EQ: case OPC_NEQ: {
			ValueObject *x = regs[i.b], *y = regs[i.c];
			ValueObject *res;
			/* Like types that compare by their integer payload need
			 * neither a cast nor an allocation. */
			if (x->type == y->type
					&& (x->type == VT_INTEGER || x->type == VT_BOOLEAN)) {
				int eq = (x->data.i == y->data.i);
				ValueObject *v = mutableReg(regs, i.a);
				if (!v) return VM_ERROR;
				v->type = VT_BOOLEAN;
				v->data.i = (i.op == OPC_EQ) ? eq : !eq;
				break;
			}
			res = applyEqualityOp(i.op == OPC_EQ ? OP_EQ : OP_NEQ,
					x, y, f->scope);
			if (!res) return VM_ERROR;
			setReg(regs, i.a, res);
			break;
		}

		case OPC_AND: case OPC_OR: case OPC_XOR: {
			int ok1, ok2;
			int l = valueIsTrueInline(regs[i.b], f->scope, &ok1);
			int r = valueIsTrueInline(regs[i.c], f->scope, &ok2);
			int res;
			ValueObject *v;
			if (!ok1 || !ok2) return VM_ERROR;
			res = i.op == OPC_AND ? (l && r)
					: i.op == OPC_OR ? (l || r) : (l != r);
			v = mutableReg(regs, i.a);
			if (!v) return VM_ERROR;
			v->type = VT_BOOLEAN;
			v->data.i = res;
			break;
		}

		case OPC_NOT: {
			int ok;
			int val = valueIsTrueInline(regs[i.b], f->scope, &ok);
			ValueObject *v;
			if (!ok) return VM_ERROR;
			v = mutableReg(regs, i.a);
			if (!v) return VM_ERROR;
			v->type = VT_BOOLEAN;
			v->data.i = !val;
			break;
		}

		case OPC_SWEQ: {
			int match = switchMatches(regs[i.b], regs[i.c]);
			ValueObject *v;
			if (match < 0) {
				error(IN_INVALID_TYPE);
				return VM_ERROR;
			}
			v = mutableReg(regs, i.a);
			if (!v) return VM_ERROR;
			v->type = VT_BOOLEAN;
			v->data.i = match;
			break;
		}

		case OPC_CONCAT: {
			ValueObject *res = concatValues(regs + i.b, (unsigned int)i.c,
					f->scope);
			if (!res) return VM_ERROR;
			setReg(regs, i.a, res);
			break;
		}

		case OPC_CAST: {
			ValueObject *res = NULL;
			ValueObject *src = regs[i.b];
			switch ((ConstantType)i.c) {
				case CT_NIL:     res = createNilValueObject(); break;
				case CT_BOOLEAN: res = castBooleanExplicit(src, f->scope); break;
				case CT_INTEGER: res = castIntegerExplicit(src, f->scope); break;
				case CT_FLOAT:   res = castFloatExplicit(src, f->scope); break;
				case CT_STRING:  res = castStringExplicit(src, f->scope); break;
				default:
					error(IN_UNKNOWN_CAST_TYPE);
					return VM_ERROR;
			}
			if (!res) return VM_ERROR;
			setReg(regs, i.a, res);
			break;
		}

		case OPC_JMP:
			pc += i.b;
			break;

		case OPC_JMPF: {
			int ok;
			int t = valueIsTrueInline(regs[i.a], f->scope, &ok);
			if (!ok) return VM_ERROR;
			if (!t) pc += i.b;
			break;
		}

		case OPC_JMPT: {
			int ok;
			int t = valueIsTrueInline(regs[i.a], f->scope, &ok);
			if (!ok) return VM_ERROR;
			if (t) pc += i.b;
			break;
		}

		case OPC_CALL: {
			ValueObject *res = callByName(p, i.b, regs + i.a + 1,
					(unsigned int)i.c, f->scope);
			/* The callee may have grown the register stack, which
			 * moves it; this frame's registers are elsewhere now. */
			regs = FRAMEREGS(f);
			if (!res) return VM_ERROR;
			setReg(regs, i.a, res);
			break;
		}

		case OPC_RET:
			*out = copyValueObject(regs[i.a]);
			return VM_RETURN;

		case OPC_RETNIL:
			*out = createNilValueObject();
			return VM_RETURN;

		case OPC_BREAK:
			return VM_BREAK;

		case OPC_PRINT: {
			PrintStmtNode *st = (PrintStmtNode *)p->kptrs[i.b];
			int n;
			for (n = 0; n < i.c; n++) {
				ValueObject *use = castStringImplicit(regs[i.a + n], f->scope);
				if (!use) return VM_ERROR;
				fprintf(st->file, "%s", getString(use));
				deleteValueObject(use);
			}
			if (!st->nonl) putc('\n', st->file);
			break;
		}

		case OPC_INPUT: {
			ValueObject *v = readLineValue();
			if (!v) return VM_ERROR;
			setReg(regs, i.a, v);
			break;
		}

		case OPC_ENDPROC:
			return VM_OK;

		default:
			error(IN_INVALID_OPCODE);
			return VM_ERROR;
		}
	}
}
/**@}*/

/**
 * \name Entry points
 */
/**@{*/

/**
 * Every procedure compiled for the running program, so that they can be freed.
 */
static Proto *mainproto = NULL;
static Proto **allprotos = NULL;
static unsigned int numprotos = 0;
static unsigned int capprotos = 0;

ValueObject *callProto(Proto *proto, ValueObject **args, unsigned int numargs,
                       ScopeObject *scope, ScopeObject *caller)
{
	Frame frame;
	ValueObject *ret = NULL;
	VMStatus status;
	unsigned int base;
	unsigned int n;

	if (stackExhausted()) {
		error(IN_RECURSION_TOO_DEEP);
		return NULL;
	}

	/* Native code is compiled on the assumption that every argument is an
	 * integer; when that does not hold, run the instructions instead. */
	if (proto->jitcode && numargs == proto->numargs) {
		unsigned int k;
		for (k = 0; k < numargs; k++)
			if (args[k]->type != VT_INTEGER) break;
		if (k == numargs) return jitCall(proto, args, numargs);
	}
	if (!reserveRegs(proto->numregs)) return NULL;

	base = regstacktop;
	regstacktop += proto->numregs;

	frame.prev = currentFrame;
	frame.proto = proto;
	frame.base = base;
	frame.itreg = -1;
	frame.owned = NULL;
	frame.pushed = 0;

	/* A procedure that declares nothing a scope can observe runs against
	 * its caller's scope, which keeps recursion from growing the chain. */
	if (proto->needscope) {
		/* The interpreter roots a call's scope at the caller's and
		 * points ME at the object the function was found on. */
		frame.owned = createScopeObjectCaller(scope, caller ? caller : scope);
		if (!frame.owned) {
			regstacktop = base;
			return NULL;
		}
		frame.scope = frame.owned;
	}
	else {
		frame.scope = scope;
	}

	/* Arguments arrive in the lowest registers; the rest start empty. */
	{
		ValueObject **regs = FRAMEREGS(&frame);
		for (n = 0; n < proto->numregs; n++)
			regs[n] = (n < numargs) ? copyValueObject(args[n]) : NULL;
	}

	currentFrame = &frame;
	status = run(&frame, &ret);
	currentFrame = frame.prev;

	/* A procedure that ends without returning yields its implicit
	 * variable, and one that breaks out yields nil. */
	{
		ValueObject **regs = FRAMEREGS(&frame);
		if (status == VM_OK) {
			ValueObject *it = proto->numregs ? regs[proto->itreg] : NULL;
			ret = it ? copyValueObject(it) : createNilValueObject();
		}
		else if (status == VM_BREAK) {
			ret = createNilValueObject();
		}

		for (n = 0; n < proto->numregs; n++)
			if (regs[n]) deleteValueObject(regs[n]);
	}
	while (frame.pushed > 0) {
		ScopeObject *parent = frame.scope->parent;
		deleteScopeObject(frame.scope);
		frame.scope = parent;
		frame.pushed--;
	}
	regstacktop = base;
	if (frame.owned) deleteScopeObject(frame.owned);

	return ret;
}

/**
 * Calls the function a \c CALL instruction names.
 */
static ValueObject *callByName(Proto *p, int k, ValueObject **argv,
                               unsigned int argc, ScopeObject *scope)
{
	ValueObject *def = cachedLookup(p, k, scope);
	FuncDefStmtNode *fn;
	IdentifierNode *id = (IdentifierNode *)
			((FuncCallExprNode *)p->kptrs[k])->name;

	if (!def) return NULL;
	if (def->type != VT_FUNC) {
		error(IN_UNDEFINED_FUNCTION, id->fname, id->line, p->knames[k]->str);
		return NULL;
	}
	fn = getFunction(def);
	if (fn->args->num != argc) {
		error(IN_INCORRECT_NUMBER_OF_ARGUMENTS, id->fname, id->line,
				p->knames[k]->str);
		return NULL;
	}
	if (fn->proto) return callProto((Proto *)fn->proto, argv, argc, scope, scope);
	return callFunctionValues(fn, argv, argc, scope);
}

/**
 * Builds a procedure from a parse tree.
 *
 * \return The compiled procedure.
 *
 * \retval NULL The procedure contains something the compiler cannot lower.
 */
static Proto *compileProc(IdentifierNodeList *args, BlockNode *body,
                          FuncDefStmtNode *def)
{
	Compiler c;
	Proto *p;
	unsigned int n;

	memset(&c, 0, sizeof(c));
	c.loopstart = -1;

	/* Arguments arrive in the lowest registers, in order.  A parameter that
	 * something outside the procedure could look up by name is copied from
	 * there into the frame's scope; the rest simply stay where they are. */
	if (args)
		for (n = 0; n < args->num; n++) {
			IdentifierNode *id = args->ids[n];
			if (!id || id->type != IT_DIRECT || id->slot) {
				bail(&c);
				break;
			}
			if (isRegisterName(id)) {
				addLocal(&c, id->iname);
			}
			else {
				int reg = allocReg(&c, 1);
				emit(&c, OPC_DECLVAR, reg,
						addConst(&c, NULL, id->iname, id), 0);
			}
		}

	/* The procedure's own implicit variable. */
	c.itreg = allocReg(&c, 1);
	emit(&c, OPC_LOADNIL, c.itreg, 0, 0);

	compileBlock(&c, body, 0);
	emit(&c, OPC_ENDPROC, 0, 0, 0);

	if (c.failed) {
		if (getenv("LCI_DUMP"))
			fprintf(stderr, "; gave up compiling at vm.c:%d\n", c.bailline);
		for (n = 0; n < c.numk; n++)
			if (c.k[n]) deleteValueObject(c.k[n]);
		free(c.code);
		free(c.k);
		free(c.knames);
		free(c.kptrs);
		free(c.locals);
		free(c.breaks);
		return NULL;
	}

	p = calloc(1, sizeof(Proto));
	if (!p) {
		perror("calloc");
		return NULL;
	}
	p->code = c.code;
	p->numcode = c.numcode;
	p->k = c.k;
	p->knames = c.knames;
	p->kptrs = c.kptrs;
	p->numk = c.numk;
	p->numregs = (unsigned int)c.maxreg;
	p->numargs = args ? args->num : 0;
	p->def = def;
	p->itreg = args ? (int)args->num : 0;

	/* Record which register holds which name, for lookups by name. */
	p->regnames = calloc(p->numregs ? p->numregs : 1, sizeof(Name *));
	if (args)
		for (n = 0; n < args->num && n < p->numregs; n++)
			p->regnames[n] = args->ids[n]->iname;

	p->icscope = calloc(p->numk ? p->numk : 1, sizeof(ScopeObject *));
	p->icfrom = calloc(p->numk ? p->numk : 1, sizeof(ScopeObject *));
	p->icslot = calloc(p->numk ? p->numk : 1, sizeof(int));
	p->icver = calloc(p->numk ? p->numk : 1, sizeof(unsigned int));

	/* A frame needs its own scope only if the procedure puts something
	 * into one. */
	p->needscope = c.needscope;
	for (n = 0; n < p->numcode; n++) {
		Opcode op = (Opcode)p->code[n].op;
		/* Anything that puts a name into a scope, or that hands work
		 * back to the interpreter, needs this frame to own one. */
		if (op == OPC_DECLVAR || op == OPC_DECLARR || op == OPC_UNDECL
				|| op == OPC_GETSLOT || op == OPC_SETSLOT
				|| op == OPC_STMTSLOW) {
			p->needscope = 1;
			break;
		}
	}

	free(c.locals);
	free(c.breaks);

	if (numprotos == capprotos) {
		unsigned int newcap = capprotos ? capprotos * 2 : 16;
		void *mem = realloc(allprotos, sizeof(Proto *) * newcap);
		if (!mem) {
			perror("realloc");
			return p;
		}
		allprotos = mem;
		capprotos = newcap;
	}
	allprotos[numprotos++] = p;
	return p;
}

void compileProgram(MainNode *node)
{
	FuncDefStmtNode **defs = NULL;
	unsigned int numdefs = 0, capdefs = 0;
	unsigned int n;

	initNameSet(&escaping);
	initNameSet(&declaredHere);
	initNameSet(&rebound);
	analysisPoisoned = 0;

	collectFuncDefs(node->block, &defs, &numdefs, &capdefs);
	for (n = 0; n < numdefs; n++) {
		unsigned int i;
		noteFuncDef(defs[n]);
		/* A parameter rebinds its name inside the function. */
		for (i = 0; i < defs[n]->args->num; i++)
			noteBinding(defs[n]->args->ids[i]);
	}

	/* The analysis has to see the whole program before anything can be
	 * assumed about a name. */
	scanProcedure(NULL, node->block);
	for (n = 0; n < numdefs; n++)
		scanProcedure(defs[n]->args, defs[n]->body);

	for (n = 0; n < numdefs; n++)
		defs[n]->proto = compileProc(defs[n]->args, defs[n]->body, defs[n]);

	/* The main block holds the program's global variables, so its frame
	 * always owns a scope. */
	mainproto = compileProc(NULL, node->block, NULL);
	if (mainproto) mainproto->needscope = 1;

	jitCompileAll(allprotos, numprotos);

	if (getenv("LCI_DUMP")) {
		for (n = 0; n < numdefs; n++) {
			if (defs[n]->proto)
				dumpProto((Proto *)defs[n]->proto, defs[n]->name->iname->str);
			else
				fprintf(stderr, "; %s: not compiled\n", defs[n]->name->iname->str);
		}
		if (mainproto) dumpProto(mainproto, "main");
		else fprintf(stderr, "; main: not compiled\n");
	}

	free(defs);
	freeNameSet(&declaredHere);
}

Proto *getMainProto(void)
{
	return mainproto;
}

void freeProtos(void)
{
	unsigned int n, i;
	for (n = 0; n < numprotos; n++) {
		Proto *p = allprotos[n];
		for (i = 0; i < p->numk; i++)
			if (p->k[i]) deleteValueObject(p->k[i]);
		free(p->code);
		free(p->k);
		free(p->knames);
		free(p->kptrs);
		free(p->regnames);
		free(p->icscope);
		free(p->icfrom);
		free(p->icslot);
		free(p->icver);
		free(p);
	}
	jitFree();
	free(allprotos);
	allprotos = NULL;
	mainproto = NULL;
	numprotos = capprotos = 0;
	free(regstack);
	regstack = NULL;
	regstackcap = regstacktop = 0;
	freeNameSet(&escaping);
}
/**@}*/

/**
 * \name Diagnostics
 */
/**@{*/

static const char *opcodeNames[] = {
	"LOADK", "LOADI", "LOADB", "LOADNIL", "MOVE",
	"GETVAR", "SETVAR", "DECLVAR", "DECLARR", "UNDECL",
	"GETSLOT", "SETSLOT", "STMTSLOW",
	"ADD", "SUB", "MUL", "DIV", "MOD", "MAX", "MIN",
	"ADDI", "SUBI",
	"AND", "OR", "XOR", "NOT", "CONCAT",
	"EQ", "NEQ", "EQI", "SWEQ",
	"CAST",
	"JMP", "JMPF", "JMPT", "TESTJMPF",
	"CALL", "RET", "RETNIL", "BREAK", "ENDPROC",
	"PRINT", "INPUT",
	"PUSHSC", "POPSC"
};

/**
 * Prints a procedure's instructions, for debugging the compiler.
 */
void dumpProto(const Proto *p, const char *name)
{
	unsigned int n;
	fprintf(stderr, "; %s: %u registers, %u constants, IT in r%d, %s\n",
			name, p->numregs, p->numk, p->itreg,
			p->needscope ? "needs a scope" : "frameless");
	for (n = 0; n < p->numcode; n++) {
		const Instr *i = p->code + n;
		fprintf(stderr, "%4u  %-8s a=%-3u b=%-6d c=%-4d",
				n, opcodeNames[i->op], i->a, i->b, i->c);
		if (i->op == OPC_JMP || i->op == OPC_JMPF || i->op == OPC_JMPT)
			fprintf(stderr, "  -> %d", (int)n + 1 + i->b);
		else if (p->numk && i->b >= 0 && (unsigned)i->b < p->numk && p->knames[i->b])
			fprintf(stderr, "  ; %s", p->knames[i->b]->str);
		fprintf(stderr, "\n");
	}
}
/**@}*/
