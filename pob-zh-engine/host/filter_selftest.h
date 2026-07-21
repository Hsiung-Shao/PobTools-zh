// Headless data-layer check for the filter editor (pob-zh.exe --filter-selftest):
// parser round-trip, operator handling, document CRUD, disable/restore and
// selection anchoring over synthetic filters. Prints a PASS/FAIL report to the
// console, writes it next to the exe as filter_selftest.txt, and returns a
// non-zero exit code on any failure.
#pragma once

#include <string>

int RunFilterSelfTest(const std::wstring& exeDir);
