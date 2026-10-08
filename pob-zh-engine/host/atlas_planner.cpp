// must precede every imgui.h include (atlas_view.h pulls it in)
#define IMGUI_DEFINE_MATH_OPERATORS

#include "atlas_planner.h"
#include "tool_panel.h"
#include "tool_window.h"
#include "atlas_diff.h"
#include "atlas_i18n.h"
#include "atlas_import.h"
#include "atlas_optimize.h"
#include "atlas_astrolabes.h"
#include "atlas_maps.h"
#include "atlas_mechanics.h"
#include "atlas_persist.h"
#include "atlas_scarabs.h"
#include "atlas_stat_agg.h"
#include "atlas_tree_data.h"
#include "atlas_update.h"
#include "error_log.h"
#include "atlas_version_index.h"
#include "atlas_view.h"
#include "editor_util.h" // EdReadFile
#include "icon_manager.h" // scarab icons (poecdn + on-disk cache)
#include "launcher_config.h" // ResolveConfiguredFontPath
#include "ui_theme.h"
#include "ui_widgets.h"   // design-system widgets (ToolHeader, Dialog, Tabs, Slot...)
#include "ui_icons.h"
#include "atlas_cost.h"           // 收益 tab: one map's cost from the project's device
#include "warehouse_format.h"     // the revenue panel's money formatting, shared
#include "warehouse_price_feed.h" // poe.ninja prices outside a snapshot job
#include "warehouse_state.h"      // WarehouseSavedLeague: prices follow the panel
#include "warehouse_tool.h"       // CreateWarehousePanel: the revenue panel, embedded
#include "filter_i18n.h"          // item names stored with a bound revenue record

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <imgui_stdlib.h> // ImGui::InputText(std::string*)

#include <algorithm>
#include <cfloat>
#include <ctime>
#include <functional>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Tok = PobUi::Tok;

// node kind by panel rank (keystone / wormhole / notable / small); the colour is
// PobUi::TreeKindColor, the same one the canvas tooltip uses
static const PobUi::TreeKind kRankKind[4] = {
	PobUi::TreeKind::Keystone, PobUi::TreeKind::Wormhole, PobUi::TreeKind::Notable, PobUi::TreeKind::Small,
};
static const char* kGroupName[4] = { u8"核心天賦", u8"蟲洞", u8"大點", u8"小點" };

// Open-file dialog scoped to data.json (new-season atlastree-export import).
static std::wstring OpenDataJsonDialog(HWND owner)
{
	wchar_t buf[MAX_PATH] = L"";
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = owner;
	ofn.lpstrFilter = L"atlastree-export data.json\0data.json;*.json\0所有檔案 (*.*)\0*.*\0\0";
	ofn.lpstrFile = buf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

static const wchar_t* kBuildJsonFilter = L"輿圖策略專案 (*.json)\0*.json\0所有檔案 (*.*)\0*.*\0\0";

// Save-file dialog for exporting one build project; buf pre-filled with the
// project name (filesystem-hostile characters stripped).
static std::wstring SaveBuildJsonDialog(HWND owner, const std::string& suggestedName)
{
	std::wstring name;
	{
		int n = MultiByteToWideChar(CP_UTF8, 0, suggestedName.c_str(), (int)suggestedName.size(), nullptr, 0);
		name.resize(n);
		if (n) MultiByteToWideChar(CP_UTF8, 0, suggestedName.c_str(), (int)suggestedName.size(), &name[0], n);
	}
	std::wstring clean;
	for (wchar_t c : name)
		if (!wcschr(L"\\/:*?\"<>|", c)) clean.push_back(c);

	wchar_t buf[MAX_PATH] = L"";
	wcsncpy_s(buf, clean.c_str(), _TRUNCATE);
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = owner;
	ofn.lpstrFilter = kBuildJsonFilter;
	ofn.lpstrDefExt = L"json";
	ofn.lpstrFile = buf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	return GetSaveFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

static std::wstring OpenBuildJsonDialog(HWND owner)
{
	wchar_t buf[MAX_PATH] = L"";
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = owner;
	ofn.lpstrFilter = kBuildJsonFilter;
	ofn.lpstrFile = buf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

// tiny file helpers for export/import payloads (same conventions as siblings)
static bool PlannerReadFile(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 26)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

static bool PlannerWriteFile(const std::wstring& path, const std::string& content)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = content.empty() ||
		(WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr) && written == content.size());
	CloseHandle(h);
	return ok;
}

// The tool's content, independent of where it is drawn: RunToolWindow gives it a
// window of its own, the launcher's tab body draws it as a tab. See tool_panel.h.
//
// Same mechanical move as the other three tools -- every local of
// ShowAtlasPlanner is a member and every captured closure a member function --
// so the bodies below moved across unchanged. The closures are the reason this
// one waited: as locals they captured the enclosing function's frame, and a
// closure that outlives that frame compiles perfectly and is undefined at run
// time. As members there is no frame to outlive.

namespace {
// Single-level undo. Plain clicking never moves anything the user placed, but
// planning mode and the compress button both re-route wiring, which is the
// one thing a user cannot predict, so there has to be a way back. Snapshots
// are taken before a change lands, not after.
struct AtlasUndo {
	bool valid = false;
	std::vector<int> alloc, targets, blocked;
};
struct PanelNode { int idx; std::string searchKey; };

// Which blocking dialog Frame() asked for. A Win32 common dialog runs its own
// modal message loop, so opening one mid-frame stops the host's loop -- and in
// the launcher that loop is what keeps the docked POB windows glued to the
// client area. So Frame() only records the intent and RunDeferred() opens it.
enum class ApDialog { None, ImportSeasonData, ExportBuild, ImportBuild };

} // namespace

class AtlasPlannerPanel : public IToolPanel {
public:
	bool Init(const ToolPanelHost& h) override
	{
		host_ = &h;
		exeDir = h.exeDir;
		scale = h.scale;
		buildFile.Load(exeDir); // legacy single-build files migrate transparently
		if (!scarabDb.Load(exeDir, &scarabErr))
			PobLog::Error("data", "scarabs_poe1.json: " + scarabErr);
		if (!astroDb.Load(exeDir, &astroErr))
			PobLog::Error("data", "astrolabes_poe1.json: " + astroErr);
		if (!mapDb.Load(exeDir, &mapErr))
			PobLog::Error("data", "atlas_maps_poe1.json: " + mapErr);
		for (AtlasBuildEntry& b : buildFile.builds) {
			b.scarabs = scarabDb.Sanitize(b.scarabs, nullptr);
			b.astrolabes = astroDb.Sanitize(b.astrolabes, nullptr);
			b.mapId = mapDb.SanitizeOne(b.mapId);
		}
		icons.Init(exeDir);
		uiState.Load(exeDir);
		verIndex.Load(exeDir);
		if (verIndex.NeedsSave() && !verIndex.Save(exeDir))
			PobLog::Error("save", "atlas_index.json 存檔失敗（賽季清單沒有更新）");
		viewTag = (!uiState.season.empty() && verIndex.Has(uiState.season))
			? uiState.season : verIndex.Active();
		updater.Init(exeDir);
		updater.RequestCheck(false);         // throttled to once per day
		loadSeason(viewTag);
		showZh = zhLoaded;                   // default Chinese when a mapping exists
		if (startupDropped > 0)
			noticeMsg_ = u8"這個配置存於舊版輿圖樹，" + std::to_string(startupDropped) +
			             u8" 個已不存在的節點已自動移除。";
		// Test aid (POBTOOLS_ATLAS_PAGE, with POBTOOLS_TOOL_SHOT): open on a page.
		{
			wchar_t pg[32] = L"";
			const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_ATLAS_PAGE", pg, 32);
			const std::wstring page = (n > 0 && n < 32) ? std::wstring(pg) : std::wstring();
			if (page == L"profit") { profitMode_ = true; profitPage_ = 0; }
			else if (page == L"help") { profitMode_ = true; profitPage_ = 1; }
			else if (page == L"settings") { profitMode_ = true; profitPage_ = 2; }
			else if (page == L"compare" && ready && verIndex.Versions().size() >= 2) {
				compareMode = true;
				refreshCompare();
			}
			// the menu and the dialogs, for a screenshot of each (nothing is written:
			// a dialog only acts on a click, and the sandbox never saves)
			else if (page == L"menu") openMoreMenu_ = true;
			else if (page == L"delete") askDelete_ = true;
			else if (page == L"reset") askReset_ = true;
			else if (page == L"new") { nameBuf = u8"新專案"; askNew_ = true; }
			else if (page == L"planexit" && ready) { enterPlanning(); planningAskExit = true; }
		}
		return true;   // missing data is a screen with an import button, not a failure
	}

	void Frame() override
	{
		// Re-read every frame and never cached: the launcher rebuilds its glyph
		// atlas when the user changes font, and every ImFont* from before that is
		// dangling afterwards.
		fontBig = host_->big;
		cjkOk = host_->cjkOk;

		// updater results land on the worker thread; apply them here (GL thread)
		AtlasUpdater::Status ust = updater.Poll();
		if (ust.reloadPending) {
			hotReload(ust.message);
			updater.AckReload();
			ust = updater.Poll();
		} else if (ust.zhRefreshed) {
			bool zhWas = zhLoaded;
			zhLoaded = i18n.Load(exeDir);
			if (!zhLoaded) showZh = false;
			else if (!zhWas) showZh = true;
			importMsg = ust.message;
			importFailed = false;
			panelDirty = true; // cached dispZh strings must pick up the new mapping
			updater.AckReload();
			ust = updater.Poll();
		}

		icons.Pump(); // GL thread: upload any scarab icons the worker finished

		// Results: a success is a toast, a failure a banner that stays until it is
		// closed. (importMsg is still where every action reports; it is turned
		// into one or the other here, once.)
		auto flushMsg = [&]() {
			if (importMsg.empty()) return;
			if (importFailed) errBanner_ = importMsg;
			else PobUi::ShowToast(importMsg.c_str(), PobUi::Tone::Ok);
			importMsg.clear();
		};
		flushMsg();

		ImGuiIO& io = ImGui::GetIO();
		// The width this panel has, which used to be io.DisplaySize.x -- the whole
		// viewport. True when the planner owned the window; short by a tab strip
		// and a window border when it is a tab.
		const float dispW = ImGui::GetContentRegionAvail().x;

		if (!ready) {
			const std::string why = loadErr + u8"\n請確認 Data\\atlas_versions\\ 底下有這個賽季的資料，或直接匯入新資料。";
			if (PobUi::Banner("##loaderr", PobUi::BannerTone::Bad, PobIcon::CircleX, u8"輿圖資料載入失敗", why.c_str(),
			                  false, u8"匯入賽季資料", false) == PobUi::BannerResult::Action)
				importSeason();
			ImGui::Dummy(ImVec2(0, PobUi::D(8.0f)));
			// the auto updater doubles as the recovery path when no data exists
			if (ust.phase == AtlasUpdatePhase::UpdateAvailable) {
				if (PobUi::Button((u8"自動下載 " + ust.latestTag).c_str(), PobUi::BtnKind::Update, PobUi::BtnSize::Md,
				                  PobIcon::Download))
					updater.StartUpdate();
			} else if (ust.phase == AtlasUpdatePhase::Downloading || ust.phase == AtlasUpdatePhase::Importing ||
			           ust.phase == AtlasUpdatePhase::Checking) {
				PobUi::Numeric(ust.message.c_str());
				if (ust.phase == AtlasUpdatePhase::Downloading && ust.filesTotal > 0)
					PobUi::ProgressBar((float)ust.filesDone / (float)ust.filesTotal, PobUi::D(200.0f));
			} else if (ust.phase == AtlasUpdatePhase::Error) {
				if (PobUi::Banner("##upderr0", PobUi::BannerTone::Bad, PobIcon::CircleX, u8"更新失敗", ust.message.c_str(),
				                  false, u8"重試", false) == PobUi::BannerResult::Action)
					updater.StartUpdate();
			}
			if (!errBanner_.empty()) {
				ImGui::Dummy(ImVec2(0, PobUi::D(8.0f)));
				if (PobUi::Banner("##apperr0", PobUi::BannerTone::Bad, PobIcon::CircleX, u8"沒有完成", errBanner_.c_str(),
				                  false, nullptr, true) == PobUi::BannerResult::Close)
					errBanner_.clear();
			}
		} else {
			// --- project row: multi-build switching, CRUD, export/import ---
			auto switchTo = [&](int i) {
				saveActive();            // capture the outgoing project first
				buildFile.active = i;
				tree.ApplyAllocIds(buildFile.Active().alloc);
				tree.ApplyTargetIds(buildFile.Active().targets);
				tree.ApplyBlockedIds(buildFile.Active().blocked);
				undo.valid = false;      // undo does not cross projects
				saveActive();            // persist active index + pruned mapping
				panelDirty = true;
			};
			// importEntry is a member: the .json import path runs from
			// RunDeferred(), after this frame is over.

			// --- tool header (ToolHeader): one row ---
			// icon + name | project | mode | season | (old-season pill) ...
			// ... update | compress | share code | more. When the row is too narrow
			// the two right-hand actions move into the more menu; nothing wraps.
			const bool updaterBusy = ust.phase == AtlasUpdatePhase::Downloading ||
			                         ust.phase == AtlasUpdatePhase::Importing;
			const bool canCompress = !planningMode && !compareMode && onCanonicalSeason() &&
			                         tree.UsedPoints() > 0;
			const bool canCompare = verIndex.Versions().size() >= 2 && !verIndex.CompareBase().empty();
			const bool lastBuild = buildFile.builds.size() <= 1;
			bool headerCollapsed = false;
			{
				const PobUi::WidgetFonts& wf = PobUi::Fonts();
				const float H = PobUi::ControlH();
				const float gap = PobUi::D(12.0f);
				const ImVec2 hp = ImGui::GetCursorScreenPos();
				const float avail = ImGui::GetContentRegionAvail().x;
				ImDrawList* hdl = ImGui::GetWindowDrawList();

				// What goes in each slot, measured before anything is drawn.
				std::vector<const char*> projNames;
				for (const AtlasBuildEntry& e : buildFile.builds) projNames.push_back(e.name.c_str());
				std::vector<std::string> seasonLabels;
				std::vector<std::string> seasonTags = verIndex.TagsNewestFirst();
				int seasonSel = -1;
				for (size_t i = 0; i < seasonTags.size(); i++) {
					std::string l = seasonTags[i];
					if (seasonTags[i] == verIndex.Active()) l += u8"（目前）";
					seasonLabels.push_back(l);
					if (seasonTags[i] == viewTag) seasonSel = (int)i;
				}
				std::vector<const char*> seasonPtrs;
				for (const std::string& l : seasonLabels) seasonPtrs.push_back(l.c_str());
				const char* modeLabels[3] = { u8"配點", u8"規劃", u8"版本比較" };
				const char* oldSeason = u8"舊賽季・唯讀";

				const float iconPx = std::floor(PobUi::D(20.0f));
				const float headingW = wf.heading ? wf.heading->CalcTextSizeA(
					wf.headingPx > 0 ? wf.headingPx : wf.heading->FontSize, FLT_MAX, 0.0f, u8"輿圖策略").x
					: ImGui::CalcTextSize(u8"輿圖策略").x;
				const float projW = std::floor(PobUi::D(200.0f));
				const float seasonW = (std::max)(std::floor(PobUi::D(150.0f)),
				                                 PobUi::SelectFitWidth(seasonPtrs.data(), (int)seasonPtrs.size()));
				float leftW = PobUi::IconWidth(PobIcon::Crosshair, iconPx) + PobUi::D(8.0f) + headingW + gap +
				              projW + gap + PobUi::SegmentedWidth(modeLabels, 3);
				if (!viewTag.empty()) leftW += gap + seasonW;
				if (!onCanonicalSeason()) leftW += gap + PobUi::PillWidth(oldSeason);

				const char* compressLbl = u8"壓縮到最少點";
				const char* shareLbl = u8"匯出分享碼";
				const float moreW = PobUi::ButtonWidth("##more", PobUi::BtnSize::Sm, PobIcon::MoreHorizontal,
				                                       std::floor(PobUi::D(28.0f)));
				const float actW = PobUi::ButtonWidth(compressLbl, PobUi::BtnSize::Sm, PobIcon::Minimize) + PobUi::D(8.0f) +
				                   PobUi::ButtonWidth(shareLbl, PobUi::BtnSize::Sm, PobIcon::Share) + PobUi::D(8.0f);
				std::string updLbl;
				float updW = 0.0f;
				if (ust.phase == AtlasUpdatePhase::UpdateAvailable) {
					updLbl = u8"更新到 " + ust.latestTag;
					updW = PobUi::ButtonWidth(updLbl.c_str(), PobUi::BtnSize::Sm, PobIcon::Download) + PobUi::D(8.0f);
				} else if (updaterBusy || ust.phase == AtlasUpdatePhase::Checking) {
					updW = PobUi::D(120.0f) + PobUi::D(8.0f);
				}
				headerCollapsed = leftW + gap + updW + actW + moreW > avail;
				const float rightW = updW + (headerCollapsed ? 0.0f : actW) + moreW;

				auto at = [&](float x, float h) { ImGui::SetCursorScreenPos(ImVec2(x, hp.y + std::floor((H - h) * 0.5f))); };
				float x = hp.x;
				// icon + name
				PobUi::IconAt(hdl, ImVec2(x, hp.y + std::floor((H - iconPx) * 0.5f)), PobIcon::Crosshair, Tok::AccentText, iconPx);
				x += PobUi::IconWidth(PobIcon::Crosshair, iconPx) + (wf.icons ? PobUi::D(8.0f) : 0.0f);
				at(x, ImGui::GetTextLineHeight());
				PobUi::Heading(u8"輿圖策略");
				x = ImGui::GetItemRectMax().x + gap;
				// project
				at(x, H);
				int projSel = buildFile.active;
				if (PobUi::Select("##buildsel", &projSel, projNames.data(), nullptr, (int)projNames.size(), projW) &&
				    projSel != buildFile.active)
					switchTo(projSel);
				x += projW + gap;
				// mode: 配點 / 規劃 / 版本比較. Comparing and planning exclude each
				// other (comparing is a read-only preview of another tree), so the
				// option that cannot be entered right now is disabled and says why.
				{
					at(x, H);
					const int modeNow = compareMode ? 2 : (planningMode ? 1 : 0);
					int mode = modeNow;
					const bool modeOn[3] = { true, !compareMode, !planningMode && canCompare };
					const char* modeTips[3] = {
						planningMode ? u8"結束規劃，並選擇是否保留這次的結果" : nullptr,
						compareMode ? u8"版本比較中不能規劃：先回到「配點」"
						            : u8"沙盒：從空白開始快速標記想要／不要的節點；期間不寫入存檔，結束時再決定要不要保留",
						planningMode ? u8"規劃中不能比較：先結束規劃"
						             : (canCompare ? u8"比較兩個已安裝的版本：節點增刪與逐詞條數值變更"
						                           : u8"需要兩個版本的資料才能比較"),
					};
					if (PobUi::SegmentedEx("##mode", &mode, modeLabels, 3, modeOn, modeTips) && mode != modeNow) {
						if (modeNow == 1) {
							planningAskExit = true;   // ask before deciding; the mode stays until answered
						} else if (modeNow == 2) {
							compareMode = false;
							view.ClearDiffOverlay();
						}
						if (modeNow == 0 && mode == 1) {
							enterPlanning();
							panelDirty = true;
						} else if (modeNow == 0 && mode == 2) {
							compareMode = true;
							refreshCompare();
						}
					}
					x += PobUi::SegmentedWidth(modeLabels, 3) + gap;
				}
				// season: which league's atlas tree is drawn on the canvas
				if (!viewTag.empty()) {
					at(x, H);
					int s = seasonSel;
					if (PobUi::Select("##seasonsel", &s, seasonPtrs.data(), nullptr, (int)seasonPtrs.size(), seasonW,
					                  seasonPtrs.size() >= 2) &&
					    s >= 0 && s < (int)seasonTags.size() && seasonTags[s] != viewTag) {
						const std::string t = seasonTags[s];
						saveActive();                 // capture edits on the outgoing canonical season
						loadSeason(t);
						uiState.season = t;
						uiState.Save(exeDir);
						panelDirty = true;
						if (compareMode) refreshCompare(); // rebuild overlay/index for the new tree
					}
					if (ImGui::IsItemHovered())
						PobUi::Tooltip(u8"切換畫布顯示的賽季輿圖樹（配點以節點 ID 跨季共用）");
					x += seasonW + gap;
				}
				if (!onCanonicalSeason()) {
					at(x, std::floor(PobUi::D(24.0f)));
					PobUi::StatusPill(PobUi::Tone::Warn, oldSeason);
					if (ImGui::IsItemHovered())
						PobUi::Tooltip(u8"舊賽季只能檢視：在這裡點的配點不會存進專案。切回目前賽季才能編輯。");
				}

				// right-hand actions, laid out from the right edge
				float rx = hp.x + avail - rightW;
				if (rx < x) rx = x;   // narrower than even the collapsed row: let it run on
				const float smH = std::floor(PobUi::D(28.0f));
				if (ust.phase == AtlasUpdatePhase::UpdateAvailable) {
					at(rx, smH);
					if (PobUi::Button(updLbl.c_str(), PobUi::BtnKind::Update, PobUi::BtnSize::Sm, PobIcon::Download))
						updater.StartUpdate();
					if (ImGui::IsItemHovered())
						PobUi::Tooltip(u8"下載 GGG 官方新版的輿圖資料；目前的配點依節點 ID 保留");
					rx += updW;
				} else if (updaterBusy || ust.phase == AtlasUpdatePhase::Checking) {
					at(rx, smH);
					ImGui::BeginGroup();
					PobUi::Numeric(ust.message.c_str());
					if (ust.phase == AtlasUpdatePhase::Downloading && ust.filesTotal > 0)
						PobUi::ProgressBar((float)ust.filesDone / (float)ust.filesTotal, PobUi::D(120.0f));
					ImGui::EndGroup();
					rx += updW;
				}
				if (!headerCollapsed) {
					at(rx, smH);
					if (PobUi::Button(compressLbl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::Minimize, 0.0f,
					                  canCompress)) {
						std::string msg;
						importFailed = !applyCompress(msg);
						importMsg = msg;
						panelDirty = true;
					}
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						PobUi::Tooltip(u8"用最少的天賦點重新接一次目前的節點。大點、鑰石與端點都會保留，"
						               u8"只有中間繞路的小點會改道；平常點節點不會自動重算，按了才會動，Ctrl+Z 可復原。");
					rx = ImGui::GetItemRectMax().x + PobUi::D(8.0f);
					at(rx, smH);
					if (PobUi::Button(shareLbl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::Share))
						copyShareCode();
					if (ImGui::IsItemHovered())
						PobUi::Tooltip(u8"把這個專案（配點、星盤、地圖格、備註）複製成一段文字，貼給別人匯入");
					rx = ImGui::GetItemRectMax().x + PobUi::D(8.0f);
				}
				at(rx, smH);
				if (PobUi::Button("##more", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::MoreHorizontal,
				                  std::floor(PobUi::D(28.0f))))
					openMoreMenu_ = true;
				moreAnchor_ = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + PobUi::D(4.0f));
				if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"更多：專案、匯入匯出、賽季資料、中英切換");

				// the row, then the header's bottom rule
				ImGui::SetCursorScreenPos(hp);
				ImGui::Dummy(ImVec2(avail, H));
				ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
				{
					const ImVec2 lp = ImGui::GetCursorScreenPos();
					hdl->AddLine(ImVec2(lp.x, lp.y), ImVec2(lp.x + avail, lp.y), Tok::Border, 1.0f);
					ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
				}
			}

			// --- the more menu: under its button, right-aligned to it ---
			if (openMoreMenu_) {
				ImGui::OpenPopup("##atlasmore");
				openMoreMenu_ = false;
			}
			ImGui::SetNextWindowPos(moreAnchor_, ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
			if (PobUi::BeginMenuPopup("##atlasmore")) {
				if (PobUi::MenuRow(PobIcon::Plus, u8"新增專案…")) {
					nameBuf = u8"新專案";
					askNew_ = true;
				}
				if (PobUi::MenuRow(PobIcon::Pencil, u8"重新命名…")) {
					nameBuf = buildFile.Active().name;
					askRename_ = true;
				}
				if (PobUi::MenuRow(PobIcon::CopyPlus, u8"複製專案")) {
					saveActive();
					int idx = buildFile.DuplicateBuild(buildFile.active);
					if (idx >= 0) switchTo(idx);
				}
				if (PobUi::MenuRow(PobIcon::Trash, u8"刪除專案…", nullptr, !lastBuild, true)) askDelete_ = true;
				PobUi::MenuSeparator();
				if (PobUi::MenuRow(PobIcon::Upload, u8"匯入專案 .json…")) pendingDialog_ = ApDialog::ImportBuild;
				if (PobUi::MenuRow(PobIcon::Copy, u8"從剪貼簿匯入分享碼")) {
					const char* clip = ImGui::GetClipboardText();
					std::string perr;
					AtlasBuildEntry e;
					if (clip && AtlasParseShareCode(clip, &e, &perr)) {
						importEntry(e);
					} else {
						importMsg = perr.empty() ? u8"剪貼簿沒有分享碼" : perr;
						importFailed = true;
					}
				}
				if (PobUi::MenuRow(PobIcon::Download, u8"匯出專案 .json…")) {
					saveActive();
					pendingExportName_ = buildFile.Active().name;
					pendingDialog_ = ApDialog::ExportBuild;
				}
				if (headerCollapsed) {
					if (PobUi::MenuRow(PobIcon::Share, u8"複製分享碼")) copyShareCode();
					if (PobUi::MenuRow(PobIcon::Minimize, u8"壓縮到最少點", nullptr, canCompress)) {
						std::string msg;
						importFailed = !applyCompress(msg);
						importMsg = msg;
						panelDirty = true;
					}
				}
				PobUi::MenuSeparator();
				if (PobUi::MenuRow(PobIcon::Refresh, u8"匯入賽季資料…", nullptr, !updaterBusy))
					askImportSeason_ = true;
				if (PobUi::MenuRow(PobIcon::RotateCcw, u8"重置配點…", nullptr, true, true)) askReset_ = true;
				if (zhLoaded) {
					PobUi::MenuSeparator();
					if (PobUi::MenuRow(PobIcon::Languages, showZh ? u8"節點改顯示英文" : u8"節點改顯示中文", "F2"))
						showZh = !showZh;
				}
				PobUi::EndMenuPopup();
			}
			// zh/en display toggle (only when a mapping is available)
			if (zhLoaded && ImGui::IsKeyPressed(ImGuiKey_F2, false)) showZh = !showZh;

			// --- dialogs: one look (Dialog), the destructive answer in danger ---
			if (PobUi::BeginDialog(u8"##newproj", &askNew_, u8"新增專案", u8"新專案從空白配點開始。")) {
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
				PobUi::PushControlFrame();
				ImGui::InputText("##newname", &nameBuf);
				PobUi::PopControlFrame();
				const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, nullptr, u8"建立", !nameBuf.empty(), true);
				if (r == PobUi::DialogResult::Primary) {
					saveActive();
					int idx = buildFile.AddBuild(nameBuf);
					switchTo(idx); // new project starts empty
				}
				PobUi::EndDialog();
			}
			if (PobUi::BeginDialog(u8"##renameproj", &askRename_, u8"重新命名專案", nullptr)) {
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
				PobUi::PushControlFrame();
				ImGui::InputText("##rename", &nameBuf);
				PobUi::PopControlFrame();
				const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", nullptr, nullptr, u8"重新命名", !nameBuf.empty(), true);
				if (r == PobUi::DialogResult::Primary) {
					if (!nameBuf.empty() && nameBuf != buildFile.Active().name)
						buildFile.Active().name = buildFile.UniqueName(nameBuf);
					buildFile.Save(exeDir);
				}
				PobUi::EndDialog();
			}
			{
				const PobUi::DialogResult r = PobUi::ConfirmDialog(u8"##delproj", &askDelete_, u8"刪除這個專案？",
					u8"配點、星盤、地圖格、備註與收益紀錄會一起刪除，無法復原。", buildFile.Active().name.c_str(),
					u8"取消", u8"刪除", nullptr);
				if (r == PobUi::DialogResult::Danger && buildFile.RemoveBuild(buildFile.active)) {
					tree.ApplyAllocIds(buildFile.Active().alloc);
					tree.ApplyTargetIds(buildFile.Active().targets);
					tree.ApplyBlockedIds(buildFile.Active().blocked);
					undo.valid = false;
					saveActive();
					panelDirty = true;
				}
			}
			{
				const PobUi::DialogResult r = PobUi::ConfirmDialog(u8"##resetalloc", &askReset_, u8"清除所有已配置的節點？",
					u8"這個專案的配點會全部清除，無法復原。星盤、地圖格與備註不受影響。", nullptr,
					u8"取消", u8"清除全部", nullptr);
				if (r == PobUi::DialogResult::Danger) {
					tree.Reset();
					saveActive();
					panelDirty = true;
				}
			}
			if (PobUi::BeginDialog(u8"##importseason", &askImportSeason_, u8"匯入新賽季的輿圖資料？",
				u8"1. 到 github.com/grindinggear/atlastree-export 下載（Code > Download ZIP）並解壓縮。\n"
				u8"2. 選取解壓縮後資料夾內的 data.json（assets 資料夾要在它旁邊）。\n"
				u8"匯入會覆寫目前的樹資料；已配置的節點依 ID 對映，消失的節點自動移除。")) {
				const PobUi::DialogResult r = PobUi::DialogButtons(u8"取消", u8"立即檢查更新", nullptr, u8"選擇 data.json");
				if (r == PobUi::DialogResult::Primary) importSeason();
				else if (r == PobUi::DialogResult::Secondary) updater.RequestCheck(true); // manual: skip the daily throttle
				PobUi::EndDialog();
			}
			// Leaving the sandbox: the ONLY place a planning session can reach
			// the build file. Both outcomes are explicit -- there is no default
			// action on a stray click, and no path that writes without asking.
			{
				const std::string planBody =
					u8"目前 " + std::to_string(tree.UsedPoints()) + u8" 點 · 想要 " +
					std::to_string(tree.TargetIdx().size()) + u8" 個 · 排除 " + std::to_string(tree.BlockedIdx().size()) +
					u8" 個。\n保留會覆蓋這個專案原本的 " + std::to_string(planSnapshot.alloc.size()) + u8" 點配置。";
				if (PobUi::BeginDialog(u8"結束規劃", &planningAskExit, u8"保留這次規劃？", planBody.c_str())) {
					const PobUi::DialogResult r = PobUi::DialogButtons(u8"繼續規劃", nullptr, u8"捨棄", u8"保留並儲存");
					if (r == PobUi::DialogResult::Primary) { keepPlanning(); panelDirty = true; finishPlanningPrompt(true); }
					else if (r == PobUi::DialogResult::Danger) { restorePlanning(); panelDirty = true; finishPlanningPrompt(true); }
					else if (r == PobUi::DialogResult::Cancel) finishPlanningPrompt(false);
					PobUi::EndDialog();
				}
			}

			// --- banners: what went wrong, under the header ---
			if (ust.phase == AtlasUpdatePhase::Error) {
				if (PobUi::Banner("##upderr", PobUi::BannerTone::Bad, PobIcon::CircleX, u8"輿圖資料更新失敗",
				                  ust.message.c_str(), false, u8"重試", false) == PobUi::BannerResult::Action)
					updater.StartUpdate();
				ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			}
			if (!errBanner_.empty()) {
				if (PobUi::Banner("##apperr", PobUi::BannerTone::Bad, PobIcon::CircleX, u8"沒有完成",
				                  errBanner_.c_str(), false, nullptr, true) == PobUi::BannerResult::Close)
					errBanner_.clear();
				ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			}
			if (!noticeMsg_.empty()) {
				if (PobUi::Banner("##apnotice", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, u8"配置已依新賽季調整",
				                  noticeMsg_.c_str(), false, nullptr, true) == PobUi::BannerResult::Close)
					noticeMsg_.clear();
				ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			}
			if (!cjkOk) {
				PobUi::Banner("##apcjk", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, u8"中文字型沒有載入",
				              u8"Fonts\\ 內的字型讀不到，中文會顯示成 ?。請確認字型檔還在。", false, nullptr, false);
				ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			}

			// Two faces of one project: the atlas itself, and what running it
			// costs and earns -- plus the revenue panel's account settings. The tab
			// row is a selector only -- each body is drawn below it, so the atlas
			// branch stays exactly as it was. Ctrl+Z sits inside that branch on
			// purpose: typed into the revenue panel's fields it must undo the text,
			// not the atlas.
			{
				const char* tabs[4] = { u8"配置", u8"收益", u8"說明", u8"設定" };
				const int cur = profitMode_ ? 1 + profitPage_ : 0;
				const int next = PobUi::PageTabs("##atlasmode", cur, tabs, 4);
				if (next != cur) {
					profitMode_ = next > 0;
					if (profitMode_) profitPage_ = next - 1;
				}
				ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			}

			if (profitMode_) {
			renderProfitTab();
			} else {

			// --- status row: how to use the canvas on the left, the count on the
			// right. The lit-up mechanic is a pill with its own clear button:
			// without it the rings look like part of the tree.
			{
				const float rowW = ImGui::GetContentRegionAvail().x;
				const ImVec2 sp = ImGui::GetCursorScreenPos();
				const float rowH = std::floor(PobUi::D(28.0f));
				char cntBuf[48];
				snprintf(cntBuf, sizeof(cntBuf), u8"已用 %d / %d 點", tree.UsedPoints(), tree.TotalPoints());
				ImFont* numF = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
				const float cntW = numF->CalcTextSizeA(numF->FontSize, FLT_MAX, 0.0f, cntBuf).x;   // Numeric's face
				float x = sp.x;
				if (!mechSel.empty()) {
					const AtlasMechanicDef* d = AtlasMechanicById(mechSel);
					const std::vector<int>* nn = mechFind(mechNodeIdx, mechSel);
					const std::vector<int>* mm = mechFind(mechMastIdx, mechSel);
					char pill[256];
					snprintf(pill, sizeof(pill), u8"機制：%s · %d 節點 · %d 叢集",
						d ? (showZh && zhLoaded ? d->zh.c_str() : d->en.c_str()) : mechSel.c_str(),
						nn ? (int)nn->size() : 0, mm ? (int)mm->size() : 0);
					ImGui::SetCursorScreenPos(ImVec2(x, sp.y + std::floor((rowH - PobUi::D(24.0f)) * 0.5f)));
					PobUi::StatusPill(PobUi::Tone::Warn, pill);
					x = ImGui::GetItemRectMax().x + PobUi::D(6.0f);
					ImGui::SetCursorScreenPos(ImVec2(x, sp.y));
					if (PobUi::Button(u8"清除##mech", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::X)) {
						mechSel.clear();
						applyMechHighlight();
					}
					x = ImGui::GetItemRectMax().x + PobUi::D(12.0f);
				}
				const std::string& hint = !view.StatusLine().empty() ? view.StatusLine()
					: std::string(planningMode ? u8"規劃模式：左鍵依序標記想要、不要、清除，路徑自動重算；期間不寫入存檔"
					                           : u8"滾輪縮放 · 拖曳平移 · 左鍵配點或移除 · 點叢集中央圖示標出同機制");
				ImGui::SetCursorScreenPos(ImVec2(x, sp.y + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f)));
				{
					// one line: clipped before the count, never wrapped
					ImGui::PushClipRect(ImVec2(x, sp.y), ImVec2(sp.x + rowW - cntW - PobUi::D(16.0f), sp.y + rowH), true);
					PobUi::Hint(hint.c_str());
					ImGui::PopClipRect();
				}
				ImGui::SetCursorScreenPos(ImVec2(sp.x + rowW - cntW, sp.y + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f)));
				PobUi::Numeric(cntBuf, tree.UsedPoints() > tree.TotalPoints() ? Tok::Warning : Tok::Text);
				ImGui::SetCursorScreenPos(sp);
				ImGui::Dummy(ImVec2(rowW, rowH));
				ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
			}

			// Ctrl+Z: planning mode and the compress button both move wiring the
			// user did not place, so there is always exactly one step back.
			if (undo.valid && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
				tree.ApplyAllocIds(undo.alloc);
				tree.ApplyTargetIds(undo.targets);
				tree.ApplyBlockedIds(undo.blocked);
				undo.valid = false;
				saveActive();
				panelDirty = true;
			}

			// --- canvas + splitter + right summary panel ---
			// default: 35% of the window; the splitter drag below overrides it
			// and the chosen width persists in PobTools/atlas_ui.json
			if (panelW < 0.0f)
				panelW = uiState.panelW > 0.0f ? uiState.panelW * scale
				                               : std::clamp(dispW * 0.35f, 380.0f * scale, 700.0f * scale);
			// The upper bound can fall BELOW the lower one in a narrow window, and
			// std::clamp with lo > hi is undefined -- not merely odd. Reachable since
			// dispW became this panel's width rather than the whole screen's: neither
			// the launcher nor the standalone window has a minimum size, so anything
			// under about 533px of content gets there.
			const float panelMin = 320.0f * scale;
			const float panelMax = (std::max)(panelMin, dispW * 0.6f);
			panelW = std::clamp(panelW, panelMin, panelMax);
			const float splitW = 8.0f * scale;
			ImGui::BeginChild("##treewrap", ImVec2(-(panelW + splitW), 0), false,
				ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
			snapshot(frameStart);   // the pre-click state, in case Draw changes it
			bool changed = view.Draw(tree, scale, showZh && zhLoaded ? &i18n : nullptr, planningMode); // auto-saves below; the file is tiny
			if (changed) undo = frameStart;
			ImGui::EndChild();

			ImGui::SameLine(0, 0);
			ImGui::InvisibleButton("##splitter", ImVec2(splitW, ImGui::GetContentRegionAvail().y));
			if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
				ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
				ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
				float cx = (a.x + b.x) * 0.5f;
				ImGui::GetWindowDrawList()->AddLine(ImVec2(cx, a.y + 8.0f * scale), ImVec2(cx, b.y - 8.0f * scale),
					(Tok::Accent & 0x00FFFFFFu) | ((ImU32)(ImGui::IsItemActive() ? 220 : 120) << 24), 2.0f);
			}
			if (ImGui::IsItemActive())
				panelW = std::clamp(panelW - io.MouseDelta.x, panelMin, panelMax);
			if (ImGui::IsItemDeactivated()) { // write once on release, not per drag frame
				uiState.panelW = panelW / scale;
				uiState.Save(exeDir);
			}
			// Clicking a mastery icon toggles its mechanic. The view consumed the
			// click, so this can never also allocate.
			if (view.ClickedMastery() >= 0) {
				int mi = view.ClickedMastery();
				std::string hit;
				for (const auto& kv : mechMastIdx)
					if (std::find(kv.second.begin(), kv.second.end(), mi) != kv.second.end()) {
						hit = kv.first;
						break;
					}
				mechSel = (hit.empty() || hit == mechSel) ? std::string() : hit;
				applyMechHighlight();
			}
			if (!mechSel.empty() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
				mechSel.clear();
				applyMechHighlight();
			}
			if (changed) {
				saveActive();
				panelDirty = true;
			}
			if (panelDirty) {
				rebuildPanel();
				panelDirty = false;
			}

			ImGui::SameLine(0, 0);
			ImGui::BeginChild("##sidepanel", ImVec2(0, 0), true);

			if (compareMode) {
			renderComparePanel();
			} else {

			// --- points summary (pinned): the count in big digits, a bar, one hint ---
			{
				const int used = tree.UsedPoints(), total = tree.TotalPoints();
				const float w = ImGui::GetContentRegionAvail().x;
				const ImVec2 p0 = ImGui::GetCursorScreenPos();
				ImDrawList* sdl = ImGui::GetWindowDrawList();
				ImFont* bigF = fontBig ? fontBig : ImGui::GetFont();
				const float bigPx = bigF->FontSize;
				const std::string usedS = std::to_string(used), totS = " / " + std::to_string(total);
				const float totW = ImGui::CalcTextSize(totS.c_str()).x;
				const float usedW = bigF->CalcTextSizeA(bigPx, FLT_MAX, 0.0f, usedS.c_str()).x;
				const float rowH = bigPx;
				ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rowH - ImGui::GetTextLineHeight() - PobUi::D(2.0f)));
				PobUi::Hint(u8"已配置點數");
				const float base = p0.y + rowH;   // the digits' and the total's common bottom
				sdl->AddText(bigF, bigPx, ImVec2(p0.x + w - totW - usedW - PobUi::D(4.0f), p0.y),
				             used > total ? Tok::Warning : Tok::Text, usedS.c_str());
				sdl->AddText(ImVec2(p0.x + w - totW, base - ImGui::GetTextLineHeight() - PobUi::D(2.0f)), Tok::TextMuted,
				             totS.c_str());
				ImGui::SetCursorScreenPos(ImVec2(p0.x, base + PobUi::D(6.0f)));
				PobUi::ProgressBar(total > 0 ? (float)used / (float)total : 0.0f, w);
				ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
				int nTargets = (int)tree.TargetIdx().size();
				int wiring = used - nTargets;
				if (wiring < 0) wiring = 0;   // (windows.h's max macro is in scope here)
				std::string line = u8"自己點的 " + std::to_string(nTargets) + u8" 個 · 連接用 " + std::to_string(wiring) + u8" 點";
				if (undo.valid) line += u8" · Ctrl+Z 復原";
				PobUi::Hint(line.c_str());
				if (nTargets > AtlasOptExactCap()) {
					ImGui::SameLine(0, PobUi::D(8.0f));
					PobUi::Hint(u8"近似解", 0.0f, Tok::Warning);
					if (ImGui::IsItemHovered()) {
						char tip[256];
						snprintf(tip, sizeof(tip), u8"超過 %d 個時，「壓縮到最少點」與規劃模式改用近似演算法，"
						                           u8"可能比真正的最少點多幾點。平常點節點不受影響。", AtlasOptExactCap());
						PobUi::Tooltip(tip);
					}
				}
			}
			ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));

			// --- search (pinned; filters stats AND the node list, en + zh) ---
			PobUi::SearchField("##panelsearch", panelSearch, (int)sizeof(panelSearch), u8"搜尋加成或節點…",
			                   ImGui::GetContentRegionAvail().x);
			std::string needle = ToLowerAscii(panelSearch);
			auto matches = [&](const std::string& key) {
				return needle.empty() || key.find(needle) != std::string::npos;
			};
			ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));

			// --- scrolling content ---
			// borderless children skip WindowPadding by default; force it so
			// text keeps a margin from the panel edges
			ImGui::PushStyleColor(ImGuiCol_ChildBg, PobUi::TokV4(Tok::Surface1));
			ImGui::BeginChild("##panelscroll", ImVec2(0, 0), false,
				ImGuiWindowFlags_AlwaysUseWindowPadding);
			ImGui::PopStyleColor();
			// Quadrants, then the map, then the device that map goes into, then
			// notes. All show even with nothing allocated: they are useful
			// before a single node is picked.
			// Each panel gets its own id namespace. Without this the astrolabe
			// quadrants and the map slots collide: both loop with PushID(index)
			// starting at 0 and both label their widgets "##slot", so in this
			// single child window they hash to the SAME ImGuiID. ImGui requires
			// ids to be unique per window; when they are not, the first widget
			// submitted keeps the interaction and the later one goes dead.
			ImGui::PushID("astrolabes"); renderAstrolabePanel(); ImGui::PopID();
			ImGui::PushID("mainmap");    renderMapPanel();       ImGui::PopID();
			ImGui::PushID("mapslots");   renderScarabPanel();    ImGui::PopID();
			ImGui::PushID("notes");      renderNotesPanel();     ImGui::PopID();
			ImGui::PushID("mechanics");  renderMechanicPanel();  ImGui::PopID();
			// (the pickers and the slot menus are submitted after EndChild, below)
			bool anyAlloc = false;
			for (const auto& g : nodeGroups) anyAlloc = anyAlloc || !g.empty();
			if (!anyAlloc) {
				ImGui::Dummy(ImVec2(0, PobUi::D(16.0f)));
				ImGui::TextUnformatted(u8"尚未配置任何節點");
				PobUi::Hint(u8"在左側輿圖上點擊節點開始規劃；滾輪縮放、拖曳平移。", ImGui::GetContentRegionAvail().x);
			} else {
				ImGui::PushID("stats");
				if (PobUi::CollapsingSection(u8"加成統計", "###statshdr", PobIcon::ChartColumn, u8"同一種加成已加總")) {
					const std::vector<int>& order = (showZh && zhLoaded) ? statOrderZh : statOrderEn;
					int shown = 0;
					for (int gi : order) {
						const StatAggGroup& g = statAgg[gi];
						if (!matches(g.searchKey)) continue;
						shown++;
						const std::string& disp = (showZh && zhLoaded) ? g.dispZh : g.dispEn;
						if (g.kind == StatAggGroup::kMulti && g.count > 1) {
							ImGui::TextColored(PobUi::TokV4(Tok::TreeLinkOn), "x%d", g.count);
							ImGui::SameLine(0, PobUi::D(6.0f));
						}
						// summed values pop in the link-on blue; on/off effects recede
						const std::uint32_t col = g.kind == StatAggGroup::kSummed ? Tok::TreeLinkOn
						                        : g.kind == StatAggGroup::kBoolean ? Tok::TextMuted : Tok::Text;
						ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(col));
						ImGui::TextWrapped("%s", disp.c_str());
						ImGui::PopStyleColor();
					}
					if (shown == 0) PobUi::Hint(u8"沒有符合搜尋的加成");
					ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
				}
				ImGui::PopID();
				ImGui::PushID("nodes");
				if (PobUi::CollapsingSection(u8"節點清單", "###nodeshdr", PobIcon::List, u8"點一下在輿圖上定位")) {
					for (int r = 0; r < 4; r++) {
						if (nodeGroups[r].empty()) continue;
						int m = 0;
						for (const PanelNode& p : nodeGroups[r])
							if (matches(p.searchKey)) m++;
						if (m == 0 && !needle.empty()) continue;
						const std::uint32_t kindCol = PobUi::TreeKindColor(kRankKind[r]);
						// group header: 4px colour bar + name + count
						ImDrawList* pdl = ImGui::GetWindowDrawList();
						ImVec2 hp = ImGui::GetCursorScreenPos();
						float lh = ImGui::GetTextLineHeight();
						pdl->AddRectFilled(ImVec2(hp.x, hp.y + lh * 0.20f),
							ImVec2(hp.x + PobUi::D(4.0f), hp.y + lh * 0.95f), kindCol, PobUi::D(2.0f));
						ImGui::Dummy(ImVec2(PobUi::D(9.0f), 0));
						ImGui::SameLine(0, 0);
						char gh[96];
						if (needle.empty()) snprintf(gh, sizeof(gh), "%s (%d)", kGroupName[r], (int)nodeGroups[r].size());
						else snprintf(gh, sizeof(gh), "%s (%d/%d)", kGroupName[r], m, (int)nodeGroups[r].size());
						ImGui::TextColored(PobUi::TokV4(Tok::TextMuted), "%s", gh);
						ImGui::Indent(PobUi::D(10.0f));
						for (const PanelNode& p : nodeGroups[r]) {
							if (!matches(p.searchKey)) continue;
							const AtlasNode& n = tree.nodes[p.idx];
							const std::string& nm = showZh && zhLoaded ? i18n.NodeName(n.id, n.name) : n.name;
							ImGui::PushID(p.idx); // duplicate display names exist
							ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(kindCol));
							if (ImGui::Selectable(nm.empty() ? u8"(未命名)" : nm.c_str(), false))
								view.CenterOn(tree, p.idx);
							ImGui::PopStyleColor();
							if (ImGui::IsItemHovered()) {
								ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(14.0f), PobUi::D(10.0f)));
								ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(460.0f * scale, FLT_MAX));
								ImGui::BeginTooltip();
								ImGui::TextColored(PobUi::TokV4(kindCol), "%s", nm.empty() ? u8"(未命名)" : nm.c_str());
								// explicit wrap width so a short title cannot make the tooltip a narrow strip
								ImGui::PushTextWrapPos(380.0f * scale);
								for (const std::string& s : n.stats) {
									ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::AccentText));
									ImGui::TextUnformatted(StripStatMarkup(showZh && zhLoaded ? i18n.StatLine(s) : s).c_str());
									ImGui::PopStyleColor();
								}
								ImGui::PopTextWrapPos();
								PobUi::Hint(u8"點擊清單以在輿圖中定位");
								ImGui::EndTooltip();
								ImGui::PopStyleVar();
							}
							ImGui::PopID();
						}
						ImGui::Unindent(PobUi::D(10.0f));
						ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
					}
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
			} // end normal side panel (else branch of compareMode)
			ImGui::EndChild();
			} // end 輿圖配置 (else branch of profitMode_)
		}

		// Outside every child: a popup submitted inside one gets clipped to it,
		// and this one is wide enough to be flipped clean out of the panel.
		renderScarabPicker();
		renderAstroPicker();
		renderSlotMenus();
		flushMsg();   // what this frame's actions reported

		// Escape dismisses an ImGui modal, and only the prompt's buttons resolve a
		// close. Without this a dismissed prompt would leave the panel permanently
		// "asking": unclosable, and blocking the launcher's own shutdown.
		if (close_ == ToolCloseState::Asking && !planningAskExit &&
		    !ImGui::IsPopupOpen(u8"結束規劃"))
			close_ = ToolCloseState::Cancelled;
	}

	// The one place this panel may block; the host calls it after the frame is on
	// screen and after the docked windows have been hidden.
	void RunDeferred() override
	{
		if (warehouse_) warehouse_->RunDeferred();
		const ApDialog want = pendingDialog_;
		pendingDialog_ = ApDialog::None;
		HWND owner = (HWND)(host_ ? host_->hostHwnd : nullptr);
		switch (want) {
			case ApDialog::None:
				break;
			case ApDialog::ImportSeasonData:
				importSeasonFrom(OpenDataJsonDialog(owner));
				break;
			case ApDialog::ExportBuild: {
				std::wstring path = SaveBuildJsonDialog(owner, pendingExportName_);
				if (!path.empty()) {
					bool ok = PlannerWriteFile(path, AtlasExportJson(buildFile.Active(), tree.Version()));
					importMsg = ok ? u8"專案已匯出" : u8"匯出檔寫入失敗";
					importFailed = !ok;
				}
				break;
			}
			case ApDialog::ImportBuild: {
				std::wstring path = OpenBuildJsonDialog(owner);
				if (!path.empty()) {
					std::string body, perr;
					AtlasBuildEntry e;
					if (PlannerReadFile(path, body) && AtlasParseExportJson(body, &e, &perr)) {
						importEntry(e);
					} else {
						importMsg = perr.empty() ? u8"無法讀取匯入檔" : perr;
						importFailed = true;
					}
				}
				break;
			}
		}
	}

	ToolCloseState RequestClose() override
	{
		if (close_ == ToolCloseState::Asking) return close_;
		// The embedded revenue panel only saves its settings and always agrees.
		if (warehouse_) warehouse_->RequestClose();
		// Closing mid-plan must not silently drop the sandbox work and must not
		// silently keep it either -- the same prompt the 結束規劃 button raises.
		if (ready && planningMode) { planningAskExit = true; close_ = ToolCloseState::Asking; }
		else close_ = ToolCloseState::Closed;
		return close_;
	}
	ToolCloseState CloseState() const override { return close_; }
	void AbortClose() override
	{
		if (close_ == ToolCloseState::Closed) close_ = ToolCloseState::Open;
		if (warehouse_) warehouse_->AbortClose();
	}

	void Shutdown() override
	{
		if (shutdown_) return;
		shutdown_ = true;
		// The embedded revenue panel and the cost card's price feed first: both
		// own worker threads, and the panel owns GL textures (context still current).
		if (warehouse_) warehouse_->Shutdown();
		if (priceFeedStarted_) priceFeed_.Shutdown();
		updater.Shutdown();     // cancels any in-flight download, joins the worker
		icons.Shutdown();       // joins the icon worker, deletes its textures (GL thread)
		view.DestroyTextures(); // while the GL context is still current
	}

	~AtlasPlannerPanel() override { Shutdown(); }

	PobUi::Density Density() const override { return PobUi::Density::Canvas; }
	const char* PanelId() const override { return "atlas"; }

private:
	// The 結束規劃 prompt is raised two ways -- the toolbar button (stay in the
	// tool) and a close request (leave once it is answered). Only the second has a
	// close to resolve, so the answer alone cannot decide it.
	void finishPlanningPrompt(bool answered)
	{
		if (close_ != ToolCloseState::Asking) return;
		close_ = answered ? ToolCloseState::Closed : ToolCloseState::Cancelled;
	}

	// --- data + view ---
	AtlasTreeData tree;
	AtlasView view;
	std::string loadErr;
	// Multi-project build file: the in-memory copy is the single source of
	// truth while the planner is open; every save funnels through saveActive.
	AtlasBuildFile buildFile;

	// Scarab catalogue + icon fetcher. Both are optional: a missing
	// Data/scarabs_poe1.json hides the section (and makes Sanitize a no-op, so a
	// saved scarab list survives untouched), and no network just means no icons.
	ScarabDb scarabDb;
	std::string scarabErr;

	// Astrolabes (3.29 Shaped Regions, one per atlas quadrant) and the map
	// catalogue behind the project's main-map pick. Optional on exactly the same
	// terms as the scarabs: absent data hides the section and turns Sanitize
	// into a no-op rather than erasing what the user saved.
	AstrolabeDb astroDb;
	std::string astroErr;
	AtlasMapDb mapDb;
	std::string mapErr;


	IconManager icons;

	// 收益 tab (倉庫收益 × 輿圖策略). The price feed and the embedded panel start
	// on first use only: Init must not touch the network, and the launcher and
	// --panel-selftest Init every panel.
	bool profitMode_ = false;
	// Which page the embedded panel shows while profitMode_ is on, in the ints
	// WarehouseEmbed::page speaks: 0 收益, 1 說明, 2 設定.
	int profitPage_ = 0;
	// This frame's prices and map cost, for the panel's left-column callbacks
	// (they run inside warehouse_->Frame(), right after these are set).
	NinjaPriceFeed::Status sidePs_;
	MapCostSummary sideCost_;
	std::unique_ptr<IToolPanel> warehouse_; // the stash revenue panel, embedded
	bool warehouseOk_ = false;
	NinjaPriceFeed priceFeed_;
	bool priceFeedStarted_ = false;
	std::string costLeague_;         // league the cost card prices in
	double lastLeagueCheck_ = -10.0; // ImGui time of the last settings poll
	// 收益紀錄 bind dialog: the league's history, read when the dialog opens.
	WarehouseHistory bindHist_;
	std::string bindLeague_;
	long long bindFromUtc_ = 0, bindToUtc_ = 0;
	FilterI18n bindI18n_; // names stored with a record; loaded on the first bind
	bool bindI18nLoaded_ = false;

	std::string nameBuf; // shared by the new/rename project modals
	// Dialogs raised from the more menu (opened at the panel's root level).
	bool askNew_ = false, askRename_ = false, askDelete_ = false, askReset_ = false, askImportSeason_ = false;
	bool openMoreMenu_ = false;
	ImVec2 moreAnchor_ = ImVec2(0, 0);   // bottom-right of the more button
	std::string errBanner_;   // last failure, shown as a banner until closed
	std::string noticeMsg_;   // startup notice (nodes dropped by a season change)

	void copyShareCode()
	{
		saveActive();
		std::string code = AtlasBuildShareCode(buildFile.Active(), tree.Version());
		if (!code.empty()) {
			ImGui::SetClipboardText(code.c_str());
			importMsg = u8"分享碼已複製到剪貼簿";
			importFailed = false;
		}
	}

	// --- persisted UI state (panel width + last viewed season) ---
	AtlasUiState uiState;
	float panelW = -1.0f; // sentinel: initialized on the first frame (needs DisplaySize)

	// --- version registry: which seasons are installed, which one is shown ---
	AtlasVersionIndex verIndex;
	// Load() repairs the index against the season folders actually on disk (the
	// packaged atlas_index.json overwrites the user's on every app update). Write
	// the repair back once so it sticks.
	// viewTag = the season currently on the canvas (persisted choice, else active)
	std::string viewTag;   // assigned in Init(), from uiState + verIndex

	int startupDropped = 0;  // nodes lost because the saved build predates this season
	bool ready = false;      // set by the initial loadSeason() below
	std::string importMsg;   // last import result shown in the toolbar
	bool importFailed = false;

	// The newest installed season is canonical (the one the build persists for);
	// an older-season view is a read-only preview.
	bool onCanonicalSeason()
	{
		return verIndex.Active().empty() || viewTag == verIndex.Active();
	}
	// Capture the allocation only on the canonical season, so a preview of an
	// older season never prunes it back to that season's subset. The file is
	// still written either way: notes and scarabs are season-independent, and
	// before they existed an edit made while previewing was simply lost.
	// Set while the sandbox planning mode is open. saveActive() checks it, so
	// there is exactly ONE place that can write during planning: nowhere.
	bool planningMode = false;
	// ---- 收益 tab: 倉庫收益 × 輿圖策略 ----------------------------------------
	// What one map of the ACTIVE project costs -- its map-device scarabs and
	// fragments at poe.ninja prices, plus a typed map price -- above the stash
	// revenue panel itself (the same panel the launcher's 倉庫收益 button opens),
	// so a strategy's cost and its income are read in one window.
	void renderProfitTab()
	{
		if (!priceFeedStarted_) {
			costLeague_ = WarehouseSavedLeague(exeDir);
			priceFeed_.Init(exeDir);
			priceFeed_.Request("poe1", costLeague_, false);
			priceFeedStarted_ = true;
			lastLeagueCheck_ = ImGui::GetTime();
		}
		// Prices follow the league the revenue panel below is set to. It saves on
		// every change, so a cheap poll of its settings file is enough.
		if (ImGui::GetTime() - lastLeagueCheck_ > 2.0) {
			lastLeagueCheck_ = ImGui::GetTime();
			const std::string lg = WarehouseSavedLeague(exeDir);
			if (lg != costLeague_) {
				costLeague_ = lg;
				priceFeed_.Request("poe1", costLeague_, false);
			}
		}
		const NinjaPriceFeed::Status ps = priceFeed_.Poll();

		const AtlasBuildEntry& b = buildFile.Active();
		std::vector<MapCostInput> slots;
		for (const std::string& id : b.scarabs) {
			const ScarabDef* d = scarabDb.ById(id);
			if (!d) continue;
			MapCostInput in;
			in.id = d->id;
			in.en = d->en;
			in.zh = d->zh;
			in.art = d->art;
			in.tradable = d->stash;
			slots.push_back(std::move(in));
		}
		const MapCostSummary cost = ComputeMapCost(
		    slots, b.mapPrice,
		    [&ps](const std::string& key, NinjaPrice* out) {
			    if (!ps.prices) return false;
			    auto it = ps.prices->find(key);
			    if (it == ps.prices->end()) return false;
			    *out = it->second;
			    return true;
		    },
		    &b.costPrices);

		// The cost card sits beside the panel's 主要增減, the bind button ends its
		// action row, and the revenue-record buttons close its 設定 page (user
		// layout, 2026-10-08): the panel calls back into these during its Frame()
		// below, so this frame's figures are handed over first.
		sidePs_ = ps;
		sideCost_ = cost;
		if (!warehouse_) {
			WarehouseEmbed e;
			e.topCard = [this] { renderCostCard(sidePs_, sideCost_); };
			e.topBar = [this] { renderBindBar(sideCost_); };
			e.topBarWidth = [this] { return bindBarWidth(); };
			e.settingsBottom = [this] { renderBindButtons(sideCost_); };
			e.page           = &profitPage_;
			// A setup banner's 「前往設定」: our tab row owns the pages.
			e.goPage = [this](int p) {
				profitMode_ = true;
				profitPage_ = p;
			};
			warehouse_.reset(CreateWarehousePanelEmbedded(e));
			warehouseOk_ = warehouse_->Init(*host_);
		}
		ImGui::PushID("warehouse");
		ImGui::BeginChild("##whembed", ImVec2(0, 0), false);
		if (warehouseOk_) warehouse_->Frame();
		ImGui::EndChild();
		ImGui::PopID();
	}

	// Why the total is incomplete, for the card's footnote; "" when it is not.
	static std::string CostNote(const MapCostSummary& cost)
	{
		std::string note;
		auto add = [&note](const std::string& part) {
			if (!note.empty()) note += u8"、";
			note += part;
		};
		if (cost.unpricedKinds > 0) add(std::to_string(cost.unpricedKinds) + u8" 種未估價");
		if (cost.untradableKinds > 0) add(std::to_string(cost.untradableKinds) + u8" 種不可交易");
		if (!cost.mapIncluded) add(u8"未含地圖");
		return note;
	}

	// Right-aligned text in what is left of the current line (a table cell).
	static void CostRightText(const char* text, const ImVec4& col)
	{
		const float w = ImGui::CalcTextSize(text).x;
		const float avail = ImGui::GetContentRegionAvail().x;
		if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
		ImGui::TextColored(col, "%s", text);
	}

	// The project's per-map cost, beside the revenue panel's 主要增減 (design
	// 2026-10-08). The panel draws the card (padded); this is its content,
	// laid out in PobUi::CardInnerWidth(): a head row -- title, total, the two
	// price actions --, the item table, and the plan footer. Cost basis = what
	// was actually PAID: a bulk buyer records the market once and the cost stops
	// floating; a batch buyer types each batch's price into the 單價 column.
	void renderCostCard(const NinjaPriceFeed::Status& ps, const MapCostSummary& cost)
	{
		AtlasBuildEntry& b = buildFile.Active();
		const ImVec4 dim = PobUi::TokV4(Tok::TextMuted);
		const ImVec4 faint = PobUi::TokV4(Tok::TextFaint);
		const ImVec4 text = PobUi::TokV4(Tok::Text);
		const double rate = ps.divineRate;
		auto money = [rate](double chaos) { return WhFmt::FormatValue(chaos, true, rate); };
		const std::string note = CostNote(cost);
		const float innerX = PobUi::CardInnerX(), innerW = PobUi::CardInnerWidth();
		ImFont* small = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
		const float smallPx = small->FontSize;

		// ---- head: title, total ...... 清除記錄 | 記錄目前市價 | 重新取得價格
		{
			const ImVec2 head = ImGui::GetCursorScreenPos();
			const float btnH = std::floor(PobUi::D(28.0f));
			const float headH = (std::max)(btnH, ImGui::GetTextLineHeight());
			ImGui::SetCursorScreenPos(ImVec2(head.x, head.y + std::floor((headH - ImGui::GetTextLineHeight()) * 0.5f)));
			PobUi::Heading(u8"每張圖成本");
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"方案：%s", b.name.c_str());
			const std::string tot = std::string(u8"合計 ") + money(cost.totalChaos);
			ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMax().x + PobUi::D(10.0f),
			                                 head.y + std::floor((headH - ImGui::GetFontSize()) * 0.5f)));
			ImGui::TextColored(PobUi::TokV4(Tok::AccentText), "%s", tot.c_str());

			const char* lClear = u8"清除記錄";
			const char* lRecord = u8"記錄目前市價";
			const char* lRefresh = ps.busy ? u8"取得價格中…##refresh" : u8"重新取得價格##refresh";
			const float gap = PobUi::D(4.0f);
			float w = PobUi::ButtonWidth(lRecord, PobUi::BtnSize::Sm) + gap +
			          PobUi::ButtonWidth(lRefresh, PobUi::BtnSize::Sm, PobIcon::Refresh);
			if (!b.costPrices.empty()) w += PobUi::ButtonWidth(lClear, PobUi::BtnSize::Sm) + gap;
			ImGui::SetCursorScreenPos(ImVec2(innerX + innerW - w, head.y + std::floor((headH - btnH) * 0.5f)));
			if (!b.costPrices.empty()) {
				if (PobUi::Button(lClear, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
					b.costPrices.clear();
					b.costRecordedUtc = 0;
					saveActive();
				}
				if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"清除記錄的成本單價，回到跟隨市價");
				ImGui::SameLine(0, gap);
			}
			if (PobUi::Button(lRecord, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, nullptr, 0.0f,
			                  ps.prices && !cost.lines.empty())) {
				for (const MapCostLine& l : cost.lines)
					if (l.marketEach > 0.0) b.costPrices[l.id] = l.marketEach;
				b.costRecordedUtc = (long long)std::time(nullptr);
				saveActive();
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				std::string tip = u8"一次大量購入：按一下記錄當下市價，之後成本固定、不再隨市價浮動。\n"
				                  u8"分批購入：直接在「單價」欄輸入實際買價。";
				if (b.costRecordedUtc > 0) tip += u8"\n目前的記錄：" + FmtLocalUtc(b.costRecordedUtc);
				else if (!b.costPrices.empty()) tip += u8"\n目前的成本為手動輸入";
				ImGui::SetTooltip("%s", tip.c_str());
			}
			ImGui::SameLine(0, gap);
			if (PobUi::Button(lRefresh, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Refresh, 0.0f, !ps.busy))
				priceFeed_.Request("poe1", costLeague_, true);
			if (ps.prices && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
				const long long mins =
				    ps.fetchedUtc > 0 ? ((long long)std::time(nullptr) - ps.fetchedUtc) / 60 : 0;
				ImGui::SetTooltip(u8"poe.ninja · %s · %lld 分鐘前", ps.league.c_str(), mins < 0 ? 0 : mins);
			}
			ImGui::SetCursorScreenPos(head);
			ImGui::Dummy(ImVec2(innerW, headH));
		}
		if (!ps.prices && !ps.error.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::Danger));
			ImGui::TextWrapped("%s", ps.error.c_str());
			ImGui::PopStyleColor();
		}

		if (!scarabDb.available()) {
			PobUi::Hint(u8"缺少聖甲蟲資料檔，無法估算");
			return;
		}
		// Column widths from what they hold (header vs a sample value), not a
		// fixed N*scale: the user's font size is not part of `scale`. The name
		// column takes the rest and wraps instead of cutting a long name.
		const ImGuiStyle& sty = ImGui::GetStyle();
		auto fit = [&sty](const char* header, const char* sample) {
			const float h = ImGui::CalcTextSize(header).x, s = ImGui::CalcTextSize(sample).x;
			return (h > s ? h : s) + sty.CellPadding.x * 2.0f;
		};
		const float iconSz = (std::min)(ImGui::GetTextLineHeight(), PobUi::D(22.0f));
		// Table (design system): muted header on surface-1, rows split by border-subtle.
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, PobUi::TokV4(Tok::Surface1));
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, PobUi::TokV4(Tok::BorderSubtle));
		ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, PobUi::TokV4(Tok::Border));
		// The price fields are the design's compact 28 px inputs, so a row is
		// not twice the height of its text.
		ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
		                    ImVec2(PobUi::D(8.0f), (std::max)(1.0f, std::floor((PobUi::D(28.0f) - ImGui::GetFontSize()) * 0.5f))));
		const float textDy = (std::max)(0.0f, std::floor((ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f));
		const float iconDy = (std::max)(0.0f, std::floor((ImGui::GetFrameHeight() - iconSz) * 0.5f));
		if (ImGui::BeginTable("##costlines", 4, ImGuiTableFlags_BordersInnerH, ImVec2(innerW, 0.0f))) {
			ImGui::TableSetupColumn(u8"項目", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(u8"每張用量", ImGuiTableColumnFlags_WidthFixed, fit(u8"每張用量", "x99"));
			ImGui::TableSetupColumn(u8"單價", ImGuiTableColumnFlags_WidthFixed,
			                        (std::max)(fit(u8"單價", "9999.9"), PobUi::D(84.0f) + sty.CellPadding.x * 2.0f));
			ImGui::TableSetupColumn(u8"小計", ImGuiTableColumnFlags_WidthFixed,
			                        (std::max)(fit(u8"小計", "9999.9 c"), fit(u8"小計", u8"不可交易")));
			// Header labels muted, the figures' columns right-aligned.
			ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
			for (int c = 0; c < 4; c++) {
				if (!ImGui::TableSetColumnIndex(c)) continue;
				const char* label = ImGui::TableGetColumnName(c);
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float cellW = ImGui::GetContentRegionAvail().x;
				ImGui::PushID(c);
				ImGui::TableHeader("##hdr");
				ImGui::PopID();
				const float tw = ImGui::CalcTextSize(label).x;
				ImGui::GetWindowDrawList()->AddText(ImVec2(c == 0 ? p.x : p.x + cellW - tw, p.y), Tok::TextMuted, label);
			}
			if (cost.lines.empty()) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				PobUi::Hint(u8"此方案的地圖格沒有放聖甲蟲或碎片");
			}
			for (const MapCostLine& l : cost.lines) {
				ImGui::TableNextRow();
				ImGui::PushID(l.id.c_str());
				ImGui::TableNextColumn();
				icons.RequestPath(l.art);
				// Icon and text centred on the price field's height.
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + iconDy);
				const ImVec2 ip = ImGui::GetCursorScreenPos();
				if (unsigned tex = icons.TextureByPath(l.art)) {
					ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(iconSz, iconSz));
				} else {
					ImGui::GetWindowDrawList()->AddRectFilled(ip, ImVec2(ip.x + iconSz, ip.y + iconSz), Tok::Surface3,
					                                          PobUi::D(4.0f));
					ImGui::Dummy(ImVec2(iconSz, iconSz));
				}
				ImGui::SameLine(0, PobUi::D(8.0f));
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() - iconDy + textDy);
				const std::string& nm = (showZh && !l.zh.empty()) ? l.zh : l.en;
				ImGui::TextWrapped("%s", nm.c_str());
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", (&nm == &l.en ? l.zh : l.en).c_str());
				ImGui::TableNextColumn();
				{
					char q[16];
					snprintf(q, sizeof(q), "x%d", l.qty);
					ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textDy);
					CostRightText(q, text);
				}
				// 單價: the recorded/typed price, or -- dimmed -- the market it
				// currently follows. Typing a price fixes it for this project.
				ImGui::TableNextColumn();
				double unit = l.chaosEach;
				if (!l.fromBasis) ImGui::PushStyleColor(ImGuiCol_Text, dim);
				ImGui::SetNextItemWidth(-FLT_MIN);
				if (ImGui::InputDouble("##unit", &unit, 0.0, 0.0, "%.1f")) {
					if (unit > 0.0) b.costPrices[l.id] = unit;
					else b.costPrices.erase(l.id); // 0 = back to following the market
				}
				if (!l.fromBasis) ImGui::PopStyleColor();
				if (ImGui::IsItemDeactivatedAfterEdit()) saveActive();
				if (ImGui::IsItemHovered()) {
					const std::string market =
					    l.marketEach > 0.0 ? money(l.marketEach)
					                       : std::string(l.tradable ? u8"未估價" : u8"不可交易");
					ImGui::SetTooltip(u8"%s\n目前市價：%s",
					                  l.fromBasis ? u8"已記錄的成本單價（混沌石）；改成 0 = 回到跟隨市價"
					                              : u8"目前跟隨市價；輸入實際買價即可固定",
					                  market.c_str());
				}
				ImGui::TableNextColumn();
				if (l.priced) CostRightText(money(l.chaosTotal).c_str(), text);
				else CostRightText(l.tradable ? u8"未估價" : u8"不可交易", dim);
				ImGui::PopID();
			}

			// The map itself: poe.ninja no longer prices regular maps, so it is
			// whatever the player pays -- typed once per project, saved with it.
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			{
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() + iconDy);
				const ImVec2 ip = ImGui::GetCursorScreenPos();
				ImGui::GetWindowDrawList()->AddRectFilled(ip, ImVec2(ip.x + iconSz, ip.y + iconSz), Tok::Surface3,
				                                          PobUi::D(4.0f));
				ImGui::Dummy(ImVec2(iconSz, iconSz));
				ImGui::SameLine(0, PobUi::D(8.0f));
				ImGui::SetCursorPosY(ImGui::GetCursorPosY() - iconDy + textDy);
			}
			std::string mapLabel = u8"地圖";
			if (const AtlasMapDef* md = b.mapId.empty() ? nullptr : mapDb.ById(b.mapId))
				mapLabel += u8"：" + ((showZh && !md->zhItem.empty()) ? md->zhItem : md->enItem);
			if (b.mapTier > 0 && b.mapTier != kMapTierUnique) mapLabel += " T" + std::to_string(b.mapTier);
			ImGui::TextWrapped("%s", mapLabel.c_str());
			const bool mapHover = ImGui::IsItemHovered();
			ImGui::SameLine(0, PobUi::D(6.0f));
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFontSize() - smallPx) * 0.5f);
			PobUi::Hint(u8"自己填");
			if (mapHover || ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"poe.ninja 已無一般地圖報價，請自行輸入單價（混沌石）；0 = 不計入");
			ImGui::TableNextColumn();
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + textDy);
			CostRightText("x1", text);
			ImGui::TableNextColumn();
			double v = b.mapPrice;
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::InputDouble("##mapprice", &v, 0.0, 0.0, "%.1f")) b.mapPrice = v < 0.0 ? 0.0 : v;
			if (ImGui::IsItemDeactivatedAfterEdit()) saveActive();
			ImGui::TableNextColumn();
			if (cost.mapIncluded) CostRightText(money(cost.mapChaos).c_str(), text);
			else CostRightText(u8"—", faint);
			ImGui::EndTable();
		}
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(3);

		// ---- footer: 計畫張數 [ ] ...... 總成本 X
		// The whole plan: the maps the player means to run x the per-map cost.
		{
			ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
			const ImVec2 p = ImGui::GetCursorScreenPos();
			ImGui::GetWindowDrawList()->AddLine(ImVec2(innerX, p.y), ImVec2(innerX + innerW, p.y), Tok::BorderSubtle, 1.0f);
			ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
			const ImVec2 row = ImGui::GetCursorScreenPos();
			const float fh = ImGui::GetFrameHeight();
			ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + std::floor((fh - smallPx) * 0.5f)));
			PobUi::Hint(u8"計畫張數");
			ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMax().x + PobUi::D(10.0f), row.y));
			int planned = b.plannedMaps;
			ImGui::SetNextItemWidth((std::max)(PobUi::D(70.0f), ImGui::CalcTextSize("0000000").x + sty.FramePadding.x * 2.0f));
			if (ImGui::InputInt("##planned", &planned, 0, 0))
				b.plannedMaps = planned < 0 ? 0 : (std::min)(planned, 1000000);
			if (ImGui::IsItemDeactivatedAfterEdit()) saveActive();
			if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"這個方案打算跑幾張圖；0 = 不計算總成本");
			const std::string plan =
			    b.plannedMaps > 0 ? money(cost.totalChaos * (double)b.plannedMaps) : std::string(u8"—");
			const float planW = ImGui::CalcTextSize(plan.c_str()).x;
			const char* planLabel = u8"總成本";
			const float labelW = small->CalcTextSizeA(smallPx, FLT_MAX, 0.0f, planLabel).x;
			const float right = innerX + innerW;
			ImGui::SetCursorScreenPos(ImVec2(right - planW - PobUi::D(8.0f) - labelW, row.y + std::floor((fh - smallPx) * 0.5f)));
			PobUi::Hint(planLabel);
			ImGui::SetCursorScreenPos(ImVec2(right - planW, row.y + std::floor((fh - ImGui::GetFontSize()) * 0.5f)));
			ImGui::TextColored(b.plannedMaps > 0 ? text : faint, "%s", plan.c_str());
			ImGui::SetCursorScreenPos(row);
			ImGui::Dummy(ImVec2(innerW, fh));
		}
		if (!note.empty()) {
			const std::string n = u8"（" + note + u8"）";
			PobUi::Hint(n.c_str());
		}
	}

	// The revenue panel's action row ends with this (design 2026-10-08): bind
	// the interval on screen -- the panel's session start to its newest
	// snapshot, which is what the dialog proposes -- to this project. The 設定
	// page keeps its own entry (renderBindButtons).
	std::string bindBarLabel()
	{
		const AtlasBuildEntry& b = buildFile.Active();
		// A long project name is cut: the row also holds the snapshot button,
		// the session pill and the schedule.
		std::string name = b.name;
		if (name.size() > 30) {
			size_t cut = 0;
			for (size_t i = 0; i < name.size();) {
				const unsigned char c = (unsigned char)name[i];
				const size_t n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
				if (i + n > 27) break;
				i += n;
				cut = i;
			}
			name = name.substr(0, cut) + u8"…";
		}
		return (b.profit.empty() ? std::string(u8"綁定到「") : std::string(u8"重新綁定「")) + name +
		       u8"」##bindtop";
	}
	float bindBarWidth()
	{
		ImFont* small = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
		const float hintW = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, u8"這個區間").x;
		return hintW + PobUi::D(8.0f) +
		       PobUi::ButtonWidth(bindBarLabel().c_str(), PobUi::BtnSize::Md, PobIcon::Link);
	}
	void renderBindBar(const MapCostSummary& cost)
	{
		AtlasBuildEntry& b = buildFile.Active();
		ImFont* small = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float rowH = PobUi::ControlH();
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((rowH - small->FontSize) * 0.5f)));
		PobUi::Hint(u8"這個區間");
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetItemRectMax().x + PobUi::D(8.0f), p.y));
		const bool canBind = !planningMode; // the sandbox never writes the file
		const std::string label = bindBarLabel();
		const bool open = PobUi::Button(label.c_str(), PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, PobIcon::Link,
		                                0.0f, canBind);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			if (!b.profit.empty()) drawProfitTooltip(b.profit);
			else
				PobUi::Tooltip(canBind ? u8"把一段快照區間的收益綁定到這個方案：\n"
				                         u8"存進方案檔，並隨匯出檔與分享碼分享。"
				                       : u8"規劃模式不會寫入方案檔；結束規劃後再綁定。");
		}
		if (open) openBindDialog();
		renderBindDialog(cost);
	}

	static std::string FmtLocalUtc(long long utc)
	{
		const std::time_t t = (std::time_t)utc;
		std::tm tmv{};
		localtime_s(&tmv, &t);
		char buf[32];
		std::strftime(buf, sizeof(buf), "%m/%d %H:%M", &tmv);
		return buf;
	}

	// 收益紀錄: a stretch of the stash history bound to this project. It is part
	// of the build (write_extras), so a shared strategy carries what running it
	// earned, and an imported one shows the sender's figures as they were bound.
	// Buttons only (user layout, 2026-09-12): the record itself is the hover
	// tooltip of the first one.
	void renderBindButtons(const MapCostSummary& cost)
	{
		AtlasBuildEntry& b = buildFile.Active();
		// On the 設定 page among the panel's own settings: say whose record.
		ImGui::TextUnformatted(u8"收益紀錄");
		ImGui::SameLine();
		PobUi::Hint(b.name.c_str());
		bool openBind = false;
		const bool canBind = !planningMode; // the sandbox never writes the file
		if (b.profit.empty()) {
			openBind = PobUi::Button(u8"綁定快照區間", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, canBind);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayNormal))
				ImGui::SetTooltip(u8"把一段快照區間的收益綁定到這個方案：\n"
				                  u8"存進方案檔，並隨匯出檔與分享碼分享。");
		} else {
			// shows the record on hover; a click does nothing
			PobUi::Button(u8"收益紀錄（已綁定）", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, canBind);
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) drawProfitTooltip(b.profit);
			ImGui::SameLine(0, PobUi::D(8.0f));
			openBind = PobUi::Button(u8"重新綁定", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, canBind);
			ImGui::SameLine(0, PobUi::D(8.0f));
			if (PobUi::Button(u8"移除##profit", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, nullptr, 0.0f, canBind)) {
				b.profit = AtlasProfitRecord{};
				saveActive();
			}
		}
		if (openBind) openBindDialog();
		renderBindDialog(cost);
	}

	// The bound record in full, as the hover tooltip of its button.
	void drawProfitTooltip(const AtlasProfitRecord& r)
	{
		const ImVec4 dim = PobUi::TokV4(Tok::TextMuted);
		const ImVec4 good = PobUi::TokV4(Tok::Success);
		const ImVec4 bad = PobUi::TokV4(Tok::Danger);
		const ImGuiStyle& sty = ImGui::GetStyle();
		ImGui::BeginTooltip();
		ImGui::TextColored(PobUi::TokV4(Tok::AccentText), u8"收益紀錄");
		{
			const double rate = r.divineRate;
			auto dc = [rate](double v, bool plus) { return WhFmt::FormatDivChaos(v, rate, plus); };
			const long long mins = (r.toUtc - r.fromUtc) / 60;
			ImGui::TextColored(dim, u8"%s ～ %s · %lldh %02lldm%s%s", FmtLocalUtc(r.fromUtc).c_str(),
			                   FmtLocalUtc(r.toUtc).c_str(), mins / 60, mins % 60,
			                   r.league.empty() ? "" : " · ", r.league.c_str());
			// Farming rate = count changes only, as on the revenue panel's card.
			const double farm = r.hours > 0 ? (r.qtyGain + r.qtyLoss) / r.hours : 0.0;
			ImGui::TextColored(dim, u8"刷圖收益");
			ImGui::SameLine();
			ImGui::TextColored(farm >= 0 ? good : bad, "%s/hr", dc(farm, true).c_str());
			ImGui::SameLine(0, 18.0f * scale);
			ImGui::TextColored(dim, u8"淨值");
			ImGui::SameLine();
			ImGui::TextColored(r.net >= 0 ? good : bad, "%s", dc(r.net, true).c_str());
			ImGui::TextColored(dim, u8"收益");
			ImGui::SameLine();
			ImGui::TextUnformatted(dc(r.qtyGain, false).c_str());
			ImGui::SameLine(0, 14.0f * scale);
			ImGui::TextColored(dim, u8"支出");
			ImGui::SameLine();
			ImGui::TextUnformatted(dc(-r.qtyLoss, false).c_str());
			ImGui::SameLine(0, 14.0f * scale);
			ImGui::TextColored(dim, u8"市價波動");
			ImGui::SameLine();
			ImGui::TextUnformatted(dc(r.priceMove, true).c_str());
			if (r.costPerMap > 0.0) {
				ImGui::TextColored(dim, u8"每張圖成本（綁定時）");
				ImGui::SameLine();
				ImGui::TextUnformatted(WhFmt::FormatValue(r.costPerMap, true, rate).c_str());
			}
			if (r.top.empty()) {
				ImGui::TextColored(dim, u8"這段期間沒有新增的物品");
			} else if (ImGui::BeginTable("##profittop", 3,
			                             ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
			                                 ImGuiTableFlags_SizingFixedFit)) {
				// Fixed columns: a tooltip sizes itself to what it holds.
				ImGui::TableSetupColumn(u8"主要產出", ImGuiTableColumnFlags_WidthFixed);
				ImGui::TableSetupColumn(u8"數量", ImGuiTableColumnFlags_WidthFixed,
				                        ImGui::CalcTextSize("+99999").x + sty.CellPadding.x * 2.0f);
				ImGui::TableSetupColumn(u8"價值", ImGuiTableColumnFlags_WidthFixed,
				                        ImGui::CalcTextSize("9999.9 d").x + sty.CellPadding.x * 2.0f);
				ImGui::TableHeadersRow();
				for (const AtlasProfitItem& t : r.top) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					const bool zh = showZh && !t.zh.empty();
					ImGui::TextUnformatted((zh ? t.zh : t.en).c_str());
					// No hover inside a tooltip: the English name rides along instead.
					if (zh && t.zh != t.en) {
						ImGui::SameLine();
						ImGui::TextColored(dim, "%s", t.en.c_str());
					}
					ImGui::TableNextColumn();
					ImGui::Text("%+lld", t.dCount);
					ImGui::TableNextColumn();
					ImGui::TextColored(good, "%s", WhFmt::FormatValue(t.chaos, true, rate).c_str());
				}
				ImGui::EndTable();
			}
		}
		ImGui::EndTooltip();
	}

	void openBindDialog()
	{
		// The league the revenue panel below is set to; only its file is read.
		bindLeague_ = costLeague_;
		bindHist_.Load(exeDir, "poe1", bindLeague_);
		// Defaults: the panel's session start -> the newest snapshot, both among
		// snapshots that kept their lines (a summary cannot be diffed).
		bindFromUtc_ = bindToUtc_ = 0;
		const Snapshot* start = bindHist_.FindByUtc(bindHist_.sessionStartUtc);
		if (start && !start->summary) bindFromUtc_ = start->utc;
		for (const Snapshot& s : bindHist_.snaps) {
			if (s.summary) continue;
			if (!bindFromUtc_) bindFromUtc_ = s.utc;
			bindToUtc_ = s.utc;
		}
		if (!bindI18nLoaded_) {
			std::string loc; // a locale id ("zh-rTW") is ASCII
			for (wchar_t c : host_->locale) loc += (c > 0 && c < 128) ? (char)c : '?';
			bindI18n_.Load(exeDir, loc);
			bindI18nLoaded_ = true;
		}
		ImGui::OpenPopup(u8"綁定快照區間##bind");
	}

	void renderBindDialog(const MapCostSummary& cost)
	{
		if (!ImGui::BeginPopupModal(u8"綁定快照區間##bind", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;
		const ImVec4 dim = PobUi::TokV4(Tok::TextMuted);
		std::vector<const Snapshot*> full; // only these diff item by item
		for (const Snapshot& s : bindHist_.snaps)
			if (!s.summary) full.push_back(&s);
		ImGui::TextColored(dim, u8"聯盟：%s", bindLeague_.empty() ? u8"（未設定）" : bindLeague_.c_str());
		if (full.size() < 2) {
			ImGui::TextUnformatted(u8"這個聯盟的完整快照不足兩份，請先在下方「倉庫收益」拍快照。");
			if (PobUi::Button(u8"關閉", PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, nullptr, PobUi::D(88.0f)))
				ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}
		auto label = [](const Snapshot& s) {
			return FmtLocalUtc(s.utc) + "   " + WhFmt::FormatValue(s.totalChaos, true, s.divineRate);
		};
		auto pick = [&](const char* name, long long* utc) {
			const Snapshot* cur = bindHist_.FindByUtc(*utc);
			ImGui::SetNextItemWidth(280.0f * scale);
			if (ImGui::BeginCombo(name, cur ? label(*cur).c_str() : "")) {
				for (auto it = full.rbegin(); it != full.rend(); ++it) { // newest first
					const bool on = (*it)->utc == *utc;
					if (ImGui::Selectable((label(**it) + "##" + std::to_string((*it)->utc)).c_str(), on))
						*utc = (*it)->utc;
					if (on) ImGui::SetItemDefaultFocus();
				}
				ImGui::EndCombo();
			}
		};
		pick(u8"起點", &bindFromUtc_);
		pick(u8"終點", &bindToUtc_);

		const Snapshot* from = bindHist_.FindByUtc(bindFromUtc_);
		const Snapshot* to = bindHist_.FindByUtc(bindToUtc_);
		AtlasProfitRecord rec;
		if (from && to)
			rec = MakeProfitRecord(
			    *from, *to, [this](const std::string& en) { return bindI18n_.DisplayName(en); },
			    cost.totalChaos);
		ImGui::Separator();
		if (rec.empty()) {
			ImGui::TextColored(PobUi::TokV4(Tok::Danger), u8"終點必須晚於起點");
		} else {
			const double rate = rec.divineRate;
			const double farm = rec.hours > 0 ? (rec.qtyGain + rec.qtyLoss) / rec.hours : 0.0;
			ImGui::Text(u8"經過 %.1f 小時 · 刷圖收益 %s/hr · 淨值 %s", rec.hours,
			            WhFmt::FormatDivChaos(farm, rate, true).c_str(),
			            WhFmt::FormatDivChaos(rec.net, rate, true).c_str());
			ImGui::TextColored(dim, u8"收益 %s · 支出 %s · 市價波動 %s",
			                   WhFmt::FormatDivChaos(rec.qtyGain, rate, false).c_str(),
			                   WhFmt::FormatDivChaos(-rec.qtyLoss, rate, false).c_str(),
			                   WhFmt::FormatDivChaos(rec.priceMove, rate, true).c_str());
			if (!rec.top.empty()) {
				std::string names;
				for (size_t i = 0; i < rec.top.size() && i < 3; i++) {
					if (!names.empty()) names += u8"、";
					names += (showZh && !rec.top[i].zh.empty()) ? rec.top[i].zh : rec.top[i].en;
				}
				ImGui::TextColored(dim, u8"主要產出：%s", names.c_str());
			}
			if (rec.costPerMap > 0.0)
				ImGui::TextColored(dim, u8"每張圖成本記為目前成本卡合計 %s",
				                   WhFmt::FormatValue(rec.costPerMap, true, rate).c_str());
			ImGui::TextColored(dim, u8"綁定後存進方案檔，並隨匯出檔與分享碼分享。");
		}
		ImGui::Separator();
		if (PobUi::Button(u8"取消", PobUi::BtnKind::Secondary, PobUi::BtnSize::Md, nullptr, PobUi::D(88.0f)))
			ImGui::CloseCurrentPopup();
		ImGui::SameLine(0, PobUi::D(8.0f));
		if (PobUi::Button(u8"綁定", PobUi::BtnKind::Primary, PobUi::BtnSize::Md, nullptr, PobUi::D(88.0f), !rec.empty())) {
			buildFile.Active().profit = rec;
			saveActive();
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}

	void saveActive()
	{
		if (planningMode) return;   // sandbox: never touch the file
		if (onCanonicalSeason()) {
			buildFile.Active().alloc = tree.AllocIds();
			buildFile.Active().targets = tree.TargetIds();
			buildFile.Active().blocked = tree.BlockedIds();
			buildFile.version = tree.Version();
		}
		buildFile.Save(exeDir);
	}

	AtlasUndo undo, frameStart;
	void snapshot(AtlasUndo& u)
	{
		u.valid = true;
		u.alloc = tree.AllocIds();
		u.targets = tree.TargetIds();
		u.blocked = tree.BlockedIds();
	}

	// --- sandbox planning mode ---
	// Entering starts from a blank slate so several core nodes can be marked
	// quickly; leaving asks whether to keep the result. NOTHING is written to
	// the build file while planning, so abandoning a session is guaranteed to
	// leave the project byte-identical -- that is the whole point of the mode.
	bool planningAskExit = false;      // the save/discard prompt is up
	AtlasUndo planSnapshot;            // state captured on entry
	void enterPlanning()
	{
		snapshot(planSnapshot);
		tree.Reset();                  // clears alloc, targets and blocked
		planningMode = true;
		planningAskExit = false;
	}
	void restorePlanning()
	{
		tree.ApplyAllocIds(planSnapshot.alloc);
		tree.ApplyTargetIds(planSnapshot.targets);
		tree.ApplyBlockedIds(planSnapshot.blocked);
		planningMode = false;
		planningAskExit = false;
		undo.valid = false;            // undo does not straddle the sandbox
	}
	void keepPlanning()
	{
		planningMode = false;
		planningAskExit = false;
		undo = planSnapshot;           // one step back = "before I started planning"
		saveActive();
	}

	// On-demand minimum-point compression.
	//
	// This used to run inside every click: the allocation was re-derived from the
	// target set each time, which is minimal but re-routes paths the user already
	// walked -- clicking a second node visibly scrambles the first one's route,
	// and it compounds. Clicking is now plain shortest-path (atlas_view.cpp), and
	// minimality is a button you press when you want it.
	//
	// What gets pinned is deliberately conservative: the user's own picks UNION
	// every notable / keystone / leaf (AtlasInferTargets). So compression can
	// only ever delete redundant wiring -- it can never cost you a big node or an
	// endpoint, which is the failure a one-way "make it smaller" button must not
	// have. Undo covers it either way.
	int compressFrom = 0, compressTo = 0;
	std::vector<int> compressTargets()
	{
		std::vector<int> t = tree.TargetIdx();
		for (int i : AtlasInferTargets(tree))
			if (std::find(t.begin(), t.end(), i) == t.end()) t.push_back(i);
		return t;
	}
	// Solved only when the button is actually pressed -- a Steiner solve is far
	// too heavy to run once per frame just to grey out a button, and "press it
	// and be told it is already minimal" is a perfectly good answer.
	bool applyCompress(std::string& msg)
	{
		std::vector<int> targets = compressTargets();
		AtlasPlan p = AtlasPlanMinimal(tree, targets, tree.BlockedIdx(), tree.AllocIdx());
		if (!p.ok()) {
			msg = u8"有節點被「不要」的標記擋住，連不上，無法壓縮";
			return false;
		}
		if (p.points >= tree.UsedPoints()) {
			msg = u8"已經是最少點的接法了（" + std::to_string(tree.UsedPoints()) + u8" 點）";
			return false;
		}
		snapshot(undo);
		compressFrom = tree.UsedPoints();
		compressTo = p.points;
		tree.SetAllocSet(p.nodes);
		for (AtlasNode& nd : tree.nodes) nd.target = false;
		for (int t : targets)
			if (t >= 0 && t < (int)tree.nodes.size()) tree.nodes[t].target = true;
		saveActive();
		msg = u8"已壓縮：" + std::to_string(compressFrom) + u8" 點減為 " + std::to_string(compressTo) +
		      u8" 點（Ctrl+Z 可復原）";
		if (!p.exact) msg += u8"（近似解）";
		return true;
	}

	// --- league-mechanic overlay ---------------------------------------------
	// "Where else is 裂痕?" -- clicking a cluster's mastery icon, or a row in the
	// side panel, rings every node of that mechanic across the whole atlas. The
	// per-season map is written next to the tree by the importer/updater; when a
	// season predates the feature the db borrows the newest one it can find and
	// says so. Purely a view: it never touches the allocation.
	AtlasMechanicDb mechDb;
	std::string mechSel;                                   // selected mechanic id ("" = none)
	std::vector<std::pair<std::string, std::vector<int>>> mechNodeIdx;   // id -> node indices
	std::vector<std::pair<std::string, std::vector<int>>> mechMastIdx;   // id -> mastery indices
	static const std::vector<int>* mechFind(const std::vector<std::pair<std::string, std::vector<int>>>& v,
	                                       const std::string& id)
	{
		for (const auto& kv : v)
			if (kv.first == id) return &kv.second;
		return nullptr;
	}
	void applyMechHighlight()
	{
		const std::vector<int>* n = mechSel.empty() ? nullptr : mechFind(mechNodeIdx, mechSel);
		const std::vector<int>* m = mechSel.empty() ? nullptr : mechFind(mechMastIdx, mechSel);
		if (!n && !m) { view.ClearMechanicHighlight(); return; }
		view.SetMechanicHighlight(n ? *n : std::vector<int>(), m ? *m : std::vector<int>());
	}
	// Resolve the season's mechanic map onto THIS tree's node indices. Ids that
	// the season does not have simply do not resolve; nothing is invented.
	void reloadMechanics()
	{
		mechNodeIdx.clear();
		mechMastIdx.clear();
		mechDb.Load(exeDir, viewTag);
		std::unordered_map<int, int> idxById;
		for (int i = 0; i < (int)tree.nodes.size(); i++) idxById[tree.nodes[i].id] = i;
		for (const AtlasMechanicDb::Entry& e : mechDb.Entries()) {
			std::vector<int> idx;
			for (int id : e.nodeIds) {
				auto it = idxById.find(id);
				if (it != idxById.end()) idx.push_back(it->second);
			}
			if (!idx.empty()) mechNodeIdx.emplace_back(e.def->id, std::move(idx));
		}
		// Mastery icons carry the English mechanic name, which is the catalogue's
		// join key -- the same key the generator used, so this cannot drift.
		std::vector<std::string> labels(tree.masteries.size());
		for (int i = 0; i < (int)tree.masteries.size(); i++) {
			const AtlasMechanicDef* d = AtlasMechanicByEn(tree.masteries[i].name);
			if (!d) continue;
			labels[i] = d->zh;
			bool found = false;
			for (auto& kv : mechMastIdx)
				if (kv.first == d->id) { kv.second.push_back(i); found = true; break; }
			if (!found) mechMastIdx.emplace_back(d->id, std::vector<int>{ i });
		}
		view.SetMasteryLabels(std::move(labels));
		if (!mechSel.empty() && !mechFind(mechNodeIdx, mechSel)) mechSel.clear();
		applyMechHighlight();
	}

	// --- zh display layer + background auto updater ---
	AtlasI18n i18n;
	bool zhLoaded = false;
	bool showZh = false;                 // set after the first season load
	AtlasUpdater updater;

	// (Re)load a season's tree + zh + textures onto the canvas, re-apply the
	// build (by GGG id), and backfill Chinese for value-only changes from the
	// previous season (same wording, adjusted number -> reuse old zh with the new
	// value; changed wording -> keep the new English).
	void loadSeason(const std::string& tag)
	{
		viewTag = tag;
		startupDropped = 0;
		ready = tree.LoadVersion(exeDir, tag, &loadErr);
		if (!ready) {
			// The season's tree is the planner: without it the whole tab is a
			// message. Nothing else records which season failed or why.
			PobLog::Error("data", "atlas season " + tag + " failed to load: " + loadErr);
		}
		if (ready) {
			int mapped = tree.ApplyAllocIds(buildFile.Active().alloc);
			tree.ApplyTargetIds(buildFile.Active().targets);   // must follow ApplyAllocIds
			tree.ApplyBlockedIds(buildFile.Active().blocked);
			if (!buildFile.version.empty() && buildFile.version != tree.Version())
				startupDropped = (int)buildFile.Active().alloc.size() - mapped;
			zhLoaded = i18n.LoadVersion(exeDir, tag);
			std::string older = verIndex.OlderThan(tag);
			if (zhLoaded && !older.empty()) {
				AtlasTreeData ot;
				AtlasI18n oi;
				std::string e;
				if (ot.LoadVersion(exeDir, older, &e) && oi.LoadVersion(exeDir, older)) {
					// zh built from a repoe older than the season: whatever got
					// paired to this season's NEW/CHANGED lines is a translation
					// of the old wording -> drop those (fall back to English) and
					// the stale names of renamed nodes. Unchanged lines keep zh.
					bool zhLags = !i18n.RepoeVersion().empty() &&
					              AtlasVersionIndex::CompareSemver(i18n.RepoeVersion(), tag) < 0;
					if (zhLags) {
						PruneStaleTranslations(i18n, tree, ot);
						DropRenamedNames(i18n, tree, ot);
					}
					BackfillAtlasI18n(i18n, tree, oi, ot); // unchanged lines repoe missed
				}
			}
			ready = view.LoadTextures(exeDir, tree, &loadErr);
			reloadMechanics();
		}
	}

	// initial load of the chosen season

	// --- version-compare state ---
	// The pair being compared is USER-CHOSEN, not fixed to compareBase -> active:
	// with every revision of the current league retained (3.29.0 alongside
	// 3.29.1), "what did GGG change mid-league?" is a question about two
	// revisions of the same league, which a fixed previous-league base could
	// never answer. Defaults to compareBase -> active.
	bool compareMode = false;
	AtlasTreeDiff diff;
	bool diffReady = false;
	std::string diffErr;
	char diffSearch[256] = "";
	std::string cmpBase, cmpTarg;               // chosen seasons; empty = use the defaults
	std::unordered_map<int, int> activeIdxById; // GGG id -> displayed-tree node index

	// --- right-hand summary panel ---
	// Stat rows are value-aggregated (atlas_stat_agg); nodes are grouped by
	// kind. Everything display-related (en/zh strings, both sort orders, the
	// search keys) is cached here so F2 language flips never rebuild.
	std::vector<StatAggGroup> statAgg;
	std::vector<int> statOrderEn, statOrderZh;
	std::vector<PanelNode> nodeGroups[4]; // keystone / wormhole / notable / small
	char panelSearch[256] = "";
	bool panelDirty = true;
	void rebuildPanel()
	{
		statAgg.clear();
		statOrderEn.clear();
		statOrderZh.clear();
		for (auto& g : nodeGroups) g.clear();
		auto rank = [](int kind) {
			return kind == kAtlasKeystone ? 0 : kind == kAtlasWormhole ? 1 : kind == kAtlasNotable ? 2 : 3;
		};
		std::unordered_map<std::string, size_t> pos;
		for (int i = 0; i < (int)tree.nodes.size(); i++) {
			const AtlasNode& n = tree.nodes[i];
			if (!n.alloc || n.kind == kAtlasStart) continue;
			const std::string& zhName = zhLoaded ? i18n.NodeName(n.id, n.name) : n.name;
			nodeGroups[rank(n.kind)].push_back({ i, ToLowerAscii(n.name + "\n" + zhName) });
			for (const std::string& s : n.stats)
				AccumulateStatLine(s, statAgg, pos);
		}
		std::function<std::string(const std::string&)> zhFn =
			[&](const std::string& en) { return i18n.StatLine(en); };
		BuildStatAggDisplay(statAgg, zhLoaded ? &zhFn : nullptr);
		statOrderEn.resize(statAgg.size());
		std::iota(statOrderEn.begin(), statOrderEn.end(), 0);
		statOrderZh = statOrderEn;
		std::sort(statOrderEn.begin(), statOrderEn.end(), [&](int a, int b) {
			return statAgg[a].dispEn != statAgg[b].dispEn ? statAgg[a].dispEn < statAgg[b].dispEn : a < b;
		});
		std::sort(statOrderZh.begin(), statOrderZh.end(), [&](int a, int b) {
			return statAgg[a].dispZh != statAgg[b].dispZh ? statAgg[a].dispZh < statAgg[b].dispZh : a < b;
		});
		for (auto& g : nodeGroups)
			std::sort(g.begin(), g.end(), [&](const PanelNode& a, const PanelNode& b) {
				return tree.nodes[a.idx].name < tree.nodes[b.idx].name;
			});
	}

	// --- astrolabe section (one Shaped Region per atlas quadrant) ---
	// Above the scarabs on purpose: an astrolabe covers a whole quadrant, a
	// scarab covers the one map you are about to open.
	char astroSearch[256] = "";
	const std::vector<std::string>& astroLines(const AstrolabeDef& d)
	{
		// One list or the other, never a line from each.
		return (showZh && !d.descZh.empty()) ? d.descZh : d.descEn;
	}
	const std::string& astroName(const AstrolabeDef& d)
	{
		return (showZh && !d.zh.empty()) ? d.zh : d.en;
	}
	// The compass label is OURS. AtlasRegions ships an Id and no name column, so
	// the only official Traditional Chinese string for a quadrant is its Memory
	// Vault's area name — shown in parentheses so the invented part and the
	// official part stay visibly separate. (Compare the scarab families, where
	// no Chinese name was invented at all; the difference is that a quadrant has
	// to be addressable here, so it needs some label.)
	std::string quadrantLabel(const AtlasQuadrant& q)
	{
		std::string compass = q.id;
		if (showZh) {
			if (q.id == "NorthWest") compass = u8"西北";
			else if (q.id == "NorthEast") compass = u8"東北";
			else if (q.id == "SouthEast") compass = u8"東南";
			else if (q.id == "SouthWest") compass = u8"西南";
		}
		const std::string& vault = (showZh && !q.vaultZh.empty()) ? q.vaultZh : q.vaultEn;
		return vault.empty() ? compass : compass + u8"（" + vault + u8"）";
	}

	// Short compass name for a slot (the full label, with the vault's official
	// name, is in the tooltip and the list under the slots).
	std::string quadrantShort(const AtlasQuadrant& q)
	{
		if (!showZh) {
			if (q.id == "NorthWest") return "NW";
			if (q.id == "NorthEast") return "NE";
			if (q.id == "SouthEast") return "SE";
			if (q.id == "SouthWest") return "SW";
			return q.id;
		}
		if (q.id == "NorthWest") return u8"西北";
		if (q.id == "NorthEast") return u8"東北";
		if (q.id == "SouthEast") return u8"東南";
		if (q.id == "SouthWest") return u8"西南";
		return q.id;
	}

	// The slot row's actions are raised in the panel and acted on outside it
	// (renderAstroPicker / renderSlotMenus): a popup submitted inside the
	// scrolling child is clipped to it.
	std::string astroPickRegion_;     // quadrant the picker places into
	bool astroPickReplace_ = false;   // true: the pick replaces what is there
	bool openAstroPicker_ = false;
	std::string astroMenuRegion_;     // quadrant whose filled slot was clicked
	bool openAstroMenu_ = false;

	void renderAstrolabePanel()
	{
		AtlasBuildEntry& b = buildFile.Active();
		if (!astroDb.available()) {
			if (PobUi::CollapsingSection(u8"星盤", "###astrohdr", PobIcon::Compass, nullptr, false)) {
				ImGui::TextWrapped(u8"星盤資料未載入：%s", astroErr.c_str());
				PobUi::Hint(u8"已存的星盤設定不會被更動。");
			}
			return;
		}
		const std::string hdr = u8"星盤  " + std::to_string(b.astrolabes.size()) + " / " +
		                        std::to_string(astroDb.Regions().size());
		if (!PobUi::CollapsingSection(hdr.c_str(), "###astrohdr", PobIcon::Compass, u8"每個半區同時只能有一片幻塑界域"))
			return;

		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		const float slot = std::floor(PobUi::D(44.0f));
		int regionIdx = 0;
		bool anyPlaced = false;
		for (const AtlasQuadrant& q : astroDb.Regions()) {
			ImGui::PushID(regionIdx);
			if (regionIdx++) ImGui::SameLine(0, PobUi::D(10.0f));
			const AstrolabePlacement* placed = nullptr;
			for (const AstrolabePlacement& p : b.astrolabes)
				if (p.region == q.id) { placed = &p; break; }
			const AstrolabeDef* def = placed ? astroDb.ById(placed->id) : nullptr;
			unsigned tex = 0;
			if (def) {
				icons.RequestPath(def->art);
				tex = icons.TextureByPath(def->art);
			}
			const std::string shortLbl = quadrantShort(q);
			if (PobUi::Slot("##slot", tex, shortLbl.c_str(), placed != nullptr, slot)) {
				if (placed) {
					astroMenuRegion_ = q.id;
					openAstroMenu_ = true;
				} else {
					astroSearch[0] = '\0';
					astroPickRegion_ = q.id;
					astroPickReplace_ = false;
					openAstroPicker_ = true;
				}
			}
			if (placed) anyPlaced = true;
			if (ImGui::IsItemHovered()) {
				std::string tip = quadrantLabel(q) + "\n";
				if (def) tip += astroName(*def) + u8"\n點擊可替換或移除";
				else if (placed) tip += u8"未知星盤\n點擊可替換或移除";
				else tip += u8"未配置 · 點擊放置星盤";
				PobUi::Tooltip(tip.c_str());
			}
			ImGui::PopID();
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		if (anyPlaced) PobUi::Hint(u8"點已放的格子可替換或移除");
		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));

		// what each placed one does, quadrant by quadrant
		for (const AtlasQuadrant& q : astroDb.Regions()) {
			const AstrolabePlacement* placed = nullptr;
			for (const AstrolabePlacement& p : b.astrolabes)
				if (p.region == q.id) { placed = &p; break; }
			if (!placed) continue;
			const AstrolabeDef* def = astroDb.ById(placed->id);
			PobUi::Hint(quadrantLabel(q).c_str());
			if (!def) {
				// Sanitize should have removed this; say so instead of drawing a blank.
				ImGui::TextColored(PobUi::TokV4(Tok::TextMuted), u8"未知星盤");
				continue;
			}
			ImGui::TextColored(PobUi::TokV4(Tok::AccentText), "%s", astroName(*def).c_str());
			ImGui::Indent(PobUi::D(10.0f));
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::TextMuted));
			for (const std::string& s : astroLines(*def))
				ImGui::TextWrapped("%s", StripStatMarkup(s).c_str());
			ImGui::PopStyleColor();
			if (!def->enabled) PobUi::Hint(u8"（本賽季尚未啟用）", 0.0f, Tok::Warning);
			ImGui::Unindent(PobUi::D(10.0f));
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	}

	// Submitted OUTSIDE the scrolling panel (see renderScarabPicker).
	void renderAstroPicker()
	{
		if (!astroDb.available()) return;
		AtlasBuildEntry& b = buildFile.Active();
		if (openAstroPicker_) {
			ImGui::OpenPopup(u8"選擇星盤");
			openAstroPicker_ = false;
		}
		const AtlasQuadrant* q = astroDb.RegionById(astroPickRegion_);
		ImGui::PushStyleColor(ImGuiCol_PopupBg, PobUi::TokV4(Tok::SurfaceRaised));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, PobUi::D(8.0f));
		if (ImGui::BeginPopup(u8"選擇星盤")) {
			PobUi::Hint(((q ? quadrantLabel(*q) : astroPickRegion_) +
			             (astroPickReplace_ ? u8" · 替換目前的星盤" : "")).c_str());
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			PobUi::SearchField("##astrosearch", astroSearch, (int)sizeof(astroSearch),
			                   u8"搜尋名稱或效果（中英、模糊）…", std::floor(PobUi::D(320.0f)));
			FuzzyQuery q2 = MakeFuzzyQuery(astroSearch);

			std::vector<std::pair<int, const AstrolabeDef*>> hits;
			for (const AstrolabeDef& d : astroDb.All()) {
				int s = astroDb.MatchScore(d, q2);
				if (s > 0) hits.push_back({ s, &d });
			}
			if (!q2.empty())
				std::stable_sort(hits.begin(), hits.end(),
					[](const auto& x, const auto& y) {
						if (x.first != y.first) return x.first > y.first;
						return x.second->zh.size() < y.second->zh.size();
					});
			PobUi::Hint((std::to_string(hits.size()) + u8" 種 · " + astroDb.Source()).c_str());

			// Replacing = this quadrant emptied first; CanPlace (the one rule) then
			// decides on that list, and nothing is written when it refuses.
			std::vector<AstrolabePlacement> base = b.astrolabes;
			for (size_t i = 0; i < base.size(); i++)
				if (base[i].region == astroPickRegion_) { base.erase(base.begin() + (ptrdiff_t)i); break; }

			ImGui::BeginChild("##astrolist", ImVec2(420.0f * scale, 300.0f * scale));
			for (size_t k = 0; k < hits.size(); k++) {
				const AstrolabeDef& d = *hits[k].second;
				ImGui::PushID((int)k);
				icons.RequestPath(d.art);
				unsigned tex = icons.TextureByPath(d.art);
				float sz = ImGui::GetTextLineHeight() * 1.4f;
				if (tex) ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(sz, sz));
				else     ImGui::Dummy(ImVec2(sz, sz));
				ImGui::SameLine();
				if (ImGui::Selectable(astroName(d).c_str())) {
					if (astroDb.CanPlace(base, astroPickRegion_, d.id).ok()) {
						base.push_back({ astroPickRegion_, d.id });
						b.astrolabes = base;
						saveActive();
					} else {
						PobUi::ShowToast(u8"這個半區放不下這片星盤，維持原本的設定", PobUi::Tone::Warn);
					}
					ImGui::CloseCurrentPopup();
				}
				if (ImGui::IsItemHovered()) {
					ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(14.0f), PobUi::D(10.0f)));
					ImGui::BeginTooltip();
					ImGui::TextColored(PobUi::TokV4(Tok::AccentText), "%s", astroName(d).c_str());
					ImGui::PushTextWrapPos(360.0f * scale);
					for (const std::string& s : astroLines(d))
						ImGui::TextUnformatted(StripStatMarkup(s).c_str());
					ImGui::PopTextWrapPos();
					if (!d.enabled) PobUi::Hint(u8"（本賽季尚未啟用，交易站也沒有）", 0.0f, Tok::Warning);
					ImGui::EndTooltip();
					ImGui::PopStyleVar();
				}
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::EndPopup();
		}
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}

	// The menus a FILLED slot opens: replace (the same picker, in replace mode)
	// or remove. Outside every child, like the pickers.
	void renderSlotMenus()
	{
		AtlasBuildEntry& b = buildFile.Active();
		if (openAstroMenu_) {
			ImGui::OpenPopup("##astroslotmenu");
			openAstroMenu_ = false;
		}
		if (PobUi::BeginMenuPopup("##astroslotmenu")) {
			const AtlasQuadrant* q = astroDb.RegionById(astroMenuRegion_);
			PobUi::Hint((q ? quadrantLabel(*q) : astroMenuRegion_).c_str());
			if (PobUi::MenuRow(PobIcon::Refresh, u8"替換…")) {
				astroSearch[0] = '\0';
				astroPickRegion_ = astroMenuRegion_;
				astroPickReplace_ = true;
				openAstroPicker_ = true;
			}
			if (PobUi::MenuRow(PobIcon::Trash, u8"移除", nullptr, true, true)) {
				for (size_t i = 0; i < b.astrolabes.size(); i++)
					if (b.astrolabes[i].region == astroMenuRegion_) {
						b.astrolabes.erase(b.astrolabes.begin() + (ptrdiff_t)i);
						break;
					}
				saveActive();
			}
			PobUi::EndMenuPopup();
		}

		if (openScarabMenu_) {
			ImGui::OpenPopup("##scarabslotmenu");
			openScarabMenu_ = false;
		}
		if (PobUi::BeginMenuPopup("##scarabslotmenu")) {
			const bool valid = scarabMenuIdx_ >= 0 && scarabMenuIdx_ < (int)b.scarabs.size();
			const ScarabDef* d = valid ? scarabDb.ById(b.scarabs[scarabMenuIdx_]) : nullptr;
			PobUi::Hint(d ? scarabName(*d).c_str() : u8"地圖格");
			if (PobUi::MenuRow(PobIcon::Refresh, u8"替換…", nullptr, valid)) {
				scarabSearch[0] = '\0';
				scarabReplaceIdx_ = scarabMenuIdx_;
				openScarabPicker = true;
			}
			if (PobUi::MenuRow(PobIcon::Trash, u8"移除", nullptr, valid, true)) {
				b.scarabs.erase(b.scarabs.begin() + scarabMenuIdx_);
				saveActive();
			}
			PobUi::EndMenuPopup();
		}
	}

	// --- main map section (one per project) ---
	char mapSearch[256] = "";
	// The two dropdowns are INDEPENDENT, and deliberately not modelled on the
	// game's own data: once a quadrant's Voidstone is socketed, every map in that
	// quadrant becomes T16, so a map's shipped tier says nothing about the tier
	// it will actually be run at. Filtering the name list by it would hide maps
	// the user can legitimately pick. So the tier dropdown records the PLAN and
	// the name dropdown always offers every map.
	const int kMapTierUnique = 99;
	const std::string& mapPrimaryName(const AtlasMapDef& d)
	{
		// The atlas shows the AREA name, so that is what the planner leads with.
		return showZh ? d.zhArea : d.enArea;
	}
	const std::string& mapSecondName(const AtlasMapDef& d)
	{
		return showZh ? d.zhItem : d.enItem;
	}
	std::string mapRegionLabel(const std::string& regionId)
	{
		const AtlasQuadrant* q = astroDb.RegionById(regionId);
		return q ? quadrantLabel(*q) : regionId;
	}
	// A map's own tier, shown only as a hint in the tooltip — never as the label,
	// so it cannot be mistaken for the tier the project plans to run.
	std::string mapOwnTierLabel(const AtlasMapDef& d)
	{
		if (d.kind == AtlasMapDef::kUnique) return std::string(showZh ? u8"傳奇" : "unique");
		return d.tier > 0 ? "T" + std::to_string(d.tier) : std::string("-");
	}
	std::string mapTierLabel(int t)
	{
		if (t == 0) return std::string(showZh ? u8"未指定" : "unset");
		if (t == kMapTierUnique) return std::string(showZh ? u8"傳奇圖" : "unique");
		return "T" + std::to_string(t);
	}
	// Endgame is almost entirely run at the top tier, so an unset project shows
	// that as its starting point. Only a deliberate pick is written to the file:
	// this is display-only until the user touches something.
	int plannedTier()
	{
		int t = buildFile.Active().mapTier;
		if (t > 0) return t;
		return mapDb.TiersPresent().empty() ? 0 : mapDb.TiersPresent().back();
	}

	void renderMapPanel()
	{
		AtlasBuildEntry& b = buildFile.Active();
		if (!mapDb.available()) {
			if (PobUi::CollapsingSection(u8"主力地圖", "###mainmaphdr", PobIcon::Map, nullptr, false)) {
				ImGui::TextWrapped(u8"地圖資料未載入：%s", mapErr.c_str());
				PobUi::Hint(u8"已存的地圖設定不會被更動。");
			}
			return;
		}
		const AtlasMapDef* cur = b.mapId.empty() ? nullptr : mapDb.ById(b.mapId);
		const std::string note = cur ? mapTierLabel(plannedTier()) + u8" · " + mapPrimaryName(*cur)
		                             : std::string(u8"尚未選擇");
		if (!PobUi::CollapsingSection(u8"主力地圖", "###mainmaphdr", PobIcon::Map, note.c_str())) return;

		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		if (cur) {
			const float sz = std::floor(PobUi::D(32.0f));
			icons.RequestPath(cur->art);
			unsigned tex = icons.TextureByPath(cur->art);
			if (tex) {
				ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(sz, sz));
				ImGui::SameLine();
			}
			ImGui::BeginGroup();
			// The PLANNED tier leads, then the name -- two separate facts, and the
			// eye should not have to split a sentence to read them.
			ImGui::TextColored(PobUi::TokV4(Tok::TreeLinkOn), "%s", mapTierLabel(plannedTier()).c_str());
			ImGui::SameLine(0, PobUi::D(8.0f));
			ImGui::TextColored(PobUi::TokV4(Tok::AccentText), "%s", mapPrimaryName(*cur).c_str());
			std::string sub;
			const std::string& item = mapSecondName(*cur);
			if (!item.empty() && item != mapPrimaryName(*cur)) sub = item + u8" · ";
			sub += mapRegionLabel(cur->region);
			PobUi::Hint(sub.c_str());
			ImGui::EndGroup();
		}

		// --- two INDEPENDENT dropdowns: the tier to run at, and which map ---
		PobUi::PushControlFrame();
		ImGui::PushStyleColor(ImGuiCol_PopupBg, PobUi::TokV4(Tok::SurfaceRaised));
		ImGui::AlignTextToFramePadding();
		PobUi::Hint(u8"階級");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(std::floor(PobUi::D(96.0f)));
		if (ImGui::BeginCombo("##maptier", mapTierLabel(plannedTier()).c_str())) {
			// Only tiers this season actually ships get an entry (AtlasMapDb
			// derives the list from the data, so a season with a different tier
			// range needs no code change).
			for (int t : mapDb.TiersPresent())
				if (ImGui::Selectable(mapTierLabel(t).c_str(), plannedTier() == t)) {
					b.mapTier = t;
					saveActive();
				}
			if (ImGui::Selectable(mapTierLabel(kMapTierUnique).c_str(),
			                      plannedTier() == kMapTierUnique)) {
				b.mapTier = kMapTierUnique;
				saveActive();
			}
			ImGui::EndCombo();
		}

		ImGui::SameLine();
		PobUi::Hint(u8"地圖");
		ImGui::SameLine();
		const float clearW = cur ? PobUi::ButtonWidth(u8"清除##map", PobUi::BtnSize::Sm) + PobUi::D(8.0f) : 0.0f;
		ImGui::SetNextItemWidth((std::max)(PobUi::D(80.0f), ImGui::GetContentRegionAvail().x - clearW));
		const char* namePreview = cur ? mapPrimaryName(*cur).c_str() : u8"選擇地圖…";
		if (ImGui::BeginCombo("##mapname", namePreview, ImGuiComboFlags_HeightLarge)) {
			// EVERY map, always. The tier dropdown does not filter this list:
			// a Voidstone lifts a whole quadrant to T16, so a map's shipped tier
			// is no reason to hide it from a T16 plan.
			if (ImGui::IsWindowAppearing()) {
				mapSearch[0] = '\0';
				ImGui::SetKeyboardFocusHere();
			}
			PobUi::SearchField("##mapsearch", mapSearch, (int)sizeof(mapSearch), u8"搜尋地圖名稱…", -FLT_MIN);
			FuzzyQuery q = MakeFuzzyQuery(mapSearch);

			std::vector<std::pair<int, const AtlasMapDef*>> hits;
			for (const AtlasMapDef& d : mapDb.All()) {
				int s = mapDb.MatchScore(d, q);
				if (s > 0) hits.push_back({ s, &d });
			}
			// With no query the natural order is by quadrant then tier, which is
			// how someone reads the atlas; a query ranks by match quality.
			if (q.empty())
				std::stable_sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) {
					if (x.second->region != y.second->region) return x.second->region < y.second->region;
					if (x.second->tier != y.second->tier) return x.second->tier < y.second->tier;
					return x.second->enArea < y.second->enArea;
				});
			else
				std::stable_sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) {
					if (x.first != y.first) return x.first > y.first;
					return x.second->enArea.size() < y.second->enArea.size();
				});

			PobUi::Hint((std::to_string(hits.size()) + u8" 張").c_str());
			ImGui::BeginChild("##maplist", ImVec2(340.0f * scale, 320.0f * scale));
			ImGuiListClipper clip;
			clip.Begin((int)hits.size());
			while (clip.Step()) {
				for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
					const AtlasMapDef& d = *hits[k].second;
					ImGui::PushID(k);
					// The name alone. The map's own tier goes in the tooltip, not
					// the label -- showing it beside a planned tier of T16 would
					// read as a contradiction rather than as extra information.
					if (ImGui::Selectable(mapPrimaryName(d).c_str(), d.id == b.mapId)) {
						b.mapId = d.id;
						saveActive();
						ImGui::CloseCurrentPopup();
					}
					if (ImGui::IsItemHovered()) {
						std::string tip = mapRegionLabel(d.region) +
							u8" · 原始階級 " + mapOwnTierLabel(d);
						const std::string& item = mapSecondName(d);
						if (!item.empty() && item != mapPrimaryName(d)) tip = item + u8" · " + tip;
						ImGui::SetTooltip("%s", tip.c_str());
					}
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
			ImGui::EndCombo();
		}
		ImGui::PopStyleColor();
		PobUi::PopControlFrame();

		if (cur) {
			ImGui::SameLine(0, PobUi::D(8.0f));
			if (PobUi::Button(u8"清除##map", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
				b.mapId.clear();
				saveActive();
			}
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	}

	// --- scarab section (map device) ---
	// Kept beside the atlas stats but never merged into them: these are map
	// modifiers, not passive bonuses, and their numbers do not add up with the
	// tree's. The picker filters on en+zh and only asks for the icons of the
	// rows actually on screen, so opening it does not queue 130 downloads.
	char scarabSearch[256] = "";
	bool openScarabPicker = false;   // set inside the panel, acted on outside it
	const std::vector<std::string>& scarabLines(const ScarabDef& d)
	{
		// One list or the other, never a line from each: a Description cell can
		// split into a different number of lines per locale.
		return (showZh && !d.descZh.empty()) ? d.descZh : d.descEn;
	}
	const std::string& scarabName(const ScarabDef& d)
	{
		return (showZh && !d.zh.empty()) ? d.zh : d.en;
	}
	std::string scarabRefuseText(const ScarabAddResult& r)
	{
		switch (r.code) {
		case ScarabAdd::kFull:
			return u8"地圖裝置只有 " + std::to_string(kMaxScarabs) + u8" 個地圖格";
		case ScarabAdd::kOverLimit:
			return u8"這個項目最多只能放 " + std::to_string(r.limit) + u8" 份";
		case ScarabAdd::kFamilyConflict:
			return u8"與「" + (r.conflict ? scarabName(*r.conflict) : std::string("?")) + u8"」不能同時使用";
		default:
			return u8"這個項目不在目前的資料中";
		}
	}

	int scarabReplaceIdx_ = -1;    // the picker replaces this slot; -1 = adds a new one
	int scarabMenuIdx_ = -1;       // the filled slot whose menu is open
	bool openScarabMenu_ = false;

	void renderScarabPanel()
	{
		AtlasBuildEntry& b = buildFile.Active();
		if (!scarabDb.available()) {
			if (PobUi::CollapsingSection(u8"地圖格", "###scarabhdr", PobIcon::Layers, nullptr, false)) {
				ImGui::TextWrapped(u8"地圖格資料未載入：%s", scarabErr.c_str());
				PobUi::Hint(u8"已存的地圖格設定不會被更動。");
			}
			return;
		}
		const std::string hdr = u8"地圖格  " + std::to_string(b.scarabs.size()) + " / " + std::to_string(kMaxScarabs);
		if (!PobUi::CollapsingSection(hdr.c_str(), "###scarabhdr", PobIcon::Layers, u8"聖甲蟲與其他地圖裝置"))
			return;

		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		const float slot = std::floor(PobUi::D(44.0f));
		for (int i = 0; i < kMaxScarabs; i++) {
			if (i) ImGui::SameLine(0, PobUi::D(10.0f));
			ImGui::PushID(i);
			if (i < (int)b.scarabs.size()) {
				const ScarabDef* d = scarabDb.ById(b.scarabs[i]);
				unsigned tex = 0;
				if (d) {
					icons.RequestPath(d->art);
					tex = icons.TextureByPath(d->art);
				}
				if (PobUi::Slot("##slot", tex, d ? u8"…" : "?", true, slot)) {
					scarabMenuIdx_ = i;
					openScarabMenu_ = true;
				}
				if (d && ImGui::IsItemHovered()) {
					ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(14.0f), PobUi::D(10.0f)));
					ImGui::BeginTooltip();
					ImGui::TextColored(PobUi::TokV4(Tok::AccentText), "%s", scarabName(*d).c_str());
					ImGui::PushTextWrapPos(360.0f * scale);
					for (const std::string& s : scarabLines(*d))
						ImGui::TextUnformatted(StripStatMarkup(s).c_str());
					ImGui::PopTextWrapPos();
					PobUi::Hint(u8"點擊可替換或移除");
					ImGui::EndTooltip();
					ImGui::PopStyleVar();
				}
			} else {
				// Every empty slot opens the same picker: which one was clicked
				// does not matter, a new item always goes after the last one.
				if (PobUi::Slot("##slot", 0, nullptr, false, slot)) {
					scarabSearch[0] = '\0';
					scarabReplaceIdx_ = -1;
					// Raised here, submitted outside the panel -- see renderScarabPicker.
					openScarabPicker = true;
				}
				if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"放一個聖甲蟲或地圖碎片");
			}
			ImGui::PopID();
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
		if (b.scarabs.empty()) {
			PobUi::Hint(u8"尚未放置任何項目");
		} else {
			for (const std::string& id : b.scarabs) {
				const ScarabDef* d = scarabDb.ById(id);
				if (!d) continue;
				ImGui::TextColored(PobUi::TokV4(Tok::AccentText), "%s", scarabName(*d).c_str());
				ImGui::Indent(PobUi::D(10.0f));
				ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::TextMuted));
				for (const std::string& s : scarabLines(*d))
					ImGui::TextWrapped("%s", StripStatMarkup(s).c_str());
				ImGui::PopStyleColor();
				ImGui::Unindent(PobUi::D(10.0f));
			}
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	}

	// Submitted OUTSIDE the scrolling panel, on purpose.
	//
	// A popup opened from inside a child window is clipped to that child. This
	// picker is ~460px wide and the slots sit near the panel's right edge, so
	// ImGui's auto-placement flips it leftwards onto the atlas canvas -- outside
	// the panel's clip rect. The popup then renders nothing at all while still
	// swallowing every mouse click. Submitting it in the main window means it is
	// never clipped.
	void renderScarabPicker()
	{
		if (!scarabDb.available()) return;
		AtlasBuildEntry& b = buildFile.Active();
		if (openScarabPicker) {
			ImGui::OpenPopup(u8"選擇地圖格項目");
			openScarabPicker = false;
		}
		ImGui::PushStyleColor(ImGuiCol_PopupBg, PobUi::TokV4(Tok::SurfaceRaised));
		ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, PobUi::D(8.0f));
		if (ImGui::BeginPopup(u8"選擇地圖格項目")) {
			// Replacing = the slot emptied first; CanAdd (the one rule) then decides
			// on that list, and the slot keeps what it had when it refuses.
			const bool replacing = scarabReplaceIdx_ >= 0 && scarabReplaceIdx_ < (int)b.scarabs.size();
			std::vector<std::string> base = b.scarabs;
			if (replacing) base.erase(base.begin() + scarabReplaceIdx_);
			if (replacing) {
				const ScarabDef* old = scarabDb.ById(b.scarabs[scarabReplaceIdx_]);
				PobUi::Hint((u8"替換：" + (old ? scarabName(*old) : std::string("?"))).c_str());
			}
			if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
			PobUi::SearchField("##scarabsearch", scarabSearch, (int)sizeof(scarabSearch),
			                   u8"搜尋名稱或效果（中英、模糊）…", std::floor(PobUi::D(320.0f)));
			ScarabQuery q = MakeScarabQuery(scarabSearch);

			// Rank by match quality; an empty query scores everything 1 so
			// the list keeps its natural family+tier order until you type.
			std::vector<std::pair<int, const ScarabDef*>> hits;
			for (const ScarabDef& d : scarabDb.All()) {
				int s = ScarabMatchScore(d, q);
				if (s > 0) hits.push_back({ s, &d });
			}
			if (!q.empty())
				std::stable_sort(hits.begin(), hits.end(),
					[](const auto& a, const auto& b) {
						if (a.first != b.first) return a.first > b.first;
						return a.second->zh.size() < b.second->zh.size(); // tighter name first
					});
			PobUi::Hint((std::to_string(hits.size()) + u8" 隻 · " + scarabDb.Source()).c_str());

			ImGui::BeginChild("##scarablist", ImVec2(420.0f * scale, 360.0f * scale));
			ImGuiListClipper clip;
			clip.Begin((int)hits.size());
			while (clip.Step()) {
				for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
					const ScarabDef& d = *hits[k].second;
					ScarabAddResult can = scarabDb.CanAdd(base, d.id);
					ImGui::PushID(k);
					// Only rows the clipper actually emits; scrolling the
					// whole list does end up fetching all 130, but that
					// happens once and then lives in the disk cache.
					icons.RequestPath(d.art);
					unsigned tex = icons.TextureByPath(d.art);
					float sz = ImGui::GetTextLineHeight() * 1.4f;
					if (tex) ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(sz, sz));
					else     ImGui::Dummy(ImVec2(sz, sz));
					ImGui::SameLine();
					if (!can.ok()) ImGui::BeginDisabled();
					if (ImGui::Selectable(scarabName(d).c_str())) {
						// Checked again at the moment of writing, on the list it will
						// actually land in.
						if (scarabDb.CanAdd(base, d.id).ok()) {
							if (replacing) base.insert(base.begin() + scarabReplaceIdx_, d.id);
							else base.push_back(d.id);
							b.scarabs = base;
							saveActive();
						}
						scarabReplaceIdx_ = -1;
						ImGui::CloseCurrentPopup();
					}
					if (!can.ok()) ImGui::EndDisabled();
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
						ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(14.0f), PobUi::D(10.0f)));
						ImGui::BeginTooltip();
						ImGui::TextColored(PobUi::TokV4(Tok::AccentText), "%s", scarabName(d).c_str());
						ImGui::PushTextWrapPos(360.0f * scale);
						for (const std::string& s : scarabLines(d))
							ImGui::TextUnformatted(StripStatMarkup(s).c_str());
						ImGui::PopTextWrapPos();
						if (d.limit > 1) PobUi::Hint((u8"可放置 " + std::to_string(d.limit) + u8" 份").c_str());
						if (!d.stash) PobUi::Hint(u8"（未出現在碎片倉庫與交易站）");
						if (!can.ok()) PobUi::Hint(scarabRefuseText(can).c_str(), 0.0f, Tok::Warning);
						ImGui::EndTooltip();
						ImGui::PopStyleVar();
					}
					ImGui::PopID();
				}
			}
			ImGui::EndChild();
			ImGui::EndPopup();
		} else {
			scarabReplaceIdx_ = -1;   // closed without a pick: the next + adds again
		}
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}

	// --- project notes ---
	void renderNotesPanel()
	{
		if (!PobUi::CollapsingSection(u8"備註", "###noteshdr", PobIcon::StickyNote, nullptr, false)) return;
		AtlasBuildEntry& b = buildFile.Active();
		ImGui::InputTextMultiline("##notes", &b.notes,
			ImVec2(-FLT_MIN, 90.0f * scale));
		// Writing on every keystroke would hit the disk once per character.
		if (ImGui::IsItemDeactivatedAfterEdit()) saveActive();
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	}

	// League mechanics: the same highlight the mastery icons drive, reachable as
	// a list so you do not have to find a cluster first. Not part of the build --
	// nothing here is saved, it is a way of reading the map.
	void renderMechanicPanel()
	{
		const AtlasMechanicDef* selDef = mechSel.empty() ? nullptr : AtlasMechanicById(mechSel);
		const std::string note = selDef ? (showZh && zhLoaded ? selDef->zh : selDef->en) : std::string();
		if (!PobUi::CollapsingSection(u8"機制", "###mechhdr", PobIcon::Crosshair, note.empty() ? nullptr : note.c_str(),
		                              false))
			return;
		if (mechNodeIdx.empty()) {
			PobUi::Hint(u8"這個賽季還沒有機制分類資料。重新下載或匯入賽季資料後就會產生。", ImGui::GetContentRegionAvail().x);
			ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
			return;
		}
		if (!mechDb.BorrowedFrom().empty())
			PobUi::Hint((u8"（分類沿用 " + mechDb.BorrowedFrom() + u8"，本賽季新增的節點可能未歸類）").c_str(),
			            ImGui::GetContentRegionAvail().x);
		PobUi::Hint(u8"點一列標出全輿圖同機制的位置；再點一次取消");
		for (const auto& kv : mechNodeIdx) {
			const AtlasMechanicDef* d = AtlasMechanicById(kv.first);
			if (!d) continue;
			const std::vector<int>* mm = mechFind(mechMastIdx, kv.first);
			std::string label = (showZh && zhLoaded ? d->zh : d->en) +
			                    "###mech_" + d->id;   // stable id, label may flip language
			ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(kv.first == mechSel ? Tok::TreeHit : Tok::Text));
			bool hit = ImGui::Selectable(label.c_str(), kv.first == mechSel);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			PobUi::Numeric(std::to_string(kv.second.size()).c_str());
			if (hit) {
				mechSel = (kv.first == mechSel) ? std::string() : kv.first;
				applyMechHighlight();
				// Jump to the first cluster so a mechanic off-screen is not just
				// "nothing happened".
				if (!mechSel.empty() && mm && !mm->empty()) {
					int mi = (*mm)[0];
					// masteries are decorations, not nodes; centre on the nearest
					// node of the mechanic instead so CenterOn has something real
					int best = -1;
					float bestSq = 0.0f;
					for (int ni : kv.second) {
						float dx = tree.nodes[ni].x - tree.masteries[mi].x;
						float dy = tree.nodes[ni].y - tree.masteries[mi].y;
						float sq = dx * dx + dy * dy;
						if (best < 0 || sq < bestSq) { best = ni; bestSq = sq; }
					}
					if (best >= 0) view.CenterOn(tree, best);
				}
			}
		}
		if (mechDb.Unassigned() > 0)
			PobUi::Hint((u8"另有 " + std::to_string(mechDb.Unassigned()) + u8" 個節點不屬於任何機制叢集").c_str());
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	}

	// Compute the compareBase -> active season diff (data-only; independent of the
	// user's allocation). Maps every changed id to a canvas node index so the
	// panel can focus it and the overlay can ring it.
	void rebuildDiff()
	{
		diffReady = false;
		diffErr.clear();
		diff = AtlasTreeDiff();
		activeIdxById.clear();
		// Fall back to the registry's defaults until the user picks a pair, and
		// re-validate every time: an update or a prune can retire a chosen tag.
		if (cmpBase.empty() || !verIndex.Has(cmpBase)) cmpBase = verIndex.CompareBase();
		if (cmpTarg.empty() || !verIndex.Has(cmpTarg)) cmpTarg = verIndex.Active();
		std::string base = cmpBase, targ = cmpTarg;
		if (base.empty() || targ.empty()) {
			diffErr = u8"需要兩個版本的資料才能比較（目前只安裝了一個版本）";
			return;
		}
		if (base == targ) {
			diffErr = u8"請選擇兩個不同的版本";
			return;
		}
		AtlasTreeData bt, tt;
		std::string e;
		if (!bt.LoadVersion(exeDir, base, &e)) { diffErr = u8"載入 " + base + u8" 失敗：" + e; return; }
		if (!tt.LoadVersion(exeDir, targ, &e)) { diffErr = u8"載入 " + targ + u8" 失敗：" + e; return; }
		AtlasI18n bz, tz;
		bool hb = bz.LoadVersion(exeDir, base), ht = tz.LoadVersion(exeDir, targ);
		// same display rules as the canvas: when the zh snapshot predates the
		// season, changed lines / renamed nodes fall back to English; unchanged
		// lines keep (or backfill) their Chinese
		if (hb && ht) {
			bool zhLags = !tz.RepoeVersion().empty() &&
			              AtlasVersionIndex::CompareSemver(tz.RepoeVersion(), targ) < 0;
			if (zhLags) {
				PruneStaleTranslations(tz, tt, bt);
				DropRenamedNames(tz, tt, bt);
			}
			BackfillAtlasI18n(tz, tt, bz, bt);
		}
		diff = ComputeAtlasTreeDiff(bt, tt, hb ? &bz : nullptr, ht ? &tz : nullptr, base, targ);
		for (int i = 0; i < (int)tree.nodes.size(); i++) activeIdxById[tree.nodes[i].id] = i;
		diffReady = true;
	}

	// Ring the changed nodes on whichever season's tree is on the canvas
	// (activeIdxById is the displayed tree): added=green, removed=red, modified=
	// amber. added only maps on the newer tree, removed only on the older one, so
	// each season shows the rings that make sense for it.
	void applyDiffOverlay()
	{
		std::unordered_map<int, ImU32> rings;
		for (const AtlasNodeDiff& n : diff.added) {
			auto it = activeIdxById.find(n.id);
			if (it != activeIdxById.end()) rings[it->second] = (Tok::TreeAdd & 0x00FFFFFFu) | 0xEB000000u;
		}
		for (const AtlasNodeDiff& n : diff.removed) {
			auto it = activeIdxById.find(n.id);
			if (it != activeIdxById.end()) rings[it->second] = (Tok::TreeRemove & 0x00FFFFFFu) | 0xEB000000u;
		}
		for (const AtlasNodeDiff& n : diff.modified) {
			auto it = activeIdxById.find(n.id);
			if (it != activeIdxById.end()) rings[it->second] = (Tok::TreeHit & 0x00FFFFFFu) | 0xEB000000u;
		}
		view.SetDiffOverlay(rings);
	}

	// Toggle helper shared by the toolbar button and hot-reload.
	void refreshCompare()
	{
		rebuildDiff();
		if (diffReady) applyDiffOverlay();
		else view.ClearDiffOverlay();
	}

	// An icon prefix when the icon font is in, the old ASCII one otherwise.
	static std::string Prefix(const char* icon, const char* ascii)
	{
		// A no-break space after the icon: word wrap must not leave the icon alone
		// on a line when the text after it is one long run of CJK.
		return PobUi::Fonts().icons ? std::string(icon) + "\xC2\xA0" : std::string(ascii);
	}

	// Right-hand panel body while the version-compare view is open. Lists added /
	// removed / value-changed nodes with per-line deltas; clicking a node that
	// still exists in the new season focuses it on the canvas.
	void renderComparePanel()
	{
		ImGui::AlignTextToFramePadding();
		if (PobUi::Fonts().icons) {
			PobUi::Icon(PobIcon::GitCompare, Tok::AccentText);
			ImGui::SameLine(0, PobUi::D(8.0f));
		}
		PobUi::Heading(u8"版本比較");
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));

		// --- pick the two versions ---
		// Every installed tag is offered on both sides, so this covers both
		// "3.28 -> 3.29" (what the new league changed) and "3.29.0 -> 3.29.1"
		// (what GGG adjusted mid-league).
		std::vector<std::string> tags = verIndex.TagsNewestFirst();
		std::vector<const char*> tagPtrs;
		for (const std::string& t : tags) tagPtrs.push_back(t.c_str());
		const float cmpRowY = ImGui::GetCursorScreenPos().y;   // both selects sit on this line
		auto versionSelect = [&](const char* id, const char* caption, std::string& slot) {
			// Caption drawn straight into the list: a text item after a framed one
			// on the same line inherits its baseline offset and drops a few pixels.
			const ImVec2 rp(ImGui::GetCursorScreenPos().x, cmpRowY);
			ImFont* cf = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
			const ImVec2 cs = cf->CalcTextSizeA(cf->FontSize, FLT_MAX, 0.0f, caption);
			ImGui::GetWindowDrawList()->AddText(cf, cf->FontSize,
				ImVec2(rp.x, rp.y + std::floor((PobUi::ControlH() - cs.y) * 0.5f)), Tok::TextMuted, caption);
			ImGui::SetCursorScreenPos(ImVec2(rp.x + cs.x + PobUi::D(6.0f), rp.y));
			int sel = -1;
			for (size_t i = 0; i < tags.size(); i++) if (tags[i] == slot) sel = (int)i;
			if (PobUi::Select(id, &sel, tagPtrs.data(), nullptr, (int)tagPtrs.size(),
			                  (std::max)(std::floor(PobUi::D(110.0f)), PobUi::SelectFitWidth(tagPtrs.data(), (int)tagPtrs.size()))) &&
			    sel >= 0 && tags[sel] != slot) {
				slot = tags[sel];
				return true;
			}
			return false;
		};
		bool pairChanged = versionSelect("##cmpbase", u8"舊", cmpBase);
		ImGui::SameLine(0, PobUi::D(12.0f));
		pairChanged |= versionSelect("##cmptarg", u8"新", cmpTarg);
		ImGui::SameLine(0, PobUi::D(8.0f));
		if (PobUi::Button(u8"對調", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
			std::swap(cmpBase, cmpTarg);
			pairChanged = true;
		}
		if (pairChanged) refreshCompare();
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));

		if (!diffReady) {
			ImGui::Dummy(ImVec2(0, PobUi::D(8.0f)));
			ImGui::TextWrapped("%s", diffErr.empty() ? u8"尚未計算比較" : diffErr.c_str());
			return;
		}
		{
			const std::string title = diff.oldVer + "  >  " + diff.newVer;
			PobUi::Numeric(title.c_str(), Tok::Text);
			ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
			const std::string a = u8"新增 " + std::to_string(diff.added.size());
			const std::string r = u8"移除 " + std::to_string(diff.removed.size());
			const std::string m = u8"變動 " + std::to_string(diff.modified.size());
			PobUi::StatusPill(PobUi::Tone::Ok, a.c_str());
			ImGui::SameLine(0, PobUi::D(6.0f));
			PobUi::StatusPill(PobUi::Tone::Bad, r.c_str());
			ImGui::SameLine(0, PobUi::D(6.0f));
			PobUi::StatusPill(PobUi::Tone::Warn, m.c_str());
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
		PobUi::SearchField("##diffsearch", diffSearch, (int)sizeof(diffSearch), u8"搜尋變更節點或詞條…",
		                   ImGui::GetContentRegionAvail().x);
		std::string needle = ToLowerAscii(diffSearch);
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));

		auto label = [&](const AtlasNodeDiff& n) -> const std::string& {
			return (showZh && zhLoaded && !n.nameZh.empty()) ? n.nameZh : n.name;
		};
		auto lineNew = [&](const AtlasStatDelta& d) -> std::string {
			return StripStatMarkup((showZh && zhLoaded && !d.zh.empty()) ? d.zh : d.en);
		};
		auto lineOld = [&](const AtlasStatDelta& d) -> std::string {
			return StripStatMarkup((showZh && zhLoaded && !d.zhOld.empty()) ? d.zhOld : d.enOld);
		};
		auto match = [&](const AtlasNodeDiff& n) -> bool {
			if (needle.empty()) return true;
			if (ToLowerAscii(n.name).find(needle) != std::string::npos) return true;
			if (ToLowerAscii(n.nameZh).find(needle) != std::string::npos) return true;
			for (const AtlasStatDelta& d : n.stats)
				if (ToLowerAscii(d.en).find(needle) != std::string::npos) return true;
			return false;
		};
		const std::string pAdd = Prefix(PobIcon::Plus, "+ ");
		const std::string pDel = Prefix(PobIcon::Minus, "- ");
		const std::string pMod = Prefix(PobIcon::Pencil, "~ ");
		const std::string pTo = Prefix(PobIcon::ChevronRight, "=> ");

		ImGui::BeginChild("##diffscroll", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding);

		if (!diff.added.empty()) {
			ImGui::PushID("added");
			const std::string h = u8"新增節點  " + std::to_string(diff.added.size());
			if (PobUi::CollapsingSection(h.c_str(), "###cmpadd", PobIcon::Plus)) {
				for (const AtlasNodeDiff& n : diff.added) {
					if (!match(n)) continue;
					ImGui::PushID(n.id);
					bool clickable = activeIdxById.count(n.id) > 0;
					ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::TreeAdd));
					if (ImGui::Selectable((pAdd + (label(n).empty() ? std::string("?") : label(n))).c_str()) && clickable)
						view.CenterOn(tree, activeIdxById[n.id]);
					ImGui::PopStyleColor();
					ImGui::PopID();
				}
			}
			ImGui::PopID();
		}
		if (!diff.removed.empty()) {
			ImGui::PushID("removed");
			const std::string h = u8"移除節點  " + std::to_string(diff.removed.size());
			if (PobUi::CollapsingSection(h.c_str(), "###cmpdel", PobIcon::Minus)) {
				for (const AtlasNodeDiff& n : diff.removed) {
					if (!match(n)) continue;
					ImGui::PushID(n.id);
					ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::TreeRemove));
					ImGui::Selectable((pDel + (label(n).empty() ? std::string("?") : label(n))).c_str());
					ImGui::PopStyleColor();
					if (ImGui::IsItemHovered()) {
						ImGui::BeginTooltip();
						PobUi::Hint(u8"此節點在新賽季已移除");
						ImGui::PushTextWrapPos(380.0f * scale);
						for (const std::string& s : n.statsOld) ImGui::TextUnformatted(StripStatMarkup(s).c_str());
						ImGui::PopTextWrapPos();
						ImGui::EndTooltip();
					}
					ImGui::PopID();
				}
			}
			ImGui::PopID();
		}
		if (!diff.modified.empty()) {
			ImGui::PushID("modified");
			const std::string h = u8"數值/詞條變動  " + std::to_string(diff.modified.size());
			if (PobUi::CollapsingSection(h.c_str(), "###cmpmod", PobIcon::Pencil)) {
				for (const AtlasNodeDiff& n : diff.modified) {
					if (!match(n)) continue;
					ImGui::PushID(n.id);
					bool clickable = activeIdxById.count(n.id) > 0;
					ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::TreeHit));
					if (ImGui::Selectable((pMod + (label(n).empty() ? std::string("?") : label(n))).c_str()) && clickable)
						view.CenterOn(tree, activeIdxById[n.id]);
					ImGui::PopStyleColor();
					ImGui::Indent(PobUi::D(12.0f));
					ImGui::PushTextWrapPos(0.0f);
					if (n.nameChanged) {
						PobUi::Hint((u8"改名自：" +
							(showZh && zhLoaded && !n.nameOldZh.empty() ? n.nameOldZh : n.nameOld)).c_str(),
							ImGui::GetContentRegionAvail().x);
					}
					for (const AtlasStatDelta& d : n.stats) {
						if (d.kind == AtlasStatDelta::kValueChanged) {
							ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::TextMuted));
							ImGui::TextWrapped("%s", lineOld(d).c_str());
							ImGui::PopStyleColor();
							ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::Text));
							ImGui::TextWrapped("%s%s", pTo.c_str(), lineNew(d).c_str());
							ImGui::PopStyleColor();
						} else if (d.kind == AtlasStatDelta::kLineAdded) {
							ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::TreeAdd));
							ImGui::TextWrapped("%s%s", pAdd.c_str(), lineNew(d).c_str());
							ImGui::PopStyleColor();
						} else {
							ImGui::PushStyleColor(ImGuiCol_Text, PobUi::TokV4(Tok::TreeRemove));
							ImGui::TextWrapped("%s%s", pDel.c_str(), lineNew(d).c_str());
							ImGui::PopStyleColor();
						}
					}
					ImGui::PopTextWrapPos();
					ImGui::Unindent(PobUi::D(12.0f));
					ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
					ImGui::PopID();
				}
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
	}

	// hot-reload after new data landed on disk (manual import or auto update);
	// GL work, main thread only
	void hotReload(const std::string& okMsg)
	{
		verIndex.Load(exeDir);              // a new season may have landed
		bool zhWas = zhLoaded;              // the zh mapping may have changed too
		loadSeason(verIndex.Active());      // follow to the (possibly new) active season
		if (ready) saveActive();            // lock in the id-mapping after any pruning
		if (!zhLoaded) showZh = false;
		else if (!zhWas) showZh = true;     // translations appeared: switch on
		importMsg = ready ? okMsg : loadErr;
		importFailed = !ready;
		panelDirty = true;
		// compute the diff so the toolbar can prompt "what changed this update"
		diffReady = false;
		if (ready && verIndex.Versions().size() >= 2 && !verIndex.CompareBase().empty()) {
			rebuildDiff();
			if (diffReady)
				importMsg += u8"｜可比較 " + diff.oldVer + u8" -> " + diff.newVer + u8"：新增" +
				             std::to_string(diff.added.size()) + u8" 移除" + std::to_string(diff.removed.size()) +
				             u8" 變動" + std::to_string(diff.modified.size()) + u8"（按「版本比較」）";
		}
		if (compareMode) { if (diffReady) applyDiffOverlay(); else view.ClearDiffOverlay(); }
		else view.ClearDiffOverlay();
	}

	// convert + hot-reload; keeps the old data untouched when conversion fails
	void importSeason()
	{
		// See ApDialog: the file dialog cannot run inside a frame.
		pendingDialog_ = ApDialog::ImportSeasonData;
	}
	void importSeasonFrom(const std::wstring& path)
	{
		if (path.empty()) return;
		// manual import replaces the active season in place (no tag is available
		// from a local data.json); the auto updater handles versioned rolling
		std::wstring dest = verIndex.ResolveDataDir(exeDir, verIndex.Active());
		std::string ierr, isum;
		if (!ImportAtlasTreeData(path, dest, &ierr, &isum)) {
			importMsg = ierr;
			importFailed = true;
			return;
		}
		hotReload(isum);
	}


	// Was a closure in the frame body; a member because the .json import path
	// reaches it from RunDeferred() rather than from a frame.
	void importEntry(const AtlasBuildEntry& e)
	{
		saveActive();
		int idx = buildFile.AddBuild(e.name);
		buildFile.active = idx;
		// Keep the raw ids so a preview season cannot prune them; the
		// canonical season's saveActive() below replaces them with the
		// mapped set.
		buildFile.builds[idx].alloc = e.alloc;
		buildFile.builds[idx].notes = e.notes;
		buildFile.builds[idx].targets = e.targets;
		buildFile.builds[idx].blocked = e.blocked;
		// Each Sanitize clears the note it is handed, so they get their own
		// and are concatenated afterwards.
		std::string snote, anote;
		buildFile.builds[idx].scarabs = scarabDb.Sanitize(e.scarabs, &snote);
		buildFile.builds[idx].astrolabes = astroDb.Sanitize(e.astrolabes, &anote);
		snote += anote;
		buildFile.builds[idx].mapId = mapDb.SanitizeOne(e.mapId);
		if (!e.mapId.empty() && buildFile.builds[idx].mapId.empty())
			snote += u8"，忽略 1 張未知地圖";
		buildFile.builds[idx].profit = e.profit; // the sender's figures, shown as bound
		int kept = tree.ApplyAllocIds(e.alloc);
		tree.ApplyTargetIds(e.targets);
		tree.ApplyBlockedIds(e.blocked);
		undo.valid = false;
		saveActive();
		panelDirty = true;
		int dropped = (int)e.alloc.size() - kept;
		importMsg = u8"已匯入「" + buildFile.Active().name + u8"」：" + std::to_string(kept) + u8" 點";
		if (dropped > 0) importMsg += u8"（丟棄 " + std::to_string(dropped) + u8" 個未知節點）";
		if (!buildFile.builds[idx].astrolabes.empty())
			importMsg += u8"、" + std::to_string(buildFile.builds[idx].astrolabes.size()) + u8" 片幻塑界域";
		if (!buildFile.builds[idx].mapId.empty()) importMsg += u8"、主力地圖";
		if (!buildFile.builds[idx].scarabs.empty())
			importMsg += u8"、" + std::to_string(buildFile.builds[idx].scarabs.size()) + u8" 個地圖格項目";
		if (!buildFile.builds[idx].notes.empty()) importMsg += u8"、備註";
		if (!buildFile.builds[idx].profit.empty()) importMsg += u8"、收益紀錄";
		importMsg += snote; // "，忽略 N 個未知甲蟲" etc., empty when nothing was dropped
		importFailed = false;
	}

	// ---- what the host lends, and what a close is waiting on -----------------
	const ToolPanelHost* host_ = nullptr;
	std::wstring exeDir;
	float scale = 1.0f;
	ImFont* fontBig = nullptr;   // refreshed from the host every frame
	bool cjkOk = false;          // ditto

	ToolCloseState close_ = ToolCloseState::Open;
	ApDialog pendingDialog_ = ApDialog::None;
	std::string pendingExportName_;   // captured with the intent, used in RunDeferred
	bool shutdown_ = false;
};

IToolPanel* CreateAtlasPlannerPanel()
{
	return new AtlasPlannerPanel();
}

void ShowAtlasPlanner(const std::wstring& exeDir, const std::wstring& locale)
{
	AtlasPlannerPanel panel;
	ToolWindowDesc desc;
	// "PobTools — 輿圖策略"
	desc.titleUtf8 = "PobTools \xe2\x80\x94 \xe8\xbc\xbf\xe5\x9c\x96\xe7\xad\x96\xe7\x95\xa5";
	desc.defW = 1280;
	desc.defH = 860;
	RunToolWindow(panel, desc, exeDir, L"poe1", locale);
}