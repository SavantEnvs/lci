/*
 * mayhem/fuzz_lci.c -- in-process libFuzzer harness for the FULL lci
 * LOLCODE front end AND tree-walking interpreter.
 *
 *     scanBuffer()  ->  tokenizeLexemes()  ->  parseMainNode()  ->  interpretMainNodeScope()
 *     (lexer.c)         (tokenizer.c)          (parser.c, unicode.c, error.c)   (interpreter.c, binding.c)
 *
 * This is the same pipeline() sequence main.c drives for `lci <file>` (see main.c's
 * pipeline()), so the harness fuzzes lci's real bug surface end to end: the lexer,
 * tokenizer, parser and the VM (variable storage, casts, arithmetic, string ops,
 * arrays, functions/recursion, and the STDLIB/STDIO/SOCKS/STRING library bindings).
 *
 * ---- why interpretation is safe to run here (it previously was not) ----
 *
 * An earlier iteration of this harness stopped after parseMainNode() specifically
 * because interpreting untrusted LOLCODE reaches outside the process in ways that are
 * unsafe to fuzz unmodified:
 *
 *   1. IM IN YR <loop> with no break condition is a genuine unbounded native loop
 *      (interpreter.c's interpretLoopStmtNode is a bare `while (1) { ... }`) -- a
 *      trivial one-line program can hang the process forever.
 *   2. CAN HAS STDIO?  binding.c's OPEN/LUK/SCRIBBEL/AGEIN/CLOSE wrap the real
 *      fopen/fread/fwrite/rewind/fclose with an attacker-controlled filename, mode,
 *      and data -- arbitrary file read/write on the host.
 *   3. DUZ <cmd>  (SystemCommandExprNode, interpreter.c's
 *      interpretSystemCommandExprNode) calls popen(cmd, "r") with an
 *      attacker-controlled shell command string -- arbitrary command execution.
 *   4. CAN HAS SOCKS?  binding.c's BIND/LISTN/KONN/PUT/GET/CLOSE/RESOLV drive
 *      inet.c's real TCP socket code -- arbitrary outbound/listening sockets.
 *   5. GIMMEH reads a line directly from stdin (interpreter.c's
 *      interpretInputStmtNode, a raw getchar()/feof(stdin) loop) -- can block
 *      forever if stdin is attached to something that never produces EOF.
 *
 * (1) is bounded the same way any other hang is: Mayhem/libFuzzer owns per-exec
 * timeouts, the harness does not. This file installs NO timer, alarm, watchdog or
 * non-local exit of its own -- see docs/netnew-worker-prompt.md SS6b ("Hangs are
 * findings -- Mayhem owns timeouts, the harness never does") for why: libFuzzer
 * already owns SIGALRM for its own -timeout mechanism, a harness-side timer makes
 * results depend on host speed (non-deterministic across Mayhem/training hosts),
 * and -- the exact failure mode measured here during this integration -- a
 * watchdog that _exit()s makes Mayhem's startup/regression sanity probe fail
 * outright for every run once a hanging input reaches the accumulated seed corpus
 * (a hard-exiting harness is a harness Mayhem cannot probe; an ordinary timeout is
 * not). The Mayhemfile's cmd-level `timeout: 30` (mayhem/Mayhemfile) is the one
 * right place for this bound: IM IN YR <loop> with no reachable exit condition, or
 * a genuinely blocked native call, both surface as an ordinary libFuzzer timeout
 * report there, which Mayhem records like a crash -- an expected LOLCODE-level
 * finding (the language permits non-terminating loops by design), not a harness
 * bug to engineer around. See mayhem/lci-buggy-mhh-run-17/known-findings/
 * gimmeh-eof-unbounded-loop/ for a documented hang of this exact shape (kept out of
 * testsuite/, which is replayed on every run including the initial sanity probe).
 * (2)-(4) are neutered at BUILD time, not in this file: mayhem/build.sh compiles
 * binding.c and interpreter.c for this harness with -Dfopen=mayhem_denied_fopen
 * (and five siblings) and -Dpopen=mayhem_denied_popen, redirecting every one of
 * those calls to the safe no-ops in mayhem/harness_stubs.c; that same file's
 * inet_open/accept/connect/send/receive/close/lookup/setup definitions are linked
 * INSTEAD of inet.c, so no socket is ever created. See harness_stubs.c's header
 * comment for the full rationale (each stub fails exactly the way the real
 * implementation already fails under an ordinary error condition, which lci's own
 * wrappers already handle without crashing).
 * (5) is handled by redirecting stdin to /dev/null before the first input, so
 * GIMMEH's getchar()/feof(stdin) loop (interpreter.c's interpretInputStmtNode)
 * always observes immediate EOF and returns an empty YARN -- a single GIMMEH call
 * never itself blocks. A LOLCODE program that calls GIMMEH repeatedly in a loop and
 * never treats that empty/EOF read as a stop condition (see (1)) is still a genuine
 * native infinite loop at the language level, bounded the same way (1) is: by the
 * Mayhemfile `timeout:`, not by anything in this file.
 *
 * Tested empirically: `HAI 1.3 / IM IN YR loop UPPIN YR i TIL BOTH SAEM i AN -1 / VISIBLE i /
 * IM OUTTA YR loop / KTHXBYE` (a loop whose exit condition can never become true under this
 * interpreter's integer semantics) hangs interpretMainNodeScope() indefinitely -- as does the
 * LOLCODE Brainfuck interpreter at
 * mayhem/lci-buggy-mhh-run-17/known-findings/gimmeh-eof-unbounded-loop/repro.lol (its outer
 * `IM IN YR main` loop calls GIMMEH repeatedly and never stops on an empty read). Both are
 * EXPECTED LOLCODE behavior (the language permits non-terminating loops and blocking reads by
 * design), not a memory-safety defect -- so libFuzzer's own per-exec timeout (bounded by the
 * Mayhemfile's `timeout: 30`) reports them as an ordinary timeout, which Mayhem records like a
 * crash, same as any other finding. (A genuine memory-safety defect surfaced while validating the
 * neutered library stubs below IS reported, at
 * mayhem/lci-buggy-mhh-run-17/known-findings/socks-resolv-null-string-deref/.)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lexer.h"
#include "tokenizer.h"
#include "parser.h"
#include "interpreter.h"

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	/* Make stdin observe EOF immediately, regardless of what libFuzzer/Mayhem
	 * attaches to fd 0 -- see (5) above. */
	FILE *ignored = freopen("/dev/null", "r", stdin);
	(void)ignored;
	return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	/* scanBuffer takes an unsigned int length; ignore inputs it cannot address. */
	if (size > 0x7fffffffu)
		return 0;

	char *buffer = (char *)malloc(size + 1);
	if (!buffer)
		return 0;
	if (size)
		memcpy(buffer, data, size);
	buffer[size] = '\0';

	/* scanBuffer copies what it needs; the CLI's pipeline() frees the buffer
	 * right after, so do we. */
	LexemeList *lexemes = scanBuffer(buffer, (unsigned int)size, "fuzz");
	free(buffer);
	if (!lexemes)
		return 0;

	Token **tokens = tokenizeLexemes(lexemes);
	deleteLexemeList(lexemes);
	if (!tokens)
		return 0;

	MainNode *node = parseMainNode(tokens);
	deleteTokens(tokens);
	if (!node)
		return 0;

	/* The deep target: drive the VM over the parsed program. A fresh, NULL scope
	 * mirrors main.c's own pipeline() call exactly, so a failure/timeout here is
	 * a genuine interpreter-level behavior, not a harness artifact of reusing
	 * state across inputs. Bounded by the Mayhemfile's `timeout:`, not by
	 * anything in this file -- see header comment. */
	interpretMainNodeScope(node, NULL);

	deleteMainNode(node);
	return 0;
}
