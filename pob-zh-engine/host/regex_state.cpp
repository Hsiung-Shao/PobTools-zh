#include "regex_state.h"

#include "regex_algo_pages.h"   // RegexAlgo::SectionHostOf (sections.ts)
#include "regex_folders.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <json.hpp>   // nlohmann::ordered_json (deps/nlohmann)

using nlohmann::ordered_json;
using RegexFrag::AlgoValue;

namespace {

bool ReadAll(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 24)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() ||
		     (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

// state.ts stringArray: the string members of an array field, others skipped.
std::vector<std::string> StringArray(const ordered_json& j, const char* key)
{
	std::vector<std::string> out;
	auto it = j.find(key);
	if (it == j.end() || !it->is_array()) return out;
	for (const auto& v : *it)
		if (v.is_string()) out.push_back(v.get<std::string>());
	return out;
}

// state.ts str(): absent = the default; present but not a string = the whole
// file is refused (a type this build did not write is not something to guess at).
std::string Str(const ordered_json& j, const char* key, const char* def)
{
	auto it = j.find(key);
	if (it == j.end()) return def;
	if (!it->is_string()) throw std::runtime_error(std::string("field ") + key + " is not a string");
	return it->get<std::string>();
}

// A mode or language read back from disk is only allowed to be one of the values
// this build knows. An unknown one is a file from a newer version (or a hand
// edit), and silently carrying it into the UI would put the panel into a state
// no control can express.
std::string OneOf(const std::string& v, const char* a, const char* b, const char* c)
{
	if (v == a || v == b || (c && v == c)) return v;
	return a;
}

std::string GameOf(const std::string& v)
{
	return (v == "poe1" || v == "poe2") ? v : std::string();
}

// JS String.prototype.slice(0, 16) on UTF-16 units, over UTF-8. A code point
// that would straddle the cut is left out rather than split.
std::string SliceUtf16(const std::string& s, size_t units)
{
	size_t used = 0, i = 0;
	while (i < s.size()) {
		const unsigned char c = (unsigned char)s[i];
		const size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
		const size_t u = len == 4 ? 2 : 1;
		if (used + u > units) break;
		used += u;
		i += len;
	}
	return s.substr(0, i < s.size() ? i : s.size());
}

// state.ts numericValue: only the fields AlgoValue knows; numbers truncated.
bool NumericValue(const ordered_json& raw, AlgoValue& v)
{
	if (!raw.is_object()) return false;
	v = AlgoValue{};
	auto num = [&](const char* k, std::optional<double>& out) {
		auto it = raw.find(k);
		if (it == raw.end() || !it->is_number()) return;
		const double d = it->get<double>();
		if (!std::isfinite(d)) return;
		out = std::trunc(d) + 0.0;   // + 0.0: -0 -> 0, as JSON.stringify prints it
	};
	num("min", v.min);
	num("max", v.max);
	auto ch = raw.find("choice");
	if (ch != raw.end() && ch->is_string()) {
		v.choice = SliceUtf16(ch->get<std::string>(), 16);
		v.hasChoice = true;
	}
	return true;
}

RegexValueList NumericMap(const ordered_json& raw)
{
	RegexValueList m;
	if (!raw.is_object()) return m;
	for (auto it = raw.begin(); it != raw.end(); ++it) {
		AlgoValue v;
		if (NumericValue(it.value(), v)) RegexValueSet(m, it.key(), v);
	}
	return m;
}

std::vector<RegexBookmarkFolder> FolderArray(const ordered_json& j, const char* key)
{
	std::vector<RegexBookmarkFolder> out;
	auto it = j.find(key);
	if (it == j.end() || !it->is_array()) return out;
	for (const auto& f : *it) {
		if (f.is_string()) {
			out.push_back({f.get<std::string>(), false});
		} else if (f.is_object()) {
			auto n = f.find("name");
			if (n == f.end() || !n->is_string()) continue;
			auto c = f.find("collapsed");
			out.push_back({n->get<std::string>(), c != f.end() && c->is_boolean() && c->get<bool>()});
		}
	}
	return out;
}

ordered_json NumberJson(double d)
{
	// JSON.stringify prints an integral double without a fraction; nlohmann
	// would print "16.0". Every value here went through Math.trunc.
	if (std::fabs(d) < 1e15) return (long long)d;
	return d;
}

ordered_json ValueJson(const AlgoValue& v)
{
	ordered_json o = ordered_json::object();
	if (v.min) o["min"] = NumberJson(*v.min);
	if (v.max) o["max"] = NumberJson(*v.max);
	if (RegexFrag::HasChoice(v)) o["choice"] = v.choice;
	return o;
}

ordered_json ValuesJson(const RegexValueList& m)
{
	ordered_json o = ordered_json::object();
	for (const auto& kv : m) o[kv.first] = ValueJson(kv.second);
	return o;
}

ordered_json FoldersJson(const std::vector<RegexBookmarkFolder>& list)
{
	ordered_json a = ordered_json::array();
	for (const RegexBookmarkFolder& f : list) {
		ordered_json o;
		o["name"] = f.name;
		o["collapsed"] = f.collapsed;
		a.push_back(std::move(o));
	}
	return a;
}

// sections.ts unionKeys
std::vector<std::string> UnionKeys(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
	std::vector<std::string> out;
	for (const std::vector<std::string>* v : {&a, &b})
		for (const std::string& k : *v)
			if (std::find(out.begin(), out.end(), k) == out.end()) out.push_back(k);
	return out;
}

} // namespace

const AlgoValue* RegexValueFind(const RegexValueList& m, const std::string& id)
{
	for (const auto& kv : m)
		if (kv.first == id) return &kv.second;
	return nullptr;
}

void RegexValueSet(RegexValueList& m, const std::string& id, const AlgoValue& v)
{
	for (auto& kv : m)
		if (kv.first == id) {
			kv.second = v;
			return;
		}
	m.emplace_back(id, v);
}

std::wstring RegexStatePath(const std::wstring& exeDir)
{
	return exeDir + L"PobTools\\regex_ui.json";
}

std::wstring RegexStateBackupPath(const std::wstring& exeDir, int schema)
{
	return RegexStatePath(exeDir) + L".bak-s" + std::to_wstring(schema < 1 ? 1 : schema);
}

int RegexStateSchemaOf(const std::string& text)
{
	try {
		const ordered_json doc = ordered_json::parse(text);
		if (!doc.is_object()) return 0;
		auto it = doc.find("schema");
		if (it == doc.end() || !it->is_number_integer()) return 0;
		const long long n = it->get<long long>();
		return n < 0 ? 0 : (n > 1000000 ? 1000000 : (int)n);
	} catch (const std::exception&) {
		return 0;
	}
}

RegexPagePicks& RegexUiState::PicksFor(const std::string& pageId)
{
	for (RegexPagePicks& p : current)
		if (p.page == pageId) return p;
	current.push_back(RegexPagePicks{pageId, {}, {}, {}});
	return current.back();
}

RegexValueList& RegexUiState::NumericFor(const std::string& key)
{
	for (auto& kv : numeric)
		if (kv.first == key) return kv.second;
	numeric.emplace_back(key, RegexValueList{});
	return numeric.back().second;
}

const RegexValueList* RegexUiState::NumericOf(const std::string& key) const
{
	for (const auto& kv : numeric)
		if (kv.first == key) return &kv.second;
	return nullptr;
}

std::vector<RegexBookmarkFolder>& RegexUiState::Folders(const std::string& g)
{
	return folders[g == "poe2" ? 1 : 0];
}

const std::vector<RegexBookmarkFolder>& RegexUiState::Folders(const std::string& g) const
{
	return folders[g == "poe2" ? 1 : 0];
}

bool RegexMigrateSections(RegexUiState& s)
{
	bool changed = false;
	// current: a numeric page's record moves into its host's `num`
	std::vector<RegexPagePicks> kept;
	std::vector<std::pair<std::string, std::vector<std::string>>> moved;
	for (RegexPagePicks& p : s.current) {
		const std::string host = RegexAlgo::SectionHostOf(p.page);
		if (!host.empty()) {
			moved.emplace_back(host, p.keys);
			changed = true;
		} else {
			kept.push_back(std::move(p));
		}
	}
	s.current = std::move(kept);
	for (const auto& m : moved) {
		if (m.second.empty()) continue;
		RegexPagePicks& slot = s.PicksFor(m.first);
		slot.num = UnionKeys(slot.num, m.second);
	}
	// values: numeric[section] merges into numeric[host] (section wins)
	for (const char* sec : {"map_numeric", "waystone_numeric"}) {
		const std::string host = RegexAlgo::SectionHostOf(sec);
		size_t at = s.numeric.size();
		for (size_t i = 0; i < s.numeric.size(); i++)
			if (s.numeric[i].first == sec) at = i;
		if (at == s.numeric.size()) continue;
		const RegexValueList m = s.numeric[at].second;
		RegexValueList& dst = s.NumericFor(host);   // appended when absent, as JS
		for (const auto& kv : m) RegexValueSet(dst, kv.first, kv.second);
		for (size_t i = 0; i < s.numeric.size(); i++)
			if (s.numeric[i].first == sec) {
				s.numeric.erase(s.numeric.begin() + i);
				break;
			}
		changed = true;
	}
	const std::string pageHost = RegexAlgo::SectionHostOf(s.page);
	if (!pageHost.empty()) {
		s.page = pageHost;
		changed = true;
	}
	// bookmarks: a numeric page's bookmark becomes a host bookmark with the
	// modifiers unticked and the section = what it had
	for (RegexBookmark& b : s.bookmarks) {
		const std::string host = RegexAlgo::SectionHostOf(b.page);
		if (host.empty()) continue;
		b.page = host;
		b.num = b.keys;
		b.keys.clear();
		b.alt.clear();
		changed = true;
	}
	std::vector<std::string> col;
	for (const std::string& id : s.collapsed) {
		const std::string h = RegexAlgo::SectionHostOf(id);
		const std::string x = h.empty() ? id : h;
		if (std::find(col.begin(), col.end(), x) == col.end()) col.push_back(x);
	}
	s.collapsed = std::move(col);
	return changed;
}

bool RegexUiState::Parse(const std::string& text)
{
	const int schemaSeen = RegexStateSchemaOf(text);
	*this = RegexUiState{};
	try {
		const ordered_json doc = ordered_json::parse(text);
		if (!doc.is_object()) throw std::runtime_error("not an object");
		// Empty is a real value here: it means "no choice recorded yet", and the
		// panel answers it with the launcher's game. OneOf() would turn that into
		// a wrong answer that looks like a decision.
		game = GameOf(Str(doc, "game", ""));
		page = Str(doc, "page", "");
		mode = OneOf(Str(doc, "mode", "any"), "any", "all", "none");
		lang = OneOf(Str(doc, "lang", "zh"), "zh", "en", nullptr);
		auto bi = doc.find("bilingual");
		if (bi != doc.end()) {
			if (!bi->is_boolean()) throw std::runtime_error("bilingual is not a boolean");
			bilingual = bi->get<bool>();
		}

		auto cur = doc.find("current");
		if (cur != doc.end() && cur->is_array()) {
			for (const auto& p : *cur) {
				if (!p.is_object()) continue;
				RegexPagePicks rec;
				rec.page = Str(p, "page", "");
				if (rec.page.empty()) continue;
				rec.keys = StringArray(p, "keys");
				rec.alt = StringArray(p, "alt");
				rec.num = StringArray(p, "num");
				current.push_back(std::move(rec));
			}
		}

		auto bm = doc.find("bookmarks");
		if (bm != doc.end() && bm->is_array()) {
			for (const auto& b : *bm) {
				if (!b.is_object()) continue;
				RegexBookmark rec;
				rec.name = Str(b, "name", "");
				rec.page = Str(b, "page", "");
				// Absent in files written before the list was split by game. Left
				// empty for the panel to fill in from the page id, which is the only
				// place that knows which catalogue a page came from.
				rec.game = GameOf(Str(b, "game", ""));
				rec.mode = OneOf(Str(b, "mode", "any"), "any", "all", "none");
				rec.lang = OneOf(Str(b, "lang", "zh"), "zh", "en", nullptr);
				rec.keys = StringArray(b, "keys");
				rec.alt = StringArray(b, "alt");
				auto nm = b.find("numeric");
				if (nm != b.end() && nm->is_object()) rec.numeric = NumericMap(*nm);
				rec.num = StringArray(b, "num");
				auto hk = b.find("hotkey");
				if (hk != b.end() && hk->is_string()) rec.hotkey = RegexAlgo::JsTrim(hk->get<std::string>());
				auto fd = b.find("folder");
				if (fd != b.end() && fd->is_string() && !RegexAlgo::JsTrim(fd->get<std::string>()).empty())
					rec.folder = fd->get<std::string>();
				// A nameless or empty bookmark is not something the UI can offer,
				// and keeping it would put a blank row in the list forever.
				if (rec.name.empty() || rec.page.empty() || (rec.keys.empty() && rec.num.empty())) continue;
				bookmarks.push_back(std::move(rec));
			}
		}

		auto nu = doc.find("numeric");
		if (nu != doc.end() && nu->is_object()) {
			for (auto it = nu->begin(); it != nu->end(); ++it) {
				RegexValueList& dst = NumericFor(it.key());
				dst = NumericMap(it.value());
			}
		}
		custom = StringArray(doc, "custom");
		excludes = StringArray(doc, "excludes");
		outScope = OneOf(Str(doc, "outScope", "combined"), "combined", "page", nullptr);
		collapsed = StringArray(doc, "collapsed");
		auto fo = doc.find("folders");
		if (fo != doc.end() && fo->is_object()) {
			folders[0] = FolderArray(*fo, "poe1");
			folders[1] = FolderArray(*fo, "poe2");
		}
		for (const std::string& g : StringArray(doc, "uncatCollapsed")) {
			const std::string x = GameOf(g);
			if (!x.empty() && std::find(uncatCollapsed.begin(), uncatCollapsed.end(), x) == uncatCollapsed.end())
				uncatCollapsed.push_back(x);
		}
		panelView = OneOf(Str(doc, "panelView", "page"), "page", "combined", nullptr);
		RegexMigrateSections(*this);
		RegexFolders::Normalize(*this);
	} catch (const std::exception&) {
		// A corrupt file must not take the tool down with it, nor leave half-read
		// rubbish behind. The defaults are a perfectly usable starting state.
		*this = RegexUiState{};
		loadedSchema = schemaSeen;
		return false;
	}
	loadedSchema = schemaSeen;
	return true;
}

std::string RegexUiState::Serialize() const
{
	ordered_json doc;
	doc["schema"] = kRegexStateSchema;
	doc["game"] = game;
	doc["page"] = page;
	doc["mode"] = mode;
	doc["lang"] = lang;
	doc["bilingual"] = bilingual;
	ordered_json cur = ordered_json::array();
	for (const RegexPagePicks& p : current) {
		if (p.keys.empty() && p.num.empty()) continue;   // nothing ticked: nothing worth writing
		ordered_json o;
		o["page"] = p.page;
		o["keys"] = p.keys;
		o["alt"] = p.alt;
		if (!p.num.empty()) o["num"] = p.num;
		cur.push_back(std::move(o));
	}
	doc["current"] = std::move(cur);
	ordered_json bm = ordered_json::array();
	for (const RegexBookmark& b : bookmarks) {
		ordered_json o;
		o["name"] = b.name;
		o["page"] = b.page;
		o["game"] = b.game;
		o["mode"] = b.mode;
		o["lang"] = b.lang;
		o["keys"] = b.keys;
		o["alt"] = b.alt;
		if (!b.numeric.empty()) o["numeric"] = ValuesJson(b.numeric);
		if (!b.num.empty()) o["num"] = b.num;
		if (!b.hotkey.empty()) o["hotkey"] = b.hotkey;
		if (!b.folder.empty()) o["folder"] = b.folder;
		bm.push_back(std::move(o));
	}
	doc["bookmarks"] = std::move(bm);
	ordered_json nu = ordered_json::object();
	for (const auto& kv : numeric)
		if (!kv.second.empty()) nu[kv.first] = ValuesJson(kv.second);
	doc["numeric"] = std::move(nu);
	doc["custom"] = custom;
	doc["excludes"] = excludes;
	doc["outScope"] = outScope;
	doc["collapsed"] = collapsed;
	ordered_json fo;
	fo["poe1"] = FoldersJson(folders[0]);
	fo["poe2"] = FoldersJson(folders[1]);
	doc["folders"] = std::move(fo);
	doc["uncatCollapsed"] = uncatCollapsed;
	if (panelView != "page") doc["panelView"] = panelView;
	return doc.dump(1, '\t');
}

bool RegexUiState::Load(const std::wstring& exeDir)
{
	std::string body;
	loadedFile = false;
	if (!ReadAll(RegexStatePath(exeDir), body)) {
		*this = RegexUiState{};
		return false;
	}
	const bool ok = Parse(body);
	loadedFile = true;   // even unparsable: there IS a file
	return ok;
}

bool RegexUiState::Save(const std::wstring& exeDir)
{
	// A file a newer build wrote holds fields this one would drop. Not ours to
	// rewrite: the panel shows that changes in this session are not kept.
	if (SaveBlocked()) return false;
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);   // ok if it exists

	const std::wstring dst = RegexStatePath(exeDir);
	// The first write over an older schema keeps the original next to it. Never
	// overwritten: the first backup is the one that holds the pre-upgrade file.
	if (loadedFile && loadedSchema < kRegexStateSchema &&
	    GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) {
		const std::wstring bak = RegexStateBackupPath(exeDir, loadedSchema);
		if (GetFileAttributesW(bak.c_str()) == INVALID_FILE_ATTRIBUTES &&
		    !CopyFileW(dst.c_str(), bak.c_str(), TRUE))
			return false;   // no backup, no overwrite
	}
	const std::string out = Serialize();

	// Written beside the real file and moved into place. Bookmarks are the only
	// thing in this tool the player cannot get back from anywhere else, and a
	// write interrupted halfway would leave a truncated JSON that the loader
	// above would throw away wholesale.
	const std::wstring tmp = dst + L".tmp";
	HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD wrote = 0;
	const bool ok = WriteFile(f, out.data(), (DWORD)out.size(), &wrote, nullptr) &&
	                wrote == out.size();
	CloseHandle(f);
	if (!ok) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	if (!MoveFileExW(tmp.c_str(), dst.c_str(),
	                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	loadedFile = true;
	loadedSchema = kRegexStateSchema;
	return true;
}

int RegexResolveKeys(const std::vector<std::string>& keys,
                     const std::vector<std::string>& alt,
                     const std::vector<std::string>& entryKeys,
                     const std::vector<std::string>& entryAlt,
                     std::vector<char>& picked)
{
	picked.assign(entryKeys.size(), 0);
	int missed = 0;
	for (size_t k = 0; k < keys.size(); k++) {
		int hit = -1;
		// English first: it is the language the data is keyed by, so it survives a
		// revision of our own Chinese wording.
		if (!keys[k].empty())
			for (size_t i = 0; i < entryKeys.size() && hit < 0; i++)
				if (entryKeys[i] == keys[k]) hit = (int)i;
		// Chinese second, for the rarer case where GGG reworded the English and
		// left the translation alone.
		if (hit < 0 && k < alt.size() && !alt[k].empty())
			for (size_t i = 0; i < entryAlt.size() && hit < 0; i++)
				if (entryAlt[i] == alt[k]) hit = (int)i;
		if (hit >= 0) picked[hit] = 1;
		else missed++;
	}
	return missed;
}
