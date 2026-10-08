#include "editor_shell.h"
#include "editor_util.h"
#include "filter_parser.h"
#include "custom_rules_io.h"
#include "sound_manager.h"   // BrowseSoundFolder
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_icons.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cmath>
#include <string>

namespace Tok = PobUi::Tok;

namespace {

const char* const kSaveHint = u8"存檔後要在遊戲「選項 > 遊戲 > UI」重新選一次過濾器才會生效";

// *.filter in one folder (the test folder), sorted by name.
std::vector<FilterListEntry> ListFiltersIn(const std::wstring& dir)
{
	std::vector<FilterListEntry> out;
	if (dir.empty()) return out;
	std::wstring base = dir;
	if (base.back() != L'\\' && base.back() != L'/') base += L'\\';
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((base + L"*.filter").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return out;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
		FilterListEntry e;
		e.path = base + fd.cFileName;
		e.name = EdNarrow(fd.cFileName);
		out.push_back(std::move(e));
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	std::sort(out.begin(), out.end(), [](const FilterListEntry& a, const FilterListEntry& b) { return a.name < b.name; });
	return out;
}

std::string StemOf(const std::string& name)
{
	size_t dot = name.rfind('.');
	return dot == std::string::npos ? name : name.substr(0, dot);
}

// A small neutral badge ("PoE1") in the hint face.
void Badge(const char* text)
{
	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	ImFont* f = wf.small ? wf.small : ImGui::GetFont();
	const float px = f->FontSize;
	const ImVec2 ts = f->CalcTextSizeA(px, FLT_MAX, 0.0f, text);
	const float padX = PobUi::D(7.0f), h = std::floor(px + PobUi::D(6.0f));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(p, p + ImVec2(ts.x + padX * 2.0f, h), Tok::Surface3, PobUi::D(4.0f));
	dl->AddText(f, px, ImVec2(p.x + padX, p.y + std::floor((h - ts.y) * 0.5f)), Tok::TextMuted, text);
	ImGui::Dummy(ImVec2(ts.x + padX * 2.0f, h));
}

float BadgeWidth(const char* text)
{
	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	ImFont* f = wf.small ? wf.small : ImGui::GetFont();
	return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, text).x + PobUi::D(14.0f);
}

} // namespace

// ---- deferred dialogs --------------------------------------------------------
//
// Every Win32 common dialog this editor opens goes through here, and here is only
// ever reached after a frame has been presented. See EdDialog in the header for
// why: a common dialog runs its own modal message loop, so opening one from inside
// a frame leaves that frame half-drawn -- and as a launcher tab it stops the
// launcher's loop, which is what keeps docked POB windows hidden and positioned.

void EdRunDeferredDialogs(EditorShell& s)
{
	const EdDialog want = s.pendingDialog;
	if (want == EdDialog::None) return;
	// Cleared FIRST. If the dialog itself somehow left the intent set, the next
	// frame would reopen it, and the user could not get out of it.
	s.pendingDialog = EdDialog::None;
	// A test run never opens a Win32 dialog: it would sit on an invisible window.
	if (s.testMode) return;

	switch (want) {
		case EdDialog::OpenFilter: {
			std::wstring p = EdFilterDialog(s.initialDir, false, s.hostHwnd);
			if (!p.empty()) s.RequestOpen(p);
			break;
		}
		case EdDialog::SaveFilterAs: {
			std::wstring p = EdFilterDialog(s.initialDir, true, s.hostHwnd);
			if (!p.empty()) s.SaveAs(p);
			break;
		}
		case EdDialog::ExportCustom: {
			std::vector<int> sel;
			sel.swap(s.pendingExportSel);   // consumed, whatever the user answers
			if (sel.empty()) break;
			std::wstring p = EdFilterDialog(s.initialDir, true, s.hostHwnd);
			if (!p.empty()) {
				std::string frag = ExportCustomRules(s.model, sel, u8"自訂規則");
				std::string err;
				if (SaveCustomRulesFile(p, frag, &err))
					s.Notify(u8"已匯出 " + std::to_string((int)sel.size()) + u8" 條規則");
				else
					s.Notify(u8"匯出失敗：" + err, true);
			}
			break;
		}
		case EdDialog::ImportCustom: {
			std::wstring p = EdFilterDialog(s.initialDir, false, s.hostHwnd);
			if (!p.empty()) {
				std::vector<unsigned char> data = EdReadFile(p);
				std::string frag(data.begin(), data.end());
				std::string err;
				int n = ImportCustomRules(s.doc, frag, &err);
				if (n < 0) s.Notify(u8"匯入失敗：" + err, true);
				else s.Notify(u8"已匯入 " + std::to_string(n) + u8" 條規則到自訂區（還沒儲存）");
			}
			break;
		}
		case EdDialog::SoundFolder: {
			std::wstring f = BrowseSoundFolder(s.hostHwnd);
			if (!f.empty()) s.sounds.SetFolder(f);
			break;
		}
		case EdDialog::None: break;
	}
}

// ---- EditorShell methods -----------------------------------------------------

bool EditorShell::OpenByPath(const std::wstring& path, bool force)
{
	if (model.dirty && !force) {
		RequestOpen(path);
		return false;
	}
	bool ok = false;
	FilterFile f = LoadFilter(path, &ok);
	if (!ok) {
		loadFailedPath = path;
		return false;
	}
	loadFailedPath.clear();
	model = std::move(f);
	loaded = true;
	selectedBlock = model.blocks.empty() ? -1 : 0;
	doc.Attach(&model);              // bumps structureVersion -> row caches rebuild
	doc.CaptureBaseline();           // "已修改" is measured against this
	watch.Reset(model.path);
	selAnchor = BlockAnchor{};
	batchMode = false;
	batchSel.clear();
	status = std::to_string(model.blocks.size()) + u8" 個規則區塊 · " + model.name;
	// a file opened from elsewhere joins the list, so the Select can show it
	bool listed = false;
	for (const FilterListEntry& e : fileList)
		if (e.path == model.path) { listed = true; break; }
	if (!listed) {
		FilterListEntry e;
		e.path = model.path;
		e.name = model.name;
		fileList.push_back(std::move(e));
	}
	return true;
}

void EditorShell::RequestOpen(const std::wstring& path)
{
	if (loaded && model.dirty) {
		pendingAction = EdPendingAction::OpenPath;
		pendingPath = path;
		askSaveFirst = true;
		return;
	}
	OpenByPath(path, true);
}

void EditorShell::RequestReload()
{
	if (!loaded || model.path.empty()) return;
	if (model.dirty) {
		pendingAction = EdPendingAction::Reload;
		pendingPath = model.path;
		askSaveFirst = true;
		return;
	}
	OpenByPath(model.path, true);
}

void EditorShell::Notify(const std::string& text, bool error)
{
	status = text;
	PobUi::ShowToast(text.c_str(), error ? PobUi::Tone::Bad : PobUi::Tone::Ok);
}

bool EditorShell::Save()
{
	if (!loaded) return false;
	std::string err;
	if (!SaveFilter(model, &err)) {
		Notify(u8"儲存失敗：" + err, true);
		return false;
	}
	// what is on disk now is the new baseline -- and our own write is not an
	// outside change
	if (doc.file() != &model) doc.Attach(&model);
	doc.CaptureBaseline();
	watch.Reset(model.path);
	Notify(u8"已儲存。到遊戲「選項 > 遊戲 > UI」重新選一次過濾器才會生效");
	return true;
}

bool EditorShell::SaveAs(const std::wstring& path)
{
	if (!loaded || path.empty()) return false;
	model.path = path;
	size_t slash = path.find_last_of(L"\\/");
	model.name = EdNarrow(slash == std::wstring::npos ? path : path.substr(slash + 1));
	const bool ok = Save();
	if (ok) {
		bool listed = false;
		for (const FilterListEntry& e : fileList)
			if (e.path == model.path) { listed = true; break; }
		if (!listed) {
			FilterListEntry e;
			e.path = model.path;
			e.name = model.name;
			fileList.push_back(std::move(e));
		}
	}
	return ok;
}

int EditorShell::UnsavedCount()
{
	if (!loaded || !model.dirty) return 0;
	if (doc.file() != &model) doc.Attach(&model);
	return doc.UnsavedBlockCount(true);
}

// ---- recently used colours ----------------------------------------------------

namespace {
std::uint32_t PackRgba(const int c[4])
{
	auto b = [](int v) { return (std::uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); };
	return (b(c[0]) << 24) | (b(c[1]) << 16) | (b(c[2]) << 8) | b(c[3]);
}
} // namespace

void PushRecentColor(std::vector<std::uint32_t>& list, const int rgba[4])
{
	const std::uint32_t c = PackRgba(rgba);
	list.erase(std::remove(list.begin(), list.end(), c), list.end());
	list.insert(list.begin(), c);
	if ((int)list.size() > kRecentColorMax) list.resize(kRecentColorMax);
}

std::string EncodeRecentColors(const std::vector<std::uint32_t>& list)
{
	std::string out;
	for (std::uint32_t c : list) {
		if (!out.empty()) out += ';';
		out += std::to_string((c >> 24) & 255) + ',' + std::to_string((c >> 16) & 255) + ',' +
		       std::to_string((c >> 8) & 255) + ',' + std::to_string(c & 255);
	}
	return out;
}

std::vector<std::uint32_t> DecodeRecentColors(const std::string& text)
{
	std::vector<std::uint32_t> out;
	size_t p = 0;
	while (p < text.size() && (int)out.size() < kRecentColorMax) {
		size_t e = text.find(';', p);
		if (e == std::string::npos) e = text.size();
		int v[4] = { -1, -1, -1, -1 };
		if (sscanf_s(text.substr(p, e - p).c_str(), "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) == 4) {
			bool ok = true;
			for (int k = 0; k < 4; k++) if (v[k] < 0 || v[k] > 255) ok = false;
			if (ok) {
				const std::uint32_t c = PackRgba(v);
				if (std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c);
			}
		}
		p = e + 1;
	}
	return out;
}

// ---- settings persistence (pob-zh.ini [PobTools]) ---------------------------

void LoadEditorSettings(EditorShell& s)
{
	std::wstring ini = s.exeDir + L"pob-zh.ini";
	wchar_t buf[128] = L"";
	GetPrivateProfileStringW(L"PobTools", L"League", L"Mirage", buf, 128, ini.c_str());
	s.league = EdNarrow(buf);
	if (s.league.empty()) s.league = "Mirage";
	s.economyEnabled = GetPrivateProfileIntW(L"PobTools", L"EconomyEnabled", 0, ini.c_str()) != 0;
	wchar_t rc[512] = L"";
	GetPrivateProfileStringW(L"PobTools", L"FilterRecentColors", L"", rc, 512, ini.c_str());
	s.recentColors = DecodeRecentColors(EdNarrow(rc));
}

void SaveEditorSettings(EditorShell& s)
{
	if (s.testMode) return;   // a test run writes nothing
	std::wstring ini = s.exeDir + L"pob-zh.ini";
	WritePrivateProfileStringW(L"PobTools", L"League", EdWiden(s.league).c_str(), ini.c_str());
	WritePrivateProfileStringW(L"PobTools", L"EconomyEnabled", s.economyEnabled ? L"1" : L"0", ini.c_str());
	WritePrivateProfileStringW(L"PobTools", L"FilterRecentColors", EdWiden(EncodeRecentColors(s.recentColors)).c_str(),
	                           ini.c_str());
}

void EdRefreshFileList(EditorShell& s)
{
	s.fileList = s.testMode ? ListFiltersIn(s.testDir) : ListFilters();
	if (s.loaded) {
		bool listed = false;
		for (const FilterListEntry& e : s.fileList)
			if (e.path == s.model.path) { listed = true; break; }
		if (!listed) {
			FilterListEntry e;
			e.path = s.model.path;
			e.name = s.model.name;
			s.fileList.push_back(std::move(e));
		}
	}
}

// ---- "save first?" -----------------------------------------------------------

EdSaveAnswer DrawSaveFirstDialog(EditorShell& s, const char* popupId, bool* open, bool closing)
{
	const std::string title = u8"先儲存「" + StemOf(s.model.name) + u8"」嗎？";
	const int n = s.UnsavedCount();
	// no spaces in the sentence (ImGui wraps CJK only at spaces)
	std::string body = u8"有" + std::to_string(n) + u8"處變更還沒儲存。不儲存的話，";
	if (closing) body += u8"關閉後這些變更會消失。";
	else if (s.pendingAction == EdPendingAction::Reload) body += u8"重新載入後這些變更會消失。";
	else body += u8"切換到其他檔案後這些變更會消失。";
	if (!PobUi::BeginDialog(popupId, open, title.c_str(), body.c_str())) return EdSaveAnswer::None;
	const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, u8"不儲存", u8"儲存");
	PobUi::EndDialog();
	switch (r) {
		case PobUi::DialogResult::Cancel: return EdSaveAnswer::Cancel;
		case PobUi::DialogResult::Danger: return EdSaveAnswer::Discard;
		case PobUi::DialogResult::Primary: return EdSaveAnswer::Save;
		default: return EdSaveAnswer::None;
	}
}

// ---- header ------------------------------------------------------------------

namespace {

void DrawMoreMenu(EditorShell& s, bool& openMore, ImVec2 anchor)
{
	if (openMore) {
		ImGui::OpenPopup("##femore");
		openMore = false;
	}
	ImGui::SetNextWindowPos(anchor, ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
	if (!PobUi::BeginMenuPopup("##femore")) return;
	if (PobUi::MenuRow(PobIcon::FolderOpen, u8"開啟其他檔案…")) s.pendingDialog = EdDialog::OpenFilter;
	if (PobUi::MenuRow(PobIcon::Save, u8"另存新檔…", nullptr, s.loaded)) s.pendingDialog = EdDialog::SaveFilterAs;
	if (PobUi::MenuRow(PobIcon::Refresh, u8"重新整理清單")) EdRefreshFileList(s);
	if (PobUi::MenuRow(PobIcon::RotateCcw, u8"重新載入", nullptr, s.loaded && !s.model.path.empty()))
		s.RequestReload();
	PobUi::MenuSeparator();
	if (PobUi::MenuRow(PobIcon::Download, u8"匯入自訂規則…", nullptr, s.loaded))
		s.pendingDialog = EdDialog::ImportCustom;
	if (PobUi::MenuRow(PobIcon::Upload, u8"匯出自訂規則…", nullptr, s.loaded)) {
		std::vector<int> sel;
		if (s.batchMode)
			for (int i = 0; i < (int)s.batchSel.size(); i++) { if (s.batchSel[i]) sel.push_back(i); }
		if (sel.empty() && s.selectedBlock >= 0) sel.push_back(s.selectedBlock);
		if (sel.empty()) {
			s.Notify(u8"先選一條規則（或在批量修改裡勾選幾條）再匯出", true);
		} else {
			// Carried to the deferred step: the selection can change between the
			// click and the dialog closing, and what was exported has to be what was
			// selected when the menu was used.
			s.pendingExportSel = sel;
			s.pendingDialog = EdDialog::ExportCustom;
		}
	}
	PobUi::EndMenuPopup();
}

} // namespace

void DrawEditorHeader(EditorShell& s)
{
	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	const float H = PobUi::ControlH();
	const float gap = PobUi::D(12.0f);
	const float smH = std::floor(PobUi::D(28.0f));
	const ImVec2 hp = ImGui::GetCursorScreenPos();
	const float avail = ImGui::GetContentRegionAvail().x;
	ImDrawList* hdl = ImGui::GetWindowDrawList();
	auto at = [&](float x, float h) { ImGui::SetCursorScreenPos(ImVec2(x, hp.y + std::floor((H - h) * 0.5f))); };

	// file Select: the open file first-class, its unsaved count in the label
	std::vector<std::string> fileLbl, fileNote;
	int fileSel = -1;
	const int unsaved = s.UnsavedCount();
	for (int i = 0; i < (int)s.fileList.size(); i++) {
		const FilterListEntry& e = s.fileList[i];
		std::string l = StemOf(e.name);
		if (s.loaded && e.path == s.model.path) {
			fileSel = i;
			if (unsaved > 0) l += u8"  · " + std::to_string(unsaved) + u8" 處未儲存";
		}
		fileLbl.push_back(l);
		fileNote.push_back(e.inItemFilters ? "ItemFilters" : "");
	}
	std::string emptyLbl = s.fileList.empty() ? u8"找不到 .filter" : u8"選擇過濾器…";
	std::vector<const char*> fileP, fileN;
	for (size_t i = 0; i < fileLbl.size(); i++) { fileP.push_back(fileLbl[i].c_str()); fileN.push_back(fileNote[i].c_str()); }

	const char* pageLabels[3] = { u8"規則", u8"掉落預覽", u8"音效" };
	const char* title = u8"過濾器編輯器";
	const float iconPx = std::floor(PobUi::D(20.0f));
	const float headingW = wf.heading ? wf.heading->CalcTextSizeA(
		wf.headingPx > 0 ? wf.headingPx : wf.heading->FontSize, FLT_MAX, 0.0f, title).x
		: ImGui::CalcTextSize(title).x;
	float fileW = std::floor(PobUi::D(250.0f));
	if (!fileP.empty()) fileW = (std::max)(fileW, (std::min)(PobUi::SelectFitWidth(fileP.data(), (int)fileP.size()),
	                                                         std::floor(PobUi::D(420.0f))));

	const bool rulesPage = s.section == Section::FilterEdit;
	const char* batchLbl = s.batchMode ? u8"結束批量修改" : u8"批量修改";
	const float batchW = rulesPage && s.loaded ? PobUi::ButtonWidth(batchLbl, PobUi::BtnSize::Sm, s.batchMode ? nullptr : PobIcon::List) : 0.0f;
	const float moreW = PobUi::ButtonWidth("##more", PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, smH);
	const float saveW = PobUi::ButtonWidth(u8"儲存", PobUi::BtnSize::Sm, nullptr, PobUi::D(64.0f));

	float x = hp.x;
	if (wf.icons) {
		PobUi::IconAt(hdl, ImVec2(x, hp.y + std::floor((H - iconPx) * 0.5f)), PobIcon::Funnel, Tok::AccentText, iconPx);
		x += PobUi::IconWidth(PobIcon::Funnel, iconPx) + PobUi::D(8.0f);
	}
	at(x, ImGui::GetTextLineHeight());
	PobUi::Heading(title);
	x = ImGui::GetItemRectMax().x + PobUi::D(8.0f);
	(void)headingW;
	{
		const PobUi::WidgetFonts& f = PobUi::Fonts();
		const float bh = std::floor((f.small ? f.small->FontSize : ImGui::GetFontSize()) + PobUi::D(6.0f));
		at(x, bh);
		Badge("PoE1");
		x += BadgeWidth("PoE1") + gap;
	}

	// file
	at(x, H);
	{
		int sel = fileSel;
		const char* const* lp = fileP.empty() ? nullptr : fileP.data();
		if (fileP.empty() || sel < 0) {
			// nothing open: the Select shows a prompt (an extra, unselectable row)
			std::vector<const char*> withPrompt = fileP;
			std::vector<const char*> notes = fileN;
			withPrompt.insert(withPrompt.begin(), emptyLbl.c_str());
			notes.insert(notes.begin(), "");
			int ps = 0;
			if (PobUi::Select("##fefile", &ps, withPrompt.data(), notes.data(), (int)withPrompt.size(), fileW) && ps > 0)
				s.RequestOpen(s.fileList[ps - 1].path);
		} else if (PobUi::Select("##fefile", &sel, lp, fileN.data(), (int)fileP.size(), fileW) && sel != fileSel) {
			s.RequestOpen(s.fileList[sel].path);
		}
		if (ImGui::IsItemHovered() && s.loaded) PobUi::Tooltip(EdNarrow(s.model.path).c_str());
	}
	x += fileW + gap;

	// page
	at(x, H);
	{
		int page = (int)s.section;
		if (PobUi::Segmented("##fepage", &page, pageLabels, 3)) s.section = (Section)page;
	}
	x += PobUi::SegmentedWidth(pageLabels, 3) + gap;

	// right side, laid out from the right edge
	float rx = hp.x + avail - saveW - PobUi::D(8.0f) - moreW;
	if (batchW > 0.0f) rx -= batchW + PobUi::D(8.0f);
	if (rx < x) rx = x;
	if (batchW > 0.0f) {
		at(rx, smH);
		if (PobUi::Button(batchLbl, s.batchMode ? PobUi::BtnKind::Update : PobUi::BtnKind::Secondary,
		                  PobUi::BtnSize::Sm, s.batchMode ? nullptr : PobIcon::List)) {
			s.batchMode = !s.batchMode;
			if (!s.batchMode) s.batchSel.assign(s.batchSel.size(), 0);
		}
		if (ImGui::IsItemHovered())
			PobUi::Tooltip(s.batchMode ? u8"回到一次編輯一條規則"
			                           : u8"在左側勾選多條規則，一次改顏色、字級、音效或顯示 / 隱藏");
		rx += batchW + PobUi::D(8.0f);
	}
	at(rx, smH);
	if (PobUi::Button("##more", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, smH))
		s.uiOpenMore = true;
	const ImVec2 moreAnchor(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + PobUi::D(4.0f));
	if (ImGui::IsItemHovered())
		PobUi::Tooltip(u8"更多：開啟其他檔案、另存新檔、重新整理清單、重新載入、匯入 / 匯出自訂規則");
	rx += moreW + PobUi::D(8.0f);
	at(rx, smH);
	if (PobUi::Button(u8"儲存", PobUi::BtnKind::Primary, PobUi::BtnSize::Sm, nullptr, PobUi::D(64.0f),
	                  s.loaded && s.model.dirty))
		s.Save();

	// the row, then the header's bottom rule
	ImGui::SetCursorScreenPos(hp);
	ImGui::Dummy(ImVec2(avail, H));
	ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
	{
		const ImVec2 lp = ImGui::GetCursorScreenPos();
		hdl->AddLine(ImVec2(lp.x, lp.y), ImVec2(lp.x + avail, lp.y), Tok::Border, 1.0f);
		ImGui::Dummy(ImVec2(0, 1.0f));
	}

	DrawMoreMenu(s, s.uiOpenMore, moreAnchor);

	// "save first?" for switching files / reloading
	{
		if (s.askSaveFirst) { s.uiAskOpen = true; s.askSaveFirst = false; }
		const EdSaveAnswer a = DrawSaveFirstDialog(s, u8"##fesavefirst", &s.uiAskOpen, false);
		if (a != EdSaveAnswer::None) {
			const EdPendingAction act = s.pendingAction;
			const std::wstring path = s.pendingPath;
			s.pendingAction = EdPendingAction::None;
			s.pendingPath.clear();
			bool proceed = a == EdSaveAnswer::Discard;
			if (a == EdSaveAnswer::Save) proceed = s.Save();
			if (proceed && act != EdPendingAction::None && !path.empty()) s.OpenByPath(path, true);
		}
	}
}

// ---- banners -------------------------------------------------------------------

void DrawEditorBanners(EditorShell& s)
{
	// the open file changed on disk (someone else wrote it)
	if (s.loaded && !s.model.path.empty()) {
		s.watch.Poll(ImGui::GetTime());
		if (s.watch.changed()) {
			const int n = s.UnsavedCount();
			const std::string desc = n > 0
				? (u8"重新載入會放棄你在這裡還沒儲存的" + std::to_string(n) + u8"處變更。")
				: std::string(u8"重新載入就會看到新的內容。");
			ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
			const PobUi::BannerResult r = PobUi::Banner("##feextchange", PobUi::BannerTone::Warn, PobIcon::TriangleAlert,
				u8"這個檔案在 PobTools 外被改過", desc.c_str(), false, u8"重新載入", true);
			if (r == PobUi::BannerResult::Action) {
				// the banner already said what reloading throws away
				const std::wstring p = s.model.path;
				s.OpenByPath(p, true);
			} else if (r == PobUi::BannerResult::Close) {
				s.watch.Acknowledge();
			}
		}
	}
	if (!s.loadFailedPath.empty()) {
		std::wstring p = s.loadFailedPath;
		size_t slash = p.find_last_of(L"\\/");
		const std::string name = EdNarrow(slash == std::wstring::npos ? p : p.substr(slash + 1));
		const std::string title = u8"讀不到「" + name + u8"」";
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
		const PobUi::BannerResult r = PobUi::Banner("##feloadfail", PobUi::BannerTone::Bad, PobIcon::CircleX, title.c_str(),
			u8"檔案可能被移走或正被其他程式使用。關掉其他編輯器後再試一次。", false, u8"重試", true);
		if (r == PobUi::BannerResult::Action) {
			std::wstring retry = s.loadFailedPath;
			s.OpenByPath(retry, true);
		} else if (r == PobUi::BannerResult::Close) {
			s.loadFailedPath.clear();
		}
	}
}

// ---- footer ------------------------------------------------------------------

void DrawStatusBar(EditorShell& s)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	ImGui::GetWindowDrawList()->AddLine(p, ImVec2(p.x + w, p.y), Tok::BorderSubtle, 1.0f);
	ImGui::Dummy(ImVec2(0, PobUi::D(5.0f)));
	if (!s.loaded) {
		PobUi::Hint(u8"從遊戲的過濾器資料夾選一個 .filter 開始編輯");
		return;
	}
	PobUi::Hint(kSaveHint);
	int nShow = 0, nHide = 0, nCustom = 0;
	for (int i = 0; i < (int)s.model.blocks.size(); i++) {
		(s.model.blocks[i].hide ? nHide : nShow)++;
		if (i < (int)s.rows.size() && s.rows[i].custom) nCustom++;
	}
	const std::string counts = std::to_string(s.model.blocks.size()) + u8" 規則 · " + std::to_string(nShow) +
		u8" 顯示 · " + std::to_string(nHide) + u8" 隱藏 · 自訂 " + std::to_string(nCustom);
	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	ImFont* f = wf.small ? wf.small : ImGui::GetFont();
	const float cw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, counts.c_str()).x;
	ImGui::SameLine(0, 0);
	ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX() + PobUi::D(16.0f),
	                                ImGui::GetWindowContentRegionMax().x - cw));
	PobUi::Numeric(counts.c_str());
}

void SetBlockHide(EditorShell& s, FilterBlock& b, bool hide)
{
	if (b.hide == hide) return;
	FilterLine& hdr = s.model.lines[b.headerLineIdx];
	hdr.keyword = hide ? "Hide" : "Show";
	hdr.dirty = true;
	b.hide = hide;
	s.model.dirty = true;
}
