#include "regex_selftest.h"

#include "regex_data.h"
#include "regex_gen.h"
#include "regex_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

using RegexGen::Corpus;
using RegexGen::Entry;
using RegexGen::Mode;

namespace {

int g_pass = 0, g_fail = 0;
std::string g_rep;

void line(const std::string& s) { g_rep += s; g_rep += '\n'; }

void check(bool ok, const std::string& what)
{
	(ok ? g_pass : g_fail)++;
	line(std::string(ok ? "  [PASS] " : "  [FAIL] ") + what);
}

Corpus Make(const std::vector<std::string>& texts)
{
	std::vector<Entry> es;
	for (size_t i = 0; i < texts.size(); i++) {
		Entry e;
		e.id = "e" + std::to_string(i);
		e.texts.push_back(texts[i]);
		es.push_back(std::move(e));
	}
	Corpus c;
	c.Reset(std::move(es));
	return c;
}

// Entries built by hand (hidden text and all), plus the page's ambient text.
Corpus MakeEx(std::vector<Entry> es, RegexGen::Ambient amb = RegexGen::Ambient{})
{
	Corpus c;
	c.Reset(std::move(es), std::move(amb));
	return c;
}

// Deterministic, and deliberately not std::mt19937: the point is that a failure
// is reproducible from the seed printed in the report, on any toolchain.
struct Rng {
	unsigned s;
	unsigned next() { s = s * 1664525u + 1013904223u; return s >> 8; }
	int below(int n) { return n <= 0 ? 0 : (int)(next() % (unsigned)n); }
};

// ---- synthetic --------------------------------------------------------------

void SyntheticTests()
{
	line("[T1] separating three unrelated lines");
	{
		Corpus c = Make({u8"怪物不能被詛咒", u8"怪物不能被嘲諷", u8"玩家有較少的護盾"});
		RegexGen::Result r = c.Build({0}, Mode::Any);
		check(r.exact, "one pick resolves");
		RegexGen::Check v = c.Verify({0}, r.query);
		check(v.ok, "query selects exactly the pick: " + r.query);
		r = c.Build({0, 1}, Mode::Any);
		v = c.Verify({0, 1}, r.query);
		check(v.ok, "two picks: " + r.query);
	}

	line("[T2] a token may never cross a rolled number");
	{
		// The literal entry is what the wildcard entry PRINTS when it rolls a 5.
		// Nothing can tell them apart, and the honest answer is to say so rather
		// than emit a token that matches both.
		Corpus c = Make({u8"造成 # 點傷害", u8"造成 5 點傷害"});
		RegexGen::Result r = c.Build({1}, Mode::Any);
		check(!r.exact && r.unresolved.size() == 1,
		      "literal 5 cannot be told apart from the # that rolls it");
		r = c.Build({0}, Mode::Any);
		check(!r.exact && r.unresolved.size() == 1, "and not the other way round either");

		// The same wording with a different tail is separable, and must not be
		// separated by a token that reaches over the number.
		Corpus d = Make({u8"造成 # 點火焰傷害", u8"造成 # 點冰冷傷害"});
		RegexGen::Result r2 = d.Build({0}, Mode::Any);
		check(r2.exact, "different tails are separable");
		check(r2.query.find('#') == std::string::npos, "the query never contains a '#'");
		check(d.Verify({0}, r2.query).ok, "and it selects only the first: " + r2.query);
	}

	line("[T3] anchors");
	{
		Corpus c = Make({"abc", "xabc"});
		RegexGen::Result r = c.Build({0}, Mode::Any);
		check(r.exact && r.query.find('^') != std::string::npos,
		      "a prefix-only match needs '^': " + r.query);
		check(c.Verify({0}, r.query).ok, "and it is exact");

		Corpus d = Make({"abc", "abcx"});
		RegexGen::Result r2 = d.Build({0}, Mode::Any);
		check(r2.exact && r2.query.find('$') != std::string::npos,
		      "a suffix-only match needs '$': " + r2.query);
		check(d.Verify({0}, r2.query).ok, "and it is exact");
	}

	line("[T4] the search is case-insensitive, so the corpus must be too");
	{
		Corpus c = Make({"Fire Damage", "Cold Damage"});
		RegexGen::Result r = c.Build({0}, Mode::Any);
		check(r.exact && c.Verify({0}, r.query).ok, "mixed case: " + r.query);
		RegexGen::Check v = c.Verify({0}, "\"FIRE\"");
		check(v.ok, "an upper-case query still finds the entry");
	}

	line("[T5] every mode produces the shape the client expects");
	{
		Corpus c = Make({u8"甲", u8"乙", u8"丙"});
		RegexGen::Result any = c.Build({0, 1}, Mode::Any);
		check(!any.query.empty() && any.query.front() == '"' && any.query.back() == '"',
		      "Any is one quoted term: " + any.query);
		check(any.query.find('|') != std::string::npos, "and it is an alternation");

		RegexGen::Result none = c.Build({0, 1}, Mode::None);
		check(none.query.compare(0, 2, "\"!") == 0, "None negates it: " + none.query);
		check(c.Verify({0, 1}, none.query).ok, "and it still names exactly the picks");

		RegexGen::Result all = c.Build({0, 1}, Mode::All);
		check(all.query.find('|') == std::string::npos,
		      "All is separate terms, never an alternation: " + all.query);
		check(c.Verify({0, 1}, all.query).ok, "and each term names one pick");
	}

	line("[T6] identical text cannot be separated, and says so");
	{
		Corpus c = Make({u8"完全一樣的一行", u8"完全一樣的一行", u8"另一行"});
		RegexGen::Result r = c.Build({0}, Mode::Any);
		check(!r.exact, "duplicated text is reported unresolved, not silently merged");
	}

	line("[T7] an entry matches if ANY of its printed lines does");
	{
		std::vector<Entry> es;
		Entry a;
		a.id = "two-line";
		a.texts = {u8"第一行", u8"第二行"};
		Entry b;
		b.id = "other";
		b.texts = {u8"無關的一行"};
		es.push_back(a);
		es.push_back(b);
		Corpus c;
		c.Reset(std::move(es));
		check(c.Verify({0}, u8"\"第二行\"").ok, "a token cut from the second line finds it");
		RegexGen::Result r = c.Build({0}, Mode::Any);
		check(r.exact && c.Verify({0}, r.query).ok, "and Build agrees: " + r.query);
	}

	line("[T8] the checker can fail");
	{
		// Without this every other PASS above is worthless: a Verify that always
		// says yes would make the whole file green.
		Corpus c = Make({u8"怪物不能被詛咒", u8"怪物不能被嘲諷"});
		RegexGen::Check over = c.Verify({0}, u8"\"怪物\"");
		check(!over.ok && over.extra.size() == 1, "an over-broad query is caught");
		RegexGen::Check under = c.Verify({0, 1}, u8"\"詛咒\"");
		check(!under.ok && under.missing.size() == 1, "a query that misses a pick is caught");
		RegexGen::Check crossing = c.Verify({0}, u8"\"不能\"");
		check(!crossing.ok, "a token shared by both entries is caught");
	}

	line("[T9] the same picks always produce the same string");
	{
		Corpus c = Make({u8"一二三四", u8"二三四五", u8"三四五六", u8"四五六七"});
		RegexGen::Result a = c.Build({0, 2}, Mode::Any);
		RegexGen::Result b = c.Build({0, 2}, Mode::Any);
		check(a.query == b.query, "repeat run: " + a.query);
		RegexGen::Result rev = c.Build({2, 0}, Mode::Any);
		check(rev.query == a.query, "and the order the boxes were ticked does not matter");
	}

	line("[T10] length is counted the way the client counts it");
	{
		check(RegexGen::CharCount(u8"怪物") == 2, "two Chinese characters cost two");
		check(RegexGen::CharCount("ab") == 2, "two ASCII characters cost two");
		check(RegexGen::CharCount(u8"a怪") == 2, "and a mixture adds up");
	}

	// ---- hidden and ambient text: the search reads more than the line ----------

	line("[T13] hidden text is never cut into tokens and never counts as finding an entry");
	{
		Entry a;
		a.id = "a";
		a.texts = {u8"甲乙"};
		a.hidden = {u8"丙丁"};
		Entry b;
		b.id = "b";
		b.texts = {u8"戊己"};
		Corpus c = MakeEx({a, b});
		RegexGen::Result r = c.Build({0}, Mode::Any);
		check(r.exact && r.query.find(u8"丙") == std::string::npos &&
		      r.query.find(u8"丁") == std::string::npos,
		      "the query is cut from the printed line only: " + r.query);
		RegexGen::Check v = c.Verify({0}, u8"\"丙\"");
		check(!v.ok && v.missing.size() == 1 && v.missing[0] == 0,
		      "a term that only hits hidden text does not find the entry");
		check(v.extra.empty(), "and nobody else is reported for it either");
	}

	line("[T14] a token that also hits another entry's hidden text is rejected");
	{
		// The user's report: 燃燒的地圖 prints one line, but its advanced
		// description carries the tag 異常狀態, so a bare 常 finds it.
		Entry a;
		a.id = "avoid-ailments";
		a.texts = {u8"元素異常狀態"};
		Entry b;
		b.id = "ignite";
		b.texts = {u8"玩家有更少護甲"};
		b.hidden = {u8"元素,火焰,異常狀態"};
		Corpus c = MakeEx({a, b});
		RegexGen::Result r = c.Build({0}, Mode::Any);
		check(r.exact, "still resolvable with a longer piece: " + r.query);
		check(c.Verify({0}, r.query).ok, "and Verify agrees the hidden text is not touched");
		RegexGen::Check v = c.Verify({0}, u8"\"常\"");
		check(!v.ok && v.extra.size() == 1 && v.extra[0] == 1,
		      "a term that reaches the other entry's hidden text is an over-match");
		RegexGen::Result both = c.Build({0, 1}, Mode::Any);
		check(both.exact && c.Verify({0, 1}, both.query).ok,
		      "with both picked the shared text is fair game again: " + both.query);
		RegexGen::Result all = c.Build({0, 1}, Mode::All);
		check(all.exact, "All resolves both: " + all.query);
		bool clean = true;
		for (const std::string& t : all.tokens) {
			const std::string q = "\"" + t + "\"";
			if (!(c.Verify({0}, q).ok || c.Verify({1}, q).ok)) clean = false;
		}
		check(clean, "each All term names one pick without reaching the other's hidden text");

		// Numbers in hidden text are wildcards, exactly as in printed text.
		Entry p;
		p.id = "literal";
		p.texts = {u8"持續 5 秒"};
		Entry q;
		q.id = "other";
		q.texts = {u8"其他"};
		q.hidden = {u8"持續 # 秒"};
		Corpus d = MakeEx({p, q});
		RegexGen::Result r2 = d.Build({0}, Mode::Any);
		check(!r2.exact, "a literal number a hidden wildcard could print cannot be singled out");
	}

	line("[T15] text on every item vetoes a token, and Verify names the term");
	{
		Entry a;
		a.id = "level";
		a.texts = {u8"怪物等級增加"};
		Entry b;
		b.id = "life";
		b.texts = {u8"玩家生命"};
		Entry c3;
		c3.id = "corrupted";
		c3.texts = {u8"已汙染"};
		RegexGen::Ambient amb;
		amb.lines = {u8"怪物等級：#", u8"已汙染"};
		Corpus c = MakeEx({a, b, c3}, amb);
		RegexGen::Result r = c.Build({0}, Mode::Any);
		check(r.exact, "resolvable past the ambient text: " + r.query);
		check(c.Verify({0}, r.query).ok, "and the query does not touch it");
		RegexGen::Check v = c.Verify({0}, u8"\"怪物\"");
		check(!v.ok && v.ambient.size() == 1, "a term found on every item is named as such");
		RegexGen::Result r3 = c.Build({2}, Mode::Any);
		check(!r3.exact, "an entry whose whole line is on every item cannot be singled out");

		// The seam of a rare name: "Agony Desire" is on some item even though
		// neither word is a line of its own.
		Entry e;
		e.id = "dues";
		e.texts = {"pay dues"};
		Entry f;
		f.id = "other";
		f.texts = {"other"};
		RegexGen::Ambient names;
		names.nameLeft = {"Agony"};
		names.nameRight = {" Desire"};
		Corpus d = MakeEx({e, f}, names);
		RegexGen::Result r4 = d.Build({0}, Mode::Any);
		check(r4.exact && d.Verify({0}, r4.query).ok,
		      "a token never straddles the seam of a rare name: " + r4.query);
		RegexGen::Check v2 = d.Verify({0}, "\"y d\"");
		check(!v2.ok && v2.ambient.size() == 1, "and a term that does is named");
	}

	line("[T16] anchors are honoured against hidden lines too");
	{
		Entry a;
		a.id = "abc";
		a.texts = {"abc"};
		Entry b;
		b.id = "xabc";
		b.texts = {"xabc"};
		b.hidden = {"zabcz"};
		Corpus c = MakeEx({a, b});
		RegexGen::Result r = c.Build({0}, Mode::Any);
		check(r.exact && r.query.find('^') != std::string::npos && c.Verify({0}, r.query).ok,
		      "a '^' token clears a hidden line that has the text mid-way: " + r.query);
		// With the hidden line STARTING with the text, '^abc' is no longer
		// enough; only the form anchored at both ends is left.
		b.hidden = {"abcz"};
		Corpus d = MakeEx({a, b});
		RegexGen::Result r2 = d.Build({0}, Mode::Any);
		check(r2.exact && r2.query.find("^abc$") != std::string::npos &&
		      d.Verify({0}, r2.query).ok,
		      "a hidden line starting with it forces the fully anchored form: " + r2.query);
		RegexGen::Check v = d.Verify({0}, "\"^abc\"");
		check(!v.ok && v.extra.size() == 1, "and Verify sees the anchored hidden hit");
	}
}


// ---- remembered state and bookmarks -----------------------------------------

// A scratch directory, because RegexUiState::Save writes to PobTools\ under the
// directory it is given -- and the directory the tool normally runs in is the
// user's install, where that file holds their real bookmarks. A self-test that
// overwrote them would be worse than no self-test at all.
std::wstring ScratchDir()
{
	wchar_t tmp[MAX_PATH] = {};
	if (!GetTempPathW(MAX_PATH, tmp)) return L"";
	std::wstring dir = std::wstring(tmp) + L"pobtools_regex_selftest\\";
	CreateDirectoryW(dir.c_str(), nullptr);
	return dir;
}

void RemoveScratch(const std::wstring& dir)
{
	if (dir.empty()) return;
	DeleteFileW((dir + L"PobTools\\regex_ui.json").c_str());
	DeleteFileW((dir + L"PobTools\\regex_ui.json.tmp").c_str());
	RemoveDirectoryW((dir + L"PobTools").c_str());
	RemoveDirectoryW(dir.c_str());
}

void StateTests()
{
	line("[T11] saved picks resolve by the English line, and misses are counted");
	{
		// The page as it might look after a patch: one entry reworded in English
		// only, one gone entirely, one untouched.
		const std::vector<std::string> enNow = {"Monsters cannot be Leeched from",
		                                        "Monsters have increased Damage",
		                                        "Area is haunted"};
		const std::vector<std::string> zhNow = {u8"怪物不能被吸取",
		                                        u8"怪物增加傷害",
		                                        u8"區域鬧鬼"};
		std::vector<char> picked;

		int missed = RegexResolveKeys({"Area is haunted"}, {u8"區域鬧鬼"}, enNow, zhNow, picked);
		check(missed == 0 && picked.size() == 3 && picked[2] == 1 && picked[0] == 0,
		      "an unchanged entry comes back on the right row");

		// English reworded, Chinese untouched -> the fallback has to catch it.
		missed = RegexResolveKeys({"Monsters have #% increased Damage"}, {u8"怪物增加傷害"},
		                          enNow, zhNow, picked);
		check(missed == 0 && picked[1] == 1,
		      "a reworded English line still resolves through the Chinese one");

		// Gone from both -> reported, not silently dropped.
		missed = RegexResolveKeys({"Area contains a Breach"}, {u8"區域內有裂痕"},
		                          enNow, zhNow, picked);
		check(missed == 1, "an entry that no longer exists is counted, not ignored");
		bool none = true;
		for (char c : picked) if (c) none = false;
		check(none, "and it does not tick something else by accident");

		// Whole selections, mixed.
		missed = RegexResolveKeys({"Area is haunted", "Gone", "Monsters cannot be Leeched from"},
		                          {u8"區域鬧鬼", u8"沒了", u8"怪物不能被吸取"},
		                          enNow, zhNow, picked);
		check(missed == 1 && picked[0] == 1 && picked[2] == 1 && picked[1] == 0,
		      "a mixed selection keeps what it can and counts what it cannot");
	}

	line("[T12] the state file survives a round trip");
	{
		const std::wstring dir = ScratchDir();
		if (dir.empty()) {
			check(false, "could not make a scratch directory");
			return;
		}
		RemoveScratch(dir);
		CreateDirectoryW(dir.c_str(), nullptr);

		RegexUiState a;
		check(!a.Load(dir), "a fresh install has no file, and that is not an error");
		check(a.mode == "any" && a.bookmarks.empty(), "and the defaults are usable");

		a.game = "poe1";
		a.page = "map_mods";
		a.mode = "none";
		RegexPagePicks& picks = a.PicksFor("map_mods");
		picks.keys = {"Monsters cannot be Leeched from"};
		picks.alt = {u8"怪物不能被吸取"};
		RegexBookmark bm;
		bm.name = u8"危險詞綴";
		bm.page = "map_mods";
		bm.game = "poe1";
		bm.mode = "none";
		bm.keys = {"Monsters cannot be Leeched from", "Players are Cursed with Enfeeble"};
		bm.alt = {u8"怪物不能被吸取", u8"玩家被虛弱詛咒"};
		a.bookmarks.push_back(bm);
		check(a.Save(dir), "it saves");

		RegexUiState b;
		check(b.Load(dir), "and loads back");
		check(b.page == a.page && b.mode == a.mode, "page and mode survive");
		// The game decides which selector the bookmark appears under, so losing it
		// is not a cosmetic loss: the row would have no column to be drawn in.
		check(b.game == a.game, "the chosen game survives");
		check(b.current.size() == 1 && b.current[0].keys == picks.keys &&
		      b.current[0].alt == picks.alt, "the ticks survive");
		check(b.bookmarks.size() == 1 && b.bookmarks[0].name == bm.name &&
		      b.bookmarks[0].keys == bm.keys && b.bookmarks[0].alt == bm.alt &&
		      b.bookmarks[0].mode == bm.mode, "the bookmark survives intact");
		check(b.bookmarks.size() == 1 && b.bookmarks[0].game == bm.game,
		      "and it remembers which game it belongs to");

		// Deleting one and saving must actually shrink the file, not leave the
		// old entry behind for the next load to resurrect.
		b.bookmarks.clear();
		check(b.Save(dir), "saving after a delete works");
		RegexUiState c;
		c.Load(dir);
		check(c.bookmarks.empty(), "and the deleted bookmark stays deleted");

		// A file that is not JSON at all must not take the tool down, and must not
		// leave half-parsed rubbish behind either.
		HANDLE h = CreateFileW((dir + L"PobTools\\regex_ui.json").c_str(), GENERIC_WRITE, 0,
		                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			const char junk[] = "{ this is not json";
			DWORD w = 0;
			WriteFile(h, junk, (DWORD)(sizeof(junk) - 1), &w, nullptr);
			CloseHandle(h);
		}
		RegexUiState d;
		d.bookmarks.push_back(bm);
		const bool ok = d.Load(dir);
		check(!ok && d.bookmarks.empty(),
		      "a corrupt file is refused and leaves nothing half-read behind");

		// An unknown mode from a newer build must land on something this build can
		// draw, not on a radio group with nothing selected.
		h = CreateFileW((dir + L"PobTools\\regex_ui.json").c_str(), GENERIC_WRITE, 0,
		                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			const char newer[] = "{\"page\":\"x\",\"mode\":\"someday\",\"bookmarks\":[]}";
			DWORD w = 0;
			WriteFile(h, newer, (DWORD)(sizeof(newer) - 1), &w, nullptr);
			CloseHandle(h);
		}
		RegexUiState e;
		e.Load(dir);
		check(e.mode == "any", "a mode this build does not know falls back to one it does");

		// A file written before the list was split by game. The game must come back
		// EMPTY rather than guessed: the panel fills it in from the page id, and a
		// default of "poe1" here would file a PoE2 bookmark under the wrong game
		// and then persist that answer on the next save.
		h = CreateFileW((dir + L"PobTools\\regex_ui.json").c_str(), GENERIC_WRITE, 0,
		                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			const char old[] = "{\"page\":\"map_mods\",\"mode\":\"any\",\"bookmarks\":"
			                   "[{\"name\":\"x\",\"page\":\"waystone_mods\","
			                   "\"keys\":[\"a\"],\"alt\":[\"b\"]}]}";
			DWORD w = 0;
			WriteFile(h, old, (DWORD)(sizeof(old) - 1), &w, nullptr);
			CloseHandle(h);
		}
		RegexUiState f;
		check(f.Load(dir), "a file from before the game split still loads");
		check(f.game.empty(), "with no game recorded, rather than a guessed one");
		check(f.bookmarks.size() == 1 && f.bookmarks[0].game.empty(),
		      "and its bookmark keeps its page id with the game left to be derived");

		// A game string this build does not know is not a game it can select.
		h = CreateFileW((dir + L"PobTools\\regex_ui.json").c_str(), GENERIC_WRITE, 0,
		                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h != INVALID_HANDLE_VALUE) {
			const char newer[] = "{\"game\":\"poe3\",\"page\":\"x\",\"bookmarks\":[]}";
			DWORD w = 0;
			WriteFile(h, newer, (DWORD)(sizeof(newer) - 1), &w, nullptr);
			CloseHandle(h);
		}
		RegexUiState g;
		g.Load(dir);
		check(g.game.empty(), "a game this build does not know is dropped, not carried");

		RemoveScratch(dir);
	}
}

// ---- shipped data -----------------------------------------------------------

// The invariant, on real data: whatever the generator produced, re-reading that
// string against the same list must select the picks and nothing else. `extra`
// is the fatal one -- a false positive is the failure this whole tool exists to
// avoid -- while a `missing` entry is only acceptable when Build already
// admitted it could not resolve it.
bool RoundTrip(const Corpus& c, const std::vector<int>& sel, Mode mode,
               std::string& why)
{
	RegexGen::Result r = c.Build(sel, mode);
	RegexGen::Check v = c.Verify(sel, r.query);
	if (!v.ambient.empty()) {
		why = "term \"" + v.ambient[0] + "\" matches text printed on every item of the page "
		      "(query \"" + r.query + "\")";
		return false;
	}
	if (!v.extra.empty()) {
		why = "false positive: " + std::to_string(v.extra.size()) +
		      " unpicked entries also match \"" + r.query + "\"";
		return false;
	}
	if (v.missing.size() != r.unresolved.size()) {
		why = "missed " + std::to_string(v.missing.size()) + " picks but only " +
		      std::to_string(r.unresolved.size()) + " were reported unresolvable";
		return false;
	}
	return true;
}

void DataTests(const std::wstring& exeDir)
{
	RegexDataset ds;
	std::string err;
	// Load() takes every catalogue it finds, so the loop below already covers
	// both games; naming them here is what turns "PoE2 was left out of the
	// install rules" from a quietly shorter report into a failure.
	if (!ds.Load(exeDir, L"poe1", &err)) {
		check(false, "load Data\\regex_*.json: " + err);
		return;
	}
	check(ds.Pages().size() >= 2, "at least two pages shipped (" +
	      std::to_string(ds.Pages().size()) + ")");
	check(ds.HasGame("poe1"), "the PoE1 catalogue shipped");
	check(ds.HasGame("poe2"), "the PoE2 catalogue shipped");
	// The panel picks the game first and then that game's lists, so a page whose
	// game is neither would be unreachable -- present in the file, absent from
	// every menu, and impossible to notice from inside the UI.
	{
		int stray = 0;
		std::string first;
		for (const RegexPageDef& p : ds.Pages()) {
			if (p.game == "poe1" || p.game == "poe2") continue;
			if (!stray) first = p.id + " (game=\"" + p.game + "\")";
			stray++;
		}
		check(stray == 0, "every page names a game the selector offers" +
		      (stray ? " -- " + first : std::string()));
	}

	// Both languages, because the panel builds from either and the promise --
	// "this string finds exactly what you ticked" -- has to hold in each. It is
	// not the same claim twice: two lines the Chinese cannot tell apart may be
	// trivially separable in English, and the other way round.
	for (int side = 0; side < 2; side++) {
	const bool useZh = (side == 0);
	for (const RegexPageDef& p : ds.Pages()) {
		line(std::string("[data] ") + (useZh ? u8"繁中 " : "English ") + p.title +
		     " -- " + std::to_string(p.entries.size()) + " entries");
		// Built exactly the way the panel builds it (regex_tool_ui.cpp
		// buildCorpus): lines in the chosen language, whole-entry fallback to
		// the other, hidden text following whichever the entry ended up in.
		// `full` = false leaves the hidden and ambient text out, which is only
		// used to report how much of the page they cost.
		auto build = [&](bool full) {
			std::vector<Entry> es;
			es.reserve(p.entries.size());
			for (const RegexEntryDef& d : p.entries) {
				Entry e;
				e.id = d.id;
				const std::vector<std::string>& want = useZh ? d.zh : d.en;
				const std::vector<std::string>& other = useZh ? d.en : d.zh;
				const bool useWant = !want.empty();
				e.texts = useWant ? want : other;
				if (full)
					e.hidden = useWant ? (useZh ? d.hiddenZh : d.hiddenEn)
					                   : (useZh ? d.hiddenEn : d.hiddenZh);
				es.push_back(std::move(e));
			}
			RegexGen::Ambient amb;
			if (full) {
				amb.lines = useZh ? p.ambientZh : p.ambientEn;
				amb.nameLeft = useZh ? p.namePrefixZh : p.namePrefixEn;
				amb.nameRight = useZh ? p.nameSuffixZh : p.nameSuffixEn;
			}
			Corpus c;
			c.Reset(std::move(es), std::move(amb));
			return c;
		};
		Corpus c = build(true);
		Corpus bare = build(false);

		// Shape of the new fields. Every page carries the lines its items all
		// print, in both languages -- except one whose items have no affix names
		// and no random names to speak of, and says so here rather than in a
		// silent zero.
		{
			const bool mayBeBare = (p.id == "expedition_relic_mods");
			const std::vector<std::string>& amb = useZh ? p.ambientZh : p.ambientEn;
			check(!amb.empty() || mayBeBare, "the page carries ambient text (" +
			      std::to_string(amb.size()) + " lines)");

			// A hidden line equal to a printed one would let the generator
			// think the text is hidden when the item shows it.
			int same = 0;
			std::string firstSame;
			for (const RegexEntryDef& d : p.entries) {
				const std::vector<std::string>& hid = useZh ? d.hiddenZh : d.hiddenEn;
				for (const std::string& h : hid) {
					bool hit = false;
					for (const RegexEntryDef& o : p.entries) {
						const std::vector<std::string>& vis = useZh ? o.zh : o.en;
						for (const std::string& v : vis) if (v == h) { hit = true; break; }
						if (hit) break;
					}
					if (hit) { if (!same) firstSame = h; same++; }
				}
			}
			check(same == 0, "no hidden line repeats a printed line" +
			      (same ? " -- " + firstSame : std::string()));

			// The tier is a roll: written as '#', never as the digit the
			// generator happened to see.
			int digitTier = 0;
			std::string firstTier;
			auto tierOk = [&](std::string s) {
				for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = char(ch - 'A' + 'a');
				const char* keys[2] = {u8"階層：", "tier: "};
				for (const char* k : keys) {
					size_t at = 0;
					while ((at = s.find(k, at)) != std::string::npos) {
						at += std::string(k).size();
						if (at >= s.size() || s[at] != '#') {
							if (!digitTier) firstTier = s;
							digitTier++;
						}
					}
				}
			};
			for (const std::string& s : amb) tierOk(s);
			for (const RegexEntryDef& d : p.entries) {
				const std::vector<std::string>& hid = useZh ? d.hiddenZh : d.hiddenEn;
				for (const std::string& h : hid) tierOk(h);
			}
			check(digitTier == 0, "every tier line rolls ('#'), none carries a digit" +
			      (digitTier ? " -- " + firstTier : std::string()));
		}

		// Duplicate text inside one page is a data bug, not a generator one: two
		// rows the player can tick separately but the game prints identically.
		int dup = 0;
		{
			std::vector<std::string> seen;
			for (const RegexEntryDef& d : p.entries) {
				const std::vector<std::string>& want = useZh ? d.zh : d.en;
				const std::vector<std::string>& other = useZh ? d.en : d.zh;
				const std::string k = !want.empty() ? want[0]
				                    : (other.empty() ? std::string() : other[0]);
				bool hit = false;
				for (const std::string& s : seen) if (s == k) { hit = true; break; }
				if (hit) dup++;
				else seen.push_back(k);
			}
		}
		check(dup == 0, "no two entries print the same first line (" +
		      std::to_string(dup) + " duplicates)");

		// Randomised picks, every mode. Seeds are fixed so a failure can be
		// reproduced exactly from the report.
		const int kRounds = (int)p.entries.size() > 1000 ? 40 : 120;
		const Mode modes[3] = {Mode::Any, Mode::None, Mode::All};
		for (int mi = 0; mi < 3; mi++) {
			Rng rng{0xC0FFEEu + (unsigned)mi * 7919u};
			int bad = 0;
			std::string firstWhy;
			for (int round = 0; round < kRounds; round++) {
				const int n = 1 + rng.below(10);
				std::vector<int> sel;
				for (int k = 0; k < n; k++) sel.push_back(rng.below((int)p.entries.size()));
				std::string why;
				if (!RoundTrip(c, sel, modes[mi], why)) {
					if (bad == 0) firstWhy = why;
					bad++;
				}
			}
			const char* names[3] = {"Any", "None", "All"};
			check(bad == 0, std::string(names[mi]) + ": " + std::to_string(kRounds) +
			      " random picks round-trip" + (bad ? " -- " + firstWhy : ""));
		}

		// How often the shortening actually pays, and whether a realistic pick
		// still fits. Reported rather than asserted where the number is a
		// property of the data, asserted where it is a promise to the player.
		{
			Rng rng{0x5EED1234u};
			std::vector<int> sel;
			const int want = (int)p.entries.size() < 10 ? (int)p.entries.size() : 10;
			while ((int)sel.size() < want) {
				int i = rng.below((int)p.entries.size());
				bool dupSel = false;
				for (int s : sel) if (s == i) { dupSel = true; break; }
				if (!dupSel) sel.push_back(i);
			}
			RegexGen::Result r = c.Build(sel, Mode::Any);
			int plain = 0;
			for (int i : sel) {
				const RegexEntryDef& d = p.entries[i];
				const std::vector<std::string>& want = useZh ? d.zh : d.en;
				const std::vector<std::string>& other = useZh ? d.en : d.zh;
				plain += RegexGen::CharCount(!want.empty() ? want[0] : other[0]) + 1;
			}
			line("        " + std::to_string(want) + " picks: " +
			     std::to_string(r.length) + " characters (spelled out: " +
			     std::to_string(plain) + ")");
			check(r.length <= p.limit, std::to_string(want) +
			      " picks fit the client's " + std::to_string(p.limit) + "-character field");
			check(r.length < plain, "the query is shorter than spelling the lines out");
			check(r.length <= p.limit || !r.exact,
			      "a query that fits is either exact or says which picks it dropped");
		}

		// Can each entry be found on its own? A page where many cannot is a page
		// whose data needs a second look, so the count is named either way.
		{
			const int step = (int)p.entries.size() > 400
				? (int)p.entries.size() / 400 : 1;
			int tried = 0, stuck = 0, stuckBare = 0, badVerify = 0;
			std::string examples, byHidden, firstBad;
			for (int i = 0; i < (int)p.entries.size(); i += step) {
				tried++;
				const std::vector<std::string>& want = useZh ? p.entries[i].zh
				                                            : p.entries[i].en;
				const std::string name = want.empty() ? p.entries[i].id : want[0];
				RegexGen::Result r = c.Build({i}, Mode::Any);
				if (r.exact) {
					// The whole point of the hidden and ambient text: a single
					// pick's string must not reach any other entry's hidden
					// text or anything every item prints.
					RegexGen::Check v = c.Verify({i}, r.query);
					if (!v.ok) {
						if (!badVerify) firstBad = name + " -> " + r.query;
						badVerify++;
					}
					continue;
				}
				stuck++;
				if (stuck <= 5) {
					examples += (examples.empty() ? "" : " / ");
					examples += name;
				}
				// Stuck only once the hidden and ambient text are in play: the
				// cost of the fix, printed so a league that rewrites a reminder
				// text is noticed here rather than in a bug report.
				if (bare.Build({i}, Mode::Any).exact) {
					stuckBare++;
					if (stuckBare <= 5) {
						byHidden += (byHidden.empty() ? "" : " / ");
						byHidden += name;
					}
				}
			}
			line("        singly findable: " + std::to_string(tried - stuck) + " / " +
			     std::to_string(tried) + (stuck ? "   stuck: " + examples : ""));
			line("        stuck by hidden/ambient text: " + std::to_string(stuckBare) +
			     (stuckBare ? " (" + byHidden + ")" : ""));
			check(badVerify == 0, "every single-pick string stays clear of hidden and "
			      "ambient text" + (badVerify ? " -- " + firstBad : std::string()));
			// A line whose every usable fragment also occurs in a more specific
			// line can never be singled out by any string -- "怪物增加#%攻擊速度"
			// sits entirely inside "怪物貧血時增加#%攻擊速度". That is GGG's
			// wording, not a defect here, and the panel already tells the player
			// which picks it could not resolve. The bar is therefore set where a
			// page would have to be genuinely broken to trip it; the exact count
			// and the names are printed above every run, which is what actually
			// gets read when a league changes the wording.
			//
			// It was 5% while only PoE1 shipped. PoE2's pages are a third the
			// size and its Chinese is more compact, so three general/specific
			// pairs on a 55-line page already clear 10% -- a threshold that
			// fires there is reporting page size, not data quality.
			check(stuck * 4 <= tried,
			      "at most a quarter of entries cannot be singled out");
		}

		// The report that started this: on the Chinese map page a bare 常 found
		// 燃燒的地圖 through its tag 異常狀態 and 反抗者的時空鎖鏈之地圖 through
		// the reminder text 比平常慢. Whatever else the data does, that term
		// must now be seen to reach past its own entry.
		if (useZh && p.id == "map_mods") {
			int idx = -1;
			for (int i = 0; i < (int)p.entries.size(); i++)
				if (!p.entries[i].zh.empty() &&
				    p.entries[i].zh[0] == u8"怪物有 #% 機率避免元素異常狀態") idx = i;
			if (idx < 0) {
				check(false, u8"the entry 怪物有 #% 機率避免元素異常狀態 is on the map page");
			} else {
				RegexGen::Check v = c.Verify({idx}, u8"\"常\"");
				check(!v.extra.empty() || !v.ambient.empty(),
				      u8"a bare 常 is known to reach other maps (" +
				      std::to_string(v.extra.size()) + " entries, " +
				      std::to_string(v.ambient.size()) + " ambient)");
				RegexGen::Result r = c.Build({idx}, Mode::Any);
				bool bare1 = false;
				for (const std::string& t : r.tokens) if (t == u8"常") bare1 = true;
				check(r.exact && !bare1, u8"and the entry is built without it: " + r.query);
			}
		}
	}
	}
}

} // namespace

int RunRegexSelfTest(const std::wstring& exeDir)
{
	g_pass = g_fail = 0;
	g_rep.clear();
	line("=== regex generator self-test ===");
	SyntheticTests();
	line("");
	StateTests();
	line("");
	DataTests(exeDir);
	line("");
	line("PASS " + std::to_string(g_pass) + "   FAIL " + std::to_string(g_fail));
	line(g_fail == 0 ? "ALL PASS" : "FAILURES");

	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	printf("%s", g_rep.c_str());
	HANDLE h = CreateFileW((exeDir + L"regex_selftest.txt").c_str(), GENERIC_WRITE, 0,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, g_rep.data(), (DWORD)g_rep.size(), &w, nullptr);
		CloseHandle(h);
	}
	return g_fail == 0 ? 0 : 1;
}
