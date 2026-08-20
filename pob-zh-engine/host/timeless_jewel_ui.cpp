#include "timeless_jewel_ui.h"
#include "tool_panel.h"
#include "tool_window.h"

#include "launcher_config.h" // ResolveConfiguredFontPath
#include "http_client.h"
#include "passive_tree_data.h"
#include "passive_tree_update.h"
#include "passive_tree_view.h"
#include "timeless_jewel.h"
#include "timeless_jewel_abyss.h"
#include "ui_theme.h"
#include "clipboard_util.h"

#include <json.hpp> // nlohmann::json (deps/nlohmann) — TjUiState

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

} // namespace

// ---- trade realms -----------------------------------------------------------

// Verified against both /api/trade/data/stats: every conqueror the calculator
// can offer (jewel types 1-6, 23 conquerors) exists on both regions with the
// same id. --tj-realm-check re-proves this on demand.
const TradeRealm kTradeRealms[] = {
	// NOTE: the .tw site answers on the bare host; www.pathofexile.tw 301s to it,
	// so use the canonical one and save a redirect.
	{ u8"國際服", "www.pathofexile.com", L"www.pathofexile.com", true },
	{ u8"台服",   "pathofexile.tw",      L"pathofexile.tw",      false },
};
const int kTradeRealmCount = (int)(sizeof(kTradeRealms) / sizeof(kTradeRealms[0]));

// Query JSON for one seed. status "securable" == the trade site's "Instant
// Buyout" mode (per awakened-poe-trade: merchantOnly -> 'securable'). No
// trade_filters block: leaving sale_type unset keeps the "Sale Type" row at
// "Any". (An explicit sale_type — especially a JSON null — got ?q= rejected.)
std::string TradeQueryJson(const std::string& tradeStatId, int seed)
{
	char q[640];
	snprintf(q, sizeof(q),
		"{\"query\":{\"status\":{\"option\":\"securable\"},\"stats\":[{\"type\":\"and\",\"filters\":"
		"[{\"id\":\"%s\",\"value\":{\"min\":%d,\"max\":%d}}]}]"
		"},\"sort\":{\"price\":\"asc\"}}",
		tradeStatId.c_str(), seed, seed);
	return q;
}

// Query JSON matching ANY of several seeds (PoE "count" stat group, one filter
// per seed, count >= 1) so a whole match-group fits in one search.
std::string TradeQueryJsonMulti(const std::string& tradeStatId, const std::vector<int>& seeds)
{
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
	return "{\"query\":{\"status\":{\"option\":\"securable\"},\"stats\":[{\"type\":\"count\","
	       "\"value\":{\"min\":1},\"filters\":[" + filters + "]}]"
	       "},\"sort\":{\"price\":\"asc\"}}";
}

// Pure URL assembly, kept separate from ShellExecute so --tj-selftest can
// assert on it. Returns "" when there is nothing sensible to open.
// platform: 0 pc, 1 xbox, 2 sony — ignored by realms without consoles.
std::string TradeSearchUrl(int realmIdx, const std::string& league, int platform,
                           const std::string& queryJson)
{
	if (league.empty() || queryJson.empty()) return std::string();
	if (realmIdx < 0 || realmIdx >= kTradeRealmCount) realmIdx = 0;
	const TradeRealm& r = kTradeRealms[realmIdx];
	const char* console = "";
	if (r.consoles) console = platform == 1 ? "xbox/" : platform == 2 ? "sony/" : "";
	// url_encode is byte-wise, so a Chinese league name comes out as the same
	// percent-escapes the trade site itself uses (亡焰咒海 -> %E4%BA%A1...).
	return "https://" + std::string(r.host) + "/trade/search/" + console +
	       url_encode(league) + "?q=" + url_encode(queryJson);
}

// ---- TjUiState --------------------------------------------------------------

static std::wstring tj_ui_path(const std::wstring& exeDir)
{
	return exeDir + L"PobTools\\tj_ui.json";
}

bool TjUiState::Load(const std::wstring& exeDir)
{
	std::vector<unsigned char> raw = read_file(tj_ui_path(exeDir));
	if (raw.empty()) return false;
	try {
		nlohmann::json doc = nlohmann::json::parse(std::string(raw.begin(), raw.end()));
		realm = doc.value("realm", 0);
		platform = doc.value("platform", 0);
		league = doc.value("league", std::string());
		return true;
	} catch (...) {
		realm = 0; platform = 0; league.clear();
		return false;
	}
}

bool TjUiState::Save(const std::wstring& exeDir) const
{
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	nlohmann::json doc;
	doc["realm"] = realm;
	doc["platform"] = platform;
	if (!league.empty()) doc["league"] = league;
	std::string out = doc.dump();
	HANDLE f = CreateFileW(tj_ui_path(exeDir).c_str(), GENERIC_WRITE, 0, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD wrote = 0;
	bool ok = WriteFile(f, out.data(), (DWORD)out.size(), &wrote, nullptr) && wrote == out.size();
	CloseHandle(f);
	return ok;
}

namespace {

void open_url(const std::string& url)
{
	if (url.empty()) return;
	ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Open the trade site pre-filled to search for a specific jewel seed.
void open_trade_search(const std::string& tradeStatId, int seed,
                       const std::string& league, int platform, int realmIdx)
{
	if (tradeStatId.empty()) return;
	open_url(TradeSearchUrl(realmIdx, league, platform, TradeQueryJson(tradeStatId, seed)));
}

void open_trade_search_multi(const std::string& tradeStatId, const std::vector<int>& seeds,
                             const std::string& league, int platform, int realmIdx)
{
	if (tradeStatId.empty() || seeds.empty()) return;
	open_url(TradeSearchUrl(realmIdx, league, platform, TradeQueryJsonMulti(tradeStatId, seeds)));
}

// Background one-shot fetch of the current trade leagues. The API lists every
// realm in one array ({"id":"Allflame","realm":"pc",...}), current league first,
// so keep the realm alongside the id and preserve that order.
struct LeagueFetch {
	std::atomic<bool> running{ false }, done{ false };
	std::vector<std::pair<std::string, std::string>> all; // (realm, id), API order
	std::thread th;
	const wchar_t* host = kTradeRealms[0].hostW;
	~LeagueFetch() { if (th.joinable()) th.join(); }
	// Switching region throws the list away and refetches from the new host.
	// Joining first keeps `all` from being written by the outgoing thread.
	void SetHost(const wchar_t* h) {
		if (h == host) return;
		if (th.joinable()) th.join();
		host = h;
		all.clear();
		done = false;
		running = false;
	}
	void start() {
		if (running.load() || done.load()) return;
		running = true;
		if (th.joinable()) th.join();
		th = std::thread([this]() {
			HttpsClient c(host);
			std::string body, err;
			std::vector<std::pair<std::string, std::string>> got;
			if (c.valid() && c.GetString(L"/api/trade/data/leagues", body, &err)) {
				// crude JSON scan: each entry is {"id":"..","realm":"..","text":".."}
				size_t p = 0;
				while ((p = body.find("\"id\":\"", p)) != std::string::npos) {
					p += 6;
					size_t e = body.find('"', p);
					if (e == std::string::npos) break;
					std::string id = body.substr(p, e - p);
					std::string realm = "pc";
					size_t r = body.find("\"realm\":\"", e);
					size_t nextId = body.find("\"id\":\"", e);
					if (r != std::string::npos && (nextId == std::string::npos || r < nextId)) {
						r += 9;
						size_t re = body.find('"', r);
						if (re != std::string::npos) realm = body.substr(r, re - r);
					}
					got.emplace_back(std::move(realm), std::move(id));
					p = e;
				}
			}
			all = std::move(got);
			running = false; done = true;
		});
	}
	// League ids for one platform, API order (current league first), deduped.
	std::vector<std::string> ForPlatform(int platform) const {
		const char* want = platform == 1 ? "xbox" : platform == 2 ? "sony" : "pc";
		std::vector<std::string> out;
		for (const auto& kv : all) {
			if (kv.first != want) continue;
			bool dup = false;
			for (const auto& s : out) if (s == kv.second) { dup = true; break; }
			if (!dup) out.push_back(kv.second);
		}
		return out;
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
	case 6: return u8"英勇悲劇 (Heroic Tragedy)";
	// Abyss jewels (3.29): one per Abyssal Lord, all seeded 100-8000.
	case 7: return u8"潰爛復仇 (Festering Vengeance)";
	case 8: return u8"撲滅之握 (Extinguishing Grasp)";
	case 9: return u8"邪惡統治 (Baleful Dominion)";
	case 10: return u8"滅亡之願 (Destructive Aspiration)";
	case 11: return u8"重奪惡意 (Reclaimed Malevolence)";
	}
	return "?";
}

// kMaxJewelType lives in timeless_jewel_ui.h so both selftests can assert
// against it. Both of the reasons the Abyss jewels used to be held back are
// gone: PoB 2.67 carries the Abyssal Lords in ModParser's conquerorList and a
// unique definition for each jewel, and timeless_jewel_abyss.cpp now reads the
// ABYS/ABYN containers. Only Zorath stays hidden, for a reason that is about
// PobTools and not about PoB — see the header.

// Legion jewels affect a Large radius (1800 world units). The Abyss ones have no
// radius at all: their file names the conquered passives directly, and the item
// text says "Passives affected" rather than "Passives in radius". So for 7-10
// the socket is not a way of choosing a radius — it IS the lookup key, and
// nothing can be searched without one.
bool JewelUsesRadius(int jewelType) { return jewelType >= 1 && jewelType <= 6; }

const char* ConquerorType(int jewelType)
{
	switch (jewelType) {
	case 1: return "vaal";
	case 2: return "karui";
	case 3: return "maraketh";
	case 4: return "templar";
	case 5: return "eternal";
	case 6: return "kalguur";
	// Abyss keystone ids are prefixed by jewel, not by conqueror
	// (abyss_murderous_keystone); TJApply falls back to the unsuffixed form.
	case 7: return "abyss_murderous";
	case 8: return "abyss_searching";
	case 9: return "abyss_hypnotic";
	case 10: return "abyss_ghastly";
	case 11: return "abyss_special";
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

	// The LUT is taken as a shared_ptr, not a raw pointer, so that switching
	// jewel mid-search cannot pull the buffer out from under the worker: the
	// loader hands the UI a NEW buffer and this thread keeps the old one alive
	// until it finishes.
	void start(const TJDataset* ds, std::shared_ptr<const std::string> blob, TJSearchQuery q) {
		if (th.joinable()) { cancel = true; th.join(); }
		cancel = false; done = false; running = true;
		results.clear();
		th = std::thread([this, ds, blob, q]() {
			auto r = TJSearch(*ds, *blob, q, 500, &cancel);
			results = std::move(r);
			running = false; done = true;
		});
	}

	// Abyss jewels search one socket's block rather than a set of nodes, so they
	// need the parsed container and the socket id instead of q.nodeIds.
	//
	// `lut` and `nodeKind` are taken BY VALUE. TJAbyssReadSocket fills a per-seed
	// offset cache inside the LUT as it walks, and the detail panel reads the
	// same socket on the UI thread every frame — sharing one index would be two
	// threads writing one std::map. Copying costs the block table (21 entries)
	// plus whatever offsets are already cached; the expensive part, the walk that
	// built the block table, is not repeated.
	void startAbyss(const TJDataset* ds, std::shared_ptr<const std::string> blob,
	                TJAbyssLUT lut, TJSearchQuery q, int socketId,
	                std::map<int, int> nodeKind) {
		if (th.joinable()) { cancel = true; th.join(); }
		cancel = false; done = false; running = true;
		results.clear();
		th = std::thread([this, ds, blob, lut = std::move(lut), q, socketId,
		                  nodeKind = std::move(nodeKind)]() mutable {
			auto r = TJAbyssSearch(*ds, *blob, lut, q, socketId, 500, &nodeKind, &cancel);
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

// ---- timeless jewel calculator, as a panel ----------------------------------
//
// The window / GL context / font atlas / main loop belong to whichever host is
// drawing this: RunToolWindow for a window of its own, the launcher's tab body
// when embedded. See tool_panel.h.
//
// Same mechanical move as the other tools -- every local of ShowTimelessJewel is
// a member and every captured closure a member function -- so the UI body below
// moved across unchanged.

namespace {

// Was declared inside ShowTimelessJewel; a member cannot have a type local to
// another function.
//
// stat-centric list (Vilsol style): a rolled stat -> the nodes that gained it
struct StatGroup { std::string name; std::vector<int> notables, smalls; double maxVal = 0; };

} // namespace

class TimelessJewelPanel : public IToolPanel {
public:
	bool Init(const ToolPanelHost& h) override
	{
		host_ = &h;
		exeDir = h.exeDir;
		scale = h.scale;

		// Data first: a clear message beats an empty window.
		ds = std::make_shared<TJDataset>();
		if (!ds->Load(exeDir + L"Data\\timeless_jewels.json", &derr)) {
			// Reported, not shown: this runs inside the launcher's frame. See
			// IToolPanel::InitError.
			initErr_ = u8"無法載入 timeless_jewels.json（資料檔遺失）。";
			return false;
		}

		ptDataOk = ptData.Load(exeDir, &ptErr);
		ptTexOk = ptDataOk && ptView.LoadTextures(exeDir, ptData, &ptErr);

		ptUpdater.Init(exeDir);
		ptUpdater.RequestCheck(false);   // throttled to once per day
		computeZhPct();

		if (tjUi.Load(exeDir)) {
			tradeRealm = std::clamp(tjUi.realm, 0, kTradeRealmCount - 1);
			tradePlatform = kTradeRealms[tradeRealm].consoles ? std::clamp(tjUi.platform, 0, 2) : 0;
			if (!tjUi.league.empty()) { tradeLeague = tjUi.league; leagueUserSet = true; }
		}
		leagues.SetHost(kTradeRealms[tradeRealm].hostW);

		templates = TJStatTemplates(*ds, jewelType);
		templatesFor = jewelType;
		rebuildKinds();
		binOk = loadBinFor(jewelType);
		return true;
	}

	void Frame() override
	{
		// Updater results land on the worker thread; applied here, on the GL thread.
		PassiveTreeUpdater::Status ptUst = ptUpdater.Poll();
		if (ptUst.reloadPending) {
			// a new league's tree + sheets landed on disk: hot reload everything
			// that hangs off the tree (selection, highlights, search bindings)
			ptDataOk = ptData.Load(exeDir, &ptErr);
			ptTexOk = ptDataOk && ptView.LoadTextures(exeDir, ptData, &ptErr);
			rebuildKinds(); // node kinds drive the Abyss scope filter
			ptView.ResetView();
			selSocket = -1;
			ptSelected.clear();
			ptHi.clear();
			dispHi.clear();
			ptTrans.clear();
			statGroups.clear();
			hiSig = -1;
			detailSeed = -1;
			selVersion++;
			computeZhPct();
			ptUpdater.AckReload();
			ptUst = ptUpdater.Poll();
		}
		// left column: the search form (fixed width); right column: the tree.
		const float leftW = 430.0f * scale;
		ImGui::BeginChild("##left", ImVec2(leftW, 0), false);

		ImGui::TextUnformatted(u8"軍團珠寶計算器");
		ImGui::SameLine();
		ImGui::TextDisabled(u8"Timeless Jewel");
		ImGui::Separator();

		// fetch the trade leagues once on open so the export defaults to the
		// current league instead of Standard (start() is a no-op once done)
		leagues.start();
		if (leagues.done.load() && !leagueUserSet) {
			std::vector<std::string> lg = leagues.ForPlatform(tradePlatform);
			if (!lg.empty() && tradeLeague != lg[0]) { tradeLeague = lg[0]; saveTjUi(); }
		}

		// --- paste a jewel from the clipboard (auto-fills jewel/conqueror/seed) ---
		if (ImGui::Button(u8"貼上珠寶 (從遊戲複製物品)", ImVec2(-1, 0))) {
			std::string txt = ReadClipboardUtf8(nullptr);
			TJPaste pasted = TJParsePaste(*ds, txt);
			const int foundJewel = pasted.jewelType;
			const int foundConqIdx = pasted.conqIndex;
			const int foundSeed = pasted.seed;
			const bool unsupported = foundJewel > kMaxJewelType;
			if (foundJewel && !unsupported) {
				jewelType = foundJewel; conquerorSel = foundConqIdx; ensureBin(jewelType);
			}
			if (foundSeed >= 0 && !unsupported) {
				seedText = std::to_string(foundSeed); detailSeed = foundSeed; mode = 1;
			}
			status = unsupported
			         ? (std::string(JewelZh(foundJewel)) +
			            u8" 尚未支援：它的效果取決於角色從插槽到起點的已配點路徑，本工具沒有角色資料")
			         : foundJewel ? (u8"已匯入：" + std::string(JewelZh(foundJewel)) +
			                         (foundSeed >= 0 ? u8"  種子 " + std::to_string(foundSeed) : ""))
			                      : u8"剪貼簿中未找到珠寶資訊（請在遊戲中對珠寶 Ctrl+C）";
			hiSig = -1;
		}

		// --- jewel type ---
		ImGui::TextUnformatted(u8"珠寶");
		ImGui::SetNextItemWidth(-1);
		if (ImGui::BeginCombo("##jewel", JewelZh(jewelType))) {
			for (int t = 1; t <= kMaxJewelType; t++)
				if (ImGui::Selectable(JewelZh(t), t == jewelType)) {
					jewelType = t;
					conquerorSel = 0;
					ensureBin(t);
				}
			ImGui::EndCombo();
		}

		// Standing caveat for the Abyss jewels. They are new in 3.29 and there is
		// far less community data behind them than behind the Legion ones: the
		// reader was checked against PoB's own container and an independent
		// implementation of the game's algorithm, but only 42 in-game recordings
		// exist to check either of those against. Say so where the choice is
		// made, not buried in a tooltip.
		if (TJIsAbyss(jewelType)) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.70f, 0.40f, 1.0f));
			ImGui::TextWrapped(u8"※ 深淵珠寶是 3.29 新增，目前可對照的實測樣本仍少，"
			                   u8"結果無法像軍團珠寶那樣完整驗證，請以遊戲內實際結果為準。");
			if (TJIsZorath(jewelType))
				ImGui::TextWrapped(u8"※ 重奪惡意另外還取決於你從插槽走到職業起點的「已配點路徑」，"
				                   u8"本工具沒有角色資料 —— 請隨意點一個插槽，用它周邊的天賦來判斷這顆種子。"
				                   u8"單一天賦會變成什麼、以及昇華天賦的選擇，這兩項是準確的。");
			ImGui::PopStyleColor();
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

		// --- trade export settings (region + league + platform) ---
		if (ImGui::CollapsingHeader(u8"交易站匯出設定")) {
			ImGui::TextUnformatted(u8"區域");
			ImGui::SameLine();
			for (int r = 0; r < kTradeRealmCount; r++) {
				if (r) ImGui::SameLine();
				if (ImGui::RadioButton(kTradeRealms[r].label, tradeRealm == r) && tradeRealm != r) {
					tradeRealm = r;
					// League names are region-specific, so the current pick and the
					// cached list are both meaningless now: refetch and re-default.
					if (!kTradeRealms[r].consoles) tradePlatform = 0;
					leagues.SetHost(kTradeRealms[r].hostW);
					leagueUserSet = false;
					saveTjUi();
				}
			}
			ImGui::TextUnformatted(u8"聯盟");
			ImGui::SameLine();
			ImGui::SetNextItemWidth(180 * scale);
			std::vector<std::string> lgList = leagues.ForPlatform(tradePlatform);
			if (!lgList.empty()) {
				if (ImGui::BeginCombo("##league", tradeLeague.c_str())) {
					for (const auto& lg : lgList)
						if (ImGui::Selectable(lg.c_str(), lg == tradeLeague)) {
							tradeLeague = lg;
							leagueUserSet = true;
							saveTjUi();
						}
					ImGui::EndCombo();
				}
			} else {
				if (ImGui::InputText("##league", &tradeLeague)) leagueUserSet = true;
				if (ImGui::IsItemDeactivatedAfterEdit()) saveTjUi();
			}
			ImGui::SameLine();
			if (leagues.running.load()) ImGui::TextDisabled(u8"取得中…");
			else if (ImGui::SmallButton(u8"重新取得")) { leagues.done = false; leagues.start(); }
			// Consoles are an international-realm concept; the .tw site has none.
			if (kTradeRealms[tradeRealm].consoles) {
				ImGui::TextUnformatted(u8"平台");
				ImGui::SameLine();
				int before = tradePlatform;
				ImGui::RadioButton("PC", &tradePlatform, 0); ImGui::SameLine();
				ImGui::RadioButton("Xbox", &tradePlatform, 1); ImGui::SameLine();
				ImGui::RadioButton("PS", &tradePlatform, 2);
				// Only remember the choice. Deliberately NOT clearing leagueUserSet:
				// switching platform never re-defaulted the league before, and this
				// change is not the place to alter that.
				if (tradePlatform != before) saveTjUi();
			}
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
			ImGui::Checkbox(u8"必須包含全部已選詞綴", &requireAll);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"關閉後只要命中任一條就算，權重高的仍排前面。\n"
				                  u8"開啟時，只滿足其中一條的種子不會出現。");
			// Abyss jewels conquer keystones too, and TJAbyssInScope counts them
			// as "big" — the label has to say so or the filter looks like it is
			// dropping results.
			ImGui::RadioButton(TJIsAbyss(jewelType) ? u8"只大型天賦與鑰石"
			                                        : u8"只中型天賦 (Notables)", &scope, 1);
			ImGui::SameLine();
			ImGui::RadioButton(u8"全部節點", &scope, 0);
			} // searchInputsOpen

			// --- search ---
			bool busy = job.running.load();
			const bool isAbyss = TJIsAbyss(jewelType);
			const bool isZorath = TJIsZorath(jewelType);
			// Every jewel needs a socket, for three different reasons: a Legion
			// one to know which radius to walk, types 7-10 because the socket IS
			// the key their file is indexed by, and Zorath because it is the
			// place the user is asking us to look around.
			if (selSocket < 0) {
				// Wrapped, not TextColored: the left column is 430px and this
				// sentence does not fit on one line in any wording — the Legion
				// one was already running off the edge.
				const char* hint =
					isZorath ? u8"請先在中間樹上點擊珠寶插槽（隨意一個即可，用來決定要看哪一帶的天賦）"
					: isAbyss ? u8"請先在中間樹上點擊珠寶插槽（深淵珠寶的結果由插槽決定，沒有半徑）"
					          : u8"請先在中間樹上點擊珠寶插槽（搜尋只計算該插槽半徑內的節點）";
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.70f, 0.40f, 1.0f));
				ImGui::TextWrapped("%s", hint);
				ImGui::PopStyleColor();
			}
			ImGui::BeginDisabled(busy || wants.empty() || selSocket < 0);
			if (ImGui::Button(u8"搜尋此插槽", ImVec2(-1, 34 * scale))) {
				if (ensureBin(jewelType)) {
					TJSearchQuery q;
					q.jewelType = jewelType;
					q.scope = scope;
					q.minTotalWeight = minTotalWeight;
					q.requireAll = requireAll;
					for (auto& w : wants) q.wants.push_back({ w.en, w.minValue, w.weight });
					detailSeed = -1;
					status = u8"搜尋中…";
					searchInputsOpen = false; // collapse inputs, show results
					// Types 7-10 need no node list: their file names the conquered
					// passives. Everything else is judged on the socket's
					// neighbourhood, narrowed to the user's picks when there are
					// any — for Zorath that neighbourhood stands in for a path we
					// cannot know, so letting the user pick the nodes IS the way
					// to aim it at their own build.
					if (!isAbyss || isZorath) {
						std::vector<int> inRad = ptData.NodesInRadius(selSocket, 1800.0f);
						bool anySel = false;
						for (int idx : inRad)
							if (idx < (int)ptSelected.size() && ptSelected[idx]) { anySel = true; break; }
						for (int idx : inRad) {
							if (ptData.nodes[idx].kind == kPtSocket) continue;
							if (anySel && !(idx < (int)ptSelected.size() && ptSelected[idx])) continue;
							q.nodeIds.push_back(ptData.nodes[idx].id);
						}
					}
					if (isAbyss)
						job.startAbyss(ds.get(), blob, *abyssLut, q,
						               ptData.nodes[selSocket].id, ptKind);
					else
						job.start(ds.get(), blob, q);
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

				// Cross-check reminder. Only shown after the user actually opened a
				// trade search, so it does not add noise to the normal search flow.
				// No arrow glyphs here: the CJK font atlas does not carry them and
				// they render as tofu (see error_imgui_font_atlas_missing_glyphs).
				if (tradeHintShown) {
					ImGui::Separator();
					ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.98f, 0.78f, 0.35f, 1.0f));
					ImGui::TextUnformatted(u8"提醒：買之前先用 Path of Building 對一次答案");
					ImGui::PopStyleColor();
					ImGui::TextWrapped(u8"%s",
					    u8"1. 在交易站找到那顆珠寶，Ctrl+C 複製，貼進 POB 的物品欄。\n"
					    u8"2. 也可以把交易站上「已鑲好這顆珠寶的整件裝備」複製進 POB。\n"
					    u8"3. 對照 POB 算出來的天賦加成，確認與本工具列出的詞綴一致。");
					ImGui::TextDisabled(u8"%s",
					    u8"本工具的轉換演算法以 POB 為規格、數值以遊戲檔為真值，"
					    u8"兩邊理應相同。若對不上請回報，那代表其中一邊有問題。");
					ImGui::Separator();
				}

				if (groupResults) {
					// the socket's picked in-radius nodes (or all) + wanted set
					std::vector<int> inRad;
					// Zorath is judged on the same neighbourhood the search used;
					// only 7-10 get their node list from the file instead.
					if (selSocket >= 0 && (!TJIsAbyss(jewelType) || TJIsZorath(jewelType))) {
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
					const TJWantMatcher matcher = makeMatcher();

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
								open_trade_search_multi(tradeStatId, seeds, tradeLeague, tradePlatform, tradeRealm);
								tradeHintShown = true;
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
								// Coverage next to the score: with several stats picked,
								// "weight 3" alone cannot tell "all three stats once" from
								// "one stat three times".
								ImGui::TextColored(ImVec4(0.98f, 0.62f, 0.30f, 1.0f),
								                   u8"種子 %d (權重 %.0f · 詞綴 %d/%d)",
								                   h->seed, h->weight, h->distinctWants, (int)wants.size());
								ImGui::SameLine();
								if (ImGui::SmallButton(u8"查看")) detailSeed = h->seed;
								ImGui::SameLine();
								ImGui::BeginDisabled(tradeOff);
								if (ImGui::SmallButton(u8"交易")) {
									open_trade_search(tradeStatId, h->seed, tradeLeague, tradePlatform, tradeRealm);
									tradeHintShown = true;
								}
								ImGui::EndDisabled();
								// this seed's matched affixes only (no node name), on the
								// picked nodes — keeps the list compact per the request
								auto drawMatched = [&](const TJTransform& t) {
									for (size_t i = 0; i < t.lines.size(); i++) {
										const TJWantStat* w = matcher.Match(t.lines[i]);
										if (!w) continue; // below 最小值 counts for nothing, so it shows as nothing
										const std::string& zh = (i < t.linesZh.size() && !t.linesZh[i].empty())
										                        ? t.linesZh[i] : t.lines[i];
										if (colorStats) ImGui::PushStyleColor(ImGuiCol_Text, stat_color(zh));
										// Show what this line contributed, so the seed's rank is legible.
										if (w->weight != 1.0)
											ImGui::BulletText(u8"%s  [權重 %.1f]", zh.c_str(), w->weight);
										else
											ImGui::BulletText("%s", zh.c_str());
										if (colorStats) ImGui::PopStyleColor();
									}
								};
								if (TJIsAbyss(jewelType)) {
									// For 7-10 inRad is empty and always will be — the
									// conquered passives come from the file, so without
									// this branch every seed here would list nothing.
									// Zorath is the other way round: the file has an
									// answer for every passive, so the neighbourhood is
									// what decides which answers are worth showing.
									std::map<int, TJAbyssMod> rec;
									bool got = false;
									if (selSocket >= 0) {
										if (TJIsZorath(jewelType)) {
											std::vector<int> ids;
											ids.reserve(inRad.size());
											for (int idx : inRad) ids.push_back(ptData.nodes[idx].id);
											got = TJAbyssReadNodes(*ds, *blob, *abyssLut, ids, h->seed, rec);
										} else {
											got = TJAbyssReadSocket(*ds, *blob, *abyssLut,
											                        ptData.nodes[selSocket].id,
											                        h->seed, rec);
										}
									}
									if (got) {
										for (const auto& kv : rec) {
											if (!TJAbyssInScope(scope, kv.first, *ds, &ptKind)) continue;
											TJTransform t = TJAbyssApply(*ds, kv.second);
											if (t.ok) drawMatched(t);
										}
									}
								} else {
									for (int idx : inRad) {
										const PtNode& n = ptData.nodes[idx];
										const char* nt = n.kind == kPtKeystone ? "Keystone"
										               : n.kind == kPtNotable ? "Notable" : "Normal";
										TJTransform t = TJApply(*ds, *blob, jewelType, h->seed, n.id, nt,
										                        n.stats, ConquerorType(jewelType), conquerorId, n.name);
										if (t.ok) drawMatched(t);
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
						if (ImGui::SmallButton(u8"交易")) {
							open_trade_search(tradeStatId, h.seed, tradeLeague, tradePlatform, tradeRealm);
							tradeHintShown = true;
						}
						ImGui::EndDisabled();
						ImGui::SameLine();
						if (ImGui::SmallButton(u8"複製")) {
							std::string t = TJItemText(*ds, jewelType, conquerorSel, h.seed);
							status = (!t.empty() && WriteClipboardUtf8(nullptr, t))
							         ? u8"已複製物品文字，可在 POB 的物品欄貼上 (Ctrl+V)"
							         : u8"複製失敗";
						}
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
			// "in radius" is only true for the Legion jewels. The Abyss ones
			// name their conquered passives in the file and scatter them over
			// the whole tree, so the same wording would describe the wrong rule.
			ImGui::Text(TJIsZorath(jewelType) ? u8"種子 %d 對這一帶天賦的效果"
			            : TJIsAbyss(jewelType) ? u8"種子 %d 征服的天賦"
			                                   : u8"種子 %d 範圍內的變更",
			            detailSeed);
			ImGui::SameLine();
			ImGui::BeginDisabled(tradeStatId.empty() || tradeLeague.empty());
			if (ImGui::SmallButton(u8"交易搜尋")) {
				open_trade_search(tradeStatId, detailSeed, tradeLeague, tradePlatform, tradeRealm);
				tradeHintShown = true;
			}
			ImGui::EndDisabled();
			ImGui::SameLine();
			// Hand the jewel to PoB the way PoB expects to receive items: as the
			// game's own copy text on the clipboard.
			if (ImGui::SmallButton(u8"複製給 POB")) {
				std::string t = TJItemText(*ds, jewelType, conquerorSel, detailSeed);
				status = (!t.empty() && WriteClipboardUtf8(nullptr, t))
				         ? u8"已複製物品文字，可在 POB 的物品欄貼上 (Ctrl+V)"
				         : u8"複製失敗";
			}
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip(u8"複製成遊戲的物品文字格式，貼進 POB「物品」分頁即可建立這顆珠寶");

			// Zorath's Ascendancy pick needs no path, so unlike everything else
			// about this jewel it is exact. That makes it the most trustworthy
			// thing on screen and worth its own section.
			if (TJIsZorath(jewelType) && detailSeed >= 0 && binOk) {
				if (ImGui::CollapsingHeader(u8"昇華天賦選擇（準確）")) {
					std::map<std::string, std::vector<int>> asc;
					if (!TJAbyssReadAscendancies(*blob, *abyssLut, detailSeed, asc)) {
						ImGui::TextDisabled(u8"讀取失敗");
					} else {
						ImGui::BeginChild("##asc", ImVec2(0, 170 * scale), true);
						for (const auto& kv : asc) {
							ImGui::TextColored(ImVec4(0.98f, 0.62f, 0.30f, 1.0f), "%s", kv.first.c_str());
							if (kv.second.empty()) {
								// Ascendant is the standing case: all of its notables cost
								// five points and the jewel cannot rewrite one costing four
								// or more.
								ImGui::SameLine();
								ImGui::TextDisabled(u8"（不受影響）");
								continue;
							}
							for (int nid : kv.second) {
								TJAbyssMod m;
								if (!TJAbyssReadNode(*ds, *blob, *abyssLut, nid, detailSeed, m)) continue;
								TJTransform t = TJAbyssApply(*ds, m);
								if (t.replaced) {
									const std::string& nm = t.newNameZh.empty() ? t.newName : t.newNameZh;
									ImGui::BulletText(u8"變為「%s」", nm.c_str());
								}
								for (size_t i = 0; i < t.lines.size(); i++) {
									const std::string& z = (i < t.linesZh.size() && !t.linesZh[i].empty())
									                       ? t.linesZh[i] : t.lines[i];
									if (colorStats) ImGui::PushStyleColor(ImGuiCol_Text, stat_color(z));
									ImGui::BulletText("%s", z.c_str());
									if (colorStats) ImGui::PopStyleColor();
								}
							}
						}
						ImGui::EndChild();
						// The Ascendancy notable's own name is not available: PobTools'
						// tree leaves Ascendancy nodes out. What it becomes is the part
						// being chosen, so that is what is shown.
						ImGui::TextDisabled(u8"（本工具的樹資料不含昇華節點，只顯示它會變成什麼）");
					}
				}
			}

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
					ImGui::TextDisabled("%s", TJIsAbyss(jewelType) ? u8"這個插槽在此種子下沒有可顯示的詞綴（或資料仍在計算）。"
					                     : u8"此範圍內沒有受影響的詞綴（或資料仍在計算）。");
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
				const TJWantMatcher nodeMatcher = makeMatcher();
				auto matchesWants = [&](const TJTransform& t) {
					if (nodeMatcher.empty()) return false;
					for (const auto& ln : t.lines)
						if (nodeMatcher.Match(ln)) return true;
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
					ImGui::TextDisabled("%s", TJIsAbyss(jewelType) ? u8"這個插槽在此種子下沒有被征服的天賦（或資料仍在計算）。"
					                     : u8"此範圍內沒有受影響的節點（或資料仍在計算）。");
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

		// tree-update status row (drawn even when the tree failed to load —
		// updating is exactly the recovery path in that case)
		{
			bool busy = ptUst.phase == PassiveUpdatePhase::Downloading ||
			            ptUst.phase == PassiveUpdatePhase::Importing ||
			            ptUst.phase == PassiveUpdatePhase::Checking;
			if (ptUst.phase == PassiveUpdatePhase::UpdateAvailable) {
				ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.60f, 0.20f, 0.45f));
				ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.60f, 0.20f, 0.65f));
				ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.85f, 0.60f, 0.20f, 0.85f));
				if (ImGui::SmallButton((u8"發現新賽季天賦樹 " + ptUst.latestVer + u8"，點擊更新").c_str()))
					ptUpdater.StartUpdate();
				ImGui::PopStyleColor(3);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip(u8"%s", ptUst.message.c_str());
			} else if (busy) {
				ImGui::TextDisabled(u8"%s", ptUst.message.c_str());
			} else if (ptUst.phase == PassiveUpdatePhase::Error) {
				ImGui::TextColored(ImVec4(0.94f, 0.27f, 0.27f, 1.0f), u8"更新失敗：%s", ptUst.message.c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton(u8"重試##ptupd")) ptUpdater.StartUpdate();
			} else if (ptDataOk) {
				// idle: current tree version + Chinese stat-line coverage
				ImGui::TextDisabled(u8"樹 %s", ptData.TreeVersion().c_str());
				if (zhPct >= 0) {
					ImGui::SameLine();
					if (zhPct < 90) {
						ImGui::TextColored(ImVec4(0.90f, 0.65f, 0.25f, 1.0f), u8"｜繁中 %d%%（字典待更新）", zhPct);
						if (ImGui::IsItemHovered())
							ImGui::SetTooltip(u8"新賽季詞條在翻譯字典更新前顯示英文；更新翻譯包後重新匯入即可補齊");
					} else {
						ImGui::TextDisabled(u8"｜繁中 %d%%", zhPct);
					}
				}
				ImGui::SameLine();
				if (ImGui::SmallButton(u8"檢查更新##ptupd")) ptUpdater.RequestCheck(true);
			}
		}

		if (!ptTexOk) {
			ImGui::TextWrapped(u8"天賦樹視圖無法載入：\n%s", ptErr.c_str());
			ImGui::Spacing();
			ImGui::TextDisabled(u8"（計算器其餘功能仍可使用）");
		} else {
			// header: hint + selected socket + preview seed
			const bool treeRadius = JewelUsesRadius(jewelType);
			const bool treeAbyss = TJIsAbyss(jewelType);
			if (selSocket < 0) {
				ImGui::TextDisabled(u8"在樹上點擊任一珠寶插槽以選擇位置");
			} else {
				const PtNode& sn = ptData.nodes[selSocket];
				ImGui::Text(u8"插槽：%s", sn.nameZh.empty() ? sn.name.c_str() : sn.nameZh.c_str());
				ImGui::SameLine();
				if (detailSeed < 0)
					ImGui::TextDisabled(u8"｜選一個種子（搜尋結果按「查看」或輸入種子）即可預覽轉換");
				else if (TJIsZorath(jewelType))
					// The circle is where we are looking, not what the jewel hits.
					// Say that outright: the same ring means an area of effect for
					// every other jewel in this window.
					ImGui::TextDisabled(u8"｜預覽種子 %d｜圈內為判斷用，實際生效看你的配點", detailSeed);
				else if (treeAbyss)
					// Not "in radius": these are the passives the jewel's own file
					// names, and they are spread over the tree rather than sitting
					// inside a circle. Saying "radius" here would describe the
					// wrong mechanic.
					ImGui::TextDisabled(u8"｜預覽種子 %d｜此插槽被征服的天賦以金框標示", detailSeed);
				else
					ImGui::TextDisabled(u8"｜預覽種子 %d｜半徑內受影響節點以金框標示", detailSeed);
			}

			// pick the socket's in-radius nodes to focus on. Empty selection = all.
			if (selSocket >= 0 && (treeRadius || TJIsZorath(jewelType))) {
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
				const bool abyssDetail = TJIsAbyss(jewelType);
				if (selSocket >= 0 && (treeRadius || abyssDetail)) {
					const bool haveBin = detailSeed >= 0 && ensureBin(jewelType);

					// Where the two engines part company. A Legion jewel is given
					// a socket and works out which passives are in reach; an Abyss
					// one is TOLD which passives it took, and they are scattered
					// across the tree rather than sitting inside a circle. So one
					// side computes its candidates and the other reads them.
					std::vector<int> cand;
					std::map<int, TJAbyssMod> abyssMods; // node index -> modification
					const bool zorathDetail = TJIsZorath(jewelType);
					if (abyssDetail && !zorathDetail) {
						std::map<int, TJAbyssMod> byId;
						if (haveBin && TJAbyssReadSocket(*ds, *blob, *abyssLut,
						                                 ptData.nodes[selSocket].id,
						                                 detailSeed, byId)) {
							for (const auto& kv : byId) {
								const int idx = ptData.IndexOfId(kv.first);
								if (idx < 0) continue; // conquered node absent from our tree copy
								cand.push_back(idx);
								abyssMods[idx] = kv.second;
							}
						}
					} else {
						// Zorath and the Legion jewels both start from the socket's
						// neighbourhood — but they mean different things by it. For
						// a Legion jewel that IS the affected set; for Zorath it is
						// where we are looking, because the real set follows a path
						// through the character's own allocation.
						cand = ptData.NodesInRadius(selSocket, 1800.0f);
						if (zorathDetail && haveBin) {
							std::vector<int> ids;
							ids.reserve(cand.size());
							for (int idx : cand) ids.push_back(ptData.nodes[idx].id);
							std::map<int, TJAbyssMod> byId;
							TJAbyssReadNodes(*ds, *blob, *abyssLut, ids, detailSeed, byId);
							std::vector<int> kept;
							for (int idx : cand) {
								auto it = byId.find(ptData.nodes[idx].id);
								if (it == byId.end()) continue; // no block for this passive
								kept.push_back(idx);
								abyssMods[idx] = it->second;
							}
							cand.swap(kept);
						}
					}

					bool anySel = false;
					for (int idx : cand)
						if (idx < (int)ptSelected.size() && ptSelected[idx]) { anySel = true; break; }
					auto included = [&](int idx) {
						return !anySel || (idx < (int)ptSelected.size() && ptSelected[idx]);
					};
					for (int idx : cand) {
						const PtNode& n = ptData.nodes[idx];
						if (detailSeed < 0 || !haveBin) {
							ptHi[idx] = kPtHiAffected; // radius-only preview (no seed yet)
							continue;
						}
						TJTransform t;
						if (abyssDetail) {
							auto m = abyssMods.find(idx);
							if (m == abyssMods.end()) continue;
							t = TJAbyssApply(*ds, m->second);
						} else {
							const char* nt = n.kind == kPtKeystone ? "Keystone"
							               : n.kind == kPtNotable ? "Notable" : "Normal";
							t = TJApply(*ds, *blob, jewelType, detailSeed, n.id, nt,
							            n.stats, ConquerorType(jewelType), conquerorId,
							            n.name);
						}
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
							// same reading of a line's value as the search's 最小值 test
							double v = TJStatValue(kv.second.lines[i]);
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
				// Frame only nodes that really satisfy the query: a node whose roll
				// sits below 最小值 contributed nothing to this seed's score, so
				// framing it would point the eye at a node that is not why the seed
				// ranked where it did.
				const TJWantMatcher hiMatcher = makeMatcher();
				for (const auto& kv : ptTrans) {
					for (const auto& ln : kv.second.lines)
						if (hiMatcher.Match(ln)) {
							dispHi[kv.first] = ptHi[kv.first] ? ptHi[kv.first] : kPtHiAffected;
							break;
						}
				}
			}

			PassiveTreeInput tin;
			tin.selectedSocket = selSocket;
			// Zorath has no radius either, but this ring marks the area being
			// judged, not an area of effect — the header says which. Leaving it
			// invisible would hide what the numbers were computed from.
			tin.radiusWorld = (treeRadius || TJIsZorath(jewelType)) ? 1800.0f : 0.0f;
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
					else ImGui::TextDisabled(u8"（保留原詞綴，珠寶額外加成）");
					ImGui::Separator();
					// An addition leaves the node itself intact, so its own stats
					// still apply and must be shown above what the jewel adds.
					if (!t.replaced) {
						const std::vector<std::string>& base = n.statsZh.empty() ? n.stats : n.statsZh;
						for (const std::string& s : base) {
							ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.68f, 0.90f, 1.0f));
							ImGui::TextUnformatted(s.c_str());
							ImGui::PopStyleColor();
						}
					}
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
	}

	ToolCloseState RequestClose() override
	{
		// Nothing here is unsaved: the calculator writes only its own small ui state,
		// and it does that as the user changes it.
		if (close_ != ToolCloseState::Asking) close_ = ToolCloseState::Closed;
		return close_;
	}
	ToolCloseState CloseState() const override { return close_; }
	void AbortClose() override
	{
		if (close_ == ToolCloseState::Closed) close_ = ToolCloseState::Open;
	}

	void Shutdown() override
	{
		if (shutdown_) return;
		shutdown_ = true;
		job.cancel = true;
		if (job.th.joinable()) job.th.join();
		ptUpdater.Shutdown();     // cancels any in-flight download, joins the worker
		ptView.DestroyTextures(); // needs the GL context, which the host still has
	}

	~TimelessJewelPanel() override { Shutdown(); }

	PobUi::Density Density() const override { return PobUi::Density::Compact; }
	const char* PanelId() const override { return "tj"; }
	const char* InitError() const override { return initErr_.c_str(); }

private:
	// The search criterion, rebuilt from the current rows wherever the UI needs to
	// know "does this line count as a hit". Everything that shows or highlights a
	// match goes through TJWantMatcher so the display can never disagree with the
	// ranking -- it used to compare templates only and ignore 最小值, so a roll the
	// search had rejected still appeared as a hit.
	TJWantMatcher makeMatcher() const
	{
		std::vector<TJWantStat> v;
		v.reserve(wants.size());
		for (const auto& w : wants) v.push_back({ w.en, w.minValue, w.weight });
		return TJWantMatcher(v);
	}

	void computeZhPct()
	{
		if (!ptDataOk) { zhPct = -1; return; }
		int lines = 0, zh = 0;
		for (const PtNode& n : ptData.nodes)
			for (size_t i = 0; i < n.stats.size(); i++) {
				lines++;
				if (i < n.statsZh.size() && !n.statsZh[i].empty()) zh++;
			}
		zhPct = lines > 0 ? (int)(100.0 * zh / lines + 0.5) : -1;
	}

	void saveTjUi()
	{
		tjUi.realm = tradeRealm;
		tjUi.platform = tradePlatform;
		tjUi.league = tradeLeague;
		tjUi.Save(exeDir);
	}

	// Node kinds decide what "只看大天賦" means for an Abyss search. Keystones are
	// absent from the Legion node index, so without this map every conquered
	// keystone would be filed as a small passive and quietly filtered out.
	void rebuildKinds()
	{
		ptKind.clear();
		for (const PtNode& n : ptData.nodes) ptKind[n.id] = n.kind;
	}

	bool loadBinFor(int type)
	{
		// Fresh buffers rather than clearing in place: a search may still be reading
		// the old ones, and it holds its own reference to them.
		blob = std::make_shared<std::string>();
		abyssLut = std::make_shared<TJAbyssLUT>();
		bool ok = TJLoadBin(exeDir, *ds, type, *blob, &binErr);
		if (ok && TJIsAbyss(type) && !TJAbyssParse(*blob, type, *abyssLut, &binErr)) {
			// A container we cannot index is a failed load, not a usable one: reading
			// from a half-built index returns confident nonsense.
			blob = std::make_shared<std::string>();
			ok = false;
		}
		loadedBinType = type;
		return ok;
	}

	bool ensureBin(int type)
	{
		if (binOk && loadedBinType == type) return true;
		binOk = loadBinFor(type);
		return binOk;
	}

	// conqueror table (name + keystone id + trade pseudo-stat) from the dataset
	const std::vector<TJConqueror>* conqListFor(int type) const
	{
		auto it = ds->conquerors.find(type);
		return it != ds->conquerors.end() ? &it->second : nullptr;
	}

	const ToolPanelHost* host_ = nullptr;
	std::string initErr_;
	ToolCloseState close_ = ToolCloseState::Open;
	bool shutdown_ = false;

	std::wstring exeDir;
	float scale = 1.0f;

	std::shared_ptr<TJDataset> ds;
	std::string derr;

	int jewelType = 3;            // Brutal Restraint
	int conquerorSel = 0;         // index into per-jewel keystone list
	int mode = 0;                 // 0 = search by stats, 1 = enter seed
	int scope = 1;                // 1 = notables, 0 = all
	float minTotalWeight = 0.0f;
	bool requireAll = true;       // picking several stats means "all of them"
	std::string statFilter;
	std::vector<WantRow> wants;
	std::string seedText = "500";
	std::string status;
	int detailSeed = -1;          // a result seed to expand
	// Set once the user opens any trade search. The calculator's numbers come from
	// our own transform of the game data; PoB is the independent second opinion, so
	// nudge people to cross-check there before they spend currency.
	bool tradeHintShown = false;

	// --- passive tree view (right pane) ---
	PassiveTreeData ptData;
	PassiveTreeView ptView;
	std::string ptErr;
	bool ptDataOk = false;
	bool ptTexOk = false;

	// --- background tree updater + zh coverage (toolbar status) ---
	PassiveTreeUpdater ptUpdater;
	int zhPct = -1;               // % of stat lines with baked Chinese
	int selSocket = -1;                          // node index of the socketed jewel
	std::vector<unsigned char> ptHi;             // per-node highlight class
	std::vector<char> ptSelected;                // per-node: user-picked focus set (1 = picked)
	int selVersion = 0;                          // bumps on any selection change
	std::map<int, TJTransform> ptTrans;          // node index -> transform (affected only)
	std::vector<StatGroup> statGroups;
	std::vector<unsigned char> dispHi;           // ptHi, optionally filtered to one stat group
	long long hiSig = -1;         // signature of the last highlight computation
	int panToNode = -1;                          // list pick: glide the tree to this node
	int emphNode = -1;                           // list pick: keep this node ring-pulsed
	// affected-list display controls
	int listView = 0;                            // 0 = stat-centric (Vilsol), 1 = node-centric
	int statSort = 0;                            // 0 count, 1 alpha, 2 rarity, 3 value
	bool splitList = true;                       // split notables / smalls
	int hlStatGroup = -1;                        // stat row -> highlight only its nodes

	// --- trade export state ---
	// League defaults to the current one as soon as the list arrives (the trade API
	// lists it first); "Standard" only stands in while offline.
	std::string tradeLeague = "Standard";
	bool leagueUserSet = false;                  // user picked one -> stop auto-defaulting
	int tradePlatform = 0;                       // 0 pc, 1 xbox, 2 sony
	int tradeRealm = 0;                          // index into kTradeRealms
	LeagueFetch leagues;
	TjUiState tjUi;                              // remembers region/league/platform
	bool groupResults = true;                    // group seeds by # of stats matched
	bool searchInputsOpen = true;                // collapse the stat picker after a search

	// --- affected-node list display option ---
	bool colorStats = true;

	// stat picker templates are jewel-specific; recompute when the jewel changes
	std::vector<TJStatTemplate> templates;
	int templatesFor = 0;
	std::shared_ptr<std::string> blob = std::make_shared<std::string>();
	std::string binErr;
	// Abyss containers carry their own block index, built by walking the whole file
	// once. It lives beside the blob and is rebuilt whenever the blob is.
	std::shared_ptr<TJAbyssLUT> abyssLut = std::make_shared<TJAbyssLUT>();
	int loadedBinType = 0;
	bool binOk = false;

	SearchJob job;
	std::map<int, int> ptKind;
};

IToolPanel* CreateTimelessJewelPanel()
{
	return new TimelessJewelPanel();
}

void ShowTimelessJewel(const std::wstring& exeDir, const std::wstring& locale)
{
	TimelessJewelPanel panel;
	ToolWindowDesc desc;
	// "PobTools — 軍團珠寶計算器"
	desc.titleUtf8 = "PobTools \xe2\x80\x94 \xe8\xbb\x8d\xe5\x9c\x98\xe7\x8f\xa0\xe5\xaf\xb6\xe8\xa8\x88\xe7\xae\x97\xe5\x99\xa8";
	desc.defW = 1500;
	desc.defH = 940;
	desc.clampToWorkArea = true;
	RunToolWindow(panel, desc, exeDir, L"", locale);
}


// ---- cross-region stat id check (--tj-realm-check) --------------------------

// Every conqueror the calculator can OFFER must exist on every region, or its
// trade button would produce a search the site cannot run. This is how the one
// real gap was found: Zorath (jewel type 11) is absent from the .tw site — it
// is harmless today only because kMaxJewelType hides types 7-11. Raise that
// constant and this check starts failing, which is exactly the point.
int RunTradeRealmCheck(const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	std::string report;
	int failures = 0;
	auto line = [&](const std::string& s) { report += s + "\n"; printf("%s\n", s.c_str()); };

	TJDataset ds;
	std::string err;
	if (!ds.Load(exeDir + L"Data\\timeless_jewels.json", &err)) {
		line("FAIL  load timeless_jewels.json: " + err);
		return 1;
	}

	for (int r = 0; r < kTradeRealmCount; r++) {
		const TradeRealm& realm = kTradeRealms[r];
		HttpsClient c(realm.hostW);
		std::string body, herr;
		if (!c.valid() || !c.GetString(L"/api/trade/data/stats", body, &herr)) {
			line(std::string("FAIL  ") + realm.label + " (" + realm.host +
			     ") /api/trade/data/stats: " + herr);
			failures++;
			continue;
		}
		// Crude scan: every "id":"..." in the document. Good enough for a
		// membership test and avoids parsing a 2 MB document.
		std::set<std::string> ids;
		for (size_t p = body.find("\"id\":\""); p != std::string::npos; p = body.find("\"id\":\"", p + 1)) {
			size_t s = p + 6, e = body.find('"', s);
			if (e == std::string::npos) break;
			ids.insert(body.substr(s, e - s));
		}

		int checked = 0, missing = 0, hiddenMissing = 0;
		std::string missingList, hiddenList;
		for (const auto& kv : ds.conquerors) {
			const bool selectable = kv.first <= kMaxJewelType;
			for (const TJConqueror& q : kv.second) {
				if (q.trade.empty()) continue;
				const bool present = ids.count(q.trade) != 0;
				if (selectable) {
					checked++;
					if (!present) { missing++; missingList += " " + q.name; }
				} else if (!present) {
					hiddenMissing++;
					hiddenList += " " + q.name + "(type " + std::to_string(kv.first) + ")";
				}
			}
		}
		char buf[512];
		snprintf(buf, sizeof(buf), "%s  %s (%s): %d stat ids, %d selectable conquerors, %d missing",
		         missing == 0 ? "PASS" : "FAIL", realm.label, realm.host,
		         (int)ids.size(), checked, missing);
		line(buf);
		if (missing) { line("      missing:" + missingList); failures++; }
		if (hiddenMissing)
			line("      note: absent but currently hidden by kMaxJewelType=" +
			     std::to_string(kMaxJewelType) + ":" + hiddenList);
	}

	line(failures == 0 ? "\nALL PASS" : "\nFAILURES: " + std::to_string(failures));
	HANDLE h = CreateFileW((exeDir + L"tj_realm_check.txt").c_str(), GENERIC_WRITE, 0,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD wrote = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &wrote, nullptr);
		CloseHandle(h);
	}
	return failures == 0 ? 0 : 1;
}
