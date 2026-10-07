#include "regex_bookmarks_share.h"

#include "regex_algo_pages.h"   // RegexAlgo::JsTrim
#include "regex_folders.h"
#include "regex_itemmods.h"
#include "regex_share.h"
#include "regex_state_json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>

using nlohmann::ordered_json;

namespace RegexBookmarksShare {

namespace {

const char* const kGames[2] = {"poe1", "poe2"};

int GameIdx(const std::string& g) { return g == "poe2" ? 1 : 0; }

// JS String(x) for the two values the errors quote (kind, v); `v` null = undefined.
std::string JsString(const ordered_json* v)
{
	if (!v) return "undefined";
	switch (v->type()) {
	case ordered_json::value_t::null: return "null";
	case ordered_json::value_t::boolean: return v->get<bool>() ? "true" : "false";
	case ordered_json::value_t::string: return v->get<std::string>();
	case ordered_json::value_t::number_integer: return std::to_string(v->get<long long>());
	case ordered_json::value_t::number_unsigned: return std::to_string(v->get<unsigned long long>());
	case ordered_json::value_t::number_float: {
		const double d = v->get<double>();
		if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 1e21) return RegexStateJson::JsIntegral(d);
		return v->dump();
	}
	case ordered_json::value_t::array: {
		std::string out;   // Array.prototype.join(","): null -> ""
		bool first = true;
		for (const ordered_json& e : *v) {
			if (!first) out += ',';
			first = false;
			if (!e.is_null()) out += JsString(&e);
		}
		return out;
	}
	default: return "[object Object]";
	}
}

const ordered_json* Field(const ordered_json& o, const char* k)
{
	auto it = o.find(k);
	return it == o.end() ? nullptr : &*it;
}

// A canonical array index ("0", "17"; < 2^32 - 1): JS Object.keys lists those first, ascending.
bool ArrayIndex(const std::string& k, uint64_t* n)
{
	if (k.empty() || k.size() > 10 || (k.size() > 1 && k[0] == '0')) return false;
	uint64_t v = 0;
	for (char c : k) {
		if (c < '0' || c > '9') return false;
		v = v * 10 + (uint64_t)(c - '0');
	}
	if (v >= 4294967295ull) return false;
	*n = v;
	return true;
}

// Object.keys order: array indices ascending, then the rest in insertion order.
std::vector<std::string> JsKeys(const ordered_json& o)
{
	std::vector<std::pair<uint64_t, std::string>> idx;
	std::vector<std::string> rest;
	for (auto it = o.begin(); it != o.end(); ++it) {
		uint64_t n = 0;
		if (ArrayIndex(it.key(), &n)) idx.emplace_back(n, it.key());
		else rest.push_back(it.key());
	}
	std::sort(idx.begin(), idx.end());
	std::vector<std::string> out;
	for (auto& p : idx) out.push_back(p.second);
	out.insert(out.end(), rest.begin(), rest.end());
	return out;
}

// bookmarks-share.ts:36 packBookmark
ordered_json PackBookmark(const RegexBookmark& b)
{
	ordered_json o;
	o["name"] = b.name;
	o["page"] = b.page;
	o["game"] = b.game;
	o["mode"] = b.mode;
	o["lang"] = b.lang;
	o["keys"] = b.keys;
	o["alt"] = b.alt;
	if (!b.numeric.empty()) {
		ordered_json m = ordered_json::object();
		for (const auto& kv : b.numeric) m[kv.first] = RegexStateJson::ValueTo(kv.second);
		o["numeric"] = std::move(m);
	}
	if (!b.num.empty()) o["num"] = b.num;
	if (!b.folder.empty()) o["folder"] = b.folder;
	return o;
}

// bookmarks-share.ts:137 slotKey
std::string SlotKey(const std::string& game, const std::string& folder, const std::string& name)
{
	return game + std::string(1, '\0') + folder + std::string(1, '\0') + name;
}

bool HasFolder(const std::vector<RegexBookmarkFolder>& list, const std::string& name)
{
	for (const RegexBookmarkFolder& f : list)
		if (f.name == name) return true;
	return false;
}

struct PackError : std::exception {
	std::string msg;
	explicit PackError(std::string m) : msg(std::move(m)) {}
	const char* what() const noexcept override { return msg.c_str(); }
};

// bookmarks-share.ts:69 normalizeBookmarkPack
Normalized NormalizeValue(const ordered_json& raw)
{
	if (!raw.is_object()) throw PackError("書籤包內容不是物件");
	const ordered_json* kind = Field(raw, "kind");
	if (!kind || !kind->is_string() || kind->get<std::string>() != kKind)
		throw PackError("不是書籤包(kind = " + JsString(kind) + ")");
	const ordered_json* v = Field(raw, "v");
	if (!v || !v->is_number() || v->get<double>() != (double)kVersion)
		throw PackError("書籤包版本不符(" + JsString(v) + ",本版只認 " + std::to_string(kVersion) + ")");
	Normalized out;
	std::vector<std::string>& warnings = out.warnings;
	Pack& pack = out.pack;
	const ordered_json* rawFolders = Field(raw, "folders");
	if (rawFolders && !rawFolders->is_object()) warnings.push_back("folders 不是物件,已忽略");
	for (int gi = 0; gi < 2; gi++) {
		const std::string g = kGames[gi];
		const ordered_json* list = rawFolders && rawFolders->is_object() ? Field(*rawFolders, kGames[gi]) : nullptr;
		if (!list) continue;
		if (!list->is_array()) {
			warnings.push_back("folders." + g + " 不是陣列,已忽略");
			continue;
		}
		std::set<std::string> seen;
		for (size_t i = 0; i < list->size(); i++) {
			const ordered_json& f = (*list)[i];
			const ordered_json* n = f.is_object() ? Field(f, "name") : nullptr;
			const std::string name = n && n->is_string() ? RegexFolders::NormalizeName(n->get<std::string>()) : "";
			if (name.empty()) {
				warnings.push_back("folders." + g + "[" + std::to_string(i) + "] 沒有名稱,已略過");
				continue;
			}
			if (!seen.insert(name).second) continue;
			const ordered_json* c = f.is_object() ? Field(f, "collapsed") : nullptr;
			pack.folders[gi].push_back({name, c && c->is_boolean() && c->get<bool>()});
		}
	}
	const ordered_json* rawBookmarks = Field(raw, "bookmarks");
	if (rawBookmarks && !rawBookmarks->is_array()) warnings.push_back("bookmarks 不是陣列,已忽略");
	if (rawBookmarks && rawBookmarks->is_array()) {
		for (size_t i = 0; i < rawBookmarks->size(); i++) {
			const std::string at = "bookmarks[" + std::to_string(i) + "]";
			RegexBookmark rec;
			bool ok = false;
			try {
				ok = RegexParseBookmark((*rawBookmarks)[i], rec);
			} catch (const std::exception& e) {
				warnings.push_back(at + " 欄位型別不符(" + e.what() + "),已略過");
				continue;
			}
			if (!ok) {
				warnings.push_back(at + " 缺名稱 / 頁 / 鍵,已略過");
				continue;
			}
			if (rec.game != "poe1" && rec.game != "poe2") {
				warnings.push_back(at + "「" + rec.name + "」沒有遊戲,已略過");
				continue;
			}
			rec.hotkey.clear();
			const std::string folder = RegexFolders::NormalizeName(rec.folder);
			rec.folder = folder;
			if (!folder.empty() && !HasFolder(pack.folders[GameIdx(rec.game)], folder))
				pack.folders[GameIdx(rec.game)].push_back({folder, false});
			pack.bookmarks.push_back(std::move(rec));
		}
	}
	for (const std::string& k : JsKeys(raw))
		if (k != "kind" && k != "v" && k != "folders" && k != "bookmarks") warnings.push_back("未知欄位「" + k + "」已忽略");
	return out;
}

std::vector<RegexFolders::Group> GroupsOf(const RegexUiState& s, const std::string& game)
{
	return RegexFolders::GroupBookmarks(s, game, false).groups;
}

const RegexFolders::Group* FindGroup(const std::vector<RegexFolders::Group>& groups, const std::string& folder)
{
	for (const RegexFolders::Group& g : groups)
		if (g.folder == folder) return &g;
	return nullptr;
}

// What the receiver keeps (bookmarks-share.ts:93..106 via parseBookmark): a game,
// a name, a page and some keys or num.
bool Sendable(const RegexBookmark& b)
{
	return (b.game == "poe1" || b.game == "poe2") && !b.name.empty() && !b.page.empty() &&
	       !(b.keys.empty() && b.num.empty());
}

} // namespace

std::string CanonicalJson(const Pack& p)
{
	// bookmarks-share.ts:52 canonicalBookmarkPackJson
	ordered_json doc;
	doc["kind"] = kKind;
	doc["v"] = kVersion;
	ordered_json fo;
	for (int gi = 0; gi < 2; gi++) {
		ordered_json a = ordered_json::array();
		for (const RegexBookmarkFolder& f : p.folders[gi]) {
			ordered_json o;
			o["name"] = f.name;
			o["collapsed"] = f.collapsed;
			a.push_back(std::move(o));
		}
		fo[kGames[gi]] = std::move(a);
	}
	doc["folders"] = std::move(fo);
	ordered_json bm = ordered_json::array();
	for (const RegexBookmark& b : p.bookmarks) bm.push_back(PackBookmark(b));
	doc["bookmarks"] = std::move(bm);
	// JSON.stringify: compact, UTF-8 as is, control characters as \u00xx.
	return doc.dump(-1, ' ', false, ordered_json::error_handler_t::replace);
}

std::string Encode(const Pack& p)
{
	// bookmarks-share.ts:64 encodeBookmarks = gzipBase64url(canonicalBookmarkPackJson(pack))
	return RegexShare::Base64Url(RegexShare::Gzip(CanonicalJson(p)));
}

bool NormalizeJson(const std::string& jsonText, Normalized& out, std::string* err)
{
	ordered_json raw;
	try {
		raw = ordered_json::parse(jsonText);
	} catch (const std::exception&) {
		if (err) *err = "書籤包內容不是 JSON";
		return false;
	}
	try {
		out = NormalizeValue(raw);
	} catch (const std::exception& e) {
		if (err) *err = e.what();
		return false;
	}
	return true;
}

bool Decode(const std::string& code, Normalized& out, std::string* err)
{
	// bookmarks-share.ts:112 decodeBookmarks = normalizeBookmarkPack(gunzipBase64url(code, '書籤包'))
	std::string json;
	if (!RegexShare::GunzipCode(code, "書籤包", json, err)) return false;
	return NormalizeJson(json, out, err);
}

MergeResult Merge(const RegexUiState& s, const Pack& p)
{
	// bookmarks-share.ts:140 mergeBookmarks
	MergeResult r;
	r.state = s;
	RegexUiState& st = r.state;
	for (int gi = 0; gi < 2; gi++) {
		for (const RegexBookmarkFolder& f : p.folders[gi]) {
			if (HasFolder(st.folders[gi], f.name)) continue;
			st.folders[gi].push_back({f.name, f.collapsed});
			r.foldersCreated.emplace_back(kGames[gi], f.name);
		}
	}
	std::set<std::string> taken;
	for (const RegexBookmark& b : s.bookmarks) taken.insert(SlotKey(b.game, b.folder, b.name));
	for (const RegexBookmark& src : p.bookmarks) {
		const std::string& game = src.game;
		const std::string& folder = src.folder;
		std::string name = src.name;
		for (int n = 2; taken.count(SlotKey(game, folder, name)); n++) name = src.name + " (" + std::to_string(n) + ")";
		taken.insert(SlotKey(game, folder, name));
		RegexBookmark b = src;
		b.name = name;
		b.hotkey.clear();
		// a folder the pack did not list (Normalize already added it; belt and braces, as the TS)
		if (!folder.empty() && (game == "poe1" || game == "poe2") && !HasFolder(st.folders[GameIdx(game)], folder)) {
			st.folders[GameIdx(game)].push_back({folder, false});
			r.foldersCreated.emplace_back(game, folder);
		}
		st.bookmarks.push_back(std::move(b));
		r.added.push_back({game, folder, name, src.name});
	}
	RegexFolders::Normalize(st);
	for (const Merged& a : r.added)
		if (a.name != a.originalName) r.renamed.push_back(a);
	return r;
}

Tri GroupTri(const RegexUiState& s, const Selection& sel, const std::string& game, const std::string& folder)
{
	const std::vector<RegexFolders::Group> groups = GroupsOf(s, game);
	const RegexFolders::Group* g = FindGroup(groups, folder);
	if (!g) return Tri::None;
	if (g->items.empty())
		return !folder.empty() && sel.folders.count({game, folder}) ? Tri::All : Tri::None;
	size_t n = 0;
	for (int i : g->items) n += sel.bookmarks.count(i);
	return n == 0 ? Tri::None : n == g->items.size() ? Tri::All : Tri::Some;
}

void SetGroup(const RegexUiState& s, Selection& sel, const std::string& game, const std::string& folder, bool on)
{
	const std::vector<RegexFolders::Group> groups = GroupsOf(s, game);
	const RegexFolders::Group* g = FindGroup(groups, folder);
	if (!g) return;
	for (int i : g->items) {
		if (on) sel.bookmarks.insert(i);
		else sel.bookmarks.erase(i);
	}
	if (folder.empty()) return;
	if (on) sel.folders.insert({game, folder});
	else sel.folders.erase({game, folder});
}

void SetAll(const RegexUiState& s, Selection& sel, bool on)
{
	for (const char* g : kGames)
		for (const RegexFolders::Group& grp : GroupsOf(s, g)) SetGroup(s, sel, g, grp.folder, on);
}

Tri AllTri(const RegexUiState& s, const Selection& sel)
{
	size_t total = 0, picked = 0;
	for (const char* g : kGames)
		for (const RegexFolders::Group& grp : GroupsOf(s, g)) {
			if (grp.items.empty()) {
				if (grp.folder.empty()) continue;
				total++;
				picked += sel.folders.count({g, grp.folder});
				continue;
			}
			total += grp.items.size();
			for (int i : grp.items) picked += sel.bookmarks.count(i);
		}
	return picked == 0 ? Tri::None : picked == total ? Tri::All : Tri::Some;
}

Pack PackOf(const RegexUiState& s, const Selection& sel, PackStats* stats)
{
	PackStats st;
	Pack p;
	for (int i = 0; i < (int)s.bookmarks.size(); i++) {
		if (!sel.bookmarks.count(i)) continue;
		const RegexBookmark& src = s.bookmarks[i];
		if (!Sendable(src)) {
			st.skipped++;
			continue;
		}
		RegexBookmark b = src;
		b.hotkey.clear();
		b.folder = RegexFolders::NormalizeName(b.folder);
		st.bookmarks[GameIdx(b.game)]++;
		if (RegexItemMods::IsPageId(b.page)) st.itemMod++;
		p.bookmarks.push_back(std::move(b));
	}
	for (int gi = 0; gi < 2; gi++) {
		const std::string g = kGames[gi];
		const std::vector<RegexFolders::Group> groups = GroupsOf(s, g);
		auto used = [&](const std::string& name) {
			for (const RegexBookmark& b : p.bookmarks)
				if (b.game == g && b.folder == name) return true;
			return false;
		};
		for (const RegexBookmarkFolder& f : s.folders[gi]) {
			const std::string name = RegexFolders::NormalizeName(f.name);
			if (name.empty() || HasFolder(p.folders[gi], name)) continue;
			const RegexFolders::Group* grp = FindGroup(groups, f.name);
			const bool bare = grp && grp->items.empty() && sel.folders.count({g, f.name});
			if (used(name) || bare) p.folders[gi].push_back({name, f.collapsed});
		}
		// a folder a sent bookmark names but the list lacks (Normalize prevents it; the receiver would add it too)
		for (const RegexBookmark& b : p.bookmarks)
			if (b.game == g && !b.folder.empty() && !HasFolder(p.folders[gi], b.folder))
				p.folders[gi].push_back({b.folder, false});
		st.folders[gi] = (int)p.folders[gi].size();
	}
	if (stats) *stats = st;
	return p;
}

} // namespace RegexBookmarksShare
