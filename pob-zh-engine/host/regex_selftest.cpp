#include "regex_selftest.h"

#include "regex_data.h"
#include "regex_frag.h"
#include "regex_gen.h"
#include "regex_match.h"
#include "regex_numeric.h"
#include "regex_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <json.hpp>   // nlohmann (deps/nlohmann): parses the golden fixture

using RegexGen::Corpus;
using RegexGen::Entry;
using RegexGen::Mode;

// regex_selftest_r3.cpp: the R3 part (numeric section, vendor page, single-page output).
void RegexR3Tests(const std::wstring& exeDir, void (*check)(bool, const std::string&), void (*line)(const std::string&));
// regex_selftest_r4.cpp: the R4 part (multi-page merge, custom text, excludes).
void RegexR4Tests(const std::wstring& exeDir, void (*check)(bool, const std::string&), void (*line)(const std::string&));
// regex_selftest_r5.cpp: the R5 / R6 part (state schema 5, bookmark folders, bookmark snapshots).
void RegexR5Tests(const std::wstring& exeDir, void (*check)(bool, const std::string&), void (*line)(const std::string&));
// regex_selftest_r7.cpp: the R7 part (item-mod values page).
void RegexR7Tests(const std::wstring& exeDir, void (*check)(bool, const std::string&), void (*line)(const std::string&));
// regex_selftest_r8.cpp: the R8 part (share codes + templates).
void RegexR8Tests(const std::wstring& exeDir, void (*check)(bool, const std::string&), void (*line)(const std::string&));
// regex_selftest_send.cpp: "送到 ExileAppraiser" (locator, command line, temp files).
void RegexSendTests(void (*check)(bool, const std::string&), void (*line)(const std::string&));

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

// ---- R1/R2: the exile-appraiser port (regex_match / regex_numeric / regex_frag)

#include "regex_port_golden.inc"

using RegexFrag::AlgoValue;
using RegexNumeric::NumRange;

std::optional<double> Opt(int v) { return v < 0 ? std::nullopt : std::optional<double>(v); }

// cond: min / max as ints, -1 = absent
AlgoValue Val(int mn, int mx)
{
	AlgoValue v;
	v.min = Opt(mn);
	v.max = Opt(mx);
	return v;
}

std::string Num(int n) { return std::to_string(n); }

// 1 = match, 0 = no match, -1 = compile error, -2 = aborted
int RxRun(const std::string& pat, const std::string& text, RxFlags f = {})
{
	std::string err;
	std::optional<Rx> rx = RxCompile(pat, &err, f);
	if (!rx) return -1;
	RxStatus st = RxSearchEx(*rx, text);
	return st == RxStatus::Match ? 1 : st == RxStatus::NoMatch ? 0 : -2;
}

std::string Quote(const std::string& s)
{
	std::string out = "\"";
	for (char c : s) {
		if (c == '\n') out += "\\n";
		else if (c == '\r') out += "\\r";
		else if (c == '\t') out += "\\t";
		else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\x%02x", (unsigned char)c); out += b; }
		else out += c;
	}
	return out + "\"";
}

void MatcherTests()
{
	line("[R1] regex matcher (regex_match.h): syntax, case, UTF-8, errors, limits");
	struct Case { const char* pat; const char* text; int want; };
	const Case cases[] = {
		// literals, '.', alternation, groups
		{"abc", "xabcx", 1}, {"^abc$", "xabc", 0}, {"a.c", "abc", 1}, {"a.c", "a\nc", 0},
		{"a|b|c", "zzc", 1}, {"(a|ab)c", "abc", 1}, {"(?:x|y)+z", "xyxz", 1}, {"(?:x|y)+z", "z", 0},
		{"a||b", "", 1}, {"(?:)", "", 1},
		// quantifiers
		{"a{2}", "aa", 1}, {"^a{2}$", "aaa", 0}, {"^a{2,}$", "aaaa", 1}, {"^a{2,3}$", "aaaa", 0},
		{"^a{0}b$", "b", 1}, {"^a?b$", "b", 1}, {"^a*$", "", 1}, {"^a+$", "", 0},
		{"a*?b", "aaab", 1}, {"^(?:ab)?c$", "abc", 1}, {"^(?:a+){2,}$", "a", 0}, {"^(?:a+){2,}$", "aa", 1},
		{"^(?:a{0,2}){3}c$", "aaaaaac", 1}, {"^(?:a{0,2}){3}c$", "aaaaaaac", 0},
		// Annex B literals
		{"a{,3}", "a{,3}", 1}, {"a{", "a{", 1}, {"a{2", "a{2", 1}, {"]", "]", 1}, {"}", "}", 1},
		// classes
		{"[a-c]+", "zzb", 1}, {"^[^0-9]$", "5", 0}, {"^[^0-9]$", "x", 1}, {"[\\d-z]", "-", 1},
		{"[]", "a", 0}, {"[^]", "\n", 1}, {"[\\]]", "]", 1}, {"^[a\\-z]$", "-", 1}, {"^[a\\-z]$", "b", 0},
		{"[\\b]", "\b", 1}, {"^[\\s\\S]$", "\n", 1}, {"^[\\D]$", "5", 0}, {"^[^\\D]$", "5", 1},
		// escapes
		{"\\d+", "x12", 1}, {"^\\D$", "5", 0}, {"\\s", u8"a　b", 1}, {"\\s", u8"a b", 1},
		{"^\\S$", " ", 0}, {"^\\w+$", "a_9", 1}, {"\\W", "abc", 0}, {"\\x41", "A", 1}, {"\\u4e2d", u8"中", 1},
		{"\\+\\)\\.", "+).", 1}, {"\\t", "\t", 1}, {"^\\cA$", "\x01", 1}, {"\\c", "\\c", 1}, {"\\q", "q", 1},
		// word boundaries, lookahead
		{"\\bfoo\\b", "a foo b", 1}, {"\\bfoo\\b", "afoo", 0}, {"\\Bfoo", "xfoo", 1},
		{"(?=a)a", "a", 1}, {"(?!a).", "a", 0}, {"ab(?=c)", "abd", 0}, {"^(?:(?!b).)*$", "acd", 1},
		// the ES empty-iteration rule (these would loop forever without it)
		{"(a*)*b", "aaab", 1}, {"(a|)*c", "c", 1}, {"^(a*)*$", "aaa", 1},
		// case-insensitive (ASCII)
		{"FIRE", "fire", 1}, {"fire", "FIRE Damage", 1}, {"[A-Z]", "q", 1}, {"[^a]", "A", 0}, {"^[a-c]$", "B", 1},
		// UTF-8: one code point per '.'
		{"^.$", u8"中", 1}, {"^..$", u8"中", 0}, {u8"中.文", u8"中X文", 1}, {u8"[^中]", u8"中", 0},
		{u8"[:：]", u8"：", 1}, {u8"階級 *16）", u8"地圖（階級16）", 1}, {u8"階級 *16）", u8"地圖（階級 16）", 1},
		{u8"^(中|文)+$", u8"文中文", 1}, {u8"[一-龥]", u8"x字y", 1},
		// anchors
		{"^$", "", 1}, {"^$", "a", 0}, {"a$|^b", "ba", 1}, {"a$|^b", "ab", 0},
	};
	int bad = 0;
	std::string first;
	for (const Case& c : cases) {
		int got = RxRun(c.pat, c.text);
		if (got != c.want) {
			if (!bad) first = std::string(c.pat) + " vs " + Quote(c.text) + " -> " + std::to_string(got);
			bad++;
		}
	}
	check(bad == 0, std::to_string(sizeof cases / sizeof cases[0]) + " syntax cases" +
	      (bad ? " -- " + std::to_string(bad) + " wrong, first " + first : std::string()));

	{
		RxFlags cs;
		cs.icase = false;
		RxFlags dot;
		dot.dotAll = true;
		check(RxRun("A", "a", cs) == 0 && RxRun("A", "A", cs) == 1 && RxRun("[A-Z]", "q", cs) == 0,
		      "icase = false is case-sensitive");
		check(RxCanonicalize(U'a') == U'A' && RxCanonicalize(0x00B5) == 0x039C && RxCanonicalize(0x00FF) == 0x0178 &&
		      RxCanonicalize(0x00DF) == 0x00DF && RxCanonicalize(0x017F) == 0x017F && RxCanonicalize(0x212A) == 0x212A &&
		      RxCanonicalize(0x01C5) == 0x01C4 && RxCanonicalize(0x4E2D) == 0x4E2D,
		      "Canonicalize: a->A, micro->Greek Mu, y-diaeresis->U+0178; sharp s, long s, Kelvin unchanged; Dz titlecase->DZ");
		check(RxRun(u8"µ", u8"μ") == 1 && RxRun(u8"[ä]", u8"Ä") == 1 && RxRun(u8"É", u8"é") == 1 &&
		      RxRun(u8"[Ａ-Ｚ]", u8"ｑ") == 1 && RxRun("k", u8"K") == 0 && RxRun("s", u8"ſ") == 0,
		      "non-ASCII case folding follows JS (and k/s never meet Kelvin/long s)");
		check(RxRun("a.c", "a\nc", dot) == 1 && RxRun("a.c", "a\rc", dot) == 1 && RxRun("a.c", u8"a c") == 0,
		      "dotAll lets '.' cross line terminators; without it U+2028 is one too");
	}

	{
		const char* errs[] = {"x{2,1}", "*a", "a**", "[z-a]", "(a", "a)", "[abc", "\\", "a{2}{3}",
		                      "^*", "$+", "\\b+", "?", "(?<=a)b", "(?<!a)b", "\\1", "a\\02", "(?x)", "\xff"};
		int ok = 0;
		std::string leak;
		for (const char* p : errs) {
			std::string err;
			if (!RxCompile(p, &err) && !err.empty()) ok++;
			else if (leak.empty()) leak = p;
		}
		check(ok == (int)(sizeof errs / sizeof errs[0]),
		      "malformed / unsupported patterns are rejected with a message (" + std::to_string(ok) + ")" +
		      (leak.empty() ? std::string() : " -- accepted " + Quote(leak)));
		std::string err;
		RxCompile("a{2,1}", &err);
		line("    e.g. a{2,1}: " + err);
	}

	{
		// Exponential backtracking: JS would hang here; we must give up and say so.
		Rx rx = *RxCompile("(a+)+b", nullptr);
		uint64_t steps = 0;
		RxStatus st = RxSearchEx(rx, std::string(28, 'a') + "c", RxLimits{}, &steps);
		check(st == RxStatus::Aborted, "catastrophic (a+)+b gives up at the step limit (" +
		      std::to_string(steps) + " steps) instead of answering");
		check(!RxSearch(rx, std::string(28, 'a') + "c"), "and RxSearch reports that as no match");
		check(RxSearch(rx, "aaab"), "the same pattern still matches normally");

		// Deep group repetition: must stop at the depth limit, not overflow the stack.
		Rx deep = *RxCompile("^(?:ab)*$", nullptr);
		std::string longText;
		for (int i = 0; i < 5000; i++) longText += "ab";
		check(RxSearchEx(deep, longText) == RxStatus::Aborted, "5000 group iterations hit the depth limit cleanly");
		check(RxSearchEx(deep, "abababab") == RxStatus::Match, "a short one matches");
		// .* uses the single-character fast path: long text, no depth.
		Rx dotStar = *RxCompile(u8"物品數量.*%", nullptr);
		check(RxSearch(dotStar, u8"物品數量" + std::string(20000, 'x') + "%"), ".* over 20000 characters");

		Rx frag = *RxCompile(*RegexFrag::StrictPropertyFragment(u8"物品數量", Val(80, -1), 3, true), nullptr);
		steps = 0;
		RxSearchEx(frag, u8"地圖（階級 16）\n物品數量: +86% (augmented)", RxLimits{}, &steps);
		line("    a strict fragment over a two-line text: " + std::to_string(steps) + " steps");
		check(steps < 2000, "real fragments stay far below the step limit");
	}
}

// Golden fixture from exile-appraiser (regex_port_golden.inc): every function's
// output must be byte-identical to the TypeScript, and the matcher must agree
// with node's RegExp on every (pattern, flags, text).
void GoldenTests()
{
	line("[R2-golden] exile-appraiser outputs, byte for byte (regex_port_golden.inc)");
	using nlohmann::json;
	struct Tally { int n = 0, bad = 0; std::vector<std::string> first; };
	std::vector<std::pair<std::string, Tally>> tallies;
	auto tally = [&](const std::string& k) -> Tally& {
		for (auto& t : tallies) if (t.first == k) return t.second;
		tallies.emplace_back(k, Tally{});
		return tallies.back().second;
	};
	auto num = [](const json& j) { return j.is_null() ? std::optional<double>() : std::optional<double>(j.get<double>()); };
	auto optStr = [](const std::optional<std::string>& s) { return s ? json(*s) : json(nullptr); };
	std::vector<std::string> rxTexts;
	int parseErr = 0;
	for (const char* rec : kRegexPortGolden) {
		json r;
		try {
			r = json::parse(rec);
		} catch (...) {
			parseErr++;
			continue;
		}
		const std::string kind = r[0].get<std::string>();
		if (kind == "rxTexts") {
			rxTexts = r[1].get<std::vector<std::string>>();
			continue;
		}
		json got, want = r.back();
		if (kind == "range") {
			got = RegexNumeric::RangeRegex(NumRange{num(r[1]), num(r[2])}, r[3].get<int>());
		} else if (kind == "readable") {
			got = RegexNumeric::ReadableRangeRegex(NumRange{num(r[1]), num(r[2])}, r[3].get<int>(), r[4].get<bool>());
		} else if (kind == "labelBase") {
			got = RegexFrag::LabelBase(r[1].get<std::string>());
		} else if (kind == "prop") {
			AlgoValue v{num(r[2]), num(r[3]), ""};
			got = optStr(RegexFrag::PropertyFragment(r[1].get<std::string>(), v, r[4].get<int>(), r[5].get<bool>(), r[6].get<bool>()));
		} else if (kind == "strict") {
			AlgoValue v{num(r[2]), num(r[3]), ""};
			got = optStr(RegexFrag::StrictPropertyFragment(r[1].get<std::string>(), v, r[4].get<int>(), r[5].get<bool>()));
		} else if (kind == "tier") {
			AlgoValue v{num(r[1]), num(r[2]), ""};
			got = optStr(RegexFrag::MapTierFragment(v, r[3].get<int>(),
			             r[4].get<std::string>() == "zh" ? RegexFrag::Lang::Zh : RegexFrag::Lang::En));
		} else if (kind == "rarity") {
			got = optStr(RegexFrag::RarityFragment(r[1].get<std::string>(), r[2].get<std::string>()));
		} else if (kind == "whole") {
			got = optStr(RegexFrag::WholeLine(r[1].get<std::vector<std::string>>()));
		} else if (kind == "linked") {
			got = optStr(RegexFrag::LinkedSockets(r[1].get<int>()));
		} else if (kind == "colors") {
			got = optStr(RegexFrag::LinkColors(r[1].get<std::string>()));
		} else if (kind == "count") {
			got = optStr(RegexFrag::SocketColorCount(r[1].get<std::string>(), r[2].get<std::string>(), r[3].get<int>()));
		} else if (kind == "tierLine") {
			got = RegexFrag::IsTierNameLine(r[1].get<std::string>());
		} else if (kind == "normLabel") {
			got = RegexNormalizeLabel(r[1].get<std::string>());
		} else if (kind == "rx") {
			RxFlags f;
			const std::string flags = r[2].get<std::string>();
			f.icase = flags.find('i') != std::string::npos;
			f.dotAll = flags.find('s') != std::string::npos;
			std::string err;
			std::optional<Rx> rx = RxCompile(r[1].get<std::string>(), &err, f);
			if (!rx) {
				got = "error";
			} else {
				std::string bits;
				for (const std::string& t : rxTexts) {
					RxStatus st = RxSearchEx(*rx, t);
					bits += st == RxStatus::Match ? '1' : st == RxStatus::NoMatch ? '0' : 'A';
				}
				got = bits;
			}
		} else {
			got = "unknown kind";
		}
		Tally& t = tally(kind);
		t.n++;
		if (got != want) {
			t.bad++;
			if (t.first.size() < 5) t.first.push_back(r.dump() + " -> got " + got.dump());
		}
	}
	check(parseErr == 0 && !tallies.empty(), "fixture parses (" + std::to_string(sizeof kRegexPortGolden / sizeof kRegexPortGolden[0]) +
	      " lines, " + std::to_string(parseErr) + " unparsable)");
	for (const auto& kt : tallies) {
		const Tally& t = kt.second;
		check(t.bad == 0, kt.first + ": " + std::to_string(t.n - t.bad) + "/" + std::to_string(t.n) + " identical");
		for (const std::string& f : t.first) line("    " + f);
	}
}

// numeric.test.ts: every fragment, matched as a whole against each number, must
// agree with the range definition -- checked with the R1 matcher.
struct NumberTexts {
	std::vector<std::u32string> n;   // "0" .. "9999"
	NumberTexts()
	{
		for (int i = 0; i < 10000; i++) {
			std::u32string s;
			for (char c : std::to_string(i)) s += (char32_t)c;
			n.push_back(std::move(s));
		}
	}
};

std::optional<Rx> WholeRx(const std::string& src)
{
	return RxCompile("^(?:" + src + ")$", nullptr);
}

void NumericTests()
{
	using RegexNumeric::RangeRegex;
	using RegexNumeric::ReadableRangeRegex;
	using RegexNumeric::NaiveRangeRegex;
	static const NumberTexts T;
	line("[R2-numeric] rangeRegex / readableRangeRegex value by value (numeric.test.ts)");

	check(RangeRegex({16, std::nullopt}, 2) == "(1[6-9]|[2-9]\\d)", ">=16 (2 digits) = (1[6-9]|[2-9]\\d)");
	check(RangeRegex({std::nullopt, 5}, 1) == "[0-5]", "<=5 (1 digit) = [0-5]");
	check(RangeRegex({0, 50}, 3) == "([1-4]?\\d|50)", "0-50 (3 digits) merges with ?");
	check(RangeRegex({80, std::nullopt}, 3) == "([89]\\d|\\d\\d\\d)", ">=80 (3 digits)");
	check(RangeRegex({50, 10}, 2).empty() && RangeRegex({100, std::nullopt}, 2).empty(), "an empty set is \"\"");

	// One range against 0..999 (rangeRegex) -- returns the first disagreeing N or -1.
	auto mismatch = [&](const NumRange& r, int digits, std::string& src) -> int {
		src = RangeRegex(r, digits);
		std::optional<Rx> rx = WholeRx(src);
		if (!rx) return -2;
		const double top = RegexNumeric::DomainMax(digits);
		for (int n = 0; n <= 999; n++) {
			const bool want = n <= top && n >= (r.min ? *r.min : 0) && n <= (r.max ? *r.max : top);
			if ((RxSearchCps(*rx, T.n[n]) == RxStatus::Match) != want) return n;
		}
		if (src.size() > NaiveRangeRegex(r, digits).size()) return -3;
		return -1;
	};
	struct Group { int count = 0, bad = 0; std::string first; };
	auto run = [&](Group& g, const NumRange& r, int digits) {
		std::string src;
		int m = mismatch(r, digits, src);
		g.count++;
		if (m != -1) {
			if (!g.bad) {
				g.first = "min=" + (r.min ? Num((int)*r.min) : std::string("-")) + " max=" +
				          (r.max ? Num((int)*r.max) : std::string("-")) + " digits=" + Num(digits) + " src=" + src +
				          (m == -2 ? " (does not compile)" : m == -3 ? " (longer than naive)" : " N=" + Num(m));
			}
			g.bad++;
		}
	};
	auto report = [&](const Group& g, const std::string& what) {
		check(g.bad == 0, what + ": " + Num(g.count) + " ranges x 0-999" + (g.bad ? " -- " + Num(g.bad) + " wrong, first " + g.first : std::string()));
	};
	for (int digits = 1; digits <= 3; digits++) {
		Group g;
		const int top = (int)RegexNumeric::DomainMax(digits);
		for (int n = 0; n <= top; n++) {
			run(g, {(double)n, std::nullopt}, digits);
			run(g, {std::nullopt, (double)n}, digits);
		}
		report(g, "digits=" + Num(digits) + " >=N and <=N");
	}
	{
		Group g;
		for (int a = 0; a <= 99; a++)
			for (int b = a; b <= 99; b++) run(g, {(double)a, (double)b}, 2);
		report(g, "digits=2 every interval (5050)");
	}
	// numeric.test.ts:60-73: 15 fixed + 3000 LCG intervals (seed 12345).
	auto lcg = [](unsigned seed) {
		return [seed]() mutable { seed = seed * 1103515245u + 12345u; return (int)(seed % 1000u); };
	};
	{
		Group g;
		const int fixed[][2] = {{0, 0}, {0, 999}, {1, 100}, {60, 86}, {68, 83}, {75, 100}, {80, 120}, {100, 999},
		                        {83, 86}, {50, 150}, {99, 101}, {9, 10}, {199, 200}, {0, 9}, {10, 99}};
		for (const auto& f : fixed) run(g, {(double)f[0], (double)f[1]}, 3);
		auto rnd = lcg(12345);
		for (int i = 0; i < 3000; i++) {
			int a = rnd(), b = rnd();
			run(g, {(double)(std::min)(a, b), (double)(std::max)(a, b)}, 3);
		}
		report(g, "digits=3 fixed + 3000 LCG intervals (seed 12345)");
	}
	{
		int bad = 0;
		for (int n = 0; n <= 999; n += 7) {
			const NumRange rs[3] = {{(double)n, std::nullopt}, {std::nullopt, (double)n}, {(double)n, (double)(std::min)(999, n + 37)}};
			for (const NumRange& r : rs) {
				const std::string s = RangeRegex(r, 3);
				for (char c : s)
					if (!(isdigit((unsigned char)c) || c == 'd' || c == '\\' || c == '[' || c == ']' || c == '-' ||
					      c == '?' || c == '|' || c == '(' || c == ')')) bad++;
			}
		}
		check(bad == 0, "rangeRegex uses only \\d [] ? | () and digits (no {n}, no upper-case escapes)");
	}

	// ---- readable (numeric.test.ts:116-185): 0..9999
	check(ReadableRangeRegex({30, std::nullopt}, 3, true) == "([3-9][0-9]|[1-9][0-9]{2,})", "readable >=30 open");
	check(ReadableRangeRegex({5, std::nullopt}, 3, true) == "([5-9]|[1-9][0-9]{1,})", "readable >=5 open");
	check(ReadableRangeRegex({100, std::nullopt}, 3, true) == "[1-9][0-9]{2,}" &&
	      ReadableRangeRegex({10, std::nullopt}, 3, true) == "[1-9][0-9]{1,}", "readable >=100 / >=10 open");
	check(ReadableRangeRegex({16, std::nullopt}, 2) == "(1[6-9]|[2-9][0-9])", "readable >=16 bounded");
	check(ReadableRangeRegex({std::nullopt, 50}, 3, true) == "([1-4]?[0-9]|50)" &&
	      ReadableRangeRegex({60, 150}, 3, true) == "([6-9][0-9]|1[0-4][0-9]|150)", "readable <=50 / 60-150");
	check(ReadableRangeRegex({50, 10}, 3, true).empty() && ReadableRangeRegex({100, std::nullopt}, 2).empty(),
	      "readable empty set is \"\"");
	check(RangeRegex({80, std::nullopt}, 3) == "([89]\\d|\\d\\d\\d)", "the shortest form is unaffected");

	auto runReadable = [&](Group& g, const NumRange& r, int digits, bool open) {
		const std::string s = ReadableRangeRegex(r, digits, open);
		g.count++;
		std::string why;
		std::optional<Rx> rx = WholeRx(s);
		const bool openTop = open && !r.max;
		if (!rx) {
			why = "does not compile";
		} else {
			const double top = openTop ? 1e300 : RegexNumeric::DomainMax(digits);
			for (int n = 0; n <= 9999 && why.empty(); n++) {
				const bool want = n <= top && n >= (r.min ? *r.min : 0) && n <= (r.max ? *r.max : top);
				if ((RxSearchCps(*rx, T.n[n]) == RxStatus::Match) != want) why = "N=" + Num(n);
			}
			if (why.empty() && s.find('\\') != std::string::npos) why = "contains a backslash";
			if (why.empty() && !openTop && s.find_first_of("{}") != std::string::npos) why = "{} without an open top";
			for (int n = 1; n <= 999 && why.empty(); n += 13)
				if (RxSearch(*rx, "0" + Num(n))) why = "matches leading zero 0" + Num(n);
		}
		if (!why.empty()) {
			if (!g.bad) g.first = "min=" + (r.min ? Num((int)*r.min) : std::string("-")) + " max=" +
			                      (r.max ? Num((int)*r.max) : std::string("-")) + " src=" + s + " " + why;
			g.bad++;
		}
	};
	auto reportR = [&](const Group& g, const std::string& what) {
		check(g.bad == 0, what + ": " + Num(g.count) + " ranges x 0-9999" + (g.bad ? " -- " + Num(g.bad) + " wrong, first " + g.first : std::string()));
	};
	{
		Group g;
		for (int n = 0; n <= 999; n++) runReadable(g, {(double)n, std::nullopt}, 3, true);
		reportR(g, "readable open >=N");
	}
	{
		Group g;
		for (int n = 0; n <= 999; n++) runReadable(g, {std::nullopt, (double)n}, 3, true);
		auto rnd = lcg(777);
		for (int i = 0; i < 3000; i++) {
			int a = rnd(), b = rnd();
			runReadable(g, {(double)(std::min)(a, b), (double)(std::max)(a, b)}, 3, true);
		}
		reportR(g, "readable open <=N + 3000 LCG intervals (seed 777)");
	}
	{
		Group g;
		for (int digits = 1; digits <= 2; digits++) {
			for (int n = 0; n <= (int)RegexNumeric::DomainMax(digits); n++) {
				runReadable(g, {(double)n, std::nullopt}, digits, false);
				runReadable(g, {std::nullopt, (double)n}, digits, false);
			}
		}
		for (int a = 0; a <= 99; a++)
			for (int b = a; b <= 99; b++) runReadable(g, {(double)a, (double)b}, 2, false);
		reportR(g, "readable bounded digits 1/2 >=N, <=N and every 2-digit interval");
	}
}

// pages.test.ts + strict-fragments.test.ts, the parts that need no
// exile-appraiser fixture files; the corpus checks run on our own catalogue.
void FragmentTests(const std::wstring& exeDir)
{
	using namespace RegexFrag;
	line("[R2-frag] fragment builders value by value (pages.test.ts, strict-fragments.test.ts)");
	RxFlags dotAll;
	dotAll.dotAll = true;
	auto comp = [](const std::string& s, RxFlags f = {}) { return *RxCompile(s, nullptr, f); };

	struct Cond { int mn, mx; };
	auto ok = [](const Cond& c, int n) { return (c.mn < 0 || n >= c.mn) && (c.mx < 0 || n <= c.mx); };

	// -- propertyFragment (pages.test.ts:41-99)
	{
		const Cond conds[] = {{80, -1}, {7, -1}, {150, -1}, {-1, 50}, {-1, 5}, {60, 86}, {100, 120}};
		int bad = 0;
		std::string first;
		for (const Cond& c : conds) {
			const std::string f = *PropertyFragment(u8"物品數量", Val(c.mn, c.mx), 3, true);
			Rx r = comp(f);
			for (int n = 0; n <= 999; n++) {
				const std::string v = Num(n);
				const std::string ls[] = {u8"物品數量: +" + v + "%", u8"物品數量：+" + v + "%", u8"物品數量 +" + v + "%",
				                          u8"物品數量: +" + v + "% (augmented)"};
				for (const std::string& l : ls)
					if (RxSearch(r, l) != ok(c, n)) { if (!bad++) first = f + " x " + l; }
			}
			const std::string g = *PropertyFragment(u8"物品等級：#", Val(c.mn, c.mx), 3, false);
			Rx rg = comp(g);
			for (int n = 0; n <= 999; n++) {
				const std::string v = Num(n);
				const std::string ls[] = {u8"物品等級: " + v, u8"物品等級：" + v, u8"物品等級 " + v};
				for (const std::string& l : ls)
					if (RxSearch(rg, l) != ok(c, n)) { if (!bad++) first = g + " x " + l; }
			}
		}
		check(bad == 0, "propertyFragment: percent and plain, 7 conditions x 0-999 x separators" + (bad ? " -- " + first : std::string()));
		const std::string f = *PropertyFragment("Item Level #", Val(84, -1), 3, false);
		check(f == "Item Level.*[^\\d](8[4-9]|9\\d|\\d\\d\\d)" && RxRun(f, "item level: 86") == 1 &&
		      RxRun(f, "Item Level: 83") == 0, "English label, case-insensitive: " + f);
		const std::string gem = *PropertyFragment(u8"等級", Val(20, -1), 2, false, true);
		check(RxRun(gem, u8"等級: 20 (最高)") == 1 && RxRun(gem, u8"等級: 19") == 0 &&
		      RxRun(gem, u8"物品等級：84") == 0 && RxRun(gem, u8"需求 等級 70") == 0, "gem level anchored at line start: " + gem);
		int esc = 0;
		for (const Cond& c : conds)
			for (bool pct : {true, false}) {
				const std::string s = *PropertyFragment("X", Val(c.mn, c.mx), 3, pct);
				if (s.find("\\D") != std::string::npos || s.find("\\W") != std::string::npos ||
				    s.find("\\S") != std::string::npos || s.find_first_of("{}") != std::string::npos) esc++;
			}
		check(esc == 0, "no upper-case escapes and no {n}");
		check(!PropertyFragment("X", AlgoValue{}, 3, true) && !PropertyFragment("X", Val(90, 10), 3, true),
		      "an input that describes nothing is null");
	}

	// -- sockets (pages.test.ts:101-145)
	{
		const std::string choices[] = {"rgb", "rrg", "gg", "rgbb", "rrggbb"};
		int bad = 0, n = 0;
		std::string first;
		for (const std::string& choice : choices) {
			const std::string f = *LinkColors(choice);
			Rx r = comp(f);
			std::string want = choice;
			std::sort(want.begin(), want.end());
			for (int len = 2; len <= 6; len++) {
				int total = 1;
				for (int i = 0; i < len; i++) total *= 3;
				for (int code = 0; code < total; code++) {
					std::string s;
					for (int i = 0, x = code; i < len; i++, x /= 3) s += "rgb"[x % 3];
					std::string text = "Sockets: ";
					for (int i = 0; i < len; i++) {
						if (i) text += '-';
						text += (char)toupper((unsigned char)s[i]);
					}
					bool expect = false;
					for (size_t i = 0; i + choice.size() <= s.size(); i++) {
						std::string w = s.substr(i, choice.size());
						std::sort(w.begin(), w.end());
						if (w == want) expect = true;
					}
					n++;
					if (RxSearch(r, text) != expect) { if (!bad++) first = f + " x " + text; }
				}
			}
		}
		check(bad == 0, "linkColors: 5 choices x every R/G/B link of length 2-6 (" + Num(n) + ")" + (bad ? " -- " + first : std::string()));
		check(*LinkColors("rgb") == "b-(g-r|r-g)|g-(b-r|r-b)|r-(b-g|g-b)", "linkColors(rgb) = " + *LinkColors("rgb"));
		check(RxRun(*LinkColors("rgb"), "Sockets: R-G B") == 0, "an unlinked socket breaks the chain");
		const std::string six = *LinkedSockets(6), five = *LinkedSockets(5);
		check(RxRun(six, u8"插槽: R-G-B-R-G-B") == 1 && RxRun(six, u8"插槽: R-G-B R-G-B") == 0 &&
		      RxRun(six, u8"插槽: R-G-B-R-G") == 0 && RxRun(five, u8"插槽: R-G-B-R-G B") == 1, "linkedSockets 6L / 5L");
		const std::string blue = *SocketColorCount(u8"插槽", "b", 3);
		check(RxRun(blue, u8"插槽: B-B R-B") == 1 && RxRun(blue, u8"插槽: B-B-R-G") == 0, "socketColorCount >= 3 blue: " + blue);
		check(*WholeLine({u8"已汙染"}) == u8"^已汙染$" && RxRun(*WholeLine({u8"塑者之物", u8"尊師之物"}), u8"尊師之物") == 1 &&
		      RxRun(*WholeLine({u8"塑者之物", u8"尊師之物"}), u8"塑者之物地圖") == 0, "wholeLine");
	}

	// -- strictPropertyFragment (strict-fragments.test.ts ①)
	const Cond strictConds[] = {{80, -1}, {30, -1}, {7, -1}, {100, -1}, {150, -1}, {0, -1}, {-1, 50},
	                            {-1, 5}, {-1, 0}, {60, 86}, {100, 120}, {9, 10}};
	{
		int bad = 0;
		std::string first;
		for (const Cond& c : strictConds) {
			const std::string f = *StrictPropertyFragment(u8"物品數量", Val(c.mn, c.mx), 3, true);
			Rx r = comp(f);
			for (int n = 0; n <= 999; n++) {
				const std::string v = Num(n), lab = u8"物品數量";
				const std::string ls[] = {lab + ": +" + v + "%", lab + ": +" + v + "% (augmented)", lab + u8"：+" + v + "%",
				                          lab + ":+" + v + "%", lab + u8"： +" + v + "%", lab + ": " + v + "%",
				                          lab + ":" + v + "%", lab + u8"：" + v + "%", lab + ": +" + v + " %"};
				for (const std::string& l : ls)
					if (RxSearch(r, l) != ok(c, n)) { if (!bad++) first = f + " x " + l; }
			}
			for (int n : {1000, 1234, 99999})
				if (RxSearch(r, u8"物品數量: +" + Num(n) + "%") != (c.mx < 0 || n <= c.mx)) { if (!bad++) first = f + " x " + Num(n); }
		}
		check(bad == 0, "strict percent: 12 conditions x 0-999 x 9 separators, open top above 999" + (bad ? " -- " + first : std::string()));
		Rx any = comp(*StrictPropertyFragment(u8"物品數量", Val(0, -1), 3, true));
		Rx le = comp(*StrictPropertyFragment(u8"物品數量", Val(-1, 50), 3, true));
		check(!RxSearch(any, u8"物品數量 +80%") && !RxSearch(any, u8"物品數量: -80%") &&
		      !RxSearch(any, u8"地圖掉落物品數量增加 80%") && !RxSearch(le, u8"物品數量: +150%"),
		      "no colon / negative / another label / a bigger number: no match");
		bad = 0;
		for (const Cond& c : strictConds) {
			const std::string f = *StrictPropertyFragment(u8"物品等級", Val(c.mn, c.mx), 3, false);
			Rx r = comp(f);
			for (int n = 0; n <= 999; n++) {
				const std::string v = Num(n);
				const std::string ls[] = {u8"物品等級: " + v, u8"物品等級：" + v, u8"物品等級 " + v, u8"物品等級:" + v + " (x)"};
				for (const std::string& l : ls)
					if (RxSearch(r, l) != ok(c, n)) { if (!bad++) first = f + " x " + l; }
			}
		}
		check(bad == 0, "strict plain: [:：]? and the <= / range tail" + (bad ? " -- " + first : std::string()));
	}

	// -- cross-line negatives (②): the strict form never reaches into the next line
	{
		const char* labs[] = {u8"物品數量", u8"物品稀有度", u8"怪群大小", "Item Quantity", "Item Rarity", "Monster Pack Size"};
		int bad = 0, oldHits = 0;
		std::string first;
		for (const char* lab : labs) {
			for (const Cond& c : strictConds) {
				const std::string f = *StrictPropertyFragment(lab, Val(c.mn, c.mx), 3, true);
				const std::string old = *PropertyFragment(lab, Val(c.mn, c.mx), 3, true);
				int badN = -1, goodN = -1;
				for (int n : {0, 5, 49, 51, 79, 99, 121, 149, 999}) if (!ok(c, n)) { badN = n; break; }
				for (int n : {999, 120, 86, 80, 60, 50, 10, 5, 0}) if (ok(c, n)) { goodN = n; break; }
				if (badN < 0 || goodN < 0) continue;
				const std::string L = lab;
				const std::string texts[] = {
					L + ": +" + Num(badN) + u8"% (augmented)\n其他屬性: +" + Num(goodN) + "% (augmented)",
					L + ": +" + Num(badN) + "% (augmented)\n+" + Num(goodN) + "%",
					L + ": +" + Num(badN) + "%\n" + Num(goodN) + "%"};
				Rx r = comp(f), rd = comp(f, dotAll), od = comp(old, dotAll);
				for (const std::string& t : texts) {
					if (RxSearch(r, t) || RxSearch(rd, t)) { if (!bad++) first = f + " x " + Quote(t); }
					if (RxSearch(od, t)) oldHits++;
				}
			}
		}
		check(bad == 0, "strict: a failing value on the label line never borrows the next line's number" + (bad ? " -- " + first : std::string()));
		check(oldHits > 0, "while the old .* form does, with '.' crossing lines (" + Num(oldHits) + " hits) -- the negatives mean something");
		const std::string t = *MapTierFragment(Val(16, -1), 2, Lang::Zh);
		const std::string text = u8"地圖（階級 14）\n--------\n怪物等級: 16）";
		check(RxRun(t, text) == 0 && RxRun(t, text, dotAll) == 0, "tier: the name's number decides, not the next line's");
	}

	// -- rarity (③), synthetic part
	struct Opt2 { const char* zh; const char* en; };
	const Opt2 rarities[] = {{u8"普通", "Normal"}, {u8"魔法", "Magic"}, {u8"稀有", "Rare"}, {u8"傳奇", "Unique"}};
	std::vector<std::pair<Rx, std::string>> rarityRx;
	{
		int bad = 0;
		std::string first;
		for (int lang = 0; lang < 2; lang++) {
			const std::string label = lang == 0 ? u8"稀有度" : "Rarity";
			const std::string itemR = lang == 0 ? u8"物品稀有度" : "Item Rarity";
			const std::string monR = lang == 0 ? u8"怪物稀有度" : "Monster Rarity";
			for (const Opt2& o : rarities) {
				const std::string f = *RarityFragment(label, lang == 0 ? o.zh : o.en);
				Rx r = comp(f);
				rarityRx.emplace_back(r, f);
				for (const Opt2& other : rarities)
					for (const char* sep : {": ", u8"：", ":", u8"： "}) {
						const std::string l = label + sep + (lang == 0 ? other.zh : other.en);
						if (RxSearch(r, l) != (&other == &o)) { if (!bad++) first = f + " x " + l; }
					}
				for (int n = 0; n <= 999; n++)
					for (const std::string& lab : {itemR, monR}) {
						const std::string v = Num(n);
						const std::string ls[] = {lab + ": +" + v + "%", lab + u8"：+" + v + "%", lab + ": " + v + "% (augmented)"};
						for (const std::string& l : ls)
							if (RxSearch(r, l)) { if (!bad++) first = f + " hits " + l; }
					}
			}
		}
		check(bad == 0, "rarity: 4 options x separators hit only their own value; item / monster rarity 0-999 never" + (bad ? " -- " + first : std::string()));
		check(!RarityFragment(u8"稀有度", "") && !RarityFragment("", u8"稀有"), "rarityFragment: empty input is null");
	}

	// -- tier names (④), synthetic part
	{
		int bad = 0;
		std::string first;
		for (const Cond& c : strictConds) {
			const std::optional<std::string> fz = MapTierFragment(Val(c.mn, c.mx), 2, Lang::Zh);
			const std::optional<std::string> fe = MapTierFragment(Val(c.mn, c.mx), 2, Lang::En);
			if (!fz || !fe) {
				// No 2-digit tier satisfies it (>= 100 ...): null is the right
				// answer exactly when no n in 0-99 would pass. (The TS test turns
				// null into /null/ here, which happens to match nothing.)
				for (int n = 0; n <= 99; n++)
					if (ok(c, n) || fz || fe) { if (!bad++) first = "null fragment for a satisfiable tier " + Num(n); }
				continue;
			}
			Rx zh = comp(*fz);
			Rx en = comp(*fe);
			for (int n = 0; n <= 99; n++) {
				const std::string v = Num(n);
				for (const std::string& t : {u8"地圖（階級 " + v + u8"）", u8"凋落的 地圖（階級 " + v + u8"）",
				                             u8"換界石（階級 " + v + u8"）", u8"堅定的地圖（階級" + v + u8"）"})
					if (RxSearch(zh, t) != ok(c, n)) { if (!bad++) first = t; }
				for (const std::string& t : {"Map (Tier " + v + ")", "Blighted Map (Tier " + v + ")", "Waystone (Tier " + v + ")"})
					if (RxSearch(en, t) != ok(c, n)) { if (!bad++) first = t; }
			}
		}
		check(bad == 0, "map tier: 12 conditions x names 0-99, both languages" + (bad ? " -- " + first : std::string()));
		check(IsTierNameLine(u8"地圖（階級#）") && IsTierNameLine(u8"地圖（階級 #）") && IsTierNameLine("map (tier #)") &&
		      !IsTierNameLine("Map (Tier 16)") && !IsTierNameLine(u8"{ 前綴 \"x\" (階級: 1) }"), "isTierNameLine");
	}

	// -- ③④ against every corpus line we ship (both games, both languages,
	// hidden and ambient included, '#' replaced by sample values)
	{
		RegexDataset ds;
		std::string err;
		if (!ds.Load(exeDir, L"poe1", &err)) {
			check(false, "load the catalogue for the corpus checks: " + err);
			return;
		}
		std::vector<std::string> lines;
		{
			std::vector<std::string> raw;
			for (const RegexPageDef& p : ds.Pages()) {
				if (p.kind != RegexPageKind::Mods && p.kind != RegexPageKind::Names) continue;
				for (const auto* v : {&p.ambientZh, &p.ambientEn}) raw.insert(raw.end(), v->begin(), v->end());
				for (const RegexEntryDef& e : p.entries)
					for (const auto* v : {&e.zh, &e.en, &e.hiddenZh, &e.hiddenEn}) raw.insert(raw.end(), v->begin(), v->end());
			}
			std::vector<std::string> out;
			const char* vals[] = {"1", "5", "14", "16", "20", "30", "50", "80", "100", "150"};
			for (const std::string& l : raw) {
				if (l.find('#') == std::string::npos) { out.push_back(l); continue; }
				for (const char* v : vals) {
					std::string s;
					for (char ch : l) { if (ch == '#') s += v; else s += ch; }
					out.push_back(std::move(s));
				}
			}
			std::sort(out.begin(), out.end());
			out.erase(std::unique(out.begin(), out.end()), out.end());
			lines = std::move(out);
		}
		check(lines.size() > 1000, "corpus lines with sample values: " + Num((int)lines.size()));
		int hits = 0;
		std::string first;
		for (const auto& rr : rarityRx)
			for (const std::string& l : lines)
				if (RxSearch(rr.first, l)) { if (!hits++) first = rr.second + " hits " + l; }
		check(hits == 0, "no rarity fragment hits any corpus line" + (hits ? " -- " + first : std::string()));

		RxFlags cs;
		cs.icase = false;
		Rx isName = *RxCompile(u8"（階級 *\\d+）|\\(Tier \\d+\\)", nullptr, cs);
		std::vector<std::string> names, rest;
		for (const std::string& l : lines) (RxSearch(isName, l) ? names : rest).push_back(l);
		check(std::find(names.begin(), names.end(), u8"地圖（階級16）") != names.end() &&
		      std::find(names.begin(), names.end(), "Map (Tier 16)") != names.end(),
		      "the corpus holds map names with their tier (" + Num((int)names.size()) + " lines) -- a correct hit");
		rest.push_back(u8"{ 前綴 \"x\" (階級: 1) }");
		rest.push_back("{ Prefix Modifier \"Shocking\" (Tier: 1) }");
		rest.push_back(u8"換界石階級: 16");
		hits = 0;
		for (const AlgoValue& v : {Val(1, -1), Val(-1, 99)})
			for (Lang lang : {Lang::Zh, Lang::En}) {
				const std::string f = *MapTierFragment(v, 2, lang);
				Rx r = comp(f);
				for (const std::string& l : rest)
					if (RxSearch(r, l)) { if (!hits++) first = f + " hits " + l; }
			}
		check(hits == 0, "tier fragments hit no other corpus line, nor the advanced-mod \"(階級: 1)\"" + (hits ? " -- " + first : std::string()));
	}
}

// R1 item 2: schema 2 `labels` and `kind` (exile-appraiser data.ts), and a
// schema-1 file still loading the way it always did.
void SchemaTests(const std::wstring& exeDir)
{
	line("[R1-data] catalogue schema 2: labels{zh,en} and page kind");
	RegexDataset ds;
	std::string err;
	if (!ds.Load(exeDir, L"poe1", &err)) {
		check(false, "load: " + err);
		return;
	}
	for (const char* game : {"poe1", "poe2"}) {
		const RegexLabels* lab = ds.Labels(game);
		check(lab && lab->present && lab->zh.size() >= 10 && lab->zh.size() == lab->en.size(),
		      std::string(game) + ": labels present, zh " + Num(lab ? (int)lab->zh.size() : -1) + " / en " +
		      Num(lab ? (int)lab->en.size() : -1));
		int raw = 0;
		if (lab)
			for (const auto* side : {&lab->zh, &lab->en})
				for (const auto& kv : *side)
					if (kv.second.find('[') != std::string::npos || kv.second.find('{') != std::string::npos || kv.second.empty()) raw++;
		check(raw == 0, std::string(game) + ": no label keeps [..] or {n} markup after normalizing");
		const std::string* tier = lab ? lab->Find(true, "ItemDisplayMapTier") : nullptr;
		check(tier && !tier->empty(), std::string(game) + ": ItemDisplayMapTier (zh) = " + (tier ? *tier : std::string("<missing>")));
	}
	int mods = 0, names = 0, other = 0, noEn = 0, groupMismatch = 0;
	for (const RegexPageDef& p : ds.Pages()) {
		if (p.kind == RegexPageKind::Mods) mods++;
		else if (p.kind == RegexPageKind::Names) names++;
		else other++;
		if (p.titleEn.empty()) noEn++;
		if (!p.groupsEn.empty() && p.groupsEn.size() != p.groups.size()) groupMismatch++;
	}
	check(other == 0 && mods > 0 && names > 0, "shipped pages are mods (" + Num(mods) + ") or names (" + Num(names) + "), never algorithmic");
	check(noEn == 0 && groupMismatch == 0, "every page has titleEn and groupsEn matches groups");
	check(RegexPageKindFrom("names") == RegexPageKind::Names && RegexPageKindFrom("numeric") == RegexPageKind::Numeric &&
	      RegexPageKindFrom("sockets") == RegexPageKind::Sockets && RegexPageKindFrom("future") == RegexPageKind::Mods &&
	      RegexPageKindFrom("") == RegexPageKind::Mods, "kind: known names map, anything else reads as mods");

	// A schema-1 file (no kind, no labels, no titleEn) in a scratch install.
	wchar_t tmp[MAX_PATH] = {};
	if (!GetTempPathW(MAX_PATH, tmp)) {
		check(false, "no temp directory for the schema-1 check");
		return;
	}
	const std::wstring dir = std::wstring(tmp) + L"pobtools_regex_selftest_schema\\";
	CreateDirectoryW(dir.c_str(), nullptr);
	CreateDirectoryW((dir + L"Data").c_str(), nullptr);
	const std::wstring file = dir + L"Data\\regex_poe1.json";
	const std::string body =
		u8"{\"schema\":1,\"pages\":[{\"id\":\"a\",\"title\":\"A\",\"groups\":[\"g\"],\"entries\":[{\"id\":\"x\",\"zh\":[\"甲\"],\"en\":[\"X\"]}]},"
		u8"{\"id\":\"b\",\"kind\":\"future\",\"title\":\"B\",\"groups\":[],\"entries\":[{\"id\":\"y\",\"zh\":[\"乙\"]}]}]}";
	HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
		CloseHandle(h);
	}
	RegexDataset old;
	const bool loaded = old.Load(dir, L"poe1", &err);
	check(loaded && old.Pages().size() == 2 && old.Pages()[0].kind == RegexPageKind::Mods &&
	      old.Pages()[1].kind == RegexPageKind::Mods && old.Pages()[0].titleEn.empty() &&
	      old.Labels("poe1") && !old.Labels("poe1")->present && !old.Labels("poe2"),
	      "a schema-1 file loads as before: kind = mods, no labels, no titleEn");
	DeleteFileW(file.c_str());
	RemoveDirectoryW((dir + L"Data").c_str());
	RemoveDirectoryW(dir.c_str());
}

void PortTests(const std::wstring& exeDir)
{
	const DWORD t0 = GetTickCount();
	MatcherTests();
	line("");
	SchemaTests(exeDir);
	line("");
	GoldenTests();
	line("");
	NumericTests();
	line("");
	FragmentTests(exeDir);
	line("");
	RegexR3Tests(exeDir, &check, &line);
	line("");
	RegexR4Tests(exeDir, &check, &line);
	line("");
	RegexR5Tests(exeDir, &check, &line);
	line("");
	RegexR7Tests(exeDir, &check, &line);
	line("");
	RegexR8Tests(exeDir, &check, &line);
	line("");
	RegexSendTests(&check, &line);
	line("    (R1/R2 port checks took " + Num((int)(GetTickCount() - t0)) + " ms)");
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
	PortTests(exeDir);
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
