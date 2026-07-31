// must precede every imgui.h include (atlas_view.h pulls it in)
#define IMGUI_DEFINE_MATH_OPERATORS

#include "atlas_planner.h"
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
#include "atlas_version_index.h"
#include "atlas_view.h"
#include "editor_util.h" // EdReadFile
#include "icon_manager.h" // scarab icons (poecdn + on-disk cache)
#include "launcher_config.h" // ResolveConfiguredFontPath
#include "ui_theme.h"

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
#include <functional>
#include <numeric>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

static const float kFontSize = 18.0f;

// node-kind palette indexed by panel rank (keystone / wormhole / notable / small);
// same hues the canvas tooltip uses
static const ImVec4 kKindCol[4] = {
	ImVec4(0.85f, 0.45f, 0.85f, 1.0f),
	ImVec4(0.55f, 0.80f, 0.95f, 1.0f),
	ImVec4(0.95f, 0.80f, 0.40f, 1.0f),
	ImVec4(0.72f, 0.76f, 0.82f, 1.0f),
};
static const char* kGroupName[4] = { u8"核心天賦", u8"蟲洞", u8"大點", u8"小點" };

// Open-file dialog scoped to data.json (new-season atlastree-export import).
static std::wstring OpenDataJsonDialog()
{
	wchar_t buf[MAX_PATH] = L"";
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = GetActiveWindow();
	ofn.lpstrFilter = L"atlastree-export data.json\0data.json;*.json\0所有檔案 (*.*)\0*.*\0\0";
	ofn.lpstrFile = buf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
	return GetOpenFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

static const wchar_t* kBuildJsonFilter = L"輿圖策略專案 (*.json)\0*.json\0所有檔案 (*.*)\0*.*\0\0";

// Save-file dialog for exporting one build project; buf pre-filled with the
// project name (filesystem-hostile characters stripped).
static std::wstring SaveBuildJsonDialog(const std::string& suggestedName)
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
	ofn.hwndOwner = GetActiveWindow();
	ofn.lpstrFilter = kBuildJsonFilter;
	ofn.lpstrDefExt = L"json";
	ofn.lpstrFile = buf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	return GetSaveFileNameW(&ofn) ? std::wstring(buf) : std::wstring();
}

static std::wstring OpenBuildJsonDialog()
{
	wchar_t buf[MAX_PATH] = L"";
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = GetActiveWindow();
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

void ShowAtlasPlanner(const std::wstring& exeDir, const std::wstring& /*locale*/)
{
	if (!glfwInit()) {
		MessageBoxW(nullptr, L"無法初始化 GLFW，輿圖策略無法顯示。", L"PobTools", MB_ICONERROR | MB_OK);
		return;
	}
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

	float scale = 1.0f;
	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	if (monitor) {
		float sx = 1.0f, sy = 1.0f;
		glfwGetMonitorContentScale(monitor, &sx, &sy);
		scale = sx > 0.0f ? sx : 1.0f;
	}
	const int winW = (int)(1280 * scale);
	const int winH = (int)(860 * scale);

	GLFWwindow* win = glfwCreateWindow(winW, winH, u8"PobTools — 輿圖策略", nullptr, nullptr);
	if (!win) {
		glfwTerminate();
		MessageBoxW(nullptr, L"無法建立輿圖策略視窗。", L"PobTools", MB_ICONERROR | MB_OK);
		return;
	}
	if (monitor) {
		const GLFWvidmode* mode = glfwGetVideoMode(monitor);
		if (mode) glfwSetWindowPos(win, (mode->width - winW) / 2, (mode->height - winH) / 2);
	}
	glfwMakeContextCurrent(win);
	glfwSwapInterval(1);
	glfwShowWindow(win);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	PobUi::ApplyTheme(scale, PobUi::Density::Canvas);

	// Full CJK atlas (same as the filter editor) for node stats and UI text,
	// plus a digits-only large face for the points summary.
	std::vector<unsigned char> ttf = EdReadFile(ResolveConfiguredFontPath(exeDir));
	ImFont* font = nullptr;
	ImFont* fontBig = nullptr;
	bool cjkOk = false;
	if (!ttf.empty()) {
		ImGuiIO& io = ImGui::GetIO();
		static ImVector<ImWchar> ranges;
		ranges.clear();
		ImFontGlyphRangesBuilder b;
		b.AddRanges(io.Fonts->GetGlyphRangesDefault());
		b.AddRanges(io.Fonts->GetGlyphRangesChineseFull());
		b.BuildRanges(&ranges);
		ImFontConfig cfg;
		cfg.FontDataOwnedByAtlas = false;
		cfg.OversampleH = 1;
		cfg.OversampleV = 1;
		cfg.PixelSnapH = true;
		io.Fonts->TexDesiredWidth = 4096;
		font = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), kFontSize * scale, &cfg, ranges.Data);
		static ImVector<ImWchar> bigRanges;
		bigRanges.clear();
		ImFontGlyphRangesBuilder bb;
		bb.AddText("0123456789 /");
		bb.BuildRanges(&bigRanges);
		fontBig = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), 30.0f * scale, &cfg, bigRanges.Data);
		if (io.Fonts->Build() && font)
			cjkOk = font->FindGlyphNoFallback((ImWchar)0x555F /* 啟 */) != nullptr;
	}
	if (!font) font = ImGui::GetIO().Fonts->AddFontDefault();

	ImGui_ImplGlfw_InitForOpenGL(win, true);
	ImGui_ImplOpenGL3_Init("#version 100");

	// --- data + view ---
	AtlasTreeData tree;
	AtlasView view;
	std::string loadErr;
	// Multi-project build file: the in-memory copy is the single source of
	// truth while the planner is open; every save funnels through saveActive.
	AtlasBuildFile buildFile;
	buildFile.Load(exeDir); // legacy single-build files migrate transparently

	// Scarab catalogue + icon fetcher. Both are optional: a missing
	// Data/scarabs_poe1.json hides the section (and makes Sanitize a no-op, so a
	// saved scarab list survives untouched), and no network just means no icons.
	ScarabDb scarabDb;
	std::string scarabErr;
	scarabDb.Load(exeDir, &scarabErr);

	// Astrolabes (3.29 Shaped Regions, one per atlas quadrant) and the map
	// catalogue behind the project's main-map pick. Optional on exactly the same
	// terms as the scarabs: absent data hides the section and turns Sanitize
	// into a no-op rather than erasing what the user saved.
	AstrolabeDb astroDb;
	std::string astroErr;
	astroDb.Load(exeDir, &astroErr);
	AtlasMapDb mapDb;
	std::string mapErr;
	mapDb.Load(exeDir, &mapErr);

	for (AtlasBuildEntry& b : buildFile.builds) {
		b.scarabs = scarabDb.Sanitize(b.scarabs, nullptr);
		b.astrolabes = astroDb.Sanitize(b.astrolabes, nullptr);
		b.mapId = mapDb.SanitizeOne(b.mapId);
	}

	IconManager icons;
	icons.Init(exeDir);

	std::string nameBuf; // shared by the new/rename project modals

	// --- persisted UI state (panel width + last viewed season) ---
	AtlasUiState uiState;
	uiState.Load(exeDir);
	float panelW = -1.0f; // sentinel: initialized on the first frame (needs DisplaySize)

	// --- version registry: which seasons are installed, which one is shown ---
	AtlasVersionIndex verIndex;
	verIndex.Load(exeDir);
	// Load() repairs the index against the season folders actually on disk (the
	// packaged atlas_index.json overwrites the user's on every app update). Write
	// the repair back once so it sticks.
	if (verIndex.NeedsSave()) verIndex.Save(exeDir);
	// viewTag = the season currently on the canvas (persisted choice, else active)
	std::string viewTag = (!uiState.season.empty() && verIndex.Has(uiState.season))
		? uiState.season : verIndex.Active();

	int startupDropped = 0;  // nodes lost because the saved build predates this season
	bool ready = false;      // set by the initial loadSeason() below
	std::string importMsg;   // last import result shown in the toolbar
	bool importFailed = false;

	// The newest installed season is canonical (the one the build persists for);
	// an older-season view is a read-only preview.
	auto onCanonicalSeason = [&]() {
		return verIndex.Active().empty() || viewTag == verIndex.Active();
	};
	// Capture the allocation only on the canonical season, so a preview of an
	// older season never prunes it back to that season's subset. The file is
	// still written either way: notes and scarabs are season-independent, and
	// before they existed an edit made while previewing was simply lost.
	// Set while the sandbox planning mode is open. saveActive() checks it, so
	// there is exactly ONE place that can write during planning: nowhere.
	bool planningMode = false;
	auto saveActive = [&]() {
		if (planningMode) return;   // sandbox: never touch the file
		if (onCanonicalSeason()) {
			buildFile.Active().alloc = tree.AllocIds();
			buildFile.Active().targets = tree.TargetIds();
			buildFile.Active().blocked = tree.BlockedIds();
			buildFile.version = tree.Version();
		}
		buildFile.Save(exeDir);
	};

	// Single-level undo. Plain clicking never moves anything the user placed, but
	// planning mode and the compress button both re-route wiring, which is the
	// one thing a user cannot predict, so there has to be a way back. Snapshots
	// are taken before a change lands, not after.
	struct AtlasUndo {
		bool valid = false;
		std::vector<int> alloc, targets, blocked;
	};
	AtlasUndo undo, frameStart;
	auto snapshot = [&](AtlasUndo& u) {
		u.valid = true;
		u.alloc = tree.AllocIds();
		u.targets = tree.TargetIds();
		u.blocked = tree.BlockedIds();
	};

	// --- sandbox planning mode ---
	// Entering starts from a blank slate so several core nodes can be marked
	// quickly; leaving asks whether to keep the result. NOTHING is written to
	// the build file while planning, so abandoning a session is guaranteed to
	// leave the project byte-identical -- that is the whole point of the mode.
	bool planningAskExit = false;      // the save/discard prompt is up
	AtlasUndo planSnapshot;            // state captured on entry
	auto enterPlanning = [&]() {
		snapshot(planSnapshot);
		tree.Reset();                  // clears alloc, targets and blocked
		planningMode = true;
		planningAskExit = false;
	};
	auto restorePlanning = [&]() {
		tree.ApplyAllocIds(planSnapshot.alloc);
		tree.ApplyTargetIds(planSnapshot.targets);
		tree.ApplyBlockedIds(planSnapshot.blocked);
		planningMode = false;
		planningAskExit = false;
		undo.valid = false;            // undo does not straddle the sandbox
	};
	auto keepPlanning = [&]() {
		planningMode = false;
		planningAskExit = false;
		undo = planSnapshot;           // one step back = "before I started planning"
		saveActive();
	};

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
	auto compressTargets = [&]() {
		std::vector<int> t = tree.TargetIdx();
		for (int i : AtlasInferTargets(tree))
			if (std::find(t.begin(), t.end(), i) == t.end()) t.push_back(i);
		return t;
	};
	// Solved only when the button is actually pressed -- a Steiner solve is far
	// too heavy to run once per frame just to grey out a button, and "press it
	// and be told it is already minimal" is a perfectly good answer.
	auto applyCompress = [&](std::string& msg) {
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
		msg = u8"已壓縮：" + std::to_string(compressFrom) + u8" → " + std::to_string(compressTo) +
		      u8" 點（Ctrl+Z 可復原）";
		if (!p.exact) msg += u8"（近似解）";
		return true;
	};

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
	auto mechFind = [](const std::vector<std::pair<std::string, std::vector<int>>>& v,
	                   const std::string& id) -> const std::vector<int>* {
		for (const auto& kv : v)
			if (kv.first == id) return &kv.second;
		return nullptr;
	};
	auto applyMechHighlight = [&]() {
		const std::vector<int>* n = mechSel.empty() ? nullptr : mechFind(mechNodeIdx, mechSel);
		const std::vector<int>* m = mechSel.empty() ? nullptr : mechFind(mechMastIdx, mechSel);
		if (!n && !m) { view.ClearMechanicHighlight(); return; }
		view.SetMechanicHighlight(n ? *n : std::vector<int>(), m ? *m : std::vector<int>());
	};
	// Resolve the season's mechanic map onto THIS tree's node indices. Ids that
	// the season does not have simply do not resolve; nothing is invented.
	auto reloadMechanics = [&]() {
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
	};

	// --- zh display layer + background auto updater ---
	AtlasI18n i18n;
	bool zhLoaded = false;
	bool showZh = false;                 // set after the first season load
	AtlasUpdater updater;
	updater.Init(exeDir);
	updater.RequestCheck(false);         // throttled to once per day

	// (Re)load a season's tree + zh + textures onto the canvas, re-apply the
	// build (by GGG id), and backfill Chinese for value-only changes from the
	// previous season (same wording, adjusted number -> reuse old zh with the new
	// value; changed wording -> keep the new English).
	auto loadSeason = [&](const std::string& tag) {
		viewTag = tag;
		startupDropped = 0;
		ready = tree.LoadVersion(exeDir, tag, &loadErr);
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
	};

	// initial load of the chosen season
	loadSeason(viewTag);
	showZh = zhLoaded;                   // default Chinese when a mapping exists
	if (startupDropped > 0)
		importMsg = u8"提醒：此配置存於舊版輿圖樹，" + std::to_string(startupDropped) +
		            u8" 個已不存在的節點已自動移除";

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
	struct PanelNode { int idx; std::string searchKey; };
	std::vector<PanelNode> nodeGroups[4]; // keystone / wormhole / notable / small
	char panelSearch[256] = "";
	bool panelDirty = true;
	auto rebuildPanel = [&]() {
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
	};

	// --- astrolabe section (one Shaped Region per atlas quadrant) ---
	// Above the scarabs on purpose: an astrolabe covers a whole quadrant, a
	// scarab covers the one map you are about to open.
	char astroSearch[256] = "";
	auto astroLines = [&](const AstrolabeDef& d) -> const std::vector<std::string>& {
		// One list or the other, never a line from each.
		return (showZh && !d.descZh.empty()) ? d.descZh : d.descEn;
	};
	auto astroName = [&](const AstrolabeDef& d) -> const std::string& {
		return (showZh && !d.zh.empty()) ? d.zh : d.en;
	};
	// The compass label is OURS. AtlasRegions ships an Id and no name column, so
	// the only official Traditional Chinese string for a quadrant is its Memory
	// Vault's area name — shown in parentheses so the invented part and the
	// official part stay visibly separate. (Compare the scarab families, where
	// no Chinese name was invented at all; the difference is that a quadrant has
	// to be addressable here, so it needs some label.)
	auto quadrantLabel = [&](const AtlasQuadrant& q) {
		std::string compass = q.id;
		if (showZh) {
			if (q.id == "NorthWest") compass = u8"西北";
			else if (q.id == "NorthEast") compass = u8"東北";
			else if (q.id == "SouthEast") compass = u8"東南";
			else if (q.id == "SouthWest") compass = u8"西南";
		}
		const std::string& vault = (showZh && !q.vaultZh.empty()) ? q.vaultZh : q.vaultEn;
		return vault.empty() ? compass : compass + u8"（" + vault + u8"）";
	};

	auto renderAstrolabePanel = [&]() {
		AtlasBuildEntry& b = buildFile.Active();
		if (!astroDb.available()) {
			if (ImGui::CollapsingHeader(u8"星盤")) {
				ImGui::TextWrapped(u8"星盤資料未載入：%s", astroErr.c_str());
				ImGui::TextDisabled(u8"已存的星盤設定不會被更動。");
			}
			return;
		}
		std::string hdr = u8"星盤 (" + std::to_string(b.astrolabes.size()) + "/" +
		                  std::to_string(astroDb.Regions().size()) + ")###astrohdr";
		if (!ImGui::CollapsingHeader(hdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;

		ImGui::TextDisabled(u8"每個半區同時只能有一片幻塑界域");
		const float slot = 38.0f * scale;
		int regionIdx = 0;
		for (const AtlasQuadrant& q : astroDb.Regions()) {
			ImGui::PushID(regionIdx++);
			// Which astrolabe (if any) sits on this quadrant.
			const AstrolabePlacement* placed = nullptr;
			for (const AstrolabePlacement& p : b.astrolabes)
				if (p.region == q.id) { placed = &p; break; }
			const AstrolabeDef* def = placed ? astroDb.ById(placed->id) : nullptr;

			const ImVec2 fp = ImGui::GetStyle().FramePadding;
			bool clicked = false;
			if (def) {
				icons.RequestPath(def->art);
				unsigned tex = icons.TextureByPath(def->art);
				clicked = tex
					? ImGui::ImageButton("##slot", (ImTextureID)(intptr_t)tex, ImVec2(slot, slot))
					: ImGui::Button("...", ImVec2(slot + fp.x * 2, slot + fp.y * 2));
			} else {
				clicked = ImGui::Button("+##add", ImVec2(slot + fp.x * 2, slot + fp.y * 2));
			}
			if (clicked) {
				if (def) {
					// Clicking a filled slot clears that quadrant.
					for (size_t i = 0; i < b.astrolabes.size(); i++)
						if (b.astrolabes[i].region == q.id) { b.astrolabes.erase(b.astrolabes.begin() + i); break; }
					saveActive();
				} else {
					astroSearch[0] = '\0';
					ImGui::OpenPopup(u8"選擇星盤");
				}
			}
			if (def && ImGui::IsItemHovered()) {
				ImGui::SetTooltip(u8"點擊移除");
			}

			// One picker per quadrant; the PushID above keeps their ids apart.
			if (ImGui::BeginPopup(u8"選擇星盤")) {
				ImGui::TextDisabled("%s", quadrantLabel(q).c_str());
				ImGui::SetNextItemWidth(320.0f * scale);
				if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
				ImGui::InputTextWithHint("##astrosearch", u8"搜尋名稱或效果（中英、模糊）…",
					astroSearch, sizeof(astroSearch), ImGuiInputTextFlags_EscapeClearsAll);
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
				ImGui::TextDisabled(u8"%d 種 · %s", (int)hits.size(), astroDb.Source().c_str());

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
						// The slot was empty, so CanPlace can only refuse on data
						// the catalogue does not know; check anyway rather than
						// trusting the UI state.
						if (astroDb.CanPlace(b.astrolabes, q.id, d.id).ok()) {
							b.astrolabes.push_back({ q.id, d.id });
							saveActive();
						}
						ImGui::CloseCurrentPopup();
					}
					if (ImGui::IsItemHovered()) {
						ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
							ImVec2(14.0f * scale, 10.0f * scale));
						ImGui::BeginTooltip();
						ImGui::TextColored(PobUi::Accent(), "%s", astroName(d).c_str());
						ImGui::PushTextWrapPos(360.0f * scale);
						for (const std::string& s : astroLines(d))
							ImGui::TextUnformatted(StripStatMarkup(s).c_str());
						ImGui::PopTextWrapPos();
						if (!d.enabled)
							ImGui::TextDisabled(u8"（本賽季尚未啟用，交易站也沒有）");
						ImGui::EndTooltip();
						ImGui::PopStyleVar();
					}
					ImGui::PopID();
				}
				ImGui::EndChild();
				ImGui::EndPopup();
			}

			ImGui::SameLine();
			ImGui::BeginGroup();
			ImGui::TextDisabled("%s", quadrantLabel(q).c_str());
			if (def) {
				ImGui::TextColored(PobUi::Accent(), "%s", astroName(*def).c_str());
			} else if (placed) {
				// Sanitize should have removed this; say so instead of drawing a blank.
				ImGui::TextDisabled(u8"未知星盤");
			} else {
				ImGui::TextDisabled(u8"未配置");
			}
			ImGui::EndGroup();

			if (def) {
				ImGui::Indent(10.0f * scale);
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.80f, 0.98f, 1.0f));
				for (const std::string& s : astroLines(*def))
					ImGui::TextWrapped("%s", StripStatMarkup(s).c_str());
				ImGui::PopStyleColor();
				if (!def->enabled)
					ImGui::TextDisabled(u8"（本賽季尚未啟用）");
				ImGui::Unindent(10.0f * scale);
			}
			ImGui::PopID();
		}
		ImGui::Spacing();
	};

	// --- main map section (one per project) ---
	char mapSearch[256] = "";
	// The two dropdowns are INDEPENDENT, and deliberately not modelled on the
	// game's own data: once a quadrant's Voidstone is socketed, every map in that
	// quadrant becomes T16, so a map's shipped tier says nothing about the tier
	// it will actually be run at. Filtering the name list by it would hide maps
	// the user can legitimately pick. So the tier dropdown records the PLAN and
	// the name dropdown always offers every map.
	const int kMapTierUnique = 99;
	auto mapPrimaryName = [&](const AtlasMapDef& d) -> const std::string& {
		// The atlas shows the AREA name, so that is what the planner leads with.
		return showZh ? d.zhArea : d.enArea;
	};
	auto mapSecondName = [&](const AtlasMapDef& d) -> const std::string& {
		return showZh ? d.zhItem : d.enItem;
	};
	auto mapRegionLabel = [&](const std::string& regionId) {
		const AtlasQuadrant* q = astroDb.RegionById(regionId);
		return q ? quadrantLabel(*q) : regionId;
	};
	// A map's own tier, shown only as a hint in the tooltip — never as the label,
	// so it cannot be mistaken for the tier the project plans to run.
	auto mapOwnTierLabel = [&](const AtlasMapDef& d) {
		if (d.kind == AtlasMapDef::kUnique) return std::string(showZh ? u8"傳奇" : "unique");
		return d.tier > 0 ? "T" + std::to_string(d.tier) : std::string("-");
	};
	auto mapTierLabel = [&](int t) {
		if (t == 0) return std::string(showZh ? u8"未指定" : "unset");
		if (t == kMapTierUnique) return std::string(showZh ? u8"傳奇圖" : "unique");
		return "T" + std::to_string(t);
	};
	// Endgame is almost entirely run at the top tier, so an unset project shows
	// that as its starting point. Only a deliberate pick is written to the file:
	// this is display-only until the user touches something.
	auto plannedTier = [&]() {
		int t = buildFile.Active().mapTier;
		if (t > 0) return t;
		return mapDb.TiersPresent().empty() ? 0 : mapDb.TiersPresent().back();
	};

	auto renderMapPanel = [&]() {
		AtlasBuildEntry& b = buildFile.Active();
		if (!mapDb.available()) {
			if (ImGui::CollapsingHeader(u8"主力地圖")) {
				ImGui::TextWrapped(u8"地圖資料未載入：%s", mapErr.c_str());
				ImGui::TextDisabled(u8"已存的地圖設定不會被更動。");
			}
			return;
		}
		if (!ImGui::CollapsingHeader(u8"主力地圖", ImGuiTreeNodeFlags_DefaultOpen)) return;

		const AtlasMapDef* cur = b.mapId.empty() ? nullptr : mapDb.ById(b.mapId);
		if (cur) {
			const float sz = 28.0f * scale;
			icons.RequestPath(cur->art);
			unsigned tex = icons.TextureByPath(cur->art);
			if (tex) {
				ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(sz, sz));
				ImGui::SameLine();
			}
			ImGui::BeginGroup();
			// The PLANNED tier leads, then the name — two separate facts, and the
			// eye should not have to split a sentence to read them.
			ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), "%s", mapTierLabel(plannedTier()).c_str());
			ImGui::SameLine(0, 8.0f * scale);
			ImGui::TextColored(PobUi::Accent(), "%s", mapPrimaryName(*cur).c_str());
			std::string sub;
			const std::string& item = mapSecondName(*cur);
			if (!item.empty() && item != mapPrimaryName(*cur)) sub = item + u8" · ";
			sub += mapRegionLabel(cur->region);
			ImGui::TextDisabled("%s", sub.c_str());
			ImGui::EndGroup();
		} else {
			ImGui::TextDisabled(u8"尚未選擇");
		}

		// --- two INDEPENDENT dropdowns: the tier to run at, and which map ---
		ImGui::TextDisabled(u8"階級");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(110.0f * scale);
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
		ImGui::TextDisabled(u8"地圖");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-FLT_MIN);
		const char* namePreview = cur ? mapPrimaryName(*cur).c_str() : u8"選擇地圖…";
		if (ImGui::BeginCombo("##mapname", namePreview)) {
			// EVERY map, always. The tier dropdown does not filter this list:
			// a Voidstone lifts a whole quadrant to T16, so a map's shipped tier
			// is no reason to hide it from a T16 plan.
			ImGui::SetNextItemWidth(-FLT_MIN);
			if (ImGui::IsWindowAppearing()) {
				mapSearch[0] = '\0';
				ImGui::SetKeyboardFocusHere();
			}
			ImGui::InputTextWithHint("##mapsearch", u8"搜尋地圖名稱…",
				mapSearch, sizeof(mapSearch), ImGuiInputTextFlags_EscapeClearsAll);
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

			ImGui::TextDisabled(u8"%d 張", (int)hits.size());
			ImGui::BeginChild("##maplist", ImVec2(340.0f * scale, 320.0f * scale));
			ImGuiListClipper clip;
			clip.Begin((int)hits.size());
			while (clip.Step()) {
				for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
					const AtlasMapDef& d = *hits[k].second;
					ImGui::PushID(k);
					// The name alone. The map's own tier goes in the tooltip, not
					// the label — showing it beside a planned tier of T16 would
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

		if (cur && ImGui::Button(u8"清除")) {
			b.mapId.clear();
			saveActive();
		}
		ImGui::Spacing();
	};

	// --- scarab section (map device) ---
	// Kept beside the atlas stats but never merged into them: these are map
	// modifiers, not passive bonuses, and their numbers do not add up with the
	// tree's. The picker filters on en+zh and only asks for the icons of the
	// rows actually on screen, so opening it does not queue 130 downloads.
	char scarabSearch[256] = "";
	auto scarabLines = [&](const ScarabDef& d) -> const std::vector<std::string>& {
		// One list or the other, never a line from each: a Description cell can
		// split into a different number of lines per locale.
		return (showZh && !d.descZh.empty()) ? d.descZh : d.descEn;
	};
	auto scarabName = [&](const ScarabDef& d) -> const std::string& {
		return (showZh && !d.zh.empty()) ? d.zh : d.en;
	};
	auto scarabRefuseText = [&](const ScarabAddResult& r) -> std::string {
		switch (r.code) {
		case ScarabAdd::kFull:
			return u8"地圖裝置只能放 " + std::to_string(kMaxScarabs) + u8" 隻甲蟲";
		case ScarabAdd::kOverLimit:
			return u8"這隻甲蟲最多只能放 " + std::to_string(r.limit) + u8" 份";
		case ScarabAdd::kFamilyConflict:
			return u8"與「" + (r.conflict ? scarabName(*r.conflict) : std::string("?")) + u8"」不能同時使用";
		default:
			return u8"這隻甲蟲不在目前的資料中";
		}
	};

	auto renderScarabPanel = [&]() {
		AtlasBuildEntry& b = buildFile.Active();
		if (!scarabDb.available()) {
			if (ImGui::CollapsingHeader(u8"甲蟲")) {
				ImGui::TextWrapped(u8"甲蟲資料未載入：%s", scarabErr.c_str());
				ImGui::TextDisabled(u8"已存的甲蟲設定不會被更動。");
			}
			return;
		}
		std::string hdr = u8"甲蟲 (" + std::to_string(b.scarabs.size()) + "/" +
		                  std::to_string(kMaxScarabs) + ")###scarabhdr";
		if (!ImGui::CollapsingHeader(hdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) return;

		const float slot = 38.0f * scale;
		int removeAt = -1;
		for (int i = 0; i < kMaxScarabs; i++) {
			if (i) ImGui::SameLine(0, 6.0f * scale);
			ImGui::PushID(i);
			if (i < (int)b.scarabs.size()) {
				const ScarabDef* d = scarabDb.ById(b.scarabs[i]);
				unsigned tex = 0;
				if (d) {
					icons.RequestPath(d->art);
					tex = icons.TextureByPath(d->art);
				}
				const ImVec2 fp = ImGui::GetStyle().FramePadding;
				bool hit = tex ? ImGui::ImageButton("##slot", (ImTextureID)(intptr_t)tex, ImVec2(slot, slot))
				               : ImGui::Button(d ? "..." : "?", ImVec2(slot + fp.x * 2, slot + fp.y * 2));
				if (hit) removeAt = i;
				if (d && ImGui::IsItemHovered()) {
					ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * scale, 10.0f * scale));
					ImGui::BeginTooltip();
					ImGui::TextColored(PobUi::Accent(), "%s", scarabName(*d).c_str());
					ImGui::PushTextWrapPos(360.0f * scale);
					for (const std::string& s : scarabLines(*d))
						ImGui::TextUnformatted(StripStatMarkup(s).c_str());
					ImGui::PopTextWrapPos();
					ImGui::TextDisabled(u8"點擊移除");
					ImGui::EndTooltip();
					ImGui::PopStyleVar();
				}
			} else {
				// ImageButton adds FramePadding around the image, so a plain
				// Button needs it too or the empty slots come out smaller.
				const ImVec2 fp = ImGui::GetStyle().FramePadding;
				if (ImGui::Button("+##add", ImVec2(slot + fp.x * 2, slot + fp.y * 2))) {
					scarabSearch[0] = '\0';
					ImGui::OpenPopup(u8"選擇甲蟲");
				}
				// The picker belongs to the first empty slot only; opening it from
				// any slot would create several popups sharing one id.
				if (ImGui::BeginPopup(u8"選擇甲蟲")) {
					ImGui::SetNextItemWidth(320.0f * scale);
					if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
					ImGui::InputTextWithHint("##scarabsearch", u8"搜尋名稱或效果（中英、模糊）…",
						scarabSearch, sizeof(scarabSearch), ImGuiInputTextFlags_EscapeClearsAll);
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
					ImGui::TextDisabled(u8"%d 隻 · %s", (int)hits.size(), scarabDb.Source().c_str());

					ImGui::BeginChild("##scarablist", ImVec2(420.0f * scale, 360.0f * scale));
					std::string lastType;
					ImGuiListClipper clip;
					clip.Begin((int)hits.size());
					while (clip.Step()) {
						for (int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
							const ScarabDef& d = *hits[k].second;
							ScarabAddResult can = scarabDb.CanAdd(b.scarabs, d.id);
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
								b.scarabs.push_back(d.id);
								saveActive();
								ImGui::CloseCurrentPopup();
							}
							if (!can.ok()) ImGui::EndDisabled();
							if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
								ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
									ImVec2(14.0f * scale, 10.0f * scale));
								ImGui::BeginTooltip();
								ImGui::TextColored(PobUi::Accent(), "%s", scarabName(d).c_str());
								ImGui::PushTextWrapPos(360.0f * scale);
								for (const std::string& s : scarabLines(d))
									ImGui::TextUnformatted(StripStatMarkup(s).c_str());
								ImGui::PopTextWrapPos();
								if (d.limit > 1)
									ImGui::TextDisabled(u8"可放置 %d 份", d.limit);
								if (!d.stash)
									ImGui::TextDisabled(u8"（未出現在碎片倉庫與交易站）");
								if (!can.ok())
									ImGui::TextColored(PobUi::StatusColor(PobUi::StatusTone::Warning),
										"%s", scarabRefuseText(can).c_str());
								ImGui::EndTooltip();
								ImGui::PopStyleVar();
							}
							ImGui::PopID();
						}
					}
					ImGui::EndChild();
					ImGui::EndPopup();
				}
				ImGui::PopID();
				break; // only the first empty slot is interactive
			}
			ImGui::PopID();
		}
		if (removeAt >= 0) {
			b.scarabs.erase(b.scarabs.begin() + removeAt);
			saveActive();
		}

		ImGui::Spacing();
		if (b.scarabs.empty()) {
			ImGui::TextDisabled(u8"尚未放置甲蟲");
		} else {
			for (const std::string& id : b.scarabs) {
				const ScarabDef* d = scarabDb.ById(id);
				if (!d) continue;
				ImGui::TextColored(PobUi::Accent(), "%s", scarabName(*d).c_str());
				ImGui::Indent(10.0f * scale);
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.80f, 0.98f, 1.0f));
				for (const std::string& s : scarabLines(*d))
					ImGui::TextWrapped("%s", StripStatMarkup(s).c_str());
				ImGui::PopStyleColor();
				ImGui::Unindent(10.0f * scale);
			}
		}
		ImGui::Spacing();
	};

	// --- project notes ---
	auto renderNotesPanel = [&]() {
		if (!ImGui::CollapsingHeader(u8"備註")) return;
		AtlasBuildEntry& b = buildFile.Active();
		ImGui::InputTextMultiline("##notes", &b.notes,
			ImVec2(-FLT_MIN, 90.0f * scale));
		// Writing on every keystroke would hit the disk once per character.
		if (ImGui::IsItemDeactivatedAfterEdit()) saveActive();
		ImGui::Spacing();
	};

	// League mechanics: the same highlight the mastery icons drive, reachable as
	// a list so you do not have to find a cluster first. Not part of the build --
	// nothing here is saved, it is a way of reading the map.
	auto renderMechanicPanel = [&]() {
		if (!ImGui::CollapsingHeader(u8"機制")) return;
		if (mechNodeIdx.empty()) {
			ImGui::TextWrapped(u8"這個賽季還沒有機制分類資料。重新下載或匯入賽季資料後就會產生。");
			ImGui::Spacing();
			return;
		}
		if (!mechDb.BorrowedFrom().empty())
			ImGui::TextDisabled(u8"（分類沿用 %s，本賽季新增的節點可能未歸類）",
				mechDb.BorrowedFrom().c_str());
		ImGui::TextDisabled(u8"點一列標出全輿圖同機制的位置；再點一次取消");
		for (const auto& kv : mechNodeIdx) {
			const AtlasMechanicDef* d = AtlasMechanicById(kv.first);
			if (!d) continue;
			const std::vector<int>* mm = mechFind(mechMastIdx, kv.first);
			std::string label = (showZh && zhLoaded ? d->zh : d->en) +
			                    "###mech_" + d->id;   // stable id, label may flip language
			ImGui::PushStyleColor(ImGuiCol_Text, kv.first == mechSel
				? ImVec4(1.0f, 0.89f, 0.43f, 1.0f) : ImVec4(0.86f, 0.88f, 0.92f, 1.0f));
			bool hit = ImGui::Selectable(label.c_str(), kv.first == mechSel);
			ImGui::PopStyleColor();
			ImGui::SameLine();
			ImGui::TextDisabled("%d", (int)kv.second.size());
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
			ImGui::TextDisabled(u8"另有 %d 個節點不屬於任何機制叢集", mechDb.Unassigned());
		ImGui::Spacing();
	};

	// Compute the compareBase -> active season diff (data-only; independent of the
	// user's allocation). Maps every changed id to a canvas node index so the
	// panel can focus it and the overlay can ring it.
	auto rebuildDiff = [&]() {
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
	};

	// Ring the changed nodes on whichever season's tree is on the canvas
	// (activeIdxById is the displayed tree): added=green, removed=red, modified=
	// amber. added only maps on the newer tree, removed only on the older one, so
	// each season shows the rings that make sense for it.
	auto applyDiffOverlay = [&]() {
		std::unordered_map<int, ImU32> rings;
		for (const AtlasNodeDiff& n : diff.added) {
			auto it = activeIdxById.find(n.id);
			if (it != activeIdxById.end()) rings[it->second] = IM_COL32(90, 220, 120, 235);
		}
		for (const AtlasNodeDiff& n : diff.removed) {
			auto it = activeIdxById.find(n.id);
			if (it != activeIdxById.end()) rings[it->second] = IM_COL32(224, 80, 80, 235);
		}
		for (const AtlasNodeDiff& n : diff.modified) {
			auto it = activeIdxById.find(n.id);
			if (it != activeIdxById.end()) rings[it->second] = IM_COL32(240, 205, 90, 235);
		}
		view.SetDiffOverlay(rings);
	};

	// Toggle helper shared by the toolbar button and hot-reload.
	auto refreshCompare = [&]() {
		rebuildDiff();
		if (diffReady) applyDiffOverlay();
		else view.ClearDiffOverlay();
	};

	// Right-hand panel body while the version-compare view is open. Lists added /
	// removed / value-changed nodes with per-line deltas; clicking a node that
	// still exists in the new season focuses it on the canvas.
	auto renderComparePanel = [&]() {
		ImGui::TextDisabled(u8"版本比較");

		// --- pick the two versions ---
		// Every installed tag is offered on both sides, so this covers both
		// "3.28 -> 3.29" (what the new league changed) and "3.29.0 -> 3.29.1"
		// (what GGG adjusted mid-league).
		std::vector<std::string> tags = verIndex.TagsNewestFirst();
		auto versionCombo = [&](const char* id, const char* caption, std::string& slot) {
			ImGui::TextDisabled("%s", caption);
			ImGui::SameLine();
			ImGui::SetNextItemWidth(110.0f * scale);
			bool changed = false;
			if (ImGui::BeginCombo(id, slot.c_str())) {
				for (const std::string& t : tags)
					if (ImGui::Selectable(t.c_str(), t == slot) && t != slot) {
						slot = t;
						changed = true;
					}
				ImGui::EndCombo();
			}
			return changed;
		};
		bool pairChanged = versionCombo("##cmpbase", u8"舊", cmpBase);
		ImGui::SameLine();
		ImGui::TextDisabled(">>");
		ImGui::SameLine();
		pairChanged |= versionCombo("##cmptarg", u8"新", cmpTarg);
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"對調")) {
			std::swap(cmpBase, cmpTarg);
			pairChanged = true;
		}
		if (pairChanged) refreshCompare();

		if (fontBig) ImGui::PushFont(fontBig);
		ImGui::TextColored(PobUi::Accent(), "%s  >>  %s", diff.oldVer.c_str(), diff.newVer.c_str());
		if (fontBig) ImGui::PopFont();
		if (!diffReady) {
			ImGui::Dummy(ImVec2(0, 12.0f * scale));
			ImGui::TextWrapped("%s", diffErr.empty() ? u8"尚未計算比較" : diffErr.c_str());
			return;
		}
		ImGui::Text(u8"新增 %d    移除 %d    數值/詞條變動 %d",
			(int)diff.added.size(), (int)diff.removed.size(), (int)diff.modified.size());
		ImGui::Spacing();
		ImGui::SetNextItemWidth(-FLT_MIN);
		ImGui::InputTextWithHint("##diffsearch", u8"搜尋變更節點或詞條…",
			diffSearch, sizeof(diffSearch), ImGuiInputTextFlags_EscapeClearsAll);
		std::string needle = ToLowerAscii(diffSearch);
		ImGui::Spacing();

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
		auto header = [&](const char* zh, size_t count) {
			return std::string(zh) + " (" + std::to_string(count) + ")";
		};

		ImGui::BeginChild("##diffscroll", ImVec2(0, 0), false, ImGuiWindowFlags_AlwaysUseWindowPadding);

		if (!diff.added.empty() &&
		    ImGui::CollapsingHeader(header(u8"新增節點", diff.added.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			for (const AtlasNodeDiff& n : diff.added) {
				if (!match(n)) continue;
				ImGui::PushID(n.id);
				bool clickable = activeIdxById.count(n.id) > 0;
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.85f, 0.55f, 1.0f));
				if (ImGui::Selectable(("+ " + (label(n).empty() ? std::string("?") : label(n))).c_str()) && clickable)
					view.CenterOn(tree, activeIdxById[n.id]);
				ImGui::PopStyleColor();
				ImGui::PopID();
			}
		}
		if (!diff.removed.empty() &&
		    ImGui::CollapsingHeader(header(u8"移除節點", diff.removed.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			for (const AtlasNodeDiff& n : diff.removed) {
				if (!match(n)) continue;
				ImGui::PushID(n.id);
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.45f, 0.45f, 1.0f));
				ImGui::Selectable(("- " + (label(n).empty() ? std::string("?") : label(n))).c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					ImGui::BeginTooltip();
					ImGui::TextDisabled(u8"此節點在新賽季已移除");
					ImGui::PushTextWrapPos(380.0f * scale);
					for (const std::string& s : n.statsOld) ImGui::TextUnformatted(StripStatMarkup(s).c_str());
					ImGui::PopTextWrapPos();
					ImGui::EndTooltip();
				}
				ImGui::PopID();
			}
		}
		if (!diff.modified.empty() &&
		    ImGui::CollapsingHeader(header(u8"數值/詞條變動", diff.modified.size()).c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
			for (const AtlasNodeDiff& n : diff.modified) {
				if (!match(n)) continue;
				ImGui::PushID(n.id);
				bool clickable = activeIdxById.count(n.id) > 0;
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.82f, 0.42f, 1.0f));
				if (ImGui::Selectable(("~ " + (label(n).empty() ? std::string("?") : label(n))).c_str()) && clickable)
					view.CenterOn(tree, activeIdxById[n.id]);
				ImGui::PopStyleColor();
				ImGui::Indent(12.0f * scale);
				ImGui::PushTextWrapPos(0.0f);
				if (n.nameChanged) {
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.60f, 0.66f, 0.74f, 1.0f));
					ImGui::TextWrapped(u8"改名自：%s",
						(showZh && zhLoaded && !n.nameOldZh.empty() ? n.nameOldZh : n.nameOld).c_str());
					ImGui::PopStyleColor();
				}
				for (const AtlasStatDelta& d : n.stats) {
					if (d.kind == AtlasStatDelta::kValueChanged) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.58f, 0.63f, 0.72f, 1.0f));
						ImGui::TextWrapped("  %s", lineOld(d).c_str());
						ImGui::PopStyleColor();
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.72f, 0.80f, 0.98f, 1.0f));
						ImGui::TextWrapped("=> %s", lineNew(d).c_str());
						ImGui::PopStyleColor();
					} else if (d.kind == AtlasStatDelta::kLineAdded) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.50f, 0.85f, 0.55f, 1.0f));
						ImGui::TextWrapped("+ %s", lineNew(d).c_str());
						ImGui::PopStyleColor();
					} else {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.90f, 0.45f, 0.45f, 1.0f));
						ImGui::TextWrapped("- %s", lineNew(d).c_str());
						ImGui::PopStyleColor();
					}
				}
				ImGui::PopTextWrapPos();
				ImGui::Unindent(12.0f * scale);
				ImGui::Spacing();
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	};

	// hot-reload after new data landed on disk (manual import or auto update);
	// GL work, main thread only
	auto hotReload = [&](const std::string& okMsg) {
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
	};

	// convert + hot-reload; keeps the old data untouched when conversion fails
	auto importSeason = [&]() {
		std::wstring path = OpenDataJsonDialog();
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
	};

	bool running = true;
	while (running) {
		glfwPollEvents();

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

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		ImGui::PushFont(font);

		ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::Begin("##atlasplanner", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

		if (!ready) {
			ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f), u8"輿圖資料載入失敗：%s", loadErr.c_str());
			ImGui::TextDisabled(u8"請確認 Data\\atlas_tree_poe1.json 與 Data\\atlas\\ 圖集存在，或直接匯入新資料。");
			if (ImGui::Button(u8"匯入賽季資料")) importSeason();
			// the auto updater doubles as the recovery path when no data exists
			if (ust.phase == AtlasUpdatePhase::UpdateAvailable) {
				ImGui::SameLine();
				if (ImGui::Button((u8"自動下載 " + ust.latestTag).c_str())) updater.StartUpdate();
			} else if (ust.phase == AtlasUpdatePhase::Downloading || ust.phase == AtlasUpdatePhase::Importing ||
			           ust.phase == AtlasUpdatePhase::Checking) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s", ust.message.c_str());
			} else if (ust.phase == AtlasUpdatePhase::Error) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f), u8"更新失敗：%s", ust.message.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton(u8"重試")) updater.StartUpdate();
			}
			if (!importMsg.empty()) {
				ImGui::SameLine();
				ImGui::TextColored(importFailed ? ImVec4(0.94f, 0.27f, 0.27f, 1.0f)
				                                : ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "%s", importMsg.c_str());
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
			auto importEntry = [&](const AtlasBuildEntry& e) {
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
					importMsg += u8"、" + std::to_string(buildFile.builds[idx].scarabs.size()) + u8" 隻甲蟲";
				if (!buildFile.builds[idx].notes.empty()) importMsg += u8"、備註";
				importMsg += snote; // "，忽略 N 個未知甲蟲" etc., empty when nothing was dropped
				importFailed = false;
			};

			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(PobUi::Accent(), u8"輿圖策略");
			ImGui::SameLine(0, 18.0f * scale);
			ImGui::TextDisabled(u8"專案");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(220.0f * scale);
			if (ImGui::BeginCombo("##buildsel", buildFile.Active().name.c_str())) {
				for (int i = 0; i < (int)buildFile.builds.size(); i++) {
					bool sel = i == buildFile.active;
					ImGui::PushID(i);
					if (ImGui::Selectable(buildFile.builds[i].name.c_str(), sel) && !sel)
						switchTo(i);
					ImGui::PopID();
				}
				ImGui::EndCombo();
			}
			ImGui::SameLine();
			if (ImGui::Button(u8"＋ 新增")) {
				nameBuf = u8"新專案";
				ImGui::OpenPopup(u8"新增專案");
			}
			ImGui::SameLine();
			if (ImGui::Button(u8"重新命名")) {
				nameBuf = buildFile.Active().name;
				ImGui::OpenPopup(u8"重新命名專案");
			}
			ImGui::SameLine();
			if (ImGui::Button(u8"複製")) {
				saveActive();
				int idx = buildFile.DuplicateBuild(buildFile.active);
				if (idx >= 0) switchTo(idx);
			}
			ImGui::SameLine();
			bool lastBuild = buildFile.builds.size() <= 1;
			if (lastBuild) ImGui::BeginDisabled();
			PobUi::PushDangerButton();
			if (ImGui::Button(u8"刪除")) ImGui::OpenPopup(u8"刪除專案確認");
			PobUi::PopButtonStyle();
			if (lastBuild) ImGui::EndDisabled();
			ImGui::SameLine(0, 24.0f * scale);
			if (ImGui::Button(u8"匯出")) ImGui::OpenPopup("##exportmenu");
			ImGui::SameLine();
			if (ImGui::Button(u8"匯入")) ImGui::OpenPopup("##importmenu");

			if (ImGui::BeginPopupModal(u8"新增專案", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::TextUnformatted(u8"專案名稱：");
				ImGui::SetNextItemWidth(260.0f * scale);
				ImGui::InputText("##newname", &nameBuf);
				ImGui::Spacing();
				if (ImGui::Button(u8"建立（空白配點）")) {
					saveActive();
					int idx = buildFile.AddBuild(nameBuf);
					switchTo(idx); // new project starts empty
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"取消")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			if (ImGui::BeginPopupModal(u8"重新命名專案", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::TextUnformatted(u8"新名稱：");
				ImGui::SetNextItemWidth(260.0f * scale);
				ImGui::InputText("##rename", &nameBuf);
				ImGui::Spacing();
				if (ImGui::Button(u8"確定")) {
					if (!nameBuf.empty() && nameBuf != buildFile.Active().name)
						buildFile.Active().name = buildFile.UniqueName(nameBuf);
					buildFile.Save(exeDir);
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"取消")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			if (ImGui::BeginPopupModal(u8"刪除專案確認", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::Text(u8"確定要刪除專案「%s」嗎？此動作無法復原。", buildFile.Active().name.c_str());
				ImGui::Spacing();
				if (ImGui::Button(u8"刪除")) {
					if (buildFile.RemoveBuild(buildFile.active)) {
						tree.ApplyAllocIds(buildFile.Active().alloc);
						tree.ApplyTargetIds(buildFile.Active().targets);
						tree.ApplyBlockedIds(buildFile.Active().blocked);
						undo.valid = false;
						saveActive();
						panelDirty = true;
					}
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"取消")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			if (ImGui::BeginPopup("##exportmenu")) {
				if (ImGui::MenuItem(u8"另存 .json 檔…")) {
					saveActive();
					std::wstring path = SaveBuildJsonDialog(buildFile.Active().name);
					if (!path.empty()) {
						bool ok = PlannerWriteFile(path, AtlasExportJson(buildFile.Active(), tree.Version()));
						importMsg = ok ? u8"專案已匯出" : u8"匯出檔寫入失敗";
						importFailed = !ok;
					}
				}
				if (ImGui::MenuItem(u8"複製分享碼")) {
					saveActive();
					std::string code = AtlasBuildShareCode(buildFile.Active(), tree.Version());
					if (!code.empty()) {
						ImGui::SetClipboardText(code.c_str());
						importMsg = u8"分享碼已複製到剪貼簿";
						importFailed = false;
					}
				}
				ImGui::EndPopup();
			}
			if (ImGui::BeginPopup("##importmenu")) {
				if (ImGui::MenuItem(u8"開啟 .json 檔…")) {
					std::wstring path = OpenBuildJsonDialog();
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
				}
				if (ImGui::MenuItem(u8"從剪貼簿貼上分享碼")) {
					const char* clip = ImGui::GetClipboardText();
					std::string perr;
					AtlasBuildEntry e;
					if (clip && AtlasParseShareCode(clip, &e, &perr)) {
						importEntry(e);
					} else {
						importMsg = perr.empty() ? u8"剪貼簿沒有內容" : perr;
						importFailed = true;
					}
				}
				ImGui::EndPopup();
			}

			// --- allocation toolbar ---
			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();
			int used = tree.UsedPoints(), total = tree.TotalPoints();
			ImGui::AlignTextToFramePadding();
			ImGui::TextDisabled(u8"已用點數");
			ImGui::SameLine();
			ImVec4 cnt = PobUi::StatusColor(used >= total ? PobUi::StatusTone::Warning : PobUi::StatusTone::Success);
			ImGui::TextColored(cnt, "%d / %d", used, total);
			ImGui::SameLine(0, 24.0f * scale);
			if (ImGui::Button(u8"重置")) ImGui::OpenPopup(u8"重置配點");
			ImGui::SameLine();
			// Minimum-point compression on demand. Clicking never re-routes any
			// more (see atlas_view.cpp), so this is the only thing that moves
			// wiring, and only when asked.
			{
				const bool canCompress = ready && !planningMode && !compareMode &&
				                         onCanonicalSeason() && tree.UsedPoints() > 0;
				if (!canCompress) ImGui::BeginDisabled();
				if (ImGui::Button(u8"壓縮到最少點")) {
					std::string msg;
					importFailed = !applyCompress(msg);
					importMsg = msg;
					panelDirty = true;
				}
				if (!canCompress) ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
					ImGui::SetTooltip(u8"用最少的天賦點重新接一次目前的節點。\n"
					                  u8"大點、鑰石與端點都會保留，只有中間繞路的小點會改道；\n"
					                  u8"平常點節點不會自動重算，按了才會動，Ctrl+Z 可復原。");
			}
			ImGui::SameLine();
			bool updaterBusy = ust.phase == AtlasUpdatePhase::Downloading ||
			                   ust.phase == AtlasUpdatePhase::Importing;
			if (updaterBusy) ImGui::BeginDisabled(); // both paths overwrite the same tree json
			if (ImGui::Button(u8"匯入賽季資料")) ImGui::OpenPopup(u8"匯入賽季資料確認");
			if (updaterBusy) ImGui::EndDisabled();

			// zh/en display toggle (only when a mapping is available); F2 mirrors it
			if (zhLoaded) {
				ImGui::SameLine();
				if (ImGui::Button(showZh ? "EN" : u8"中")) showZh = !showZh;
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(u8"切換節點中/英顯示（F2）· 對照 %s", i18n.VersionNote().c_str());
			}
			if (zhLoaded && ImGui::IsKeyPressed(ImGuiKey_F2, false)) showZh = !showZh;

			// season switcher: which league's atlas tree is drawn on the canvas
			if (verIndex.Versions().size() >= 2 && !viewTag.empty()) {
				ImGui::SameLine(0, 18.0f * scale);
				ImGui::TextDisabled(u8"賽季");
				ImGui::SameLine();
				ImGui::SetNextItemWidth(96.0f * scale);
				if (ImGui::BeginCombo("##seasonsel", viewTag.c_str())) {
					for (const std::string& t : verIndex.TagsNewestFirst()) {
						bool sel = t == viewTag;
						if (ImGui::Selectable(t.c_str(), sel) && !sel) {
							saveActive();                 // capture edits on the outgoing canonical season
							loadSeason(t);
							uiState.season = t;
							uiState.Save(exeDir);
							panelDirty = true;
							if (compareMode) refreshCompare(); // rebuild overlay/index for the new tree
						}
					}
					ImGui::EndCombo();
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(u8"切換畫布顯示的賽季輿圖樹（配點以節點 ID 跨季共用）");
				if (!onCanonicalSeason()) {
					ImGui::SameLine();
					ImGui::TextColored(ImVec4(0.90f, 0.65f, 0.25f, 1.0f), u8"舊賽季檢視（唯讀）");
				}
			}

			// sandbox planning toggle (not available while comparing seasons:
			// that view is a read-only preview of a different tree)
			{
				ImGui::SameLine(0, 18.0f * scale);
				if (compareMode) ImGui::BeginDisabled();
				// Latch the flag BEFORE the button: the click handler flips the
				// very flag the Push/Pop is keyed on (enterPlanning sets
				// planningMode), so testing it again afterwards pops a style that
				// was never pushed on one edge, and leaks a pushed style on the
				// other — which then tints every later button on the toolbar.
				const bool wasPlanning = planningMode;
				if (wasPlanning) PobUi::PushDangerButton();
				if (ImGui::Button(wasPlanning ? u8"結束規劃" : u8"規劃模式")) {
					if (planningMode) planningAskExit = true;   // ask before deciding
					else { enterPlanning(); panelDirty = true; }
				}
				if (wasPlanning) PobUi::PopButtonStyle();
				if (compareMode) ImGui::EndDisabled();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(planningMode
						? u8"結束並選擇是否保留這次規劃"
						: u8"沙盒：從空白開始快速標記想要／不要的節點，"
						  u8"期間不會寫入存檔，結束時再決定要不要保留");
			}

			// version-compare toggle (needs two installed seasons)
			{
				bool canCompare = verIndex.Versions().size() >= 2 && !verIndex.CompareBase().empty() &&
				                  !planningMode;
				ImGui::SameLine(0, 18.0f * scale);
				if (!canCompare) ImGui::BeginDisabled();
				// Same latching rule as the planning button above — this one is
				// what actually leaked: leaving compare mode pushed the danger
				// style and never popped it, so every button drawn afterwards
				// stayed red until the window was reopened.
				const bool wasCompare = compareMode;
				if (wasCompare) PobUi::PushDangerButton();
				if (ImGui::Button(wasCompare ? u8"結束比較" : u8"版本比較")) {
					compareMode = !compareMode;
					if (compareMode) refreshCompare();
					else view.ClearDiffOverlay();
				}
				if (wasCompare) PobUi::PopButtonStyle();
				if (!canCompare) ImGui::EndDisabled();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(canCompare
						? u8"比較任兩個已安裝版本（可選）：節點增刪與逐詞條數值變更"
						  u8"（綠=新增, 紅=移除, 黃=變動）\n"
						  u8"當季各小版本都會保留，所以也能比 %s 這種賽季中的官方調整\n"
						  u8"目前已安裝 %d 個版本"
						: u8"需要兩個版本的資料才能比較",
						(verIndex.Versions().size() >= 2 ? u8"3.29.0 -> 3.29.1" : ""),
						(int)verIndex.Versions().size());
			}

			// background auto-update status / prompt
			if (ust.phase == AtlasUpdatePhase::UpdateAvailable) {
				ImGui::SameLine(0, 24.0f * scale);
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.60f, 0.20f, 0.45f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.60f, 0.20f, 0.65f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.60f, 0.20f, 0.85f));
				if (ImGui::Button((u8"發現新版 " + ust.latestTag + u8"，點擊更新").c_str()))
					updater.StartUpdate();
				ImGui::PopStyleColor(3);
			} else if (updaterBusy || ust.phase == AtlasUpdatePhase::Checking) {
				ImGui::SameLine(0, 24.0f * scale);
				ImGui::TextDisabled("%s", ust.message.c_str());
			} else if (ust.phase == AtlasUpdatePhase::Error) {
				ImGui::SameLine(0, 24.0f * scale);
				ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f), u8"更新失敗：%s", ust.message.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton(u8"重試")) updater.StartUpdate();
			}

			ImGui::Spacing();
			ImGui::TextDisabled(u8"滾輪縮放  ·  拖曳平移  ·  左鍵配點或移除"
			                    u8"  ·  點叢集中央的機制圖示可標出同機制的所有位置");
			// Which mechanic is lit up right now, and how to turn it off. Without
			// this the rings look like part of the tree.
			if (!mechSel.empty()) {
				const AtlasMechanicDef* d = AtlasMechanicById(mechSel);
				const std::vector<int>* nn = mechFind(mechNodeIdx, mechSel);
				const std::vector<int>* mm = mechFind(mechMastIdx, mechSel);
				ImGui::SameLine(0, 18.0f * scale);
				ImGui::TextColored(ImVec4(1.0f, 0.89f, 0.43f, 1.0f), u8"機制：%s（%d 節點 · %d 叢集）",
					d ? (showZh && zhLoaded ? d->zh.c_str() : d->en.c_str()) : mechSel.c_str(),
					nn ? (int)nn->size() : 0, mm ? (int)mm->size() : 0);
				ImGui::SameLine();
				if (ImGui::SmallButton(u8"清除##mech")) { mechSel.clear(); applyMechHighlight(); }
			}
			if (!importMsg.empty()) {
				ImGui::SameLine(0, 18.0f * scale);
				ImGui::TextColored(PobUi::StatusColor(importFailed ? PobUi::StatusTone::Error : PobUi::StatusTone::Success),
					"%s", importMsg.c_str());
			} else if (!view.StatusLine().empty()) {
				ImGui::SameLine(0, 18.0f * scale);
				ImGui::TextDisabled("%s", view.StatusLine().c_str());
			}

			if (ImGui::BeginPopupModal(u8"匯入賽季資料確認", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::TextUnformatted(u8"從 GGG 官方 atlastree-export 匯入新賽季的輿圖天賦樹：");
				ImGui::TextDisabled(u8"1. 到 github.com/grindinggear/atlastree-export 下載（Code → Download ZIP）並解壓縮");
				ImGui::TextDisabled(u8"2. 選取解壓縮後資料夾內的 data.json（assets 資料夾需在旁邊）");
				ImGui::TextDisabled(u8"匯入會覆寫目前的樹資料；已配置的節點會依 ID 對映，消失的節點自動移除。");
				ImGui::Spacing();
				if (ImGui::Button(u8"選擇 data.json")) {
					ImGui::CloseCurrentPopup();
					importSeason();
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"立即檢查更新")) {
					ImGui::CloseCurrentPopup();
					updater.RequestCheck(true); // manual entry: skip the daily throttle
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"取消")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}
			if (!cjkOk) {
				ImGui::SameLine(0, 24.0f * scale);
				ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f),
					"[!] CJK font atlas not loaded (Fonts\\FZ_ZY.ttf).");
			}

			// Leaving the sandbox: the ONLY place a planning session can reach
			// the build file. Both outcomes are explicit -- there is no default
			// action on a stray click, and no path that writes without asking.
			if (planningAskExit) { ImGui::OpenPopup(u8"結束規劃"); planningAskExit = false; }
			if (ImGui::BeginPopupModal(u8"結束規劃", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				int want = (int)tree.TargetIdx().size();
				int avoid = (int)tree.BlockedIdx().size();
				ImGui::TextUnformatted(u8"要保留這次規劃嗎？");
				ImGui::Spacing();
				ImGui::Text(u8"目前 %d 點 · 想要 %d 個 · 排除 %d 個", tree.UsedPoints(), want, avoid);
				ImGui::TextDisabled(u8"保留會覆蓋這個專案原本的 %d 點配置。", (int)planSnapshot.alloc.size());
				ImGui::Spacing();
				if (ImGui::Button(u8"保留並儲存")) { keepPlanning(); panelDirty = true; ImGui::CloseCurrentPopup(); }
				ImGui::SameLine();
				if (ImGui::Button(u8"捨棄")) { restorePlanning(); panelDirty = true; ImGui::CloseCurrentPopup(); }
				ImGui::SameLine();
				if (ImGui::Button(u8"繼續規劃")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}

			if (ImGui::BeginPopupModal(u8"重置配點", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::TextUnformatted(u8"確定要清除所有已配置的節點嗎？");
				ImGui::Spacing();
				if (ImGui::Button(u8"清除全部")) {
					tree.Reset();
					saveActive();
					panelDirty = true;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"取消")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}

			ImGui::Separator();

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
				                               : std::clamp(io.DisplaySize.x * 0.35f, 380.0f * scale, 700.0f * scale);
			panelW = std::clamp(panelW, 320.0f * scale, io.DisplaySize.x * 0.6f);
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
					IM_COL32(99, 102, 241, ImGui::IsItemActive() ? 220 : 120), 2.0f);
			}
			if (ImGui::IsItemActive())
				panelW = std::clamp(panelW - io.MouseDelta.x, 320.0f * scale, io.DisplaySize.x * 0.6f);
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

			// --- points summary (pinned) ---
			ImGui::TextDisabled(u8"已配置點數");
			if (fontBig) ImGui::PushFont(fontBig);
			ImGui::TextColored(cnt, "%d / %d", used, total);
			if (fontBig) ImGui::PopFont();
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram,
				used >= total ? PobUi::StatusColor(PobUi::StatusTone::Warning)
				              : PobUi::Accent());
			ImGui::ProgressBar(total > 0 ? (float)used / (float)total : 0.0f,
				ImVec2(-FLT_MIN, 8.0f * scale), "");
			ImGui::PopStyleColor();
			{
				int nTargets = (int)tree.TargetIdx().size();
				int wiring = used - nTargets;
				if (wiring < 0) wiring = 0;   // (windows.h's max macro is in scope here)
				ImGui::TextDisabled(u8"自己點的 %d 個 · 連接用 %d 點", nTargets, wiring);
				if (nTargets > AtlasOptExactCap()) {
					ImGui::SameLine(0, 8.0f * scale);
					ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), u8"近似解");
					if (ImGui::IsItemHovered())
						ImGui::SetTooltip(u8"超過 %d 個時，「壓縮到最少點」與規劃模式改用近似演算法，\n"
							u8"可能比真正的最少點多幾點。平常點節點不受影響。",
							AtlasOptExactCap());
				}
				if (undo.valid) {
					ImGui::SameLine(0, 10.0f * scale);
					ImGui::TextDisabled(u8"Ctrl+Z 復原");
				}
			}
			ImGui::Spacing();


			// --- search (pinned; filters stats AND the node list, en + zh) ---
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputTextWithHint("##panelsearch", u8"搜尋統計或節點…",
				panelSearch, sizeof(panelSearch), ImGuiInputTextFlags_EscapeClearsAll);
			std::string needle = ToLowerAscii(panelSearch);
			auto matches = [&](const std::string& key) {
				return needle.empty() || key.find(needle) != std::string::npos;
			};
			ImGui::Spacing();

			// --- scrolling content ---
			// borderless children skip WindowPadding by default; force it so
			// text keeps a margin from the panel edges
			ImGui::BeginChild("##panelscroll", ImVec2(0, 0), false,
				ImGuiWindowFlags_AlwaysUseWindowPadding);
			// Quadrants, then the map, then the device that map goes into, then
			// notes. All show even with nothing allocated: they are useful
			// before a single node is picked.
			renderAstrolabePanel();
			renderMapPanel();
			renderScarabPanel();
			renderNotesPanel();
			renderMechanicPanel();
			bool anyAlloc = false;
			for (const auto& g : nodeGroups) anyAlloc = anyAlloc || !g.empty();
			if (!anyAlloc) {
				ImGui::Dummy(ImVec2(0, 30.0f * scale));
				ImGui::TextDisabled(u8"尚未配置任何節點");
				ImGui::TextWrapped(u8"在左側輿圖上點擊節點開始規劃；滾輪縮放、拖曳平移。");
			} else {
				if (ImGui::CollapsingHeader(u8"加成統計", ImGuiTreeNodeFlags_DefaultOpen)) {
					const std::vector<int>& order = (showZh && zhLoaded) ? statOrderZh : statOrderEn;
					int shown = 0;
					for (int gi : order) {
						const StatAggGroup& g = statAgg[gi];
						if (!matches(g.searchKey)) continue;
						shown++;
						const std::string& disp = (showZh && zhLoaded) ? g.dispZh : g.dispEn;
						if (g.kind == StatAggGroup::kMulti && g.count > 1) {
							ImGui::TextColored(ImVec4(0.55f, 0.80f, 0.95f, 1.0f), "x%d", g.count);
							ImGui::SameLine(0, 6.0f * scale);
						}
						ImVec4 col = g.kind == StatAggGroup::kSummed
							? ImVec4(0.72f, 0.80f, 0.98f, 1.0f)   // summed value rows pop
							: g.kind == StatAggGroup::kBoolean
							? ImVec4(0.62f, 0.67f, 0.74f, 1.0f)   // boolean effects recede
							: ImVec4(0.973f, 0.980f, 0.988f, 1.0f);
						ImGui::PushStyleColor(ImGuiCol_Text, col);
						ImGui::TextWrapped("%s", disp.c_str());
						ImGui::PopStyleColor();
					}
					if (shown == 0)
						ImGui::TextDisabled(u8"沒有符合搜尋的統計");
					ImGui::Spacing();
				}
				if (ImGui::CollapsingHeader(u8"節點清單", ImGuiTreeNodeFlags_DefaultOpen)) {
					for (int r = 0; r < 4; r++) {
						if (nodeGroups[r].empty()) continue;
						int m = 0;
						for (const PanelNode& p : nodeGroups[r])
							if (matches(p.searchKey)) m++;
						if (m == 0 && !needle.empty()) continue;
						// group header: 4px color bar (DrawList; the font atlas
						// has no "●" glyph) + name + count
						ImDrawList* pdl = ImGui::GetWindowDrawList();
						ImVec2 hp = ImGui::GetCursorScreenPos();
						float lh = ImGui::GetTextLineHeight();
						pdl->AddRectFilled(ImVec2(hp.x, hp.y + lh * 0.20f),
							ImVec2(hp.x + 4.0f * scale, hp.y + lh * 0.95f),
							ImGui::GetColorU32(kKindCol[r]), 2.0f * scale);
						ImGui::Dummy(ImVec2(9.0f * scale, 0));
						ImGui::SameLine(0, 0);
						if (needle.empty())
							ImGui::TextDisabled("%s (%d)", kGroupName[r], (int)nodeGroups[r].size());
						else
							ImGui::TextDisabled("%s (%d/%d)", kGroupName[r], m, (int)nodeGroups[r].size());
						ImGui::Indent(10.0f * scale);
						for (const PanelNode& p : nodeGroups[r]) {
							if (!matches(p.searchKey)) continue;
							const AtlasNode& n = tree.nodes[p.idx];
							const std::string& nm = showZh && zhLoaded ? i18n.NodeName(n.id, n.name) : n.name;
							ImGui::PushID(p.idx); // duplicate display names exist
							ImGui::PushStyleColor(ImGuiCol_Text, kKindCol[r]);
							if (ImGui::Selectable(nm.empty() ? u8"(未命名)" : nm.c_str(), false))
								view.CenterOn(tree, p.idx);
							ImGui::PopStyleColor();
							if (ImGui::IsItemHovered()) {
								ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * scale, 12.0f * scale));
								ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(460.0f * scale, FLT_MAX));
								ImGui::BeginTooltip();
								ImGui::TextColored(kKindCol[r], "%s", nm.empty() ? u8"(未命名)" : nm.c_str());
								// 明確 wrap 寬度,不讓短標題把 tooltip 寬度撐死(同 atlas_view drawTooltip)
								ImGui::PushTextWrapPos(380.0f * scale);
								for (const std::string& s : n.stats) {
									ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.68f, 0.90f, 1.0f));
									ImGui::TextUnformatted(StripStatMarkup(showZh && zhLoaded ? i18n.StatLine(s) : s).c_str());
									ImGui::PopStyleColor();
								}
								ImGui::PopTextWrapPos();
								ImGui::TextDisabled(u8"點擊清單以在輿圖中定位");
								ImGui::EndTooltip();
								ImGui::PopStyleVar();
							}
							ImGui::PopID();
						}
						ImGui::Unindent(10.0f * scale);
						ImGui::Spacing();
					}
				}
			}
			ImGui::EndChild();
			} // end normal side panel (else branch of compareMode)
			ImGui::EndChild();
		}

		ImGui::End();

		if (glfwWindowShouldClose(win)) {
			// Closing the window mid-plan must not silently drop the work, and
			// must not silently keep it either -- ask, same as the button does.
			if (planningMode) { planningAskExit = true; glfwSetWindowShouldClose(win, 0); }
			else running = false;
		}

		ImGui::PopFont();
		ImGui::Render();
		int fbW = 0, fbH = 0;
		glfwGetFramebufferSize(win, &fbW, &fbH);
		glViewport(0, 0, fbW, fbH);
		glClearColor(0.031f, 0.035f, 0.047f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		glfwSwapBuffers(win);
	}

	updater.Shutdown();     // cancels any in-flight download, joins the worker
	icons.Shutdown();       // joins the icon worker, deletes its textures (GL thread)
	view.DestroyTextures(); // while the GL context is still current
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();
}
