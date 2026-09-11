/**
 * \file intern.c
 *
 * \author Justin J. Meza
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "intern.h"

/**
 * The interner is a power-of-two open-addressing table of \c Name pointers
 * with linear probing.  It is kept at or below a 0.7 load factor.
 */
static const Name **table = NULL;
static unsigned int tablecap = 0;
static unsigned int tablenum = 0;

/**
 * FNV-1a, which is short, fast and good enough for identifier-length keys.
 */
static unsigned int hashBytes(const char *s, size_t len)
{
	unsigned int h = 2166136261u;
	size_t n;
	for (n = 0; n < len; n++) {
		h ^= (unsigned char)s[n];
		h *= 16777619u;
	}
	/* Reserve 0 so a zeroed slot is unambiguously empty. */
	return h ? h : 1u;
}

static void growTable(void)
{
	unsigned int newcap = tablecap ? tablecap * 2 : 1024;
	const Name **newtable = calloc(newcap, sizeof(Name *));
	unsigned int n;
	if (!newtable) {
		perror("calloc");
		exit(EXIT_FAILURE);
	}
	for (n = 0; n < tablecap; n++) {
		const Name *name = table[n];
		unsigned int i;
		if (!name) continue;
		i = name->hash & (newcap - 1);
		while (newtable[i]) i = (i + 1) & (newcap - 1);
		newtable[i] = name;
	}
	free(table);
	table = newtable;
	tablecap = newcap;
}

const Name *internNameLen(const char *s, size_t len)
{
	unsigned int hash = hashBytes(s, len);
	unsigned int i;
	Name *name;

	if (tablenum * 10 >= tablecap * 7) growTable();

	i = hash & (tablecap - 1);
	while (table[i]) {
		const Name *cand = table[i];
		if (cand->hash == hash
				&& cand->len == len
				&& !memcmp(cand->str, s, len))
			return cand;
		i = (i + 1) & (tablecap - 1);
	}

	name = malloc(sizeof(Name) + len);
	if (!name) {
		perror("malloc");
		exit(EXIT_FAILURE);
	}
	name->hash = hash;
	name->len = (unsigned int)len;
	memcpy(name->str, s, len);
	name->str[len] = '\0';

	table[i] = name;
	tablenum++;
	return name;
}

const Name *internName(const char *s)
{
	return internNameLen(s, strlen(s));
}
