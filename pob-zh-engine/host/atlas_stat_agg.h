// PobTools atlas planner stat aggregation.
//
// Groups the stat lines of allocated nodes by their number-normalized English
// template and sums single-number lines, so "2% increased X" from four nodes
// and "3% increased X" from five reads as one "23% increased X" row. Lines
// without numbers are booleans (shown once); lines with several numbers keep
// the classic "xN raw line" display.
//
// Pure data, no ImGui/GL. Chinese display is injected as a lookup callback and
// backfilled by value matching (see BuildStatAggDisplay), so it never touches
// the aggregation keys. Verified headless via "pob-zh.exe --atlas-agg-selftest".
#pragma once

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// One number found inside a stat line.
struct StatNumTok {
	size_t pos = 0, len = 0; // byte range in the source string (sign included)
	double val = 0.0;
	bool plus = false;       // written with an explicit leading '+'
};

struct StatAggGroup {
	enum Kind { kSummed, kBoolean, kMulti };
	Kind kind = kBoolean;
	std::string key;         // kSummed: en template ('#' placeholder); else raw en line
	double sum = 0.0;        // kSummed only
	int count = 0;           // contributing lines (displayed only for kMulti)
	bool plusSign = false;   // representative en token had an explicit '+'
	// distinct contributing en lines + their single value (kSummed); tried in
	// order for the zh backfill so one untranslated member can't sink the group
	std::vector<std::pair<std::string, double>> reps;
	// display cache, filled by BuildStatAggDisplay:
	std::string dispEn, dispZh;
	bool zhFallback = false; // zh lookup/backfill failed -> dispZh == dispEn
	std::string searchKey;   // lowered "en\nzh" for the panel filter
};

// Scans ASCII numbers ("\d+(\.\d+)?", optional +/- sign when not preceded by an
// alphanumeric). Safe on UTF-8: CJK bytes are >= 0x80 and never match.
std::vector<StatNumTok> FindStatNumbers(const std::string& s);

// 24 -> "24", 0.75 -> "0.75" (trailing zeros trimmed); forcePlus -> "+24".
std::string FmtStatNum(double v, bool forcePlus);

// ASCII-only lowercase copy (bytes >= 0x80 untouched).
std::string ToLowerAscii(const std::string& s);

// Phase 1: classify `line` and accumulate it into `groups`; `pos` maps the
// prefixed key ("S:"/"B:"/"M:") to the group index.
void AccumulateStatLine(const std::string& line, std::vector<StatAggGroup>& groups,
                        std::unordered_map<std::string, size_t>& pos);

// Phase 2: fill dispEn/dispZh/zhFallback/searchKey on every group. zhLookup(en)
// returns the zh line, or en itself when untranslated (AtlasI18n::StatLine
// semantics); pass nullptr when zh display is unavailable.
void BuildStatAggDisplay(std::vector<StatAggGroup>& groups,
                         const std::function<std::string(const std::string&)>* zhLookup);

// Headless logic check for "pob-zh.exe --atlas-agg-selftest": synthetic cases,
// console report + atlas_agg_selftest.txt next to the exe, 0 on full pass.
int RunAtlasAggSelfTest(const std::wstring& exeDir);
