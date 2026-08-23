#include "regex_tool.h"

#include "clipboard_util.h"
#include "regex_data.h"
#include "regex_gen.h"
#include "regex_state.h"
#include "tool_panel.h"
#include "tool_window.h"
#include "ui_theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <string>
#include <vector>

// 搜尋字串產生器 — tick the modifiers, get the shortest string that finds exactly
// those and nothing else, paste it into the game's search box.
//
// The panel owns no knowledge of what a map modifier is. It shows the pages the
// data file happens to contain, and everything about picking tokens lives in
// regex_gen. The one thing it does add is the last page, 自訂清單, where the
// corpus is whatever the player pasted -- the same machinery pointed at a list
// the tool has never seen.
//
// A row shows the line in the language the query is being built from, and -- when
// the bilingual switch is on -- the other language underneath it. Which language
// the QUERY uses is the player's choice, because the two are not interchangeable:
// a token cut from the Chinese only avoids false positives among the Chinese
// lines, and pasting it into an English client would match nothing at all.

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
};

// The synthetic last page. Its corpus is the text the player pasted, one entry
// per line, so the tool works on lists nobody has extracted yet -- divination
// cards, a guild's buy list, whatever is in front of them.
constexpr const char* kCustomPageTitle = u8"自訂清單";
constexpr const char* kCustomPageNote =
	u8"一行一個候選字串。勾選要找的那幾行，產生的字串保證只中勾選的行、不中其他行。"
	u8"數字請寫成 #，代表遊戲會填入一個數值。";
constexpr const char* kCustomPageId = "__custom";

// Which modal wants to open. Raised by a button deep inside a child window and
// acted on at the top level, because OpenPopup and BeginPopupModal have to be
// called from the same ID scope or the popup simply never appears.
enum class Modal { None, Save, Rename, Delete };

class RegexToolPanel : public IToolPanel {
public:
	bool Init(const ToolPanelHost& h) override
	{
		host_ = &h;
		exeDir_ = h.exeDir;
		game_ = h.game.empty() ? std::wstring(L"poe1") : h.game;
		dataOk_ = data_.Load(exeDir_, game_, &dataErr_);
		ownGame_ = data_.HasGame(NarrowAscii(game_));
		pages_.resize(data_.Pages().size() + 1);   // + the custom page
		for (size_t i = 0; i < data_.Pages().size(); i++)
			pages_[i].picked.assign(data_.Pages()[i].entries.size(), 0);

		state_.Load(exeDir_);   // a fresh install has no file; the defaults are fine
		restoreState();
		lang_ = state_.lang == "en" ? Lang::En : Lang::Zh;
		bilingual_ = state_.bilingual;
		return true;   // a missing data file is a message, not a dead tab
	}

	const char* InitError() const override { return ""; }

	void Frame() override
	{
		if (!dataOk_) {
			ImGui::TextColored(kBad, u8"搜尋字串資料載入失敗：%s", dataErr_.c_str());
			ImGui::TextDisabled(u8"「自訂清單」不需要資料檔，仍可使用。");
			ImGui::Dummy(ImVec2(0, 8));
		}
		// No banner when the launcher's game has no list of its own. The page
		// labels still carry "（PoE1）" in that case, which is the part that
		// actually prevents a PoE2 player from mistaking the list for theirs.

		drawHeader();
		ImGui::Separator();

		const float avail = ImGui::GetContentRegionAvail().x;
		const float leftW = std::max(340.0f, avail * 0.54f);
		ImGui::BeginChild("##rx_left", ImVec2(leftW, 0), false);
		if (onCustom()) drawCustomSource();
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
	}
	PobUi::Density Density() const override { return PobUi::Density::Compact; }
	const char* PanelId() const override { return "regex"; }

private:
	int customIdx() const { return (int)data_.Pages().size(); }
	bool onCustom() const { return page_ == customIdx(); }
	PageState& st() { return pages_[page_]; }
	const PageState& st() const { return pages_[page_]; }

	// The entries of whatever page is showing. The custom page has no data-file
	// entries, so it keeps its own vector built from the pasted text.
	const std::vector<RegexEntryDef>& entries() const
	{
		return onCustom() ? customEntries_ : data_.Pages()[page_].entries;
	}
	const std::vector<std::string>& groups() const
	{
		static const std::vector<std::string> none;
		return onCustom() ? none : data_.Pages()[page_].groups;
	}
	int limit() const { return onCustom() ? 250 : data_.Pages()[page_].limit; }

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
	}
	std::string pageId() const
	{
		return onCustom() ? std::string(kCustomPageId) : data_.Pages()[page_].id;
	}

	// The game tag is appended only when it is NOT the launcher's game: on a
	// matching install every page would carry the same suffix, which is noise.
	std::string pageLabel(int i) const
	{
		const RegexPageDef& p = data_.Pages()[i];
		if (ownGame_ || p.game.empty()) return p.title;
		return p.title + (p.game == "poe2" ? u8"（PoE2）" : u8"（PoE1）");
	}
	std::string pageTitleById(const std::string& id) const
	{
		if (id == kCustomPageId) return kCustomPageTitle;
		for (const RegexPageDef& p : data_.Pages())
			if (p.id == id) return p.title;
		return id;
	}

	// ---- remembered state ----------------------------------------------------

	void flushState()
	{
		if (!stateDirty_) return;
		state_.Save(exeDir_);
		stateDirty_ = false;
	}

	void restoreState()
	{
		for (size_t i = 0; i < data_.Pages().size(); i++)
			if (data_.Pages()[i].id == state_.page) page_ = (int)i;
		if (state_.page == kCustomPageId) page_ = customIdx();
		mode_ = ModeFromId(state_.mode);

		int missedTotal = 0;
		std::string firstPage;
		for (size_t i = 0; i < data_.Pages().size(); i++) {
			const RegexPageDef& def = data_.Pages()[i];
			for (const RegexPagePicks& saved : state_.current) {
				if (saved.page != def.id) continue;
				const int missed = applyKeys(def.entries, pages_[i].picked,
				                             saved.keys, saved.alt);
				if (missed > 0) {
					missedTotal += missed;
					if (firstPage.empty()) firstPage = def.title;
				}
			}
		}
		if (missedTotal > 0)
			notice_ = u8"上次的勾選有 " + std::to_string(missedTotal) + u8" 項（" + firstPage +
			          u8" 等）在目前的資料裡找不到，可能是賽季更新後詞條有變動。";
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

	// Keys -> ticks, through the shared resolver in regex_state so --regex-selftest
	// exercises the same code the panel does.
	static int applyKeys(const std::vector<RegexEntryDef>& defs, std::vector<char>& picked,
	                     const std::vector<std::string>& keys,
	                     const std::vector<std::string>& alt)
	{
		std::vector<std::string> entryKeys, entryAlt;
		entryKeys.reserve(defs.size());
		entryAlt.reserve(defs.size());
		for (const RegexEntryDef& d : defs) {
			entryKeys.push_back(KeyOf(d));
			entryAlt.push_back(ZhLine(d));
		}
		return RegexResolveKeys(keys, alt, entryKeys, entryAlt, picked);
	}

	void collectKeys(std::vector<std::string>& keys, std::vector<std::string>& alt) const
	{
		keys.clear();
		alt.clear();
		const PageState& s = st();
		for (int i = 0; i < (int)entries().size(); i++) {
			if (i >= (int)s.picked.size() || !s.picked[i]) continue;
			keys.push_back(KeyOf(entries()[i]));
			alt.push_back(ZhLine(entries()[i]));
		}
	}

	// Everything that changes what is ticked funnels through here, so there is
	// exactly one place that could forget to persist.
	void picksChanged()
	{
		st().dirty = true;
		st().filterDirty = true;   // ticked rows move to the top; see refreshFilter
		copied_ = false;
		if (onCustom()) return;   // a pasted list is not worth carrying to next run
		RegexPagePicks& p = state_.PicksFor(pageId());
		collectKeys(p.keys, p.alt);
		state_.page = pageId();
		state_.mode = modeId();
		stateDirty_ = true;
	}

	// ---- header --------------------------------------------------------------

	void drawHeader()
	{
		ImGui::SetNextItemWidth(240 * host_->scale);
		const std::string cur = onCustom() ? kCustomPageTitle : pageLabel(page_);
		if (ImGui::BeginCombo(u8"清單", cur.c_str())) {
			for (size_t i = 0; i < data_.Pages().size(); i++) {
				if (ImGui::Selectable(pageLabel((int)i).c_str(), page_ == (int)i))
					switchPage((int)i);
			}
			if (ImGui::Selectable(kCustomPageTitle, onCustom())) switchPage(customIdx());
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
			state_.mode = modeId();
			stateDirty_ = true;
		}

		ImGui::SameLine(0, 24 * host_->scale);
		ImGui::TextDisabled(u8"已勾選 %d / %d", pickCount(), (int)entries().size());

		// Right-hand end of the header row. It belongs to the whole panel rather
		// than to the list toolbar: it changes how both columns read, and the
		// toolbar is the row that runs out of width first.
		{
			const char* label = u8"雙語顯示";
			const float w = ImGui::GetFrameHeight() +
			                ImGui::GetStyle().ItemInnerSpacing.x +
			                ImGui::CalcTextSize(label).x;
			ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
			if (ImGui::Checkbox(label, &bilingual_)) {
				state_.bilingual = bilingual_;
				stateDirty_ = true;
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"在每一列下面加上另一種語言的原文");
		}

		const std::string& note = onCustom() ? customNote_ : data_.Pages()[page_].note;
		if (!note.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextWrapped("%s", note.c_str());
			ImGui::PopStyleColor();
		}
	}

	int pickCount() const
	{
		int n = 0;
		for (char c : st().picked) n += c ? 1 : 0;
		return n;
	}

	void drawCustomSource()
	{
		ImGui::TextDisabled(u8"候選清單（一行一個）");
		const ImVec2 boxSize(-1, ImGui::GetContentRegionAvail().y * 0.38f);
		if (ImGui::InputTextMultiline("##rx_custom", &customText_, boxSize))
			customTextDirty_ = true;
		if (customTextDirty_) rebuildCustom();
		ImGui::Separator();
		drawList();
	}

	// ---- the list ------------------------------------------------------------

	void drawList()
	{
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
		ImGui::EndTooltip();
	}

	// ---- output --------------------------------------------------------------

	void drawOutput()
	{
		PageState& s = st();
		if (!s.corpusReady) buildCorpus();
		if (s.dirty) recompute();

		ImGui::TextDisabled(u8"貼進遊戲搜尋列");
		std::string q = s.result.query;
		ImGui::InputTextMultiline("##rx_out", &q, ImVec2(-1, 70 * host_->scale),
		                          ImGuiInputTextFlags_ReadOnly);

		const int len = s.result.length;
		const int lim = limit();
		ImGui::PushStyleColor(ImGuiCol_Text, len > lim ? kBad : (len > lim * 4 / 5 ? kWarn : kGood));
		ImGui::Text(u8"長度 %d / %d 字", len, lim);
		ImGui::PopStyleColor();
		if (len > lim) {
			ImGui::SameLine();
			ImGui::TextColored(kBad, u8"超過上限，請減少勾選");
		}
		// Whatever the panel last did, said next to the thing it changed. It used
		// to sit at the very top, three sections away from the string it was
		// talking about.
		if (!notice_.empty()) {
			ImGui::TextColored(kWarn, "%s", notice_.c_str());
			ImGui::SameLine();
			if (ImGui::SmallButton(u8"知道了###rx_notice")) notice_.clear();
		}

		ImGui::BeginDisabled(s.result.query.empty());
		if (ImGui::Button(u8"複製", ImVec2(90 * host_->scale, 0))) {
			copyRequest_ = s.result.query;
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
			                   u8"遊戲的搜尋沒有辦法只中其中一個。");
			ImGui::PopStyleColor();
		}

		if (!s.result.tokens.empty() &&
		    ImGui::CollapsingHeader((u8"用到的片段（" +
		                             std::to_string(s.result.tokens.size()) +
		                             u8" 段）###rx_tok").c_str())) {
			ImGui::TextDisabled(u8"括號只是為了看清楚頭尾的空白，不要打進去");
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			// Bracketed, because a space at either end of a token is significant
			// and otherwise invisible: " 傷" and "傷" are different searches, and
			// the first is the one that does not also match 怪物傷害.
			for (const std::string& t : s.result.tokens)
				ImGui::BulletText(u8"「%s」", t.c_str());
			ImGui::PopStyleColor();
		}
	}

	// ---- bookmarks -----------------------------------------------------------

	void drawBookmarks()
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled(u8"書籤");
		ImGui::SameLine();
		const int picks = pickCount();
		ImGui::BeginDisabled(picks == 0);
		if (ImGui::SmallButton(u8"存成書籤")) {
			nameBuf_ = pageTitleById(pageId()) + " " + std::to_string(picks) + u8" 項";
			editIdx_ = -1;
			modal_ = Modal::Save;
		}
		ImGui::EndDisabled();
		if (picks == 0 && ImGui::IsItemHovered())
			ImGui::SetTooltip(u8"先勾選幾項才有東西可以存");

		if (state_.bookmarks.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			ImGui::TextWrapped(u8"還沒有書籤。勾好一組常用的詞綴後按「存成書籤」，"
			                   u8"下次可以直接叫回來。");
			ImGui::PopStyleColor();
			return;
		}

		ImGui::BeginChild("##rx_bm", ImVec2(0, 0), true);
		for (int i = 0; i < (int)state_.bookmarks.size(); i++) {
			const RegexBookmark& b = state_.bookmarks[i];
			ImGui::PushID(i);
			ImGui::TextUnformatted(b.name.c_str());
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::MutedText());
			const char* modeZh = b.mode == "all" ? u8"全部都有"
			                   : b.mode == "none" ? u8"一個都沒有" : u8"含任一個";
			ImGui::Text(u8"%s · %s · %s · %d 項", pageTitleById(b.page).c_str(), modeZh,
			            b.lang == "en" ? "English" : u8"繁中", (int)b.keys.size());
			ImGui::PopStyleColor();
			if (ImGui::SmallButton(u8"載入")) loadBookmark(i);
			ImGui::SameLine();
			if (ImGui::SmallButton(u8"更新")) updateBookmark(i);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"用目前的清單、模式與勾選覆寫這個書籤");
			ImGui::SameLine();
			if (ImGui::SmallButton(u8"改名")) {
				nameBuf_ = b.name;
				editIdx_ = i;
				modal_ = Modal::Rename;
			}
			ImGui::SameLine();
			PobUi::PushDangerButton();
			if (ImGui::SmallButton(u8"刪除")) {
				editIdx_ = i;
				modal_ = Modal::Delete;
			}
			PobUi::PopButtonStyle();
			ImGui::Separator();
			ImGui::PopID();
		}
		ImGui::EndChild();
	}

	void loadBookmark(int i)
	{
		if (i < 0 || i >= (int)state_.bookmarks.size()) return;
		// By value: switchPage and the ticks below both run while `state_` is
		// being read, and a reference into a vector that anything appends to is
		// a dangling pointer waiting for a slow day.
		const RegexBookmark b = state_.bookmarks[i];
		int target = -1;
		for (size_t p = 0; p < data_.Pages().size(); p++)
			if (data_.Pages()[p].id == b.page) target = (int)p;
		if (target < 0 && b.page == kCustomPageId) target = customIdx();
		if (target < 0) {
			notice_ = u8"書籤「" + b.name + u8"」的清單「" + pageTitleById(b.page) +
			          u8"」在這個版本不存在，沒有載入。";
			return;
		}
		switchPage(target);
		mode_ = ModeFromId(b.mode);
		state_.mode = modeId();
		setLang(b.lang == "en" ? Lang::En : Lang::Zh);
		const int missed = applyKeys(entries(), st().picked, b.keys, b.alt);
		notice_ = missed > 0
			? u8"已載入書籤「" + b.name + u8"」，但其中 " + std::to_string(missed) +
			  u8" 項在目前的資料裡找不到（賽季更新後詞條可能有變動）。"
			: u8"已載入書籤「" + b.name + u8"」。";
		st().filterDirty = true;
		picksChanged();
	}

	void updateBookmark(int i)
	{
		if (i < 0 || i >= (int)state_.bookmarks.size()) return;
		std::vector<std::string> keys, alt;
		collectKeys(keys, alt);
		if (keys.empty()) {
			notice_ = u8"目前一項都沒有勾選，沒有更新書籤（要清空請改用刪除）。";
			return;
		}
		RegexBookmark& b = state_.bookmarks[i];
		b.page = pageId();
		b.mode = modeId();
		b.lang = (lang_ == Lang::En) ? "en" : "zh";
		b.keys = std::move(keys);
		b.alt = std::move(alt);
		notice_ = u8"書籤「" + b.name + u8"」已更新為目前的勾選。";
		stateDirty_ = true;
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
		}

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
					stateDirty_ = true;
				}
				editIdx_ = -1;
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
				stateDirty_ = true;
			}
		} else {
			RegexBookmark b;
			b.name = nameBuf_;
			b.page = pageId();
			b.mode = modeId();
			b.lang = (lang_ == Lang::En) ? "en" : "zh";
			collectKeys(b.keys, b.alt);
			if (!b.keys.empty()) {
				state_.bookmarks.push_back(std::move(b));
				notice_ = u8"已存成書籤「" + nameBuf_ + u8"」。";
				stateDirty_ = true;
			}
		}
		editIdx_ = -1;
	}

	// ---- plumbing ------------------------------------------------------------

	bool pageHasT17()
	{
		if (onCustom()) return false;
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
		if (p == page_) return;
		page_ = p;
		copied_ = false;
		st().filterDirty = true;
		st().dirty = true;
		state_.page = pageId();
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
	void buildCorpus()
	{
		PageState& s = st();
		std::vector<RegexGen::Entry> es;
		es.reserve(entries().size());
		for (const RegexEntryDef& d : entries()) {
			RegexGen::Entry e;
			e.id = d.id;
			// The lines in the language being built for. "No false positives" is
			// a claim about ONE language's list: two entries the Chinese cannot
			// tell apart may be trivially separable in English, and the other way
			// round, so the corpus has to be the one the player will paste into.
			const std::vector<std::string>& want = (lang_ == Lang::Zh) ? d.zh : d.en;
			const std::vector<std::string>& fallback = (lang_ == Lang::Zh) ? d.en : d.zh;
			e.texts = want.empty() ? fallback : want;
			es.push_back(std::move(e));
		}
		s.corpus.Reset(std::move(es));
		s.corpusReady = true;
		s.dirty = true;
	}

	void recompute()
	{
		PageState& s = st();
		std::vector<int> sel;
		for (int i = 0; i < (int)s.picked.size(); i++)
			if (s.picked[i]) sel.push_back(i);
		s.result = s.corpus.Build(sel, mode_);
		s.dirty = false;
	}

	void rebuildCustom()
	{
		customTextDirty_ = false;
		customEntries_.clear();
		size_t start = 0;
		for (size_t i = 0; i <= customText_.size(); i++) {
			if (i != customText_.size() && customText_[i] != '\n') continue;
			std::string line = customText_.substr(start, i - start);
			start = i + 1;
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
			size_t b = line.find_first_not_of(' ');
			if (b == std::string::npos) continue;
			line = line.substr(b);
			RegexEntryDef d;
			d.id = line;
			d.zh.push_back(line);
			customEntries_.push_back(std::move(d));
		}
		PageState& s = pages_[customIdx()];
		// Ticks are keyed by position here, so a changed list has to drop them
		// rather than silently move them onto different lines.
		s.picked.assign(customEntries_.size(), 0);
		s.corpusReady = false;
		s.filterDirty = true;
		s.dirty = true;
	}

	const ToolPanelHost* host_ = nullptr;
	std::wstring exeDir_, game_;
	RegexDataset data_;
	bool dataOk_ = false;
	bool ownGame_ = false;      // a catalogue exists for the launcher's game
	std::string dataErr_;

	RegexUiState state_;
	bool stateDirty_ = false;

	std::vector<PageState> pages_;
	int page_ = 0;
	RegexGen::Mode mode_ = RegexGen::Mode::Any;
	Lang lang_ = Lang::Zh;
	bool bilingual_ = true;

	std::vector<RegexEntryDef> customEntries_;
	std::string customText_;
	std::string customNote_ = kCustomPageNote;
	bool customTextDirty_ = false;

	Modal modal_ = Modal::None;
	bool renameMode_ = false;
	int editIdx_ = -1;
	std::string nameBuf_;
	std::string notice_;

	std::string copyRequest_;
	bool copied_ = false;
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
