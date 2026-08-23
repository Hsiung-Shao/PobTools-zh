// Headless checks for the search-string generator ("--regex-selftest").
//
// Two halves. The first is synthetic and proves the rules the generator claims:
// a token never crosses a rolled number, anchors are used when nothing else can
// separate two entries, and the checker itself can fail (a deliberately broken
// query must be reported broken -- otherwise every other PASS is worthless).
// The second runs the shipped Data\regex_*.json through hundreds of pseudo-random
// selections and demands the same invariant every time: the query finds every
// pick, and finds nothing else.
//
// Returns 0 when everything passes. Writes regex_selftest.txt next to the exe as
// well as printing, because this binary is a GUI subsystem program and its stdout
// is only there when the caller went out of its way to redirect it.
#pragma once

#include <string>

int RunRegexSelfTest(const std::wstring& exeDir);
