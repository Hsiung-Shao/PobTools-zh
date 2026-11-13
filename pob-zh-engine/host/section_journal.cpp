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

#include <algorithm>
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
	// a rule the new version dropped counts too: it is the only way to keep that edit
	return r.applied > 0 || !r.conflicts.empty() || r.customImported > 0 || !r.missing.empty();
}

std::string Join(const std::vector<std::string>& lines)
{
	std::string out;
	for (const std::string& l : lines) out += l + "\r\n";
	return out;
}

} // namespace

namespace {

std::wstring JournalPathFor(const EditorShell& s, const std::string& fileName)
{
	std::wstring name = EdWiden(fileName);
	const size_t dot = name.find_last_of(L'.');
	if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
	if (name.empty()) name = L"filter";
	return JournalDir(s) + name + L".json";
}

void EnsureJournalDir(const EditorShell& s)
{
	const std::wstring dir = JournalDir(s);
	const std::wstring parent = dir.substr(0, dir.find_last_of(L'\\', dir.size() - 2) + 1);
	CreateDirectoryW(parent.c_str(), nullptr);
	CreateDirectoryW(dir.c_str(), nullptr);
}

bool ReadAll(const std::wstring& path, std::string* out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER sz{};
	GetFileSizeEx(h, &sz);
	out->resize((size_t)sz.QuadPart);
	DWORD got = 0;
	const bool ok = sz.QuadPart == 0 || (ReadFile(h, &(*out)[0], (DWORD)sz.QuadPart, &got, nullptr) && got == (DWORD)sz.QuadPart);
	CloseHandle(h);
	return ok;
}

bool WriteAll(const std::wstring& path, const std::string& data)
{
	const std::wstring tmp = path + L".tmp";
	HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD put = 0;
	const bool ok = data.empty() || (WriteFile(h, data.data(), (DWORD)data.size(), &put, nullptr) && put == (DWORD)data.size());
	CloseHandle(h);
	if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	return true;
}

// The subscription this local copy follows, when it has a newer NeverSink version.
void CheckSubscription(EditorShell& s)
{
	s.onlineBanner = false;
	if (s.journal.subscription.empty() || s.testMode) return;
	for (const OnlineFilterEntry& e : ListOnlineFilters()) {
		if (e.name != s.journal.subscription) continue;
		const std::string here = FilterBannerValue(s.model, "VERSION");
		if (!e.version.empty() && e.version != here) {
			s.onlineUpdate = e;
			s.onlineBanner = true;
		}
		return;
	}
}

} // namespace

std::wstring EdJournalPath(const EditorShell& s)
{
	return JournalPathFor(s, s.model.name);
}

// A rule already copied into the custom zone (not saved yet) is no longer asked about.
static JournalReport WithoutKept(const EditorShell& s, JournalReport r)
{
	r.missing.erase(std::remove_if(r.missing.begin(), r.missing.end(),
	                               [&](const std::string& k) { return s.journalDropOnSave.count(k) > 0; }),
	                r.missing.end());
	return r;
}

void EdJournalOnOpen(EditorShell& s)
{
	s.journal = EditJournal{};
	s.journalPreview = JournalReport{};
	s.journalChoices.clear();
	s.journalCarried.clear();
	s.journalDropOnSave.clear();
	s.journalBanner = false;
	s.journalDialogOpen = false;
	if (!s.loaded) return;
	JournalLoad(EdJournalPath(s), &s.journal);
	CheckSubscription(s);
	if (s.journal.rules.empty() && s.journal.customZone.empty()) return;
	s.journalPreview = JournalApply(s.journal, s.doc, {}, true);
	s.journalBanner = Pending(s.journalPreview);
}

void EdJournalOnSave(EditorShell& s, const FilterFile& baseline)
{
	const bool had = !s.journal.rules.empty() || !s.journal.customZone.empty();
	if (!had && !AnyNeverSinkRule(s.model)) return;   // not a NeverSink filter: nothing to keep
	// kept as custom rules, now saved in the file: out of the journal for good
	// (only those still there: an undo may have taken the copy out again)
	for (const std::string& k : s.journalDropOnSave)
		if (CustomZoneHasRule(s.model, k)) s.journal.rules.erase(k);
	s.journalDropOnSave.clear();
	JournalRecord(s.journal, baseline, s.model, &s.journalCarried);
	s.journalCarried.clear();   // recorded against this version now
	EnsureJournalDir(s);
	std::string err;
	if (!JournalSave(EdJournalPath(s), s.journal, &err))
		s.Notify(u8"已儲存，但修改紀錄沒有寫入（" + err + u8"）：之後 NeverSink 更新時可能沒辦法套回", true);
	s.journalPreview = WithoutKept(s, JournalApply(s.journal, s.doc, {}, true));
	s.journalBanner = s.journalBanner && Pending(s.journalPreview);
}

void DrawJournalBanner(EditorShell& s)
{
	if (s.onlineBanner && s.loaded) {
		const std::string here = FilterBannerValue(s.model, "VERSION");
		const std::string title = u8"訂閱的 NeverSink 有新版 " + s.onlineUpdate.version + u8"（這份是 " + here + u8"）";
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
		const PobUi::BannerResult r = PobUi::Banner("##feonline", PobUi::BannerTone::Info, PobIcon::Download, title.c_str(),
			u8"用訂閱的新版重建這個本機檔，再把你的修改套回去（原檔會備份成 .bak）。", false, u8"更新並保留修改", true);
		if (r == PobUi::BannerResult::Action) {
			if (s.model.dirty) s.Notify(u8"先儲存或還原目前的修改，再從訂閱更新", true);
			else s.onlineConfirmOpen = true;
		} else if (r == PobUi::BannerResult::Close) {
			s.onlineBanner = false;
		}
	}
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
	// 從訂閱建立可編輯的本機檔
	if (s.wantOnlineDialog) {
		s.wantOnlineDialog = false;
		if (!s.testMode) s.onlineList = ListOnlineFilters();
		s.onlineSel = 0;
		s.onlineDialogOpen = true;
	}
	if (PobUi::BeginDialog(u8"##feonlinedlg", &s.onlineDialogOpen, u8"從訂閱建立可編輯的本機檔",
	                       u8"遊戲會自己重新下載訂閱的過濾器、蓋掉修改，所以改在一份本機副本上。之後訂閱更新時，這裡會提醒你更新並把修改套回去。遊戲裡要改選這份本機檔。",
	                       nullptr, 560.0f)) {
		if (s.onlineList.empty()) {
			PobUi::Hint(u8"找不到訂閱的過濾器（Documents\\My Games\\Path of Exile\\OnlineFilters）。先在遊戲裡訂閱並進一次遊戲。");
		} else {
			for (int i = 0; i < (int)s.onlineList.size(); i++) {
				const OnlineFilterEntry& e = s.onlineList[i];
				const std::string row = e.name + u8"    " + e.version;
				if (ImGui::Selectable(row.c_str(), s.onlineSel == i)) s.onlineSel = i;
			}
			const std::string local = SafeFilterFileName(s.onlineList[s.onlineSel].name) + " (PobTools).filter";
			ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			PobUi::Hint((u8"會建立：My Games\\Path of Exile\\" + local).c_str());
		}
		const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, nullptr, u8"建立並開啟", !s.onlineList.empty());
		if (r == PobUi::DialogResult::Primary && !s.onlineList.empty()) {
			const OnlineFilterEntry e = s.onlineList[s.onlineSel];
			// the name comes from the game's download: never a path (T40d)
			const std::string localName = SafeFilterFileName(e.name) + " (PobTools).filter";
			const std::wstring base = Poe1FilterBaseDir();
			std::string text;
			if (s.testMode) {
				s.Notify(u8"測試模式不建立檔案");
			} else if (base.empty() || !ReadAll(e.path, &text)) {
				s.Notify(u8"讀不到訂閱的過濾器", true);
			} else {
				const std::wstring local = base + EdWiden(localName);
				if (GetFileAttributesW(local.c_str()) == INVALID_FILE_ATTRIBUTES && !WriteAll(local, StripOnlineHeader(text))) {
					s.Notify(u8"沒辦法建立 " + localName, true);
				} else {
					// the journal knows which subscription this copy follows
					EditJournal j;
					JournalLoad(JournalPathFor(s, localName), &j);
					j.subscription = e.name;
					EnsureJournalDir(s);
					std::string err;
					JournalSave(JournalPathFor(s, localName), j, &err);
					s.RequestOpen(local);
					s.Notify(u8"已建立 " + localName + u8"：到遊戲「選項 > 遊戲 > UI」改選這份過濾器");
				}
			}
		}
		PobUi::EndDialog();
	}
	// 更新並保留修改
	if (PobUi::BeginDialog(u8"##feonlineupd", &s.onlineConfirmOpen, u8"用訂閱的新版更新這個檔？",
	                       u8"這份本機檔會換成訂閱的新版內容（原檔備份成 .bak），接著把你的修改套回去，可以逐處確認。",
	                       nullptr, 480.0f)) {
		const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, nullptr, u8"更新");
		if (r == PobUi::DialogResult::Primary) {
			std::string text;
			const std::wstring path = s.model.path;
			if (s.testMode) {
				s.Notify(u8"測試模式不寫檔");
			} else if (!ReadAll(s.onlineUpdate.path, &text)) {
				s.Notify(u8"讀不到訂閱的過濾器", true);
			} else if (!CopyFileW(path.c_str(), (path + L".bak").c_str(), FALSE) || !WriteAll(path, StripOnlineHeader(text))) {
				s.Notify(u8"更新失敗：檔案可能正被其他程式使用", true);
			} else {
				s.onlineBanner = false;
				s.OpenByPath(path, true);   // the journal banner offers your edits on the new version
				s.Notify(u8"已更新成 " + s.onlineUpdate.version + u8"：按上方「檢視並套用」把修改套回去");
			}
		}
		PobUi::EndDialog();
	}

	if (s.wantJournalDialog) {
		s.wantJournalDialog = false;
		s.journalDialogOpen = true;
		s.journalPreview = WithoutKept(s, JournalApply(s.journal, s.doc, s.journalChoices, true));
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

	bool dropMissing = false;
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
				if (got > 0) { n += got; s.journalDropOnSave.insert(k); }   // leaves the journal when the file is saved
			}
			s.Notify(u8"已把 " + std::to_string(n) + u8" 條新版沒有的規則放進自訂區（還沒儲存；存檔後才從修改紀錄移除）");
			s.journalPreview = WithoutKept(s, JournalApply(s.journal, s.doc, s.journalChoices, true));
		}
		if (ImGui::IsItemHovered())
			PobUi::Tooltip(u8"用你改過的樣子放進檔案最上方的自訂區；自訂區優先，這些物品會照你的設定顯示");
		ImGui::SameLine();
		if (PobUi::Button(u8"不保留", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) dropMissing = true;
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"從修改紀錄移除這些規則，之後不再提示");
		if (dropMissing) {
			// let go: out of the journal for good
			for (const std::string& k : p.missing) s.journal.rules.erase(k);
			if (!s.testMode) {
				EnsureJournalDir(s);
				std::string err;
				JournalSave(EdJournalPath(s), s.journal, &err);
			}
			s.journalPreview = WithoutKept(s, JournalApply(s.journal, s.doc, s.journalChoices, true));
		}
	}

	ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, nullptr, u8"套用", Pending(p));
	if (r == PobUi::DialogResult::Primary) {
		const JournalReport done = JournalApply(s.journal, s.doc, s.journalChoices, false);
		s.journalCarried.insert(done.carried.begin(), done.carried.end());
		s.journalPreview = WithoutKept(s, done);
		s.journalBanner = false;
		std::string msg = u8"已套回 " + std::to_string(done.applied) + u8" 條規則的修改";
		if (done.customImported > 0) msg += u8"、放回 " + std::to_string(done.customImported) + u8" 條自訂規則";
		msg += u8"（還沒儲存，可以復原）";
		s.Notify(msg);
	}
	PobUi::EndDialog();
}
