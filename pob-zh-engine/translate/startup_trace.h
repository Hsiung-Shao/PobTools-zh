/*
** startup_trace.h - startup timeline for both pob-zh processes
**
** The launcher (pob-zh.exe) and the engine child (pob-zh.exe --engine) each
** write <exe dir>\PobTools\startup_<role>.txt, one line per stage:
**     +123.4 ms  <stage>
** with 0 being the moment Windows created the process (GetProcessTimes), so the
** loader / CRT / DLL-resolution cost before wWinMain shows up as the first
** line's offset rather than vanishing.
**
** Always on: a few hundred bytes per start, overwritten on the next one. When
** someone reports "it takes ages to open", this file is what to ask for.
** Every failure (no PobTools\ folder, read-only install) is silent -- a timing
** aid must never become a startup failure of its own.
**
** Compiled into BOTH targets (the /MT host and the /MD engine DLL), exactly like
** translation_manager.cpp; they never share state.
*/
#ifndef POB_ZH_STARTUP_TRACE_H
#define POB_ZH_STARTUP_TRACE_H

/* First call picks the file: role = "launcher" / "engine" (ASCII). Later calls
** ignore the role. Must run before any thread that will mark (both callers do it
** at process entry); after that marking is safe from any thread -- lines from
** different threads are whole lines but may interleave in order. */
void startup_trace_begin(const char* role);

/* Append one stage line. No-op until startup_trace_begin has run. printf-style
** so a stage can carry a number ("translation ready, waited %d ms"). */
void startup_trace_mark(const char* fmt, ...);

/* Milliseconds since process creation (also usable without the file). */
double startup_trace_now_ms(void);

#endif
