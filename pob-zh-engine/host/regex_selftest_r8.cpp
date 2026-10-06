// --regex-selftest, R8 part: share codes and templates (regex_share), plus the
// step-40 rarity | corruption row's wiring / bookmark / share round trips
// (exile-appraiser regex/test/rarity.test.ts @ d5ccb47). Ports
// exile-appraiser regex/test/share.test.ts and the v1 -> v2 share-code cases of
// sections.test.ts, adds what only exists here (miniz gzip with our own header /
// trailer: CRC, ISIZE, truncation, flipped bytes, a zip bomb, oversized input),
// and replays regex_r8_golden.inc, which tools/regex_port/gen-golden-r8.mts made
// by running share.ts / embed.ts / combine.ts over our Data files:
//   * codes exile-appraiser produced decode here to the same JSON, and resolve /
//     combine on our data to the same picks, values, misses and query strings;
//   * codes from there carrying ITS item-mod page (trade stat ids) come back with
//     every such key counted as missed, per page;
//   * v1 legacy codes, normalizeShareState cases (state, warnings, errors) and
//     damaged codes (accepted / refused) all match.
// The other direction (codes made HERE decoded by exile-appraiser) needs Node:
// with POBTOOLS_REGEX_SHARE_DUMP=<file> set, this writes every code it made to
// that file and tools/regex_port/verify-a-codes.mts decodes them with share.ts.
#include "regex_algo_pages.h"
#include "regex_data.h"
#include "regex_embed.h"
#include "regex_itemmods.h"
#include "regex_share.h"
#include "regex_state.h"
#include "regex_state_json.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <json.hpp>

namespace {

#include "regex_r8_golden.inc"

using namespace RegexAlgo;
using json = nlohmann::json;
using ojson = nlohmann::ordered_json;
using RegexFrag::AlgoValue;
namespace IM = RegexItemMods;
namespace S = RegexShare;

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_line)(const std::string&) = nullptr;

void check(bool ok, const std::string& what) { g_check(ok, what); }
void line(const std::string& s) { g_line(s); }
std::string Num(long long n) { return std::to_string(n); }

struct Rng {
	uint32_t s;
	explicit Rng(uint32_t seed) : s(seed) {}
	uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
	int below(int n) { return n <= 0 ? 0 : (int)(next() % (uint32_t)n); }
};

// One game's catalogue as the panel builds it: corpus pages in data order, the
// algorithmic pages, the item-mod values page (loaded).
struct Game {
	std::string id;
	std::vector<AlgoPage> algo;
	std::vector<PageRef> pages;
	bool itemOk = false;
	const PageRef* Find(const std::string& pid) const
	{
		for (const PageRef& p : pages)
			if (p.Id() == pid) return &p;
		return nullptr;
	}
};

// Codes made here, for verify-a-codes.mts.
struct Dumped {
	std::string label, json, code;
};
std::vector<Dumped> g_dump;

void Dump(const std::string& label, const S::State& s)
{
	g_dump.push_back({label, S::ToJson(s), S::Encode(s)});
}

RegexGen::Mode ModeOf(const std::string& m)
{
	return m == "all" ? RegexGen::Mode::All : m == "none" ? RegexGen::Mode::None : RegexGen::Mode::Any;
}

json ValuesJson(const RegexValueList& m)
{
	json o = json::object();
	for (const auto& kv : m) o[kv.first] = json::parse(RegexStateJson::ValueTo(kv.second).dump());
	return o;
}

// The resolve result as the TS's JSON (object key order not compared).
json ResolvedJson(const S::Resolved& r)
{
	json o;
	o["picks"] = json::object();
	for (const auto& kv : r.picks) o["picks"][kv.first] = kv.second;
	o["values"] = json::object();
	for (const auto& kv : r.values) o["values"][kv.first] = ValuesJson(kv.second);
	o["missed"] = r.missed;
	o["unknownPages"] = r.unknownPages;
	return o;
}

// embed.ts combineSels(pages, picks, resolvedValues(values)) + combine().
std::string Query(const Game& g, const S::State& s, const S::Resolved& r, RegexFrag::Lang lang, UnionCorpusCache& cache)
{
	const S::ValueLists store = S::ResolvedValues(r.values);
	std::deque<ValueMap> keep;
	std::vector<CombineSel> sels;
	for (const PageRef& p : CombineOrder(g.pages)) {
		auto it = r.picks.find(p.Id());
		if (it == r.picks.end() || it->second.empty()) continue;
		CombineSel sel;
		sel.page = p;
		sel.picks = it->second;
		if (p.algo) {
			keep.emplace_back();
			for (const auto& kv : store)
				if (kv.first == NumericKeyOf(p.Id()))
					for (const auto& e : kv.second) keep.back()[e.first] = e.second;
			sel.values = &keep.back();
		}
		sels.push_back(std::move(sel));
	}
	return Combine(lang, ModeOf(s.mode), sels, s.custom, s.excludes, &cache).query;
}

std::vector<std::string> StrVec(const json& j)
{
	std::vector<std::string> out;
	for (const json& x : j) out.push_back(x.get<std::string>());
	return out;
}

std::string Clip(const std::string& s, size_t n = 90)
{
	return s.size() > n ? s.substr(0, n) + "..." : s;
}

// ---- base64url / gzip -------------------------------------------------------------

void CodecTests()
{
	line("  base64url / gzip");
	// share.test.ts base64url: arbitrary bytes 0-300 long
	bool all = true, charset = true;
	for (int n = 0; n <= 300; n++) {
		std::string b;
		for (int i = 0; i < n; i++) b += (char)((i * 37 + n) & 255);
		const std::string s = S::Base64Url(b);
		for (char c : s) charset = charset && (isalnum((unsigned char)c) || c == '-' || c == '_');
		std::string back;
		all = all && S::FromBase64Url(s, back, nullptr) && back == b;
	}
	check(all && charset, u8"base64url：0–300 長度任意位元組往返相同、只用 A–Z a–z 0–9 - _（share.test.ts）");
	std::string out, err;
	check(S::FromBase64Url(" SGVs\nbG8+/w== ", out, nullptr) && out == std::string("Hello>\xff", 7) &&
	          S::FromBase64Url(u8"SGVs　bG8", out, nullptr) && out == "Hello",
	      u8"解碼容忍空白（含全形）、= 填充、+ / 寫法（fromBase64url）");
	{
		std::string e1, e2;
		const bool r1 = S::FromBase64Url("SGV*", out, &e1), r2 = S::FromBase64Url(u8"SG碼", out, &e2);
		check(!r1 && e1 == u8"分享碼含有不合法字元「*」" && !r2 && e2 == u8"分享碼含有不合法字元「碼」",
		      u8"不合法字元回報該字元：" + e1 + " / " + e2);
	}

	// gzip round trips: empty, tiny, text, incompressible, large
	Rng rng(8);
	std::vector<std::string> inputs = {"", "x", "{\"v\":2}", std::string(100000, 'a')};
	std::string noise;
	for (int i = 0; i < 70000; i++) noise += (char)rng.below(256);
	inputs.push_back(noise);
	std::string text;
	for (int i = 0; i < 4000; i++) text += u8"玩家最大抗性 -" + Num(i % 13) + "% ";
	inputs.push_back(text);
	bool rt = true, hdr = true;
	for (const std::string& in : inputs) {
		const std::string gz = S::Gzip(in);
		hdr = hdr && gz.size() >= 18 && gz.compare(0, 10, std::string("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x0a", 10)) == 0;
		std::string back;
		rt = rt && S::Gunzip(gz, back, 1u << 30, &err) && back == in;
	}
	check(rt && hdr, u8"gzip 往返（空、1 位元組、文字、70 KB 雜訊、100 KB 重複、中文）；標頭與 Node 相同（1f8b0800 00000000 000a）");
	// Node's own bytes for {"v":2} (CompressionStream on Windows, recorded 2026-10-06)
	const char node[] = "\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\x0a\xab\x56\x2a\x53\xb2\x32\xaa\x05\x00\xfe\xb4\xf6\xaf\x07\x00\x00\x00";
	std::string back;
	check(S::Gunzip(std::string(node, sizeof node - 1), back, 1024, &err) && back == "{\"v\":2}",
	      u8"解得開 Node zlib 產的 gzip；miniz 產的位元組 " +
	          std::string(S::Gzip("{\"v\":2}") == std::string(node, sizeof node - 1) ? u8"恰好相同" : u8"不同（deflate 實作不同，解開相同）"));

	// damaged gzip
	const std::string gz = S::Gzip(text);
	int refused = 0, cut = 0;
	for (size_t n = 0; n < gz.size(); n += (n < 64 ? 1 : 97)) {
		cut++;
		refused += S::Gunzip(gz.substr(0, n), back, 1u << 30, &err) ? 0 : 1;
	}
	check(refused == cut, u8"截斷的 gzip（" + Num(cut) + u8" 個長度）全部拒絕");
	int flipOk = 0, flipBadOk = 0, flips = 0;
	for (size_t i = 0; i < gz.size(); i += 1 + i / 50) {
		std::string m = gz;
		m[i] ^= (char)(1 << (i % 8));
		flips++;
		if (S::Gunzip(m, back, 1u << 30, &err)) {
			flipOk++;
			if (!(i >= 4 && i <= 9) || back != text) flipBadOk++;
		}
	}
	check(flipBadOk == 0, u8"翻轉位元組 " + Num(flips) + u8" 處：只有 mtime / xfl / os（不影響內容）的 " + Num(flipOk) +
	                          u8" 處照常解開，其餘全部拒絕（CRC / 長度 / deflate 檢查）");
	{
		std::string tail = gz + "x";
		std::string two = gz + gz;
		check(!S::Gunzip(tail, back, 1u << 30, &err) && !S::Gunzip(two, back, 1u << 30, &err),
		      u8"結尾多餘位元組、兩個 gzip 成員：拒絕（同 Node DecompressionStream）");
	}
	// zip bomb: 64 MB of zeros, ~64 KB compressed
	{
		const std::string zeros(64u << 20, '\0');
		const std::string bomb = S::Gzip(zeros);
		const DWORD t0 = GetTickCount();
		const bool ok = S::Gunzip(bomb, back, S::kMaxJsonBytes, &err);
		check(!ok && back.empty() && err.find(u8"上限") != std::string::npos,
		      u8"zip bomb（64 MB 的 0 壓成 " + Num((long long)bomb.size()) + u8" 位元組）：解到 " +
		          Num((long long)(S::kMaxJsonBytes >> 20)) + u8" MB 上限就停（" + err + u8"，" + Num((long long)(GetTickCount() - t0)) + " ms）");
		S::Normalized n;
		bool r = S::Decode(S::Base64Url(bomb), n, &err);
		check(!r && err.find(u8"上限") != std::string::npos, u8"包成分享碼也一樣被擋：" + err);
		r = S::Decode(std::string(S::kMaxCodeChars + 1, 'A'), n, &err);
		check(!r && err.find(u8"太長") != std::string::npos,
		      u8"超長輸入（" + Num((long long)S::kMaxCodeChars + 1) + u8" 字元）直接拒絕：" + err);
	}
}

// ---- share.test.ts ------------------------------------------------------------------

S::State Sample()
{
	S::State s;
	s.game = "poe1";
	s.mode = "none";
	s.pages = {{"map_mods", {"#% more Monster Life", "Monsters Blind on Hit"}}};
	s.sections = {{"map_mods", {"tier", "quantity"}}};
	AlgoValue t, q;
	t.min = 16;
	q.min = 80;
	q.max = 150;
	s.numeric = {{"map_mods", {{"tier", t}, {"quantity", q}}}};
	s.custom = {u8"自訂 一", "Custom \"two\""};
	s.excludes = {u8"反射", u8"無法回復"};
	return s;
}

void ShareTestPort(const Game& poe1)
{
	line("  share.test.ts");
	const S::State sample = Sample();
	const std::string code = S::Encode(sample);
	S::Normalized d;
	std::string err;
	check(code.compare(0, 4, "H4sI") == 0 && code.find_first_not_of(
	          "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") == std::string::npos,
	      u8"分享碼形如 H4sI[A-Za-z0-9_-]+（" + Num((long long)code.size()) + u8" 字元）");
	check(S::Decode(code, d, &err) && d.warnings.empty() && S::ToJson(d.state) == S::ToJson(sample),
	      u8"encode → decode 往返相同");
	Dump("sample", sample);
	check(S::Decode("  " + code.substr(0, 20) + "\n" + code.substr(20) + "  ", d, &err) &&
	          S::ToJson(d.state) == S::ToJson(sample),
	      u8"前後空白、換行容忍");
	{
		std::string e1, e2;
		const bool r1 = S::Decode("not-a-code", d, &e1), r2 = S::Decode("", d, &e2);
		check(!r1 && !r2 && e2 == u8"分享碼是空的", u8"壞碼 / 空字串丟錯誤（" + e1 + u8"；" + e2 + u8"）");
	}
	{
		// unknown page + a key that is gone
		S::State s = sample;
		s.pages[0].second.push_back("no such mod");
		s.pages.push_back({"gone_page", {"x"}});
		const S::Resolved r = S::Resolve(s, poe1.pages);
		const AlgoValue* q = nullptr;
		for (const auto& kv : r.values)
			if (kv.first == "map_numeric") q = RegexValueFind(kv.second, "quantity");
		check(r.unknownPages == std::vector<std::string>{"gone_page"} && r.missed == 2 &&
		          r.picks.count("map_mods") && r.picks.at("map_mods").size() == 2 && r.picks.count("map_numeric") &&
		          r.picks.at("map_numeric").size() == 2 && q && q->min == 80.0 && q->max == 150.0,
		      u8"resolveState：勾選與數值還原到目前清單；不存在的頁與鍵計數（missed 2、unknownPages gone_page）");
	}
	{
		// tick -> code -> clear -> paste: the same query
		const PageRef* mm = poe1.Find("map_mods");
		const PageRef* mn = poe1.Find("map_numeric");
		RegexEmbed::PicksMap picks = {{"map_mods", {3, 17, 40}}, {"map_numeric", {0, 1}}};
		AlgoValue t, q;
		t.min = 16;
		q.min = 80;
		RegexEmbed::ValuesMap values = {{"map_mods", {{"tier", t}, {"quantity", q}}}};
		UnionCorpusCache cache;
		auto run = [&](const RegexEmbed::PicksMap& pk, const ValueMap& vm, const std::string& mode, RegexFrag::Lang lang) {
			std::vector<CombineSel> sels;
			CombineSel a;
			a.page = *mm;
			a.picks = pk.at("map_mods");
			sels.push_back(a);
			CombineSel b;
			b.page = *mn;
			b.picks = pk.at("map_numeric");
			b.values = &vm;
			sels.push_back(b);
			return Combine(lang, ModeOf(mode), sels, {}, {u8"反射"}, &cache).query;
		};
		const std::string before = run(picks, values["map_mods"], "any", RegexFrag::Lang::Zh);
		const S::State st = S::StateOf("poe1", poe1.pages, picks, values, "any", {}, {u8"反射"});
		S::Normalized dd;
		const bool ok = S::Decode(S::Encode(st), dd, &err);
		const S::Resolved r = ok ? S::Resolve(dd.state, poe1.pages) : S::Resolved{};
		RegexEmbed::PicksMap after;
		ValueMap vm;
		for (const auto& kv : r.picks) after[kv.first] = kv.second;
		for (const auto& kv : S::ResolvedValues(r.values))
			if (kv.first == "map_mods")
				for (const auto& e : kv.second) vm[e.first] = e.second;
		const std::string afterQ = ok && after.count("map_mods") && after.count("map_numeric")
			? run(after, vm, dd.state.mode, RegexFrag::Lang::Zh) : std::string();
		check(ok && !before.empty() && afterQ == before && after["map_numeric"] == std::vector<int>{0, 1},
		      u8"勾選 → 分享碼 → 清空 → 貼上：同一串 query（" + before + u8"）");
		Dump("tick-clear-paste", st);
	}
	{
		// normalizeShareState: version / game / v1 / bad fields
		std::string sj = S::ToJson(sample);
		auto with = [&](const std::string& field, const std::string& val) {
			ojson j = ojson::parse(sj);
			j[field] = ojson::parse(val);
			return j.dump();
		};
		S::Normalized n;
		bool r = S::NormalizeJson(with("v", "3"), true, n, &err);
		check(!r && err.find(u8"版本") != std::string::npos, u8"版本 3 → 錯誤：" + err);
		check(S::NormalizeJson(with("v", "1"), true, n, &err), u8"版本 1 照讀");
		r = S::NormalizeJson(with("game", "\"poe3\""), true, n, &err);
		check(!r && err.find(u8"遊戲") != std::string::npos, u8"遊戲不明 → 錯誤：" + err);
		{
			// choice is cut to 16 UTF-16 units; a cut inside a surrogate pair drops the whole
			// code point here (JS would keep a lone surrogate) -- the one known difference.
			const std::string in = u8"一二三四五六七八九十一二三四五😀";
			S::Normalized c;
			const bool okc = S::NormalizeJson("{\"v\":2,\"game\":\"poe1\",\"numeric\":{\"p\":{\"a\":{\"choice\":\"" + in + "\"}}}}", true, c, &err);
			const bool cut = okc && !c.state.numeric.empty() && !c.state.numeric[0].second.empty() &&
			                 c.state.numeric[0].second[0].second.choice == u8"一二三四五六七八九十一二三四五";
			check(cut, u8"choice 截在 16 個 UTF-16 單位、切到 emoji 中間時整個字丟掉（TS 會留半個代理對；已知差異，只在這種情況）");
		}
		ojson j = ojson::parse(sj);
		j["extra"] = 1;
		j["custom"] = "oops";
		j["numeric"] = ojson::parse("{\"map_numeric\":{\"tier\":5}}");
		const bool ok = S::NormalizeJson(j.dump(), true, n, &err);
		auto has = [&](const char* w) {
			for (const std::string& x : n.warnings)
				if (x.find(w) != std::string::npos) return true;
			return false;
		};
		check(ok && has("extra") && has("custom") && has("numeric.map_numeric.tier") && n.state.custom.empty(),
		      u8"未知欄位忽略並回報；型別不符的欄位丟掉並回報（" + Num((long long)n.warnings.size()) + u8" 則）");
	}
}

// ---- templates ------------------------------------------------------------------------

void TemplateTests(const std::wstring& exeDir, std::map<std::string, Game>& G)
{
	line("  templates (Data\\regex_templates.json)");
	std::vector<S::Template> ts;
	std::vector<std::string> errors;
	std::string err;
	const bool ok = S::LoadTemplates(exeDir, ts, errors, &err);
	int p1 = 0, p2 = 0;
	bool names = true;
	for (const S::Template& t : ts) {
		(t.game == "poe1" ? p1 : p2)++;
		names = names && !t.nameZh.empty() && !t.nameEn.empty() && !t.descZh.empty() && !t.descEn.empty();
	}
	check(ok && errors.empty() && ts.size() == 7 && p1 == 5 && p2 == 2 && names,
	      u8"7 個範本（PoE1 5、PoE2 2）、全部可解析、雙語名稱與說明齊全" + (ok ? std::string() : "  " + err));
	UnionCorpusCache cache;
	for (const S::Template& t : ts) {
		const Game& g = G[t.game];
		const S::Resolved r = S::Resolve(t.state, g.pages);
		std::string q[2];
		bool fine = r.missed == 0 && r.unknownPages.empty();
		for (int li = 0; li < 2; li++) {
			const RegexFrag::Lang lang = li ? RegexFrag::Lang::En : RegexFrag::Lang::Zh;
			const S::ValueLists store = S::ResolvedValues(r.values);
			std::deque<ValueMap> keep;
			std::vector<CombineSel> sels;
			for (const PageRef& p : CombineOrder(g.pages)) {
				auto it = r.picks.find(p.Id());
				if (it == r.picks.end() || it->second.empty()) continue;
				CombineSel sel;
				sel.page = p;
				sel.picks = it->second;
				if (p.algo) {
					keep.emplace_back();
					for (const auto& kv : store)
						if (kv.first == NumericKeyOf(p.Id()))
							for (const auto& e : kv.second) keep.back()[e.first] = e.second;
					sel.values = &keep.back();
				}
				sels.push_back(sel);
			}
			const CombineResult c = Combine(lang, ModeOf(t.state.mode), sels, t.state.custom, t.state.excludes, &cache);
			q[li] = c.query;
			fine = fine && !c.query.empty() && c.conflicts.empty() && c.length <= c.limit;
		}
		check(fine, t.id + u8"：每個鍵都還原得到（找不到 " + Num(r.missed) + u8"），產生的字串無衝突、不超長：「" + q[0] + u8"」/「" + q[1] + u8"」");
	}
	for (const S::Template& t : ts)
		if (t.id == "poe1_t17_danger") {
			const S::Resolved r = S::Resolve(t.state, G["poe1"].pages);
			check(t.state.mode == "none" && r.picks.count("map_mods") && r.picks.at("map_mods").size() == 8,
			      u8"T17 危險詞綴範本 = 8 個勾選、None 模式");
		}
	{
		std::vector<S::Template> rb;
		std::vector<std::string> re;
		S::ParseTemplates("{\"templates\":[{\"id\":\"ok\",\"game\":\"poe1\",\"mode\":\"any\",\"pages\":{}},{\"id\":\"bad\",\"game\":\"x\"},5]}",
		                  rb, re, &err);
		check(rb.size() == 1 && rb[0].id == "ok" && re.size() == 2, u8"壞掉的範本只略過那一筆（" + (re.empty() ? std::string() : re[0]) + u8"）");
		check(!S::ParseTemplates("{\"x\":1}", rb, re, &err) && !S::ParseTemplates("nope", rb, re, &err),
		      u8"整份檔不對（缺 templates / 不是 JSON）→ 失敗");
	}
	{
		std::string body;
		HANDLE h = CreateFileW((exeDir + L"Data\\regex_templates.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			LARGE_INTEGER sz{};
			GetFileSizeEx(h, &sz);
			body.resize((size_t)sz.QuadPart);
			DWORD rd = 0;
			if (!body.empty()) ReadFile(h, &body[0], (DWORD)body.size(), &rd, nullptr);
			CloseHandle(h);
		}
		check(!body.empty() && body.find("map_numeric") == std::string::npos && body.find("waystone_numeric") == std::string::npos &&
		          body.find("item_mod_values") == std::string::npos,
		      u8"出貨的範本沒有引用舊頁 id（map_numeric / waystone_numeric）也沒有引用物品詞綴頁");
	}
}

// ---- round trips made here (all pages, item-mods included) -------------------------------

void RoundTrips(std::map<std::string, Game>& G)
{
	line("  A -> A round trips");
	Rng rng(808);
	static const char* const kCustom[] = {u8"6 連結", "\"q\"", "a.b (c)", "!neg", "tab\there", "ctl\x01x", u8" sep", u8"😀"};
	static const char* const kExcl[] = {u8"反射", "reflect", u8"時空鎖鏈"};
	int n = 0, ok = 0, query = 0, withItem = 0;
	UnionCorpusCache cache;
	for (auto& kv : G) {
		Game& g = kv.second;
		std::vector<PageRef> listed = ListedPages(g.pages);
		for (int k = 0; k < 12; k++) {
			RegexEmbed::PicksMap picks;
			RegexEmbed::ValuesMap values;
			const int pagesN = 1 + rng.below(4);
			for (int t = 0; t < pagesN; t++) {
				// every few rounds the item-mod page for sure (when loaded)
				const PageRef* p = &listed[rng.below((int)listed.size())];
				if (k % 3 == 0 && g.itemOk)
					for (const PageRef& x : listed)
						if (IM::IsPageId(x.Id())) p = &x;
				if (p->Size() == 0) continue;
				std::set<int> set;
				const int cnt = 1 + rng.below((int)std::min<size_t>(8, p->Size()));
				while ((int)set.size() < cnt) set.insert(rng.below((int)p->Size()));
				picks[p->Id()] = std::vector<int>(set.begin(), set.end());
				auto rv = [&]() {
					AlgoValue v;
					switch (rng.below(4)) {
					case 0: v.min = rng.below(200); break;
					case 1: v.max = rng.below(200); break;
					case 2: v.min = rng.below(50); v.max = 50 + rng.below(50); break;
					default: v.choice = "6"; v.hasChoice = true; break;
					}
					return v;
				};
				if (p->algo)
					for (int i : picks[p->Id()])
						if (rng.below(2)) values[NumericKeyOf(p->Id())][p->algo->entries[i].def.id] = rv();
				if (const AlgoPage* sec = SectionPageOf(g.pages, p->Id(), g.id)) {
					std::set<int> ss;
					const int sc = 1 + rng.below((int)std::min<size_t>(4, sec->entries.size()));
					while ((int)ss.size() < sc) ss.insert(rng.below((int)sec->entries.size()));
					picks[sec->id] = std::vector<int>(ss.begin(), ss.end());
					for (int i : picks[sec->id])
						if (rng.below(2)) values[p->Id()][sec->entries[i].def.id] = rv();
				}
			}
			std::vector<std::string> custom, excl;
			for (const char* c : kCustom)
				if (rng.below(4) == 0) custom.push_back(c);
			for (const char* c : kExcl)
				if (rng.below(3) == 0) excl.push_back(c);
			static const char* const kModes[] = {"any", "all", "none"};
			const S::State st = S::StateOf(g.id, g.pages, picks, values, kModes[rng.below(3)], custom, excl);
			bool item = false;
			for (const auto& pk : st.pages) item = item || IM::IsPageId(pk.first);
			withItem += item ? 1 : 0;
			n++;
			S::Normalized d;
			std::string err;
			if (!S::Decode(S::Encode(st), d, &err) || !d.warnings.empty() || S::ToJson(d.state) != S::ToJson(st)) {
				check(false, "round trip " + g.id + " " + Num(k) + ": " + err);
				continue;
			}
			// Resolve gives back exactly the ticks and the (ticked) values
			const S::Resolved r = S::Resolve(d.state, g.pages);
			bool same = r.missed == 0 && r.unknownPages.empty();
			for (const auto& pk : picks) same = same && r.picks.count(pk.first) && r.picks.at(pk.first) == pk.second;
			for (const auto& rp : r.picks) same = same && picks.count(rp.first);
			if (!same) {
				check(false, "round trip " + g.id + " " + Num(k) + ": picks differ");
				continue;
			}
			ok++;
			// and the merged string is the same before and after (share.test.ts, every page)
			RegexEmbed::PicksMap bp = picks;
			S::Resolved fake;
			for (const auto& pk : bp) fake.picks[pk.first] = pk.second;
			for (const auto& vv : st.numeric) {
				// st.numeric is keyed by store key; Resolve's values by internal page id
				const AlgoPage* sec = SectionPageOf(g.pages, vv.first, g.id);
				fake.values.push_back({sec ? sec->id : vv.first, vv.second});
			}
			const std::string q0 = Query(g, st, fake, RegexFrag::Lang::Zh, cache);
			const std::string q1 = Query(g, d.state, r, RegexFrag::Lang::Zh, cache);
			query += (q0 == q1) ? 1 : 0;
			Dump("A " + g.id + " " + Num(k) + (item ? " +item-mods" : ""), st);
		}
	}
	check(ok == n && n > 0, u8"本工具產的分享碼往返 " + Num(ok) + " / " + Num(n) + u8" 組（含數值區、商店頁、物品詞綴頁 " +
	                            Num(withItem) + u8" 組、自訂文字含控制字元 / U+2028 / emoji、排除詞、三種模式）：JSON 逐字相同、勾選全部還原");
	check(query == n, u8"往返前後合併字串相同 " + Num(query) + " / " + Num(n));
}

// ---- golden ----------------------------------------------------------------------------

void GoldenR8(std::map<std::string, Game>& G)
{
	line("  golden (exile-appraiser share.ts over our Data)");
	int counts[6] = {0, 0, 0, 0, 0, 0}, fails[6] = {0, 0, 0, 0, 0, 0};
	int byteSame = 0, codes = 0, itemMissOk = 0, itemCodes = 0;
	std::vector<std::string> failMsgs;
	UnionCorpusCache cache;
	auto fail = [&](int k, const std::string& m) {
		fails[k]++;
		if (failMsgs.size() < 12) failMsgs.push_back(m);
	};
	for (const char* rec : kRegexR8Golden) {
		const json r = json::parse(rec);
		const std::string kind = r[0].get<std::string>();
		const std::string label = r[1].get<std::string>();
		if (kind == "code" || kind == "v1") {
			const int k = kind == "code" ? 0 : 1;
			counts[k]++;
			const std::string game = r[2].get<std::string>();
			const std::string code = kind == "code" ? r[4].get<std::string>() : r[3].get<std::string>();
			const std::string wantJson = kind == "code" ? r[3].get<std::string>() : r[4].get<std::string>();
			const json& wantRes = kind == "code" ? r[5] : r[6];
			const json& wantQ = kind == "code" ? r[6] : r[7];
			S::Normalized d;
			std::string err;
			if (!S::Decode(code, d, &err)) {
				fail(k, label + ": decode " + err);
				continue;
			}
			if (S::ToJson(d.state) != wantJson) {
				fail(k, label + ": json " + Clip(S::ToJson(d.state)) + " != " + Clip(wantJson));
				continue;
			}
			if (kind == "v1" && json(d.warnings) != r[5]) {
				fail(k, label + ": warnings");
				continue;
			}
			Game& g = G[game];
			const S::Resolved res = S::Resolve(d.state, g.pages);
			if (ResolvedJson(res) != wantRes) {
				fail(k, label + ": resolve " + Clip(ResolvedJson(res).dump(), 200) + " != " + Clip(wantRes.dump(), 200));
				continue;
			}
			const std::string qz = Query(g, d.state, res, RegexFrag::Lang::Zh, cache);
			const std::string qe = Query(g, d.state, res, RegexFrag::Lang::En, cache);
			if (qz != wantQ["zh"].get<std::string>() || qe != wantQ["en"].get<std::string>()) {
				fail(k, label + ": query " + qz + " != " + wantQ["zh"].get<std::string>());
				continue;
			}
			if (kind == "code") {
				codes++;
				byteSame += S::Encode(d.state) == code ? 1 : 0;
				// exile-appraiser's item-mod page: every key it carries is reported as missed on that page
				if (label.find("b-item-mods") != std::string::npos) {
					itemCodes++;
					int keys = 0;
					for (const auto& kv : d.state.pages)
						if (IM::IsPageId(kv.first)) keys += (int)kv.second.size();
					int reported = 0;
					for (const auto& kv : res.missedByPage)
						if (IM::IsPageId(kv.first)) reported = kv.second;
					if (keys > 0 && reported == keys && res.missed == keys) itemMissOk++;
				}
			}
			Dump("re-encoded " + label, d.state);
		} else if (kind == "norm") {
			counts[2]++;
			S::Normalized n;
			std::string err;
			const bool ok = S::NormalizeJson(r[2].get<std::string>(), r[3].get<bool>(), n, &err);
			if (ok != r[4].get<bool>()) fail(2, label + ": ok " + (ok ? "true" : "false") + " " + err);
			else if (!ok && err != r[5].get<std::string>()) fail(2, label + ": error " + err + " != " + r[5].get<std::string>());
			else if (ok && S::ToJson(n.state) != r[6].get<std::string>()) fail(2, label + ": state " + Clip(S::ToJson(n.state)) + " != " + Clip(r[6].get<std::string>()));
			else if (ok && json(n.warnings) != r[7]) fail(2, label + ": warnings " + json(n.warnings).dump() + " != " + r[7].dump());
		} else if (kind == "bad") {
			counts[3]++;
			S::Normalized n;
			std::string err;
			const bool ok = S::Decode(r[2].get<std::string>(), n, &err);
			if (ok != r[3].get<bool>()) fail(3, label + ": ok " + (ok ? "true" : "false") + " " + err);
			else if (ok && (S::ToJson(n.state) != r[4].get<std::string>() || json(n.warnings) != r[5])) fail(3, label + ": state");
		} else if (kind == "tpl") {
			counts[4]++;
			std::string err;
			// the record's own state through ParseTemplates' path (normalize, no version)
			S::Normalized n;
			const bool ok = S::NormalizeJson(r[7].get<std::string>(), false, n, &err);
			const Game& g = G[r[2].get<std::string>()];
			const S::Resolved res = ok ? S::Resolve(n.state, g.pages) : S::Resolved{};
			if (!ok || ResolvedJson(res) != r[8]) fail(4, label + ": resolve");
			else {
				const std::string qz = Query(g, n.state, res, RegexFrag::Lang::Zh, cache);
				const std::string qe = Query(g, n.state, res, RegexFrag::Lang::En, cache);
				if (qz != r[9]["zh"]["query"].get<std::string>() || qe != r[9]["en"]["query"].get<std::string>())
					fail(4, label + ": query " + qz);
			}
		} else if (kind == "tplparse") {
			counts[5]++;
			std::vector<S::Template> ts;
			std::vector<std::string> errs;
			std::string err;
			S::ParseTemplates(r[1].get<std::string>(), ts, errs, &err);
			json got = json::array();
			for (const S::Template& t : ts) got.push_back({t.id, t.game, t.nameZh, t.nameEn, t.descZh, t.descEn, S::ToJson(t.state)});
			if (got != r[2]) fail(5, label + ": templates " + got.dump());
			else if (json(errs) != r[3]) fail(5, label + ": errors " + json(errs).dump() + " != " + r[3].dump());
		}
	}
	static const char* const kNames[6] = {u8"exile-appraiser 產的分享碼", u8"v1 舊碼", "normalizeShareState", u8"損壞 / 變形的碼",
	                                       u8"範本", "parseTemplates"};
	for (int k = 0; k < 6; k++)
		check(counts[k] > 0 && fails[k] == 0, std::string(kNames[k]) + u8"：" + Num(counts[k] - fails[k]) + " / " + Num(counts[k]) +
		                                          u8" 筆與 TS 相同");
	for (const std::string& m : failMsgs) line("      " + m);
	check(itemCodes > 0 && itemMissOk == itemCodes,
	      u8"exile-appraiser 物品詞綴頁的鍵（交易站 stat id）在這裡全部回報為找不到、歸在該頁：" + Num(itemMissOk) + " / " + Num(itemCodes) + u8" 組");
	line(u8"    同一狀態本工具重新編碼與 exile-appraiser 的碼逐位元組相同：" + Num(byteSame) + " / " + Num(codes) +
	     u8"（JSON 逐字相同；gzip 標頭相同；deflate 位元組由 miniz 與 zlib 各自決定，不要求相同）");
}

void TemplatesMatchGolden(const std::wstring& exeDir)
{
	std::vector<S::Template> ts;
	std::vector<std::string> errs;
	std::string err;
	S::LoadTemplates(exeDir, ts, errs, &err);
	int total = 0, same = 0;
	for (const char* rec : kRegexR8Golden) {
		const json r = json::parse(rec);
		if (r[0] != "tpl") continue;
		total++;
		for (const S::Template& t : ts)
			if (t.id == r[1].get<std::string>() && t.game == r[2].get<std::string>() && t.nameZh == r[3].get<std::string>() &&
			    t.nameEn == r[4].get<std::string>() && t.descZh == r[5].get<std::string>() && t.descEn == r[6].get<std::string>() &&
			    S::ToJson(t.state) == r[7].get<std::string>())
				same++;
	}
	check(total == 7 && same == total, u8"Data\\regex_templates.json 解析結果（名稱、說明、狀態 JSON）與 TS parseTemplates 相同：" +
	                                       Num(same) + " / " + Num(total));
}

void WriteDump()
{
	wchar_t path[MAX_PATH];
	const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_REGEX_SHARE_DUMP", path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return;
	ojson a = ojson::array();
	for (const Dumped& d : g_dump) a.push_back({{"label", d.label}, {"json", d.json}, {"code", d.code}});
	const std::string text = a.dump(-1, ' ', false, ojson::error_handler_t::replace);
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD w = 0;
	const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size();
	if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
	line(u8"    POBTOOLS_REGEX_SHARE_DUMP：寫出 " + Num((long long)g_dump.size()) + u8" 個本工具產的分享碼" + (ok ? "" : u8"（寫檔失敗）") +
	     u8"，用 tools/regex_port/verify-a-codes.mts 讓 exile-appraiser 解碼");
}

// ---- step 40: the rarity | corruption row (exile-appraiser regex/test/rarity.test.ts @ d5ccb47) ----

AlgoValue Ch(const std::vector<std::string>& rarity, Corruption c = Corruption::None)
{
	RarityChoice rc;
	rc.rarity = rarity;
	rc.corruption = c;
	AlgoValue v;
	v.choice = EncodeRarityChoice(rc);
	v.hasChoice = true;
	return v;
}

int EntryIdx(const PageRef& p, const std::string& id)
{
	if (!p.algo) return -1;
	for (int i = 0; i < (int)p.algo->entries.size(); i++)
		if (p.algo->entries[i].def.id == id) return i;
	return -1;
}

// embed.ts combineSels(pages, picks, values, only) + combine()
CombineResult CombineOnly(const Game& g, const RegexEmbed::PicksMap& picks, const RegexEmbed::ValuesMap& values,
                          const std::string& only, RegexFrag::Lang lang, RegexGen::Mode mode)
{
	std::vector<CombineSel> sels;
	for (const PageRef& p : CombineOrder(g.pages, &only)) {
		CombineSel s;
		s.page = p;
		auto it = picks.find(p.Id());
		if (it != picks.end()) s.picks = it->second;
		if (p.algo) {
			auto v = values.find(NumericKeyOf(p.Id()));
			if (v != values.end()) s.values = &v->second;
		}
		sels.push_back(s);
	}
	return Combine(lang, mode, sels);
}

void RarityTests(std::map<std::string, Game>& G)
{
	line("  step 40: rarity | corruption row (rarity.test.ts)");
	using RegexFrag::Lang;
	using RegexGen::Mode;
	// 條件區接線: hosts, default, not listed, single page = host + section
	{
		bool ok = true;
		std::string why;
		for (auto& kv : G) {
			const Game& g = kv.second;
			for (const std::string& id : ConditionSectionIds(g.id)) {
				const std::string host = SectionHostOf(id);
				const AlgoPage* sec = SectionPageOf(g.pages, host, g.id);
				if (!sec || sec->id != id || sec->entries.size() != 1 || sec->entries[0].def.id != kRarityEntryId ||
				    sec->entries[0].input.kind != InputKind::Rarity || sec->entries[0].input.def.choice != "r") {
					ok = false;
					why = g.id + " " + id;
				}
				for (const PageRef& p : ListedPages(g.pages))
					if (p.Id() == id) { ok = false; why = id + " listed"; }
				if (g.Find(host)) {
					std::vector<std::string> order;
					for (const PageRef& p : CombineOrder(g.pages, &host)) order.push_back(p.Id());
					if (order != std::vector<std::string>{host, id}) { ok = false; why = host + " order"; }
				}
			}
		}
		check(ok && ConditionSectionIds("poe1") == std::vector<std::string>{"vendor_bases_cond", "item_mod_values_cond"} &&
		      ConditionSectionIds("poe2") == std::vector<std::string>{"vendor_bases_cond", "tablet_mods_cond", "item_mod_values_poe2_cond"},
		      u8"條件區：物品基底（兩遊戲）、碑牌詞綴（PoE2）、物品詞綴數值（兩遊戲）；不在清單、單頁 = 宿主 + 條件區" + (ok ? "" : "  " + why));
	}
	// 商店頁: the old "corrupted" row id, default "|c" -> the old output verbatim
	{
		bool ok = true;
		for (const char* gid : {"poe1", "poe2"}) {
			const Game& g = G[gid];
			const std::string vid = std::string(gid) == "poe1" ? "vendor_items" : "vendor_items_poe2";
			const PageRef* v = g.Find(vid);
			const int ci = v ? EntryIdx(*v, "corrupted") : -1;
			if (ci < 0) { ok = false; continue; }
			const AlgoEntry& e = v->algo->entries[ci];
			ok = ok && e.input.kind == InputKind::Rarity && e.input.def.choice == "|c";
			const CombineResult q = CombineOnly(g, {{vid, {ci}}}, {}, vid, Lang::Zh, Mode::Any);
			ok = ok && q.query == u8"^已汙染$";
			const CombineResult q2 = CombineOnly(g, {{vid, {ci}}}, {{vid, {{"corrupted", Ch({"rare"}, Corruption::Uncorrupted)}}}}, vid,
			                                     Lang::En, Mode::Any);
			ok = ok && q2.query == u8"\"Rarity[:：] *Rare\" \"!^Corrupted$\"";
		}
		check(ok, u8"商店頁：原「已汙染」列換成條件列，id 仍是 corrupted、預設只有已汙染 → 舊勾選輸出逐字相同（^已汙染$）");
	}
	// combine: one row, two AND terms; '!' quoted; mode-independent; no conflicts
	{
		const Game& g = G["poe2"];
		const AlgoPage* sec = SectionPageOf(g.pages, "vendor_bases", "poe2");
		bool ok = sec != nullptr;
		std::string why;
		for (Mode mode : {Mode::Any, Mode::All, Mode::None}) {
			if (!sec) break;
			const CombineResult r = CombineOnly(g, {{"vendor_bases", {0}}, {sec->id, {0}}},
			                                    {{"vendor_bases", {{kRarityEntryId, Ch({"normal", "magic"}, Corruption::Uncorrupted)}}}},
			                                    "vendor_bases", Lang::Zh, mode);
			const std::string want = u8"\"稀有度[:：] *(中|魔法)\" \"!^已汙染$\"";
			bool m = r.query.find(want) != std::string::npos;
			const std::string tail = u8" \"!^已汙染$\"";
			if (mode != Mode::None)
				m = m && r.query.size() >= tail.size() && r.query.compare(r.query.size() - tail.size(), tail.size(), tail) == 0;
			const PageContribution* pc = nullptr;
			for (const PageContribution& c : r.perPage)
				if (c.id == sec->id) pc = &c;
			m = m && pc && pc->fragments == std::vector<std::string>{u8"稀有度[:：] *(中|魔法)", u8"!^已汙染$"} &&
			    pc->length == RegexGen::CharCount(want + " ") && r.conflicts.empty();
			if (!m) { ok = false; why = r.query; }
		}
		check(ok, u8"一列兩個 AND term：稀有度、汙染各自一個，`!` 開頭加引號，不受模式影響，無衝突" + (ok ? "" : "  " + why));
	}
	{
		bool ok = true;
		for (const char* gid : {"poe1", "poe2"}) {
			const Game& g = G[gid];
			const AlgoPage* sec = SectionPageOf(g.pages, "vendor_bases", gid);
			if (!sec) { ok = false; continue; }
			const CombineResult r = CombineOnly(g, {{"vendor_bases", {0, 1}}, {sec->id, {0}}},
			                                    {{"vendor_bases", {{kRarityEntryId, Ch({}, Corruption::Corrupted)}}}}, "vendor_bases",
			                                    Lang::Zh, Mode::Any);
			const std::string tail = u8" ^已汙染$";
			ok = ok && r.query.size() >= tail.size() && r.query.compare(r.query.size() - tail.size(), tail.size(), tail) == 0;
			for (const Conflict& c : r.conflicts) ok = ok && c.kind != ConflictKind::Fragment;
		}
		const Game& g1 = G["poe1"];
		const AlgoPage* sec = SectionPageOf(g1.pages, "vendor_bases", "poe1");
		const CombineResult r = CombineOnly(g1, {{sec->id, {0}}}, {{"vendor_bases", {{kRarityEntryId, Ch({})}}}}, "vendor_bases",
		                                    Lang::Zh, Mode::Any);
		check(ok, u8"已汙染 term 對有「已汙染」行的語料頁不算誤中（ownLine）");
		check(r.query.empty() && r.conflicts.size() == 1 && r.conflicts[0].kind == ConflictKind::Invalid,
		      u8"什麼都沒選 = 輸入不成立（invalid），不輸出");
	}
	// 舊值相容: a schema-5 state's legacy word reads the same
	{
		RegexUiState st;
		const bool parsed = st.Parse(u8"{\"schema\":5,\"current\":[{\"page\":\"waystone_mods\",\"keys\":[],\"alt\":[],\"num\":[\"item_rarity_class\"]}],"
		                             u8"\"numeric\":{\"waystone_mods\":{\"item_rarity_class\":{\"choice\":\"magic\"}}}}");
		const Game& g = G["poe2"];
		const AlgoPage* sec = SectionPageOf(g.pages, "waystone_mods", "poe2");
		RegexEmbed::ValuesMap vm;
		if (const RegexValueList* m = st.NumericOf("waystone_mods"))
			for (const auto& kv : *m) vm["waystone_mods"][kv.first] = kv.second;
		PageRef sr;
		sr.algo = sec;
		const CombineResult q = CombineOnly(g, {{sec->id, {EntryIdx(sr, kRarityEntryId)}}}, vm, "waystone_mods", Lang::Zh, Mode::Any);
		check(parsed && q.query == u8"\"稀有度[:：] *魔法\"", u8"schema 5 state 的 item_rarity_class 舊單字照讀，輸出與第 35 步相同：" + q.query);
	}
	// 書籤: vendor_bases + its condition section round trip
	{
		const Game& g = G["poe1"];
		const PageRef* host = g.Find("vendor_bases");
		const AlgoPage* sec = SectionPageOf(g.pages, "vendor_bases", "poe1");
		const RegexEmbed::PicksMap picks{{"vendor_bases", {2, 5}}, {sec->id, {0}}};
		const RegexEmbed::ValuesMap values{{"vendor_bases", {{kRarityEntryId, Ch({"normal"}, Corruption::Uncorrupted)}}}};
		const std::optional<RegexBookmark> body = RegexEmbed::BookmarkBodyOf(g.pages, *host, picks, values, "poe1", "any", "zh");
		const std::string want = CombineOnly(g, picks, values, "vendor_bases", Lang::Zh, Mode::Any).query;
		bool ok = body && body->num == std::vector<std::string>{kRarityEntryId} && body->numeric.size() == 1 &&
		          body->numeric[0].second.choice == "n|u" && want.find(u8"\"稀有度[:：] *普通\" \"!^已汙染$\"") != std::string::npos;
		if (ok) {
			const std::optional<RegexEmbed::BookmarkApply> a = RegexEmbed::BookmarkApplyOf(g.pages, *body);
			RegexEmbed::PicksMap bp;
			RegexEmbed::ValuesMap bv;
			if (a) {
				for (const auto& p : a->picks) bp[p.first] = p.second;
				for (const auto& v : a->values)
					for (const auto& kv : v.second) bv[v.first][kv.first] = kv.second;
			}
			ok = a && CombineOnly(g, bp, bv, "vendor_bases", Lang::Zh, Mode::Any).query == want;
		}
		check(ok, u8"書籤：物品基底 + 條件區往返，單頁輸出相同");
	}
	// 物品詞綴數值頁: the host's own values and the section's share the store key
	{
		int ok = 0, n = 0;
		std::string why;
		for (const char* gid : {"poe1", "poe2"}) {
			Game& g = G[gid];
			if (!g.itemOk) continue;
			n++;
			const std::string pid = IM::PageId(gid);
			const PageRef* page = g.Find(pid);
			const AlgoPage* sec = SectionPageOf(g.pages, pid, gid);
			if (!page || !sec || sec->id != pid + "_cond" || page->Size() == 0) { why = "wiring"; continue; }
			const std::string e0 = page->algo->entries[0].def.id;
			AlgoValue seven;
			seven.min = 7;
			const RegexEmbed::PicksMap picks{{pid, {0}}, {sec->id, {0}}};
			const RegexEmbed::ValuesMap values{{pid, {{e0, seven}, {kRarityEntryId, Ch({"rare"}, Corruption::Uncorrupted)}}}};
			const std::string want = CombineOnly(g, picks, values, pid, Lang::En, Mode::Any).query;
			const std::string tail = u8" \"Rarity[:：] *Rare\" \"!^Corrupted$\"";
			if (want.size() < tail.size() || want.compare(want.size() - tail.size(), tail.size(), tail) != 0) { why = want; continue; }
			// bookmark
			const std::optional<RegexBookmark> body = RegexEmbed::BookmarkBodyOf(g.pages, *page, picks, values, gid, "any", "en");
			if (!body || body->numeric.size() != 2 || !RegexValueFind(body->numeric, e0) || !RegexValueFind(body->numeric, kRarityEntryId)) {
				why = "bookmark body";
				continue;
			}
			const std::optional<RegexEmbed::BookmarkApply> a = RegexEmbed::BookmarkApplyOf(g.pages, *body);
			RegexEmbed::PicksMap bp;
			RegexEmbed::ValuesMap bv;
			if (a) {
				for (const auto& p : a->picks) bp[p.first] = p.second;
				for (const auto& v : a->values)
					for (const auto& kv : v.second) bv[v.first][kv.first] = kv.second;
			}
			if (!a || bv[pid].size() != 2 || CombineOnly(g, bp, bv, pid, Lang::En, Mode::Any).query != want) { why = "bookmark apply"; continue; }
			// share code
			const S::State st = S::StateOf(gid, g.pages, picks, values, "any", {}, {});
			Dump(std::string("rarity item-mods + cond ") + gid, st);   // A -> B: verify-a-codes.mts
			S::Normalized d;
			std::string err;
			if (!S::Decode(S::Encode(st), d, &err) || !d.warnings.empty()) { why = "decode " + err; continue; }
			const RegexValueList* nv = nullptr;
			for (const auto& kv : d.state.numeric)
				if (kv.first == pid) nv = &kv.second;
			if (!nv || nv->size() != 2) { why = "share numeric"; continue; }
			const S::Resolved r = S::Resolve(d.state, g.pages);
			const S::ValueLists back = S::ResolvedValues(r.values);
			RegexEmbed::ValuesMap rv;
			for (const auto& kv : back)
				for (const auto& e : kv.second) rv[kv.first][e.first] = e.second;
			RegexEmbed::PicksMap rp;
			for (const auto& kv : r.picks) rp[kv.first] = kv.second;
			if (rp[pid] != std::vector<int>{0} || rp[sec->id] != std::vector<int>{0} || rv[pid].size() != 2 ||
			    CombineOnly(g, rp, rv, pid, Lang::En, Mode::Any).query != want) { why = "share resolve"; continue; }
			ok++;
		}
		check(n > 0 && ok == n, u8"物品詞綴數值頁：宿主自己的數值與條件區共用存放鍵 → 書籤 / 分享碼兩邊都保留（" + Num(ok) + " / " + Num(n) + u8" 遊戲）" +
		                            (why.empty() ? "" : "  " + why));
	}
	// 分享碼: the tablet condition section (sections + numeric under the host id)
	{
		const Game& g = G["poe2"];
		const AlgoPage* sec = SectionPageOf(g.pages, "tablet_mods", "poe2");
		const RegexEmbed::PicksMap picks{{"tablet_mods", {1}}, {sec->id, {0}}};
		const RegexEmbed::ValuesMap values{{"tablet_mods", {{kRarityEntryId, Ch({"magic"})}}}};
		const S::State st = S::StateOf("poe2", g.pages, picks, values, "any", {}, {});
		Dump("rarity tablet cond", st);
		bool ok = st.sections.size() == 1 && st.sections[0].first == "tablet_mods" &&
		          st.sections[0].second == std::vector<std::string>{kRarityEntryId} && st.numeric.size() == 1 &&
		          st.numeric[0].first == "tablet_mods" && st.numeric[0].second.size() == 1 && st.numeric[0].second[0].second.choice == "m";
		S::Normalized d;
		std::string err;
		ok = ok && S::Decode(S::Encode(st), d, &err);
		const S::Resolved r = S::Resolve(d.state, g.pages);
		auto it = r.picks.find(sec->id);
		ok = ok && it != r.picks.end() && it->second == std::vector<int>{0};
		const S::ValueLists back = S::ResolvedValues(r.values);
		bool val = false;
		for (const auto& kv : back)
			if (kv.first == "tablet_mods") val = kv.second.size() == 1 && kv.second[0].second.choice == "m";
		check(ok && val, u8"分享碼：碑牌條件區（sections + numeric 以宿主 id 為鍵）往返");
	}
}

} // namespace

void RegexR8Tests(const std::wstring& exeDir, void (*checkFn)(bool, const std::string&), void (*lineFn)(const std::string&))
{
	g_check = checkFn;
	g_line = lineFn;
	g_dump.clear();
	line("--- R8: share codes + templates (regex_share) ---");
	const DWORD t0 = GetTickCount();
	CodecTests();
	RegexDataset ds;
	std::string err;
	if (!ds.Load(exeDir, L"poe1", &err)) {
		check(false, "R8: load data: " + err);
		return;
	}
	std::map<std::string, Game> G;
	for (const char* g : {"poe1", "poe2"}) {
		Game& gm = G[g];
		gm.id = g;
		gm.algo = AlgoPages(g, ds.Labels(g));
		IM::Data data;
		std::string ierr;
		gm.itemOk = IM::LoadFile(exeDir, g, data, &ierr);
		check(gm.itemOk, std::string(u8"載入物品詞綴資料 ") + g + (gm.itemOk ? "" : "  " + ierr));
		gm.algo.push_back(IM::MakePage(g, gm.itemOk ? &data : nullptr));
		for (const RegexPageDef& p : ds.Pages())
			if (p.game == g) gm.pages.push_back({&p, nullptr});
		for (const AlgoPage& p : gm.algo) gm.pages.push_back({nullptr, &p});
	}
	DWORD t = t0;
	auto lap = [&](const char* what) {
		line(std::string("    (") + what + " " + Num((long long)(GetTickCount() - t)) + " ms)");
		t = GetTickCount();
	};
	lap("codec + item-mod data");
	ShareTestPort(G["poe1"]);
	TemplateTests(exeDir, G);
	TemplatesMatchGolden(exeDir);
	lap("share.test / templates");
	RoundTrips(G);
	lap("round trips");
	GoldenR8(G);
	lap("golden");
	RarityTests(G);
	lap("rarity | corruption");
	WriteDump();
	line("    (R8 checks took " + Num((long long)(GetTickCount() - t0)) + " ms)");
}
