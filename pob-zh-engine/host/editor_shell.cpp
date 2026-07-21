#include "editor_shell.h"
#include "editor_util.h"
#include "filter_parser.h"
#include "custom_rules_io.h"
#include "ui_theme.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>

// ---- EditorShell methods -----------------------------------------------------

void EditorShell::OpenByPath(const std::wstring& path, bool force)
{
	if (model.dirty && !force) {
		status = u8"※ 有未儲存變更，請先「儲存」或「重新載入」捨棄後再切換。";
		return;
	}
	bool ok = false;
	FilterFile f = LoadFilter(path, &ok);
	if (!ok) { status = u8"讀取失敗：" + EdNarrow(path); return; }
	model = std::move(f);
	loaded = true;
	selectedBlock = model.blocks.empty() ? -1 : 0;
	doc.Attach(&model);              // bumps structureVersion -> row caches rebuild
	selAnchor = BlockAnchor{};
	batchMode = false;
	batchSel.clear();
	status = std::to_string(model.blocks.size()) + u8" 個規則區塊 · " + model.name;
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
}

void SaveEditorSettings(EditorShell& s)
{
	std::wstring ini = s.exeDir + L"pob-zh.ini";
	WritePrivateProfileStringW(L"PobTools", L"League", EdWiden(s.league).c_str(), ini.c_str());
	WritePrivateProfileStringW(L"PobTools", L"EconomyEnabled", s.economyEnabled ? L"1" : L"0", ini.c_str());
}

// ---- top toolbar ------------------------------------------------------------

void DrawTopToolbar(EditorShell& s)
{
	const float scale = s.scale;

	ImGui::AlignTextToFramePadding();
	ImGui::TextColored(PobUi::Accent(), u8"過濾器工作台");
	ImGui::SameLine(0, 18 * scale);
	ImGui::SetNextItemWidth(300 * scale);
	{
		std::string preview = s.loaded ? s.model.name : u8"選擇過濾器…";
		if (ImGui::BeginCombo("##file", preview.c_str())) {
			if (s.fileList.empty())
				ImGui::TextDisabled(u8"在 Documents\\My Games\\Path of Exile\\ 找不到 .filter");
			for (const FilterListEntry& e : s.fileList) {
				std::string label = e.name + (e.inItemFilters ? u8"  （ItemFilters）" : "");
				bool sel = s.loaded && e.path == s.model.path;
				if (ImGui::Selectable(label.c_str(), sel)) s.OpenByPath(e.path, false);
			}
			ImGui::EndCombo();
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(u8"開啟檔案…")) {
		std::wstring p = EdFilterDialog(s.initialDir, false);
		if (!p.empty()) s.OpenByPath(p, false);
	}
	ImGui::SameLine();
	if (ImGui::Button(u8"重新整理列表")) { s.fileList = ListFilters(); }

	ImGui::SameLine(0, 20 * scale);
	ImGui::BeginDisabled(!s.loaded || !s.model.dirty);
	PobUi::PushPrimaryButton();
	if (ImGui::Button(s.model.dirty ? u8"儲存 *" : u8"儲存")) {
		std::string err;
		s.status = SaveFilter(s.model, &err)
			? (u8"已儲存：" + s.model.name + u8"　※ 遊戲內要到 選項→遊戲→UI 重新選擇過濾器才會生效")
			: (u8"儲存失敗：" + err);
	}
	PobUi::PopButtonStyle();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!s.loaded);
	if (ImGui::Button(u8"另存為…")) {
		std::wstring p = EdFilterDialog(s.initialDir, true);
		if (!p.empty()) {
			s.model.path = p;
			size_t slash = p.find_last_of(L"\\/");
			s.model.name = EdNarrow(slash == std::wstring::npos ? p : p.substr(slash + 1));
			std::string err;
			s.status = SaveFilter(s.model, &err)
				? (u8"已儲存：" + s.model.name + u8"　※ 遊戲內要到 選項→遊戲→UI 重新選擇過濾器才會生效")
				: (u8"儲存失敗：" + err);
		}
	}
	ImGui::SameLine();
	if (ImGui::Button(u8"重新載入")) { if (!s.model.path.empty()) s.OpenByPath(s.model.path, true); }

	// --- 批量修改 / 自訂規則匯出入 ---
	ImGui::Spacing();
	ImGui::TextDisabled(u8"規則工具");
	ImGui::SameLine(0, 14 * scale);
	bool batchOn = s.batchMode;
	if (batchOn) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.39f, 0.40f, 0.95f, 0.70f));
	if (ImGui::Button(u8"批量修改")) s.batchMode = !s.batchMode;
	if (batchOn) ImGui::PopStyleColor();
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"開啟後可在左側清單勾選多條規則，一次套用樣式");

	ImGui::SameLine();
	if (ImGui::Button(u8"保存自定義")) {
		std::vector<int> sel;
		if (s.batchMode)
			for (int i = 0; i < (int)s.batchSel.size(); i++) { if (s.batchSel[i]) sel.push_back(i); }
		if (sel.empty() && s.selectedBlock >= 0) sel.push_back(s.selectedBlock);
		if (sel.empty()) {
			s.status = u8"請先選取（或批量勾選）要保存的規則。";
		} else {
			std::wstring p = EdFilterDialog(s.initialDir, true);
			if (!p.empty()) {
				std::string frag = ExportCustomRules(s.model, sel, u8"自訂規則");
				std::string err;
				s.status = SaveCustomRulesFile(p, frag, &err)
					? (u8"已保存 " + std::to_string((int)sel.size()) + u8" 條自訂規則")
					: (u8"保存失敗：" + err);
			}
		}
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"把選取的規則匯出成獨立檔，可在其他過濾器導入");

	ImGui::SameLine();
	if (ImGui::Button(u8"導入自定義")) {
		std::wstring p = EdFilterDialog(s.initialDir, false);
		if (!p.empty()) {
			std::vector<unsigned char> data = EdReadFile(p);
			std::string frag(data.begin(), data.end());
			std::string err;
			int n = ImportCustomRules(s.doc, frag, &err);
			s.status = (n < 0) ? (u8"導入失敗：" + err)
			         : (u8"已導入 " + std::to_string(n) + u8" 條規則到自訂區（未儲存）");
		}
	}
	if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"把匯出的自訂規則檔加入此過濾器最上方的自訂區");
	ImGui::EndDisabled();
}

// ---- left navigation --------------------------------------------------------

void DrawLeftNav(EditorShell& s)
{
	struct NavItem { Section sec; const char* label; };
	static const NavItem items[] = {
		{ Section::FilterEdit,  u8"過濾編輯" },
		{ Section::DropPreview, u8"掉落預覽" },
		{ Section::Sounds,      u8"音效管理" },
	};
	for (const NavItem& it : items) {
		if (ImGui::Selectable(it.label, s.section == it.sec, 0, ImVec2(0, 30 * s.scale)))
			s.section = it.sec;
	}
}

// ---- bottom status bar ------------------------------------------------------

void DrawStatusBar(EditorShell& s)
{
	ImGui::Separator();
	if (!s.loaded) {
		ImGui::TextDisabled(u8"請從上方選擇或開啟一個 POE1 .filter 檔（位於 Documents\\My Games\\Path of Exile\\）。");
		return;
	}
	int nShow = 0, nHide = 0;
	for (const FilterBlock& b : s.model.blocks) (b.hide ? nHide : nShow)++;
	const std::string& msg = s.status;
	ImGui::TextDisabled(u8"%zu 規則  ·  %d 顯示  ·  %d 隱藏%s%s",
		s.model.blocks.size(), nShow, nHide,
		msg.empty() ? "" : u8"  ·  ", msg.c_str());
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
