#include "editor_shell.h"
#include "editor_util.h"
#include "sound_library_service.h"
#include "audio_player.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_icons.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cwchar>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// 音效 — reference: POE-Filter-Audio-Manager; design: FilterSounds.dc.html.
// Top: the sound folder. Left card: naming rules (add / edit / delete, saved to
// Data\sound_rules.json). Right card: the folder's files with preview, per-file
// rename, replace-references and reference counts against the open filter.
// Batch rename runs through a dry-run plan dialog; conflicts must be resolved
// explicitly, and CustomAlertSound references update only after the user
// confirms (model marked dirty, never auto-saved).

namespace Tok = PobUi::Tok;

struct SoundsUiState {
	// rule editor
	bool ruleOpen = false;
	int editIdx = -1;
	NamingRule editBuf;
	// batch rename plan
	bool planOpen = false;
	std::vector<RenamePlanEntry> plan;
	bool planSync = true;
	// rename one
	bool renameOpen = false;
	std::wstring renameTarget;
	std::string renameNew;
	bool renameSync = true;
	// replace references
	bool replOpen = false;
	std::wstring replTarget;
	int replChoice = -1;
	// reference counts (name lower -> count), rebuilt on structure change
	std::unordered_map<std::wstring, int> counts;
	unsigned countsVer = ~0u;
	const void* countsModel = nullptr;
	std::string folderEdit;
	bool folderEditing = false;
	std::wstring testPlaying;   // test aid: show this file as playing
};

namespace {

SoundsUiState& SUI(EditorShell& s)
{
	if (!s.soundsUi) s.soundsUi = std::make_shared<SoundsUiState>();
	return *s.soundsUi;
}

ImFont* SmallFace() { const PobUi::WidgetFonts& wf = PobUi::Fonts(); return wf.small ? wf.small : ImGui::GetFont(); }

// name(lower) -> CustomAlertSound reference count in the open filter. Rebuilt
// when the doc's structure version changes (value-only edits to sound paths are
// rare and 重新整理 forces it).
std::unordered_map<std::wstring, int>& RefCounts(EditorShell& s, bool force = false)
{
	SoundsUiState& u = SUI(s);
	if (force || u.countsVer != s.doc.structureVersion() || u.countsModel != (const void*)&s.model) {
		u.counts.clear();
		if (s.loaded) {
			for (const FilterLine& ln : s.model.lines) {
				if (ln.kind != FilterLineKind::Action) continue;
				if (ln.keyword != "CustomAlertSound" && ln.keyword != "CustomAlertSoundOptional") continue;
				if (ln.values.empty()) continue;
				std::wstring v = EdWiden(ln.values[0].text);
				size_t slash = v.find_last_of(L"\\/");
				std::wstring base = (slash == std::wstring::npos) ? v : v.substr(slash + 1);
				for (wchar_t& c : base) c = towlower(c);
				u.counts[base]++;
			}
		}
		u.countsVer = s.doc.structureVersion();
		u.countsModel = &s.model;
	}
	return u.counts;
}

int RefCountOf(EditorShell& s, const std::wstring& name)
{
	std::wstring key = name;
	for (wchar_t& c : key) c = towlower(c);
	auto& m = RefCounts(s);
	auto it = m.find(key);
	return it == m.end() ? 0 : it->second;
}

void CenterHint(const char* t, float h)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((h - SmallFace()->FontSize) * 0.5f)));
	PobUi::Hint(t);
}

// A small count pill ("24 條" accent / "0" faint), right-aligned in `w`.
void RefPill(int n, float w, float h)
{
	ImFont* f = SmallFace();
	const std::string t = n > 0 ? std::to_string(n) + u8" 條" : std::string("0");
	const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, t.c_str());
	const float pw = ts.x + PobUi::D(16.0f), ph = std::floor(f->FontSize + PobUi::D(4.0f));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(w, h));
	const ImVec2 a(p.x + w - pw, p.y + std::floor((h - ph) * 0.5f));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(a, a + ImVec2(pw, ph), n > 0 ? Tok::AccentSoft : Tok::Surface2, ph * 0.5f);
	dl->AddText(f, f->FontSize, ImVec2(a.x + PobUi::D(8.0f), a.y + std::floor((ph - ts.y) * 0.5f)),
	            n > 0 ? Tok::AccentText : Tok::TextFaint, t.c_str());
}

// ---- dialogs ------------------------------------------------------------------

void DrawRuleEditor(EditorShell& s, SoundsUiState& u)
{
	const bool editing = u.editIdx >= 0 && u.editIdx < (int)s.sounds.rules().size();
	if (!PobUi::BeginDialog(u8"##fesndrule", &u.ruleOpen, editing ? u8"編輯命名規則" : u8"新增命名規則",
	                        u8"檔名含「比對文字」的音效檔，依規則改成「新檔名」。{n} 是流水號，{ext} 保留原副檔名。"))
		return;
	const float labW = std::floor(PobUi::D(84.0f));
	const float fw = std::floor(PobUi::D(300.0f));
	auto field = [&](const char* label, const char* id, std::string* v, const char* hint) {
		const ImVec2 p = ImGui::GetCursorScreenPos();
		CenterHint(label, PobUi::ControlH());
		ImGui::SetCursorScreenPos(ImVec2(p.x + labW, p.y));
		PobUi::PushControlFrame();
		ImGui::SetNextItemWidth(fw);
		ImGui::InputTextWithHint(id, hint, v);
		PobUi::PopControlFrame();
	};
	field(u8"規則名稱", "##rn", &u.editBuf.name, u8"例：高價通貨");
	field(u8"比對文字", "##rm", &u.editBuf.match, u8"留空＝只供手動套用");
	field(u8"新檔名", "##rr", &u.editBuf.rename, u8"例：top-{n}.{ext}");
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		CenterHint(u8"參與批次改名", PobUi::ControlH());
		ImGui::SetCursorScreenPos(ImVec2(p.x + labW, p.y + std::floor((PobUi::ControlH() - PobUi::D(22.0f)) * 0.5f)));
		PobUi::Switch("##ren", &u.editBuf.enabled);
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + PobUi::ControlH()));
		ImGui::Dummy(ImVec2(0, 0));
	}
	const bool ok = !u.editBuf.name.empty() || !u.editBuf.rename.empty();
	const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, editing ? u8"刪除規則" : nullptr, u8"儲存", ok);
	if (r == PobUi::DialogResult::Primary) {
		if (editing) s.sounds.rules()[u.editIdx] = u.editBuf;
		else s.sounds.rules().push_back(u.editBuf);
		std::string err;
		if (!s.sounds.SaveRules(&err)) s.Notify(u8"規則儲存失敗：" + err, true);
	} else if (r == PobUi::DialogResult::Danger && editing) {
		s.sounds.rules().erase(s.sounds.rules().begin() + u.editIdx);
		s.sounds.SaveRules();
		s.Notify(u8"已刪除命名規則");
	}
	PobUi::EndDialog();
}

void StatePill(const RenamePlanEntry& e)
{
	switch (e.state) {
		case RenamePlanEntry::State::Rename: PobUi::StatusPill(PobUi::Tone::Ok, u8"可改名"); break;
		case RenamePlanEntry::State::Conflict: PobUi::StatusPill(PobUi::Tone::Warn, u8"已有同名檔"); break;
		default: PobUi::StatusPill(PobUi::Tone::Idle, u8"不變"); break;
	}
}

void DrawPlanDialog(EditorShell& s, SoundsUiState& u)
{
	int nRef = 0, nUnresolved = 0, nAct = 0;
	for (const RenamePlanEntry& e : u.plan) {
		nRef += (int)e.refLines.size();
		if (e.state != RenamePlanEntry::State::Unchanged) nAct++;
		if (e.state == RenamePlanEntry::State::Conflict && e.resolution == RenamePlanEntry::Resolution::Unset) nUnresolved++;
	}
	const std::string title = u8"依規則改名 " + std::to_string(nAct) + u8" 個檔案？";
	if (!PobUi::BeginDialog(u8"##fesndplan", &u.planOpen, title.c_str(),
	                        u8"過濾器裡引用到這些檔案的地方會一起更新，改完記得儲存過濾器。", nullptr, 740.0f))
		return;
	ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
	const float rowH = PobUi::ControlH() + PobUi::D(8.0f);
	const float listH = (std::min)((float)u.plan.size(), 8.0f) * (rowH + ImGui::GetStyle().CellPadding.y * 2.0f) +
	                    ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f + PobUi::D(6.0f);
	if (ImGui::BeginTable("##plan", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
	                      ImVec2(0, listH))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn(u8"原檔名", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn(u8"新檔名", ImGuiTableColumnFlags_WidthStretch, 1.0f);
		ImGui::TableSetupColumn(u8"狀態", ImGuiTableColumnFlags_WidthFixed, PobUi::D(100.0f));
		ImGui::TableSetupColumn(u8"名稱衝突時", ImGuiTableColumnFlags_WidthFixed, PobUi::D(190.0f));
		ImGui::TableSetupColumn(u8"引用", ImGuiTableColumnFlags_WidthFixed, PobUi::D(44.0f));
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Tok::TextMuted));
		ImGui::TableHeadersRow();
		ImGui::PopStyleColor();
		for (int i = 0; i < (int)u.plan.size(); i++) {
			RenamePlanEntry& e = u.plan[i];
			ImGui::TableNextRow(0, rowH);
			ImGui::PushID(i);
			ImGui::TableSetColumnIndex(0);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f));
			ImGui::TextUnformatted(EdNarrow(e.oldName).c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f));
			ImGui::TextUnformatted(EdNarrow(e.newName).c_str());
			ImGui::TableSetColumnIndex(2);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f));
			StatePill(e);
			ImGui::TableSetColumnIndex(3);
			if (e.state == RenamePlanEntry::State::Conflict) {
				static const char* kRes[3] = { u8"跳過", u8"加後綴", u8"互換" };
				int sel = (int)e.resolution - 1;   // Unset -> -1, nothing highlighted
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((rowH - PobUi::ControlH()) * 0.5f));
				if (PobUi::Segmented("##res", &sel, kRes, 3)) e.resolution = (RenamePlanEntry::Resolution)(sel + 1);
			} else {
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f));
				PobUi::Hint(u8"—");
			}
			ImGui::TableSetColumnIndex(4);
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f));
			ImGui::Text("%d", (int)e.refLines.size());
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - PobUi::D(22.0f)) * 0.5f)));
		bool sync = u.planSync && s.loaded && nRef > 0;
		if (PobUi::Switch("##psync", &sync, s.loaded && nRef > 0)) u.planSync = sync;
		ImGui::SameLine(0, PobUi::D(8.0f));
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
		const std::string t = u8"一起更新過濾器的 " + std::to_string(nRef) + u8" 行引用（標成未儲存，不會自動存檔）";
		CenterHint(t.c_str(), PobUi::ControlH());
	}
	if (nUnresolved > 0) {
		const std::string t = u8"還有 " + std::to_string(nUnresolved) + u8" 個名稱衝突要選處理方式";
		PobUi::Hint(t.c_str(), 0.0f, Tok::Warning);
	}
	const std::string go = u8"改名 " + std::to_string(nAct) + u8" 個檔案";
	const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, nullptr, go.c_str(), nUnresolved == 0 && nAct > 0);
	if (r == PobUi::DialogResult::Primary) {
		SoundLibraryService::ApplyResult res =
			s.sounds.ApplyRenamePlan(u.plan, (u.planSync && s.loaded) ? &s.doc : nullptr);
		std::string msg = u8"改名 " + std::to_string(res.renamed) + u8" · 跳過 " + std::to_string(res.skipped) +
		                  u8" · 互換 " + std::to_string(res.swapped) + u8" · 更新引用 " + std::to_string(res.refsUpdated) + u8" 行";
		if (res.refsUpdated) msg += u8"（還沒儲存）";
		if (!res.err.empty()) msg += u8" ※ " + res.err;
		s.Notify(msg, !res.err.empty());
		RefCounts(s, true);
	}
	PobUi::EndDialog();
}

void DrawRenameOne(EditorShell& s, SoundsUiState& u)
{
	const std::string title = u8"改名「" + EdNarrow(u.renameTarget) + u8"」";
	if (!PobUi::BeginDialog(u8"##fesndren", &u.renameOpen, title.c_str(), u8"只改這一個檔案。")) return;
	PobUi::PushControlFrame();
	ImGui::SetNextItemWidth(std::floor(PobUi::D(392.0f)));
	ImGui::InputTextWithHint("##newname", u8"新檔名", &u.renameNew);
	PobUi::PopControlFrame();
	// quick-fill from a rule (expands {ext}; {n} becomes 1)
	if (!s.sounds.rules().empty()) {
		std::vector<std::string> lbl;
		std::vector<const char*> lp;
		lbl.push_back(u8"套用規則…");
		for (const NamingRule& r : s.sounds.rules()) lbl.push_back(r.name + "  (" + r.rename + ")");
		for (const std::string& t : lbl) lp.push_back(t.c_str());
		int sel = 0;
		if (PobUi::Select("##rulefill", &sel, lp.data(), nullptr, (int)lp.size(), std::floor(PobUi::D(392.0f))) && sel > 0) {
			const NamingRule& r = s.sounds.rules()[sel - 1];
			std::string t = r.rename;
			const std::string tn = EdNarrow(u.renameTarget);
			const size_t dot = tn.find_last_of('.');
			const std::string ext = dot == std::string::npos ? "" : tn.substr(dot + 1);
			size_t p;
			while ((p = t.find("{ext}")) != std::string::npos) t.replace(p, 5, ext);
			while ((p = t.find("{n}")) != std::string::npos) t.replace(p, 3, "1");
			u.renameNew = t;
		}
	}
	const int refs = RefCountOf(s, u.renameTarget);
	if (refs > 0) {
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - PobUi::D(22.0f)) * 0.5f)));
		PobUi::Switch("##rsync", &u.renameSync);
		ImGui::SameLine(0, PobUi::D(8.0f));
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, p.y));
		const std::string t = u8"一起更新過濾器的 " + std::to_string(refs) + u8" 行引用（標成未儲存）";
		CenterHint(t.c_str(), PobUi::ControlH());
	}
	const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, nullptr, u8"改名", !u.renameNew.empty(), true);
	if (r == PobUi::DialogResult::Primary) {
		RenamePlanEntry e = s.sounds.BuildSingleRename(u.renameTarget, EdWiden(u.renameNew), s.loaded ? &s.model : nullptr);
		if (e.state == RenamePlanEntry::State::Conflict) {
			s.Notify(u8"沒有改名：已經有叫「" + u.renameNew + u8"」的檔案", true);
		} else if (e.state == RenamePlanEntry::State::Rename) {
			std::vector<RenamePlanEntry> plan{ e };
			SoundLibraryService::ApplyResult res = s.sounds.ApplyRenamePlan(plan, (u.renameSync && s.loaded) ? &s.doc : nullptr);
			if (res.err.empty())
				s.Notify(u8"已改名為 " + u.renameNew +
				         (res.refsUpdated ? (u8"，更新 " + std::to_string(res.refsUpdated) + u8" 行引用（還沒儲存）") : std::string()));
			else
				s.Notify(u8"改名失敗：" + res.err, true);
			RefCounts(s, true);
		}
	}
	PobUi::EndDialog();
}

// 替換引用: rewrite every CustomAlertSound reference of the target to another
// existing file (the files themselves untouched; volume and path prefix kept;
// marked unsaved, not saved).
void DrawReplaceRefs(EditorShell& s, SoundsUiState& u)
{
	const std::vector<SoundFileInfo>& files = s.sounds.files();
	const int refs = RefCountOf(s, u.replTarget);
	const std::string title = u8"替換「" + EdNarrow(u.replTarget) + u8"」的引用";
	const std::string body = u8"過濾器裡的" + std::to_string(refs) +
		u8"行引用改成另一個音效檔。音效檔本身不動；每行原本的音量與路徑前綴保留。";
	if (!PobUi::BeginDialog(u8"##fesndrepl", &u.replOpen, title.c_str(), body.c_str())) return;
	std::vector<std::string> names;
	std::vector<int> idx;
	names.push_back(u8"選一個音效檔…");
	idx.push_back(-1);
	for (int i = 0; i < (int)files.size(); i++) {
		if (files[i].name == u.replTarget) continue;   // not itself
		names.push_back(EdNarrow(files[i].name));
		idx.push_back(i);
	}
	std::vector<const char*> np;
	for (const std::string& n : names) np.push_back(n.c_str());
	int sel = 0;
	for (int k = 0; k < (int)idx.size(); k++) if (idx[k] == u.replChoice && u.replChoice >= 0) sel = k;
	if (PobUi::Select("##replto", &sel, np.data(), nullptr, (int)np.size(), std::floor(PobUi::D(300.0f))))
		u.replChoice = idx[sel];
	ImGui::SameLine(0, PobUi::D(8.0f));
	const bool can = u.replChoice >= 0 && u.replChoice < (int)files.size();
	if (PobUi::Button(u8"試聽", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::Play, 0.0f, can))
		PlayAudioFileVol(s.sounds.folder() + L"\\" + files[u.replChoice].name, 100);
	const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, nullptr, u8"全部替換", can);
	if (r == PobUi::DialogResult::Primary && can) {
		StopAudio();
		const int n = ReplaceSoundRefs(&s.doc, u.replTarget, files[u.replChoice].name);
		RefCounts(s, true);
		s.Notify(u8"已把 " + std::to_string(n) + u8" 行引用換成 " + EdNarrow(files[u.replChoice].name) + u8"（還沒儲存）");
	}
	PobUi::EndDialog();
}

} // namespace

void DrawSoundsSection(EditorShell& s)
{
	if (!s.soundsInit) {
		s.sounds.Init(s.exeDir);
		s.soundsInit = true;
	}
	SoundsUiState& u = SUI(s);
	const float smH = std::floor(PobUi::D(28.0f));
	const float H0 = ImGui::GetContentRegionAvail().y;
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	const float W = ImGui::GetContentRegionAvail().x;

	// ---- folder row ----
	{
		const float padX = PobUi::D(16.0f), padY = PobUi::D(10.0f);
		const ImVec2 p = origin + ImVec2(padX, padY);
		ImGui::SetCursorScreenPos(p);
		CenterHint(u8"音效資料夾", PobUi::ControlH());
		ImFont* sf = SmallFace();
		const float labW = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, u8"音效資料夾").x + PobUi::D(10.0f);
		const float b1 = PobUi::ButtonWidth(u8"瀏覽…", PobUi::BtnSize::Sm);
		const float b2 = PobUi::ButtonWidth(u8"重新整理", PobUi::BtnSize::Sm);
		const float b3 = PobUi::ButtonWidth(u8"停止播放", PobUi::BtnSize::Sm, PobIcon::Square);
		const float g = PobUi::D(8.0f);
		const float inW = (std::max)(PobUi::D(160.0f), W - padX * 2.0f - labW - b1 - b2 - b3 - g * 4.0f);
		ImGui::SetCursorScreenPos(ImVec2(p.x + labW, p.y));
		if (!u.folderEditing) u.folderEdit = EdNarrow(s.sounds.folder());
		PobUi::PushControlFrame();
		ImGui::SetNextItemWidth(inW);
		if (ImGui::InputText("##sndfolder", &u.folderEdit, ImGuiInputTextFlags_EnterReturnsTrue))
			s.sounds.SetFolder(EdWiden(u.folderEdit));
		u.folderEditing = ImGui::IsItemActive();
		PobUi::PopControlFrame();
		const float by = p.y + std::floor((PobUi::ControlH() - smH) * 0.5f);
		float x = p.x + labW + inW + g;
		ImGui::SetCursorScreenPos(ImVec2(x, by));
		if (PobUi::Button(u8"瀏覽…", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) s.pendingDialog = EdDialog::SoundFolder;
		x += b1 + g;
		ImGui::SetCursorScreenPos(ImVec2(x, by));
		if (PobUi::Button(u8"重新整理", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) { s.sounds.Rescan(); RefCounts(s, true); }
		x += b2 + g;
		ImGui::SetCursorScreenPos(ImVec2(x, by));
		if (PobUi::Button(u8"停止播放", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Square)) StopAudio();
		const float lineY = p.y + PobUi::ControlH() + padY;
		ImGui::GetWindowDrawList()->AddLine(ImVec2(origin.x, lineY), ImVec2(origin.x + W, lineY), Tok::BorderSubtle, 1.0f);
		ImGui::SetCursorScreenPos(ImVec2(origin.x, lineY + 1.0f));
	}

	const float pad = PobUi::D(14.0f);
	const ImVec2 body = ImGui::GetCursorScreenPos() + ImVec2(PobUi::D(16.0f), pad);
	const float bodyH = origin.y + H0 - body.y - pad;
	const float bodyW = W - PobUi::D(32.0f);
	const float leftW = (std::min)(std::floor(PobUi::D(400.0f)), bodyW * 0.42f);
	const float rightW = bodyW - leftW - pad;
	bool wantRule = false, wantPlan = false, wantRename = false, wantRepl = false;

	// ---- left: naming rules ----
	ImGui::SetCursorScreenPos(body);
	ImGui::BeginChild("##sndrules", ImVec2(leftW, bodyH), false);
	PobUi::CardBegin("##rulescard", nullptr, u8"命名規則", nullptr, false, leftW, bodyH);
	if (PobUi::CardHeadButton(u8"新增", PobUi::BtnKind::Ghost, PobIcon::Plus)) {
		u.editIdx = -1;
		u.editBuf = NamingRule{};
		wantRule = true;
	}
	{
		const float x0 = PobUi::CardInnerX(), iw = PobUi::CardInnerWidth();
		const float cardX = ImGui::GetCursorScreenPos().x;
		const float rowH = PobUi::ControlH() + PobUi::D(14.0f);
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImFont* sf = SmallFace();
		for (int i = 0; i < (int)s.sounds.rules().size(); i++) {
			NamingRule& r = s.sounds.rules()[i];
			ImGui::PushID(i);
			const ImVec2 p(cardX, ImGui::GetCursorScreenPos().y);
			if (i) dl->AddLine(ImVec2(p.x + 1, p.y), ImVec2(p.x + leftW - 1, p.y), Tok::BorderSubtle, 1.0f);
			ImGui::SetCursorScreenPos(ImVec2(x0, p.y + std::floor((rowH - PobUi::D(22.0f)) * 0.5f)));
			bool en = r.enabled;
			if (PobUi::Switch("##en", &en)) { r.enabled = en; s.sounds.SaveRules(); }
			if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"開啟：批次改名時套用這條規則");
			const float tx = x0 + PobUi::SwitchWidth() + PobUi::D(10.0f);
			const float ew = PobUi::ButtonWidth(u8"編輯", PobUi::BtnSize::Sm);
			const float ty = p.y + std::floor((rowH - ImGui::GetTextLineHeight() - sf->FontSize) * 0.5f);
			const ImVec4 clip(tx, p.y, x0 + iw - ew - PobUi::D(8.0f), p.y + rowH);
			dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, ty), Tok::Text,
			            r.name.empty() ? u8"（沒有名稱）" : r.name.c_str(), nullptr, 0.0f, &clip);
			const std::string sub = (r.match.empty() ? std::string(u8"只供手動套用") : (u8"比對「" + r.match + u8"」")) +
			                        u8" · 改成 " + r.rename;
			dl->AddText(sf, sf->FontSize, ImVec2(tx, ty + ImGui::GetTextLineHeight()), Tok::TextMuted, sub.c_str(), nullptr, 0.0f, &clip);
			ImGui::SetCursorScreenPos(ImVec2(x0 + iw - ew, p.y + std::floor((rowH - smH) * 0.5f)));
			if (PobUi::Button(u8"編輯", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
				u.editIdx = i;
				u.editBuf = r;
				wantRule = true;
			}
			ImGui::SetCursorScreenPos(ImVec2(cardX, p.y + rowH));
			ImGui::Dummy(ImVec2(leftW, 0));
			ImGui::PopID();
		}
		if (s.sounds.rules().empty()) {
			ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + PobUi::D(12.0f)));
			PobUi::Hint(u8"還沒有規則。例：比對「divine」，改成 divine-{n}.{ext}", iw);
		}
		// the batch button at the card's bottom
		const float bh = PobUi::ControlH();
		const float by = body.y + bodyH - bh - PobUi::D(12.0f);
		dl->AddLine(ImVec2(cardX + 1, by - PobUi::D(10.0f)), ImVec2(cardX + leftW - 1, by - PobUi::D(10.0f)), Tok::BorderSubtle, 1.0f);
		ImGui::SetCursorScreenPos(ImVec2(x0, by));
		if (PobUi::Button(u8"依規則批次改名…", PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, nullptr, iw, !s.sounds.files().empty())) {
			u.plan = s.sounds.BuildRenamePlan(s.loaded ? &s.model : nullptr);
			u.planSync = true;
			if (u.plan.empty()) s.Notify(u8"沒有檔案符合任何已啟用的規則");
			else wantPlan = true;
		}
	}
	PobUi::CardEnd();
	ImGui::EndChild();

	// ---- right: files ----
	ImGui::SetCursorScreenPos(ImVec2(body.x + leftW + pad, body.y));
	ImGui::BeginChild("##sndfiles", ImVec2(rightW, bodyH), false);
	{
		int nRef = 0;
		for (const SoundFileInfo& fi : s.sounds.files()) if (RefCountOf(s, fi.name) > 0) nRef++;
		std::string note = std::to_string((int)s.sounds.files().size()) + u8" 個";
		if (s.loaded) note += u8" · 被過濾器引用 " + std::to_string(nRef) + u8" 個";
		else note += u8" · 沒開過濾器，看不到引用";
		PobUi::CardBegin("##filescard", nullptr, u8"音效檔", note.c_str(), false, rightW, bodyH);
		const float x0 = PobUi::CardInnerX(), iw = PobUi::CardInnerWidth();
		const float cardX = ImGui::GetCursorScreenPos().x;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		ImFont* sf = SmallFace();
		const float rowH = PobUi::ControlH() + PobUi::D(10.0f);
		const float refW = std::floor(PobUi::D(70.0f));
		const float rb = PobUi::ButtonWidth(u8"替換引用…", PobUi::BtnSize::Sm), nb = PobUi::ButtonWidth(u8"改名…", PobUi::BtnSize::Sm);
		const float actW = rb + nb + PobUi::D(6.0f);
		const float nameW = iw - refW - actW - PobUi::D(16.0f);
		// header
		{
			const ImVec2 p(x0, ImGui::GetCursorScreenPos().y);
			const float hh = sf->FontSize + PobUi::D(14.0f);
			const float ty = p.y + std::floor((hh - sf->FontSize) * 0.5f);
			dl->AddText(sf, sf->FontSize, ImVec2(p.x, ty), Tok::TextMuted, u8"檔名");
			const float rw = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, u8"引用").x;
			dl->AddText(sf, sf->FontSize, ImVec2(p.x + nameW + PobUi::D(8.0f) + refW - rw, ty), Tok::TextMuted, u8"引用");
			const float aw = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, u8"動作").x;
			dl->AddText(sf, sf->FontSize, ImVec2(x0 + iw - aw, ty), Tok::TextMuted, u8"動作");
			dl->AddLine(ImVec2(cardX + 1, p.y + hh), ImVec2(cardX + rightW - 1, p.y + hh), Tok::BorderSubtle, 1.0f);
			ImGui::SetCursorScreenPos(ImVec2(cardX, p.y + hh));
			ImGui::Dummy(ImVec2(rightW, 0));
		}
		const float listH = body.y + bodyH - ImGui::GetCursorScreenPos().y - PobUi::D(2.0f);
		ImGui::SetCursorScreenPos(ImVec2(cardX + 1.0f, ImGui::GetCursorScreenPos().y));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
		ImGui::BeginChild("##sndlist", ImVec2(rightW - 2.0f, (std::max)(PobUi::D(40.0f), listH)), false);
		ImDrawList* ldl = ImGui::GetWindowDrawList();
		const float lx0 = x0;
		if (s.sounds.files().empty()) {
			ImGui::SetCursorScreenPos(ImVec2(lx0, ImGui::GetCursorScreenPos().y + PobUi::D(12.0f)));
			PobUi::Hint(u8"這個資料夾沒有音效檔（wav / mp3 / ogg / flac / m4a / aac）。", iw);
		}
		for (int i = 0; i < (int)s.sounds.files().size(); i++) {
			const SoundFileInfo& fi = s.sounds.files()[i];
			ImGui::PushID(i);
			const ImVec2 p(lx0, ImGui::GetCursorScreenPos().y);
			if (i) ldl->AddLine(ImVec2(cardX + 1, p.y), ImVec2(cardX + rightW - 1, p.y), Tok::BorderSubtle, 1.0f);
			const std::wstring full = s.sounds.folder() + L"\\" + fi.name;
			const float by = p.y + std::floor((rowH - smH) * 0.5f);
			// playing: asked from MCI each frame (it ends on its own)
			const bool playing = (!u.testPlaying.empty() && u.testPlaying == fi.name) ||
			                     (AudioIsPlaying() && _wcsicmp(AudioCurrentPath().c_str(), full.c_str()) == 0);
			if (playing)
				ldl->AddRectFilled(ImVec2(cardX + 1.0f, p.y + 1.0f), ImVec2(cardX + rightW - 1.0f, p.y + rowH), Tok::AccentSoft);
			ImGui::SetCursorScreenPos(ImVec2(p.x, by));
			if (PobUi::Button("##play", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, playing ? PobIcon::Square : PobIcon::Play, smH)) {
				if (playing) StopAudio();
				else if (!s.testMode) PlayAudioFileVol(full, 100);
			}
			if (ImGui::IsItemHovered()) PobUi::Tooltip(playing ? u8"停止" : u8"試聽");
			const std::string nm = EdNarrow(fi.name);
			const float tx = p.x + smH + PobUi::D(8.0f);
			const ImVec4 clip(tx, p.y, p.x + nameW, p.y + rowH);
			const float nty = p.y + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f);
			ldl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, nty), Tok::Text, nm.c_str(), nullptr, 0.0f, &clip);
			float mx = tx + ImGui::CalcTextSize(nm.c_str()).x + PobUi::D(8.0f);
			if (playing) {
				ImFont* sf2 = SmallFace();
				ldl->AddText(sf2, sf2->FontSize, ImVec2(mx, p.y + std::floor((rowH - sf2->FontSize) * 0.5f)), Tok::TextMuted, u8"播放中");
				mx += sf2->CalcTextSizeA(sf2->FontSize, FLT_MAX, 0.0f, u8"播放中").x + PobUi::D(8.0f);
			}
			if (SoundNameHasDownloadSuffix(fi.name) && mx + PobUi::PillWidth(u8"檔名有下載後綴") < p.x + nameW) {
				ImGui::SetCursorScreenPos(ImVec2(mx, p.y + std::floor((rowH - PobUi::D(24.0f)) * 0.5f)));
				PobUi::StatusPill(PobUi::Tone::Warn, u8"檔名有下載後綴");
				if (ImGui::IsItemHovered())
					PobUi::Tooltip(u8"瀏覽器下載同名檔案時會自動在檔名加上「 (1)」。過濾器引用的是原本的檔名（例如 6maps.mp3），"
					               u8"所以這個檔案永遠不會被播放。按「改名…」把後綴去掉。");
			}
			const int refs = RefCountOf(s, fi.name);
			ImGui::SetCursorScreenPos(ImVec2(p.x + nameW + PobUi::D(8.0f), p.y));
			RefPill(refs, refW, rowH);
			ImGui::SetCursorScreenPos(ImVec2(lx0 + iw - actW, by));
			if (PobUi::Button(u8"替換引用…", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, refs > 0)) {
				u.replTarget = fi.name;
				u.replChoice = -1;
				wantRepl = true;
			}
			if (refs > 0 && ImGui::IsItemHovered())
				PobUi::Tooltip(u8"把過濾器裡引用這個檔案的規則，改成引用另一個音效檔（檔案不動）");
			ImGui::SameLine(0, PobUi::D(6.0f));
			if (PobUi::Button(u8"改名…", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
				u.renameTarget = fi.name;
				u.renameNew = nm;
				u.renameSync = true;
				wantRename = true;
			}
			ImGui::SetCursorScreenPos(ImVec2(cardX, p.y + rowH));
			ImGui::Dummy(ImVec2(rightW, 0));
			ImGui::PopID();
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
		PobUi::CardEnd();
	}
	ImGui::EndChild();
	ImGui::SetCursorScreenPos(origin + ImVec2(0, H0));

	// ---- dialogs, at the page's top level ----
	if (wantRule) u.ruleOpen = true;
	if (wantPlan) u.planOpen = true;
	if (wantRename) u.renameOpen = true;
	if (wantRepl) u.replOpen = true;
	DrawRuleEditor(s, u);
	DrawPlanDialog(s, u);
	DrawRenameOne(s, u);
	DrawReplaceRefs(s, u);
}

// Test aid: show a file as playing (the test files are not real audio).
void SoundsTestPlaying(EditorShell& s, const std::wstring& name)
{
	SUI(s).testPlaying = name;
}

// Test aid: open the batch-rename plan as if the button were pressed.
void SoundsTestOpenPlan(EditorShell& s)
{
	if (!s.soundsInit) { s.sounds.Init(s.exeDir); s.soundsInit = true; }
	SoundsUiState& u = SUI(s);
	u.plan = s.sounds.BuildRenamePlan(s.loaded ? &s.model : nullptr);
	u.planSync = true;
	if (!u.plan.empty()) u.planOpen = true;
}
