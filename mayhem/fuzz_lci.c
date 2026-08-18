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
 * (1) is bounded PRIMARILY by an interpreter-internal step budget, injected into a
 * build-time SHADOW COPY of interpreter.c (never the committed interpreter.c
 * in-repo) by mayhem/build.sh -- see that file's header comment (search
 * "FUZZ_SHADOW") for the full rationale, including why a source-level injection was
 * the only option: interpretStmtNode()'s only caller and interpretLoopStmtNode()'s
 * `while (1)` both live inside interpreter.c itself, so neither a preprocessor -D
 * rename (like HOST_DENY_DEFS in build.sh) nor a linker `ld --wrap=interpretStmtNode`
 * interposition can reach them from outside that file -- the latter was tried first
 * specifically to avoid touching any interpreter.c-shaped source at all, and does
 * NOT work on this toolchain (clang/lld resolve that intra-object call directly,
 * bypassing --wrap entirely; verified empirically, a wrapped build still hung past a
 * 30s deadline on repro.lol with zero wrapper invocations observed). Once a single
 * LLVMFuzzerTestOneInput() call has interpreted MAYHEM_STMT_BUDGET (50,000 --
 * see build.sh's FUZZ_SHADOW comment for why this is tuned much lower than it
 * sounds like it needs to be: this interpreter's per-statement cost is high enough
 * that a "generous" 5,000,000 took ~12s, past this file's own 1.5s watchdog
 * deadline) statements/loop-iterations, interpretMainNodeScope() unwinds and
 * returns a normal error (1) exactly like any other interpretation failure, and
 * this function simply returns 0 -- the fuzzer moves on to the next input instead
 * of the whole process dying. This is what makes a hanging LOLCODE program
 * (fuzzer-discovered OR already
 * sitting in Mayhem's accumulated server-side corpus, replayed on every future run)
 * a cheap per-input no-op instead of a run-ending failure.
 *
 * A hard process-level watchdog (timer_create(CLOCK_MONOTONIC)/SIGRTMIN+10, ~1.5s)
 * remains as a LAST-RESORT backstop for a genuinely blocked native call the step
 * budget cannot see (e.g. something stuck in a libc call with no interpreter
 * statement ever dispatched) -- exactly like an unbounded VM/interpreter loop is
 * bounded in other engine integrations (see docs/netnew-worker-prompt.md SS6b). It
 * deliberately does NOT use alarm()/setitimer(ITIMER_REAL, ...)/SIGALRM: libFuzzer
 * itself owns SIGALRM (its own -timeout mechanism), so stacking another SIGALRM
 * handler on top risks starving or being clobbered by libFuzzer's. timer_create on a
 * dedicated real-time signal is independent of that.
 * (2)-(4) are neutered at BUILD time, not in this file: mayhem/build.sh compiles
 * binding.c and interpreter.c for this harness with -Dfopen=mayhem_denied_fopen
 * (and five siblings) and -Dpopen=mayhem_denied_popen, redirecting every one of
 * those calls to the safe no-ops in mayhem/harness_stubs.c; that same file's
 * inet_open/accept/connect/send/receive/close/lookup/setup definitions are linked
 * INSTEAD of inet.c, so no socket is ever created. See harness_stubs.c's header
 * comment for the full rationale (each stub fails exactly the way the real
 * implementation already fails under an ordinary error condition, which lci's own
 * wrappers already handle without crashing).
 * (5) is handled TWO ways: (a) stdin is redirected to /dev/null before the first
 * input, so GIMMEH's getchar()/feof(stdin) loop (interpreter.c's
 * interpretInputStmtNode) always observes immediate EOF and returns an empty YARN --
 * a single GIMMEH call never itself blocks; (b) a LOLCODE program that calls GIMMEH
 * repeatedly in a loop and never treats that empty/EOF read as a stop condition (see
 * (1)'s repro.lol) is still a genuine native infinite loop at the language level, so
 * it is bounded the same way (1) is: by the interpreter step budget, backstopped by
 * the process watchdog.
 *
 * Tested empirically: `HAI 1.3 / IM IN YR loop UPPIN YR i TIL BOTH SAEM i AN -1 / VISIBLE i /
 * IM OUTTA YR loop / KTHXBYE` (a loop whose exit condition can never become true under this
 * interpreter's integer semantics) hangs interpretMainNodeScope() indefinitely with the step
 * budget AND the watchdog both disabled; with the step budget active, interpretStmtNode()
 * returns NULL once MAYHEM_STMT_BUDGET statements have executed (here, once VISIBLE has printed
 * that many lines) and this function returns 0 normally, no process exit. A LOLCODE Brainfuck
 * interpreter (mayhem/lci/known-findings/gimmeh-eof-unbounded-loop/repro.lol) independently
 * exercises the same class of hang (its outer instruction loop calls GIMMEH repeatedly and never
 * stops on an empty read) and is likewise bounded by the step budget, not the watchdog: its
 * `IM IN YR main` loop dispatches interpretInputStmtNode/interpretFuncCallExprNode/
 * interpretIfThenElseStmtNode via interpretStmtNode every iteration, so the budget is what fires.
 * Both are bounding an EXPECTED LOLCODE behavior (the language permits non-terminating loops and
 * blocking reads by design), not a memory-safety defect -- so they are handled as harness-bounding
 * concerns, not reported as findings. (A genuine memory-safety defect surfaced while validating the
 * neutered library stubs below IS reported, at
 * mayhem/lci/known-findings/socks-resolv-null-string-deref/.)
 */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lexer.h"
#include "tokenizer.h"
#include "parser.h"
#include "interpreter.h"

/* Hard process-level deadline for one interpretMainNodeScope() call, backstopping
 * the interpreter step budget (interpreter.c) for a genuinely blocked native call
 * (no interpreter statement ever dispatched, so the step budget can't see it).
 * timer_create(CLOCK_MONOTONIC) on a dedicated real-time signal, NOT
 * alarm(2)/setitimer(ITIMER_REAL, ...)/SIGALRM: libFuzzer itself owns SIGALRM (its
 * own -timeout mechanism), so a second SIGALRM handler installed on top of/before
 * libFuzzer's own risks the two clobbering each other. SIGRTMIN+10 is a signal
 * nothing else in this process uses. */
#define MAYHEM_HARD_DEADLINE_US 1500000L /* 1.5s */
#define MAYHEM_WATCHDOG_SIGNAL (SIGRTMIN + 10)

static timer_t mayhem_watchdog_timer;
static int mayhem_watchdog_timer_ready = 0;

static void mayhem_watchdog_fired(int sig)
{
	/* write() + _exit() are both async-signal-safe, so this can't itself
	 * deadlock inside libc/malloc state the interrupted call may hold locked.
	 * _exit(70) gives libFuzzer a clean, distinguishable, non-zero process exit
	 * that it reports and moves past, exactly like its own -timeout mechanism. */
	static const char msg[] =
	    "mayhem: hard watchdog deadline exceeded (blocked native call the "
	    "interpreter step budget could not see), aborting\n";
	ssize_t ignored = write(2, msg, sizeof(msg) - 1);
	(void)ignored;
	(void)sig;
	_exit(70);
}

static void mayhem_install_watchdog(void)
{
	struct sigaction sa;
	struct sigevent sev;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = mayhem_watchdog_fired;
	sigemptyset(&sa.sa_mask);
	sigaction(MAYHEM_WATCHDOG_SIGNAL, &sa, NULL);

	memset(&sev, 0, sizeof(sev));
	sev.sigev_notify = SIGEV_SIGNAL;
	sev.sigev_signo = MAYHEM_WATCHDOG_SIGNAL;
	sev.sigev_value.sival_ptr = &mayhem_watchdog_timer;
	/* Non-fatal if this fails (e.g. RT signal exhaustion) -- the step budget is
	 * the primary defense; mayhem_arm_deadline()/mayhem_disarm_deadline() below
	 * just become no-ops. */
	mayhem_watchdog_timer_ready =
	    (timer_create(CLOCK_MONOTONIC, &sev, &mayhem_watchdog_timer) == 0);
}

static void mayhem_arm_deadline(void)
{
	struct itimerspec its;
	if (!mayhem_watchdog_timer_ready) return;
	memset(&its, 0, sizeof(its));
	its.it_value.tv_sec = (long)(MAYHEM_HARD_DEADLINE_US / 1000000L);
	its.it_value.tv_nsec = (long)(MAYHEM_HARD_DEADLINE_US % 1000000L) * 1000L;
	timer_settime(mayhem_watchdog_timer, 0, &its, NULL);
}

static void mayhem_disarm_deadline(void)
{
	struct itimerspec its;
	if (!mayhem_watchdog_timer_ready) return;
	memset(&its, 0, sizeof(its));
	timer_settime(mayhem_watchdog_timer, 0, &its, NULL);
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	(void)argc;
	(void)argv;
	mayhem_install_watchdog();
	/* Defense #2 against GIMMEH blocking: make stdin observe EOF immediately,
	 * regardless of what libFuzzer/Mayhem attaches to fd 0. Non-fatal if it
	 * fails -- the watchdog still bounds a stuck getchar() either way. */
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

	/* The deep target: drive the VM over the parsed program, bounded by the
	 * hard watchdog armed for the duration of the call (see header comment). A
	 * fresh, NULL scope mirrors main.c's own pipeline() call exactly, so a
	 * failure/timeout here is a genuine interpreter-level behavior, not a
	 * harness artifact of reusing state across inputs. */
	mayhem_arm_deadline();
	interpretMainNodeScope(node, NULL);
	mayhem_disarm_deadline();

	deleteMainNode(node);
	return 0;
}
