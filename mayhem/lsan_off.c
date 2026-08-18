/* Disable LeakSanitizer preventively (ASan's memory-corruption checks + UBSan stay on) — this
 * fleet fuzzes for memory corruption, not leaks. Build-time hook only, per PORTING.md. */
int __lsan_is_turned_off(void) { return 1; }
