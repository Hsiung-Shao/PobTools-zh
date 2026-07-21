#include "atlas_stat_agg.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h> // AttachConsole for the selftest CLI

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// ---- scanning / formatting ----------------------------------------------------

std::vector<StatNumTok> FindStatNumbers(const std::string& s)
{
	std::vector<StatNumTok> out;
	size_t i = 0, n = s.size();
	while (i < n) {
		if (!isdigit((unsigned char)s[i])) { i++; continue; }
		size_t start = i;
		// fold a +/- sign in only when it isn't part of a word or range ("2-3")
		if (start > 0 && (s[start - 1] == '+' || s[start - 1] == '-')) {
			bool signOk = start == 1 || !isalnum((unsigned char)s[start - 2]);
			if (signOk) start--;
		}
		size_t j = i;
		while (j < n && isdigit((unsigned char)s[j])) j++;
		if (j + 1 < n && s[j] == '.' && isdigit((unsigned char)s[j + 1])) {
			j++;
			while (j < n && isdigit((unsigned char)s[j])) j++;
		}
		StatNumTok t;
		t.pos = start;
		t.len = j - start;
		t.val = strtod(s.c_str() + start, nullptr);
		t.plus = s[start] == '+';
		out.push_back(t);
		i = j;
	}
	return out;
}

std::string FmtStatNum(double v, bool forcePlus)
{
	double r = std::round(v * 10000.0) / 10000.0; // shed double-accumulation noise
	char buf[64];
	if (std::fabs(r - std::round(r)) < 1e-9) {
		snprintf(buf, sizeof(buf), "%lld", (long long)std::llround(r));
	} else {
		snprintf(buf, sizeof(buf), "%.4f", r);
		char* end = buf + strlen(buf) - 1;
		while (end > buf && *end == '0') *end-- = '\0';
		if (end > buf && *end == '.') *end = '\0';
	}
	std::string s = buf;
	if (forcePlus && r >= 0) s.insert(s.begin(), '+');
	return s;
}

std::string ToLowerAscii(const std::string& s)
{
	std::string out = s;
	for (char& c : out)
		if ((unsigned char)c < 0x80) c = (char)tolower((unsigned char)c);
	return out;
}

// ---- aggregation --------------------------------------------------------------

void AccumulateStatLine(const std::string& line, std::vector<StatAggGroup>& groups,
                        std::unordered_map<std::string, size_t>& pos)
{
	std::vector<StatNumTok> toks = FindStatNumbers(line);

	std::string key;
	StatAggGroup::Kind kind;
	if (toks.empty()) {
		kind = StatAggGroup::kBoolean;
		key = "B:" + line;
	} else if (toks.size() == 1) {
		kind = StatAggGroup::kSummed;
		key = "S:" + line.substr(0, toks[0].pos) + "#" + line.substr(toks[0].pos + toks[0].len);
	} else {
		kind = StatAggGroup::kMulti;
		key = "M:" + line;
	}

	auto it = pos.find(key);
	if (it == pos.end()) {
		pos.emplace(key, groups.size());
		StatAggGroup g;
		g.kind = kind;
		g.key = key;
		g.count = 1;
		if (kind == StatAggGroup::kSummed) {
			g.sum = toks[0].val;
			g.plusSign = toks[0].plus;
			g.reps.push_back({ line, toks[0].val });
		}
		groups.push_back(std::move(g));
		return;
	}

	StatAggGroup& g = groups[it->second];
	g.count++;
	if (kind == StatAggGroup::kSummed) {
		g.sum += toks[0].val;
		bool seen = false;
		for (const auto& r : g.reps) seen = seen || r.first == line;
		if (!seen) g.reps.push_back({ line, toks[0].val });
	}
}

void BuildStatAggDisplay(std::vector<StatAggGroup>& groups,
                         const std::function<std::string(const std::string&)>* zhLookup)
{
	for (StatAggGroup& g : groups) {
		g.zhFallback = false;
		if (g.kind != StatAggGroup::kSummed) {
			const std::string& raw = g.key.substr(2);
			g.dispEn = raw;
			g.dispZh = zhLookup ? (*zhLookup)(raw) : raw; // StatLine falls back to en itself
		} else {
			// en: template placeholder -> formatted sum
			const std::string tmpl = g.key.substr(2);
			size_t hash = tmpl.find('#');
			g.dispEn = tmpl.substr(0, hash) + FmtStatNum(g.sum, g.plusSign) + tmpl.substr(hash + 1);

			// zh: value-matched backfill; a rep whose translation is missing or
			// whose numbers were reordered by the translator just gets skipped
			g.dispZh.clear();
			if (zhLookup) {
				for (const auto& [en, repVal] : g.reps) {
					std::string zh = (*zhLookup)(en);
					if (zh == en) continue; // untranslated
					for (const StatNumTok& t : FindStatNumbers(zh)) {
						if (std::fabs(t.val - repVal) < 1e-6) {
							g.dispZh = zh.substr(0, t.pos) + FmtStatNum(g.sum, t.plus) +
							           zh.substr(t.pos + t.len);
							break;
						}
					}
					if (!g.dispZh.empty()) break;
				}
			}
			if (g.dispZh.empty()) {
				g.dispZh = g.dispEn;
				g.zhFallback = true;
			}
		}
		g.searchKey = ToLowerAscii(g.key.substr(2) + "\n" + g.dispEn + "\n" + g.dispZh);
	}
}

// ---- selftest -----------------------------------------------------------------

int RunAtlasAggSelfTest(const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}

	int failures = 0;
	std::string report;
	auto check = [&](bool ok, const char* what, const std::string& detail = "") {
		std::string line = std::string(ok ? "PASS  " : "FAIL  ") + what +
		                   (detail.empty() ? "" : "  -> " + detail) + "\n";
		report += line;
		printf("%s", line.c_str());
		if (!ok) failures++;
	};

	auto agg = [](const std::vector<std::string>& lines,
	              const std::function<std::string(const std::string&)>* zh) {
		std::vector<StatAggGroup> groups;
		std::unordered_map<std::string, size_t> pos;
		for (const std::string& l : lines) AccumulateStatLine(l, groups, pos);
		BuildStatAggDisplay(groups, zh);
		return groups;
	};

	// 1: same-template summation collapses to one row
	{
		auto g = agg({ "2% increased X", "2% increased X", "3% increased X" }, nullptr);
		check(g.size() == 1 && g[0].kind == StatAggGroup::kSummed &&
		      g[0].count == 3 && g[0].dispEn == "7% increased X",
		      "same-template sum", g.empty() ? "no groups" : g[0].dispEn);
	}
	// 2: explicit plus sign survives
	{
		auto g = agg({ "+5 to maximum Y", "+6 to maximum Y" }, nullptr);
		check(g.size() == 1 && g[0].dispEn == "+11 to maximum Y",
		      "plus sign preserved", g.empty() ? "no groups" : g[0].dispEn);
	}
	// 3: boolean lines dedupe to a single row without xN
	{
		auto g = agg({ "Your Maps cannot contain Abysses", "Your Maps cannot contain Abysses" }, nullptr);
		check(g.size() == 1 && g[0].kind == StatAggGroup::kBoolean &&
		      g[0].dispEn == "Your Maps cannot contain Abysses",
		      "boolean dedupe");
	}
	// 4: multi-number lines keep the raw text and count
	{
		auto g = agg({ "10% chance to gain 2 additional Z", "10% chance to gain 2 additional Z" }, nullptr);
		check(g.size() == 1 && g[0].kind == StatAggGroup::kMulti && g[0].count == 2 &&
		      g[0].dispEn == "10% chance to gain 2 additional Z",
		      "multi-number stays raw");
	}
	// 5: fractional sums stay clean
	{
		auto g = agg({ "0.25% chance to W", "0.25% chance to W", "0.25% chance to W" }, nullptr);
		check(g.size() == 1 && g[0].dispEn == "0.75% chance to W",
		      "fractional sum", g.empty() ? "no groups" : g[0].dispEn);
	}
	// 6: zh backfill replaces the value-matched number
	{
		std::function<std::string(const std::string&)> zh = [](const std::string& en) -> std::string {
			if (en == "Ore Deposits contain 10% increased Ore") return u8"礦床蘊含的礦石增加 10%";
			if (en == "Ore Deposits contain 20% increased Ore") return u8"礦床蘊含的礦石增加 20%";
			return en;
		};
		auto g = agg({ "Ore Deposits contain 10% increased Ore", "Ore Deposits contain 20% increased Ore" }, &zh);
		check(g.size() == 1 && !g[0].zhFallback && g[0].dispZh == u8"礦床蘊含的礦石增加 30%",
		      "zh value-matched backfill", g.empty() ? "no groups" : g[0].dispZh);
	}
	// 7: an extra explicit "1" in the zh line is not mistaken for the value
	{
		std::function<std::string(const std::string&)> zh = [](const std::string& en) -> std::string {
			if (en == "Maps have 5% chance to contain a Beast") return u8"地圖有 5% 機率含有 1 個野獸";
			return en;
		};
		auto g = agg({ "Maps have 5% chance to contain a Beast",
		               "Maps have 5% chance to contain a Beast" }, &zh);
		check(g.size() == 1 && g[0].dispZh == u8"地圖有 10% 機率含有 1 個野獸",
		      "zh skips unrelated numbers", g.empty() ? "no groups" : g[0].dispZh);
	}
	// 8: no matching value anywhere -> English fallback
	{
		std::function<std::string(const std::string&)> zh = [](const std::string& en) -> std::string {
			(void)en;
			return u8"翻譯數字對不上 99%";
		};
		auto g = agg({ "Maps have 5% chance to contain V" }, &zh);
		check(g.size() == 1 && g[0].zhFallback && g[0].dispZh == g[0].dispEn,
		      "zh fallback to English");
	}

	std::string tail = failures == 0 ? "\nALL PASS\n"
	                                 : "\nFAILURES: " + std::to_string(failures) + "\n";
	report += tail;
	printf("%s", tail.c_str());

	HANDLE h = CreateFileW((exeDir + L"atlas_agg_selftest.txt").c_str(), GENERIC_WRITE, 0,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &written, nullptr);
		CloseHandle(h);
	}
	return failures == 0 ? 0 : 1;
}
