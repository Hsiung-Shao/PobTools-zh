// --regex-selftest, bookmark-pack part (regex_bookmarks_share.h, "送到 ExileAppraiser"):
//
//   * exile-appraiser regex/test/bookmarks-share.test.ts @ c9a7aae, ported case by case
//     (bookmarkMissed is receiver-side display there and is not ported);
//   * regex_r9_golden.inc, made by tools/regex_port/gen-golden-r9.mts from that TS:
//     docs/regex-share-cli.md's three examples, B-made codes, normalizeBookmarkPack cases,
//     damaged codes, mergeBookmarks over state files, rarity | corruption toggles;
//   * the three examples built HERE from matching states (PackOf) -> the docs' JSON, byte for byte;
//   * the selection model of the "選擇要傳送的書籤" dialog (whole folder, partial, both games,
//     uncategorised, empty folders, hotkeys dropped, unsendable ones counted);
//   * importing a pack from the clipboard (Merge into the panel's state).
//
// With POBTOOLS_REGEX_BOOKMARKS_DUMP=<file> set, every pack made here is written to that file
// (label, canonical JSON, code) for tools/regex_port/verify-a-bookmarks.mts, which has
// exile-appraiser's decodeBookmarks read them back (the A -> B direction).
#include "regex_bookmarks_share.h"
#include "regex_algo_pages.h"
#include "regex_folders.h"
#include "regex_share.h"
#include "regex_state.h"
#include "regex_state_json.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>
#include <vector>

#include <json.hpp>

namespace {

#include "regex_r9_golden.inc"

namespace BS = RegexBookmarksShare;
using json = nlohmann::json;
using ojson = nlohmann::ordered_json;
using RegexFrag::AlgoValue;

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_line)(const std::string&) = nullptr;
void check(bool ok, const std::string& what) { g_check(ok, what); }
void line(const std::string& s) { g_line(s); }
std::string Num(long long n) { return std::to_string(n); }

std::string Clip(const std::string& s, size_t n = 160)
{
	if (s.size() <= n) return s;
	size_t k = n;
	while (k > 0 && ((unsigned char)s[k] & 0xc0) == 0x80) k--;
	return s.substr(0, k) + "...";
}

struct Dumped {
	std::string label, json, code;
};
std::vector<Dumped> g_dump;
void Dump(const std::string& label, const BS::Pack& p) { g_dump.push_back({label, BS::CanonicalJson(p), BS::Encode(p)}); }

RegexBookmark Bm(const std::string& name, const std::string& game = "poe1", const std::string& folder = "",
                 const std::string& page = "map_mods")
{
	RegexBookmark b;
	b.name = name;
	b.page = page;
	b.game = game;
	b.keys = {"k"};
	b.alt = {u8"甲"};
	b.folder = folder;
	return b;
}

BS::Pack PackOfList(std::vector<RegexBookmark> bms, std::vector<RegexBookmarkFolder> f1 = {},
                    std::vector<RegexBookmarkFolder> f2 = {})
{
	BS::Pack p;
	p.bookmarks = std::move(bms);
	p.folders[0] = std::move(f1);
	p.folders[1] = std::move(f2);
	return p;
}

std::string Names(const std::vector<RegexBookmark>& v, const std::string& game = "")
{
	std::string s;
	for (const RegexBookmark& b : v)
		if (game.empty() || b.game == game) s += (s.empty() ? "" : ",") + b.folder + "/" + b.name;
	return s;
}

// gen-golden-r9.mts bmRow
ojson BmRow(const RegexBookmark& b)
{
	ojson a = ojson::array();
	a.push_back(b.name);
	a.push_back(b.page);
	a.push_back(b.game);
	a.push_back(b.mode);
	a.push_back(b.lang);
	a.push_back(b.keys);
	a.push_back(b.alt);
	if (b.numeric.empty()) {
		a.push_back(nullptr);
	} else {
		ojson m = ojson::object();
		for (const auto& kv : b.numeric) m[kv.first] = RegexStateJson::ValueTo(kv.second);
		a.push_back(std::move(m));
	}
	a.push_back(b.num);
	a.push_back(b.hotkey);
	a.push_back(b.folder);
	return a;
}

ojson FoldersJson(const RegexUiState& s)
{
	ojson fo;
	for (int gi = 0; gi < 2; gi++) {
		ojson a = ojson::array();
		for (const RegexBookmarkFolder& f : s.folders[gi]) {
			ojson o;
			o["name"] = f.name;
			o["collapsed"] = f.collapsed;
			a.push_back(std::move(o));
		}
		fo[gi ? "poe2" : "poe1"] = std::move(a);
	}
	return fo;
}

std::string MergeDump(const BS::MergeResult& r)
{
	ojson o;
	ojson bms = ojson::array();
	for (const RegexBookmark& b : r.state.bookmarks) bms.push_back(BmRow(b));
	o["bookmarks"] = std::move(bms);
	o["folders"] = FoldersJson(r.state);
	ojson added = ojson::array(), renamed = ojson::array(), created = ojson::array();
	for (const BS::Merged& a : r.added) added.push_back({a.game, a.folder, a.name, a.originalName});
	for (const BS::Merged& a : r.renamed) renamed.push_back(a.name);
	for (const auto& f : r.foldersCreated) created.push_back({f.first, f.second});
	o["added"] = std::move(added);
	o["renamed"] = std::move(renamed);
	o["created"] = std::move(created);
	return o.dump(-1, ' ', false, ojson::error_handler_t::replace);
}

// ---- bookmarks-share.test.ts ---------------------------------------------------------------

void TestPort()
{
	line("  bookmarks-share.test.ts (exile-appraiser c9a7aae)");
	// 編碼: 往返;正規 JSON 鍵順序固定;hotkey 不寫出;選填欄位空的不寫
	{
		RegexBookmark a = Bm(u8"地圖", "poe1", u8"刷圖");
		a.hotkey = "Ctrl + 1";
		a.num = {"tier"};
		AlgoValue v;
		v.min = 16;
		RegexValueSet(a.numeric, "tier", v);
		RegexBookmark b = Bm(u8"換界石", "poe2", "", "waystone_mods");
		b.mode = "all";
		b.lang = "en";
		b.keys = {"a", "b"};
		b.alt = {u8"甲", u8"乙"};
		const BS::Pack p = PackOfList({a, b}, {{u8"刷圖", true}});
		const std::string js = BS::CanonicalJson(p);
		const std::string want =
			u8"{\"kind\":\"regex-bookmarks\",\"v\":1,\"folders\":{\"poe1\":[{\"name\":\"刷圖\",\"collapsed\":true}],\"poe2\":[]},\"bookmarks\":["
			u8"{\"name\":\"地圖\",\"page\":\"map_mods\",\"game\":\"poe1\",\"mode\":\"any\",\"lang\":\"zh\",\"keys\":[\"k\"],\"alt\":[\"甲\"],\"numeric\":{\"tier\":{\"min\":16}},\"num\":[\"tier\"],\"folder\":\"刷圖\"},"
			u8"{\"name\":\"換界石\",\"page\":\"waystone_mods\",\"game\":\"poe2\",\"mode\":\"all\",\"lang\":\"en\",\"keys\":[\"a\",\"b\"],\"alt\":[\"甲\",\"乙\"]}]}";
		check(js == want, u8"正規 JSON 鍵順序固定、選填欄位空的不寫：" + Clip(js));
		check(js.find("hotkey") == std::string::npos, u8"hotkey 不寫出");
		BS::Normalized d;
		std::string err;
		const bool ok = BS::Decode(BS::Encode(p), d, &err);
		check(ok && d.warnings.empty() && BS::CanonicalJson(d.pack) == js && d.pack.bookmarks[0].hotkey.empty(),
		      u8"往返：解碼後正規 JSON 相同、沒有警告、沒有 hotkey " + err);
		Dump("test.ts encode", p);
	}
	// 解碼時 hotkey 也丟掉
	{
		BS::Normalized d;
		std::string err;
		const bool ok = BS::NormalizeJson(
			u8"{\"kind\":\"regex-bookmarks\",\"v\":1,\"bookmarks\":[{\"name\":\"n\",\"page\":\"map_mods\",\"game\":\"poe1\",\"mode\":\"any\",\"lang\":\"zh\",\"keys\":[\"k\"],\"alt\":[\"甲\"],\"hotkey\":\"F5\"}]}",
			d, &err);
		check(ok && d.pack.bookmarks.size() == 1 && d.pack.bookmarks[0].hotkey.empty() && d.warnings.empty(),
		      u8"解碼時 hotkey 也丟掉（對方若帶了）");
	}
	// 分享碼輸出在抽出共用 gzip 後不變
	{
		RegexShare::State s;
		s.game = "poe1";
		s.pages.push_back({"map_mods", {"x"}});
		check(RegexShare::Encode(s) == RegexShare::Base64Url(RegexShare::Gzip(RegexShare::ToJson(s))),
		      u8"分享碼輸出在抽出共用 gzip 後不變（同一條路徑）");
		RegexShare::Normalized n;
		std::string err;
		const bool refused = !RegexShare::Decode("", n, &err);
		check(refused && err == u8"分享碼是空的", u8"分享碼的錯誤訊息仍是「分享碼…」：" + err);
	}
	// 損壞輸入: kind / v 不符、不是物件
	{
		BS::Normalized d;
		std::string e1, e2, e3, e4;
		BS::NormalizeJson("null", d, &e1);
		BS::NormalizeJson(R"({"kind":"x","v":1})", d, &e2);
		BS::NormalizeJson(R"({"kind":"regex-bookmarks","v":2})", d, &e3);
		BS::NormalizeJson("{x", d, &e4);
		check(e1.find(u8"不是物件") != std::string::npos && e2.find(u8"不是書籤包") != std::string::npos &&
		          e3.find(u8"版本不符") != std::string::npos && e4 == u8"書籤包內容不是 JSON",
		      u8"kind / v 不符、不是物件、不是 JSON → 失敗：" + e1 + " / " + e2 + " / " + e3 + " / " + e4);
	}
	// 壞書籤 / 資料夾略過並記 warning
	{
		BS::Normalized d;
		std::string err;
		const bool ok = BS::NormalizeJson(
			u8R"({"kind":"regex-bookmarks","v":1,"extra":1,"folders":{"poe1":[{"name":"  A  "},{"name":""},3,{"name":"A"}],"poe2":"x"},)"
			u8R"("bookmarks":[{"name":"ok","page":"map_mods","game":"poe1","keys":["k"]},{"name":5},{"name":"","page":"map_mods","game":"poe1","keys":["k"]},)"
			u8R"({"name":"g","page":"map_mods","game":"","keys":["k"]},{"name":"f","page":"map_mods","game":"poe1","keys":["k"],"folder":"B"},"x"]})",
			d, &err);
		std::string f1;
		for (const RegexBookmarkFolder& f : d.pack.folders[0]) f1 += f.name + ",";
		std::string all;
		for (const std::string& w : d.warnings) all += w + "\n";
		check(ok && f1 == "A,B," && d.pack.folders[1].empty() && Names(d.pack.bookmarks) == "/ok,B/f" && d.warnings.size() >= 6 &&
		          all.find(u8"未知欄位「extra」") != std::string::npos,
		      u8"壞書籤 / 資料夾略過並記 warning；沒遊戲的略過；書籤的資料夾沒列在 folders 就補（" + Num((long long)d.warnings.size()) + u8" 個警告）");
	}
	// 解碼:不是 base64url / 不是 gzip / 不是 JSON / 超長 / 壓縮炸彈
	{
		BS::Normalized d;
		std::string e1, e2, e3, e4, e5, e6;
		BS::Decode("", d, &e1);
		BS::Decode("!!!", d, &e2);
		BS::Decode(RegexShare::Base64Url("abc"), d, &e3);
		BS::Decode(RegexShare::Base64Url(RegexShare::Gzip("{x")), d, &e4);
		BS::Decode(std::string(RegexShare::kMaxCodeChars + 1, 'A'), d, &e5);
		BS::Decode(RegexShare::Base64Url(RegexShare::Gzip(std::string(RegexShare::kMaxJsonBytes + 1024, '\0'))), d, &e6);
		check(e1 == u8"書籤包是空的" && e2.rfind(u8"書籤包無法解壓縮", 0) == 0 && e3.rfind(u8"書籤包無法解壓縮", 0) == 0 &&
		          e4 == u8"書籤包內容不是 JSON" && e5.rfind(u8"書籤包太長", 0) == 0 &&
		          e6.rfind(u8"書籤包無法解壓縮", 0) == 0 && e6.find(u8"超過") != std::string::npos,
		      u8"損壞的碼：空 / 非 base64url / 非 gzip / 非 JSON / 超長 / 壓縮炸彈 → " + e1 + " | " + e2 + " | " + e3 + " | " + e4 +
		          " | " + Clip(e5, 40) + " | " + e6);
	}
	// parseBookmark 與 regex_state.json 讀檔同一套規則
	{
		auto parse = [](const char* text, RegexBookmark& b, std::string* threw) {
			try {
				return RegexParseBookmark(ojson::parse(text), b);
			} catch (const std::exception& e) {
				*threw = e.what();
				return false;
			}
		};
		RegexBookmark b;
		std::string threw;
		const bool hk = parse(R"({"name":"n","page":"map_mods","game":"poe1","keys":["k"],"hotkey":" F1 "})", b, &threw) && b.hotkey == "F1";
		const bool noKeys = !parse(R"({"name":"n","page":"map_mods","game":"poe1","keys":[]})", b, &threw);
		const bool numOnly = parse(R"({"name":"n","page":"map_mods","game":"poe1","keys":[],"num":["tier"]})", b, &threw) &&
		                     b.num == std::vector<std::string>{"tier"};
		threw.clear();
		parse(R"({"name":1,"page":"map_mods","keys":["k"]})", b, &threw);
		check(hk && noKeys && numOnly && threw == u8"欄位 name 不是字串",
		      u8"parseBookmark：hotkey 保留（state 檔）、沒鍵 = 略過、只有 num 可以、型別不符丟例外「" + threw + u8"」");
		RegexUiState st;
		check(!st.Parse(R"({"schema":5,"bookmarks":[{"name":1,"page":"p","keys":["k"]}]})"),
		      u8"regex_ui.json 裡書籤欄位型別不符仍是整份拒讀（行為不變）");
	}
	// mergeBookmarks
	auto base = []() {
		RegexUiState s;
		RegexBookmark a = Bm(u8"地圖", "poe1", u8"刷圖");
		a.hotkey = "Ctrl + 1";
		s.bookmarks = {a, Bm(u8"地圖"), Bm(u8"換界石", "poe2", "", "waystone_mods")};
		s.folders[0] = {{u8"刷圖", true}};
		RegexPagePicks c;
		c.page = "map_mods";
		c.keys = {"k"};
		s.current = {c};
		AlgoValue v;
		v.min = 16;
		RegexValueSet(s.NumericFor("map_mods"), "tier", v);
		s.custom = {"c"};
		s.excludes = {"e"};
		return s;
	};
	{
		const RegexUiState s = base();
		const BS::MergeResult r = BS::Merge(s, PackOfList({Bm(u8"地圖", "poe1", u8"刷圖"), Bm(u8"地圖", "poe1", u8"刷圖"),
		                                                   Bm(u8"地圖", "poe1", u8"新"), Bm(u8"地圖")}));
		std::string added, renamed;
		for (const BS::Merged& a : r.added) added += a.folder + "/" + a.name + ",";
		for (const BS::Merged& a : r.renamed) renamed += a.name + ",";
		int noHotkey = 0;
		for (const RegexBookmark& b : r.state.bookmarks) noHotkey += b.hotkey.empty() ? 1 : 0;
		check(added == u8"刷圖/地圖 (2),刷圖/地圖 (3),新/地圖,/地圖 (2)," && renamed == u8"地圖 (2),地圖 (3),地圖 (2)," &&
		          Names(r.state.bookmarks, "poe1") == u8"刷圖/地圖,刷圖/地圖 (2),刷圖/地圖 (3),新/地圖,/地圖,/地圖 (2)" &&
		          r.state.bookmarks[0].hotkey == "Ctrl + 1" && noHotkey == 6,
		      u8"merge：同遊戲 + 同資料夾 + 同名 → (2)(3)；不同資料夾同名不改；既有的不動（hotkey 保留）：" + added);
	}
	{
		const BS::MergeResult r = BS::Merge(base(), PackOfList({Bm("x", "poe1", "B"), Bm("y", "poe2", "C", "waystone_mods")},
		                                                       {{u8"刷圖", false}, {"A", true}, {"B", false}}, {{"C", false}}));
		std::string f1, created;
		for (const RegexBookmarkFolder& f : r.state.folders[0]) f1 += f.name + (f.collapsed ? "+" : "-") + ",";
		for (const auto& f : r.foldersCreated) created += f.first + ":" + f.second + ",";
		check(f1 == u8"刷圖+,A+,B-," && r.state.folders[1].size() == 1 && r.state.folders[1][0].name == "C" &&
		          created == "poe1:A,poe1:B,poe2:C," && Names(r.state.bookmarks, "poe2") == u8"C/y,/換界石",
		      u8"merge：資料夾已有就併入（收合不動）、沒有的依包內順序加在最後；另一遊戲不受影響：" + f1);
	}
	{
		const RegexUiState s = base();
		const std::string before = s.Serialize();
		const BS::MergeResult r = BS::Merge(s, PackOfList({Bm("z", "poe1", u8"新")}));
		check(s.Serialize() == before && r.state.current.size() == 1 && r.state.current[0].keys == s.current[0].keys &&
		          r.state.numeric.size() == s.numeric.size() && r.state.custom == s.custom && r.state.excludes == s.excludes &&
		          r.state.outScope == s.outScope && r.state.mode == s.mode && r.state.bookmarks.size() == 4,
		      u8"merge 不改輸入；勾選 / 數值 / 自訂 / 排除 / 輸出範圍 / 模式不動");
		const BS::MergeResult e = BS::Merge(s, BS::Pack{});
		check(e.added.empty() && e.state.bookmarks.size() == 3 && e.foldersCreated.empty(), u8"空包 = 什麼都沒加");
	}
}

// ---- golden --------------------------------------------------------------------------------

void Golden(std::vector<std::string>& docJson)
{
	line("  golden (exile-appraiser bookmarks-share.ts / state.ts / rarity.ts)");
	const char* names[6] = {"doc", "code", "norm", "bad", "merge", "rarity"};
	int counts[6] = {}, fails[6] = {};
	int byteSame = 0, codes = 0;
	std::vector<std::string> msgs;
	auto fail = [&](int k, const std::string& m) {
		fails[k]++;
		if (msgs.size() < 12) msgs.push_back(m);
	};
	for (const char* rec : kRegexR9Golden) {
		const json r = json::parse(rec);
		const std::string kind = r[0].get<std::string>();
		const std::string label = r[1].get<std::string>();
		int k = 0;
		while (k < 6 && kind != names[k]) k++;
		if (k == 6) {
			fail(0, "unknown kind " + kind);
			continue;
		}
		counts[k]++;
		if (kind == "doc" || kind == "code") {
			const std::string code = r[2].get<std::string>(), want = r[3].get<std::string>();
			BS::Normalized d;
			std::string err;
			if (!BS::Decode(code, d, &err)) { fail(k, label + ": decode " + err); continue; }
			const std::string got = BS::CanonicalJson(d.pack);
			if (got != want || !d.warnings.empty()) { fail(k, label + ": " + Clip(got) + " != " + Clip(want)); continue; }
			// our own code of the same pack decodes to the same JSON
			const std::string mine = BS::Encode(d.pack);
			BS::Normalized d2;
			if (!BS::Decode(mine, d2, &err) || BS::CanonicalJson(d2.pack) != want) { fail(k, label + ": A round trip"); continue; }
			codes++;
			byteSame += mine == code ? 1 : 0;
			Dump("golden " + label, d.pack);
			if (kind == "doc") docJson.push_back(want);
		} else if (kind == "norm") {
			const json& want = r[3];
			BS::Normalized d;
			std::string err;
			const bool ok = BS::NormalizeJson(r[2].get<std::string>(), d, &err);
			if (want.contains("error")) {
				if (ok || err != want["error"].get<std::string>()) fail(k, label + ": error " + err + " != " + want["error"].dump());
				continue;
			}
			if (!ok) { fail(k, label + ": threw " + err); continue; }
			if (BS::CanonicalJson(d.pack) != want["json"].get<std::string>()) {
				fail(k, label + ": " + Clip(BS::CanonicalJson(d.pack)) + " != " + Clip(want["json"].get<std::string>()));
				continue;
			}
			if (json(d.warnings) != want["warnings"]) { fail(k, label + ": warnings " + Clip(json(d.warnings).dump())); continue; }
		} else if (kind == "bad") {
			std::string code = r[2].get<std::string>();
			if (code.rfind("@A*", 0) == 0) code = std::string((size_t)std::stoull(code.substr(3)), 'A');
			const std::string want = r[3].get<std::string>();
			BS::Normalized d;
			std::string err;
			const bool ok = BS::Decode(code, d, &err);
			// The inflate / base64 detail inside "無法解壓縮(...)" is each side's own wording.
			const std::string pre = u8"書籤包無法解壓縮(";
			const bool same = want.rfind(pre, 0) == 0
				? (err.rfind(pre, 0) == 0 && (want.find(u8"分享碼含有不合法字元") == std::string::npos || err == want))
				: err == want;
			if (ok || !same) fail(k, label + ": " + err + " != " + want);
		} else if (kind == "merge") {
			RegexUiState base;
			if (!base.Parse(r[2].get<std::string>())) { fail(k, label + ": base parse"); continue; }
			BS::Normalized d;
			std::string err;
			if (!BS::NormalizeJson(r[3].get<std::string>(), d, &err)) { fail(k, label + ": pack " + err); continue; }
			const std::string before = base.Serialize();
			const BS::MergeResult m = BS::Merge(base, d.pack);
			const std::string got = MergeDump(m);
			if (got != r[4].get<std::string>()) { fail(k, label + ": " + Clip(got, 300) + " != " + Clip(r[4].get<std::string>(), 300)); continue; }
			if (base.Serialize() != before || m.state.custom != base.custom || m.state.current.size() != base.current.size())
				fail(k, label + ": other state touched");
		} else {   // rarity
			std::string c = r[2].get<std::string>();
			std::vector<std::string> got;
			for (const json& s : r[3]) {
				const std::string st = s.get<std::string>();
				if (st == "uncorrupted") c = RegexAlgo::ToggleCorruptionIn(c, RegexAlgo::Corruption::Uncorrupted);
				else if (st == "corrupted") c = RegexAlgo::ToggleCorruptionIn(c, RegexAlgo::Corruption::Corrupted);
				else c = RegexAlgo::ToggleRarityIn(c, st);
				got.push_back(c);
			}
			if (json(got) != r[4]) fail(k, label + ": " + json(got).dump() + " != " + r[4].dump());
		}
	}
	for (const std::string& m : msgs) line("      FAIL " + m);
	const char* what[6] = {u8"docs/regex-share-cli.md 三組範例碼 → 解碼後正規 JSON 與文件逐字相同、沒有警告",
	                       u8"exile-appraiser 產的書籤包碼 → 解碼後正規 JSON 逐字相同（本工具再編碼也解得回同一份）",
	                       u8"normalizeBookmarkPack：正規 JSON、警告（措辭與順序）、錯誤訊息逐字相同",
	                       u8"損壞的碼：錯誤訊息相同（解壓細節各自措辭，只比到「書籤包無法解壓縮(」）",
	                       u8"mergeBookmarks（regex_state.json 文字 + 包）：書籤、資料夾、新增 / 改名 / 新資料夾逐字相同；其他狀態不動",
	                       u8"稀有度 | 汙染條件列 choice：切換序列結果與 rarity.ts 相同（mr|u、|c 等字母編碼）"};
	for (int i = 0; i < 6; i++)
		check(counts[i] > 0 && fails[i] == 0, std::string(what[i]) + "  " + Num(counts[i] - fails[i]) + " / " + Num(counts[i]));
	line(u8"    本工具與 exile-appraiser 的碼逐位元組相同：" + Num(byteSame) + " / " + Num(codes) +
	     u8"（gzip 實作不同，不要求相同；比的是解碼後的 JSON）");
}

// ---- the docs' examples built from our own state ----------------------------------------------

void ExamplesFromState(const std::vector<std::string>& docJson)
{
	line(u8"  docs 範例：從對應的 state 組出的包");
	if (docJson.size() != 3) {
		check(false, u8"golden 裡沒有三組 docs 範例");
		return;
	}
	// One state holding all three examples' bookmarks plus distractors; each example is a selection.
	RegexUiState s;
	const bool parsed = s.Parse(u8R"({"schema":5,"bookmarks":[)"
		u8R"({"name":"範例一","page":"map_mods","game":"poe1","mode":"none","lang":"zh","keys":["#% more Monster Life"],"alt":["#% 更多怪物生命"],"hotkey":"Ctrl + 1"},)"
		u8R"({"name":"範例二 A","page":"map_mods","game":"poe1","mode":"any","lang":"zh","keys":["#% more Monster Life","Players have #% less effect of Flasks applied to them"],"alt":["#% 更多怪物生命","#%更少施加於玩家的藥劑效果"],"numeric":{"tier":{"min":16},"item_rarity_class":{"choice":"mr|u"}},"num":["tier","item_rarity_class"],"folder":"刷圖","hotkey":"F2"},)"
		u8R"({"name":"範例二 B","page":"map_mods","game":"poe1","mode":"any","lang":"en","keys":[],"alt":[],"numeric":{"quantity":{"min":80}},"num":["quantity"],"folder":"刷圖"},)"
		u8R"({"name":"範例三 A","page":"map_mods","game":"poe1","mode":"none","lang":"zh","keys":["#% more Monster Life"],"alt":["#% 更多怪物生命"]},)"
		u8R"({"name":"範例三 B","page":"waystone_mods","game":"poe2","mode":"all","lang":"zh","keys":["Monsters have #% Critical Damage Bonus"],"alt":["#%怪物暴擊傷害加成"],"numeric":{"tier":{"min":15}},"num":["tier"],"folder":"換界石"},)"
		u8R"({"name":"別的","page":"waystone_mods","game":"poe2","keys":["x"],"folder":"其他"})"
		u8R"(],"folders":{"poe1":[{"name":"空的","collapsed":false},{"name":"刷圖","collapsed":false}],"poe2":[{"name":"換界石","collapsed":true},{"name":"其他","collapsed":false}]}})");
	check(parsed, u8"範例 state 讀得進來");
	auto idx = [&](const std::string& name) {
		for (int i = 0; i < (int)s.bookmarks.size(); i++)
			if (s.bookmarks[i].name == name) return i;
		return -1;
	};
	// 範例一: one uncategorised bookmark
	BS::Selection a;
	a.bookmarks.insert(idx(u8"範例一"));
	BS::PackStats st;
	BS::Pack p = BS::PackOf(s, a, &st);
	check(BS::CanonicalJson(p) == docJson[0] && st.bookmarks[0] == 1 && st.folders[0] == 0,
	      u8"範例一（單筆、未分類、hotkey 剔除）逐字相同：" + Clip(BS::CanonicalJson(p)));
	Dump(u8"example 1 from state", p);
	// 範例二: the whole folder 刷圖 (ticked as a folder)
	BS::Selection b;
	BS::SetGroup(s, b, "poe1", u8"刷圖", true);
	p = BS::PackOf(s, b, &st);
	check(BS::CanonicalJson(p) == docJson[1] && st.bookmarks[0] == 2 && st.folders[0] == 1,
	      u8"範例二（整個資料夾，含數值區與 mr|u 條件列）逐字相同：" + Clip(BS::CanonicalJson(p)));
	Dump(u8"example 2 from state", p);
	// 範例三: one bookmark from each game (the poe2 one in a folder: the folder comes along)
	BS::Selection c;
	c.bookmarks.insert(idx(u8"範例三 A"));
	c.bookmarks.insert(idx(u8"範例三 B"));
	p = BS::PackOf(s, c, &st);
	check(BS::CanonicalJson(p) == docJson[2] && st.bookmarks[0] == 1 && st.bookmarks[1] == 1 && st.folders[1] == 1,
	      u8"範例三（兩個遊戲混選；書籤有資料夾就帶上資料夾，收合狀態照本機）逐字相同：" + Clip(BS::CanonicalJson(p)));
	Dump(u8"example 3 from state", p);
}

// ---- selection model ---------------------------------------------------------------------------

void SelectionTests()
{
	line(u8"  選擇要傳送的書籤（資料夾整夾 / 半選 / 跨遊戲 / 未分類 / 空資料夾）");
	RegexUiState s;
	s.folders[0] = {{"F", false}, {"E", true}};
	s.folders[1] = {{"G", false}};
	RegexBookmark f1 = Bm("f1", "poe1", "F"), f2 = Bm("f2", "poe1", "F"), u1 = Bm("u1"), g1 = Bm("g1", "poe2", "G");
	f1.hotkey = "Ctrl + 1";
	RegexBookmark im = Bm("im", "poe2", "", "item_mod_values_poe2");
	RegexBookmark orphan = Bm("orphan", "");
	s.bookmarks = {f1, f2, u1, g1, im, orphan};
	RegexFolders::Normalize(s);
	auto at = [&](const std::string& n) {
		for (int i = 0; i < (int)s.bookmarks.size(); i++)
			if (s.bookmarks[i].name == n) return i;
		return -1;
	};
	BS::Selection sel;
	check(BS::GroupTri(s, sel, "poe1", "F") == BS::Tri::None && BS::AllTri(s, sel) == BS::Tri::None, u8"一開始什麼都沒選");
	BS::SetGroup(s, sel, "poe1", "F", true);
	check(BS::GroupTri(s, sel, "poe1", "F") == BS::Tri::All && sel.bookmarks.count(at("f1")) && sel.bookmarks.count(at("f2")),
	      u8"整夾勾選 = 夾內每筆都勾");
	sel.bookmarks.erase(at("f2"));
	check(BS::GroupTri(s, sel, "poe1", "F") == BS::Tri::Some, u8"取消夾內一筆 → 半選");
	BS::PackStats st;
	BS::Pack p = BS::PackOf(s, sel, &st);
	check(p.bookmarks.size() == 1 && p.bookmarks[0].name == "f1" && p.bookmarks[0].hotkey.empty() && p.folders[0].size() == 1 &&
	          p.folders[0][0].name == "F" && BS::CanonicalJson(p).find("hotkey") == std::string::npos,
	      u8"半選的資料夾：只送勾的書籤，資料夾一起送；hotkey 剔除");
	BS::SetGroup(s, sel, "poe1", "F", false);
	check(BS::GroupTri(s, sel, "poe1", "F") == BS::Tri::None && sel.bookmarks.empty(), u8"整夾取消");
	// empty folder: only as a whole
	BS::SetGroup(s, sel, "poe1", "E", true);
	check(BS::GroupTri(s, sel, "poe1", "E") == BS::Tri::All, u8"空資料夾可整夾勾");
	p = BS::PackOf(s, sel, &st);
	check(p.bookmarks.empty() && p.folders[0].size() == 1 && p.folders[0][0].name == "E" && p.folders[0][0].collapsed && !st.Empty(),
	      u8"只勾空資料夾 → 送一個空資料夾（收合狀態照本機）");
	BS::SetGroup(s, sel, "poe1", "E", false);
	// uncategorised + other game
	BS::SetGroup(s, sel, "poe1", "", true);
	sel.bookmarks.insert(at("g1"));
	sel.bookmarks.insert(at("im"));
	p = BS::PackOf(s, sel, &st);
	check(Names(p.bookmarks) == "/u1,G/g1,/im" && p.folders[0].empty() && p.folders[1].size() == 1 && st.bookmarks[0] == 1 &&
	          st.bookmarks[1] == 2 && st.itemMod == 1,
	      u8"跨遊戲混選：PoE1 未分類 + PoE2 資料夾內一筆 + 物品詞綴頁一筆（計數 1）：" + Names(p.bookmarks));
	// select all / none; the orphan (no game) is not listed, and skipped if ticked by index
	BS::SetAll(s, sel, true);
	check(BS::AllTri(s, sel) == BS::Tri::All && BS::GroupTri(s, sel, "poe2", "G") == BS::Tri::All, u8"全選");
	sel.bookmarks.insert(at("orphan"));
	p = BS::PackOf(s, sel, &st);
	check(p.bookmarks.size() == 5 && st.skipped == 1 && p.folders[0].size() == 2 && p.folders[1].size() == 1,
	      u8"全選：5 筆、PoE1 兩個資料夾（含空的 E）、沒遊戲的那筆略過並計數");
	BS::Normalized d;
	std::string err;
	check(BS::Decode(BS::Encode(p), d, &err) && d.warnings.empty() && BS::CanonicalJson(d.pack) == BS::CanonicalJson(p),
	      u8"組出的包往返不變、沒有警告（對方收到的就是這份）");
	Dump("select all", p);
	BS::SetAll(s, sel, false);
	check(BS::AllTri(s, sel) == BS::Tri::None && BS::PackOf(s, sel, &st).bookmarks.empty() && st.Empty(), u8"全不選");
	// a folder name that needs normalising on the way out
	RegexUiState t;
	t.bookmarks = {Bm("n", "poe2", u8"  A   b ")};
	t.folders[1] = {{u8"  A   b ", false}};
	BS::Selection all;
	BS::SetAll(t, all, true);
	p = BS::PackOf(t, all, &st);
	check(p.bookmarks.size() == 1 && p.bookmarks[0].folder == "A b" && p.folders[1].size() == 1 && p.folders[1][0].name == "A b",
	      u8"送出前資料夾名稱正規化（去頭尾空白、連續空白合一）");
	// bookmarks with an empty name / page / keys are not sent
	RegexUiState u;
	RegexBookmark bad = Bm("x");
	bad.keys.clear();
	u.bookmarks = {bad};
	BS::Selection one;
	one.bookmarks.insert(0);
	p = BS::PackOf(u, one, &st);
	check(p.bookmarks.empty() && st.skipped == 1, u8"沒有鍵的書籤不送（對方也會略過）");
}

// ---- import (clipboard) into the panel's state ---------------------------------------------------

void ImportTests()
{
	line(u8"  從剪貼簿匯入書籤包（併進本工具的書籤）");
	RegexUiState s;
	const bool parsed = s.Parse(u8R"({"schema":5,"current":[{"page":"map_mods","keys":["k"],"alt":[]}],"bookmarks":[)"
		u8R"({"name":"範例一","page":"map_mods","game":"poe1","keys":["k"],"hotkey":"F1"}],"custom":["c"]})");
	RegexBookmark x = Bm(u8"範例一");
	x.hotkey = "F9";
	const BS::Pack p = PackOfList({x, Bm("y", "poe2", u8"新的", "waystone_mods")});
	BS::Normalized d;
	std::string err;
	const bool ok = parsed && BS::Decode("  " + BS::Encode(p) + "\r\n", d, &err);
	const BS::MergeResult r = BS::Merge(s, d.pack);
	RegexUiState after = s;
	after.bookmarks = r.state.bookmarks;
	after.folders[0] = r.state.folders[0];
	after.folders[1] = r.state.folders[1];
	RegexUiState back;
	const bool reread = back.Parse(after.Serialize());
	check(ok && r.renamed.size() == 1 && r.renamed[0].name == u8"範例一 (2)" && after.bookmarks.size() == 3 &&
	          after.bookmarks[0].hotkey == "F1" && after.bookmarks[1].hotkey.empty() && after.folders[1].size() == 1 &&
	          after.current.size() == 1 && after.custom == std::vector<std::string>{"c"} && reread &&
	          back.bookmarks.size() == 3,
	      u8"剪貼簿的碼（前後空白）解得開；同名改「範例一 (2)」；本機 hotkey 不動、包裡的不帶；勾選與自訂文字不動；存檔再讀回一致");
	RegexShare::Normalized sh;
	std::string serr;
	const bool refused = !RegexShare::Decode(BS::Encode(p), sh, &serr);
	BS::Normalized again;
	check(refused && serr.find(u8"遊戲不明") != std::string::npos && BS::Decode(BS::Encode(p), again, &err),
	      u8"書籤包貼到「貼上分享碼」會被拒（" + serr + u8"）而書籤包解得開：面板據此提示改用「匯入書籤包」");
}

void WriteDump()
{
	wchar_t path[MAX_PATH];
	const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_REGEX_BOOKMARKS_DUMP", path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return;
	ojson a = ojson::array();
	for (const Dumped& d : g_dump) a.push_back({{"label", d.label}, {"json", d.json}, {"code", d.code}});
	const std::string text = a.dump(-1, ' ', false, ojson::error_handler_t::replace);
	HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	DWORD w = 0;
	const bool ok = h != INVALID_HANDLE_VALUE && WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size();
	if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
	line(u8"    POBTOOLS_REGEX_BOOKMARKS_DUMP：寫出 " + Num((long long)g_dump.size()) + u8" 個本工具產的書籤包" +
	     (ok ? "" : u8"（寫檔失敗）") + u8"，用 tools/regex_port/verify-a-bookmarks.mts 讓 exile-appraiser 解碼");
}

} // namespace

void RegexR9Tests(void (*check_)(bool, const std::string&), void (*line_)(const std::string&))
{
	g_check = check_;
	g_line = line_;
	const DWORD t0 = GetTickCount();
	line(u8"[bookmark-pack] 書籤包（送到 ExileAppraiser / 匯入）");
	TestPort();
	std::vector<std::string> docJson;
	Golden(docJson);
	ExamplesFromState(docJson);
	SelectionTests();
	ImportTests();
	WriteDump();
	line("    (bookmark-pack checks took " + Num((long long)(GetTickCount() - t0)) + " ms)");
}
