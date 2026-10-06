#include "regex_tool.h"

#include "clipboard_util.h"
#include "regex_algo_pages.h"
#include "regex_data.h"
#include "regex_embed.h"
#include "regex_folders.h"
#include "regex_gen.h"
#include "regex_itemmods.h"
#include "regex_share.h"
#include "regex_state.h"
#include "error_log.h"
#include "tool_panel.h"
#include "tool_window.h"
#include "ui_theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <thread>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <vector>

// 搜尋字串產生器 — tick the modifiers, get the shortest string that finds exactly
// those and nothing else, paste it into the game's search box.
//
// The panel owns no knowledge of what a map modifier is. It shows the pages the
// data files happen to contain, and everything about picking tokens lives in
// regex_gen.
//
// Choosing is two-level: the game first, then that game's list. PoE1 and PoE2
// have nothing to say to each other -- a waystone modifier is not a candidate
// for a map search and vice versa -- so a flat list of every page with the game
// spelled out in each label was one reading step where there should be none. The
// bookmark list follows the same selector for the same reason.
//
// A row shows the line in the language the query is being built from, and -- when
// the bilingual switch is on -- the other language underneath it. Which language
// the QUERY uses is the player's choice, because the two are not interchangeable:
// a token cut from the Chinese only avoids false positives among the Chinese
// lines, and pasting it into an English client would match nothing at all.
//
// Algorithmic pages (regex_algo_pages, ported from exile-appraiser): the map /
// waystone modifier pages carry a collapsible numeric SECTION on top (tier,
// quantity, rarity ...) whose terms join the modifier tokens in one string, and
// each game has a vendor page. Their rows are an input + a fragment, not a line
// to cut tokens from.
//
// Multi-page merge (R4, exile-appraiser combine.ts / RegexCombined.vue): every
// page of the current game with ticks, plus free-typed custom terms and excludes,
// go into ONE string (RegexAlgo::Combine). The output switches between that
// merged string and the current page's own ("合併 / 單頁", B's outScope, default
// merged); a "已選（合併）" view lists which pages take part, what each costs,
// the custom / exclude chips and the merge conflicts. Ticks live per page, so
// they survive switching pages either way.
//
// Persistence (R5, regex_ui.json schema 5 = exile-appraiser's state.ts): every
// page's ticks (a section's under its host's `num`), algorithmic values, custom
// text, excludes, output scope, the merged / page view and folded sections all
// go to state_ the moment they change. A bookmark is the whole page (embed.ts):
// a host page carries its section, the vendor page its values.
//
// Bookmarks (R6, exile-appraiser folders.ts / RegexBookmarks.vue): PoE1 / PoE2
// tabs, one level of folders (add / rename / delete / fold, an "uncategorised"
// group), drag to reorder or into a folder, with up / down buttons and a
// "move to" menu as the non-drag way. No hotkeys and no paste-into-game: those
// are exile-appraiser overlay features this panel does not have.
//
// Share codes and templates (R8, exile-appraiser share.ts / RegexPanel.vue): the
// whole game's ticks, values, custom text and excludes as one gzip+base64url
// string ("複製分享碼"), pasted back through a dialog ("貼上分享碼"), and seven
// hand-written templates (Data/regex_templates.json). Both OVERWRITE every list
// of that game, so both ask first (B's confirm dialogs). Codes travel both ways
// with exile-appraiser except for the item-mod values page, whose keys differ.

namespace {

const ImVec4 kWarn(0.95f, 0.66f, 0.25f, 1.0f);
const ImVec4 kBad(0.94f, 0.27f, 0.27f, 1.0f);
const ImVec4 kGood(0.45f, 0.85f, 0.55f, 1.0f);

// The game id is "poe1" / "poe2" and nothing else, so narrowing it for a message
// is a cast, not a conversion. Spelled out because the implicit form warns, and a
// silenced warning here would also silence the day someone passes real text.
std::string NarrowAscii(const std::wstring& w)
{
	std::string out;
	out.reserve(w.size());
	for (wchar_t c : w) out += (c > 0 && c < 128) ? (char)c : '?';
	return out;
}

// Which language a query is built from, and therefore which line a row leads
// with. Not a display preference: the two produce completely different tokens.
enum class Lang { Zh = 0, En = 1 };

const std::string& ZhLine(const RegexEntryDef& e)
{
	static const std::string empty;
	if (!e.zh.empty()) return e.zh[0];
	if (!e.en.empty()) return e.en[0];
	return empty;
}

// An entry can carry more than one English name where GGG gave two things the
// same Chinese one, so they are all named rather than one of them picked.
std::string EnLine(const RegexEntryDef& e)
{
	std::string out;
	for (size_t i = 0; i < e.en.size(); i++) {
		if (i) out += " / ";
		out += e.en[i];
	}
	return out.empty() ? ZhLine(e) : out;
}

// The line in `lang`, and the one in the other language. Every label, warning
// and bookmark caption goes through these, so nothing can disagree about which
// language the panel is currently in.
std::string LineIn(const RegexEntryDef& e, Lang lang)
{
	return lang == Lang::Zh ? ZhLine(e) : EnLine(e);
}

std::string OtherLine(const RegexEntryDef& e, Lang lang)
{
	return lang == Lang::Zh ? EnLine(e) : ZhLine(e);
}

// The language-neutral identity of an entry, and therefore what a saved pick is
// stored as. See regex_state.h for why it is not the row number.
const std::string& KeyOf(const RegexEntryDef& e)
{
	static const std::string empty;
	return e.en.empty() ? empty : e.en[0];
}

std::string ToLowerAscii(std::string s)
{
	for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
	return s;
}

// Per-page UI state. Kept separate from the data so switching pages and coming
// back does not lose the ticks -- a player comparing two pages should not be
// punished for looking.
struct PageState {
	std::vector<char> picked;      // parallel to the page's entries
	RegexGen::Corpus corpus;
	bool corpusReady = false;
	RegexGen::Result result;
	bool dirty = true;
	std::string search;
	int groupFilter = -1;          // -1 = every group
	bool t17Only = false;
	bool hideT17 = false;
	std::vector<int> visible;      // entry indices passing the filter
	bool filterDirty = true;
	// Algorithmic pages (vendor page, numeric section): ticks + values.
	RegexAlgo::AlgoSelection algo;
	// This page's output: its own picks plus, for a host page, its numeric
	// section (exile-appraiser store.ts pageCombined).
	RegexAlgo::CombineResult combined;
	// Item-mod values page (R7, RegexItemModList.vue): "only ticked" and the
	// filtered rows (ticked first, then at most kListCap more), rebuilt when the
	// search / group / ticks change.
	bool pickedOnly = false;
	std::vector<int> imvRows;
	int imvTotal = 0;
};

// R7: the item-mod values page of one game is built the first time it is opened
// (store.ts ensureItemMods) -- a few thousand modifiers, ~0.1-0.3 s to read and
// index -- on a worker thread; the panel polls `done` once per frame.
struct ItemModLoad {
	enum class Phase { Idle, Loading, Ready, Error };
	Phase phase = Phase::Idle;
	std::string err;
	long long ms = 0;
	int count = 0;
	std::vector<int> groupCounts;
	std::thread worker;
	std::atomic<bool> done{false};
	// Written by the worker before `done`, read by the panel after it.
	std::unique_ptr<RegexItemMods::Data> result;
	std::string resultErr;
	long long resultMs = 0;
};

// The panel is two-level: pick the game, then the list. Both games' catalogues
// are loaded, and every page belongs to exactly one of them, so this is the only
// vocabulary the selector needs.
constexpr const char* kGames[2] = {"poe1", "poe2"};

const char* GameLabel(const std::string& g)
{
	return g == "poe2" ? "PoE2" : "PoE1";
}

// Which modal wants to open. Raised by a button deep inside a child window and
// acted on at the top level, because OpenPopup and BeginPopupModal have to be
// called from the same ID scope or the popup simply never appears.
enum class Modal { None, Save, Rename, Delete, FolderAdd, FolderRename, FolderDelete, Paste, Template };

// The left column: the merged overview or the page's own list (store.ts panelView).
enum class View { Page, Combined };

class RegexToolPanel : public IToolPanel {
public:
	bool Init(const ToolPanelHost& h) override
	{
		host_ = &h;
		exeDir_ = h.exeDir;
		game_ = h.game.empty() ? std::wstring(L"poe1") : h.game;
		dataOk_ = data_.Load(exeDir_, game_, &dataErr_);
		// Corpus pages first, in the data's order, so a corpus page's index here
		// is its index in data_.Pages(); the algorithmic pages of both games after
		// them (store.ts:116 appends algoPages to each catalogue). `algo_` is
		// filled once and never grows again: refs_ point into it.
		for (const char* g : kGames) {
			for (RegexAlgo::AlgoPage& p : RegexAlgo::AlgoPages(g, data_.Labels(g)))
				algo_.push_back(std::move(p));
			// R7 (store.ts:116): the item-mod values page, without entries until
			// it is first opened. Only for a game that has a catalogue at all.
			if (data_.HasGame(g)) algo_.push_back(RegexItemMods::MakePage(g, nullptr));
		}
		for (const RegexPageDef& p : data_.Pages()) refs_.push_back({&p, nullptr});
		for (const RegexAlgo::AlgoPage& p : algo_) refs_.push_back({nullptr, &p});
		pages_.resize(refs_.size());
		for (size_t i = 0; i < refs_.size(); i++) {
			pages_[i].picked.assign(refs_[i].Size(), 0);
			if (refs_[i].algo) pages_[i].algo.Reset(refs_[i].Size());
		}
		// The launcher's game is the opening answer, but only if it has a
		// catalogue: offering an empty PoE2 tab to someone whose Data folder
		// predates it would be a dead end, not information.
		selGame_ = NarrowAscii(game_);
		if (!data_.HasGame(selGame_)) {
			selGame_.clear();
			for (const char* g : kGames)
				if (selGame_.empty() && data_.HasGame(g)) selGame_ = g;
		}

		// R8 templates: a missing / broken file only disables the drop-down.
		{
			std::vector<std::string> errs;
			if (!RegexShare::LoadTemplates(exeDir_, templates_, errs, &templatesErr_))
				PobLog::Error("data", "regex_templates.json: " + templatesErr_);
			for (const std::string& e : errs) PobLog::Error("data", "regex_templates.json: " + e);
		}
		state_.Load(exeDir_);   // a fresh install has no file; the defaults are fine
		restoreState();
		lang_ = state_.lang == "en" ? Lang::En : Lang::Zh;
		bilingual_ = state_.bilingual;
		scopeCombined_ = state_.outScope != "page";
		view_ = state_.panelView == "combined" ? View::Combined : View::Page;
		bmTab_ = bmTabFollow_ = selGame_;
		if (state_.SaveBlocked())
			notice_ = u8"regex_ui.json 是較新版本的 PobTools 寫的（schema " + std::to_string(state_.loadedSchema) +
			          u8"），這個版本不會覆寫它：可以照常使用，但這次的變更（勾選、書籤）不會保存。";
		return true;   // a missing data file is a message, not a dead tab
	}

	const char* InitError() const override { return ""; }

	~RegexToolPanel() override { joinItemMods(); }

	void Frame() override
	{
		pollItemMods();
		if (!dataOk_) {
			ImGui::TextColored(kBad, u8"搜尋字串資料載入失敗：%s", dataErr_.c_str());
			ImGui::TextDisabled(u8"請確認安裝目錄的 Data 底下有 regex_poe1.json / regex_poe2.json。");
			return;
		}
		drawHeader();
		ImGui::Separator();
		// Guarded because every panel below reaches for the current page. Load()
		// having succeeded does not promise a page survived the entry filter.
		if (!hasPage()) {
			ImGui::TextColored(kWarn, u8"這個版本沒有任何可用的清單。");
			return;
		}

		const float avail = ImGui::GetContentRegionAvail().x;
		const float leftW = std::max(340.0f, avail * 0.54f);
		ImGui::BeginChild("##rx_left", ImVec2(leftW, 0), false);
		if (view_ == View::Combined) drawCombinedView();
		else drawList();
		ImGui::EndChild();
		ImGui::SameLine();
		ImGui::BeginChild("##rx_right", ImVec2(0, 0), false);
		drawOutput();
		ImGui::Separator();
		drawBookmarks();
		ImGui::EndChild();

		drawModals();
	}

	void RunDeferred() override
	{
		if (!copyRequest_.empty()) {
			WriteClipboardUtf8(host_ ? host_->hostHwnd : nullptr, copyRequest_);
			copied_ = true;
			copyRequest_.clear();
		}
		if (!shareCopyRequest_.empty()) {
			shareCopied_ = WriteClipboardUtf8(host_ ? host_->hostHwnd : nullptr, shareCopyRequest_) ? 1 : 2;
			shareCopiedAt_ = std::chrono::steady_clock::now();
			shareCopyRequest_.clear();
		}
		// Written here rather than in Frame(): this is the one place a panel is
		// allowed to touch the disk, and it runs at most once per frame.
		flushState();
	}

	ToolCloseState RequestClose() override { return close_ = ToolCloseState::Closed; }
	ToolCloseState CloseState() const override { return close_; }
	void AbortClose() override
	{
		if (close_ == ToolCloseState::Closed) close_ = ToolCloseState::Open;
	}
	void Shutdown() override
	{
		// The backstop. RunDeferred normally gets there first, but a close can
		// land between a tick and the next deferred pass, and bookmarks are the
		// one thing here the player cannot recreate from anywhere else.
		flushState();
		joinItemMods();
	}
	PobUi::Density Density() const override { return PobUi::Density::Compact; }
	const char* PanelId() const override { return "regex"; }

private:
	bool hasPage() const { return page_ >= 0 && page_ < (int)refs_.size(); }
	PageState& st() { return pages_[page_]; }
	const PageState& st() const { return pages_[page_]; }

	// The current page is an algorithmic one (the vendor page); everything that
	// reads entries() / groups() is for corpus pages only.
	bool isAlgo() const { return refs_[page_].algo != nullptr; }
	const std::vector<RegexEntryDef>& entries() const
	{
		return refs_[page_].corpus->entries;
	}
	const std::vector<std::string>& groups() const
	{
		return refs_[page_].corpus->groups;
	}
	int limit() const { return refs_[page_].Limit(); }
	const std::string& pageNote() const
	{
		return refs_[page_].corpus ? refs_[page_].corpus->note : refs_[page_].algo->note;
	}

	// The numeric / condition section of page `host` (same game), as an index into
	// refs_; -1 if none. Step 40: a host may itself be algorithmic (the item-mod
	// values page carries the rarity | corruption section).
	int sectionIndexOf(int host) const
	{
		if (host < 0 || host >= (int)refs_.size() || refs_[host].IsSection()) return -1;
		for (int i = 0; i < (int)refs_.size(); i++)
			if (refs_[i].IsSection() && refs_[i].algo->sectionOf == refs_[host].Id() &&
			    refs_[i].algo->game == refs_[host].Game())
				return i;
		return -1;
	}

	// ---- games ---------------------------------------------------------------

	// The first page of a game, or -1. Also the answer to "does this game have a
	// catalogue at all", which is why the caller never asks that separately.
	int firstPageOf(const std::string& g) const
	{
		for (size_t i = 0; i < refs_.size(); i++)
			if (refs_[i].Game() == g && !refs_[i].IsSection()) return (int)i;
		return -1;
	}
	// Which game a page id belongs to; empty when no loaded catalogue has it.
	// That case is a bookmark saved against a list this build no longer ships,
	// and it is reported rather than quietly filed under one of the games.
	std::string gameOfPage(const std::string& id) const
	{
		for (const RegexAlgo::PageRef& p : refs_)
			if (p.Id() == id) return p.Game();
		return std::string();
	}

	// Every page's corpus is language-specific, so switching language throws them
	// all away rather than only the one on screen: coming back to a page whose
	// index was built from the other language would silently produce tokens that
	// match nothing.
	void invalidateCorpora()
	{
		for (PageState& ps : pages_) {
			ps.corpusReady = false;
			ps.dirty = true;
		}
		combinedDirty_ = true;
	}
	std::string pageId() const
	{
		return hasPage() ? refs_[page_].Id() : std::string();
	}
	std::string pageTitleById(const std::string& id) const
	{
		for (const RegexAlgo::PageRef& p : refs_)
			if (p.Id() == id) return p.Title();
		return id;
	}

	// ---- remembered state ----------------------------------------------------

	void flushState()
	{
		if (!stateDirty_ || saveFailed_) return;
		// A newer build's file: not ours to overwrite (said once, at Init).
		if (state_.SaveBlocked()) {
			stateDirty_ = false;
			return;
		}
		// The return value used to be dropped. Bookmarks are the only thing this
		// tool holds that the player cannot rebuild from anywhere else, so a save
		// that quietly did nothing would surface days later as "my bookmarks are
		// gone" with nothing to point at.
		if (!state_.Save(exeDir_)) {
			PobLog::Error("save", u8"regex_ui.json 存檔失敗（書籤與勾選沒有保存）");
			// RunDeferred runs EVERY FRAME. Retrying here without a brake meant
			// sixty failed opens a second -- and, before the log learned to
			// collapse repeats, sixty identical lines a second with it.
			// The moment worth retrying is the next time the user changes
			// something, not the next frame; `stateDirty_` stays set so that
			// retry still writes everything.
			saveFailed_ = true;
			return;
		}
		stateDirty_ = false;
	}

	void restoreState()
	{
		// The remembered game wins over the launcher's, but only if it still has
		// a catalogue; otherwise Init's answer stands.
		if (!state_.game.empty() && data_.HasGame(state_.game)) selGame_ = state_.game;
		page_ = firstPageOf(selGame_);
		for (size_t i = 0; i < refs_.size(); i++)
			if (refs_[i].Id() == state_.page && refs_[i].Game() == selGame_ && !refs_[i].IsSection())
				page_ = (int)i;
		mode_ = ModeFromId(state_.mode);

		// Bookmarks written before the split carry no game. Filling it in from
		// the page id -- and writing it back -- is what keeps them visible: the
		// list is filtered by game, and an unfilled one would have no column to
		// appear in. One that names a page this build no longer ships stays
		// empty on purpose and is counted in the panel instead of vanishing.
		bool filled = false;
		for (RegexBookmark& b : state_.bookmarks) {
			if (!b.game.empty()) continue;
			const std::string g = gameOfPage(b.page);
			if (g.empty()) continue;
			b.game = g;
			filled = true;
			stateDirty_ = true;
		}
		// A filled-in bookmark may name a folder: list it and keep the order
		// invariant (store.ts onCatalogueReady).
		if (filled) RegexFolders::Normalize(state_);

		// Every page's saved ticks (a section's live in its host's `num`) and
		// every algorithmic page's values (a section's under its host id).
		int missedTotal = 0;
		std::string firstPage;
		for (size_t i = 0; i < refs_.size(); i++) {
			const RegexAlgo::PageRef& ref = refs_[i];
			if (ref.algo) {
				if (const RegexValueList* m = state_.NumericOf(RegexAlgo::NumericKeyOf(ref.Id())))
					for (const auto& kv : *m)
						if (ownsValue((int)i, kv.first)) pages_[i].algo.values[kv.first] = kv.second;
			}
			// The item-mod values page has no entries until it is loaded: its
			// ticks are restored then (finishItemMods), else every saved key
			// would read as "not found" (store.ts onCatalogueReady).
			if (isItemPage((int)i)) continue;
			const std::optional<RegexEmbed::Applied> r = RegexEmbed::SavedPicksOf(ref, state_);
			if (!r) continue;
			setTicks((int)i, r->picked);
			if (r->missed > 0) {
				missedTotal += r->missed;
				if (firstPage.empty()) firstPage = refs_[hostIndexOf((int)i)].Title();
			}
		}
		if (missedTotal > 0)
			notice_ = u8"上次的勾選有 " + std::to_string(missedTotal) + u8" 項（" + firstPage +
			          u8" 等）在目前的資料裡找不到，可能是賽季更新後詞條有變動。";
		// Saved ticks on an item-mod values page, or it is the remembered page:
		// load it in the background now (it restores itself when ready).
		for (const char* g : kGames) {
			const int ip = itemPageIndex(g);
			if (ip < 0) continue;
			bool want = (selGame_ == g && page_ == ip);
			for (const RegexPagePicks& c : state_.current)
				if (c.page == refs_[ip].Id() && !c.keys.empty()) want = true;
			if (want) startItemMods(g);
		}
	}

	static RegexGen::Mode ModeFromId(const std::string& id)
	{
		return id == "all" ? RegexGen::Mode::All
		     : id == "none" ? RegexGen::Mode::None : RegexGen::Mode::Any;
	}
	const char* modeId() const
	{
		return mode_ == RegexGen::Mode::All ? "all"
		     : mode_ == RegexGen::Mode::None ? "none" : "any";
	}

	// Overwrite one page's ticks (corpus or algorithmic) from a list of indices.
	void setTicks(int idx, const std::vector<int>& picked)
	{
		std::vector<char>& v = refs_[idx].algo ? pages_[idx].algo.picked : pages_[idx].picked;
		std::fill(v.begin(), v.end(), (char)0);
		for (int i : picked)
			if (i >= 0 && i < (int)v.size()) v[i] = 1;
		pages_[idx].dirty = true;
		pages_[idx].filterDirty = true;
	}

	// Does algorithmic page `idx` own value `id`? A host that is itself algorithmic
	// (the item-mod values page) and its rarity | corruption section keep their
	// values under the same store key (the host id; entry ids never overlap), so
	// each page only takes -- and only writes back -- its own entries' values.
	// Otherwise a stale copy on one page would overwrite the other's newer value.
	bool ownsValue(int idx, const std::string& id) const
	{
		if (!refs_[idx].algo) return false;
		if (refs_[idx].IsSection()) {
			for (const RegexAlgo::AlgoEntry& e : refs_[idx].algo->entries)
				if (e.def.id == id) return true;
			return false;
		}
		const int sec = sectionIndexOf(idx);
		if (sec >= 0)
			for (const RegexAlgo::AlgoEntry& e : refs_[sec].algo->entries)
				if (e.def.id == id) return false;
		return true;
	}

	// store.ts syncCurrent: page idx's ticks -> its saved record. A section's
	// ticks are its host's `num`; the vendor page's are entry ids.
	void syncCurrent(int idx)
	{
		const RegexAlgo::PageRef& ref = refs_[idx];
		const RegexEmbed::Keys k = RegexEmbed::PageKeysOf(ref, picksOf(idx));
		if (ref.IsSection()) {
			state_.PicksFor(ref.algo->sectionOf).num = k.keys;
		} else {
			RegexPagePicks& p = state_.PicksFor(ref.Id());
			p.keys = k.keys;
			p.alt = k.alt;
		}
		markStateDirty();
	}

	// store.ts setValue: an algorithmic page's values -> numeric[store key].
	void syncValues(int idx)
	{
		if (!refs_[idx].algo) return;
		RegexValueList& dst = state_.NumericFor(RegexAlgo::NumericKeyOf(refs_[idx].Id()));
		for (const auto& kv : pages_[idx].algo.values) {
			if (!ownsValue(idx, kv.first)) continue;
			const RegexFrag::AlgoValue* had = RegexValueFind(dst, kv.first);
			if (!had || had->min != kv.second.min || had->max != kv.second.max ||
			    had->choice != kv.second.choice || RegexFrag::HasChoice(*had) != RegexFrag::HasChoice(kv.second))
				RegexValueSet(dst, kv.first, kv.second);
		}
		markStateDirty();
	}

	void markStateDirty()
	{
		stateDirty_ = true;
		saveFailed_ = false;   // a fresh change deserves a fresh attempt
	}

	// Everything that changes what is ticked funnels through here, so there is
	// exactly one place that could forget to persist.
	void picksChanged()
	{
		st().dirty = true;
		combinedDirty_ = true;
		st().filterDirty = true;   // ticked rows move to the top; see refreshFilter
		copied_ = false;
		syncCurrent(page_);
		state_.game = selGame_;
		state_.page = pageId();
		state_.mode = modeId();
	}

	// A tick or a value on an algorithmic page changed.
	void algoChanged(int idx)
	{
		pages_[idx].dirty = true;
		pages_[idx].filterDirty = true;   // the item-mod page keeps ticked rows on top
		combinedDirty_ = true;
		if (refs_[idx].IsSection()) {
			// The section's output is part of its host page's string.
			const int host = hostIndexOf(idx);
			if (host != idx) pages_[host].dirty = true;
		}
		copied_ = false;
		syncCurrent(idx);
		syncValues(idx);
	}

	// ---- header --------------------------------------------------------------

	void drawHeader()
	{
		// Game first, then that game's list. A game with no catalogue is not
		// offered at all: a selectable option that leads to an empty panel is
		// worse than not seeing it, and --regex-selftest fails when either file
		// is missing from the install, so this cannot hide a packaging mistake.
		ImGui::SetNextItemWidth(88 * host_->scale);
		if (ImGui::BeginCombo(u8"遊戲", GameLabel(selGame_))) {
			for (const char* g : kGames) {
				if (firstPageOf(g) < 0) continue;
				if (ImGui::Selectable(GameLabel(g), selGame_ == g)) switchGame(g);
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine(0, 16 * host_->scale);
		// RegexPanel.vue view seg: the merged overview or the page's own list.
		{
			const std::string merged = u8"已選（合併）· " + std::to_string(combineOrderIdx(true).size()) +
			                           u8" 頁###rx_view_combined";
			if (segButton(merged.c_str(), view_ == View::Combined)) setView(View::Combined);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"目前遊戲所有有勾選的清單、自訂文字與排除詞，合成一串");
			ImGui::SameLine(0, 2 * host_->scale);
			if (segButton(u8"單頁清單###rx_view_page", view_ == View::Page)) setView(View::Page);
		}
		ImGui::SameLine(0, 16 * host_->scale);
		ImGui::SetNextItemWidth(170 * host_->scale);
		if (ImGui::BeginCombo(u8"清單", refs_[page_].Title().c_str())) {
			// Numeric sections are not pages of their own: they sit on top of
			// their host page (exile-appraiser step 32, listedPages). The count
			// includes the section, as store.ts pagePickCount does.
			for (size_t i = 0; i < refs_.size(); i++) {
				if (refs_[i].Game() != selGame_ || refs_[i].IsSection()) continue;
				const int n = pagePickCount((int)i);
				const std::string label = refs_[i].Title() + (n ? u8"（" + std::to_string(n) + u8"）" : std::string()) +
				                          "###rx_pg" + std::to_string(i);
				if (ImGui::Selectable(label.c_str(), page_ == (int)i)) {
					switchPage((int)i);
					setView(View::Page);   // store.ts switchPage: back to the page's list
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine(0, 24 * host_->scale);

		// The three shapes the client's search actually has. Changing this
		// changes the string, not the picks, so it lives next to the list.
		int m = (int)mode_;
		bool changed = false;
		changed |= ImGui::RadioButton(u8"含任一個", &m, 0); ImGui::SameLine();
		changed |= ImGui::RadioButton(u8"全部都有", &m, 1); ImGui::SameLine();
		changed |= ImGui::RadioButton(u8"一個都沒有", &m, 2);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(u8"「一個都沒有」產生的是排除字串（開頭的 ! ），"
			                  u8"用來把有這些詞綴的東西藏起來。");
		if (changed && m != (int)mode_) {
			mode_ = (RegexGen::Mode)m;
			st().dirty = true;
			combinedDirty_ = true;
			copied_ = false;
			state_.mode = modeId();
			stateDirty_ = true;
		}

		if (view_ == View::Page) {
			ImGui::SameLine(0, 24 * host_->scale);
			ImGui::TextDisabled(u8"已勾選 %d / %d", pickCount(), (int)refs_[page_].Size());
		}

		// Right-hand end of the header row. It belongs to the whole panel rather
		// than to the list toolbar: it changes how both columns read, and the
		// toolbar is the row that runs out of width first.
		{
			const char* label = u8"雙語顯示";
			const float w = ImGui::GetFrameHeight() +
			                ImGui::GetStyle().ItemInnerSpacing.x +
			                ImGui::CalcTextSize(label).x;
			// Right-aligned, but never to the left of where the row already is:
			// a narrow window would otherwise draw this on top of the mode radios
			// instead of wrapping, and an overlap reads as a rendering bug.
			ImGui::SameLine();
			const float after = ImGui::GetCursorPosX();
			ImGui::SameLine(std::max(after, ImGui::GetContentRegionMax().x - w));
			if (ImGui::Checkbox(label, &bilingual_)) {
				state_.bilingual = bilingual_;
				stateDirty_ = true;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"在每一列下面加上另一種語言的原文");
		}

		drawShareRow();

		const std::string& note = pageNote();
		if (view_ == View::Page && !note.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextWrapped("%s", note.c_str());
			ImGui::PopStyleColor();
		}
	}

	int pickCount() const
	{
		if (isAlgo()) return st().algo.Count();
		int n = 0;
		for (char c : st().picked) n += c ? 1 : 0;
		return n;
	}

	// ---- the list ------------------------------------------------------------

	void drawList()
	{
		if (isAlgo()) {
			if (isItemPage(page_)) {
				// RegexPanel.vue (step 40, B d5ccb47): the
				// rarity | corruption section above the item-mod list, loaded or not
				const int sec = sectionIndexOf(page_);
				if (sec >= 0) drawSection(page_, sec);
				drawItemModPage(page_);
			} else {
				drawAlgoPage(page_);
			}
			return;
		}
		const int sec = sectionIndexOf(page_);
		if (sec >= 0) drawSection(page_, sec);
		PageState& s = st();
		ImGui::SetNextItemWidth(150 * host_->scale);
		if (ImGui::InputTextWithHint("##rx_search", u8"搜尋中英文…", &s.search))
			s.filterDirty = true;
		if (!groups().empty()) {
			ImGui::SameLine();
			ImGui::SetNextItemWidth(130 * host_->scale);
			const char* label = s.groupFilter < 0 ? u8"全部分類"
			                                      : groups()[s.groupFilter].c_str();
			if (ImGui::BeginCombo("##rx_group", label)) {
				if (ImGui::Selectable(u8"全部分類", s.groupFilter < 0)) {
					s.groupFilter = -1;
					s.filterDirty = true;
				}
				for (int g = 0; g < (int)groups().size(); g++) {
					if (ImGui::Selectable(groups()[g].c_str(), s.groupFilter == g)) {
						s.groupFilter = g;
						s.filterDirty = true;
					}
				}
				ImGui::EndCombo();
			}
		}
		if (pageHasT17()) {
			// One three-way control rather than two checkboxes: "only" and
			// "exclude" are mutually exclusive, and two boxes that silently
			// untick each other are a worse explanation than a list of three.
			ImGui::SameLine();
			ImGui::SetNextItemWidth(120 * host_->scale);
			const int cur = s.t17Only ? 1 : (s.hideT17 ? 2 : 0);
			const char* names[3] = {u8"T17：全部", u8"T17：只看", u8"T17：排除"};
			if (ImGui::BeginCombo("##rx_t17", names[cur])) {
				for (int i = 0; i < 3; i++) {
					if (!ImGui::Selectable(names[i], cur == i)) continue;
					s.t17Only = (i == 1);
					s.hideT17 = (i == 2);
					s.filterDirty = true;
				}
				ImGui::EndCombo();
			}
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"全選")) {
			refreshFilter();
			for (int i : s.visible) s.picked[i] = 1;
			picksChanged();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(u8"把目前篩選出來的 %d 項全部勾選", (int)s.visible.size());
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"清除")) {
			std::fill(s.picked.begin(), s.picked.end(), (char)0);
			picksChanged();
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"取消所有勾選");

		if (s.filterDirty) refreshFilter();

		ImGui::BeginChild("##rx_rows", ImVec2(0, 0), true);
		// A row is one line, or two when the bilingual switch is on, so the clipper
		// cannot work the height out for itself. It is given one, and each row is
		// then PINNED to that height rather than left to whatever the widgets
		// happened to measure: a per-row error of a fraction of a pixel is
		// invisible at the top of a long list and puts the bottom out of reach.
		const ImGuiStyle& style = ImGui::GetStyle();
		const float textH = bilingual_ ? ImGui::GetTextLineHeight() * 2 + style.ItemSpacing.y
		                               : ImGui::GetTextLineHeight();
		const float rowH = std::max(ImGui::GetFrameHeight(), textH) + style.ItemSpacing.y;
		const float top = ImGui::GetCursorPosY();
		ImGuiListClipper clip;
		clip.Begin((int)s.visible.size(), rowH);
		while (clip.Step()) {
			for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
				ImGui::SetCursorPosY(top + row * rowH);
				drawRow(s, s.visible[row], rowH);
			}
		}
		ImGui::EndChild();
	}

	void drawRow(PageState& s, int idx, float rowH)
	{
		const RegexEntryDef& e = entries()[idx];
		ImGui::PushID(idx);
		bool on = s.picked[idx] != 0;
		if (on) {
			// Drawn behind the row, in its own space, so it costs no layout.
			const ImVec2 p0 = ImGui::GetCursorScreenPos();
			const ImVec2 p1(p0.x + ImGui::GetContentRegionAvail().x, p0.y + rowH);
			ImGui::GetWindowDrawList()->AddRectFilled(
				ImVec2(p0.x - 2, p0.y - 1), p1, ImGui::GetColorU32(ImGuiCol_Header, 0.55f),
				3.0f * host_->scale);
		}
		if (ImGui::Checkbox("##pick", &on)) {
			s.picked[idx] = on ? 1 : 0;
			picksChanged();
		}
		ImGui::SameLine();
		ImGui::BeginGroup();
		{
			std::string label = LineIn(e, lang_);
			const size_t extra = (lang_ == Lang::Zh ? e.zh.size() : e.en.size());
			if (extra > 1)
				label += u8"  （另有 " + std::to_string(extra - 1) + u8" 行）";
			if (e.t17) {
				ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
				ImGui::TextUnformatted("T17");
				ImGui::PopStyleColor();
				ImGui::SameLine();
			}
			ImGui::TextUnformatted(label.c_str());
			if (bilingual_) {
				const std::string other = OtherLine(e, lang_);
				ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
				ImGui::TextUnformatted(other.empty() ? u8"（沒有對照）" : other.c_str());
				ImGui::PopStyleColor();
			}
		}
		ImGui::EndGroup();
		if (ImGui::IsItemHovered()) drawEntryTooltip(e);
		ImGui::PopID();
	}

	void drawEntryTooltip(const RegexEntryDef& e)
	{
		ImGui::BeginTooltip();
		for (const std::string& l : e.zh) ImGui::TextUnformatted(l.c_str());
		if (!e.en.empty()) {
			ImGui::Separator();
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			for (const std::string& l : e.en) ImGui::TextUnformatted(l.c_str());
			ImGui::PopStyleColor();
		}
		if (!e.affixZh.empty()) {
			ImGui::Separator();
			ImGui::TextDisabled(u8"來源詞綴：%s", e.affixZh.c_str());
		}
		// What the search reads besides the line. Shown because it explains
		// why a token is longer than the line alone would need -- and cut to a
		// few rows, since a reminder text can run to a paragraph.
		const std::vector<std::string>& hidden = (lang_ == Lang::Zh) ? e.hiddenZh : e.hiddenEn;
		if (!hidden.empty()) {
			ImGui::Separator();
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextUnformatted(u8"遊戲搜尋也會比對：");
			const size_t shown = hidden.size() < 4 ? hidden.size() : 4;
			for (size_t i = 0; i < shown; i++) ImGui::BulletText("%s", hidden[i].c_str());
			if (hidden.size() > shown)
				ImGui::Text(u8"（另有 %d 行）", (int)(hidden.size() - shown));
			ImGui::PopStyleColor();
		}
		ImGui::EndTooltip();
	}

	// ---- output --------------------------------------------------------------

	void drawOutput()
	{
		PageState& s = st();
		if (!isAlgo() && !s.corpusReady) buildCorpus();
		if (s.dirty) recompute();
		// RegexPanel.vue `out`: the merged string or this page's own.
		const RegexAlgo::CombineResult& out = scopeCombined_ ? combinedAll() : s.combined;

		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(u8"貼進遊戲搜尋列");
		{
			// The out-scope seg, right-aligned on the same row.
			const char* a = u8"合併";
			const char* b = u8"單頁";
			const ImGuiStyle& style = ImGui::GetStyle();
			const float w = ImGui::CalcTextSize(a).x + ImGui::CalcTextSize(b).x + style.FramePadding.x * 4 + 2 * host_->scale;
			ImGui::SameLine();
			const float after = ImGui::GetCursorPosX();
			ImGui::SameLine(std::max(after, ImGui::GetContentRegionMax().x - w));
			if (segButton(a, scopeCombined_)) setScope(true);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"合併 = 所有有勾選的清單合成一串；單頁 = 只有目前這份清單");
			ImGui::SameLine(0, 2 * host_->scale);
			if (segButton(b, !scopeCombined_)) setScope(false);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"合併 = 所有有勾選的清單合成一串；單頁 = 只有目前這份清單");
		}
		std::string q = out.query;
		ImGui::InputTextMultiline("##rx_out", &q, ImVec2(-1, 70 * host_->scale),
		                          ImGuiInputTextFlags_ReadOnly);

		// combine.ts `limit`: the smallest limit of the pages taking part (250).
		const int len = out.length;
		const int lim = out.limit;
		ImGui::PushStyleColor(ImGuiCol_Text, len > lim ? kBad : (len > lim * 4 / 5 ? kWarn : kGood));
		ImGui::Text(u8"長度 %d / %d 字", len, lim);
		ImGui::PopStyleColor();
		// RegexPanel.vue partsText: what each part costs, when more than one part.
		if (scopeCombined_ && out.perPage.size() + (out.custom.empty() ? 0 : 1) + (out.excludes.empty() ? 0 : 1) > 1) {
			std::string parts;
			for (const RegexAlgo::PageContribution& c : out.perPage)
				parts += (parts.empty() ? "" : u8" · ") + pageTitleInGame(c.id) + " " + std::to_string(c.length);
			if (!out.custom.empty()) parts += (parts.empty() ? "" : u8" · ") + std::string(u8"自訂文字 ") + std::to_string(out.customLength);
			if (!out.excludes.empty()) parts += (parts.empty() ? "" : u8" · ") + std::string(u8"排除詞 ") + std::to_string(out.excludesLength);
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextWrapped("%s", parts.c_str());
			ImGui::PopStyleColor();
		}
		if (len > lim) ImGui::TextColored(kBad, u8"超過上限，請減少勾選（遊戲搜尋列最多 %d 字）", lim);
		// Whatever the panel last did, said next to the thing it changed. It used
		// to sit at the very top, three sections away from the string it was
		// talking about.
		if (!notice_.empty()) {
			ImGui::TextColored(kWarn, "%s", notice_.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton(u8"知道了###rx_notice")) notice_.clear();
		}

		ImGui::BeginDisabled(out.query.empty());
		if (ImGui::Button(u8"複製", ImVec2(90 * host_->scale, 0))) {
			copyRequest_ = out.query;
			copied_ = false;
		}
		ImGui::EndDisabled();
		ImGui::SameLine();
		// Next to the copy button because that is the decision it changes: which
		// client the copied string is for.
		ImGui::SetNextItemWidth(150 * host_->scale);
		if (ImGui::BeginCombo("##rx_lang", lang_ == Lang::Zh ? u8"輸出：繁體中文"
		                                                     : u8"輸出：English")) {
			if (ImGui::Selectable(u8"輸出：繁體中文", lang_ == Lang::Zh)) setLang(Lang::Zh);
			if (ImGui::Selectable(u8"輸出：English", lang_ == Lang::En)) setLang(Lang::En);
			ImGui::EndCombo();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(u8"要貼進哪一種語言的遊戲客戶端。"
			                  u8"兩邊產生的片段完全不同，不能互換使用。");
		if (copied_) {
			ImGui::SameLine();
			ImGui::TextColored(kGood, u8"已複製");
		}

		// Merged: the details live in the merged view (RegexCombined.vue); here
		// only how many conflicts there are, and a way there.
		if (scopeCombined_) {
			if (!out.conflicts.empty()) {
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(kWarn, u8"%d 個合併衝突", (int)out.conflicts.size());
				if (view_ != View::Combined) {
					ImGui::SameLine();
					if (ImGui::SmallButton(u8"查看###rx_see_combined")) setView(View::Combined);
				}
			}
			if (!out.custom.empty()) ImGui::TextDisabled(u8"自訂文字不經驗證，可能誤中其他物品。");
			return;
		}

		// Two things the player cannot check for themselves, so both are stated
		// rather than implied: which picks the string could not express, and what
		// it is actually made of.
		if (!s.result.unresolved.empty()) {
			ImGui::TextColored(kWarn, u8"有 %d 項無法單獨指定：",
			                   (int)s.result.unresolved.size());
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			for (int i : s.result.unresolved)
				ImGui::BulletText("%s", LineIn(entries()[i], lang_).c_str());
			ImGui::TextWrapped(u8"清單裡有其他項目印出一模一樣的文字，"
			                   u8"或這一行能用的每一段字也出現在每張物品都有的文字裡"
			                   u8"（詞綴名稱、階層、提示說明、已汙染這類標籤），"
			                   u8"遊戲的搜尋沒有辦法只中它。");
			ImGui::PopStyleColor();
		}

		// Numeric / vendor conditions that did not make it into the string, or
		// that would also hit a modifier line of this page.
		{
			int invalid = 0;
			std::vector<std::string> clash;
			for (const RegexAlgo::Conflict& c : s.combined.conflicts) {
				if (c.kind == RegexAlgo::ConflictKind::Invalid) invalid++;
				else if (c.kind == RegexAlgo::ConflictKind::Fragment) clash.push_back(c.text);
			}
			if (invalid > 0)
				ImGui::TextColored(kWarn, u8"有 %d 個數值條件輸入不成立，沒有放進字串。", invalid);
			if (!clash.empty()) {
				ImGui::TextColored(kWarn, u8"有 %d 個數值條件也會中這一頁的詞綴：", (int)clash.size());
				ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
				for (const std::string& t : clash) ImGui::BulletText("%s", t.c_str());
				ImGui::PopStyleColor();
			}
		}

		// Corpus tokens, then the numeric / vendor terms, as they appear in the string.
		std::vector<std::string> parts = s.result.tokens;
		for (const RegexAlgo::PageContribution& c : s.combined.perPage)
			if (c.kind == RegexPageKind::Numeric || c.kind == RegexPageKind::Sockets)
				parts.insert(parts.end(), c.fragments.begin(), c.fragments.end());
		if (!parts.empty() &&
		    ImGui::CollapsingHeader((u8"用到的片段（" +
		                             std::to_string(parts.size()) +
		                             u8" 段）###rx_tok").c_str())) {
			ImGui::TextDisabled(u8"括號只是為了看清楚頭尾的空白，不要打進去");
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			// Bracketed, because a space at either end of a token is significant
			// and otherwise invisible: " 傷" and "傷" are different searches, and
			// the first is the one that does not also match 怪物傷害.
			for (const std::string& t : parts)
				ImGui::BulletText(u8"「%s」", t.c_str());
			ImGui::PopStyleColor();
		}
	}

	// ---- bookmarks -----------------------------------------------------------
	//
	// R6 (exile-appraiser RegexBookmarks.vue): PoE1 / PoE2 tabs, one level of
	// folders, drag to reorder / into a folder. The folder logic itself is
	// regex_folders (pure, under --regex-selftest); this only draws it. Edits
	// made while drawing are queued in bmAction_ and applied after the list, so
	// no index the loop is still using moves under it.

	// One game's pages (corpus + algorithmic, sections included): page ids repeat
	// across games (gem_names, vendor_bases), so RegexEmbed always looks within one.
	std::vector<RegexAlgo::PageRef> gamePages(const std::string& g) const
	{
		std::vector<RegexAlgo::PageRef> out;
		for (const RegexAlgo::PageRef& p : refs_)
			if (p.Game() == g) out.push_back(p);
		return out;
	}
	int indexInGame(const std::string& g, const std::string& id) const
	{
		for (int i = 0; i < (int)refs_.size(); i++)
			if (refs_[i].Game() == g && refs_[i].Id() == id) return i;
		return -1;
	}
	std::string pageTitleIn(const std::string& g, const std::string& id) const
	{
		const int i = indexInGame(g, id);
		return i >= 0 ? refs_[i].Title() : pageTitleById(id);
	}

	// store.ts currentBookmarkBody: the page on screen (+ its section) as a bookmark.
	std::optional<RegexBookmark> currentBookmarkBody() const
	{
		if (!hasPage()) return std::nullopt;
		RegexEmbed::PicksMap picks;
		RegexEmbed::ValuesMap values;
		auto add = [&](int i) {
			picks[refs_[i].Id()] = picksOf(i);
			// merged: the item-mod values page and its section share the store key
			if (refs_[i].algo) {
				RegexAlgo::ValueMap& dst = values[RegexAlgo::NumericKeyOf(refs_[i].Id())];
				for (const auto& kv : pages_[i].algo.values)
					if (ownsValue(i, kv.first)) dst[kv.first] = kv.second;
			}
		};
		add(page_);
		const int sec = sectionIndexOf(page_);
		if (sec >= 0) add(sec);
		return RegexEmbed::BookmarkBodyOf(gamePages(selGame_), refs_[page_], picks, values, selGame_, modeId(),
		                                  lang_ == Lang::En ? "en" : "zh");
	}

	struct BmAction {
		enum Kind { None, ToFolder, Before, Step, FolderTo, FolderStep, Fold } kind = None;
		int index = -1;      // bookmark
		int before = -1;     // Before: the bookmark to land in front of
		int delta = 0;       // Step / FolderStep
		int to = 0;          // FolderTo
		bool on = false;     // Fold
		std::string folder;  // ToFolder / FolderTo / FolderStep / Fold ("" = uncategorised)
	};

	void applyBmAction()
	{
		const BmAction a = bmAction_;
		bmAction_ = BmAction{};
		bool changed = false;
		switch (a.kind) {
		case BmAction::None: return;
		case BmAction::ToFolder: changed = RegexFolders::MoveBookmark(state_, a.index, a.folder) >= 0; break;
		case BmAction::Before: changed = RegexFolders::MoveBookmark(state_, a.index, std::string(), a.before) >= 0; break;
		case BmAction::Step: changed = RegexFolders::MoveBookmarkBy(state_, a.index, a.delta) != a.index; break;
		case BmAction::FolderTo: changed = RegexFolders::MoveTo(state_, bmTab_, a.folder, a.to); break;
		case BmAction::FolderStep: changed = RegexFolders::MoveBy(state_, bmTab_, a.folder, a.delta); break;
		case BmAction::Fold: changed = RegexFolders::SetCollapsed(state_, bmTab_, a.folder, a.on); break;
		}
		if (changed) markStateDirty();
	}

	void drawBookmarks()
	{
		// The tab follows the list's game when that changes (RegexBookmarks.vue
		// watches selGame); a tab picked by hand stays until then.
		if (bmTabFollow_ != selGame_) bmTab_ = bmTabFollow_ = selGame_;
		int count[2] = {0, 0}, orphans = 0;
		for (const RegexBookmark& b : state_.bookmarks) {
			if (b.game.empty()) orphans++;
			else count[b.game == "poe2" ? 1 : 0]++;
		}
		const int tabIdx = bmTab_ == "poe2" ? 1 : 0;

		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(u8"書籤");
		for (int gi = 0; gi < 2; gi++) {
			const std::string g = kGames[gi];
			// A game with neither a catalogue nor bookmarks has nothing to show.
			if (firstPageOf(g) < 0 && count[gi] == 0) continue;
			ImGui::SameLine(0, (gi ? 2.0f : 8.0f) * host_->scale);
			const std::string label = std::string(GameLabel(g)) + u8"（" + std::to_string(count[gi]) +
			                          u8"）###rx_bmtab" + g;
			if (segButton(label.c_str(), bmTab_ == g)) bmTab_ = g;
		}
		ImGui::SameLine(0, 12 * host_->scale);
		const int picks = pickCount() + sectionPickCount();
		ImGui::BeginDisabled(picks == 0);
		if (ImGui::SmallButton(u8"存成書籤")) {
			nameBuf_ = pageTitleIn(selGame_, pageId()) + " " + std::to_string(picks) + u8" 項";
			editIdx_ = -1;
			modal_ = Modal::Save;
		}
		ImGui::EndDisabled();
		if (picks == 0 && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(u8"先勾選幾項才有東西可以存");
		else if (ImGui::IsItemHovered())
			ImGui::SetTooltip(u8"把目前這一頁存成 %s 的書籤（放在未分類）：勾選、數值條件與數值、模式、輸出語言",
			                  GameLabel(selGame_));
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"新增資料夾")) {
			nameBuf_.clear();
			folderErr_.clear();
			modal_ = Modal::FolderAdd;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(u8"在 %s 的書籤裡新增一個資料夾（只有一層）", GameLabel(bmTab_));

		if (bmTab_ != selGame_) {
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextWrapped(u8"這是 %s 的書籤：「載入」會切換到 %s；「更新」要先在上方切到 %s。",
			                   GameLabel(bmTab_), GameLabel(bmTab_), GameLabel(bmTab_));
			ImGui::PopStyleColor();
		}

		const RegexFolders::Grouped grouped = RegexFolders::GroupBookmarks(state_, bmTab_, false);
		if (count[tabIdx] == 0 && !grouped.headers) {
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextWrapped(u8"%s 還沒有書籤。勾好一組常用的詞綴後按「存成書籤」，"
			                   u8"下次可以直接叫回來。", GameLabel(bmTab_));
			ImGui::PopStyleColor();
			drawOrphanNote(orphans);
			return;
		}

		ImGui::BeginChild("##rx_bm", ImVec2(0, 0), true);
		for (size_t gi = 0; gi < grouped.groups.size(); gi++) {
			const RegexFolders::Group& grp = grouped.groups[gi];
			ImGui::PushID((int)gi);
			if (grouped.headers) drawFolderHeader(grp);
			if (!grouped.headers || !grp.collapsed) {
				if (grouped.headers) ImGui::Indent(14 * host_->scale);
				if (grp.items.empty() && grouped.headers) {
					ImGui::TextDisabled(u8"（空的：把書籤拖到這裡）");
					if (ImGui::BeginDragDropTarget()) {
						if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("RX_BM"))
							bmAction_ = BmAction{BmAction::ToFolder, *(const int*)pl->Data, -1, 0, 0, false, grp.folder};
						ImGui::EndDragDropTarget();
					}
				}
				for (size_t k = 0; k < grp.items.size(); k++)
					drawBookmarkRow(grp.items[k], grp, k);
				if (grouped.headers) ImGui::Unindent(14 * host_->scale);
			}
			ImGui::PopID();
		}
		drawOrphanNote(orphans);
		ImGui::EndChild();
		applyBmAction();
	}

	// A folder's header (or "uncategorised"): fold arrow, name + count, grip and
	// buttons. A drop target for bookmarks (-> end of this folder) and folders
	// (upper / lower half -> before / after this one).
	void drawFolderHeader(const RegexFolders::Group& grp)
	{
		const bool uncat = grp.folder.empty();
		const std::vector<RegexBookmarkFolder>& list = state_.Folders(bmTab_);
		int fi = -1;
		for (int i = 0; i < (int)list.size(); i++)
			if (list[i].name == grp.folder) fi = i;
		ImGui::BeginGroup();
		if (ImGui::ArrowButton("##fold", grp.collapsed ? ImGuiDir_Right : ImGuiDir_Down))
			bmAction_ = BmAction{BmAction::Fold, -1, -1, 0, 0, !grp.collapsed, grp.folder};
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(grp.collapsed ? u8"展開" : u8"收合");
		if (!uncat) {
			ImGui::SameLine();
			ImGui::SmallButton(u8"::##fgrip");
			if (ImGui::IsItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
				ImGui::SetTooltip(u8"拖曳來排序資料夾");
			if (ImGui::BeginDragDropSource()) {
				ImGui::SetDragDropPayload("RX_FOLDER", &fi, sizeof fi);
				ImGui::Text(u8"移動資料夾：%s", grp.folder.c_str());
				ImGui::EndDragDropSource();
			}
		}
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		const std::string label = (uncat ? std::string(u8"未分類") : grp.folder) + u8"（" +
		                          std::to_string(grp.items.size()) + u8"）";
		ImGui::TextUnformatted(label.c_str());
		if (ImGui::IsItemClicked()) bmAction_ = BmAction{BmAction::Fold, -1, -1, 0, 0, !grp.collapsed, grp.folder};
		if (!uncat) {
			ImGui::SameLine();
			if (ImGui::SmallButton(u8"上移###fup")) bmAction_ = BmAction{BmAction::FolderStep, -1, -1, -1, 0, false, grp.folder};
			ImGui::SameLine(0, 2 * host_->scale);
			if (ImGui::SmallButton(u8"下移###fdown")) bmAction_ = BmAction{BmAction::FolderStep, -1, -1, 1, 0, false, grp.folder};
			ImGui::SameLine();
			if (ImGui::SmallButton(u8"改名###fren")) {
				folderEdit_ = grp.folder;
				nameBuf_ = grp.folder;
				folderErr_.clear();
				modal_ = Modal::FolderRename;
			}
			ImGui::SameLine();
			PobUi::PushDangerButton();
			if (ImGui::SmallButton(u8"刪除###fdel")) {
				folderEdit_ = grp.folder;
				modal_ = Modal::FolderDelete;
			}
			PobUi::PopButtonStyle();
		}
		ImGui::EndGroup();
		if (ImGui::BeginDragDropTarget()) {
			const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
			const bool lower = ImGui::GetMousePos().y > (r0.y + r1.y) * 0.5f;
			const ImGuiDragDropFlags f = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
			if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("RX_BM", f)) {
				ImGui::GetWindowDrawList()->AddRect(r0, r1, ImGui::GetColorU32(ImGuiCol_DragDropTarget), 3.0f, 0, 2.0f);
				if (pl->IsDelivery())
					bmAction_ = BmAction{BmAction::ToFolder, *(const int*)pl->Data, -1, 0, 0, false, grp.folder};
			}
			if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("RX_FOLDER", f)) {
				const float y = (uncat || lower) ? r1.y : r0.y;
				ImGui::GetWindowDrawList()->AddLine(ImVec2(r0.x, y), ImVec2(r1.x, y),
				                                    ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
				const int src = *(const int*)pl->Data;
				if (pl->IsDelivery() && src >= 0 && src < (int)list.size()) {
					// moveFolderTo: before / after this header; "uncategorised" = last.
					int to = uncat ? (int)list.size() - 1 : (lower ? fi + 1 : fi);
					if (!uncat && src < to) to--;
					bmAction_ = BmAction{BmAction::FolderTo, -1, -1, 0, to, false, list[src].name};
				}
			}
			ImGui::EndDragDropTarget();
		}
	}

	void drawBookmarkRow(int i, const RegexFolders::Group& grp, size_t k)
	{
		const RegexBookmark& b = state_.bookmarks[i];
		ImGui::PushID(i);
		ImGui::BeginGroup();
		ImGui::SmallButton(u8"::##grip");
		if (ImGui::IsItemHovered() && !ImGui::IsMouseDown(ImGuiMouseButton_Left))
			ImGui::SetTooltip(u8"拖曳來排序，或拖進資料夾");
		if (ImGui::BeginDragDropSource()) {
			ImGui::SetDragDropPayload("RX_BM", &i, sizeof i);
			ImGui::Text(u8"移動書籤：%s", b.name.c_str());
			ImGui::EndDragDropSource();
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(b.name.c_str());
		ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
		const char* modeZh = b.mode == "all" ? u8"全部都有"
		                   : b.mode == "none" ? u8"一個都沒有" : u8"含任一個";
		std::string meta = pageTitleIn(b.game, b.page) + u8" · " + modeZh + u8" · " +
		                   (b.lang == "en" ? "English" : u8"繁中") + u8" · " + std::to_string(b.keys.size()) + u8" 項";
		if (!b.num.empty())
			meta += (RegexAlgo::IsConditionSectionId(RegexAlgo::SectionIdOf(b.page)) ? std::string(u8" ＋ 稀有度 / 汙染")
			                                                                        : u8" ＋ 數值條件 " + std::to_string(b.num.size()) + u8" 項");
		ImGui::TextUnformatted(meta.c_str());
		ImGui::PopStyleColor();
		if (ImGui::SmallButton(u8"載入")) loadBookmark(i);
		ImGui::SameLine();
		const bool sameGame = b.game == selGame_;
		ImGui::BeginDisabled(!sameGame);
		if (ImGui::SmallButton(u8"更新")) updateBookmark(i);
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(sameGame ? u8"用目前這一頁的勾選、數值條件、模式與輸出語言覆寫這個書籤"
			                           : u8"這個書籤屬於另一個遊戲：先在上方切換遊戲才能更新");
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"改名")) {
			nameBuf_ = b.name;
			editIdx_ = i;
			modal_ = Modal::Rename;
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"移到…")) ImGui::OpenPopup("##mv");
		if (ImGui::BeginPopup("##mv")) {
			if (ImGui::Selectable(u8"未分類###mv_uncat", b.folder.empty()))
				bmAction_ = BmAction{BmAction::ToFolder, i, -1, 0, 0, false, std::string()};
			const std::vector<RegexBookmarkFolder>& list = state_.Folders(b.game);
			for (int f = 0; f < (int)list.size(); f++)
				if (ImGui::Selectable((list[f].name + "###mvf" + std::to_string(f)).c_str(), b.folder == list[f].name))
					bmAction_ = BmAction{BmAction::ToFolder, i, -1, 0, 0, false, list[f].name};
			if (list.empty()) ImGui::TextDisabled(u8"還沒有資料夾：先按上方的「新增資料夾」");
			ImGui::EndPopup();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"上移")) bmAction_ = BmAction{BmAction::Step, i, -1, -1, 0, false, std::string()};
		ImGui::SameLine(0, 2 * host_->scale);
		if (ImGui::SmallButton(u8"下移")) bmAction_ = BmAction{BmAction::Step, i, -1, 1, 0, false, std::string()};
		ImGui::SameLine();
		PobUi::PushDangerButton();
		if (ImGui::SmallButton(u8"刪除")) {
			editIdx_ = i;
			modal_ = Modal::Delete;
		}
		PobUi::PopButtonStyle();
		ImGui::EndGroup();
		// A bookmark dropped on this row lands in front of it (upper half) or
		// after it (lower half), and in its folder.
		if (ImGui::BeginDragDropTarget()) {
			const ImVec2 r0 = ImGui::GetItemRectMin(), r1 = ImGui::GetItemRectMax();
			const bool lower = ImGui::GetMousePos().y > (r0.y + r1.y) * 0.5f;
			const ImGuiDragDropFlags f = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
			if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("RX_BM", f)) {
				const float y = lower ? r1.y + 1 : r0.y - 1;
				ImGui::GetWindowDrawList()->AddLine(ImVec2(r0.x, y), ImVec2(r1.x, y),
				                                    ImGui::GetColorU32(ImGuiCol_DragDropTarget), 2.0f);
				if (pl->IsDelivery()) {
					const int src = *(const int*)pl->Data;
					if (!lower) bmAction_ = BmAction{BmAction::Before, src, i, 0, 0, false, std::string()};
					else if (k + 1 < grp.items.size())
						bmAction_ = BmAction{BmAction::Before, src, grp.items[k + 1], 0, 0, false, std::string()};
					else bmAction_ = BmAction{BmAction::ToFolder, src, -1, 0, 0, false, b.folder};
				}
			}
			ImGui::EndDragDropTarget();
		}
		ImGui::Separator();
		ImGui::PopID();
	}

	// A bookmark whose page id belongs to no loaded catalogue -- saved against a
	// list this build dropped, or against a Data file that is not installed. It
	// is still in regex_ui.json and still written back on every save; what it has
	// lost is a game to be filed under, so it is counted here rather than shown
	// as a row that no button could act on.
	void drawOrphanNote(int orphans)
	{
		if (orphans <= 0) return;
		ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
		ImGui::TextWrapped(u8"另有 %d 筆書籤存在這個版本沒有的清單上，沒有顯示"
		                   u8"（資料仍保留在 PobTools\\regex_ui.json）。", orphans);
		ImGui::PopStyleColor();
	}

	// store.ts loadBookmark / embed.ts bookmarkApplyOf: the bookmark's page (and
	// its section) is overwritten -- a bookmark is the whole page -- and every
	// other page keeps its ticks.
	void loadBookmark(int i)
	{
		if (i < 0 || i >= (int)state_.bookmarks.size()) return;
		// By value: everything below writes to state_.
		const RegexBookmark b = state_.bookmarks[i];
		const std::string g = b.game.empty() ? gameOfPage(b.page) : b.game;
		// store.ts loadBookmark: an item-mod values bookmark loads that page first
		// (here synchronously: the click is the moment, and it takes well under a second).
		if (RegexItemMods::IsPageId(b.page) && !g.empty() && !ensureItemModsNow(g)) {
			notice_ = u8"書籤「" + b.name + u8"」的物品詞綴資料載入失敗：" + imv_[GameIdx(g)].err;
			return;
		}
		const std::optional<RegexEmbed::BookmarkApply> a =
			g.empty() ? std::nullopt : RegexEmbed::BookmarkApplyOf(gamePages(g), b);
		const int target = a ? indexInGame(g, a->page) : -1;
		if (target < 0) {
			notice_ = u8"書籤「" + b.name + u8"」的清單「" + pageTitleById(b.page) +
			          u8"」在這個版本不存在，沒有載入。";
			return;
		}
		// The bookmark carries its own game: loading one never leaves the
		// selector pointing somewhere else.
		if (selGame_ != g) {
			selGame_ = g;
			combinedDirty_ = true;
		}
		state_.game = selGame_;
		switchPage(target);
		setView(View::Page);
		mode_ = ModeFromId(b.mode);
		state_.mode = modeId();
		setLang(b.lang == "en" ? Lang::En : Lang::Zh);
		for (const auto& p : a->picks) {
			const int idx = indexInGame(g, p.first);
			if (idx < 0) continue;
			setTicks(idx, p.second);
			syncCurrent(idx);
		}
		for (const auto& v : a->values)
			for (int idx = 0; idx < (int)refs_.size(); idx++) {
				if (!refs_[idx].algo || refs_[idx].Game() != g || RegexAlgo::NumericKeyOf(refs_[idx].Id()) != v.first)
					continue;
				for (const auto& kv : v.second)
					if (ownsValue(idx, kv.first)) pages_[idx].algo.values[kv.first] = kv.second;
				syncValues(idx);
			}
		for (PageState& ps : pages_) ps.dirty = true;   // mode / values changed under them
		combinedDirty_ = true;
		copied_ = false;
		notice_ = a->missed > 0
			? u8"已載入書籤「" + b.name + u8"」，但其中 " + std::to_string(a->missed) +
			  u8" 項在目前的資料裡找不到（賽季更新後詞條可能有變動）。"
			: u8"已載入書籤「" + b.name + u8"」。";
		state_.page = pageId();
		markStateDirty();
	}

	// store.ts updateBookmark: the page on screen overwrites the bookmark; its
	// name, folder (and exile-appraiser hotkey) stay.
	void updateBookmark(int i)
	{
		if (i < 0 || i >= (int)state_.bookmarks.size()) return;
		RegexBookmark& b = state_.bookmarks[i];
		if (b.game != selGame_) {
			notice_ = u8"書籤「" + b.name + u8"」屬於 " + GameLabel(b.game) + u8"，先切到那個遊戲再更新。";
			return;
		}
		std::optional<RegexBookmark> body = currentBookmarkBody();
		if (!body) {
			notice_ = u8"目前一項都沒有勾選，沒有更新書籤（要清空請改用刪除）。";
			return;
		}
		b.page = body->page;
		b.game = body->game;
		b.mode = body->mode;
		b.lang = body->lang;
		b.keys = std::move(body->keys);
		b.alt = std::move(body->alt);
		b.numeric = std::move(body->numeric);
		b.num = std::move(body->num);
		notice_ = u8"書籤「" + b.name + u8"」已更新為目前的勾選。";
		markStateDirty();
	}

	void drawModals()
	{
		// OpenPopup and BeginPopupModal must be called from the same ID scope, so
		// the request travels up here from whatever child window raised it.
		const Modal opening = modal_;
		modal_ = Modal::None;
		if (opening == Modal::Save || opening == Modal::Rename) {
			renameMode_ = (opening == Modal::Rename);
			ImGui::OpenPopup("###rx_name");
		} else if (opening == Modal::Delete) {
			ImGui::OpenPopup("###rx_del");
		} else if (opening == Modal::FolderAdd || opening == Modal::FolderRename) {
			folderRenameMode_ = (opening == Modal::FolderRename);
			ImGui::OpenPopup("###rx_folder");
		} else if (opening == Modal::FolderDelete) {
			ImGui::OpenPopup("###rx_fdel");
		} else if (opening == Modal::Paste) {
			ImGui::OpenPopup("###rx_paste");
		} else if (opening == Modal::Template) {
			ImGui::OpenPopup("###rx_tpl");
		}
		drawShareModals(opening);

		const std::string title = (renameMode_ ? std::string(u8"重新命名書籤")
		                                       : std::string(u8"存成書籤")) + "###rx_name";
		if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::SetNextItemWidth(320 * host_->scale);
			if (opening != Modal::None) ImGui::SetKeyboardFocusHere();
			const bool entered = ImGui::InputText(u8"名稱", &nameBuf_,
			                                      ImGuiInputTextFlags_EnterReturnsTrue);
			const bool ok = !nameBuf_.empty();
			ImGui::BeginDisabled(!ok);
			if (ImGui::Button(u8"確定", ImVec2(90 * host_->scale, 0)) || (entered && ok)) {
				commitName();
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button(u8"取消", ImVec2(90 * host_->scale, 0))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal(u8"刪除書籤###rx_del", nullptr,
		                           ImGuiWindowFlags_AlwaysAutoResize)) {
			const bool valid = editIdx_ >= 0 && editIdx_ < (int)state_.bookmarks.size();
			ImGui::TextUnformatted(valid
				? (u8"確定要刪除書籤「" + state_.bookmarks[editIdx_].name + u8"」？").c_str()
				: u8"這個書籤已經不在了。");
			ImGui::TextDisabled(u8"刪掉就沒有了，沒有復原。");
			PobUi::PushDangerButton();
			if (ImGui::Button(u8"刪除", ImVec2(90 * host_->scale, 0))) {
				if (valid) {
					notice_ = u8"已刪除書籤「" + state_.bookmarks[editIdx_].name + u8"」。";
					state_.bookmarks.erase(state_.bookmarks.begin() + editIdx_);
					markStateDirty();
				}
				editIdx_ = -1;
				ImGui::CloseCurrentPopup();
			}
			PobUi::PopButtonStyle();
			ImGui::SameLine();
			if (ImGui::Button(u8"取消", ImVec2(90 * host_->scale, 0))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		const std::string ftitle = (folderRenameMode_ ? std::string(u8"重新命名資料夾")
		                                              : std::string(u8"新增資料夾")) + "###rx_folder";
		if (ImGui::BeginPopupModal(ftitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextDisabled(u8"%s 的書籤", GameLabel(bmTab_));
			ImGui::SetNextItemWidth(320 * host_->scale);
			if (opening != Modal::None) ImGui::SetKeyboardFocusHere();
			const bool entered = ImGui::InputText(u8"名稱###fname", &nameBuf_, ImGuiInputTextFlags_EnterReturnsTrue);
			if (!folderErr_.empty()) ImGui::TextColored(kBad, "%s", folderErr_.c_str());
			if (ImGui::Button(u8"確定", ImVec2(90 * host_->scale, 0)) || entered) {
				const RegexFolders::Result r = folderRenameMode_
					? RegexFolders::Rename(state_, bmTab_, folderEdit_, nameBuf_)
					: RegexFolders::Add(state_, bmTab_, nameBuf_);
				if (r == RegexFolders::Result::Ok) {
					const std::string n = RegexFolders::NormalizeName(nameBuf_);
					notice_ = folderRenameMode_ ? u8"資料夾已改名為「" + n + u8"」。" : u8"已新增資料夾「" + n + u8"」。";
					markStateDirty();
					ImGui::CloseCurrentPopup();
				} else {
					folderErr_ = r == RegexFolders::Result::Empty ? u8"名稱不能是空白。"
					           : r == RegexFolders::Result::Duplicate ? u8"已經有同名的資料夾。"
					                                                  : u8"這個資料夾已經不在了。";
				}
			}
			ImGui::SameLine();
			if (ImGui::Button(u8"取消", ImVec2(90 * host_->scale, 0))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal(u8"刪除資料夾###rx_fdel", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			const int n = RegexFolders::Counts(state_, bmTab_)[folderEdit_];
			ImGui::Text(u8"確定要刪除資料夾「%s」？", folderEdit_.c_str());
			ImGui::TextDisabled(u8"裡面的 %d 筆書籤會移回未分類，書籤本身不會刪除。", n);
			PobUi::PushDangerButton();
			if (ImGui::Button(u8"刪除", ImVec2(90 * host_->scale, 0))) {
				const int moved = RegexFolders::Delete(state_, bmTab_, folderEdit_);
				if (moved >= 0) {
					notice_ = u8"已刪除資料夾「" + folderEdit_ + u8"」，" + std::to_string(moved) + u8" 筆書籤移回未分類。";
					markStateDirty();
				}
				ImGui::CloseCurrentPopup();
			}
			PobUi::PopButtonStyle();
			ImGui::SameLine();
			if (ImGui::Button(u8"取消", ImVec2(90 * host_->scale, 0))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}
	}

	void commitName()
	{
		if (editIdx_ >= 0) {
			if (editIdx_ < (int)state_.bookmarks.size()) {
				state_.bookmarks[editIdx_].name = nameBuf_;
				notice_ = u8"書籤已改名為「" + nameBuf_ + u8"」。";
				markStateDirty();
			}
		} else if (std::optional<RegexBookmark> body = currentBookmarkBody()) {
			// store.ts saveBookmark: appended = this game's uncategorised, which
			// sorts last, so the order invariant holds without a sort.
			body->name = nameBuf_;
			state_.bookmarks.push_back(std::move(*body));
			bmTab_ = selGame_;
			notice_ = u8"已存成書籤「" + nameBuf_ + u8"」。";
			markStateDirty();
		}
		editIdx_ = -1;
	}

	// ---- share codes and templates (R8) ---------------------------------------
	//
	// exile-appraiser RegexPanel.vue head tools row: template drop-down, copy /
	// paste share code. store.ts currentShareState / makeShareCode / applyCombo.

	// Is the item-mod values page of game g still unloaded while the saved state has ticks on it?
	bool itemTicksPending(const std::string& g) const
	{
		const int ip = itemPageIndex(g);
		if (ip < 0 || imv_[GameIdx(g)].phase == ItemModLoad::Phase::Ready) return false;
		for (const RegexPagePicks& c : state_.current)
			if (c.page == refs_[ip].Id() && !c.keys.empty()) return true;
		return false;
	}

	bool hasShareable() const
	{
		return !combineOrderIdx(true).empty() || !state_.custom.empty() || !state_.excludes.empty() ||
		       itemTicksPending(selGame_);
	}

	// RegexPanel.vue openPaste: /^[A-Za-z0-9_-]{16,}$/
	static bool LooksLikeCode(const std::string& s)
	{
		if (s.size() < 16) return false;
		for (char c : s)
			if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
		return true;
	}

	void drawShareRow()
	{
		// Templates of the selected game (RegexPanel.vue myTemplates).
		int mine = 0;
		for (const RegexShare::Template& t : templates_) mine += t.game == selGame_ ? 1 : 0;
		const std::string ph = u8"套用範本…（" + std::to_string(mine) + u8"）";
		ImGui::SetNextItemWidth(200 * host_->scale);
		ImGui::BeginDisabled(mine == 0);
		if (ImGui::BeginCombo("##rx_tplcombo", ph.c_str())) {
			for (int i = 0; i < (int)templates_.size(); i++) {
				const RegexShare::Template& t = templates_[i];
				if (t.game != selGame_) continue;
				if (ImGui::Selectable((t.nameZh + "###tpl" + std::to_string(i)).c_str(), false)) {
					tplPending_ = i;
					modal_ = Modal::Template;
				}
				if (ImGui::IsItemHovered() && !t.descZh.empty()) {
					ImGui::BeginTooltip();
					ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28);
					ImGui::TextUnformatted(t.descZh.c_str());
					ImGui::PopTextWrapPos();
					ImGui::EndTooltip();
				}
			}
			ImGui::EndCombo();
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			if (!templatesErr_.empty()) ImGui::SetTooltip(u8"範本檔載入失敗：%s", templatesErr_.c_str());
			else if (mine == 0) ImGui::SetTooltip(u8"%s 沒有內建範本", GameLabel(selGame_));
			else ImGui::SetTooltip(u8"內建的常用組合；套用前會先確認（會覆蓋 %s 目前所有清單的勾選）", GameLabel(selGame_));
		}
		ImGui::SameLine();
		const bool any = hasShareable();
		ImGui::BeginDisabled(!any);
		if (ImGui::SmallButton(u8"複製分享碼")) makeShareCode();
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
			ImGui::TextUnformatted(any ? u8"把目前遊戲所有清單的勾選、數值、自訂文字與排除詞壓成一串分享碼。"
			                           : u8"先勾選幾項（或加自訂文字 / 排除詞）才有東西可以分享。");
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextUnformatted(u8"分享碼與 exile-appraiser 互通；只有「物品詞綴數值」頁例外："
			                       u8"這裡的鍵是 GGPK stat id、那邊是交易站 stat id，"
			                       u8"所以那一頁的勾選對方讀不到（對方的也讀不進來，會回報找不到幾項）。");
			ImGui::PopStyleColor();
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"貼上分享碼")) {
			// RegexPanel.vue openPaste: pre-filled when the clipboard looks like a code
			pasteBuf_.clear();
			pasteErr_.clear();
			const std::string clip = RegexAlgo::JsTrim(ReadClipboardUtf8(host_ ? host_->hostHwnd : nullptr));
			if (LooksLikeCode(clip)) pasteBuf_ = clip;
			modal_ = Modal::Paste;
		}
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"貼上別人給的分享碼並套用（套用前會先確認）");
		if (shareCopied_) {
			if (std::chrono::steady_clock::now() - shareCopiedAt_ > std::chrono::milliseconds(2500)) {
				shareCopied_ = 0;
			} else {
				ImGui::SameLine();
				if (shareCopied_ == 1) ImGui::TextColored(kGood, u8"已複製分享碼");
				else ImGui::TextColored(kBad, u8"複製失敗");
			}
		}
	}

	// store.ts makeShareCode / currentShareState: every page of the game with ticks.
	void makeShareCode()
	{
		// Ticks saved on the item-mod page but not restored yet (it loads in the
		// background): load it now, or the code would silently leave them out.
		if (itemTicksPending(selGame_) && !ensureItemModsNow(selGame_)) {
			notice_ = u8"物品詞綴資料載入失敗（" + imv_[GameIdx(selGame_)].err + u8"），沒有產生分享碼：那一頁的勾選會漏掉。";
			return;
		}
		RegexEmbed::PicksMap picks;
		RegexEmbed::ValuesMap values;
		for (int i = 0; i < (int)refs_.size(); i++) {
			if (refs_[i].Game() != selGame_) continue;
			picks[refs_[i].Id()] = picksOf(i);
			if (refs_[i].algo) {
				RegexAlgo::ValueMap& dst = values[RegexAlgo::NumericKeyOf(refs_[i].Id())];
				for (const auto& kv : pages_[i].algo.values)
					if (ownsValue(i, kv.first)) dst[kv.first] = kv.second;
			}
		}
		const RegexShare::State st = RegexShare::StateOf(selGame_, gamePages(selGame_), picks, values, modeId(),
		                                                 state_.custom, state_.excludes);
		shareCopyRequest_ = RegexShare::Encode(st);
		shareCopied_ = 0;
	}

	// store.ts applyCombo: OVERWRITE every page of the code's game (ticks; values
	// merged in), custom text, excludes and mode; show the merged view. Returns
	// false (with *err, nothing changed) when it cannot be applied at all.
	bool applyCombo(const RegexShare::State& s, const std::string& what, const std::vector<std::string>& warnings,
	                std::string* err)
	{
		const std::string g = s.game;
		if (firstPageOf(g) < 0) {
			if (err) *err = std::string(u8"這個安裝沒有 ") + GameLabel(g) + u8" 的清單（Data\\regex_" + g + u8".json），無法套用。";
			return false;
		}
		// The item-mod values page resolves against its entries: load it first (store.ts prepareItemMods).
		const std::string itemId = RegexItemMods::PageId(g);
		bool needItem = false;
		for (const auto& kv : s.pages) needItem = needItem || kv.first == itemId;
		for (const auto& kv : s.numeric) needItem = needItem || kv.first == itemId;
		if (needItem && itemPageIndex(g) >= 0 && !ensureItemModsNow(g)) {
			if (err) *err = u8"物品詞綴資料載入失敗（" + imv_[GameIdx(g)].err + u8"），沒有套用。";
			return false;
		}
		if (selGame_ != g) switchGame(g);
		const RegexShare::Resolved r = RegexShare::Resolve(s, gamePages(g));
		for (int idx = 0; idx < (int)refs_.size(); idx++) {
			if (refs_[idx].Game() != g) continue;
			auto it = r.picks.find(refs_[idx].Id());
			setTicks(idx, it != r.picks.end() ? it->second : std::vector<int>());
			syncCurrent(idx);
		}
		for (const auto& kv : RegexShare::ResolvedValues(r.values))
			for (int idx = 0; idx < (int)refs_.size(); idx++) {
				if (!refs_[idx].algo || refs_[idx].Game() != g || RegexAlgo::NumericKeyOf(refs_[idx].Id()) != kv.first) continue;
				for (const auto& e : kv.second)
					if (ownsValue(idx, e.first)) pages_[idx].algo.values[e.first] = e.second;
				syncValues(idx);
			}
		state_.custom = s.custom;
		state_.excludes = s.excludes;
		mode_ = ModeFromId(s.mode);
		state_.mode = modeId();
		state_.game = selGame_;
		setScope(true);
		setView(View::Combined);
		for (PageState& ps : pages_) {
			ps.dirty = true;
			ps.filterDirty = true;
		}
		combinedDirty_ = true;
		copied_ = false;
		markStateDirty();

		std::string msg = u8"已套用" + what;
		if (r.missed > 0 || !r.unknownPages.empty()) {
			std::string pages;
			for (const std::string& id : r.unknownPages) pages += (pages.empty() ? "" : u8"、") + id;
			msg += u8"，但有 " + std::to_string(r.missed) + u8" 項在目前資料找不到";
			if (!pages.empty()) msg += u8"（不存在的清單：" + pages + u8"）";
			for (const auto& kv : r.missedByPage)
				if (RegexItemMods::IsPageId(kv.first))
					msg += u8"；其中 " + std::to_string(kv.second) + u8" 項在「物品詞綴數值」頁："
					       u8"這一頁的鍵是 GGPK stat id，與 exile-appraiser 的交易站 stat id 不互通";
		}
		msg += u8"。";
		if (!warnings.empty()) {
			msg += u8"分享碼有 " + std::to_string(warnings.size()) + u8" 處格式不對，已略過：";
			for (size_t i = 0; i < warnings.size() && i < 3; i++) msg += (i ? u8"；" : "") + warnings[i];
			if (warnings.size() > 3) msg += u8"…";
		}
		notice_ = msg;
		for (const std::string& w : warnings) PobLog::Diag("regex", u8"分享碼警告：" + w);
		return true;
	}

	void drawShareModals(Modal opening)
	{
		if (ImGui::BeginPopupModal(u8"貼上分享碼###rx_paste", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			ImGui::TextUnformatted(u8"把別人給的分享碼貼在下面（剪貼簿裡像分享碼的內容會自動帶入）。");
			if (opening != Modal::None) ImGui::SetKeyboardFocusHere();
			ImGui::InputTextMultiline("##rx_paste_code", &pasteBuf_, ImVec2(440 * host_->scale, 90 * host_->scale));
			ImGui::PushTextWrapPos(440 * host_->scale);
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextUnformatted(u8"套用後會覆蓋分享碼那個遊戲目前所有清單的勾選、數值、自訂文字、排除詞與模式"
			                       u8"（不影響書籤；要保留目前的勾選，先存成書籤）。");
			ImGui::PopStyleColor();
			if (!pasteErr_.empty()) ImGui::TextColored(kBad, "%s", pasteErr_.c_str());
			ImGui::PopTextWrapPos();
			const bool empty = RegexAlgo::JsTrim(pasteBuf_).empty();
			ImGui::BeginDisabled(empty);
			if (ImGui::Button(u8"套用", ImVec2(90 * host_->scale, 0))) {
				RegexShare::Normalized d;
				std::string err;
				if (!RegexShare::Decode(pasteBuf_, d, &err)) {
					pasteErr_ = u8"分享碼無法套用：" + err;
					if (err.find(u8"版本不符") != std::string::npos)
						pasteErr_ += u8"（可能是較新版本的 PobTools / exile-appraiser 產的）";
				} else if (!applyCombo(d.state, std::string(u8"分享碼（") + GameLabel(d.state.game) + u8"）", d.warnings, &err)) {
					pasteErr_ = u8"分享碼無法套用：" + err;
				} else {
					ImGui::CloseCurrentPopup();
				}
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button(u8"從剪貼簿貼上")) {
				pasteBuf_ = RegexAlgo::JsTrim(ReadClipboardUtf8(host_ ? host_->hostHwnd : nullptr));
				pasteErr_.clear();
			}
			ImGui::SameLine();
			if (ImGui::Button(u8"取消", ImVec2(90 * host_->scale, 0))) ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
		}

		if (ImGui::BeginPopupModal(u8"套用範本###rx_tpl", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			const bool valid = tplPending_ >= 0 && tplPending_ < (int)templates_.size();
			if (!valid) {
				ImGui::TextUnformatted(u8"這個範本已經不在了。");
			} else {
				const RegexShare::Template& t = templates_[tplPending_];
				ImGui::Text(u8"套用範本「%s」", t.nameZh.c_str());
				ImGui::PushTextWrapPos(420 * host_->scale);
				if (!t.descZh.empty()) ImGui::TextUnformatted(t.descZh.c_str());
				ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
				ImGui::Text(u8"會覆蓋 %s 目前所有清單的勾選、數值、自訂文字、排除詞與模式（不影響書籤）。", GameLabel(t.game));
				ImGui::PopStyleColor();
				ImGui::PopTextWrapPos();
			}
			ImGui::BeginDisabled(!valid);
			if (ImGui::Button(u8"套用", ImVec2(90 * host_->scale, 0))) {
				const RegexShare::Template t = templates_[tplPending_];
				std::string err;
				if (!applyCombo(t.state, u8"範本「" + t.nameZh + u8"」", {}, &err)) notice_ = u8"範本無法套用：" + err;
				tplPending_ = -1;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			if (ImGui::Button(u8"取消", ImVec2(90 * host_->scale, 0))) {
				tplPending_ = -1;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	}

	// ---- plumbing ------------------------------------------------------------

	bool pageHasT17()
	{
		if (t17Cache_ != page_) {
			t17Cache_ = page_;
			t17Present_ = false;
			for (const RegexEntryDef& e : entries())
				if (e.t17) { t17Present_ = true; break; }
		}
		return t17Present_;
	}

	void setLang(Lang l)
	{
		if (l == lang_) return;
		lang_ = l;
		state_.lang = (l == Lang::En) ? "en" : "zh";
		stateDirty_ = true;
		copied_ = false;
		invalidateCorpora();
	}

	void switchPage(int p)
	{
		if (p >= 0 && p < (int)refs_.size() && isItemPage(p)) startItemMods(refs_[p].Game());
		if (p == page_) return;
		page_ = p;
		copied_ = false;
		st().filterDirty = true;
		st().dirty = true;
		state_.game = selGame_;
		state_.page = pageId();
		stateDirty_ = true;
	}

	// Moving to a game moves to its first list. Nothing is thrown away: every
	// page keeps its own ticks, so coming back finds the work where it was left.
	void switchGame(const std::string& g)
	{
		if (g == selGame_) return;
		const int first = firstPageOf(g);
		if (first < 0) return;
		selGame_ = g;
		combinedDirty_ = true;   // the merge is per game
		switchPage(first);
		state_.game = selGame_;
		stateDirty_ = true;
	}

	void refreshFilter()
	{
		PageState& s = st();
		const std::string needle = ToLowerAscii(s.search);
		s.visible.clear();
		// Ticked first, then the rest, each keeping the data file's order. Two
		// passes rather than a sort: a sort would need a comparator that is a
		// strict weak ordering over "is it ticked", and this says the same thing
		// in a way that cannot silently shuffle equal rows between frames.
		for (int pass = 0; pass < 2; pass++) {
			const bool wantPicked = (pass == 0);
			for (int i = 0; i < (int)entries().size(); i++) {
				const bool isPicked = i < (int)s.picked.size() && s.picked[i] != 0;
				if (isPicked != wantPicked) continue;
				const RegexEntryDef& e = entries()[i];
				if (s.groupFilter >= 0 && e.group != s.groupFilter) continue;
				if (s.t17Only && !e.t17) continue;
				if (s.hideT17 && e.t17) continue;
				if (!needle.empty() && !matches(e, needle)) continue;
				s.visible.push_back(i);
			}
		}
		s.filterDirty = false;
	}

	// Chinese, English, the affix name and the GGPK id all count as searchable:
	// people look for 反射 and for "reflect" and occasionally for the mod id off a
	// wiki page, and the cheapest way to be right is to accept all of them.
	static bool matches(const RegexEntryDef& e, const std::string& needle)
	{
		for (const std::string& l : e.zh)
			if (l.find(needle) != std::string::npos) return true;
		for (const std::string& l : e.en)
			if (ToLowerAscii(l).find(needle) != std::string::npos) return true;
		if (!e.affixZh.empty() && e.affixZh.find(needle) != std::string::npos) return true;
		return ToLowerAscii(e.id).find(needle) != std::string::npos;
	}

	// The corpus is every entry on the page, not just the ticked ones: "does this
	// token also hit something else?" is a question about the whole list, and
	// building it from the selection would make the answer change as the player
	// ticks -- which is exactly the bug that produces false positives.
	//
	// The lines are the ones in the language being built for: "no false
	// positives" is a claim about ONE language's list, so the corpus has to be
	// the one the player will paste into (RegexAlgo::BuildPageCorpus, shared
	// with --regex-selftest).
	void buildCorpus()
	{
		PageState& s = st();
		RegexAlgo::BuildPageCorpus(*refs_[page_].corpus, fragLang(), s.corpus);
		s.corpusReady = true;
		s.dirty = true;
	}

	RegexFrag::Lang fragLang() const { return lang_ == Lang::Zh ? RegexFrag::Lang::Zh : RegexFrag::Lang::En; }

	// The page's string: its own picks, plus its numeric section for a host
	// page, in one go (exile-appraiser store.ts pageCombined). With no section
	// picks this is exactly the corpus Build() query, as before.
	void recompute()
	{
		PageState& s = st();
		std::vector<RegexAlgo::CombineSel> sels;
		RegexAlgo::CombineSel own;
		own.page = refs_[page_];
		if (isAlgo()) {
			own.picks = s.algo.Picks();
			own.values = &s.algo.values;
		} else {
			for (int i = 0; i < (int)s.picked.size(); i++)
				if (s.picked[i]) own.picks.push_back(i);
			own.corpus = &s.corpus;
		}
		sels.push_back(own);
		const int sec = sectionIndexOf(page_);
		if (sec >= 0) {
			RegexAlgo::CombineSel b;
			b.page = refs_[sec];
			b.picks = pages_[sec].algo.Picks();
			b.values = &pages_[sec].algo.values;
			sels.push_back(b);
		}
		s.combined = RegexAlgo::CombineSingle(fragLang(), mode_, sels);
		s.result = s.combined.corpusResult;
		s.dirty = false;
	}

	int sectionPickCount() const
	{
		if (!hasPage()) return 0;
		const int sec = sectionIndexOf(page_);
		return sec >= 0 ? pages_[sec].algo.Count() : 0;
	}

	// ---- algorithmic rows (vendor page, numeric section) -----------------------
	//
	// Row layout after exile-appraiser RegexAlgoList.vue: tick + name | input |
	// the fragment this row produces. Editing a value ticks the row.

	static std::string NumText(double v)
	{
		char buf[32];
		if (std::floor(v) == v && std::fabs(v) < 1e15) snprintf(buf, sizeof buf, "%.0f", v);
		else snprintf(buf, sizeof buf, "%g", v);
		return buf;
	}

	// A number box that can be empty. Returns true when edited; `out` is the
	// new value, nullopt when the box was cleared or holds no number (TS:
	// `raw === '' || !Number.isFinite(n)` deletes the bound).
	bool numField(const char* id, const std::optional<double>& val, std::optional<double>& out)
	{
		std::string buf = val ? NumText(*val) : std::string();
		ImGui::SetNextItemWidth(56 * host_->scale);
		if (!ImGui::InputText(id, &buf, ImGuiInputTextFlags_CharsDecimal)) return false;
		size_t a = buf.find_first_not_of(" \t");
		if (a == std::string::npos) {
			out.reset();
			return true;
		}
		const std::string t = buf.substr(a);
		char* end = nullptr;
		const double v = std::strtod(t.c_str(), &end);
		if (end && *end == '\0' && std::isfinite(v)) out = v;
		else out.reset();
		return true;
	}

	bool segButton(const char* label, bool on)
	{
		if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		const bool r = ImGui::SmallButton(label);
		if (on) ImGui::PopStyleColor();
		return r;
	}

	// SameLine when an item `w` wide still fits in the cell, else the next line
	// (the .rx-algo-input flex-wrap of RegexAlgoList.vue): a row of buttons in a
	// narrow input column wraps instead of running under the fragment column.
	void sameLineOrWrap(float w, float spacing)
	{
		// GetContentRegionMax is the cell's work rect inside a table (window-relative)
		const float right = ImGui::GetWindowPos().x + ImGui::GetContentRegionMax().x;
		if (ImGui::GetItemRectMax().x + spacing + w <= right) ImGui::SameLine(0, spacing);
	}
	float smallButtonWidth(const char* label) const
	{
		return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
	}

	void drawAlgoRow(int idx, int i)
	{
		using namespace RegexAlgo;
		PageState& s = pages_[idx];
		const AlgoPage& page = *refs_[idx].algo;
		const AlgoEntry& e = page.entries[i];
		ImGui::PushID(i);
		bool on = s.algo.picked[i] != 0;
		if (on)
			ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImGuiCol_Header, 0.55f));

		ImGui::TableSetColumnIndex(0);
		if (ImGui::Checkbox("##on", &on)) {
			s.algo.picked[i] = on ? 1 : 0;
			algoChanged(idx);
		}
		ImGui::SameLine();
		ImGui::TextUnformatted(e.def.zh.empty() ? e.def.id.c_str() : e.def.zh[0].c_str());
		if (ImGui::IsItemHovered() && !e.def.en.empty()) ImGui::SetTooltip("%s", e.def.en[0].c_str());
		if (e.untested) {
			ImGui::SameLine();
			ImGui::TextColored(kWarn, u8"待實測");
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"假設繁中客戶端的插槽顯示為 R-G-B（顏色字母與 - 不翻譯），尚未進遊戲確認。");
		}

		ImGui::TableSetColumnIndex(1);
		const AlgoValue cur = ValueOf(s.algo.values, e);   // a copy: the edit below replaces it
		std::optional<AlgoValue> next;
		std::optional<double> n;
		switch (e.input.kind) {
		case InputKind::Range: {
			const RangeOp op = OpOf(e, cur);
			if (e.input.ops.size() > 1) {
				for (size_t k = 0; k < e.input.ops.size(); k++) {
					const RangeOp o = e.input.ops[k];
					if (k) ImGui::SameLine(0, 2 * host_->scale);
					const char* label = o == RangeOp::Ge ? u8"≥" : o == RangeOp::Le ? u8"≤" : u8"區間";
					if (segButton(label, op == o)) next = WithOp(e, cur, o);
				}
			} else {
				ImGui::AlignTextToFramePadding();
				ImGui::TextDisabled(u8"≥");
			}
			if (op != RangeOp::Le) {
				ImGui::SameLine();
				if (numField("##min", cur.min, n)) next = WithNum(e, cur, false, n);
			}
			if (op == RangeOp::Range) {
				ImGui::SameLine();
				ImGui::TextDisabled(u8"–");
			}
			if (op != RangeOp::Ge) {
				ImGui::SameLine();
				if (numField("##max", cur.max, n)) next = WithNum(e, cur, true, n);
			}
			if (e.input.percent) {
				ImGui::SameLine();
				ImGui::TextDisabled("%%");
			}
			break;
		}
		case InputKind::Select: {
			const std::vector<AlgoOption>& opts = e.input.options;
			if (opts.size() > 5) {
				// Many options (the eight influences): a drop-down keeps the row narrow.
				std::string curText = cur.choice;
				for (const AlgoOption& o : opts)
					if (o.id == cur.choice) curText = o.zh;
				ImGui::SetNextItemWidth(130 * host_->scale);
				if (ImGui::BeginCombo("##choice", curText.c_str())) {
					for (const AlgoOption& o : opts)
						if (ImGui::Selectable(o.zh.c_str(), o.id == cur.choice)) next = WithChoice(cur, o.id);
					ImGui::EndCombo();
				}
			} else {
				for (size_t k = 0; k < opts.size(); k++) {
					if (k) ImGui::SameLine(0, 2 * host_->scale);
					if (segButton(opts[k].zh.c_str(), cur.choice == opts[k].id)) next = WithChoice(cur, opts[k].id);
				}
			}
			break;
		}
		case InputKind::Count: {
			const std::vector<AlgoOption>& opts = e.input.options;
			for (size_t k = 0; k < opts.size(); k++) {
				if (k) ImGui::SameLine(0, 2 * host_->scale);
				if (segButton(opts[k].zh.c_str(), cur.choice == opts[k].id)) next = WithChoice(cur, opts[k].id);
			}
			ImGui::SameLine();
			ImGui::TextDisabled(u8"≥");
			ImGui::SameLine();
			if (numField("##min", cur.min, n)) next = WithNum(e, cur, false, n);
			break;
		}
		case InputKind::Rarity: {
			// RegexAlgoList.vue (step 40, B d5ccb47):
			// "普通 魔法 稀有 傳奇 | 未汙染 已汙染" -- rarities multi-select, corruption
			// one of two (clicking the lit one clears it), a divider between
			const RarityChoice rc = ParseRarityChoice(cur.choice);
			const float sp = 2 * host_->scale;
			bool first = true;
			for (const AlgoOption& o : e.input.options) {
				const std::string label = o.zh + "###r_" + o.id;
				if (!first) sameLineOrWrap(smallButtonWidth(label.c_str()), sp);
				first = false;
				const bool lit = std::find(rc.rarity.begin(), rc.rarity.end(), o.id) != rc.rarity.end();
				if (segButton(label.c_str(), lit)) next = WithChoice(cur, ToggleRarityIn(cur.choice, o.id));
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"%s（可多選）", o.en.c_str());
			}
			{
				// the divider: a thin vertical rule the height of a button
				const float h = ImGui::GetFrameHeight() - ImGui::GetStyle().FramePadding.y * 2;
				const float dw = 9 * host_->scale;
				sameLineOrWrap(dw, sp);
				const ImVec2 p = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(dw, h > 0 ? h : ImGui::GetTextLineHeight()));
				const float x = p.x + dw * 0.5f;
				ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p.y + 1), ImVec2(x, p.y + (h > 0 ? h : ImGui::GetTextLineHeight()) - 1),
				                                    ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
			}
			for (const AlgoOption& o : e.input.corruption) {
				const std::string label = o.zh + "###c_" + o.id;
				sameLineOrWrap(smallButtonWidth(label.c_str()), sp);
				const Corruption c = o.id == "uncorrupted" ? Corruption::Uncorrupted : Corruption::Corrupted;
				if (segButton(label.c_str(), rc.corruption == c)) next = WithChoice(cur, ToggleCorruptionIn(cur.choice, c));
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(c == Corruption::Uncorrupted ? u8"排除「已汙染」行（再點一次取消）"
					                                               : u8"只要有「已汙染」行（再點一次取消）");
			}
			break;
		}
		case InputKind::Colors: {
			static const char kLetters[3] = {'r', 'g', 'b'};
			static const ImVec4 kDot[3] = {ImVec4(0.90f, 0.35f, 0.30f, 1), ImVec4(0.40f, 0.80f, 0.45f, 1), ImVec4(0.40f, 0.60f, 0.95f, 1)};
			for (int k = 0; k < 3; k++) {
				if (k) ImGui::SameLine();
				ImGui::PushID(k);
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(kDot[k], "%c", kLetters[k] - 'a' + 'A');
				ImGui::SameLine(0, 3 * host_->scale);
				const std::optional<double> count = (double)ColorCount(cur, kLetters[k]);
				if (numField("##n", count, n)) {
					// TS: Math.trunc(Number(value) || 0), clamped 0..6
					const int c = n ? (int)std::trunc(*n) : 0;
					next = WithColor(cur, kLetters[k], c);
				}
				ImGui::PopID();
			}
			break;
		}
		}
		if (next) {
			s.algo.SetValue(page, i, *next);   // editing a value ticks the row
			algoChanged(idx);
		}

		ImGui::TableSetColumnIndex(2);
		const std::optional<std::string> f = e.fragment(ValueOf(s.algo.values, e), fragLang());
		ImGui::AlignTextToFramePadding();
		if (f) {
			// Wrapped inside the cell, never clipped: a fragment is the thing to
			// check before pasting, so all of it has to be readable. Only rows whose
			// fragment is longer than the column grow (a long token is cut anywhere).
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::PushTextWrapPos(0.0f);
			ImGui::TextUnformatted(f->c_str());
			ImGui::PopTextWrapPos();
			ImGui::PopStyleColor();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f->c_str());
		} else {
			ImGui::TextColored(kBad, u8"（輸入不成立）");
		}
		ImGui::PopID();
	}

	void drawAlgoRows(int idx)
	{
		const RegexAlgo::AlgoPage& page = *refs_[idx].algo;
		for (int g = 0; g < (int)page.groups.size(); g++) {
			bool any = false;
			for (const RegexAlgo::AlgoEntry& e : page.entries) any |= (e.def.group == g);
			if (!any) continue;
			if (page.groups.size() > 1) ImGui::TextDisabled("%s", page.groups[g].c_str());
			ImGui::PushID(g);
			const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH;
			if (ImGui::BeginTable("##rx_algo", 3, flags)) {
				// Name | input | fragment. The fragment wraps in its cell (drawAlgoRow), so
				// it gets the larger share; the input column wraps its buttons.
				ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupColumn("input", ImGuiTableColumnFlags_WidthStretch, 1.35f);
				ImGui::TableSetupColumn("frag", ImGuiTableColumnFlags_WidthStretch, 1.65f);
				for (int i = 0; i < (int)page.entries.size(); i++) {
					if (page.entries[i].def.group != g) continue;
					ImGui::TableNextRow();
					drawAlgoRow(idx, i);
				}
				ImGui::EndTable();
			}
			ImGui::PopID();
		}
	}

	// The vendor page: the rows are the whole list.
	void drawAlgoPage(int idx)
	{
		PageState& s = pages_[idx];
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(u8"每個勾選各自一個條件（同時成立）；改數值會自動勾選。");
		ImGui::SameLine();
		ImGui::BeginDisabled(s.algo.Count() == 0);
		if (ImGui::SmallButton(u8"清除")) {
			std::fill(s.algo.picked.begin(), s.algo.picked.end(), (char)0);
			algoChanged(idx);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(u8"取消所有勾選");
		ImGui::BeginChild("##rx_algo_rows", ImVec2(0, 0), true);
		drawAlgoRows(idx);
		ImGui::EndChild();
	}

	// ---- item-mod values page (R7, RegexItemModList.vue) ---------------------------

	static int GameIdx(const std::string& g) { return g == "poe2" ? 1 : 0; }
	bool isItemPage(int idx) const
	{
		return idx >= 0 && idx < (int)refs_.size() && refs_[idx].algo && RegexItemMods::IsPageId(refs_[idx].Id());
	}
	int itemPageIndex(const std::string& g) const
	{
		for (int i = 0; i < (int)refs_.size(); i++)
			if (refs_[i].Game() == g && isItemPage(i)) return i;
		return -1;
	}

	// store.ts ensureItemMods: start the background load (once; again after an error).
	void startItemMods(const std::string& g)
	{
		ItemModLoad& L = imv_[GameIdx(g)];
		if (L.phase == ItemModLoad::Phase::Ready || L.phase == ItemModLoad::Phase::Loading) return;
		if (itemPageIndex(g) < 0) return;
		if (L.worker.joinable()) L.worker.join();
		L.phase = ItemModLoad::Phase::Loading;
		L.err.clear();
		L.result.reset();
		L.done = false;
		const std::wstring dir = exeDir_;
		ItemModLoad* lp = &L;
		L.worker = std::thread([lp, dir, g]() {
			const auto t0 = std::chrono::steady_clock::now();
			std::unique_ptr<RegexItemMods::Data> d(new RegexItemMods::Data);
			std::string err;
			const bool ok = RegexItemMods::LoadFile(dir, g, *d, &err);
			lp->resultMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			lp->resultErr = err;
			if (ok) lp->result = std::move(d);
			lp->done = true;
		});
	}

	void pollItemMods()
	{
		for (int gi = 0; gi < 2; gi++)
			if (imv_[gi].phase == ItemModLoad::Phase::Loading && imv_[gi].done) finishItemMods(gi);
	}

	// Load now and wait (a bookmark that needs the page). True when ready.
	bool ensureItemModsNow(const std::string& g)
	{
		ItemModLoad& L = imv_[GameIdx(g)];
		if (L.phase == ItemModLoad::Phase::Ready) return true;
		startItemMods(g);
		if (L.phase != ItemModLoad::Phase::Loading) return false;
		if (L.worker.joinable()) L.worker.join();
		finishItemMods(GameIdx(g));
		return L.phase == ItemModLoad::Phase::Ready;
	}

	// The worker is done: swap the entries into the page (same AlgoPage object,
	// so refs_ stays valid), size the ticks, restore this page's saved ticks.
	void finishItemMods(int gi)
	{
		ItemModLoad& L = imv_[gi];
		if (L.worker.joinable()) L.worker.join();
		const std::string g = kGames[gi];
		L.ms = L.resultMs;
		if (!L.result) {
			L.phase = ItemModLoad::Phase::Error;
			L.err = L.resultErr.empty() ? std::string(u8"未知錯誤") : L.resultErr;
			PobLog::Error("data", "regex_itemmods_" + g + ".json: " + L.err);
			return;
		}
		const int idx = itemPageIndex(g);
		RegexAlgo::AlgoPage* page = nullptr;
		for (RegexAlgo::AlgoPage& a : algo_)
			if (a.game == g && RegexItemMods::IsPageId(a.id)) page = &a;
		if (idx < 0 || !page) {
			L.phase = ItemModLoad::Phase::Error;
			L.err = u8"清單裡沒有這一頁";
			return;
		}
		*page = RegexItemMods::MakePage(g, L.result.get());
		L.result.reset();   // the page holds what it needs (templates + anchors)
		L.count = (int)page->entries.size();
		L.groupCounts = RegexItemMods::GroupCounts(*page);
		PageState& ps = pages_[idx];
		ps.picked.assign(page->entries.size(), 0);
		ps.algo.picked.assign(page->entries.size(), 0);   // values (restored at Init) stay
		ps.dirty = true;
		ps.filterDirty = true;
		combinedDirty_ = true;
		L.phase = ItemModLoad::Phase::Ready;
		PobLog::Diag("data", "regex item-mod values " + g + ": " + std::to_string(L.count) + " entries in " +
		                         std::to_string(L.ms) + " ms");
		if (const std::optional<RegexEmbed::Applied> r = RegexEmbed::SavedPicksOf(refs_[idx], state_)) {
			setTicks(idx, r->picked);
			if (r->missed > 0)
				notice_ = u8"上次的勾選有 " + std::to_string(r->missed) + u8" 項（" + refs_[idx].Title() +
				          u8"）在目前的資料裡找不到，可能是賽季更新後詞條有變動。";
		}
	}

	void joinItemMods()
	{
		for (ItemModLoad& L : imv_)
			if (L.worker.joinable()) L.worker.join();
	}

	void drawItemModPage(int idx)
	{
		const std::string g = refs_[idx].Game();
		ItemModLoad& L = imv_[GameIdx(g)];
		if (L.phase == ItemModLoad::Phase::Error) {
			ImGui::TextColored(kBad, u8"物品詞綴資料載入失敗：%s", L.err.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton(u8"重試###rx_imv_retry")) startItemMods(g);
			return;
		}
		if (L.phase != ItemModLoad::Phase::Ready) {
			if (L.phase == ItemModLoad::Phase::Idle) startItemMods(g);
			ImGui::TextDisabled(u8"載入物品詞綴中…（第一次打開要整理幾千條詞綴的模板與唯一片段）");
			return;
		}
		PageState& s = pages_[idx];
		const RegexAlgo::AlgoPage& page = *refs_[idx].algo;
		ImGui::SetNextItemWidth(190 * host_->scale);
		if (ImGui::InputTextWithHint("##rx_imv_search", u8"搜尋繁中 / 英文（空白分隔多個字）", &s.search))
			s.filterDirty = true;
		ImGui::SameLine();
		ImGui::SetNextItemWidth(150 * host_->scale);
		const std::string allLabel = u8"全部分類（" + std::to_string(page.entries.size()) + u8"）";
		const std::string curLabel = s.groupFilter < 0 || s.groupFilter >= (int)page.groups.size()
			? allLabel
			: page.groups[s.groupFilter] + u8"（" + std::to_string(L.groupCounts[s.groupFilter]) + u8"）";
		if (ImGui::BeginCombo("##rx_imv_group", curLabel.c_str())) {
			if (ImGui::Selectable(allLabel.c_str(), s.groupFilter < 0)) {
				s.groupFilter = -1;
				s.filterDirty = true;
			}
			for (int gi = 0; gi < (int)page.groups.size(); gi++) {
				const std::string label = page.groups[gi] + u8"（" + std::to_string(L.groupCounts[gi]) + u8"）###imvg" + std::to_string(gi);
				if (ImGui::Selectable(label.c_str(), s.groupFilter == gi)) {
					s.groupFilter = gi;
					s.filterDirty = true;
				}
			}
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (ImGui::Checkbox(u8"只看已勾選", &s.pickedOnly)) s.filterDirty = true;
		ImGui::SameLine();
		ImGui::BeginDisabled(s.algo.Count() == 0);
		if (ImGui::SmallButton(u8"清除###rx_imv_clear")) {
			std::fill(s.algo.picked.begin(), s.algo.picked.end(), (char)0);
			algoChanged(idx);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(u8"取消所有勾選");

		if (s.filterDirty) {
			RegexItemMods::Filter f;
			f.search = s.search;
			f.group = s.groupFilter;
			f.pickedOnly = s.pickedOnly;
			const RegexItemMods::Filtered r = RegexItemMods::FilterRows(page, s.algo.Picks(), f, RegexItemMods::kListCap);
			s.imvRows = r.rows;
			s.imvTotal = r.total;
			s.filterDirty = false;
		}
		ImGui::SameLine();
		ImGui::TextDisabled(u8"顯示 %d / 符合 %d", (int)s.imvRows.size(), s.imvTotal);

		ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
		ImGui::TextWrapped(u8"已勾選的永遠在最上面；每個勾選各自一個條件（同時成立），改數值會自動勾選。"
		                   u8"只收恰好一個整數數值的詞綴，負值在 ≥ 條件下會被當成正數。");
		if (s.imvTotal > (int)s.imvRows.size())
			ImGui::TextWrapped(u8"還有 %d 條符合：用搜尋或分類縮小範圍。", s.imvTotal - (int)s.imvRows.size());
		ImGui::PopStyleColor();
		if (s.imvRows.empty()) {
			ImGui::TextDisabled(u8"沒有符合的詞綴");
			return;
		}
		ImGui::BeginChild("##rx_imv_rows", ImVec2(0, 0), true);
		const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH;
		if (ImGui::BeginTable("##rx_imv", 3, flags)) {
			ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 1.3f);
			ImGui::TableSetupColumn("input", ImGuiTableColumnFlags_WidthStretch, 1.4f);
			ImGui::TableSetupColumn("frag", ImGuiTableColumnFlags_WidthStretch, 1.3f);
			// A copy: ticking a row re-filters next frame, never under this loop.
			const std::vector<int> rows = s.imvRows;
			for (int i : rows) {
				ImGui::TableNextRow();
				drawAlgoRow(idx, i);
			}
			ImGui::EndTable();
		}
		ImGui::EndChild();
	}

	// The numeric section on top of a host page (exile-appraiser
	// RegexNumericSection.vue): a foldable block whose terms join the modifier
	// tokens below in one string. Folded, its header still says what is set.
	// Step 40 (B d5ccb47): the same block draws the
	// one-row "稀有度 / 汙染" condition sections (section_title_cond / _hint_cond).
	void drawSection(int host, int sec)
	{
		using namespace RegexAlgo;
		PageState& ss = pages_[sec];
		const AlgoPage& page = *refs_[sec].algo;
		const std::string hostId = refs_[host].Id();
		const bool cond = IsConditionSectionId(page.id);
		const char* title = cond ? u8"稀有度 / 汙染" : u8"數值條件";
		const bool collapsed = std::find(state_.collapsed.begin(), state_.collapsed.end(), hostId) != state_.collapsed.end();
		ImGui::PushID("rx_sec");
		bool toggle = ImGui::ArrowButton("##toggle", collapsed ? ImGuiDir_Right : ImGuiDir_Down);
		if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"%s%s", collapsed ? u8"展開" : u8"收合", title);
		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(title);
		if (ImGui::IsItemClicked()) toggle = true;
		ImGui::SameLine();
		ImGui::TextDisabled(u8"已設 %d / %d", ss.algo.Count(), (int)page.entries.size());
		int contrib = 0;
		for (const PageContribution& c : pages_[host].combined.perPage)
			if (c.id == page.id) contrib = c.length;
		if (contrib > 0) {
			ImGui::SameLine();
			ImGui::TextDisabled(u8"%d 字", contrib);
		}
		ImGui::SameLine();
		ImGui::TextDisabled("(?)");
		if (ImGui::IsItemHovered()) {
			ImGui::BeginTooltip();
			ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
			// i18n cmn-Hant.json ppz.regex.section_hint / section_hint_cond (step 40)
			if (cond)
				ImGui::TextUnformatted(u8"物品稀有度可多選、汙染二選一（再點一次取消），各自一個條件（同時成立），"
				                       u8"與下方勾選合成同一條字串。點按鈕會自動勾選。");
			else
				ImGui::TextUnformatted(u8"階級、物品數量、稀有度等屬性行的數值；每個勾選各自一個條件（同時成立），"
				                       u8"與下方詞綴合成同一條字串。改數值會自動勾選。寫法依社群實用格式「標籤: +N%」"
				                       u8"（半形／全形冒號、+ 可有可無，只比對冒號後的整個數字，不跨行）；"
				                       u8"階級比對名稱「（階級 N）」；稀有度 / 汙染列：稀有度可多選（比對「稀有度: 稀有」行），"
				                       u8"汙染二選一（再點一次取消；未汙染 = 排除「已汙染」行），兩者各自一個條件。");
			ImGui::PopTextWrapPos();
			ImGui::EndTooltip();
		}
		ImGui::SameLine();
		ImGui::BeginDisabled(ss.algo.Count() == 0);
		if (ImGui::SmallButton(u8"清除###rx_sec_clear")) {
			std::fill(ss.algo.picked.begin(), ss.algo.picked.end(), (char)0);
			algoChanged(sec);
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(cond ? u8"取消稀有度 / 汙染條件的勾選" : u8"取消所有數值條件的勾選");
		if (toggle) {
			// store.ts setCollapsed: remembered per host page
			if (collapsed) state_.collapsed.erase(std::remove(state_.collapsed.begin(), state_.collapsed.end(), hostId), state_.collapsed.end());
			else state_.collapsed.push_back(hostId);
			markStateDirty();
		}

		if (collapsed) {
			// view.ts sectionSummary: ticked rows in row order, "地圖階級 ≥16 · 物品數量 ≥80%".
			const std::vector<SummaryItem> items = SectionSummary(page, ss.algo.Picks(), ss.algo.values, RegexFrag::Lang::Zh);
			if (items.empty()) {
				ImGui::TextDisabled(cond ? u8"沒有設定稀有度 / 汙染條件" : u8"沒有設定數值條件");
			} else {
				std::string ok, bad;
				for (const SummaryItem& it : items) {
					if (it.cond) ok += (ok.empty() ? "" : u8" · ") + it.label + " " + *it.cond;
					else bad += (bad.empty() ? "" : u8"、") + it.label;
				}
				if (!ok.empty()) {
					ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
					ImGui::TextWrapped("%s", ok.c_str());
					ImGui::PopStyleColor();
				}
				if (!bad.empty()) ImGui::TextColored(kBad, u8"輸入不成立：%s", bad.c_str());
			}
		} else {
			drawAlgoRows(sec);
		}
		ImGui::PopID();
		ImGui::Separator();
	}

	// ---- multi-page merge (R4) ---------------------------------------------------
	//
	// exile-appraiser store.ts `combined` / RegexCombined.vue. Everything below
	// is per game: the merge only ever takes the selected game's pages.

	// The page's ticks as indices (corpus or algorithmic).
	std::vector<int> picksOf(int idx) const
	{
		if (refs_[idx].algo) return pages_[idx].algo.Picks();
		std::vector<int> out;
		for (int i = 0; i < (int)pages_[idx].picked.size(); i++)
			if (pages_[idx].picked[i]) out.push_back(i);
		return out;
	}

	// pages/index.ts:55 combineOrder over the selected game, as indices into
	// refs_: listed pages in order, each host followed by its section.
	// embed.ts:35 combineSels keeps only those with ticks (`pickedOnly`).
	std::vector<int> combineOrderIdx(bool pickedOnly) const
	{
		std::vector<int> out;
		for (int i = 0; i < (int)refs_.size(); i++) {
			if (refs_[i].Game() != selGame_ || refs_[i].IsSection()) continue;
			out.push_back(i);
			const int sec = sectionIndexOf(i);
			if (sec >= 0) out.push_back(sec);
		}
		if (pickedOnly)
			out.erase(std::remove_if(out.begin(), out.end(), [&](int i) { return picksOf(i).empty(); }), out.end());
		return out;
	}

	// store.ts pagePickCount: a page's ticks plus its section's.
	int pagePickCount(int idx) const
	{
		int n = (int)picksOf(idx).size();
		const int sec = sectionIndexOf(idx);
		if (sec >= 0) n += pages_[sec].algo.Count();
		return n;
	}

	// The page's corpus in the output language, built on first use (the merge
	// needs every ticked page's, not just the one on screen).
	void ensureCorpus(int idx)
	{
		PageState& ps = pages_[idx];
		if (!refs_[idx].corpus || ps.corpusReady) return;
		RegexAlgo::BuildPageCorpus(*refs_[idx].corpus, fragLang(), ps.corpus);
		ps.corpusReady = true;
		ps.dirty = true;
	}

	// store.ts `combined`: every page of the game with ticks + custom + excludes.
	const RegexAlgo::CombineResult& combinedAll()
	{
		if (!combinedDirty_) return combined_;
		std::vector<RegexAlgo::CombineSel> sels;
		for (int i : combineOrderIdx(true)) {
			RegexAlgo::CombineSel sel;
			sel.page = refs_[i];
			sel.picks = picksOf(i);
			if (refs_[i].algo) {
				sel.values = &pages_[i].algo.values;
			} else {
				ensureCorpus(i);
				sel.corpus = &pages_[i].corpus;
			}
			sels.push_back(std::move(sel));
		}
		combined_ = RegexAlgo::Combine(fragLang(), mode_, sels, state_.custom, state_.excludes, &unions_);
		combinedDirty_ = false;
		return combined_;
	}

	// store.ts setPanelView; remembered (panelView, a PobTools-only field).
	void setView(View v)
	{
		if (v == view_) return;
		view_ = v;
		state_.panelView = v == View::Combined ? "combined" : "page";
		markStateDirty();
	}

	void setScope(bool combined)
	{
		if (combined == scopeCombined_) return;
		scopeCombined_ = combined;
		state_.outScope = combined ? "combined" : "page";
		markStateDirty();
		copied_ = false;
	}

	// A page title within the selected game (gem_names / vendor_bases exist in both).
	std::string pageTitleInGame(const std::string& id) const
	{
		for (const RegexAlgo::PageRef& p : refs_)
			if (p.Id() == id && p.Game() == selGame_) return p.Title();
		return pageTitleById(id);
	}

	// pages/index.ts:45 hostIdOf, as an index: a section -> its host page.
	int hostIndexOf(int idx) const
	{
		if (!refs_[idx].IsSection()) return idx;
		for (int i = 0; i < (int)refs_.size(); i++)
			if (!refs_[i].IsSection() && refs_[i].Id() == refs_[idx].algo->sectionOf && refs_[i].Game() == refs_[idx].Game())
				return i;
		return idx;
	}

	// store.ts clearAllPicks: every page of the game, values kept.
	void clearAllPicks()
	{
		for (int i : combineOrderIdx(true)) {
			PageState& ps = pages_[i];
			if (refs_[i].algo) std::fill(ps.algo.picked.begin(), ps.algo.picked.end(), (char)0);
			else std::fill(ps.picked.begin(), ps.picked.end(), (char)0);
			syncCurrent(i);
			ps.dirty = true;
			ps.filterDirty = true;
		}
		// A host's single-page output carries its section.
		for (PageState& ps : pages_) ps.dirty = true;
		combinedDirty_ = true;
		copied_ = false;
		notice_ = u8"已清除這個遊戲所有清單的勾選（數值條件的數值保留）。";
	}

	// store.ts addCustom / removeCustom: trimmed, no duplicates.
	bool addChip(std::vector<std::string>& list, std::string& draft)
	{
		const std::string t = RegexAlgo::JsTrim(draft);
		if (t.empty() || std::find(list.begin(), list.end(), t) != list.end()) return false;
		list.push_back(t);
		draft.clear();
		markStateDirty();
		combinedDirty_ = true;
		copied_ = false;
		return true;
	}

	void drawChips(const char* id, const char* title, const char* hint, const char* placeholder,
	               std::vector<std::string>& list, std::string& draft)
	{
		ImGui::PushID(id);
		ImGui::TextUnformatted(title);
		ImGui::SameLine();
		ImGui::TextDisabled("%s", hint);
		int remove = -1;
		const float right = ImGui::GetContentRegionMax().x;
		for (int i = 0; i < (int)list.size(); i++) {
			ImGui::PushID(i);
			// A chip: the text and its own remove button, wrapped like words.
			const float w = ImGui::CalcTextSize(list[i].c_str()).x + ImGui::GetFrameHeight() +
			                ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
			if (i > 0) {
				ImGui::SameLine();
				if (ImGui::GetCursorPosX() + w > right) ImGui::NewLine();
			}
			ImGui::BeginGroup();
			// Plain text, not a button label: typed text may contain "##".
			ImGui::TextUnformatted(list[i].c_str());
			ImGui::SameLine(0, 1 * host_->scale);
			if (ImGui::SmallButton(u8"×")) remove = i;
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"移除");
			ImGui::EndGroup();
			ImGui::PopID();
		}
		if (remove >= 0) {
			list.erase(list.begin() + remove);
			markStateDirty();
			combinedDirty_ = true;
			copied_ = false;
		}
		ImGui::SetNextItemWidth(180 * host_->scale);
		const bool entered = ImGui::InputTextWithHint("##draft", placeholder, &draft, ImGuiInputTextFlags_EnterReturnsTrue);
		ImGui::SameLine();
		ImGui::BeginDisabled(RegexAlgo::JsTrim(draft).empty());
		const bool clicked = ImGui::SmallButton(u8"加入");
		ImGui::EndDisabled();
		if ((entered || clicked) && addChip(list, draft) && entered) ImGui::SetKeyboardFocusHere(-1);
		ImGui::PopID();
	}

	// combine.ts:46 conflict kinds, worded as RegexCombined.vue's i18n.
	std::string conflictText(const RegexAlgo::Conflict& c) const
	{
		using RegexAlgo::ConflictKind;
		const std::string page = c.page.empty() ? std::string() : pageTitleInGame(c.page);
		switch (c.kind) {
		case ConflictKind::Extra: return page + u8"：也會選到未勾選的「" + c.text + u8"」";
		case ConflictKind::Missing: return page + u8"：「" + c.text + u8"」沒被選到";
		case ConflictKind::Ambient: return u8"片段「" + c.text + u8"」會中每件物品都有的文字";
		case ConflictKind::Fragment: return page + u8"：條件片段會誤中詞綴行（" + c.text + u8"）";
		case ConflictKind::Exclude: return page + u8"：排除詞與已勾選的詞綴衝突（" + c.text + u8"）";
		case ConflictKind::Invalid: return page + u8"：「" + c.text + u8"」的輸入不成立，已略過";
		}
		return c.text;
	}

	// RegexCombined.vue: which pages take part and what each costs, the custom /
	// exclude chips, and the merge conflicts.
	void drawCombinedView()
	{
		using namespace RegexAlgo;
		const CombineResult& r = combinedAll();
		const std::vector<int> picked = combineOrderIdx(true);

		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted((std::string(u8"已選（合併）· ") + GameLabel(selGame_)).c_str());
		ImGui::SameLine();
		const char* clearLabel = u8"全部清除";
		ImGui::SameLine(std::max(ImGui::GetCursorPosX(),
		                         ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(clearLabel).x -
		                         ImGui::GetStyle().FramePadding.x * 2));
		ImGui::BeginDisabled(picked.empty());
		if (ImGui::SmallButton(clearLabel)) clearAllPicks();
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip(u8"取消這個遊戲所有清單（含數值條件）的勾選；自訂文字與排除詞保留");

		ImGui::BeginChild("##rx_comb", ImVec2(0, 0), true);
		if (picked.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextWrapped(u8"還沒有勾選任何清單。切到「單頁清單」勾選，勾好的清單會在這裡合成一串。");
			ImGui::PopStyleColor();
		} else {
			const ImGuiTableFlags flags = ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH |
			                              ImGuiTableFlags_RowBg;
			if (ImGui::BeginTable("##rx_comb_pages", 4, flags)) {
				ImGui::TableSetupColumn(u8"清單", ImGuiTableColumnFlags_WidthStretch, 2.4f);
				ImGui::TableSetupColumn(u8"勾選", ImGuiTableColumnFlags_WidthStretch, 0.7f);
				ImGui::TableSetupColumn(u8"貢獻長度", ImGuiTableColumnFlags_WidthStretch, 1.0f);
				ImGui::TableSetupColumn(u8"無法單獨指定", ImGuiTableColumnFlags_WidthStretch, 1.2f);
				ImGui::TableHeadersRow();
				int jump = -1;
				for (int i : picked) {
					const PageContribution* c = nullptr;
					for (const PageContribution& x : r.perPage)
						if (x.id == refs_[i].Id()) c = &x;
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::PushID(i);
					// Clicking a row goes to that page (a section: its host page).
					if (ImGui::Selectable(refs_[i].Title().c_str(), false)) jump = hostIndexOf(i);
					if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"到這份清單");
					if (refs_[i].algo) {
						ImGui::SameLine();
						ImGui::TextDisabled(u8"數值 / 條件");
					}
					ImGui::PopID();
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%d", (int)picksOf(i).size());
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%d", c ? c->length : 0);
					ImGui::TableSetColumnIndex(3);
					const int un = c ? c->unresolved : 0;
					if (un > 0) ImGui::TextColored(kWarn, "%d", un);
					else ImGui::Text("0");
				}
				if (!r.custom.empty()) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(u8"自訂文字");
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%d", (int)r.custom.size());
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%d", r.customLength);
					ImGui::TableSetColumnIndex(3);
					ImGui::TextDisabled(u8"—");
				}
				if (!r.excludes.empty()) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(u8"排除詞");
					ImGui::TableSetColumnIndex(1);
					ImGui::Text("%d", (int)r.excludes.size());
					ImGui::TableSetColumnIndex(2);
					ImGui::Text("%d", r.excludesLength);
					ImGui::TableSetColumnIndex(3);
					ImGui::TextDisabled(u8"—");
				}
				ImGui::EndTable();
				if (jump >= 0) {
					switchPage(jump);
					setView(View::Page);
				}
			}
		}

		ImGui::Spacing();
		drawChips("rx_custom", u8"自訂文字", u8"每項各自一個條件（同時成立），原樣比對", u8"輸入文字後按 Enter",
		          state_.custom, customDraft_);
		if (!state_.custom.empty()) ImGui::TextDisabled(u8"自訂文字不經驗證，可能誤中其他物品。");
		ImGui::Spacing();
		drawChips("rx_excludes", u8"排除詞", u8"併進唯一的排除條件（!）：有其中任一個就不選", u8"例如：反射",
		          state_.excludes, excludeDraft_);

		if (!r.conflicts.empty()) {
			ImGui::Spacing();
			const std::string head = std::to_string(r.conflicts.size()) + u8" 個合併衝突###rx_conflicts";
			ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
			const bool open = ImGui::CollapsingHeader(head.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
			ImGui::PopStyleColor();
			if (open) {
				// Merging unrelated pages can report thousands of `extra` lines;
				// the first few hundred say everything the player can act on.
				const size_t shown = std::min<size_t>(r.conflicts.size(), 300);
				ImGui::PushTextWrapPos(0.0f);
				for (size_t k = 0; k < shown; k++) ImGui::BulletText("%s", conflictText(r.conflicts[k]).c_str());
				ImGui::PopTextWrapPos();
				if (r.conflicts.size() > shown)
					ImGui::TextDisabled(u8"（另有 %d 個未列出）", (int)(r.conflicts.size() - shown));
			}
		}
		ImGui::EndChild();
	}

	const ToolPanelHost* host_ = nullptr;
	std::wstring exeDir_, game_;
	RegexDataset data_;
	// Algorithmic pages of both games, built once from the data's labels, and
	// every page (corpus first, same index as data_.Pages()) as one list.
	std::vector<RegexAlgo::AlgoPage> algo_;
	std::vector<RegexAlgo::PageRef> refs_;
	// R4 merge. Custom text / excludes live in state_ (saved); the view and
	// scope are mirrored there (panelView / outScope).
	View view_ = View::Page;           // store.ts panelView, default the page list
	bool scopeCombined_ = true;        // state.ts outScope, default 'combined'
	std::string customDraft_, excludeDraft_;
	RegexAlgo::CombineResult combined_;
	bool combinedDirty_ = true;
	RegexAlgo::UnionCorpusCache unions_;
	bool dataOk_ = false;
	std::string dataErr_;
	std::string selGame_ = "poe1";   // which game's lists are showing

	RegexUiState state_;
	bool stateDirty_ = false;
	// Set when a save failed; cleared by the next real change. Without it the
	// deferred pass retries a doomed write on every single frame.
	bool saveFailed_ = false;

	std::vector<PageState> pages_;
	int page_ = 0;
	RegexGen::Mode mode_ = RegexGen::Mode::Any;
	Lang lang_ = Lang::Zh;
	bool bilingual_ = true;

	Modal modal_ = Modal::None;
	bool renameMode_ = false;
	int editIdx_ = -1;
	// R6 bookmark list: which game's tab, the game it last followed, the
	// queued drag / button edit, and the folder modals' target and error.
	std::string bmTab_ = "poe1", bmTabFollow_ = "poe1";
	BmAction bmAction_;
	bool folderRenameMode_ = false;
	std::string folderEdit_, folderErr_;
	std::string nameBuf_;
	std::string notice_;

	std::string copyRequest_;
	bool copied_ = false;
	// R8: share code on its way to the clipboard, and how that went (1 ok, 2 failed).
	std::string shareCopyRequest_;
	int shareCopied_ = 0;
	std::chrono::steady_clock::time_point shareCopiedAt_;
	std::vector<RegexShare::Template> templates_;
	std::string templatesErr_;
	int tplPending_ = -1;                 // the template the confirm dialog is about
	std::string pasteBuf_, pasteErr_;     // the paste dialog
	ItemModLoad imv_[2];   // R7, per game (kGames order)
	int t17Cache_ = -1;
	bool t17Present_ = false;
	ToolCloseState close_ = ToolCloseState::Open;
};

} // namespace

IToolPanel* CreateRegexToolPanel()
{
	return new RegexToolPanel();
}

void ShowRegexTool(const std::wstring& exeDir, const std::wstring& game,
                   const std::wstring& locale)
{
	RegexToolPanel panel;
	ToolWindowDesc desc;
	// "PobTools — Poe Regex"
	desc.titleUtf8 = "PobTools \xe2\x80\x94 Poe Regex";
	desc.defW = 1200;
	desc.defH = 800;
	RunToolWindow(panel, desc, exeDir, game, locale);
}
