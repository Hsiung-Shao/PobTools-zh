// must precede every imgui.h include (atlas_view.h pulls it in)
#define IMGUI_DEFINE_MATH_OPERATORS

#include "atlas_planner.h"
#include "atlas_diff.h"
#include "atlas_i18n.h"
#include "atlas_import.h"
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
#include <cmath>
#include <deque>
#include <functional>
#include <numeric>
#include <queue>
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

static const wchar_t* kBuildJsonFilter = L"輿圖配點專案 (*.json)\0*.json\0所有檔案 (*.*)\0*.*\0\0";

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
		MessageBoxW(nullptr, L"無法初始化 GLFW，輿圖配點器無法顯示。", L"PobTools", MB_ICONERROR | MB_OK);
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

	GLFWwindow* win = glfwCreateWindow(winW, winH, u8"PobTools — 輿圖配點器", nullptr, nullptr);
	if (!win) {
		glfwTerminate();
		MessageBoxW(nullptr, L"無法建立輿圖配點器視窗。", L"PobTools", MB_ICONERROR | MB_OK);
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
	for (AtlasBuildEntry& b : buildFile.builds)
		b.scarabs = scarabDb.Sanitize(b.scarabs, nullptr);

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
	// viewTag = the season currently on the canvas (persisted choice, else active)
	std::string viewTag = (!uiState.season.empty() && verIndex.Has(uiState.season))
		? uiState.season : verIndex.Active();

	int startupDropped = 0;  // nodes lost because the saved build predates this season
	bool ready = false;      // set by the initial loadSeason() below
	std::string importMsg;   // last import result shown in the toolbar
	bool importFailed = false;
	bool planningMode = false;
	bool planningPaused = false;
	bool planningDirty = false;
	std::vector<char> planningPref; // 0 neutral, 1 desired, 2 undesired
	std::vector<char> planningGroupPref; // aligned with tree.masteries
	std::string planningMsg;

	// The newest installed season is canonical (the one the build persists for);
	// an older-season view is a read-only preview.
	auto onCanonicalSeason = [&]() {
		return verIndex.Active().empty() || viewTag == verIndex.Active();
	};
	// Capture the allocation only on the canonical season, so a preview of an
	// older season never prunes it back to that season's subset. The file is
	// still written either way: notes and scarabs are season-independent, and
	// before they existed an edit made while previewing was simply lost.
	auto saveActive = [&]() {
		if (onCanonicalSeason()) {
			buildFile.Active().alloc = tree.AllocIds();
			buildFile.version = tree.Version();
		}
		buildFile.Save(exeDir);
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
			planningPref.assign(tree.nodes.size(), 0);
			planningGroupPref.assign(tree.masteries.size(), 0);
			planningDirty = false;
			planningMsg.clear();
		}
	};

	// initial load of the chosen season
	loadSeason(viewTag);
	showZh = zhLoaded;                   // default Chinese when a mapping exists
	if (startupDropped > 0)
		importMsg = u8"提醒：此配置存於舊版輿圖樹，" + std::to_string(startupDropped) +
		            u8" 個已不存在的節點已自動移除";

	// --- version-compare state (compareBase season -> active season) ---
	bool compareMode = false;
	AtlasTreeDiff diff;
	bool diffReady = false;
	std::string diffErr;
	char diffSearch[256] = "";
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

	auto rebuildPlanningAllocation = [&]() -> bool {
		if (!planningMode || planningPaused || !ready) return false;
		if ((int)planningPref.size() != (int)tree.nodes.size())
			planningPref.assign(tree.nodes.size(), 0);

		std::vector<char> desired(tree.nodes.size(), 0), blocked(tree.nodes.size(), 0);
		int desiredCount = 0, blockedCount = 0;
		for (int i = 0; i < (int)tree.nodes.size(); i++) {
			if (tree.nodes[i].kind == kAtlasStart) continue;
			if (planningPref[i] == 2) { blocked[i] = 1; blockedCount++; }
			else if (planningPref[i] == 1) { desired[i] = 1; desiredCount++; }
		}

		std::vector<char> before(tree.nodes.size(), 0);
		for (int i = 0; i < (int)tree.nodes.size(); i++) before[i] = tree.nodes[i].alloc ? 1 : 0;
		tree.Reset();

		auto containsAscii = [](const std::string& text, const char* needle) {
			std::string hay = ToLowerAscii(text);
			std::string nd = ToLowerAscii(needle);
			return hay.find(nd) != std::string::npos;
		};
		auto hasStat = [&](const AtlasNode& n, const char* needle) {
			for (const std::string& stat : n.stats)
				if (containsAscii(stat, needle)) return true;
			return false;
		};
		auto isTravelNode = [&](const AtlasNode& n) {
			return hasStat(n, "chance for map drops to be duplicated") ||
			       hasStat(n, "increased quantity of items found in your maps") ||
			       hasStat(n, "increased scarabs found in your maps") ||
			       hasStat(n, "increased effect of modifiers on your maps") ||
			       hasStat(n, "additional connected map");
		};
		auto nodeWeight = [&](int idx) {
			if (idx < 0 || idx >= (int)tree.nodes.size()) return 1.0;
			const AtlasNode& n = tree.nodes[idx];
			double w = 1.0;
			if (n.kind == kAtlasNotable || n.kind == kAtlasKeystone) w = 0.5;
			if (isTravelNode(n)) w -= 0.01;
			else if (n.kind == kAtlasNormal) {
				for (int nb : n.adj) {
					if (nb >= 0 && nb < (int)tree.nodes.size() && isTravelNode(tree.nodes[nb])) {
						w -= 0.01;
						break;
					}
				}
			}
			if (n.kind == kAtlasWormhole) w = (std::max)(w, 1.5);
			return (std::max)(0.05, w);
		};

		struct PlanPath {
			std::vector<int> nodes; // origin first, target last
			int originGroup = -1;
			int targetGroup = -1;
			double cost = 0.0;
			double overlap = 0.0;
		};
		auto mergeGroups = [](std::vector<std::vector<int>>& groups, int a, int b, const std::vector<int>& path) {
			if (a < 0 || b < 0 || a >= (int)groups.size() || b >= (int)groups.size() || a == b) return;
			std::vector<char> seen;
			int maxIdx = 0;
			for (const auto& g : groups)
				for (int n : g) maxIdx = (std::max)(maxIdx, n);
			for (int n : path) maxIdx = (std::max)(maxIdx, n);
			seen.assign(maxIdx + 1, 0);
			std::vector<int> merged;
			auto add = [&](int n) {
				if (n < 0) return;
				if (n >= (int)seen.size()) seen.resize(n + 1, 0);
				if (!seen[n]) { seen[n] = 1; merged.push_back(n); }
			};
			for (int n : groups[a]) add(n);
			for (int n : groups[b]) add(n);
			for (int n : path) add(n);
			if (a < b) {
				groups.erase(groups.begin() + b);
				groups.erase(groups.begin() + a);
			} else {
				groups.erase(groups.begin() + a);
				groups.erase(groups.begin() + b);
			}
			groups.push_back(std::move(merged));
		};

		std::vector<std::vector<int>> planGroups;
		auto mergeAdjacentGroups = [&]() {
			bool merged = true;
			while (merged) {
				merged = false;
				std::vector<int> member(tree.nodes.size(), -1);
				for (int gi = 0; gi < (int)planGroups.size(); gi++)
					for (int n : planGroups[gi])
						if (n >= 0 && n < (int)member.size()) member[n] = gi;
				for (int i = 0; i < (int)tree.nodes.size() && !merged; i++) {
					int gi = member[i];
					if (gi == -1) continue;
					for (int nb : tree.nodes[i].adj) {
						int gj = (nb >= 0 && nb < (int)member.size()) ? member[nb] : -1;
						if (gj != -1 && gj != gi) {
							mergeGroups(planGroups, gi, gj, {});
							merged = true;
							break;
						}
					}
				}
			}
		};

		if (tree.root >= 0) planGroups.push_back({ tree.root });
		for (int i = 0; i < (int)tree.nodes.size(); i++) {
			if (!desired[i] || blocked[i]) continue;
			tree.nodes[i].alloc = true;
			planGroups.push_back({ i });
		}
		mergeAdjacentGroups();

		auto shortestPathsFromGroup = [&](const std::vector<int>& group, const std::vector<int>& member) {
			const double kInf = 1e30;
			std::vector<double> dist(tree.nodes.size(), kInf);
			std::vector<int> prev(tree.nodes.size(), -1);
			struct QItem { double dist; int idx; };
			struct Greater { bool operator()(const QItem& a, const QItem& b) const { return a.dist > b.dist; } };
			std::priority_queue<QItem, std::vector<QItem>, Greater> pq;
			for (int n : group) {
				if (n < 0 || n >= (int)tree.nodes.size() || blocked[n]) continue;
				dist[n] = 0.0;
				pq.push({ 0.0, n });
			}

			std::vector<PlanPath> found;
			double best = kInf;
			while (!pq.empty()) {
				QItem item = pq.top();
				pq.pop();
				if (item.dist != dist[item.idx]) continue;
				if (item.dist > best * 2.0 + 0.001) break;
				int itemGroup = member[item.idx];
				if (itemGroup != -1 && itemGroup != member[group[0]]) {
					std::vector<int> path;
					for (int at = item.idx; at != -1; at = prev[at]) path.push_back(at);
					std::reverse(path.begin(), path.end());
					found.push_back({ path, member[group[0]], itemGroup, item.dist, 0.0 });
					best = (std::min)(best, item.dist);
					continue;
				}
				for (int nb : tree.nodes[item.idx].adj) {
					if (nb < 0 || nb >= (int)tree.nodes.size() || blocked[nb]) continue;
					if (tree.nodes[nb].kind == kAtlasStart && nb != tree.root) continue;
					double nd = item.dist + ((tree.nodes[nb].alloc || member[nb] == member[group[0]]) ? 0.0 : nodeWeight(nb));
					if (nd + 0.000001 < dist[nb]) {
						dist[nb] = nd;
						prev[nb] = item.idx;
						pq.push({ nd, nb });
					}
				}
			}
			return found;
		};

		int guard = 0;
		while (planGroups.size() > 1 && ++guard <= 500) {
			std::vector<int> member(tree.nodes.size(), -1);
			for (int gi = 0; gi < (int)planGroups.size(); gi++)
				for (int n : planGroups[gi])
					if (n >= 0 && n < (int)member.size()) member[n] = gi;

			std::vector<PlanPath> paths;
			for (const auto& group : planGroups) {
				std::vector<PlanPath> found = shortestPathsFromGroup(group, member);
				paths.insert(paths.end(), found.begin(), found.end());
			}
			if (paths.empty()) break;

			std::vector<int> occurrences(tree.nodes.size(), 0);
			for (const PlanPath& p : paths)
				for (int i = 1; i + 1 < (int)p.nodes.size(); i++)
					if (p.nodes[i] >= 0 && p.nodes[i] < (int)occurrences.size()) occurrences[p.nodes[i]]++;
			for (PlanPath& p : paths) {
				double sum = 0.0;
				int count = 0;
				for (int i = 1; i + 1 < (int)p.nodes.size(); i++) {
					sum += occurrences[p.nodes[i]];
					count++;
				}
				p.overlap = count ? (sum / count) : 0.0;
			}
			std::sort(paths.begin(), paths.end(), [](const PlanPath& a, const PlanPath& b) {
				if (std::fabs(a.cost - b.cost) > 0.000001) return a.cost < b.cost;
				if (std::fabs(a.overlap - b.overlap) > 0.000001) return a.overlap > b.overlap;
				return a.nodes.size() < b.nodes.size();
			});

			const PlanPath& best = paths.front();
			for (int n : best.nodes)
				if (n >= 0 && n < (int)tree.nodes.size() && tree.nodes[n].kind != kAtlasStart)
					tree.nodes[n].alloc = true;
			mergeGroups(planGroups, best.originGroup, best.targetGroup, best.nodes);
		}

		if (tree.root >= 0) {
			std::vector<int> desiredNodes;
			for (int i = 0; i < (int)desired.size(); i++)
				if (desired[i] && tree.nodes[i].alloc) desiredNodes.push_back(i);

			std::vector<int> candidates;
			for (int i = 0; i < (int)tree.nodes.size(); i++)
				if (tree.nodes[i].alloc && tree.nodes[i].kind != kAtlasStart && !desired[i])
					candidates.push_back(i);
			for (int removeIdx : candidates) {
				if (!tree.nodes[removeIdx].alloc) continue;
				tree.nodes[removeIdx].alloc = false;
				std::vector<char> reach(tree.nodes.size(), 0);
				std::deque<int> q;
				reach[tree.root] = 1;
				q.push_back(tree.root);
				while (!q.empty()) {
					int cur = q.front();
					q.pop_front();
					for (int nb : tree.nodes[cur].adj) {
						if (nb < 0 || nb >= (int)tree.nodes.size() || reach[nb] || !tree.nodes[nb].alloc) continue;
						reach[nb] = 1;
						q.push_back(nb);
					}
				}
				bool ok = true;
				for (int dIdx : desiredNodes) {
					if (!reach[dIdx]) {
						ok = false;
						break;
					}
				}
				if (!ok) tree.nodes[removeIdx].alloc = true;
			}
		}

		int reached = 0;
		for (int i = 0; i < (int)desired.size(); i++)
			if (desired[i] && tree.nodes[i].alloc) reached++;

		bool changed = false;
		for (int i = 0; i < (int)tree.nodes.size(); i++) {
			if ((tree.nodes[i].alloc ? 1 : 0) != before[i]) {
				changed = true;
				break;
			}
		}
		planningMsg = std::string(u8"規劃：") + std::to_string(reached) + "/" + std::to_string(desiredCount) +
		              u8" 個想要節點，排除 " + std::to_string(blockedCount) + u8" 個不要節點";
		if (desiredCount > reached)
			planningMsg += u8"（有節點無法避開不要點連到）";
		return changed;
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

	// Compute the compareBase -> active season diff (data-only; independent of the
	// user's allocation). Maps every changed id to a canvas node index so the
	// panel can focus it and the overlay can ring it.
	auto rebuildDiff = [&]() {
		diffReady = false;
		diffErr.clear();
		diff = AtlasTreeDiff();
		activeIdxById.clear();
		std::string base = verIndex.CompareBase(), targ = verIndex.Active();
		if (base.empty() || targ.empty() || base == targ) {
			diffErr = u8"需要兩個賽季的資料才能比較（目前只安裝了一個賽季）";
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
				planningPref.assign(tree.nodes.size(), 0);
				planningGroupPref.assign(tree.masteries.size(), 0);
				planningDirty = false;
				planningMsg.clear();
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
				std::string snote;
				buildFile.builds[idx].scarabs = scarabDb.Sanitize(e.scarabs, &snote);
				int kept = tree.ApplyAllocIds(e.alloc);
				planningPref.assign(tree.nodes.size(), 0);
				planningGroupPref.assign(tree.masteries.size(), 0);
				planningDirty = false;
				planningMsg.clear();
				saveActive();
				panelDirty = true;
				int dropped = (int)e.alloc.size() - kept;
				importMsg = u8"已匯入「" + buildFile.Active().name + u8"」：" + std::to_string(kept) + u8" 點";
				if (dropped > 0) importMsg += u8"（丟棄 " + std::to_string(dropped) + u8" 個未知節點）";
				if (!buildFile.builds[idx].scarabs.empty())
					importMsg += u8"、" + std::to_string(buildFile.builds[idx].scarabs.size()) + u8" 隻甲蟲";
				if (!buildFile.builds[idx].notes.empty()) importMsg += u8"、備註";
				importMsg += snote; // "，忽略 N 個未知甲蟲" etc., empty when nothing was dropped
				importFailed = false;
			};

			ImGui::AlignTextToFramePadding();
			ImGui::TextColored(PobUi::Accent(), u8"輿圖配置器");
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
			ImGui::SameLine();
			bool planningButtonWasActive = planningMode;
			if (planningButtonWasActive) PobUi::PushDangerButton();
			if (ImGui::Button(planningMode ? u8"結束規劃" : u8"規劃模式")) {
				planningMode = !planningMode;
				planningPaused = false;
				planningPref.assign(tree.nodes.size(), 0);
				planningGroupPref.assign(tree.masteries.size(), 0);
				planningDirty = false;
				planningMsg = planningMode
					? std::string(u8"規劃模式：左鍵想要/不要/清除，右鍵清除；點群組圖示可批量標記")
					: std::string();
				if (!planningMode) {
					tree.ApplyAllocIds(buildFile.Active().alloc);
					panelDirty = true;
				}
			}
			if (planningButtonWasActive) PobUi::PopButtonStyle();

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

			// version-compare toggle (needs two installed seasons)
			{
				bool canCompare = verIndex.Versions().size() >= 2 && !verIndex.CompareBase().empty();
				ImGui::SameLine(0, 18.0f * scale);
				if (!canCompare) ImGui::BeginDisabled();
				bool compareButtonWasActive = compareMode;
				if (compareButtonWasActive) PobUi::PushDangerButton();
				if (ImGui::Button(compareMode ? u8"結束比較" : u8"版本比較")) {
					compareMode = !compareMode;
					if (compareMode) refreshCompare();
					else view.ClearDiffOverlay();
				}
				if (compareButtonWasActive) PobUi::PopButtonStyle();
				if (!canCompare) ImGui::EndDisabled();
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(canCompare
						? u8"比較 %s -> %s：節點增刪與逐詞條數值變更（綠=新增, 黃=變動）"
						: u8"需要兩個賽季的資料才能比較",
						verIndex.CompareBase().c_str(), verIndex.Active().c_str());
			}
			if (planningMode) {
				ImGui::SameLine(0, 18.0f * scale);
				if (ImGui::Button(planningPaused ? u8"恢復計算" : u8"暫停計算")) {
					planningPaused = !planningPaused;
					if (!planningPaused) planningDirty = true;
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"重新計算")) planningDirty = true;
				ImGui::SameLine();
				if (ImGui::Button(u8"清除規劃")) {
					planningPref.assign(tree.nodes.size(), 0);
					planningGroupPref.assign(tree.masteries.size(), 0);
					planningDirty = true;
				}
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
			ImGui::TextDisabled(u8"滾輪縮放  ·  拖曳平移  ·  左鍵配點或移除");
			if (!importMsg.empty()) {
				ImGui::SameLine(0, 18.0f * scale);
				ImGui::TextColored(PobUi::StatusColor(importFailed ? PobUi::StatusTone::Error : PobUi::StatusTone::Success),
					"%s", importMsg.c_str());
			} else if (planningMode && !planningMsg.empty()) {
				ImGui::SameLine(0, 18.0f * scale);
				ImGui::TextColored(planningPaused ? PobUi::StatusColor(PobUi::StatusTone::Warning) : PobUi::Accent(),
					"%s%s", planningPaused ? u8"[暫停] " : "", planningMsg.c_str());
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

			if (ImGui::BeginPopupModal(u8"重置配點", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
				ImGui::TextUnformatted(u8"確定要清除所有已配置的節點嗎？");
				ImGui::Spacing();
				if (ImGui::Button(u8"清除全部")) {
					tree.Reset();
					planningPref.assign(tree.nodes.size(), 0);
					planningGroupPref.assign(tree.masteries.size(), 0);
					planningDirty = false;
					planningMsg.clear();
					saveActive();
					panelDirty = true;
					ImGui::CloseCurrentPopup();
				}
				ImGui::SameLine();
				if (ImGui::Button(u8"取消")) ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
			}

			ImGui::Separator();

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
			AtlasPlanOptions planOpts;
			planOpts.enabled = planningMode;
			planOpts.paused = planningPaused;
			planOpts.pref = &planningPref;
			planOpts.groupPref = &planningGroupPref;
			bool changed = view.Draw(tree, scale, showZh && zhLoaded ? &i18n : nullptr,
			                         planningMode ? &planOpts : nullptr); // auto-saves below; the file is tiny
			if (planOpts.prefChanged) {
				planningDirty = true;
				changed = false;
			}
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
			if (planningDirty && !planningPaused) {
				bool planChanged = rebuildPlanningAllocation();
				planningDirty = false;
				if (planChanged) {
					saveActive();
					panelDirty = true;
				}
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
			// Map device + notes come first and show even with nothing allocated:
			// both are useful before a single node is picked.
			renderScarabPanel();
			renderNotesPanel();
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

		if (glfwWindowShouldClose(win)) running = false;

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
