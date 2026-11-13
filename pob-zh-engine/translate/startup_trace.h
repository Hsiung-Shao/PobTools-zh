/*
** startup_trace.h - startup timeline for both pob-zh processes
**
** The launcher (pob-zh.exe) and the engine child (pob-zh.exe --engine) each
** write <exe dir>\PobTools\startup_<role>.txt, one line per stage:
**     +123.4 ms  <stage>
** with 0 being the moment Windows created the process (GetProcessTimes), so the
** loader / CRT cost before wWinMain shows up as the first line's offset.
**
** Always on, overwritten on the next start. Every failure is silent -- a timing
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

/* Append one stage line (printf-style). No-op until startup_trace_begin has run. */
void startup_trace_mark(const char* fmt, ...);

/* Milliseconds since process creation (also usable without the file). */
double startup_trace_now_ms(void);

#endif
