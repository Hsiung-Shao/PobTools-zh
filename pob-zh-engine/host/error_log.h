// One place every failure in PobTools gets written down: PobTools\logs\error-<date>.log
//
// Rules:
//
//   * AT MOST TEN LINES PER INCIDENT. The same feature tag with the same message
//     is written ten times per run and the tenth line says so (unannounced
//     silence looks like the problem stopping); a per-frame failure must not flood.
//
//   * FAILURES ONLY. An empty (or absent) file means the install is healthy.
//
//   * ONE FAILURE IS ONE LINE, carrying the version and feature tag
//     ("v0.28.0 [inject] ..."); embedded newlines are collapsed so one message
//     cannot masquerade as several failures.
//
//   * LOGGING NEVER FAILS LOUDLY. Every entry point swallows its own errors.
//
// Linked into BOTH pob-zh.exe and SimpleGraphic.dll (the engine's POB patches
// live there). The two copies are independent: appends through FILE_APPEND_DATA
// are atomic, so a line is never torn even with two writers.
#pragma once

#include <string>

namespace PobLog {

// One failure, one line. `feature` is a short stable ascii tag -- see the table
// in error_log.cpp; keep new ones lowercase and hyphenated so a user's report
// can be grepped for without guessing at capitalisation.
void Error(const char* feature, const std::string& msg);

// One NORMAL event, one line, in a separate file: PobTools\logs\diag-<date>.log.
//
// The error log stays failures-only; paths we cannot run here (CrossOver on a
// Mac) are diagnosed from what the machine that CAN run them writes down, where
// "nothing went wrong" is not the same as "it worked".
//
// Same guarantees as Error (one line, newlines collapsed, never throws). The cap
// is per feature tag and higher (200): a trace truncated at ten lines is useless.
void Diag(const char* feature, const std::string& msg);

// <install>\PobTools\logs\, created if absent. Also what the settings page's
// "open the log folder" button opens, which is why it creates: a button that
// opens nothing looks broken.
std::wstring LogDir();

// Deletes error-YYYY-MM-DD.log and perf-YYYY-MM-DD.log files, and the watchdog's hang-/crash-<date>-...
// .txt reports, older than `keepDays`. Returns how many. Only files in LogDir()
// whose names match those exact shapes are considered -- never a wildcard sweep
// of the directory.
int PruneOlderThan(int keepDays);

// Forgets how many times each incident has been written. Self-test only: the cap
// below is per process on purpose, so nothing in the product should clear it.
void ResetCapsForTest();

// Redirects the log to `dir` for the duration of a self-test: the install's real
// log is the user's evidence and must not be written by a test run.
// Pass an empty string to go back to the real location.
void SetDirForTest(const std::wstring& dir);

} // namespace PobLog

// --error-log-selftest. Declared here beside the module it exercises, the way
// every other self-test in this project hangs off its own header.
int RunErrorLogSelfTest(const std::wstring& exeDir);
