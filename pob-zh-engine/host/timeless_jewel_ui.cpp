#include "timeless_jewel_ui.h"

#include "launcher_config.h" // ResolveConfiguredFontPath
#include "http_client.h"
#include "passive_tree_data.h"
#include "passive_tree_view.h"
#include "timeless_jewel.h"
#include "ui_theme.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr float kFontSize = 18.0f;

std::vector<unsigned char> read_file(const std::wstring& path)
{
	std::vector<unsigned char> data;
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return data;
	LARGE_INTEGER size{};
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 30)) {
		data.resize((size_t)size.QuadPart);
		DWORD rd = 0;
		if (!ReadFile(h, data.data(), (DWORD)data.size(), &rd, nullptr) || rd != data.size())
			data.clear();
	}
	CloseHandle(h);
	return data;
}

// percent-encode for a URL query component (RFC 3986 unreserved kept as-is).
std::string url_encode(const std::string& s)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
		else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
	}
	return out;
}

std::wstring widen(const std::string& s)
{
	if (s.empty()) return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
	return w;
}

// Read the clipboard as UTF-8 text (empty on failure).
std::string read_clipboard_utf8(HWND owner)
{
	std::string out;
	if (!OpenClipboard(owner)) return out;
	HANDLE h = GetClipboardData(CF_UNICODETEXT);
	if (h) {
		const wchar_t* w = (const wchar_t*)GlobalLock(h);
		if (w) {
			int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
			if (n > 1) {
				out.resize(n - 1);
				WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], n, nullptr, nullptr);
			}
			GlobalUnlock(h);
		}
	}
	CloseClipboard();
	return out;
}

// Open the PoE official trade site pre-filled to search for a specific jewel
// seed. The seed is encoded as the conqueror's pseudo stat filter, exactly like
// pathofexile.com/trade does. platform: 0 pc, 1 xbox, 2 sony.
void open_trade_search(const std::string& tradeStatId, int seed,
                       const std::string& league, int platform)
{
	if (tradeStatId.empty() || league.empty()) return;
	// status "securable" == the trade site's "Instant Buyout" mode (per
	// awakened-poe-trade: merchantOnly -> 'securable'). No trade_filters block:
	// leaving sale_type unset keeps the "Sale Type" row at "Any". (An explicit
	// sale_type — especially a JSON null — got the ?q= state rejected.)
	char q[640];
	snprintf(q, sizeof(q),
		"{\"query\":{\"status\":{\"option\":\"securable\"},\"stats\":[{\"type\":\"and\",\"filters\":"
		"[{\"id\":\"%s\",\"value\":{\"min\":%d,\"max\":%d}}]}]"
		"},\"sort\":{\"price\":\"asc\"}}",
		tradeStatId.c_str(), seed, seed);
	const char* realm = platform == 1 ? "xbox/" : platform == 2 ? "sony/" : "";
	std::string url = "https://www.pathofexile.com/trade/search/" + std::string(realm) +
	                  url_encode(league) + "?q=" + url_encode(q);
	ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Open a trade search that matches ANY of several seeds at once (PoE "count"
// stat group with one filter per seed, count >= 1). Used to bundle a whole
// match-group into a single query. Caps at kMaxTradeSeeds filters per URL.
void open_trade_search_multi(const std::string& tradeStatId, const std::vector<int>& seeds,
                             const std::string& league, int platform)
{
	if (tradeStatId.empty() || league.empty() || seeds.empty()) return;
	const size_t kMaxTradeSeeds = 40;
	std::string filters;
	size_t n = 0;
	for (int s : seeds) {
		if (n >= kMaxTradeSeeds) break;
		char f[256];
		snprintf(f, sizeof(f), "%s{\"id\":\"%s\",\"value\":{\"min\":%d,\"max\":%d}}",
		         n ? "," : "", tradeStatId.c_str(), s, s);
		filters += f;
		n++;
	}
	std::string q = "{\"query\":{\"status\":{\"option\":\"securable\"},\"stats\":[{\"type\":\"count\","
	                "\"value\":{\"min\":1},\"filters\":[" + filters + "]}]"
	                "},\"sort\":{\"price\":\"asc\"}}";
	const char* realm = platform == 1 ? "xbox/" : platform == 2 ? "sony/" : "";
	std::string url = "https://www.pathofexile.com/trade/search/" + std::string(realm) +
	                  url_encode(league) + "?q=" + url_encode(q);
	ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Background one-shot fetch of the current trade leagues (names only).
struct LeagueFetch {
	std::atomic<bool> running{ false }, done{ false };
	std::vector<std::string> leagues;
	std::thread th;
	~LeagueFetch() { if (th.joinable()) th.join(); }
	void start() {
		if (running.load() || done.load()) return;
		running = true;
		th = std::thread([this]() {
			HttpsClient c(L"www.pathofexile.com");
			std::string body, err;
			std::vector<std::string> got;
			if (c.valid() && c.GetString(L"/api/trade/data/leagues", body, &err)) {
				// crude JSON scan for "id":"<league>"
				size_t p = 0;
				while ((p = body.find("\"id\":\"", p)) != std::string::npos) {
					p += 6;
					size_t e = body.find('"', p);
					if (e == std::string::npos) break;
					got.push_back(body.substr(p, e - p));
					p = e;
				}
			}
			leagues = std::move(got);
			running = false; done = true;
		});
	}
};

// Traditional-Chinese jewel names (display only); the type ids match the dataset.
const char* JewelZh(int t)
{
	switch (t) {
	case 1: return u8"輝煌的虛榮 (Glorious Vanity)";
	case 2: return u8"致命的驕傲 (Lethal Pride)";
	case 3: return u8"殘酷的紀律 (Brutal Restraint)";
	case 4: return u8"激進的信仰 (Militant Faith)";
	case 5: return u8"優雅的高傲 (Elegant Hubris)";
	case 6: return u8"英勇的悲劇 (Heroic Tragedy)";
	}
	return "?";
}

const char* ConquerorType(int jewelType)
{
	switch (jewelType) {
	case 1: return "vaal";
	case 2: return "karui";
	case 3: return "maraketh";
	case 4: return "templar";
	case 5: return "eternal";
	case 6: return "kalguur";
	}
	return "";
}

// Tint a stat line by its dominant damage/defence keyword (Vilsol-style, but
// whole-line for simplicity). Matches both English and Traditional-Chinese words.
ImVec4 stat_color(const std::string& s)
{
	struct KW { const char* a; const char* b; ImVec4 c; };
	static const KW kws[] = {
		{ "Fire",      u8"火焰",   ImVec4(0.95f, 0.45f, 0.35f, 1.0f) },
		{ "Cold",      u8"冰冷",   ImVec4(0.45f, 0.75f, 0.95f, 1.0f) },
		{ "Lightning", u8"閃電",   ImVec4(0.95f, 0.85f, 0.40f, 1.0f) },
		{ "Chaos",     u8"混沌",   ImVec4(0.80f, 0.45f, 0.85f, 1.0f) },
		{ "Physical",  u8"物理",   ImVec4(0.86f, 0.74f, 0.58f, 1.0f) },
		{ "Life",      u8"生命",   ImVec4(0.90f, 0.45f, 0.45f, 1.0f) },
		{ "Mana",      u8"魔力",   ImVec4(0.50f, 0.65f, 0.95f, 1.0f) },
		{ "Energy Shield", u8"能量護盾", ImVec4(0.55f, 0.80f, 0.90f, 1.0f) },
		{ "Attack",    u8"攻擊",   ImVec4(0.90f, 0.72f, 0.50f, 1.0f) },
		{ "Spell",     u8"法術",   ImVec4(0.70f, 0.70f, 0.95f, 1.0f) },
	};
	for (const KW& k : kws)
		if (s.find(k.a) != std::string::npos || s.find(k.b) != std::string::npos) return k.c;
	return ImVec4(0.62f, 0.68f, 0.90f, 1.0f);
}

bool contains_ci(const std::string& hay, const std::string& needle)
{
	if (needle.empty()) return true;
	auto lower = [](std::string s) { for (char& c : s) if ((unsigned char)c < 0x80) c = (char)tolower((unsigned char)c); return s; };
	return lower(hay).find(lower(needle)) != std::string::npos;
}

// A stat the user wants to search for.
struct WantRow {
	std::string en;   // match key
	std::string zh;   // display
	float minValue = 0.0f;
	float weight = 1.0f;
};

// Background search worker (TJSearch is ~1s; never block the UI thread).
struct SearchJob {
	std::atomic<bool> running{ false };
	std::atomic<bool> done{ false };
	volatile bool cancel = false; // one-way flag; TJSearch polls it as const volatile bool*
	std::vector<TJSeedHit> results;
	std::thread th;

	~SearchJob() { cancel = true; if (th.joinable()) th.join(); }

	void start(const TJDataset* ds, const std::string* blob, TJSearchQuery q) {
		if (th.joinable()) { cancel = true; th.join(); }
		cancel = false; done = false; running = true;
		results.clear();
		th = std::thread([this, ds, blob, q]() {
			auto r = TJSearch(*ds, *blob, q, 500, &cancel);
			results = std::move(r);
			running = false; done = true;
		});
	}
};

// Write a bottom-up 24-bit BMP (no external encoder needed).
static bool write_bmp(const std::wstring& path, const unsigned char* rgba, int w, int h)
{
	int stride = (w * 3 + 3) & ~3;
	int dataSize = stride * h;
	unsigned char hdr[54] = { 'B', 'M' };
	auto put32 = [&](int off, unsigned v) {
		hdr[off] = v & 0xFF; hdr[off + 1] = (v >> 8) & 0xFF;
		hdr[off + 2] = (v >> 16) & 0xFF; hdr[off + 3] = (v >> 24) & 0xFF;
	};
	put32(2, 54 + dataSize); put32(10, 54); put32(14, 40);
	put32(18, w); put32(22, h);
	hdr[26] = 1; hdr[28] = 24;
	put32(34, dataSize);
	HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD wr = 0;
	WriteFile(f, hdr, 54, &wr, nullptr);
	std::vector<unsigned char> row(stride, 0);
	for (int y = 0; y < h; y++) {           // glReadPixels rows are already bottom-up
		const unsigned char* src = rgba + (size_t)y * w * 4;
		for (int x = 0; x < w; x++) {
			row[x * 3 + 0] = src[x * 4 + 2];
			row[x * 3 + 1] = src[x * 4 + 1];
			row[x * 3 + 2] = src[x * 4 + 0];
		}
		WriteFile(f, row.data(), stride, &wr, nullptr);
	}
	CloseHandle(f);
	return true;
}

} // namespace

// Headless one-frame render of the passive-tree canvas to pt_render.bmp next to
// the exe ("--pt-render [zoom cx cy]"). Debug aid: lets the tree view be
// inspected without a visible window / manual screenshotting.
int RunPassiveTreeRender(const std::wstring& exeDir, float zoom, float cx, float cy)
{
	PassiveTreeData ptData;
	std::string err;
	if (!ptData.Load(exeDir, &err)) { printf("load: %s\n", err.c_str()); return 1; }

	// diagnostic: parsed extents must match the JSON (bounds + node min/max)
	{
		float nx0 = 1e9f, ny0 = 1e9f, nx1 = -1e9f, ny1 = -1e9f;
		for (const PtNode& n : ptData.nodes) {
			nx0 = (std::min)(nx0, n.x); nx1 = (std::max)(nx1, n.x);
			ny0 = (std::min)(ny0, n.y); ny1 = (std::max)(ny1, n.y);
		}
		printf("bounds json: x %.0f..%.0f y %.0f..%.0f\n", ptData.minX, ptData.maxX, ptData.minY, ptData.maxY);
		printf("nodes real:  x %.0f..%.0f y %.0f..%.0f\n", nx0, nx1, ny0, ny1);
		for (int k = 0; k < 3 && k < (int)ptData.nodes.size(); k++)
			printf("node[%d] id=%d x=%.1f y=%.1f kind=%d\n", k,
			       ptData.nodes[k].id, ptData.nodes[k].x, ptData.nodes[k].y, ptData.nodes[k].kind);
		int arcs = 0;
		for (const PtEdge& e : ptData.edges) if (e.hasArc) arcs++;
		printf("edges=%d arcs=%d\n", (int)ptData.edges.size(), arcs);
	}

	if (!glfwInit()) { printf("glfwInit failed\n"); return 1; }
	glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
	glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
	glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
	const int W = 1000, H = 900;
	GLFWwindow* win = glfwCreateWindow(W, H, "pt-render", nullptr, nullptr);
	if (!win) { glfwTerminate(); printf("window failed\n"); return 1; }
	glfwMakeContextCurrent(win);

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::GetIO().IniFilename = nullptr;
	ImGui_ImplGlfw_InitForOpenGL(win, false);
	ImGui_ImplOpenGL3_Init("#version 100");

	PassiveTreeView view;
	if (!view.LoadTextures(exeDir, ptData, &err)) { printf("tex: %s\n", err.c_str()); }
	if (zoom > 0.0f) view.SetCamera(zoom, cx, cy);

	std::vector<unsigned char> ptHi(ptData.nodes.size(), kPtHiNone);
	std::vector<char> ptSel(ptData.nodes.size(), 0);
	// exercise both paths: some nodes matched (red ring), some selected (lit+green)
	PassiveTreeInput tin;
	if (!ptData.sockets.empty()) {
		tin.selectedSocket = ptData.sockets[ptData.sockets.size() / 2];
		std::vector<int> inr = ptData.NodesInRadius(tin.selectedSocket, 1800.0f);
		for (size_t k = 0; k < inr.size(); k++) {
			if (k % 3 == 0) ptHi[inr[k]] = kPtHiAffected;  // matched -> red ring
			if (k % 4 == 0) ptSel[inr[k]] = 1;             // selected -> lit + green
		}
		if (!inr.empty()) tin.emphasize = inr[inr.size() / 2];
	}
	tin.hi = &ptHi;
	tin.selected = &ptSel;

	// two frames: first sizes the view (auto-fit), second is the real render
	for (int frame = 0; frame < 2; frame++) {
		glfwPollEvents();
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
		ImGui::Begin("##rt", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
		view.Draw(ptData, 1.0f, tin);
		ImGui::End();
		ImGui::Render();
		int fbW = 0, fbH = 0;
		glfwGetFramebufferSize(win, &fbW, &fbH);
		glViewport(0, 0, fbW, fbH);
		glClearColor(0.04f, 0.05f, 0.07f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		if (frame == 1) {
			std::vector<unsigned char> px((size_t)fbW * fbH * 4);
			glReadPixels(0, 0, fbW, fbH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
			bool ok = write_bmp(exeDir + L"pt_render.bmp", px.data(), fbW, fbH);
			printf("pt_render.bmp %s (%dx%d)\n", ok ? "written" : "WRITE FAILED", fbW, fbH);
		}
		glfwSwapBuffers(win);
	}

	view.DestroyTextures();
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();
	return 0;
}

void ShowTimelessJewel(const std::wstring& exeDir, const std::wstring& locale)
{
	(void)locale;
	// --- load data first; a clear message beats an empty window ---
	auto ds = std::make_shared<TJDataset>();
	std::string derr;
	if (!ds->Load(exeDir + L"Data\\timeless_jewels.json", &derr)) {
		MessageBoxW(nullptr, L"無法載入 timeless_jewels.json（資料檔遺失）。", L"PobTools", MB_ICONERROR | MB_OK);
		return;
	}

	if (!glfwInit()) {
		MessageBoxW(nullptr, L"無法初始化 GLFW，軍團珠寶計算器無法顯示。", L"PobTools", MB_ICONERROR | MB_OK);
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
	const int winW = (int)(1640 * scale);
	const int winH = (int)(940 * scale);
	GLFWwindow* win = glfwCreateWindow(winW, winH,
		"PobTools \xe2\x80\x94 \xe8\xbb\x8d\xe5\x9c\x98\xe7\x8f\xa0\xe5\xaf\xb6", nullptr, nullptr);
	if (!win) { glfwTerminate(); return; }
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
	PobUi::ApplyTheme(scale, PobUi::Density::Compact);

	std::vector<unsigned char> ttf = read_file(ResolveConfiguredFontPath(exeDir));
	ImFont* font = nullptr;
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
		cfg.OversampleH = 1; cfg.OversampleV = 1; cfg.PixelSnapH = true;
		io.Fonts->TexDesiredWidth = 4096;
		font = io.Fonts->AddFontFromMemoryTTF(ttf.data(), (int)ttf.size(), kFontSize * scale, &cfg, ranges.Data);
		io.Fonts->Build();
	}
	if (!font) font = ImGui::GetIO().Fonts->AddFontDefault();

	ImGui_ImplGlfw_InitForOpenGL(win, true);
	ImGui_ImplOpenGL3_Init("#version 100");

	// --- state ---
	int jewelType = 3;            // Brutal Restraint
	int conquerorSel = 0;         // index into per-jewel keystone list
	int mode = 0;                 // 0 = search by stats, 1 = enter seed
	int scope = 1;                // 1 = notables, 0 = all
	float minTotalWeight = 0.0f;
	std::string statFilter;
	std::vector<WantRow> wants;
	std::string seedText = "500";
	std::string status;
	int detailSeed = -1;          // a result seed to expand

	// --- passive tree view (right pane) ---
	PassiveTreeData ptData;
	PassiveTreeView ptView;
	std::string ptErr;
	bool ptDataOk = ptData.Load(exeDir, &ptErr);
	bool ptTexOk = ptDataOk && ptView.LoadTextures(exeDir, ptData, &ptErr);
	int selSocket = -1;                          // node index of the socketed jewel
	std::vector<unsigned char> ptHi;             // per-node highlight class
	std::vector<char> ptSelected;                // per-node: user-picked focus set (1 = picked)
	int selVersion = 0;                          // bumps on any selection change (recompute key)
	std::map<int, TJTransform> ptTrans;          // node index -> transform (affected only)
	// stat-centric list (Vilsol style): a rolled stat -> the nodes that gained it
	struct StatGroup { std::string name; std::vector<int> notables, smalls; double maxVal = 0; };
	std::vector<StatGroup> statGroups;
	std::vector<unsigned char> dispHi;           // ptHi, optionally filtered to one stat group
	// signature of the last highlight computation, to recompute only on change
	long long hiSig = -1;
	int panToNode = -1;                          // list pick: glide the tree to this node
	int emphNode = -1;                           // list pick: keep this node ring-pulsed
	// affected-list display controls
	int listView = 0;                            // 0 = stat-centric (Vilsol), 1 = node-centric
	int statSort = 0;                            // 0 count, 1 alpha, 2 rarity, 3 value
	bool splitList = true;                       // split notables / smalls
	int hlStatGroup = -1;                        // stat row -> highlight only its nodes

	// --- trade export state ---
	std::string tradeLeague = "Standard";
	int tradePlatform = 0;                       // 0 pc, 1 xbox, 2 sony
	LeagueFetch leagues;
	bool groupResults = true;                    // group seeds by # of stats matched
	bool searchInputsOpen = true;                // collapse the stat picker after a search

	// --- affected-node list display option ---
	bool colorStats = true;

	// stat picker templates are jewel-specific; recompute when the jewel changes
	std::vector<TJStatTemplate> templates = TJStatTemplates(*ds, jewelType);
	int templatesFor = jewelType;
	auto blob = std::make_shared<std::string>();
	std::string binErr;
	bool binOk = TJLoadBin(exeDir, *ds, jewelType, *blob, &binErr);
	int loadedBinType = jewelType;

	SearchJob job;

	auto ensureBin = [&](int type) {
		if (binOk && loadedBinType == type) return true;
		blob->clear();
		binOk = TJLoadBin(exeDir, *ds, type, *blob, &binErr);
		loadedBinType = type;
		return binOk;
	};

	// conqueror table (name + keystone id + trade pseudo-stat) from the dataset
	auto conqListFor = [&](int type) -> const std::vector<TJConqueror>* {
		auto it = ds->conquerors.find(type);
		return it != ds->conquerors.end() ? &it->second : nullptr;
	};

	while (!glfwWindowShouldClose(win)) {
		glfwPollEvents();
		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();
		ImGui::PushFont(font);

		ImGuiIO& io = ImGui::GetIO();
		ImGui::SetNextWindowPos(ImVec2(0, 0));
		ImGui::SetNextWindowSize(io.DisplaySize);
		ImGui::Begin("##tj", nullptr,
			ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

		// left column: the search form (fixed width); right column: the tree.
		const float leftW = 430.0f * scale;
		ImGui::BeginChild("##left", ImVec2(leftW, 0), false);

		ImGui::TextUnformatted(u8"軍團珠寶計算器");
		ImGui::SameLine();
		ImGui::TextDisabled(u8"Timeless Jewel");
		ImGui::Separator();

		// --- paste a jewel from the clipboard (auto-fills jewel/conqueror/seed) ---
		if (ImGui::Button(u8"貼上珠寶 (從遊戲複製物品)", ImVec2(-1, 0))) {
			std::string txt = read_clipboard_utf8(nullptr);
			int foundJewel = 0, foundConqIdx = -1;
			size_t namePos = std::string::npos;
			// a conqueror name is unique across all jewels -> identifies both
			for (auto& kv : ds->conquerors) {
				for (int i = 0; i < (int)kv.second.size(); i++) {
					const std::string& nm = kv.second[i].name;
					size_t p = nm.empty() ? std::string::npos : txt.find(nm);
					if (p != std::string::npos) { foundJewel = kv.first; foundConqIdx = i; namePos = p; }
				}
			}
			// the seed is the number on the flavour line (the line naming the
			// conqueror); fall back to the largest integer anywhere.
			int foundSeed = -1;
			auto lineOf = [&](size_t pos) {
				size_t a = txt.rfind('\n', pos); a = (a == std::string::npos) ? 0 : a + 1;
				size_t b = txt.find('\n', pos); if (b == std::string::npos) b = txt.size();
				return txt.substr(a, b - a);
			};
			std::string scan = (namePos != std::string::npos) ? lineOf(namePos) : txt;
			for (size_t p = 0; p < scan.size(); p++)
				if (isdigit((unsigned char)scan[p])) {
					int v = atoi(scan.c_str() + p);
					if (v > foundSeed) foundSeed = v;
					while (p < scan.size() && isdigit((unsigned char)scan[p])) p++;
				}
			if (foundJewel) {
				jewelType = foundJewel; conquerorSel = foundConqIdx; ensureBin(jewelType);
			}
			if (foundSeed >= 0) {
				seedText = std::to_string(foundSeed); detailSeed = foundSeed; mode = 1;
			}
			status = foundJewel ? (u8"已匯入：" + std::string(JewelZh(foundJewel)) +
			                       (foundSeed >= 0 ? u8"  種子 " + std::to_string(foundSeed) : ""))
			                    : u8"剪貼簿中未找到珠寶資訊（請在遊戲中對珠寶 Ctrl+C）";
			hiSig = -1;
		}

		// --- jewel type ---
		ImGui::TextUnformatted(u8"珠寶");
		ImGui::SetNextItemWidth(-1);
		if (ImGui::BeginCombo("##jewel", JewelZh(jewelType))) {
			for (int t = 1; t <= 6; t++)
				if (ImGui::Selectable(JewelZh(t), t == jewelType)) {
					jewelType = t;
					conquerorSel = 0;
					ensureBin(t);
				}
			ImGui::EndCombo();
		}

		// jewel changed (combo or paste): the affix pool is jewel-specific
		if (jewelType != templatesFor) {
			templates = TJStatTemplates(*ds, jewelType);
			templatesFor = jewelType;
			wants.clear();          // previously-picked stats may not exist on this jewel
		}

		// --- conqueror (affects keystones + trade export) ---
		const std::vector<TJConqueror>* conqs = conqListFor(jewelType);
		int conqN = conqs ? (int)conqs->size() : 0;
		if (conquerorSel >= conqN) conquerorSel = 0;
		auto conqLabel = [&](int i) -> std::string {
			const TJConqueror& c = (*conqs)[i];
			std::string s = c.nameZh.empty() ? c.name : (c.nameZh + " (" + c.name + ")");
			if (c.id.find("_v2") != std::string::npos) s += u8"  (舊版)";
			return s;
		};
		ImGui::TextUnformatted(u8"征服者 (影響關鍵天賦)");
		ImGui::SetNextItemWidth(-1);
		std::string curConq = conqN ? conqLabel(conquerorSel) : u8"(無)";
		if (ImGui::BeginCombo("##conq", curConq.c_str())) {
			for (int i = 0; i < conqN; i++)
				if (ImGui::Selectable(conqLabel(i).c_str(), i == conquerorSel))
					conquerorSel = i;
			ImGui::EndCombo();
		}
		std::string conquerorId = conqN ? (*conqs)[conquerorSel].id : std::string("1");
		std::string tradeStatId = conqN ? (*conqs)[conquerorSel].trade : std::string();

		// --- trade export settings (league + platform) ---
		if (ImGui::CollapsingHeader(u8"交易站匯出設定")) {
			ImGui::TextUnformatted(u8"聯盟");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(180 * scale);
			if (!leagues.leagues.empty()) {
				if (ImGui::BeginCombo("##league", tradeLeague.c_str())) {
					for (const auto& lg : leagues.leagues)
						if (ImGui::Selectable(lg.c_str(), lg == tradeLeague)) tradeLeague = lg;
					ImGui::EndCombo();
				}
			} else {
				ImGui::InputText("##league", &tradeLeague);
			}
			ImGui::SameLine();
			if (leagues.running.load()) ImGui::TextDisabled(u8"取得中…");
			else if (ImGui::SmallButton(u8"取得聯盟")) leagues.start();
			ImGui::TextUnformatted(u8"平台");
			ImGui::SameLine();
			ImGui::RadioButton("PC", &tradePlatform, 0); ImGui::SameLine();
			ImGui::RadioButton("Xbox", &tradePlatform, 1); ImGui::SameLine();
			ImGui::RadioButton("PS", &tradePlatform, 2);
		}

		ImGui::Separator();
		if (ImGui::BeginTabBar("##mode")) {
			if (ImGui::BeginTabItem(u8"選擇統計搜尋")) { mode = 0; ImGui::EndTabItem(); }
			if (ImGui::BeginTabItem(u8"輸入種子")) { mode = 1; ImGui::EndTabItem(); }
			ImGui::EndTabBar();
		}

		if (mode == 0) {
			// stat picker collapses once a search runs, freeing space for results
			ImGui::SetNextItemOpen(searchInputsOpen, ImGuiCond_Always);
			searchInputsOpen = ImGui::CollapsingHeader(u8"搜尋條件（詞綴 / 權重 / 範圍）");
			if (searchInputsOpen) {
			// --- add stat ---
			ImGui::TextUnformatted(u8"新增統計 (可輸入關鍵字篩選)");
			ImGui::SetNextItemWidth(-1);
			ImGui::InputTextWithHint("##statfilter", u8"篩選詞綴…", &statFilter);
			ImGui::BeginChild("##statlist", ImVec2(0, 150 * scale), true);
			int shown = 0;
			for (const auto& t : templates) {
				const std::string& disp = t.zh.empty() ? t.en : t.zh;
				if (!contains_ci(disp, statFilter) && !contains_ci(t.en, statFilter)) continue;
				if (++shown > 300) break;
				if (ImGui::Selectable(disp.c_str())) {
					bool exists = false;
					for (auto& w : wants) if (w.en == t.en) exists = true;
					if (!exists) wants.push_back({ t.en, disp, 0.0f, 1.0f });
				}
			}
			ImGui::EndChild();

			// --- selected stats ---
			if (!wants.empty()) {
				ImGui::TextUnformatted(u8"已選統計");
				for (int i = 0; i < (int)wants.size(); i++) {
					ImGui::PushID(i);
					if (ImGui::SmallButton(u8"移除")) { wants.erase(wants.begin() + i); ImGui::PopID(); i--; continue; }
					ImGui::SameLine();
					ImGui::TextUnformatted(wants[i].zh.c_str());
					ImGui::SetNextItemWidth(120 * scale);
					ImGui::InputFloat(u8"最小值", &wants[i].minValue, 0, 0, "%.0f");
					ImGui::SameLine();
					ImGui::SetNextItemWidth(120 * scale);
					ImGui::InputFloat(u8"權重", &wants[i].weight, 0, 0, "%.1f");
					ImGui::PopID();
				}
			}

			ImGui::SetNextItemWidth(160 * scale);
			ImGui::InputFloat(u8"最小總權重", &minTotalWeight, 0, 0, "%.1f");
			ImGui::RadioButton(u8"只中型天賦 (Notables)", &scope, 1);
			ImGui::SameLine();
			ImGui::RadioButton(u8"全部節點", &scope, 0);
			} // searchInputsOpen

			// --- search ---
			bool busy = job.running.load();
			if (selSocket < 0)
				ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.40f, 1.0f),
				                   u8"請先在中間樹上點擊珠寶插槽（搜尋只計算該插槽半徑內的節點）");
			ImGui::BeginDisabled(busy || wants.empty() || selSocket < 0);
			if (ImGui::Button(u8"搜尋此插槽", ImVec2(-1, 34 * scale))) {
				if (ensureBin(jewelType)) {
					TJSearchQuery q;
					q.jewelType = jewelType;
					q.scope = scope;
					q.minTotalWeight = minTotalWeight;
					for (auto& w : wants) q.wants.push_back({ w.en, w.minValue, w.weight });
					// bind the search to the socket's in-radius nodes — the picked
					// subset if any node is picked, otherwise all of them.
					std::vector<int> inRad = ptData.NodesInRadius(selSocket, 1800.0f);
					bool anySel = false;
					for (int idx : inRad)
						if (idx < (int)ptSelected.size() && ptSelected[idx]) { anySel = true; break; }
					for (int idx : inRad) {
						if (ptData.nodes[idx].kind == kPtSocket) continue;
						if (anySel && !(idx < (int)ptSelected.size() && ptSelected[idx])) continue;
						q.nodeIds.push_back(ptData.nodes[idx].id);
					}
					detailSeed = -1;
					status = u8"搜尋中…";
					searchInputsOpen = false; // collapse inputs, show results
					job.start(ds.get(), blob.get(), q);
				} else {
					status = binErr;
				}
			}
			ImGui::EndDisabled();
			if (busy) { ImGui::SameLine(); ImGui::TextDisabled(u8"搜尋中…"); }

			if (job.done.load() && !job.running.load()) {
				status = u8"找到 " + std::to_string(job.results.size()) + u8" 個種子";
			}
			if (!status.empty()) ImGui::TextDisabled("%s", status.c_str());

			// --- results ---
			if (!job.results.empty() && !job.running.load()) {
				ImGui::Separator();
				ImGui::Checkbox(u8"依命中節點數分組（可整組一鍵交易）", &groupResults);
				bool tradeOff = tradeStatId.empty() || tradeLeague.empty();

				if (groupResults) {
					// the socket's picked in-radius nodes (or all) + wanted set
					std::vector<int> inRad;
					if (selSocket >= 0) {
						std::vector<int> rad = ptData.NodesInRadius(selSocket, 1800.0f);
						bool anySel = false;
						for (int idx : rad)
							if (idx < (int)ptSelected.size() && ptSelected[idx]) { anySel = true; break; }
						for (int idx : rad) {
							if (ptData.nodes[idx].kind == kPtSocket) continue;
							if (anySel && !(idx < (int)ptSelected.size() && ptSelected[idx])) continue;
							inRad.push_back(idx);
						}
					}
					std::set<std::string> wantSet;
					for (const auto& w : wants) wantSet.insert(w.en);

					// group seeds by how many nodes matched (desc), like Vilsol
					std::map<int, std::vector<const TJSeedHit*>, std::greater<int>> groups;
					for (const auto& h : job.results) groups[h.matches].push_back(&h);
					ImGui::BeginChild("##resg", ImVec2(0, 300 * scale), false);
					bool firstGroup = true;
					for (auto& kv : groups) {
						char hdr[128];
						snprintf(hdr, sizeof(hdr), u8"命中 %d 個節點 · %d 個種子##grp%d",
						         kv.first, (int)kv.second.size(), kv.first);
						ImGuiTreeNodeFlags gf = firstGroup ? ImGuiTreeNodeFlags_DefaultOpen : 0;
						firstGroup = false;
						if (ImGui::CollapsingHeader(hdr, gf)) {
							ImGui::PushID(kv.first);
							ImGui::BeginDisabled(tradeOff);
							if (ImGui::SmallButton(u8"交易查詢整組")) {
								std::vector<int> seeds;
								for (auto* h : kv.second) seeds.push_back(h->seed);
								open_trade_search_multi(tradeStatId, seeds, tradeLeague, tradePlatform);
							}
							ImGui::EndDisabled();
							if (kv.second.size() > 40) { ImGui::SameLine(); ImGui::TextDisabled(u8"(交易取前 40)"); }
							ImGui::PopID();

							int shown = 0;
							for (auto* h : kv.second) {
								if (++shown > 50) {
									ImGui::TextDisabled(u8"… 還有 %d 個（請縮小條件）", (int)kv.second.size() - 50);
									break;
								}
								ImGui::PushID(h->seed);
								ImGui::TextColored(ImVec4(0.98f, 0.62f, 0.30f, 1.0f),
								                   u8"種子 %d (權重 %.0f)", h->seed, h->weight);
								ImGui::SameLine();
								if (ImGui::SmallButton(u8"查看")) detailSeed = h->seed;
								ImGui::SameLine();
								ImGui::BeginDisabled(tradeOff);
								if (ImGui::SmallButton(u8"交易"))
									open_trade_search(tradeStatId, h->seed, tradeLeague, tradePlatform);
								ImGui::EndDisabled();
								// this seed's matched affixes only (no node name), on the
								// picked nodes — keeps the list compact per the request
								for (int idx : inRad) {
									const PtNode& n = ptData.nodes[idx];
									const char* nt = n.kind == kPtKeystone ? "Keystone"
									               : n.kind == kPtNotable ? "Notable" : "Normal";
									TJTransform t = TJApply(*ds, *blob, jewelType, h->seed, n.id, nt,
									                        n.stats, ConquerorType(jewelType), conquerorId, n.name);
									if (!t.ok) continue;
									for (size_t i = 0; i < t.lines.size(); i++) {
										if (!wantSet.count(TJNormalizeStat(t.lines[i]))) continue;
										const std::string& zh = (i < t.linesZh.size() && !t.linesZh[i].empty())
										                        ? t.linesZh[i] : t.lines[i];
										if (colorStats) ImGui::PushStyleColor(ImGuiCol_Text, stat_color(zh));
										ImGui::BulletText("%s", zh.c_str());
										if (colorStats) ImGui::PopStyleColor();
									}
								}
								ImGui::Separator();
								ImGui::PopID();
							}
						}
					}
					ImGui::EndChild();
				} else if (ImGui::BeginTable("##res", 5,
					ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
					ImVec2(0, 240 * scale))) {
					ImGui::TableSetupColumn(u8"種子");
					ImGui::TableSetupColumn(u8"權重");
					ImGui::TableSetupColumn(u8"詞綴");
					ImGui::TableSetupColumn(u8"");
					ImGui::TableSetupColumn(u8"");
					ImGui::TableHeadersRow();
					for (const auto& h : job.results) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn(); ImGui::Text("%d", h.seed);
						ImGui::TableNextColumn(); ImGui::Text("%.1f", h.weight);
						ImGui::TableNextColumn(); ImGui::Text("%d", h.distinctWants);
						ImGui::TableNextColumn();
						ImGui::PushID(h.seed);
						if (ImGui::SmallButton(u8"查看")) detailSeed = h.seed;
						ImGui::TableNextColumn();
						ImGui::BeginDisabled(tradeOff);
						if (ImGui::SmallButton(u8"交易"))
							open_trade_search(tradeStatId, h.seed, tradeLeague, tradePlatform);
						ImGui::EndDisabled();
						ImGui::PopID();
					}
					ImGui::EndTable();
				}
			}
		} else {
			// --- enter seed mode ---
			ImGui::SetNextItemWidth(200 * scale);
			ImGui::InputText(u8"種子", &seedText);
			ImGui::SameLine();
			if (ImGui::Button(u8"查詢")) {
				detailSeed = atoi(seedText.c_str());
				if (!ensureBin(jewelType)) status = binErr;
			}
		}

		// affected node/stat list — defined here, rendered into the right sidebar
		auto drawAffectedList = [&]() {
		if (selSocket < 0) {
			ImGui::TextDisabled(u8"在中間樹上點擊珠寶插槽，選擇珠寶放置位置");
		} else if (detailSeed < 0) {
			ImGui::TextDisabled(u8"選一個種子（搜尋結果按「查看」或於「輸入種子」查詢）");
		} else {
			ImGui::Text(u8"種子 %d 範圍內的變更", detailSeed);
			ImGui::SameLine();
			ImGui::BeginDisabled(tradeStatId.empty() || tradeLeague.empty());
			if (ImGui::SmallButton(u8"交易搜尋"))
				open_trade_search(tradeStatId, detailSeed, tradeLeague, tradePlatform);
			ImGui::EndDisabled();

			// view toggle: Vilsol-style stat list vs. node-centric list
			ImGui::RadioButton(u8"詞綴檢視", &listView, 0); ImGui::SameLine();
			ImGui::RadioButton(u8"節點檢視", &listView, 1); ImGui::SameLine();
			ImGui::Checkbox(u8"上色", &colorStats);

			if (listView == 0) {
				// ---- Vilsol-style: "(N) stat", click to highlight those N nodes ----
				ImGui::SetNextItemWidth(120 * scale);
				const char* sorts[] = { u8"數量", u8"字母", u8"稀有度", u8"數值" };
				ImGui::Combo("##statsort", &statSort, sorts, 4);
				ImGui::SameLine(); ImGui::Checkbox(u8"中型/一般分欄", &splitList);
				if (hlStatGroup >= 0) {
					ImGui::SameLine();
					if (ImGui::SmallButton(u8"顯示全部")) hlStatGroup = -1;
				}
				std::vector<int> order(statGroups.size());
				for (int i = 0; i < (int)statGroups.size(); i++) order[i] = i;
				auto cnt = [&](int g) { return (int)(statGroups[g].notables.size() + statGroups[g].smalls.size()); };
				std::sort(order.begin(), order.end(), [&](int a, int b) {
					if (statSort == 1) return statGroups[a].name < statGroups[b].name;
					if (statSort == 2) {
						bool na = !statGroups[a].notables.empty(), nb = !statGroups[b].notables.empty();
						if (na != nb) return na > nb;
						return cnt(a) > cnt(b);
					}
					if (statSort == 3) return statGroups[a].maxVal > statGroups[b].maxVal;
					return cnt(a) > cnt(b);
				});

				ImGui::BeginChild("##statlist2", ImVec2(0, 0), true);
				if (statGroups.empty())
					ImGui::TextDisabled(u8"此範圍內沒有受影響的詞綴（或資料仍在計算）。");
				auto drawGroup = [&](int g, int section) { // section: -1 all, 0 notables, 1 smalls
					int c = section == 0 ? (int)statGroups[g].notables.size()
					      : section == 1 ? (int)statGroups[g].smalls.size() : cnt(g);
					if (c == 0) return;
					char lbl[256];
					snprintf(lbl, sizeof(lbl), "(%d) %s##g%d%d", c, statGroups[g].name.c_str(), g, section + 1);
					if (colorStats) ImGui::PushStyleColor(ImGuiCol_Text, stat_color(statGroups[g].name));
					bool sel = ImGui::Selectable(lbl, hlStatGroup == g);
					if (colorStats) ImGui::PopStyleColor();
					if (sel) hlStatGroup = (hlStatGroup == g) ? -1 : g;
				};
				if (splitList) {
					ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.0f), u8"中型天賦 / 關鍵天賦");
					ImGui::Separator();
					for (int g : order) drawGroup(g, 0);
					ImGui::Spacing();
					ImGui::TextColored(ImVec4(0.80f, 0.80f, 0.80f, 1.0f), u8"一般節點");
					ImGui::Separator();
					for (int g : order) drawGroup(g, 1);
				} else {
					for (int g : order) drawGroup(g, -1);
				}
				ImGui::EndChild();
			} else {
				// ---- node-centric: "{node}: {affix}", click to jump on the tree ----
				auto matchesWants = [&](const TJTransform& t) {
					if (wants.empty()) return false;
					for (const auto& ln : t.lines) {
						std::string k = TJNormalizeStat(ln);
						for (const auto& w : wants) if (w.en == k) return true;
					}
					return false;
				};
				bool anySel = false;
				for (size_t i = 0; i < ptSelected.size(); i++) if (ptSelected[i]) { anySel = true; break; }
				struct Row { int idx; bool big; bool prio; };
				std::vector<Row> rows;
				for (const auto& kv : ptTrans) {
					if (anySel && !(kv.first < (int)ptSelected.size() && ptSelected[kv.first])) continue;
					const PtNode& n = ptData.nodes[kv.first];
					bool big = kv.second.replaced || n.kind == kPtNotable || n.kind == kPtKeystone;
					rows.push_back({ kv.first, big, matchesWants(kv.second) });
				}
				std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
					if (a.big != b.big) return a.big > b.big;        // 大點最上方
					if (a.prio != b.prio) return a.prio > b.prio;    // 搜尋命中優先
					return a.idx < b.idx;
				});

				ImGui::BeginChild("##nodelist", ImVec2(0, 0), true);
				if (rows.empty())
					ImGui::TextDisabled(u8"此範圍內沒有受影響的節點（或資料仍在計算）。");
				for (const Row& r : rows) {
					const PtNode& n = ptData.nodes[r.idx];
					const TJTransform& t = ptTrans.at(r.idx);
					std::string nm = t.replaced ? (t.newNameZh.empty() ? t.newName : t.newNameZh)
					                            : (n.nameZh.empty() ? n.name : n.nameZh);
					if (r.prio) nm = u8"★ " + nm;
					ImVec4 nc = r.big ? ImVec4(0.98f, 0.82f, 0.42f, 1.0f)
					                  : ImVec4(0.82f, 0.86f, 0.95f, 1.0f);
					ImGui::PushID(r.idx);
					ImGui::PushStyleColor(ImGuiCol_Text, nc);
					bool sel = ImGui::Selectable((nm + u8"：").c_str(), emphNode == r.idx);
					ImGui::PopStyleColor();
					if (sel) { panToNode = r.idx; emphNode = r.idx; }
					for (size_t i = 0; i < t.lines.size(); i++) {
						const std::string& zh = (i < t.linesZh.size() && !t.linesZh[i].empty())
						                        ? t.linesZh[i] : t.lines[i];
						ImGui::Indent(18 * scale);
						if (colorStats) ImGui::PushStyleColor(ImGuiCol_Text, stat_color(zh));
						ImGui::TextWrapped("%s", zh.c_str());
						if (colorStats) ImGui::PopStyleColor();
						ImGui::Unindent(18 * scale);
					}
					ImGui::PopID();
				}
				ImGui::EndChild();
			}
		}
		}; // end drawAffectedList

		ImGui::EndChild(); // ##left
		ImGui::SameLine();

		// ---- middle column: passive tree + jewel radius ------------------
		const float rightW = 400.0f * scale;
		float midW = ImGui::GetContentRegionAvail().x - rightW - ImGui::GetStyle().ItemSpacing.x;
		if (midW < 240.0f) midW = ImGui::GetContentRegionAvail().x * 0.62f;
		ImGui::BeginChild("##mid", ImVec2(midW, 0), false);
		if (!ptTexOk) {
			ImGui::TextWrapped(u8"天賦樹視圖無法載入：\n%s", ptErr.c_str());
			ImGui::Spacing();
			ImGui::TextDisabled(u8"（計算器其餘功能仍可使用）");
		} else {
			// header: hint + selected socket + preview seed
			if (selSocket < 0)
				ImGui::TextDisabled(u8"在樹上點擊任一珠寶插槽以選擇位置");
			else {
				const PtNode& sn = ptData.nodes[selSocket];
				ImGui::Text(u8"插槽：%s", sn.nameZh.empty() ? sn.name.c_str() : sn.nameZh.c_str());
				ImGui::SameLine();
				if (detailSeed >= 0)
					ImGui::TextDisabled(u8"｜預覽種子 %d｜半徑內受影響節點以金框標示", detailSeed);
				else
					ImGui::TextDisabled(u8"｜選一個種子（搜尋結果按「查看」或輸入種子）即可預覽轉換");
			}

			// pick the socket's in-radius nodes to focus on. Empty selection = all.
			if (selSocket >= 0) {
				if (ptSelected.size() != ptData.nodes.size()) ptSelected.assign(ptData.nodes.size(), 0);
				std::vector<int> inRad = ptData.NodesInRadius(selSocket, 1800.0f);
				auto setRange = [&](bool sel, int kindFilter) {
					for (int idx : inRad) {
						if (ptData.nodes[idx].kind == kPtSocket) continue;
						if (kindFilter == 1 && ptData.nodes[idx].kind != kPtNotable && ptData.nodes[idx].kind != kPtKeystone) continue;
						if (kindFilter == 2 && ptData.nodes[idx].kind != kPtNormal) continue;
						ptSelected[idx] = sel ? 1 : 0;
					}
					selVersion++;
				};
				ImGui::TextDisabled(u8"選取："); ImGui::SameLine();
				if (ImGui::SmallButton(u8"全部")) setRange(true, 0); ImGui::SameLine();
				if (ImGui::SmallButton(u8"中型天賦##sel")) setRange(true, 1); ImGui::SameLine();
				if (ImGui::SmallButton(u8"一般##sel")) setRange(true, 2); ImGui::SameLine();
				if (ImGui::SmallButton(u8"清除")) setRange(false, 0);
				ImGui::SameLine(); ImGui::TextDisabled(u8"(點樹上節點單獨選取；未選=全部)");
			}

			// recompute highlight + per-node transforms when the input tuple changes
			// (selVersion covers the picked-node set).
			long long sig = ((long long)selSocket * 1000003 + detailSeed) * 97 +
			                (long long)jewelType * 13 + conquerorSel * 131 + selVersion;
			if (sig != hiSig) {
				hiSig = sig;
				ptHi.assign(ptData.nodes.size(), kPtHiNone);
				ptTrans.clear();
				statGroups.clear();
				emphNode = -1; hlStatGroup = -1;
				if (selSocket >= 0) {
					std::vector<int> inRad = ptData.NodesInRadius(selSocket, 1800.0f);
					bool anySel = false;
					for (int idx : inRad)
						if (idx < (int)ptSelected.size() && ptSelected[idx]) { anySel = true; break; }
					auto included = [&](int idx) {
						return !anySel || (idx < (int)ptSelected.size() && ptSelected[idx]);
					};
					bool haveBin = detailSeed >= 0 && ensureBin(jewelType);
					// transforms for ALL in-radius nodes (tooltips + tree highlight)
					for (int idx : inRad) {
						const PtNode& n = ptData.nodes[idx];
						if (detailSeed < 0 || !haveBin) {
							ptHi[idx] = kPtHiAffected; // radius-only preview (no seed yet)
							continue;
						}
						const char* nt = n.kind == kPtKeystone ? "Keystone"
						               : n.kind == kPtNotable ? "Notable" : "Normal";
						TJTransform t = TJApply(*ds, *blob, jewelType, detailSeed, n.id, nt,
						                        n.stats, ConquerorType(jewelType), conquerorId,
						                        n.name);
						if (t.ok && (!t.lines.empty() || t.replaced)) {
							ptHi[idx] = t.replaced ? kPtHiReplaced : kPtHiAffected;
							ptTrans[idx] = std::move(t);
						}
					}
					// stat groups from the PICKED subset (or all if nothing picked)
					std::map<std::string, int> gi; // normalized-en -> statGroups index
					for (const auto& kv : ptTrans) {
						if (!included(kv.first)) continue;
						bool big = ptData.nodes[kv.first].kind == kPtNotable ||
						           ptData.nodes[kv.first].kind == kPtKeystone || kv.second.replaced;
						for (size_t i = 0; i < kv.second.lines.size(); i++) {
							std::string key = TJNormalizeStat(kv.second.lines[i]);
							auto it = gi.find(key);
							int g;
							if (it == gi.end()) {
								g = (int)statGroups.size(); gi[key] = g;
								StatGroup sg;
								const std::string& disp = (i < kv.second.linesZh.size() && !kv.second.linesZh[i].empty())
								                          ? kv.second.linesZh[i] : kv.second.lines[i];
								sg.name = TJNormalizeStat(disp);
								statGroups.push_back(std::move(sg));
							} else g = it->second;
							(big ? statGroups[g].notables : statGroups[g].smalls).push_back(kv.first);
							double v = 0;
							const std::string& ln = kv.second.lines[i];
							for (size_t p = 0; p < ln.size(); p++)
								if (isdigit((unsigned char)ln[p])) { v = atof(ln.c_str() + p); break; }
							if (v > statGroups[g].maxVal) statGroups[g].maxVal = v;
						}
					}
				}
			}

			// per-frame draw highlight. Default: nothing framed (clean tree). Frame
			// only the nodes that MATCH a searched stat; a clicked stat-row narrows
			// it to that one stat's nodes.
			dispHi.assign(ptData.nodes.size(), kPtHiNone);
			if (hlStatGroup >= 0 && hlStatGroup < (int)statGroups.size()) {
				auto mark = [&](const std::vector<int>& v) {
					for (int idx : v) if (idx < (int)dispHi.size()) dispHi[idx] = ptHi[idx] ? ptHi[idx] : kPtHiAffected;
				};
				mark(statGroups[hlStatGroup].notables);
				mark(statGroups[hlStatGroup].smalls);
			} else if (!wants.empty()) {
				std::set<std::string> wset;
				for (const auto& w : wants) wset.insert(w.en);
				for (const auto& kv : ptTrans) {
					for (const auto& ln : kv.second.lines)
						if (wset.count(TJNormalizeStat(ln))) {
							dispHi[kv.first] = ptHi[kv.first] ? ptHi[kv.first] : kPtHiAffected;
							break;
						}
				}
			}

			PassiveTreeInput tin;
			tin.selectedSocket = selSocket;
			tin.radiusWorld = 1800.0f;
			tin.hi = &dispHi;
			tin.selected = ptSelected.empty() ? nullptr : &ptSelected;
			tin.emphasize = emphNode;
			PassiveTreeOutput tout = ptView.Draw(ptData, scale, tin);

			if (panToNode >= 0) { ptView.CenterOn(ptData, panToNode); panToNode = -1; }

			if (tout.clickedSocket >= 0 && tout.clickedSocket != selSocket) {
				selSocket = tout.clickedSocket;
				hiSig = -1; // force recompute next frame
				ptView.CenterOn(ptData, selSocket);
			}
			// click a non-socket node to add/remove it from the picked focus set
			if (tout.clickedNode >= 0) {
				if (ptSelected.size() != ptData.nodes.size()) ptSelected.assign(ptData.nodes.size(), 0);
				ptSelected[tout.clickedNode] = ptSelected[tout.clickedNode] ? 0 : 1;
				selVersion++;
			}

			// tooltip for the hovered node: transformed stats if affected, else base
			if (tout.hoveredNode >= 0) {
				const PtNode& n = ptData.nodes[tout.hoveredNode];
				ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * scale, 12 * scale));
				ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(440 * scale, FLT_MAX));
				ImGui::BeginTooltip();
				ImGui::PushTextWrapPos(400 * scale);
				auto it = ptTrans.find(tout.hoveredNode);
				if (it != ptTrans.end() && it->second.ok) {
					const TJTransform& t = it->second;
					const std::string& nm = !t.newNameZh.empty() ? t.newNameZh
					                       : !t.newName.empty() ? t.newName
					                       : (n.nameZh.empty() ? n.name : n.nameZh);
					ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f), "%s", nm.c_str());
					if (t.replaced) ImGui::TextDisabled(u8"（節點被替換）");
					ImGui::Separator();
					for (size_t i = 0; i < t.lines.size(); i++) {
						const std::string& zh = (i < t.linesZh.size() && !t.linesZh[i].empty())
						                        ? t.linesZh[i] : t.lines[i];
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.88f, 0.68f, 1.0f));
						ImGui::TextUnformatted(zh.c_str());
						ImGui::PopStyleColor();
					}
				} else {
					const std::string& nm = n.nameZh.empty() ? n.name : n.nameZh;
					ImVec4 c = n.kind == kPtKeystone ? ImVec4(0.85f, 0.45f, 0.85f, 1.0f)
					         : n.kind == kPtNotable ? ImVec4(0.95f, 0.80f, 0.40f, 1.0f)
					         : n.kind == kPtSocket ? ImVec4(0.70f, 0.78f, 0.95f, 1.0f)
					         : ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
					ImGui::TextColored(c, "%s", nm.empty() ? "?" : nm.c_str());
					const std::vector<std::string>& lines = n.statsZh.empty() ? n.stats : n.statsZh;
					if (!lines.empty()) ImGui::Separator();
					for (const std::string& s : lines) {
						ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.68f, 0.90f, 1.0f));
						ImGui::TextUnformatted(s.c_str());
						ImGui::PopStyleColor();
					}
				}
				ImGui::PopTextWrapPos();
				ImGui::EndTooltip();
				ImGui::PopStyleVar();
			}
		}
		ImGui::EndChild(); // ##mid
		ImGui::SameLine();

		// ---- right column: affected node / stat list (sidebar) -----------
		ImGui::BeginChild("##right", ImVec2(0, 0), true);
		ImGui::TextUnformatted(u8"受影響的節點 / 詞綴");
		ImGui::Separator();
		drawAffectedList();
		ImGui::EndChild(); // ##right

		ImGui::End();
		ImGui::PopFont();
		ImGui::Render();
		int fbW = 0, fbH = 0;
		glfwGetFramebufferSize(win, &fbW, &fbH);
		glViewport(0, 0, fbW, fbH);
		glClearColor(0.043f, 0.063f, 0.078f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
		glfwSwapBuffers(win);
	}

	job.cancel = true;
	if (job.th.joinable()) job.th.join();
	ptView.DestroyTextures();
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();
	glfwDestroyWindow(win);
	glfwTerminate();
}
