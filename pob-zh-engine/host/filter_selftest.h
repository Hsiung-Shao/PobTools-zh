// Headless data-layer check for the filter editor (pob-zh.exe --filter-selftest):
// parser round-trip, operator handling, document CRUD, disable/restore and
// selection anchoring over synthetic filters. Prints a PASS/FAIL report to the
// console, writes it next to the exe as filter_selftest.txt, and returns a
// non-zero exit code on any failure.
#pragma once

#include <string>

int RunFilterSelfTest(const std::wstring& exeDir);

// Headless probe of the drop-preview "import item from game" path
// (pob-zh.exe --filter-import-probe [item.txt [file.filter]]): reads the item
// text from the clipboard (default) or a UTF-8 file, parses it exactly like
// the paste button does, evaluates it against the given .filter (default:
// Filters\default.filter next to the exe) and prints the parsed fields,
// warnings and the matched rule. Returns non-zero when parsing fails.
int RunFilterImportProbe(const std::wstring& exeDir, const std::wstring& itemFile,
                         const std::wstring& filterFile);
