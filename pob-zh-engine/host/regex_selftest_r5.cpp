// --regex-selftest, R5 / R6 part: regex_ui.json schema 5 (regex_state), bookmark
// folders (regex_folders) and the bookmark-as-whole-page snapshot (regex_embed).
// Ports exile-appraiser regex/test/state.test.ts and folders.test.ts, adds the
// file-level checks that only exist here (schema 1 -> 5 migration with the
// .bak-s1 backup, a newer schema never overwritten), snapshots on our own Data
// (numeric section, vendor page, merge state), and replays regex_r5_golden.inc,
// which tools/regex_port/gen-golden-r5.ts produced by running state.ts /
// folders.ts / embed.ts over our Data files, to byte-identical files.
#include "regex_algo_pages.h"
#include "regex_data.h"
#include "regex_embed.h"
#include "regex_folders.h"
#include "regex_frag.h"
#include "regex_gen.h"
#include "regex_state.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <json.hpp>

namespace {

#include "regex_r5_golden.inc"

using namespace RegexAlgo;
using json = nlohmann::json;
using RegexFrag::AlgoValue;
namespace F = RegexFolders;

void (*g_check)(bool, const std::string&) = nullptr;
void (*g_line)(const std::string&) = nullptr;

void check(bool ok, const std::string& what) { g_check(ok, what); }
void line(const std::string& s) { g_line(s); }
std::string Num(long long n) { return std::to_string(n); }

// One game's pages as the panel builds them: corpus pages in data order, then
// the algorithmic pages (store.ts:116).
struct Game {
	std::vector<AlgoPage> algo;
	std::vector<PageRef> pages;
	const PageRef* Find(const std::string& id) const
	{
		for (const PageRef& p : pages)
			if (p.Id() == id) return &p;
		return nullptr;
	}
};

// The selftest LCG (regex_selftest.cpp Rng / rng.ts).
struct Rng {
	uint32_t s;
	explicit Rng(uint32_t seed) : s(seed) {}
	uint32_t next() { s = s * 1664525u + 1013904223u; return s >> 8; }
	int below(int n) { return n <= 0 ? 0 : (int)(next() % (uint32_t)n); }
};

RegexBookmark Bm(const std::string& name, const std::string& game = "poe1", const std::string& folder = "")
{
	RegexBookmark b;
	b.name = name;
	b.page = "map_mods";
	b.game = game;
	b.keys = {"k"};
	b.folder = folder;
	return b;
}

std::vector<std::string> Names(const RegexUiState& s, const std::string& g)
{
	std::vector<std::string> out;
	for (const RegexBookmark& b : s.bookmarks)
		if (b.game == g) out.push_back(b.folder + "/" + b.name);
	return out;
}

// folders.test.ts assertSorted: same-game bookmarks in folder order, uncategorised last.
bool Sorted(const RegexUiState& s, std::string* why = nullptr)
{
	for (const char* g : {"poe1", "poe2"}) {
		const auto& list = s.Folders(g);
		int last = -1;
		for (const RegexBookmark& b : s.bookmarks) {
			if (b.game != g) continue;
			int r = (int)list.size();
			if (!b.folder.empty()) {
				r = -1;
				for (int i = 0; i < (int)list.size(); i++)
					if (list[i].name == b.folder) r = i;
				if (r < 0) {
					if (why) *why = std::string(g) + " bookmark " + b.name + " names a missing folder " + b.folder;
					return false;
				}
			}
			if (r < last) {
				if (why) *why = std::string(g) + " out of order at " + b.name;
				return false;
			}
			last = r;
		}
	}
	return true;
}

int IndexOfName(const RegexUiState& s, const std::string& name)
{
	for (int i = 0; i < (int)s.bookmarks.size(); i++)
		if (s.bookmarks[i].name == name) return i;
	return -1;
}

// ---- state.test.ts + the new fields ---------------------------------------------

void StateTests()
{
	line("[R5-state] regex_ui.json schema 5 (state.test.ts + the new fields)");
	RegexUiState d;
	check(d.mode == "any" && d.bookmarks.empty() && d.outScope == "combined" && d.panelView == "page" &&
	      d.folders[0].empty() && d.folders[1].empty(), "defaults are usable");
	{
		const json doc = json::parse(d.Serialize());
		check(doc["schema"] == 5 && doc["folders"] == json::parse(R"({"poe1":[],"poe2":[]})") &&
		      doc["uncatCollapsed"] == json::array() && !doc.contains("panelView"),
		      "writes schema 5, folders, uncatCollapsed; panelView only when not the default");
	}

	RegexUiState a;
	a.game = "poe1";
	a.page = "map_mods";
	a.mode = "none";
	RegexPagePicks& p = a.PicksFor("map_mods");
	p.keys = {"Monsters cannot be Leeched from"};
	p.alt = {u8"怪物不能被吸取"};
	p.num = {"map_tier", "item_quantity"};
	const RegexPagePicks saved = p;   // PicksFor below may reallocate `current`
	a.PicksFor("logbook_mods");      // nothing ticked: not written
	RegexBookmark bm;
	bm.name = u8"危險詞綴";
	bm.page = "map_mods";
	bm.game = "poe1";
	bm.mode = "none";
	bm.keys = {"Monsters cannot be Leeched from", "Players are Cursed with Enfeeble"};
	bm.alt = {u8"怪物不能被吸取", u8"玩家被虛弱詛咒"};
	bm.num = {"map_tier"};
	AlgoValue v16;
	v16.min = 16;
	RegexValueSet(bm.numeric, "map_tier", v16);
	bm.hotkey = "Ctrl + Shift + 1";
	a.bookmarks.push_back(bm);
	AlgoValue vr;
	vr.choice = "rare";
	vr.hasChoice = true;
	RegexValueSet(a.NumericFor("map_mods"), "item_rarity_class", vr);
	AlgoValue empty;
	empty.hasChoice = true;   // choice: '' is a value, not "no choice"
	RegexValueSet(a.NumericFor("vendor_items"), "x", empty);
	a.custom = {u8"6 連結", "a.b"};
	a.excludes = {u8"反射"};
	a.outScope = "page";
	a.collapsed = {"map_mods"};
	a.panelView = "combined";
	RegexUiState b;
	check(b.Parse(a.Serialize()), "parses its own output");
	check(b.page == a.page && b.mode == a.mode && b.game == a.game, "page, mode, game survive");
	check(b.current.size() == 1 && b.current[0].keys == saved.keys && b.current[0].alt == saved.alt && b.current[0].num == saved.num,
	      "ticks survive, the numeric section's under its host (num); an empty page is not written");
	check(b.bookmarks.size() == 1 && b.bookmarks[0].name == bm.name && b.bookmarks[0].keys == bm.keys &&
	      b.bookmarks[0].num == bm.num && b.bookmarks[0].numeric.size() == 1 &&
	      b.bookmarks[0].numeric[0].second.min == 16.0 && b.bookmarks[0].hotkey == bm.hotkey,
	      "the bookmark survives whole: keys, num, numeric, and exile-appraiser's hotkey kept verbatim");
	const RegexValueList* nm = b.NumericOf("map_mods");
	const RegexValueList* nv = b.NumericOf("vendor_items");
	check(nm && nm->size() == 1 && (*nm)[0].second.choice == "rare" && nv && nv->size() == 1 &&
	      RegexFrag::HasChoice((*nv)[0].second) && (*nv)[0].second.choice.empty(),
	      "values survive, including an explicitly empty choice");
	check(b.custom == a.custom && b.excludes == a.excludes && b.outScope == "page" && b.collapsed == a.collapsed &&
	      b.panelView == "combined", "custom text, excludes, output scope, folded sections and the view survive");
	check(b.Serialize() == a.Serialize(), "and a second round trip is byte-identical");
	b.bookmarks.clear();
	RegexUiState c;
	c.Parse(b.Serialize());
	check(c.bookmarks.empty(), "a deleted bookmark stays deleted");

	RegexUiState e;
	e.bookmarks.push_back(bm);
	check(!e.Parse("{ this is not json") && e.bookmarks.empty() && e.current.empty(),
	      "a corrupt file is refused and leaves nothing half-read behind");
	e.Parse(R"({"page":"x","mode":"someday","bookmarks":[]})");
	check(e.mode == "any", "a mode this build does not know falls back to one it does");
	check(e.Parse(R"({"page":"map_mods","mode":"any","bookmarks":[{"name":"x","page":"waystone_mods","keys":["a"],"alt":["b"]}]})") &&
	      e.game.empty() && e.bookmarks.size() == 1 && e.bookmarks[0].game.empty(),
	      "a file from before the game split loads with the game left empty (not guessed)");
	e.Parse(R"({"game":"poe3","page":"x","bookmarks":[]})");
	check(e.game.empty(), "a game this build does not know is dropped, not carried");
	e.Parse(R"({"bookmarks":[{"name":"","page":"p","keys":["a"]},{"name":"n","page":"","keys":["a"]},{"name":"n","page":"p","keys":[]},{"name":"s","page":"p","keys":[],"num":["t"]}]})");
	check(e.bookmarks.size() == 1 && e.bookmarks[0].name == "s",
	      "nameless / pageless / empty bookmarks are not offered; a numeric-only one is");
	check(!e.Parse(R"({"bilingual":"yes"})"), "a field of the wrong type refuses the file (state.ts str / bilingual)");

	// schema <= 2: the numeric pages move onto their host page
	e.Parse(R"({"schema":2,"page":"map_numeric","current":[{"page":"map_numeric","keys":["map_tier"]}],)"
	        R"("numeric":{"map_numeric":{"map_tier":{"min":16}}},"collapsed":["map_numeric"],)"
	        R"("bookmarks":[{"name":"n","page":"map_numeric","game":"poe1","keys":["map_tier"],"alt":[],"numeric":{"map_tier":{"min":14}}}]})");
	check(e.page == "map_mods" && e.current.size() == 1 && e.current[0].page == "map_mods" &&
	      e.current[0].num == std::vector<std::string>{"map_tier"} && e.NumericOf("map_mods") && !e.NumericOf("map_numeric") &&
	      e.collapsed == std::vector<std::string>{"map_mods"} && e.bookmarks.size() == 1 &&
	      e.bookmarks[0].page == "map_mods" && e.bookmarks[0].keys.empty() && e.bookmarks[0].num == std::vector<std::string>{"map_tier"},
	      "schema 2 numeric pages migrate onto the host page (ticks, values, page, folded, bookmark)");
	RegexUiState again = e;
	check(!RegexMigrateSections(again) && again.Serialize() == e.Serialize(), "and migrating twice changes nothing");
	check(RegexStateSchemaOf(R"({"schema":4})") == 4 && RegexStateSchemaOf("{}") == 0 && RegexStateSchemaOf("x") == 0,
	      "schema detection");
}

// ---- the file: schema 1 -> 5 with a backup, a newer schema left alone ----------------

std::wstring Scratch()
{
	wchar_t tmp[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, tmp)) return L"";
	return std::wstring(tmp) + L"pobtools_regex_selftest_r5\\";
}

bool WriteText(const std::wstring& path, const std::string& text)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &w, nullptr) && w == text.size();
	CloseHandle(h);
	return ok;
}

std::string ReadText(const std::wstring& path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return "<missing>";
	std::string out;
	char buf[65536];
	DWORD r = 0;
	while (ReadFile(h, buf, sizeof buf, &r, nullptr) && r > 0) out.append(buf, r);
	CloseHandle(h);
	return out;
}

void CleanScratch(const std::wstring& dir)
{
	for (const wchar_t* f : {L"regex_ui.json", L"regex_ui.json.tmp", L"regex_ui.json.bak-s1", L"regex_ui.json.bak-s6"})
		DeleteFileW((dir + L"PobTools\\" + f).c_str());
	RemoveDirectoryW((dir + L"PobTools").c_str());
	RemoveDirectoryW(dir.c_str());
}

// A schema 1 file as the previous PobTools wrote it (Save, dump(1, '\t')).
const char* const kSchema1 =
	"{\n\t\"schema\": 1,\n\t\"game\": \"poe2\",\n\t\"page\": \"waystone_mods\",\n\t\"mode\": \"all\",\n"
	"\t\"lang\": \"en\",\n\t\"bilingual\": false,\n\t\"current\": [\n\t\t{\n\t\t\t\"page\": \"map_mods\",\n"
	"\t\t\t\"keys\": [\n\t\t\t\t\"Area is haunted\"\n\t\t\t],\n\t\t\t\"alt\": [\n\t\t\t\t\"\xe5\x8d\x80\xe5\x9f\x9f\"\n\t\t\t]\n\t\t}\n\t],\n"
	"\t\"bookmarks\": [\n\t\t{\n\t\t\t\"name\": \"A\",\n\t\t\t\"page\": \"map_mods\",\n\t\t\t\"game\": \"poe1\",\n"
	"\t\t\t\"mode\": \"any\",\n\t\t\t\"lang\": \"zh\",\n\t\t\t\"keys\": [\n\t\t\t\t\"k1\"\n\t\t\t],\n\t\t\t\"alt\": [\n\t\t\t\t\"z1\"\n\t\t\t]\n\t\t},\n"
	"\t\t{\n\t\t\t\"name\": \"B\",\n\t\t\t\"page\": \"waystone_mods\",\n\t\t\t\"game\": \"\",\n"
	"\t\t\t\"mode\": \"none\",\n\t\t\t\"lang\": \"en\",\n\t\t\t\"keys\": [\n\t\t\t\t\"k2\"\n\t\t\t],\n\t\t\t\"alt\": [\n\t\t\t\t\"z2\"\n\t\t\t]\n\t\t}\n\t]\n}";

void FileTests()
{
	line("[R5-file] schema 1 -> 5 on disk: backup once, never over a newer schema");
	const std::wstring dir = Scratch();
	if (dir.empty()) {
		check(false, "could not make a scratch directory");
		return;
	}
	CleanScratch(dir);
	CreateDirectoryW(dir.c_str(), nullptr);
	CreateDirectoryW((dir + L"PobTools").c_str(), nullptr);
	const std::wstring path = RegexStatePath(dir);
	const std::wstring bak1 = RegexStateBackupPath(dir, 1);

	check(WriteText(path, kSchema1), "write a schema 1 file");
	RegexUiState s;
	check(s.Load(dir) && s.loadedSchema == 1, "a schema 1 file loads (loadedSchema 1)");
	check(s.game == "poe2" && s.page == "waystone_mods" && s.mode == "all" && s.lang == "en" && !s.bilingual &&
	      s.current.size() == 1 && s.current[0].keys == std::vector<std::string>{"Area is haunted"} &&
	      s.bookmarks.size() == 2 && s.bookmarks[0].name == "A" && s.bookmarks[1].game.empty(),
	      "every schema 1 field comes back (game, page, mode, lang, bilingual, ticks, bookmarks, an empty game kept empty)");
	check(s.custom.empty() && s.outScope == "combined" && s.folders[0].empty() && s.uncatCollapsed.empty() &&
	      s.bookmarks[0].folder.empty(), "the fields schema 1 did not have take their defaults (all bookmarks uncategorised)");
	check(GetFileAttributesW(bak1.c_str()) == INVALID_FILE_ATTRIBUTES, "loading alone writes nothing (no backup yet)");
	check(s.Save(dir), "the first save works");
	check(ReadText(bak1) == kSchema1, "and kept the schema 1 file, byte for byte, as regex_ui.json.bak-s1");
	const std::string after = ReadText(path);
	check(RegexStateSchemaOf(after) == 5, "the file is now schema 5");
	RegexUiState t;
	check(t.Load(dir) && t.loadedSchema == 5 && t.Serialize() == s.Serialize(), "and reads back to the same state");
	t.custom.push_back("x");
	check(t.Save(dir) && ReadText(bak1) == kSchema1, "a later save leaves the backup alone");

	// An older build writes schema 1 again; the next upgrade must not replace
	// the first backup (that one holds the original).
	const std::string second = std::string(kSchema1).replace(std::string(kSchema1).find("\"A\""), 3, "\"A2\"");
	check(WriteText(path, second), "an older build rewrites schema 1");
	RegexUiState u;
	u.Load(dir);
	check(u.Save(dir) && ReadText(bak1) == kSchema1, "the upgrade does not overwrite an existing .bak-s1");
	check(u.bookmarks.size() == 2 && u.bookmarks[0].name == "A2", "and the file itself was migrated from the rewrite");

	// A file from a newer schema: read, never written over.
	const std::string newer = R"({"schema":6,"game":"poe1","page":"map_mods","future":{"x":1},"bookmarks":[{"name":"N","page":"map_mods","game":"poe1","keys":["k"],"alt":[],"extra":true}]})";
	check(WriteText(path, newer), "write a schema 6 file");
	RegexUiState n;
	check(n.Load(dir) && n.SaveBlocked() && n.bookmarks.size() == 1 && n.bookmarks[0].name == "N",
	      "a newer schema is read for display (its bookmarks show)");
	n.bookmarks.clear();
	n.custom.push_back("y");
	check(!n.Save(dir), "but Save refuses");
	check(ReadText(path) == newer, "and the file is byte-for-byte untouched");
	check(GetFileAttributesW(RegexStateBackupPath(dir, 6).c_str()) == INVALID_FILE_ATTRIBUTES &&
	      GetFileAttributesW((path + L".tmp").c_str()) == INVALID_FILE_ATTRIBUTES, "no backup and no temp file either");

	check(WriteText(path, "{ not json"), "write a corrupt file");
	RegexUiState k;
	check(!k.Load(dir) && k.bookmarks.empty() && !k.SaveBlocked(), "a corrupt file is refused with the defaults");
	CleanScratch(dir);
}

// ---- folders.test.ts ------------------------------------------------------------

RegexUiState Sample()
{
	RegexUiState s;
	s.bookmarks = {Bm("p1a"), Bm("p2a", "poe2"), Bm("p1b"), Bm("p1c"), Bm("p2b", "poe2"), Bm("orphan", "")};
	return s;
}

void FolderTests()
{
	line("[R6-folders] bookmark folders (folders.test.ts)");
	{
		RegexUiState s = Sample();
		check(F::Add(s, "poe1", u8"  地圖   常用 ") == F::Result::Ok && s.folders[0].size() == 1 &&
		      s.folders[0][0].name == u8"地圖 常用" && !s.folders[0][0].collapsed, "add: the name is normalised");
		check(F::Add(s, "poe1", u8"地圖 常用") == F::Result::Duplicate && F::Add(s, "poe1", "   ") == F::Result::Empty,
		      "add: duplicate and blank are refused");
		check(F::Add(s, "poe2", u8"地圖 常用") == F::Result::Ok, "add: the other game may have the same name");
		std::string longName;
		for (int i = 0; i < 100; i++) longName += "x";
		check(F::NormalizeName(longName).size() == 40 && F::NormalizeName(u8"　a  b\t") == "a b",
		      "names are cut to 40 code points; JS whitespace collapses");
	}
	{
		RegexUiState s = Sample();
		F::Add(s, "poe1", "A");
		F::Add(s, "poe1", "B");
		F::Add(s, "poe2", "A");
		F::MoveBookmark(s, 0, "A");
		F::MoveBookmark(s, IndexOfName(s, "p2a"), "A");
		check(F::Rename(s, "poe1", "A", "B") == F::Result::Duplicate && F::Rename(s, "poe1", "Z", "C") == F::Result::Missing &&
		      F::Rename(s, "poe1", "A", " A ") == F::Result::Ok && F::Rename(s, "poe1", "A", "C") == F::Result::Ok,
		      "rename: duplicate / missing / same name / ok");
		check(s.folders[0][0].name == "C" && s.folders[0][1].name == "B" && s.bookmarks[IndexOfName(s, "p1a")].folder == "C" &&
		      s.bookmarks[IndexOfName(s, "p2a")].folder == "A" && Sorted(s),
		      "rename: bookmarks follow; the other game's same-named folder does not");
	}
	{
		RegexUiState s = Sample();
		F::Add(s, "poe1", "A");
		F::Add(s, "poe1", "B");
		F::MoveBookmark(s, IndexOfName(s, "p1c"), "A");
		F::MoveBookmark(s, IndexOfName(s, "p1b"), "B");
		check(Names(s, "poe1") == std::vector<std::string>{"A/p1c", "B/p1b", "/p1a"}, "moves put bookmarks in folder order");
		const size_t before = s.bookmarks.size();
		check(F::Delete(s, "poe1", "A") == 1 && F::Delete(s, "poe1", "nope") == -1 && s.bookmarks.size() == before,
		      "delete: returns how many went back; no bookmark is deleted");
		check(s.folders[0].size() == 1 && Names(s, "poe1") == std::vector<std::string>{"B/p1b", "/p1c", "/p1a"} && Sorted(s),
		      "delete: they move to uncategorised, sorted last");
	}
	{
		RegexUiState s = Sample();
		for (const char* f : {"A", "B", "C"}) F::Add(s, "poe1", f);
		F::MoveBookmark(s, IndexOfName(s, "p1a"), "C");
		F::MoveBookmark(s, IndexOfName(s, "p1b"), "A");
		F::MoveBookmark(s, IndexOfName(s, "p1c"), "B");
		check(Names(s, "poe1") == std::vector<std::string>{"A/p1b", "B/p1c", "C/p1a"}, "three folders in order");
		check(F::MoveTo(s, "poe1", "C", 0) && Names(s, "poe1") == std::vector<std::string>{"C/p1a", "A/p1b", "B/p1c"},
		      "moving a folder moves its bookmarks with it");
		check(!F::MoveBy(s, "poe1", "C", -1) && F::MoveBy(s, "poe1", "C", 1) && s.folders[0][0].name == "A" &&
		      s.folders[0][1].name == "C", "up at the top does nothing; down works");
		check(F::MoveTo(s, "poe1", "A", 99) && s.folders[0][2].name == "A" &&
		      Names(s, "poe1") == std::vector<std::string>{"C/p1a", "B/p1c", "A/p1b"} && Sorted(s), "a target past the end is clamped");
	}
	{
		RegexUiState s = Sample();
		F::Add(s, "poe1", "A");
		check(!F::IsCollapsed(s, "poe1", "A") && F::SetCollapsed(s, "poe1", "A", true) && !F::SetCollapsed(s, "poe1", "A", true) &&
		      F::SetCollapsed(s, "poe1", "", true) && F::IsCollapsed(s, "poe1", "") && !F::IsCollapsed(s, "poe2", "") &&
		      !F::SetCollapsed(s, "poe1", "nope", true) && s.uncatCollapsed == std::vector<std::string>{"poe1"},
		      "folding: per folder and per game's uncategorised");
		F::SetCollapsed(s, "poe1", "", false);
		check(s.uncatCollapsed.empty(), "unfolding uncategorised");
	}
	{
		RegexUiState s = Sample();
		F::Add(s, "poe1", "A");
		int i = F::MoveBookmark(s, IndexOfName(s, "p1c"), "A");
		check(i >= 0 && s.bookmarks[i].name == "p1c", "move returns the new index");
		F::MoveBookmark(s, IndexOfName(s, "p1a"), "A");
		check(Names(s, "poe1") == std::vector<std::string>{"A/p1c", "A/p1a", "/p1b"}, "menu move = end of the folder");
		i = F::MoveBookmark(s, IndexOfName(s, "p1b"), "", IndexOfName(s, "p1c"));
		check(s.bookmarks[i].name == "p1b" && Names(s, "poe1") == std::vector<std::string>{"A/p1b", "A/p1c", "A/p1a"},
		      "drop on a bookmark = in front of it, in its folder");
		const int self = IndexOfName(s, "p1c");
		check(F::MoveBookmark(s, self, "", self) == self, "dropping on itself does nothing");
		const int k2 = F::MoveBookmarkBy(s, IndexOfName(s, "p1a"), -1);
		check(s.bookmarks[k2].name == "p1a" && Names(s, "poe1") == std::vector<std::string>{"A/p1b", "A/p1a", "A/p1c"},
		      "up / down within the folder");
		const int top = IndexOfName(s, "p1b");
		check(F::MoveBookmarkBy(s, top, -1) == top, "up at the top does nothing");
		check(F::MoveBookmark(s, IndexOfName(s, "p2a"), "", IndexOfName(s, "p1a")) == -1 &&
		      F::MoveBookmark(s, IndexOfName(s, "p1a"), "nope") == -1 && F::MoveBookmark(s, IndexOfName(s, "orphan"), "") == -1 &&
		      Sorted(s), "across games / to a missing folder / an orphan: refused");
	}
	{
		RegexUiState s = Sample();
		F::Add(s, "poe1", "A");
		auto slotsOf = [&](const std::string& g) {
			std::vector<int> v;
			for (int i = 0; i < (int)s.bookmarks.size(); i++)
				if (s.bookmarks[i].game == g) v.push_back(i);
			return v;
		};
		const std::vector<int> p2 = slotsOf("poe2"), orphan = slotsOf("");
		F::MoveBookmark(s, IndexOfName(s, "p1c"), "A");
		F::MoveTo(s, "poe1", "A", 0);
		check(slotsOf("poe2") == p2 && slotsOf("") == orphan, "the other game's and the orphans' slots never move");
	}
	{
		RegexUiState s = Sample();
		s.uncatCollapsed = {"poe1"};
		F::Grouped g = F::GroupBookmarks(s, "poe1");
		check(!g.headers && g.groups.size() == 1 && !g.groups[0].collapsed && g.groups[0].items == std::vector<int>{0, 2, 3} &&
		      !F::GroupBookmarks(s, "poe1", true).headers,
		      "no folders: one uncategorised group, no headers, folding ignored (looks as before folders)");
		s.uncatCollapsed.clear();
		F::Add(s, "poe1", "A");
		F::Add(s, "poe1", u8"空");
		F::Add(s, "poe1", "B");
		F::MoveBookmark(s, IndexOfName(s, "p1b"), "B");
		F::SetCollapsed(s, "poe1", "B", true);
		g = F::GroupBookmarks(s, "poe1");
		check(g.headers && g.groups.size() == 4 && g.groups[0].folder == "A" && g.groups[0].items.empty() &&
		      g.groups[2].folder == "B" && g.groups[2].collapsed && g.groups[2].items.size() == 1 &&
		      g.groups[3].folder.empty() && g.groups[3].items.size() == 2,
		      "folders in order, uncategorised last; the manager lists empty folders");
		const F::Grouped q = F::GroupBookmarks(s, "poe1", true);
		check(q.groups.size() == 2 && q.groups[0].folder == "B" && q.groups[1].folder.empty(), "skipEmpty leaves them out");
		F::Delete(s, "poe1", "B");
		check(!F::GroupBookmarks(s, "poe1", true).headers && F::GroupBookmarks(s, "poe1").headers,
		      "only uncategorised left after skipping: no headers there, still headers in the manager");
		for (RegexBookmark& b : s.bookmarks)
			if (b.game == "poe1") b.folder = "A";
		const F::Grouped m = F::GroupBookmarks(s, "poe1");
		check(m.groups.back().folder.empty() && m.groups.back().items.empty() &&
		      F::GroupBookmarks(s, "poe1", true).groups.size() == 1 && F::Counts(s, "poe1")["A"] == 3,
		      "an empty uncategorised still shows in the manager (to drag back into); counts");
	}
	{
		// The other game's edits never touch the current game's list.
		Rng rng(36);
		bool same = true, sorted = true, round = true;
		std::string why;
		for (int r = 0; r < 30; r++) {
			RegexUiState s;
			for (int i = 0; i < 14; i++) {
				RegexBookmark b = Bm("b" + std::to_string(i), rng.below(2) ? "poe1" : "poe2");
				if (!rng.below(3)) b.hotkey = "F" + std::to_string(i + 1);
				s.bookmarks.push_back(b);
			}
			F::Add(s, "poe1", u8"一");
			F::Add(s, "poe1", u8"二");
			for (int i = 0; i < (int)s.bookmarks.size(); i++)
				if (s.bookmarks[i].game == "poe1" && rng.below(2)) F::MoveBookmark(s, i, rng.below(2) ? u8"一" : u8"二");
			auto snap = [&]() {
				json j = json::array();
				for (int i = 0; i < (int)s.bookmarks.size(); i++)
					if (s.bookmarks[i].game == "poe1")
						j.push_back({i, s.bookmarks[i].name, s.bookmarks[i].folder, s.bookmarks[i].hotkey});
				for (const auto& f : s.folders[0]) j.push_back({f.name, f.collapsed});
				j.push_back(F::IsCollapsed(s, "poe1", ""));
				return j.dump();
			};
			const std::string before = snap();
			for (int step = 0; step < 40; step++) {
				std::vector<int> list;
				for (int i = 0; i < (int)s.bookmarks.size(); i++)
					if (s.bookmarks[i].game == "poe2") list.push_back(i);
				const int pick = list.empty() ? -1 : list[rng.below((int)list.size())];
				std::string fname;
				if (!s.folders[1].empty()) fname = s.folders[1][rng.below((int)s.folders[1].size())].name;
				switch (rng.below(9)) {
				case 0: F::Add(s, "poe2", "f" + std::to_string(rng.below(5))); break;
				case 1: if (!fname.empty()) F::Rename(s, "poe2", fname, "r" + std::to_string(rng.below(5))); break;
				case 2: if (!fname.empty()) F::Delete(s, "poe2", fname); break;
				case 3: if (!fname.empty()) F::MoveBy(s, "poe2", fname, rng.below(2) ? 1 : -1); break;
				case 4: if (pick >= 0) F::MoveBookmark(s, pick, rng.below(2) ? fname : ""); break;
				case 5: if (pick >= 0 && list.size() > 1) F::MoveBookmark(s, pick, "", list[rng.below((int)list.size())]); break;
				case 6: if (pick >= 0) F::MoveBookmarkBy(s, pick, rng.below(2) ? 1 : -1); break;
				case 7: if (pick >= 0) { s.bookmarks[pick].hotkey = "Ctrl + " + std::to_string(rng.below(9)); s.bookmarks[pick].name += "!"; } break;
				case 8: F::SetCollapsed(s, "poe2", rng.below(2) ? fname : "", rng.below(2) == 1); break;
				}
				if (!Sorted(s, &why)) sorted = false;
			}
			if (snap() != before) same = false;
			RegexUiState back;
			back.Parse(s.Serialize());
			if (back.Serialize() != s.Serialize()) round = false;
		}
		check(same, "30 rounds x 40 random PoE2 edits: PoE1's list, folders, slots and hotkeys unchanged");
		check(sorted, "and the order invariant held after every step" + (why.empty() ? std::string() : ": " + why));
		check(round, "and every result survives a file round trip");
	}
	{
		Rng rng(3601);
		RegexUiState s;
		const char* games[3] = {"poe1", "poe2", ""};
		for (int i = 0; i < 20; i++) s.bookmarks.push_back(Bm("b" + std::to_string(i), games[rng.below(3)]));
		std::multiset<std::string> ids;
		for (const RegexBookmark& b : s.bookmarks) ids.insert(b.name);
		bool sorted = true;
		std::string why;
		for (int step = 0; step < 2000; step++) {
			const std::string g = rng.below(2) ? "poe1" : "poe2";
			const auto& fl = s.Folders(g);
			const std::string fname = fl.empty() ? std::string() : fl[rng.below((int)fl.size())].name;
			const int idx = rng.below((int)s.bookmarks.size());
			switch (rng.below(8)) {
			case 0: F::Add(s, g, "f" + std::to_string(rng.below(6))); break;
			case 1: if (!fname.empty()) F::Rename(s, g, fname, "f" + std::to_string(rng.below(6))); break;
			case 2: if (!fname.empty() && rng.below(3) == 0) F::Delete(s, g, fname); break;
			case 3: if (!fname.empty()) F::MoveTo(s, g, fname, rng.below(6)); break;
			case 4: F::MoveBookmark(s, idx, rng.below(2) ? fname : ""); break;
			case 5: F::MoveBookmark(s, idx, "", rng.below((int)s.bookmarks.size())); break;
			case 6: F::MoveBookmarkBy(s, idx, rng.below(2) ? 1 : -1); break;
			case 7: F::SetCollapsed(s, g, fname, rng.below(2) == 1); break;
			}
			if (sorted && !Sorted(s, &why)) sorted = false;
		}
		std::multiset<std::string> now;
		for (const RegexBookmark& b : s.bookmarks) now.insert(b.name);
		check(now == ids, "2000 random steps: no bookmark gained or lost");
		check(sorted, "the order invariant held throughout" + (why.empty() ? std::string() : ": " + why));
		const std::string snap = s.Serialize();
		check(!F::Normalize(s) && !F::SortBookmarks(s, "poe1") && !F::SortBookmarks(s, "poe2") && s.Serialize() == snap,
		      "normalize is idempotent on a kept-sorted state");
		RegexUiState back;
		back.Parse(snap);
		check(back.Serialize() == snap, "and the result survives a file round trip");
	}
}

// ---- bookmark = whole page, on our own Data -----------------------------------------

using RegexEmbed::PicksMap;
using RegexEmbed::ValuesMap;

std::string QueryOf(const Game& G, const std::string& pageId, const PicksMap& picks, const ValuesMap& values,
                    RegexGen::Mode mode, Lang lang)
{
	std::vector<CombineSel> sels;
	std::vector<RegexGen::Corpus> corpora(2);
	for (const PageRef& p : CombineOrder(G.pages, &pageId)) {
		CombineSel s;
		s.page = p;
		auto it = picks.find(p.Id());
		if (it != picks.end()) s.picks = it->second;
		auto v = values.find(NumericKeyOf(p.Id()));
		if (p.algo && v != values.end()) s.values = &v->second;
		sels.push_back(s);
	}
	return CombineSingle(lang, mode, sels).query;
}

void ApplyInto(const Game& G, const RegexEmbed::BookmarkApply& a, PicksMap& picks, ValuesMap& values)
{
	for (const auto& p : a.picks) picks[p.first] = p.second;
	for (const auto& v : a.values)
		for (const auto& kv : v.second) values[v.first][kv.first] = kv.second;
	(void)G;
}

void SnapshotTests(const std::map<std::string, Game>& games)
{
	line("[R5-snapshot] a bookmark is the whole page: numeric section, vendor page, merge state");
	struct Case {
		const char* game;
		const char* page;
	};
	Rng rng(505);
	int n = 0, bad = 0;
	std::string firstBad;
	for (const Case& c : {Case{"poe1", "map_mods"}, Case{"poe2", "waystone_mods"}, Case{"poe1", "vendor_items"},
	                      Case{"poe2", "vendor_items_poe2"}, Case{"poe1", "scarabs"}, Case{"poe2", "tablet_mods"}}) {
		const Game& G = games.at(c.game);
		const PageRef* page = G.Find(c.page);
		if (!page) {
			check(false, std::string("page ") + c.page + " exists");
			continue;
		}
		const AlgoPage* sec = SectionPageOf(G.pages, c.page, c.game);
		for (int k = 0; k < 12; k++) {
			PicksMap picks;
			ValuesMap values;
			std::set<int> own;
			const int want = std::min<int>((int)page->Size(), 1 + rng.below(4));
			while ((int)own.size() < want) own.insert(rng.below((int)page->Size()));
			if (k % 4 == 3 && sec) own.clear();   // section only
			picks[c.page] = std::vector<int>(own.begin(), own.end());
			auto fill = [&](const AlgoPage& ap) {
				ValueMap& m = values[NumericKeyOf(ap.id)];
				for (const AlgoEntry& e : ap.entries) {
					if (rng.below(2)) continue;
					AlgoValue v = e.input.def;
					if (e.input.kind == InputKind::Range) v.min = (double)(1 + rng.below(20));
					m[e.def.id] = v;
				}
			};
			if (page->algo) fill(*page->algo);
			if (sec) {
				std::set<int> sp;
				const int sn = 1 + rng.below(4);
				while ((int)sp.size() < std::min<int>(sn, (int)sec->entries.size())) sp.insert(rng.below((int)sec->entries.size()));
				picks[sec->id] = std::vector<int>(sp.begin(), sp.end());
				fill(*sec);
			}
			const RegexGen::Mode mode = (RegexGen::Mode)rng.below(3);
			const Lang lang = rng.below(2) ? Lang::En : Lang::Zh;
			std::optional<RegexBookmark> body = RegexEmbed::BookmarkBodyOf(G.pages, *page, picks, values, c.game,
			                                                                mode == RegexGen::Mode::All ? "all" : mode == RegexGen::Mode::None ? "none" : "any",
			                                                                lang == Lang::En ? "en" : "zh");
			n++;
			if (!body) {
				if (!bad++) firstBad = std::string(c.page) + ": no body";
				continue;
			}
			body->name = "snap";
			RegexUiState st;
			st.bookmarks.push_back(*body);
			RegexUiState back;
			back.Parse(st.Serialize());
			if (back.bookmarks.size() != 1) {
				if (!bad++) firstBad = std::string(c.page) + ": bookmark dropped on re-read";
				continue;
			}
			// Restore over a panel that has OTHER ticks and values on the same page.
			PicksMap later;
			ValuesMap laterV;
			later[c.page] = {0};
			if (sec) {
				later[sec->id] = {(int)sec->entries.size() - 1};
				laterV[NumericKeyOf(sec->id)][sec->entries.back().def.id] = sec->entries.back().input.def;
			}
			const std::optional<RegexEmbed::BookmarkApply> a = RegexEmbed::BookmarkApplyOf(G.pages, back.bookmarks[0]);
			if (!a || a->missed != 0) {
				if (!bad++) firstBad = std::string(c.page) + ": apply failed / missed";
				continue;
			}
			ApplyInto(G, *a, later, laterV);
			const std::string q0 = QueryOf(G, c.page, picks, values, mode, lang);
			const std::string q1 = QueryOf(G, c.page, later, laterV, mode, lang);
			if (q0 != q1 || later[c.page] != picks[c.page] || (sec && later[sec->id] != picks[sec->id])) {
				if (!bad++) firstBad = std::string(c.page) + " #" + Num(k) + ": \"" + q0 + "\" vs \"" + q1 + "\"";
			}
		}
	}
	check(bad == 0, Num(n - bad) + " / " + Num(n) + " snapshots restore the same single-page string (ticks, section, values)" +
	                (bad ? "\n      first: " + firstBad : std::string()));

	// An old modifier bookmark (no num) restores with the section UNticked: the
	// string it gives is the one it gave when it was saved.
	{
		const Game& G = games.at("poe1");
		RegexBookmark old;
		old.name = "old";
		old.page = "map_mods";
		old.game = "poe1";
		old.keys = {RegexEmbed::KeyOf(G.Find("map_mods")->corpus->entries[3])};
		const auto a = RegexEmbed::BookmarkApplyOf(G.pages, old);
		bool sectionCleared = false;
		if (a)
			for (const auto& p : a->picks)
				if (p.first == "map_numeric" && p.second.empty()) sectionCleared = true;
		check(a && a->page == "map_mods" && sectionCleared && a->values.empty(),
		      "a bookmark from before the numeric section restores with the section unticked");
	}

	// The merge state survives the file and gives the same merged string.
	{
		const Game& G = games.at("poe1");
		RegexUiState s;
		s.custom = {u8"6 連結", "a.b (c)"};
		s.excludes = {u8"反射", "Life"};
		s.outScope = "page";
		RegexUiState back;
		back.Parse(s.Serialize());
		const PageRef* mp = G.Find("map_mods");
		const PageRef* sc = G.Find("scarabs");
		std::vector<CombineSel> sels(2);
		sels[0].page = *mp;
		sels[0].picks = {1, 4};
		sels[1].page = *sc;
		sels[1].picks = {2};
		const CombineResult r0 = Combine(Lang::Zh, RegexGen::Mode::Any, sels, s.custom, s.excludes);
		const CombineResult r1 = Combine(Lang::Zh, RegexGen::Mode::Any, sels, back.custom, back.excludes);
		check(back.outScope == "page" && r0.query == r1.query && !r0.query.empty(),
		      "custom text, excludes and the output scope survive the file; the merged string is the same");
	}
}

// ---- golden ----------------------------------------------------------------------

AlgoValue ValueFrom(const json& j)
{
	AlgoValue v;
	if (j.contains("min") && j["min"].is_number()) v.min = j["min"].get<double>();
	if (j.contains("max") && j["max"].is_number()) v.max = j["max"].get<double>();
	if (j.contains("choice") && j["choice"].is_string()) {
		v.choice = j["choice"].get<std::string>();
		v.hasChoice = true;
	}
	return v;
}

json NumJ(double d)
{
	if (std::floor(d) == d && std::fabs(d) < 1e15) return (long long)d;
	return d;
}

json ValueJ(const AlgoValue& v)
{
	json o = json::object();
	if (v.min) o["min"] = NumJ(*v.min);
	if (v.max) o["max"] = NumJ(*v.max);
	if (RegexFrag::HasChoice(v)) o["choice"] = v.choice;
	return o;
}

json ApplyJ(const std::optional<RegexEmbed::BookmarkApply>& a)
{
	if (!a) return nullptr;
	json picks = json::array(), values = json::array();
	for (const auto& p : a->picks) picks.push_back({p.first, p.second});
	for (const auto& v : a->values) {
		json m = json::array();
		for (const auto& kv : v.second) m.push_back({kv.first, ValueJ(kv.second)});
		values.push_back({v.first, m});
	}
	return {a->page, picks, values, a->missed};
}

json GroupJ(const F::Grouped& g)
{
	json groups = json::array();
	for (const F::Group& x : g.groups) groups.push_back({x.folder, x.collapsed, x.items});
	return {g.headers, groups};
}

// Replays one recorded folders.ts operation; returns its result as the TS printed it.
json RunOp(RegexUiState& s, const json& op)
{
	const std::string k = op[0].get<std::string>();
	if (k == "add") return F::ResultId(F::Add(s, op[1], op[2]));
	if (k == "rename") return F::ResultId(F::Rename(s, op[1], op[2], op[3]));
	if (k == "delete") return F::Delete(s, op[1], op[2]);
	if (k == "moveTo") return F::MoveTo(s, op[1], op[2], op[3].get<int>());
	if (k == "moveBy") return F::MoveBy(s, op[1], op[2], op[3].get<int>());
	if (k == "collapse") return F::SetCollapsed(s, op[1], op[2], op[3].get<bool>());
	if (k == "moveBm") return F::MoveBookmark(s, op[1].get<int>(), op[2], op[3].get<int>());
	if (k == "moveBmBy") return F::MoveBookmarkBy(s, op[1].get<int>(), op[2].get<int>());
	if (k == "group") return GroupJ(F::GroupBookmarks(s, op[1], op[2].get<bool>()));
	if (k == "counts") {
		json out = json::array();
		for (const auto& kv : F::Counts(s, op[1])) out.push_back({kv.first, kv.second});
		return out;
	}
	if (k == "normalize") return F::Normalize(s);
	if (k == "name") return F::NormalizeName(op[1]);
	return "unknown op " + k;
}

void GoldenR5(const std::map<std::string, Game>& games)
{
	line("[R5-golden] exile-appraiser state.ts / folders.ts / embed.ts over our Data files (regex_r5_golden.inc)");
	std::map<std::string, int> total, bad;
	std::map<std::string, std::string> firstBad;
	int parseErr = 0;
	auto fail = [&](const std::string& kind, const std::string& what) {
		if (!bad[kind]++) firstBad[kind] = what;
	};
	auto firstDiff = [](const std::string& a, const std::string& b) {
		size_t i = 0;
		while (i < a.size() && i < b.size() && a[i] == b[i]) i++;
		const size_t from = i > 60 ? i - 60 : 0;
		return "at byte " + std::to_string(i) + "\n        got  " + json(a.substr(from, 140)).dump() +
		       "\n        want " + json(b.substr(from, 140)).dump();
	};
	for (const char* rec : kRegexR5Golden) {
		json r = json::parse(rec, nullptr, false);
		if (r.is_discarded() || !r.is_array() || r.empty()) {
			parseErr++;
			continue;
		}
		const std::string kind = r[0].get<std::string>();
		total[kind]++;
		if (kind == "state") {
			// [state, label, text, ok, serialized]
			RegexUiState s;
			const bool ok = s.Parse(r[2].get<std::string>());
			const std::string got = s.Serialize();
			if (ok != r[3].get<bool>()) fail(kind, r[1].get<std::string>() + ": ok " + (ok ? "true" : "false"));
			else if (got != r[4].get<std::string>()) fail(kind, r[1].get<std::string>() + " " + firstDiff(got, r[4]));
		} else if (kind == "ops") {
			// [ops, label, input, ops, results, final]
			RegexUiState s;
			s.Parse(r[2].get<std::string>());
			bool ok = true;
			for (size_t i = 0; i < r[3].size() && ok; i++) {
				const json got = RunOp(s, r[3][i]);
				if (got != r[4][i]) {
					fail(kind, r[1].get<std::string>() + " step " + Num((long long)i) + " " + r[3][i].dump() + ": got " +
					           got.dump().substr(0, 300) + " want " + r[4][i].dump().substr(0, 300));
					ok = false;
				}
			}
			if (ok && s.Serialize() != r[5].get<std::string>()) fail(kind, r[1].get<std::string>() + " final " + firstDiff(s.Serialize(), r[5]));
		} else if (kind == "body") {
			// [body, game, page, picks, values, mode, lang, bodyText|null, applies]
			const Game& G = games.at(r[1].get<std::string>());
			const PageRef* page = G.Find(r[2].get<std::string>());
			if (!page) { fail(kind, "unknown page " + r[2].dump()); continue; }
			PicksMap picks;
			ValuesMap values;
			for (const json& p : r[3]) picks[p[0].get<std::string>()] = p[1].get<std::vector<int>>();
			for (const json& v : r[4]) {
				ValueMap& m = values[v[0].get<std::string>()];
				for (const json& kv : v[1]) m[kv[0].get<std::string>()] = ValueFrom(kv[1]);
			}
			const std::optional<RegexBookmark> body =
				RegexEmbed::BookmarkBodyOf(G.pages, *page, picks, values, r[1], r[5], r[6]);
			std::string label = r[1].get<std::string>() + " " + r[2].get<std::string>() + " " + r[3].dump().substr(0, 120);
			if (r[7].is_null() != !body) {
				fail(kind, label + ": body " + (body ? "present" : "absent"));
				continue;
			}
			if (body) {
				RegexUiState s;
				s.bookmarks.push_back(*body);
				s.bookmarks[0].name = "x";
				const std::string got = s.Serialize();
				if (got != r[7].get<std::string>()) {
					fail(kind, label + " " + firstDiff(got, r[7]));
					continue;
				}
			}
			for (const json& ap : r[8]) {
				RegexUiState s;
				s.Parse(ap[0].get<std::string>());
				if (s.bookmarks.size() != 1) {
					fail(kind, label + ": apply input bookmark dropped");
					break;
				}
				const json got = ApplyJ(RegexEmbed::BookmarkApplyOf(G.pages, s.bookmarks[0]));
				if (got != ap[1]) {
					fail(kind, label + ": apply got " + got.dump().substr(0, 300) + " want " + ap[1].dump().substr(0, 300));
					break;
				}
			}
		} else if (kind == "apply") {
			// [apply, game, text, expected]
			const Game& G = games.at(r[1].get<std::string>());
			RegexUiState s;
			s.Parse(r[2].get<std::string>());
			const json got = s.bookmarks.size() == 1 ? ApplyJ(RegexEmbed::BookmarkApplyOf(G.pages, s.bookmarks[0])) : json("no bookmark");
			if (got != r[3]) fail(kind, r[2].get<std::string>().substr(0, 120) + ": got " + got.dump() + " want " + r[3].dump());
		} else if (kind == "saved") {
			// [saved, game, text, [[pageId, [picked, missed]|null]]]
			const Game& G = games.at(r[1].get<std::string>());
			RegexUiState s;
			s.Parse(r[2].get<std::string>());
			json got = json::array();
			for (const PageRef& p : G.pages) {
				const std::optional<RegexEmbed::Applied> a = RegexEmbed::SavedPicksOf(p, s);
				got.push_back({p.Id(), a ? json{a->picked, a->missed} : json(nullptr)});
			}
			if (got != r[3]) fail(kind, r[1].get<std::string>() + ": got " + got.dump().substr(0, 300) + " want " + r[3].dump().substr(0, 300));
		} else {
			fail("unknown", kind);
		}
	}
	check(parseErr == 0 && !total.empty(), "fixture parses (" + Num(sizeof kRegexR5Golden / sizeof kRegexR5Golden[0]) + " records)");
	for (const auto& kv : total) {
		const int b = bad[kv.first];
		check(b == 0, kv.first + ": " + Num(kv.second - b) + " / " + Num(kv.second) + " identical" +
		              (b ? "\n      first: " + firstBad[kv.first] : std::string()));
	}
	if (bad.count("unknown")) check(false, "unknown record kind " + firstBad["unknown"]);
}

} // namespace

void RegexR5Tests(const std::wstring& exeDir, void (*checkFn)(bool, const std::string&), void (*lineFn)(const std::string&))
{
	g_check = checkFn;
	g_line = lineFn;
	const DWORD t0 = GetTickCount();
	StateTests();
	line("");
	FileTests();
	line("");
	FolderTests();
	line("");
	RegexDataset ds;
	std::string err;
	if (!ds.Load(exeDir, L"poe1", &err)) {
		check(false, "R5: load data: " + err);
		return;
	}
	std::map<std::string, Game> games;
	for (const char* g : {"poe1", "poe2"}) games[g].algo = AlgoPages(g, ds.Labels(g));
	// PageRefs point into ds and games[g].algo; neither moves from here on.
	for (auto& kv : games) {
		for (const RegexPageDef& p : ds.Pages())
			if (p.game == kv.first) kv.second.pages.push_back({&p, nullptr});
		for (const AlgoPage& p : kv.second.algo) kv.second.pages.push_back({nullptr, &p});
	}
	SnapshotTests(games);
	line("");
	GoldenR5(games);
	line("    (R5/R6 checks took " + Num((long long)(GetTickCount() - t0)) + " ms)");
}
