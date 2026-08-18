/*
 * mayhem/harness_stubs.c -- safe no-op replacements for lci's host-facing
 * primitives, linked ONLY into the fuzz/standalone harness builds (never into the
 * CMake+ctest oracle build in build-tests/, which keeps the real implementations so
 * its own STDIO/SOCKS binding tests --
 * test/1.4-Tests/13-Bindings/{1-stdio,3-socket}/... -- still exercise real file and
 * socket code, as upstream intends).
 *
 * lci's CORE language (not an optional module) includes primitives that reach
 * outside the process when interpreting untrusted source:
 *
 *   - CAN HAS STDIO?  binding.c's OPEN/LUK/SCRIBBEL/AGEIN/CLOSE/DIAF wrap
 *     fopen/fread/fwrite/rewind/fclose/ferror directly, with an attacker-controlled
 *     filename/mode/data. mayhem/build.sh compiles binding.c for the harness with
 *     -Dfopen=mayhem_denied_fopen (and the other five), which redirects every one
 *     of those calls to the definitions below.
 *   - DUZ <cmd>  interpretSystemCommandExprNode() (interpreter.c) calls
 *     popen(cmd, "r") with an attacker-controlled shell command string -- genuine
 *     command execution reachable straight from parsed LOLCODE source.
 *     mayhem/build.sh compiles interpreter.c with -Dpopen=mayhem_denied_popen for
 *     the same reason.
 *   - CAN HAS SOCKS?  binding.c's BIND/LISTN/KONN/PUT/GET/CLOSE/RESOLV call
 *     inet.c's real socket code (inet_open/accept/connect/send/receive/close/
 *     lookup/setup). This file's definitions of those same functions are linked
 *     INSTEAD of inet.c for the harness build (inet.c is simply left out of the
 *     harness's source list in build.sh) -- no socket is ever created.
 *
 * Every stub below fails exactly the way the REAL implementation already fails
 * under an ordinary error condition -- fopen() returning NULL on a bad path,
 * inet_lookup() returning NULL when DNS resolution fails, popen() returning NULL
 * when the shell can't be spawned -- and lci's own binding.c wrappers already
 * handle those NULL/negative returns without crashing (fopenWrapper's result flows
 * into ferrorWrapper's short-circuited `file == NULL || ferror(file)` check;
 * interpretSystemCommandExprNode has an explicit `if (f == NULL)` guard). So
 * denying the operation outright degrades gracefully along a path upstream already
 * exercises -- a network-denied / permission-denied environment sees exactly this,
 * every time.
 */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>

#include "inet.h"

/* ---- CAN HAS STDIO? (fopen/fread/fwrite/fclose/rewind/ferror) ---- */

FILE *mayhem_denied_fopen(const char *path, const char *mode)
{
	(void)path;
	(void)mode;
	errno = EACCES;
	return NULL;
}

size_t mayhem_denied_fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	(void)ptr;
	(void)size;
	(void)nmemb;
	(void)stream;
	return 0;
}

size_t mayhem_denied_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	(void)ptr;
	(void)size;
	(void)nmemb;
	(void)stream;
	return 0;
}

int mayhem_denied_fclose(FILE *stream)
{
	(void)stream;
	return 0;
}

void mayhem_denied_rewind(FILE *stream)
{
	(void)stream;
}

int mayhem_denied_ferror(FILE *stream)
{
	(void)stream;
	return 1;
}

/* ---- DUZ <cmd> system-command primitive (popen/pclose) ---- */

FILE *mayhem_denied_popen(const char *command, const char *type)
{
	(void)command;
	(void)type;
	errno = EACCES;
	return NULL;
}

int mayhem_denied_pclose(FILE *stream)
{
	(void)stream;
	return -1;
}

/* ---- CAN HAS SOCKS? (inet_*, replacing inet.c entirely for the harness) ---- */

void inet_setup(inet_host_t *h, int protocol, const char *addr, unsigned short port)
{
	(void)protocol;
	(void)addr;
	(void)port;
	if (h) {
		h->fd = -1;
		h->protocol = 0;
	}
}

int inet_open(inet_host_t *h, int protocol, const char *addr, unsigned short port)
{
	(void)protocol;
	(void)addr;
	(void)port;
	if (h) {
		h->fd = -1;
		h->protocol = 0;
	}
	return -1;
}

int inet_accept(inet_host_t *h, inet_host_t *local)
{
	(void)local;
	if (h)
		h->fd = -1;
	return -1;
}

int inet_connect(inet_host_t *local, inet_host_t *remote)
{
	(void)local;
	(void)remote;
	return -1;
}

int inet_receive(inet_host_t *local, inet_host_t *remote, void *data, int size, int timeout)
{
	(void)local;
	(void)remote;
	(void)data;
	(void)size;
	(void)timeout;
	return -1;
}

int inet_send(inet_host_t *local, inet_host_t *remote, void *data, int size)
{
	(void)local;
	(void)remote;
	(void)data;
	(void)size;
	return -1;
}

int inet_close(inet_host_t *h)
{
	(void)h;
	return 0;
}

char *inet_lookup(const char *name)
{
	(void)name;
	return NULL;
}
