/**
 * A global string interner.  Identifier names are compared and hashed
 * constantly at run time; interning them turns every such comparison into a
 * pointer equality test and removes the per-access malloc/strcpy that
 * resolving a name used to require.
 *
 * Interned names live for the lifetime of the process and are never freed.
 *
 * \file   intern.h
 *
 * \author Justin J. Meza
 */

#ifndef __INTERN_H__
#define __INTERN_H__

#include <stddef.h>

/**
 * A unique, immortal representation of a string.  Two interned names are equal
 * if and only if their pointers are equal.
 */
typedef struct name {
	unsigned int hash; /**< Cached hash of \a str. */
	unsigned int len;  /**< Length of \a str, excluding the terminator. */
	char str[1];       /**< The NUL-terminated characters (over-allocated). */
} Name;

/**
 * Interns a NUL-terminated string.
 */
const Name *internName(const char *s);

/**
 * Interns a string of a known length, which may contain no NUL bytes.
 */
const Name *internNameLen(const char *s, size_t len);

#endif /* __INTERN_H__ */
