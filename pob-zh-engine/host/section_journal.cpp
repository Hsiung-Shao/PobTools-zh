// NeverSink 更新後保留修改: the journal of your edits to NeverSink's own rules,
// recorded at every save and offered back when the file is a newer NeverSink
// (a banner, then a dialog listing what applies, what both of you changed and
// what the new version no longer has). The merge itself is filter_edit_journal.
#include "editor_shell.h"
#include "editor_util.h"
#include "filter_edit_journal.h"
#include "filter_i18n.h"
#include "filter_parser.h"
#include "filter_schema.h"
#include "custom_rules_io.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_icons.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <imgui.h>

#include <string>
#include <vector>

namespace Tok = PobUi::Tok;

namespace {

std::wstring JournalDir(const EditorShell& s)
{
	if (s.testMode) {
		// a test run never touches the real journals
		wchar_t tmp[MAX_PATH] = {};
		GetTempPathW(MAX_PATH, tmp);
		return std::wstring(tmp) + L"pobtools_fe_journal\\";
	}
	return s.exeDir + L"PobTools\\filter_edits\\";
}

std::string SettingZh(const std::string& setting)
{
	if (setting == "Show/Hide") return u8"顯示 / 隱藏";
	if (setting == "header") return u8"規則標頭";
	return FilterSchemaKeywordZh(setting);
}

// The rule's name as the list shows it (from this file's rule of that identity),
// else the identity translated as far as it goes.
std::string RuleZh(const EditorShell& s, const std::string& key)
{
	for (const FilterBlock& b : s.model.blocks)
		if (FilterRuleKey(s.model, b) == key) return EdRuleLabel(s.model, b, s.i18n, nullptr);
	return NeverSinkHeaderZh(key);
}

bool AnyNeverSinkRule(const FilterFile& f)
{
	for (const FilterBlock& b : f.blocks)
		if (!FilterRuleKey(f, b).empty()) return true;
	return false;
}

bool Pending(const JournalReport& r)
{
	return r.applied > 0 || !r.conflicts.empty() || r.customImported > 0;
}

std::string Join(const std::vector<std::string>& lines)
{
	std::string out;
	for (const std::string& l : lines) out += l + "\r\n";
	return out;
}

} // namespace

std::wstring EdJournalPath(const EditorShell& s)
{
	std::wstring name = EdWiden(s.model.name);
	const size_t dot = name.find_last_of(L'.');
	if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
	if (name.empty()) name = L"filter";
	return JournalDir(s) + name + L".json";
}

void EdJournalOnOpen(EditorShell& s)
{
	s.journal = EditJournal{};
	s.journalPreview = JournalReport{};
	s.journalChoices.clear();
	s.journalCarried.clear();
	s.journalBanner = false;
	s.journalDialogOpen = false;
	if (!s.loaded) return;
	JournalLoad(EdJournalPath(s), &s.journal);
	if (s.journal.rules.empty() && s.journal.customZone.empty()) return;
	s.journalPreview = JournalApply(s.journal, s.doc, {}, true);
	s.journalBanner = Pending(s.journalPreview);
}

void EdJournalOnSave(EditorShell& s, const FilterFile& baseline)
{
	const bool had = !s.journal.rules.empty() || !s.journal.customZone.empty();
	if (!had && !AnyNeverSinkRule(s.model)) return;   // not a NeverSink filter: nothing to keep
	JournalRecord(s.journal, baseline, s.model, &s.journalCarried);
	s.journalCarried.clear();   // recorded against this version now
	const std::wstring dir = JournalDir(s);
	const std::wstring parent = dir.substr(0, dir.find_last_of(L'\\', dir.size() - 2) + 1);
	CreateDirectoryW(parent.c_str(), nullptr);
	CreateDirectoryW(dir.c_str(), nullptr);
	std::string err;
	if (!JournalSave(EdJournalPath(s), s.journal, &err))
		s.Notify(u8"已儲存，但修改紀錄沒有寫入（" + err + u8"）：之後 NeverSink 更新時可能沒辦法套回", true);
	s.journalPreview = JournalApply(s.journal, s.doc, {}, true);
	s.journalBanner = s.journalBanner && Pending(s.journalPreview);
}

void DrawJournalBanner(EditorShell& s)
{
	if (!s.journalBanner || !s.loaded) return;
	const JournalReport& p = s.journalPreview;
	const std::string here = FilterBannerValue(s.model, "VERSION");
	std::string title = u8"找到你之前對 NeverSink 規則做的修改";
	if (!s.journal.fromVersion.empty() && !here.empty() && here != s.journal.fromVersion)
		title += u8"（紀錄是 " + s.journal.fromVersion + u8"，這份是 " + here + u8"）";
	// no spaces inside the sentence: ImGui wraps only at spaces
	std::string desc = u8"可以套回" + std::to_string(p.applied) + u8"條規則";
	if (!p.conflicts.empty()) desc += u8"，其中有" + std::to_string(p.conflicts.size()) + u8"處你和新版都改過，要你決定";
	if (p.customImported > 0) desc += u8"，並放回" + std::to_string(p.customImported) + u8"條自訂規則";
	if (!p.missing.empty()) desc += u8"；" + std::to_string(p.missing.size()) + u8"條規則新版沒有了";
	desc += u8"。";
	ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	const PobUi::BannerResult r = PobUi::Banner("##fejournal", PobUi::BannerTone::Info, PobIcon::History, title.c_str(),
		desc.c_str(), false, u8"檢視並套用", true);
	if (r == PobUi::BannerResult::Action) s.wantJournalDialog = true;
	else if (r == PobUi::BannerResult::Close) s.journalBanner = false;
}

void DrawJournalDialog(EditorShell& s)
{
	if (s.wantJournalDialog) {
		s.wantJournalDialog = false;
		s.journalDialogOpen = true;
		s.journalPreview = JournalApply(s.journal, s.doc, s.journalChoices, true);
	}
	const JournalReport& p = s.journalPreview;
	const std::string body = u8"可以直接套回" + std::to_string(p.applied) +
		u8"條規則。下面是你和NeverSink新版都改過的地方，每一處選要用哪一個（預設用你的）。套用後還沒儲存，可以按「復原」退回。";
	if (!PobUi::BeginDialog(u8"##fejournaldlg", &s.journalDialogOpen, u8"套回之前的修改", body.c_str(), nullptr, 640.0f)) return;
	ImFont* sf = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
	const float w = ImGui::GetContentRegionAvail().x;

	if (!p.conflicts.empty()) {
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
		PobUi::Hint((std::to_string(p.conflicts.size()) + u8" 處兩邊都改過").c_str());
		// a row: the rule and setting, the choice, then the two versions
		const ImGuiStyle& st = ImGui::GetStyle();
		const float rowH = ImGui::GetTextLineHeight() + PobUi::ControlH() + 2.0f * sf->FontSize + 5.0f * st.ItemSpacing.y +
		                   PobUi::D(6.0f);
		const float listH = (std::min)(PobUi::D(340.0f), rowH * (float)p.conflicts.size() + 2.0f * st.WindowPadding.y);
		ImGui::BeginChild("##jconf", ImVec2(w, listH), true);
		const char* opts[2] = { u8"用我的", u8"用新版" };
		for (size_t i = 0; i < p.conflicts.size(); i++) {
			const JournalConflict& c = p.conflicts[i];
			ImGui::PushID((int)i);
			const std::string ck = c.key + "|" + c.setting;
			int sel = s.journalChoices.count(ck) && s.journalChoices[ck] ? 1 : 0;
			ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
			ImGui::TextUnformatted((RuleZh(s, c.key) + u8" · " + SettingZh(c.setting)).c_str());
			ImGui::PopTextWrapPos();
			if (PobUi::Segmented("##pick", &sel, opts, 2)) s.journalChoices[ck] = sel == 1;
			ImGui::PushFont(sf);
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sel == 0 ? Tok::Text : Tok::TextMuted));
			ImGui::TextUnformatted((u8"你的：" + (c.mine.empty() ? std::string(u8"（沒有這一行）") : c.mine)).c_str());
			ImGui::PopStyleColor();
			ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(sel == 1 ? Tok::Text : Tok::TextMuted));
			ImGui::TextUnformatted((u8"新版：" + (c.theirs.empty() ? std::string(u8"（新版拿掉了這一行）") : c.theirs)).c_str());
			ImGui::PopStyleColor();
			ImGui::PopFont();
			ImGui::Separator();
			ImGui::PopID();
		}
		ImGui::EndChild();
	}

	if (!p.missing.empty()) {
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
		std::string names;
		for (size_t i = 0; i < p.missing.size() && i < 6; i++) names += (i ? u8"、" : "") + RuleZh(s, p.missing[i]);
		if (p.missing.size() > 6) names += u8"…";
		PobUi::Hint((std::to_string(p.missing.size()) + u8" 條規則新版沒有了：" + names).c_str(), w);
		if (PobUi::Button(u8"把這些改成自訂規則保留", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) {
			int n = 0;
			for (const std::string& k : p.missing) {
				const auto it = s.journal.rules.find(k);
				if (it == s.journal.rules.end()) continue;
				std::string err;
				const int got = ImportCustomRules(s.doc, Join(it->second.after), &err);
				if (got > 0) n += got;
			}
			s.Notify(u8"已把 " + std::to_string(n) + u8" 條新版沒有的規則放進自訂區（還沒儲存）");
		}
		if (ImGui::IsItemHovered())
			PobUi::Tooltip(u8"用你改過的樣子放進檔案最上方的自訂區；自訂區優先，這些物品會照你的設定顯示");
	}

	ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, nullptr, u8"套用", Pending(p));
	if (r == PobUi::DialogResult::Primary) {
		const JournalReport done = JournalApply(s.journal, s.doc, s.journalChoices, false);
		s.journalCarried.insert(done.carried.begin(), done.carried.end());
		s.journalPreview = done;
		s.journalBanner = false;
		std::string msg = u8"已套回 " + std::to_string(done.applied) + u8" 條規則的修改";
		if (done.customImported > 0) msg += u8"、放回 " + std::to_string(done.customImported) + u8" 條自訂規則";
		msg += u8"（還沒儲存，可以復原）";
		s.Notify(msg);
	}
	PobUi::EndDialog();
}
