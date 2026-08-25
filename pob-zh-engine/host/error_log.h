// One place every failure in PobTools gets written down: PobTools\logs\error-<date>.log
//
// Why this exists: until v0.28.0 the only log in the project was app_update.cpp's
// own app_update_log.txt. Everything else -- a POB source patch whose anchor
// drifted, a bookmark that failed to save, a data file that would not parse --
// failed on screen for a second and left nothing behind. When a user says "it
// stopped working" there was nothing to ask them for.
//
// Three rules follow from that purpose:
//
//   * AT MOST TEN LINES PER INCIDENT. The same feature tag with the same message
//     is written ten times per run and then never again; the tenth line says so,
//     because silence that is not announced cannot be told apart from the problem
//     having stopped. A failure on a per-frame path would otherwise write sixty
//     lines a second, and a log that floods is a log nobody opens.
//
//   * FAILURES ONLY. A file that also records normal events cannot answer "did
//     anything go wrong today?" at a glance. An empty (or absent) file means the
//     install is healthy; anything in it is something that actually broke.
//
//   * ONE FAILURE IS ONE LINE, carrying the version and which feature produced
//     it. A report that says "v0.28.0 [inject] ..." tells us where to look
//     without a round trip; embedded newlines are collapsed so a multi-line
//     message cannot masquerade as several separate failures.
//
//   * LOGGING NEVER FAILS LOUDLY. Every entry point swallows its own errors. A
//     logger that interrupts the user because it could not write a log is worse
//     than no logger at all.
//
// Linked into BOTH pob-zh.exe and SimpleGraphic.dll -- the engine's ui_api.cpp
// is where the POB patches live, and those are the failures worth catching. The
// two copies are independent, which is fine: appends through FILE_APPEND_DATA
// are atomic, so a line is never torn even with two writers.
#pragma once

#include <string>

namespace PobLog {

// One failure, one line. `feature` is a short stable ascii tag -- see the table
// in error_log.cpp; keep new ones lowercase and hyphenated so a user's report
// can be grepped for without guessing at capitalisation.
void Error(const char* feature, const std::string& msg);

// <install>\PobTools\logs\, created if absent. Also what the settings page's
// "open the log folder" button opens, which is why it creates: a button that
// opens nothing looks broken.
std::wstring LogDir();

// Deletes error-YYYY-MM-DD.log files older than `keepDays`. Returns how many.
// Only files in LogDir() whose names match that exact shape are considered --
// never a wildcard sweep of the directory.
int PruneOlderThan(int keepDays);

// Forgets how many times each incident has been written. Self-test only: the cap
// below is per process on purpose, so nothing in the product should clear it.
void ResetCapsForTest();

// Redirects the log to `dir` for the duration of a self-test. The install's real
// log must never be written to by a test run, for the same reason the regex
// self-test uses a scratch directory: it is the user's evidence, not ours.
// Pass an empty string to go back to the real location.
void SetDirForTest(const std::wstring& dir);

} // namespace PobLog

// --error-log-selftest. Declared here beside the module it exercises, the way
// every other self-test in this project hangs off its own header.
int RunErrorLogSelfTest(const std::wstring& exeDir);
