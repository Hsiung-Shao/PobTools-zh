#include "timeless_jewel_ui.h"
#include "tool_panel.h"
#include "tool_window.h"

#include "launcher_config.h" // ResolveConfiguredFontPath
#include "http_client.h"
#include "passive_tree_data.h"
#include "passive_tree_update.h"
#include "passive_tree_view.h"
#include "timeless_jewel.h"
#include "error_log.h"
#include "timeless_jewel_abyss.h"
#include "ui_theme.h"
#include "ui_widgets.h"   // design-system widgets (Select, Segmented, Banner, EmptyState...)
#include "ui_icons.h"
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
#include <cfloat>
#include <cmath>
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

// The two halves of JewelZh: the Chinese name (the select's label) and the
// English one (its right-hand note).
std::string JewelZhShort(int t)
{
	const std::string s = JewelZh(t);
	const size_t p = s.find(" (");
	return p == std::string::npos ? s : s.substr(0, p);
}
std::string JewelEn(int t)
{
	const std::string s = JewelZh(t);
	const size_t p = s.find(" (");
	if (p == std::string::npos || s.size() < p + 3) return std::string();
	return s.substr(p + 2, s.size() - p - 3);
}

// A filled five-point star (the "this node matched" mark): a pentagon plus five
// tips, all convex, so ImDrawList can fill them.
void DrawStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
	ImVec2 outer[5], inner[5];
	const float ri = r * 0.40f;
	for (int i = 0; i < 5; i++) {
		const float a = -1.5707963f + i * 1.2566371f;           // 72 degrees
		const float b = a + 0.6283185f;                          // +36
		outer[i] = ImVec2(c.x + std::cos(a) * r, c.y + std::sin(a) * r);
		inner[i] = ImVec2(c.x + std::cos(b) * ri, c.y + std::sin(b) * ri);
	}
	dl->AddConvexPolyFilled(inner, 5, col);
	for (int i = 0; i < 5; i++) dl->AddTriangleFilled(outer[i], inner[i], inner[(i + 4) % 5], col);
}

// Cut a hint-face string to fit `maxW`, on a UTF-8 boundary, with an ellipsis.
std::string Ellipsize(const std::string& s, float maxW)
{
	const PobUi::WidgetFonts& wf = PobUi::Fonts();
	ImFont* f = wf.small ? wf.small : ImGui::GetFont();
	auto width = [&](const std::string& t) { return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, t.c_str()).x; };
	if (width(s) <= maxW) return s;
	const std::string ell = u8"…";
	std::string out = s;
	while (!out.empty()) {
		size_t i = out.size() - 1;
		while (i > 0 && ((unsigned char)out[i] & 0xC0) == 0x80) i--;
		out.erase(i);
		if (width(out + ell) <= maxW) return out + ell;
	}
	return ell;
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
	return PobUi::TokV4(PobUi::Tok::Text);   // no keyword: plain text (the hues above are data)
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
	// exercise both paths: some nodes matched (gold ring), some selected (lit)
	PassiveTreeInput tin;
	if (!ptData.sockets.empty()) {
		tin.selectedSocket = ptData.sockets[ptData.sockets.size() / 2];
		std::vector<int> inr = ptData.NodesInRadius(tin.selectedSocket, 1800.0f);
		for (size_t k = 0; k < inr.size(); k++) {
			if (k % 3 == 0) ptHi[inr[k]] = kPtHiAffected;  // matched -> gold ring
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

namespace Tok = PobUi::Tok;

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
			// The panel message is deliberately short; `derr` says which file and
			// what was wrong with it, and it exists nowhere else after this line.
			PobLog::Error("data", "timeless_jewels.json: " + derr);
			initErr_ = u8"無法載入 timeless_jewels.json（資料檔遺失）。";
			return false;
		}

		ptDataOk = ptData.Load(exeDir, &ptErr);
		ptTexOk = ptDataOk && ptView.LoadTextures(exeDir, ptData, &ptErr);

		// Test aid (POBTOOLS_TJ_STATE, with POBTOOLS_TOOL_SHOT): a known state for a
		// hidden-window screenshot. Either variable makes this a test run, which
		// must not touch the network or write anything (tj_ui.json, the tree
		// updater's check stamp).
		{
			wchar_t st[32] = L"";
			const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_TJ_STATE", st, 32);
			if (n > 0 && n < 32) for (const wchar_t* c = st; *c; c++) testState_ += (char)(*c < 128 ? *c : '?');
			testMode_ = !testState_.empty() || GetEnvironmentVariableW(L"POBTOOLS_TOOL_SHOT", nullptr, 0) > 0;
		}

		ptUpdater.Init(exeDir);
		if (!testMode_) ptUpdater.RequestCheck(false);   // throttled to once per day
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
		applyTestState();
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

		// fetch the trade leagues once on open so the export defaults to the
		// current league instead of Standard (start() is a no-op once done).
		// Not under a test aid: that run must neither touch the network nor write
		// tj_ui.json.
		if (!testMode_) {
			leagues.start();
			if (leagues.done.load() && !leagueUserSet) {
				std::vector<std::string> lg = leagues.ForPlatform(tradePlatform);
				if (!lg.empty() && tradeLeague != lg[0]) { tradeLeague = lg[0]; saveTjUi(); }
			}
		}

		// jewel changed (select or paste): the affix pool is jewel-specific
		if (jewelType != templatesFor) {
			templates = TJStatTemplates(*ds, jewelType);
			templatesFor = jewelType;
			wants.clear();          // previously-picked stats may not exist on this jewel
		}

		// conqueror (affects keystones + trade export)
		const std::vector<TJConqueror>* conqs = conqListFor(jewelType);
		const int conqN = conqs ? (int)conqs->size() : 0;
		if (conquerorSel >= conqN) conquerorSel = 0;
		conquerorId_ = conqN ? (*conqs)[conquerorSel].id : std::string("1");
		tradeStatId_ = conqN ? (*conqs)[conquerorSel].trade : std::string();

		// A finished search reports once, as a toast; the table says the rest.
		if (job.done.load() && !job.running.load() && !jobReported_) {
			jobReported_ = true;
			if (job.results.empty())
				PobUi::ShowToast(u8"沒有符合條件的種子", PobUi::Tone::Warn);
			else
				PobUi::ShowToast((u8"找到 " + std::to_string(job.results.size()) + u8" 個種子").c_str(),
				                 PobUi::Tone::Ok);
			if (autoPick_ && !job.results.empty()) detailSeed = job.results.front().seed;
			autoPick_ = false;
		}

		ImGui::PushID("tjhdr");
		drawHeader(ptUst, conqs, conqN);
		ImGui::PopID();

		ImGui::PushID("tjbanners");
		drawBanners(ptUst);
		ImGui::PopID();

		// Three columns: the form (left), the tree (middle), results + the chosen
		// seed (right). Proportions follow the design (380 | * | 400 of 1440).
		const float availW = ImGui::GetContentRegionAvail().x;
		const float leftW = std::floor(std::clamp(availW * 0.264f, PobUi::D(260.0f), PobUi::D(380.0f)));
		const float rightW = std::floor(std::clamp(availW * 0.278f, PobUi::D(280.0f), PobUi::D(400.0f)));
		float midW = availW - leftW - rightW;
		if (midW < PobUi::D(200.0f)) midW = PobUi::D(200.0f);

		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, ImGui::GetStyle().ItemSpacing.y));
		const ImVec2 colTop = ImGui::GetCursorScreenPos();
		const float colH = ImGui::GetContentRegionAvail().y;

		ImGui::PushID("tjleft");
		beginSidePanel("##left", leftW);
		if (mode == 0) drawSearchForm();
		else drawSeedForm();
		endSidePanel();
		ImGui::PopID();

		ImGui::SameLine(0.0f, 0.0f);
		ImGui::PushID("tjmid");
		ImGui::BeginChild("##mid", ImVec2(midW, 0), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		drawTree();
		ImGui::EndChild();
		ImGui::PopID();

		ImGui::SameLine(0.0f, 0.0f);
		ImGui::PushID("tjright");
		beginSidePanel("##right", 0.0f);
		drawRight();
		endSidePanel();
		ImGui::PopID();
		ImGui::PopStyleVar();

		// the column rules (border token), over the children's edges
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddLine(ImVec2(colTop.x + leftW - 0.5f, colTop.y), ImVec2(colTop.x + leftW - 0.5f, colTop.y + colH),
		            Tok::Border, 1.0f);
		dl->AddLine(ImVec2(colTop.x + leftW + midW + 0.5f, colTop.y),
		            ImVec2(colTop.x + leftW + midW + 0.5f, colTop.y + colH), Tok::Border, 1.0f);
	}

	// ---- header ------------------------------------------------------------
	// One row (ToolHeader): icon + name | paste | jewel | conqueror | mode ...
	// ... tree version + Chinese coverage (or the tree update) | more menu.
	void drawHeader(const PassiveTreeUpdater::Status& ust, const std::vector<TJConqueror>* conqs, int conqN)
	{
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		const float H = PobUi::ControlH();
		const float gap = PobUi::D(12.0f);
		const float smH = std::floor(PobUi::D(28.0f));
		const ImVec2 hp = ImGui::GetCursorScreenPos();
		const float avail = ImGui::GetContentRegionAvail().x;
		ImDrawList* hdl = ImGui::GetWindowDrawList();
		auto at = [&](float x, float h) { ImGui::SetCursorScreenPos(ImVec2(x, hp.y + std::floor((H - h) * 0.5f))); };

		// what goes in each slot, measured before anything is drawn
		std::vector<std::string> jewelLbl, jewelNote;
		for (int t = 1; t <= kMaxJewelType; t++) { jewelLbl.push_back(JewelZhShort(t)); jewelNote.push_back(JewelEn(t)); }
		std::vector<const char*> jewelP, jewelN;
		for (size_t i = 0; i < jewelLbl.size(); i++) { jewelP.push_back(jewelLbl[i].c_str()); jewelN.push_back(jewelNote[i].c_str()); }
		std::vector<std::string> conqLbl, conqNote;
		for (int i = 0; i < conqN; i++) {
			const TJConqueror& c = (*conqs)[i];
			conqLbl.push_back(c.nameZh.empty() ? c.name : c.nameZh);
			std::string note = c.nameZh.empty() ? std::string() : c.name;
			if (c.id.find("_v2") != std::string::npos) note += u8"（舊版）";
			conqNote.push_back(note);
		}
		std::vector<const char*> conqP, conqNP;
		for (int i = 0; i < conqN; i++) { conqP.push_back(conqLbl[i].c_str()); conqNP.push_back(conqNote[i].c_str()); }
		const char* modeLabels[2] = { u8"找種子", u8"查已知種子" };
		const char* pasteLbl = u8"從遊戲貼上珠寶";

		const float iconPx = std::floor(PobUi::D(20.0f));
		const char* title = u8"軍團珠寶";
		const float headingW = wf.heading ? wf.heading->CalcTextSizeA(
			wf.headingPx > 0 ? wf.headingPx : wf.heading->FontSize, FLT_MAX, 0.0f, title).x
			: ImGui::CalcTextSize(title).x;
		const float jewelW = std::floor(PobUi::D(170.0f));
		const float conqW = std::floor(PobUi::D(190.0f));
		const float pasteW = PobUi::ButtonWidth(pasteLbl, PobUi::BtnSize::Sm, PobIcon::Copy);
		const float leftW = PobUi::IconWidth(PobIcon::Gem, iconPx) + (wf.icons ? PobUi::D(8.0f) : 0.0f) + headingW +
		                    gap + pasteW + gap + jewelW + gap + conqW + gap + PobUi::SegmentedWidth(modeLabels, 2);

		// right: the tree's version and coverage, or the tree update in progress
		const bool updBusy = ust.phase == PassiveUpdatePhase::Downloading ||
		                     ust.phase == PassiveUpdatePhase::Importing ||
		                     ust.phase == PassiveUpdatePhase::Checking;
		std::string verText, updLbl;
		if (ptDataOk) {
			verText = u8"天賦樹 " + ptData.TreeVersion();
			if (zhPct >= 0) verText += u8" · 繁中 " + std::to_string(zhPct) + "%";
		}
		const ImFont* smallF = wf.small ? wf.small : ImGui::GetFont();
		auto smallW = [&](const std::string& s) {
			return s.empty() ? 0.0f : ((ImFont*)smallF)->CalcTextSizeA(smallF->FontSize, FLT_MAX, 0.0f, s.c_str()).x;
		};
		float rightInfoW = 0.0f;
		if (ust.phase == PassiveUpdatePhase::UpdateAvailable) {
			updLbl = u8"更新天賦樹 " + ust.latestVer;
			rightInfoW = PobUi::ButtonWidth(updLbl.c_str(), PobUi::BtnSize::Sm, PobIcon::Download);
		} else if (updBusy) {
			rightInfoW = (std::min)(smallW(ust.message), PobUi::D(260.0f));
		} else {
			rightInfoW = smallW(verText);
		}
		const float moreW = PobUi::ButtonWidth("##more", PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, smH);
		// too narrow: the version text moves into the more menu (an update offer
		// stays -- it is an action, not information)
		verInMenu_ = ust.phase != PassiveUpdatePhase::UpdateAvailable &&
		             leftW + gap + rightInfoW + PobUi::D(8.0f) + moreW > avail;
		const float rightW = (verInMenu_ ? 0.0f : rightInfoW + PobUi::D(8.0f)) + moreW;

		float x = hp.x;
		PobUi::IconAt(hdl, ImVec2(x, hp.y + std::floor((H - iconPx) * 0.5f)), PobIcon::Gem, Tok::AccentText, iconPx);
		x += PobUi::IconWidth(PobIcon::Gem, iconPx) + (wf.icons ? PobUi::D(8.0f) : 0.0f);
		at(x, ImGui::GetTextLineHeight());
		PobUi::Heading(title);
		x = ImGui::GetItemRectMax().x + gap;

		// paste from the game (auto-fills jewel / conqueror / seed)
		at(x, smH);
		if (PobUi::Button(pasteLbl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::Copy))
			applyPaste(ReadClipboardUtf8(nullptr));
		if (ImGui::IsItemHovered())
			PobUi::Tooltip(u8"在遊戲中對珠寶按 Ctrl+C，再按這裡：自動帶入珠寶、征服者與種子，並切到「查已知種子」。");
		x += pasteW + gap;

		// jewel
		at(x, H);
		int jsel = jewelType - 1;
		if (PobUi::Select("##jewel", &jsel, jewelP.data(), jewelN.data(), (int)jewelP.size(), jewelW) &&
		    jsel + 1 != jewelType) {
			jewelType = jsel + 1;
			conquerorSel = 0;
			ensureBin(jewelType);
		}
		x += jewelW + gap;

		// conqueror (keystones + trade export)
		at(x, H);
		int csel = conquerorSel;
		if (PobUi::Select("##conq", &csel, conqP.data(), conqNP.data(), conqN, conqW, conqN > 0))
			conquerorSel = csel;
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"征服者決定關鍵天賦會變成哪一個，也是交易站搜尋用的詞綴");
		x += conqW + gap;

		// mode. Bound straight to `mode`, so setting it elsewhere (a paste) sticks:
		// the old tab bar wrote its own selection back every frame.
		at(x, H);
		PobUi::Segmented("##mode", &mode, modeLabels, 2);
		x += PobUi::SegmentedWidth(modeLabels, 2) + gap;

		// right-hand side, laid out from the right edge
		float rx = hp.x + avail - rightW;
		if (rx < x) rx = x;
		if (!verInMenu_) {
			if (ust.phase == PassiveUpdatePhase::UpdateAvailable) {
				at(rx, smH);
				if (PobUi::Button(updLbl.c_str(), PobUi::BtnKind::Update, PobUi::BtnSize::Sm, PobIcon::Download))
					ptUpdater.StartUpdate();
				if (ImGui::IsItemHovered()) PobUi::Tooltip(ust.message.c_str());
			} else if (updBusy) {
				at(rx, smallF->FontSize);
				PobUi::Numeric(ust.message.c_str());
			} else if (!verText.empty()) {
				at(rx, smallF->FontSize);
				const bool low = zhPct >= 0 && zhPct < 90;
				PobUi::Numeric(verText.c_str(), low ? Tok::Warning : 0);
				if (ImGui::IsItemHovered() && low)
					PobUi::Tooltip(u8"新賽季詞條在翻譯字典更新前顯示英文；更新翻譯包後重新匯入即可補齊");
			}
			rx += rightInfoW + PobUi::D(8.0f);
		}
		at(rx, smH);
		if (PobUi::Button("##more", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::MoreHorizontal, smH))
			openMore_ = true;
		moreAnchor_ = ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y + PobUi::D(4.0f));
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"更多：交易站區域、聯盟、平台、天賦樹更新");

		// the row, then the header's bottom rule
		ImGui::SetCursorScreenPos(hp);
		ImGui::Dummy(ImVec2(avail, H));
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
		{
			const ImVec2 lp = ImGui::GetCursorScreenPos();
			hdl->AddLine(ImVec2(lp.x, lp.y), ImVec2(lp.x + avail, lp.y), Tok::Border, 1.0f);
			ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
		}

		drawMoreMenu(ust, updBusy, verText);
	}

	// The more menu: trade export settings and the tree update. Opened and begun
	// here, at the panel's top level -- never from inside a child window.
	void drawMoreMenu(const PassiveTreeUpdater::Status& ust, bool updBusy, const std::string& verText)
	{
		if (openMore_) {
			ImGui::OpenPopup("##tjmore");
			openMore_ = false;
		}
		ImGui::SetNextWindowPos(moreAnchor_, ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
		if (!PobUi::BeginMenuPopup("##tjmore")) return;
		const float padX = PobUi::D(10.0f);
		auto section = [&](const char* t) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
			PobUi::Overline(t);
		};

		section(u8"交易站區域");
		for (int r = 0; r < kTradeRealmCount; r++) {
			if (PobUi::MenuRow(tradeRealm == r ? PobIcon::Check : nullptr, kTradeRealms[r].label) && tradeRealm != r) {
				tradeRealm = r;
				// League names are region-specific, so the current pick and the
				// cached list are both meaningless now: refetch and re-default.
				if (!kTradeRealms[r].consoles) tradePlatform = 0;
				leagues.SetHost(kTradeRealms[r].hostW);
				leagueUserSet = false;
				saveTjUi();
			}
		}
		PobUi::MenuSeparator();

		section(u8"聯盟");
		{
			const float w = std::floor(PobUi::D(240.0f));
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
			std::vector<std::string> lgList = leagues.ForPlatform(tradePlatform);
			if (!lgList.empty()) {
				std::vector<const char*> p;
				int sel = -1;
				for (size_t i = 0; i < lgList.size(); i++) {
					p.push_back(lgList[i].c_str());
					if (lgList[i] == tradeLeague) sel = (int)i;
				}
				if (PobUi::Select("##league", &sel, p.data(), nullptr, (int)p.size(), w) && sel >= 0) {
					tradeLeague = lgList[sel];
					leagueUserSet = true;
					saveTjUi();
				}
			} else {
				PobUi::PushControlFrame();
				ImGui::SetNextItemWidth(w);
				if (ImGui::InputText("##league", &tradeLeague)) leagueUserSet = true;
				if (ImGui::IsItemDeactivatedAfterEdit()) saveTjUi();
				PobUi::PopControlFrame();
			}
			ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		}
		if (PobUi::MenuRow(PobIcon::Refresh, leagues.running.load() ? u8"聯盟清單取得中…" : u8"重新取得聯盟清單",
		                   nullptr, !leagues.running.load() && !testMode_)) {
			leagues.done = false;
			leagues.start();
		}
		// Consoles are an international-realm concept; the .tw site has none.
		if (kTradeRealms[tradeRealm].consoles) {
			PobUi::MenuSeparator();
			section(u8"平台");
			const char* plats[3] = { "PC", "Xbox", "PlayStation" };
			for (int i = 0; i < 3; i++) {
				// Only remember the choice. Deliberately NOT clearing leagueUserSet:
				// switching platform never re-defaulted the league before.
				if (PobUi::MenuRow(tradePlatform == i ? PobIcon::Check : nullptr, plats[i]) && tradePlatform != i) {
					tradePlatform = i;
					saveTjUi();
				}
			}
		}
		PobUi::MenuSeparator();
		if (PobUi::MenuRow(PobIcon::Refresh, u8"檢查天賦樹更新", nullptr, !updBusy && !testMode_))
			ptUpdater.RequestCheck(true);
		if (verInMenu_ && !verText.empty()) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
			PobUi::Numeric(verText.c_str());
		}
		(void)ust;
		PobUi::EndMenuPopup();
	}

	// Banners under the header: a failed tree update, a failed paste or copy.
	void drawBanners(const PassiveTreeUpdater::Status& ust)
	{
		bool any = false;
		if (ust.phase == PassiveUpdatePhase::Error && ust.message != ptErrDismissed_) {
			const PobUi::BannerResult r = PobUi::Banner("##pterr", PobUi::BannerTone::Bad, PobIcon::CircleX,
			                                            u8"天賦樹更新失敗", ust.message.c_str(), false, u8"重試", true);
			if (r == PobUi::BannerResult::Action) ptUpdater.StartUpdate();
			if (r == PobUi::BannerResult::Close) ptErrDismissed_ = ust.message;
			any = true;
		}
		if (!errTitle_.empty()) {
			if (any) ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			const bool warn = errWarn_;
			if (PobUi::Banner("##tjerr", warn ? PobUi::BannerTone::Warn : PobUi::BannerTone::Bad,
			                  warn ? PobIcon::TriangleAlert : PobIcon::CircleX, errTitle_.c_str(), errDesc_.c_str(),
			                  false, nullptr, true) == PobUi::BannerResult::Close) {
				errTitle_.clear();
				errDesc_.clear();
			}
			any = true;
		}
		if (any) ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	}

	void showError(const std::string& title, const std::string& desc, bool warn)
	{
		errTitle_ = title;
		errDesc_ = desc;
		errWarn_ = warn;
	}

	// ---- side panels -------------------------------------------------------
	void beginSidePanel(const char* id, float width)
	{
		ImGui::PushStyleColor(ImGuiCol_ChildBg, PobUi::TokV4(Tok::Surface1));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(16.0f), PobUi::D(16.0f)));
		ImGui::BeginChild(id, ImVec2(width, 0), false,
		                  ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar |
		                      ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
	}
	void endSidePanel() { ImGui::EndChild(); }

	float smallLineH() const
	{
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		return wf.small ? wf.small->FontSize : ImGui::GetTextLineHeight();
	}

	// Label (hint) on the left, a control on the right, both centred on `ctrlH`.
	template <class F>
	void labelRow(const char* label, const char* tip, float ctrlW, float ctrlH, F&& ctrl)
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((ctrlH - smallLineH()) * 0.5f)));
		PobUi::Hint(label);
		if (tip) {
			ImGui::SameLine(0.0f, PobUi::D(6.0f));
			PobUi::InfoTip(tip);
		}
		ImGui::SetCursorScreenPos(ImVec2(p.x + w - ctrlW, p.y));
		ctrl();
		ImGui::SetCursorScreenPos(p);
		ImGui::Dummy(ImVec2(w, ctrlH));
	}

	// The standing caveat for the Abyss jewels: new in 3.29, far less community
	// data behind them than behind the Legion ones. Said where the choice is
	// made, not buried in a tooltip.
	void drawAbyssBanner()
	{
		if (!TJIsAbyss(jewelType)) return;
		const char* desc = TJIsZorath(jewelType)
			? u8"目前可對照的實測樣本仍少，請以遊戲內實際結果為準。重奪惡意另外取決於你從插槽走到職業起點的已配點路徑，"
			  u8"本工具沒有角色資料：請隨意點一個插槽，用它周邊的天賦判斷這顆種子。單一天賦會變成什麼、昇華天賦的選擇，這兩項是準確的。"
			: u8"目前可對照的實測樣本仍少，結果無法像軍團珠寶那樣完整驗證，請以遊戲內實際結果為準。";
		PobUi::Banner("##abyss", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, u8"深淵珠寶是 3.29 新增", desc,
		              false, nullptr, false);
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
	}

	// One line under the form saying which socket the jewel sits in -- or, in
	// warning colour, that one has to be picked first and why.
	std::string socketHint(bool* missing) const
	{
		*missing = selSocket < 0;
		if (selSocket < 0) {
			// Every jewel needs a socket, for three different reasons: a Legion one
			// to know which radius to walk, types 7-10 because the socket IS the key
			// their file is indexed by, and Zorath because it is the place the user
			// is asking us to look around.
			return TJIsZorath(jewelType) ? u8"請先在天賦樹上點一個珠寶插槽（隨意一個即可，用來決定要看哪一帶的天賦）"
			       : TJIsAbyss(jewelType) ? u8"請先在天賦樹上點一個珠寶插槽（深淵珠寶的結果由插槽決定，沒有半徑）"
			                              : u8"請先在天賦樹上點一個珠寶插槽（搜尋只計算該插槽半徑內的節點）";
		}
		const PtNode& sn = ptData.nodes[selSocket];
		std::string s = u8"插槽：" + (sn.nameZh.empty() ? sn.name : sn.nameZh);
		int picked = 0;
		for (char c : ptSelected) if (c) picked++;
		if (picked > 0) s += u8"（只看已選的 " + std::to_string(picked) + u8" 個節點）";
		else s += u8"（已在樹上選取）";
		return s;
	}

	// ---- left: find seeds --------------------------------------------------
	void drawSearchForm()
	{
		const bool busy = job.running.load();
		bool noSocket = false;
		const std::string hint = socketHint(&noSocket);
		const float w = ImGui::GetContentRegionAvail().x;
		// the bottom block (socket hint + the search button) is pinned; measure it
		const ImFont* sf = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
		const float hintH = ((ImFont*)sf)->CalcTextSizeA(sf->FontSize, FLT_MAX, w, hint.c_str()).y;
		const float bottomH = hintH + PobUi::D(6.0f) + std::floor(PobUi::D(44.0f));
		const float scrollH = ImGui::GetContentRegionAvail().y - bottomH - PobUi::D(12.0f);

		ImGui::BeginChild("##formscroll", ImVec2(0, (std::max)(scrollH, PobUi::D(80.0f))), false);
		drawAbyssBanner();
		PobUi::Heading(u8"想要的詞綴");
		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));

		// add a stat: type to filter, pick from the list
		const float fw = ImGui::GetContentRegionAvail().x;
		PobUi::SearchField("##statfilter", statFilter_, (int)sizeof(statFilter_), u8"輸入關鍵字加入詞綴…", fw);
		const std::string filter = statFilter_;
		if (!filter.empty() || browseAll_) {
			const float rowH = ImGui::GetTextLineHeightWithSpacing();
			int matches = 0;
			for (const auto& t : templates) {
				const std::string& disp = t.zh.empty() ? t.en : t.zh;
				if (contains_ci(disp, filter) || contains_ci(t.en, filter)) matches++;
			}
			const float listH = (std::min)(PobUi::D(200.0f), (std::max)(1, (std::min)(matches, 300)) * rowH + PobUi::D(12.0f));
			ImGui::PushStyleColor(ImGuiCol_ChildBg, PobUi::TokV4(Tok::Surface2));
			ImGui::PushStyleColor(ImGuiCol_Border, PobUi::TokV4(Tok::Border));
			ImGui::BeginChild("##statpick", ImVec2(0, listH), true);
			if (matches == 0) PobUi::Hint((u8"沒有符合「" + filter + u8"」的詞綴").c_str());
			int shown = 0;
			for (const auto& t : templates) {
				const std::string& disp = t.zh.empty() ? t.en : t.zh;
				if (!contains_ci(disp, filter) && !contains_ci(t.en, filter)) continue;
				if (++shown > 300) break;
				bool exists = false;
				for (auto& wr : wants) if (wr.en == t.en) exists = true;
				ImGui::PushID(t.en.c_str());
				ImGui::BeginDisabled(exists);
				if (ImGui::Selectable(disp.c_str(), exists)) {
					wants.push_back({ t.en, disp, 0.0f, 1.0f });
					statFilter_[0] = '\0';   // added: clear the box for the next one
				}
				ImGui::EndDisabled();
				ImGui::PopID();
			}
			ImGui::EndChild();
			ImGui::PopStyleColor(2);
		}
		if (filter.empty()) {
			const std::string lbl = browseAll_ ? std::string(u8"收起詞綴清單")
			                                   : u8"瀏覽全部詞綴（" + std::to_string(templates.size()) + u8"）";
			if (PobUi::Link(lbl.c_str())) browseAll_ = !browseAll_;
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));

		// the picked stats: 詞綴 / 最小值 / 權重 / 移除
		if (wants.empty()) {
			PobUi::Hint(u8"還沒有詞綴：在上方輸入關鍵字，從清單點選加入。", ImGui::GetContentRegionAvail().x);
		} else {
			pushTableStyle();
			if (ImGui::BeginTable("##wants", 4, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
				const float numW = std::floor(PobUi::D(52.0f));
				ImGui::TableSetupColumn(u8"詞綴", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn(u8"最小值", ImGuiTableColumnFlags_WidthFixed, numW);
				ImGui::TableSetupColumn(u8"權重", ImGuiTableColumnFlags_WidthFixed, numW);
				ImGui::TableSetupColumn(u8"移除", ImGuiTableColumnFlags_WidthFixed, std::floor(PobUi::D(30.0f)));
				headerRow(4, nullptr);
				int removeAt = -1;
				for (int i = 0; i < (int)wants.size(); i++) {
					ImGui::PushID(i);
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::AlignTextToFramePadding();
					ImGui::PushTextWrapPos(0.0f);
					ImGui::TextUnformatted(wants[i].zh.c_str());
					ImGui::PopTextWrapPos();
					ImGui::TableNextColumn();
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::InputFloat("##min", &wants[i].minValue, 0, 0, "%.0f");
					if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"這條詞綴的數值低於最小值時不算命中");
					ImGui::TableNextColumn();
					ImGui::SetNextItemWidth(-FLT_MIN);
					ImGui::InputFloat("##w", &wants[i].weight, 0, 0, "%.1f");
					if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"每次命中加進種子總分的權重");
					ImGui::TableNextColumn();
					if (PobUi::Button("##rm", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::X,
					                  std::floor(PobUi::D(28.0f))))
						removeAt = i;
					if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"移除這條詞綴");
					ImGui::PopID();
				}
				ImGui::EndTable();
				if (removeAt >= 0) wants.erase(wants.begin() + removeAt);
			}
			popTableStyle();
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(8.0f)));

		const float H = PobUi::ControlH();
		// 最小總權重: kept (it filters the ranking), as a row like the other two
		labelRow(u8"最小總權重", u8"總分低於這個值的種子不列出（0 = 不限）", std::floor(PobUi::D(90.0f)), H, [&]() {
			PobUi::PushControlFrame();
			ImGui::SetNextItemWidth(std::floor(PobUi::D(90.0f)));
			ImGui::InputFloat("##mintotal", &minTotalWeight, 0, 0, "%.1f");
			PobUi::PopControlFrame();
		});
		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		// Abyss jewels conquer keystones too, and TJAbyssInScope counts them as
		// "big" -- the label has to say so or the filter looks like it is dropping
		// results.
		const char* scopeLbl[2] = { TJIsAbyss(jewelType) ? u8"大天賦與鑰石" : u8"只看大點", u8"全部節點" };
		int scopeSel = scope == 1 ? 0 : 1;
		labelRow(u8"範圍", nullptr, PobUi::SegmentedWidth(scopeLbl, 2), H, [&]() {
			if (PobUi::Segmented("##scope", &scopeSel, scopeLbl, 2)) scope = scopeSel == 0 ? 1 : 0;
		});
		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		labelRow(u8"必須包含全部詞綴",
		         u8"開啟時，只滿足其中一條的種子不會出現。關閉後只要命中任一條就算，權重高的仍排前面。",
		         PobUi::SwitchWidth(), H, [&]() { PobUi::Switch("##requireall", &requireAll); });
		ImGui::EndChild(); // ##formscroll

		// pinned: where the jewel sits + the one primary action
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x,
		                                 ImGui::GetWindowPos().y + ImGui::GetWindowHeight() -
		                                     ImGui::GetStyle().WindowPadding.y - bottomH));
		PobUi::Hint(hint.c_str(), w, noSocket ? Tok::Warning : 0);
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f) - ImGui::GetStyle().ItemSpacing.y));
		const bool canSearch = !busy && !wants.empty() && selSocket >= 0;
		if (PobUi::Button(busy ? u8"搜尋中…" : u8"搜尋這個插槽", PobUi::BtnKind::Primary, PobUi::BtnSize::Lg, nullptr, w,
		                  canSearch))
			runSearch();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canSearch && !busy)
			PobUi::Tooltip(wants.empty() ? u8"先加入至少一條想要的詞綴" : u8"先在天賦樹上點一個珠寶插槽");
	}

	void runSearch()
	{
		if (job.running.load() || wants.empty() || selSocket < 0) return;
		if (!ensureBin(jewelType)) {
			showError(u8"讀不到這顆珠寶的查表資料", binErr, false);
			return;
		}
		const bool isAbyss = TJIsAbyss(jewelType);
		const bool isZorath = TJIsZorath(jewelType);
		TJSearchQuery q;
		q.jewelType = jewelType;
		q.scope = scope;
		q.minTotalWeight = minTotalWeight;
		q.requireAll = requireAll;
		for (auto& w : wants) q.wants.push_back({ w.en, w.minValue, w.weight });
		detailSeed = -1;
		// Types 7-10 need no node list: their file names the conquered passives.
		// Everything else is judged on the socket's neighbourhood, narrowed to the
		// user's picks when there are any -- for Zorath that neighbourhood stands
		// in for a path we cannot know, so letting the user pick the nodes IS the
		// way to aim it at their own build.
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
		jobReported_ = false;
		if (isAbyss)
			job.startAbyss(ds.get(), blob, *abyssLut, q, ptData.nodes[selSocket].id, ptKind);
		else
			job.start(ds.get(), blob, q);
	}

	// ---- left: look up a known seed ----------------------------------------
	void drawSeedForm()
	{
		drawAbyssBanner();
		PobUi::Heading(u8"查已知種子");
		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		const float w = ImGui::GetContentRegionAvail().x;
		PobUi::Hint(u8"輸入珠寶上的種子數字；或在遊戲中對珠寶按 Ctrl+C，再按上方「從遊戲貼上珠寶」。", w);
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
		const char* qLbl = u8"查詢";
		const float bw = PobUi::ButtonWidth(qLbl, PobUi::BtnSize::Md, PobIcon::Search);
		PobUi::PushControlFrame();
		ImGui::SetNextItemWidth(w - bw - PobUi::D(8.0f));
		const bool enter = ImGui::InputTextWithHint("##seed", u8"種子", &seedText,
		                                            ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_EnterReturnsTrue);
		PobUi::PopControlFrame();
		ImGui::SameLine(0.0f, PobUi::D(8.0f));
		if (PobUi::Button(qLbl, PobUi::BtnKind::Primary, PobUi::BtnSize::Md, PobIcon::Search) || enter) querySeed();

		// The page's own errors, where the page is. These used to be written to a
		// status line that only the search page ever drew.
		if (!seedErr_.empty()) {
			ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			if (PobUi::Banner("##seederr", PobUi::BannerTone::Bad, PobIcon::CircleX, u8"查詢失敗", seedErr_.c_str(),
			                  false, nullptr, true) == PobUi::BannerResult::Close)
				seedErr_.clear();
		} else if (detailSeed >= 0) {
			ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			PobUi::Hint((u8"右側顯示種子 " + std::to_string(detailSeed) + u8" 的變化").c_str());
		}

		bool noSocket = false;
		const std::string hint = socketHint(&noSocket);
		const ImFont* sf = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
		const float hintH = ((ImFont*)sf)->CalcTextSizeA(sf->FontSize, FLT_MAX, w, hint.c_str()).y;
		const float y = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - hintH;
		if (y > ImGui::GetCursorScreenPos().y) ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y));
		PobUi::Hint(hint.c_str(), w, noSocket ? Tok::Warning : 0);
	}

	void querySeed()
	{
		std::string s;
		for (char c : seedText) if (c != ' ' && c != ',') s += c;
		bool digits = !s.empty() && s.size() <= 9;
		for (char c : s) if (c < '0' || c > '9') digits = false;
		if (!digits) {
			seedErr_ = u8"請輸入珠寶上的種子數字（只能是數字）。";
			return;
		}
		if (!ensureBin(jewelType)) {
			seedErr_ = binErr.empty() ? std::string(u8"讀不到這顆珠寶的查表資料") : binErr;
			detailSeed = -1;
			return;
		}
		seedErr_.clear();
		detailSeed = atoi(s.c_str());
	}

	// Paste from the game: jewel, conqueror and seed in one go, then straight to
	// the seed page. Success is a toast; anything that needs the user is a banner.
	void applyPaste(const std::string& txt)
	{
		TJPaste pasted = TJParsePaste(*ds, txt);
		const int foundJewel = pasted.jewelType;
		if (!foundJewel) {
			showError(u8"剪貼簿裡沒有珠寶", u8"在遊戲中對珠寶按 Ctrl+C 複製，再按「從遊戲貼上珠寶」。", true);
			return;
		}
		if (foundJewel > kMaxJewelType) {
			showError(std::string(JewelZh(foundJewel)) + u8" 尚未支援",
			          u8"它的效果取決於角色從插槽到起點的已配點路徑，本工具沒有角色資料。", true);
			return;
		}
		errTitle_.clear();
		jewelType = foundJewel;
		conquerorSel = pasted.conqIndex >= 0 ? pasted.conqIndex : 0;
		std::string msg = u8"已匯入：" + JewelZhShort(foundJewel);
		if (pasted.seed >= 0) {
			seedText = std::to_string(pasted.seed);
			mode = 1;   // the seed page shows what was just pasted
			querySeed();
			msg += u8"　種子 " + std::to_string(pasted.seed);
		} else {
			ensureBin(jewelType);
		}
		PobUi::ShowToast(msg.c_str(), PobUi::Tone::Ok);
		hiSig = -1;
	}

	void copyForPob(int seed)
	{
		std::string t = TJItemText(*ds, jewelType, conquerorSel, seed);
		if (!t.empty() && WriteClipboardUtf8(nullptr, t))
			PobUi::ShowToast(u8"已複製物品文字，可在 POB 的物品欄貼上 (Ctrl+V)", PobUi::Tone::Ok);
		else
			showError(u8"複製失敗", u8"無法寫入剪貼簿，請再試一次。", false);
	}

	bool tradeOff() const { return tradeStatId_.empty() || tradeLeague.empty(); }
	const char* tradeOffWhy() const
	{
		return tradeStatId_.empty() ? u8"這個征服者沒有交易站詞綴" : u8"先在「⋯」選單設定聯盟";
	}

	// ---- middle: the tree --------------------------------------------------
	void drawTree()
	{
		if (!ptTexOk) {
			ImGui::Dummy(ImVec2(0, PobUi::D(12.0f)));
			ImGui::Indent(PobUi::D(12.0f));
			PobUi::Banner("##treeerr", PobUi::BannerTone::Bad, PobIcon::CircleX, u8"天賦樹視圖無法載入", ptErr.c_str(),
			              false, nullptr, false, true, ImGui::GetContentRegionAvail().x - PobUi::D(12.0f));
			ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			PobUi::Hint(u8"計算器其餘功能仍可使用。");
			ImGui::Unindent(PobUi::D(12.0f));
			return;
		}
		const bool treeRadius = JewelUsesRadius(jewelType);
		const bool treeAbyss = TJIsAbyss(jewelType);
		const bool zorath = TJIsZorath(jewelType);

		// status row: what the canvas is showing (left), picking shortcuts (right)
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			const float rowH = std::floor(PobUi::D(40.0f));
			const float smH = std::floor(PobUi::D(28.0f));
			ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + w, p.y + rowH), Tok::Bg);
			ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y + rowH - 0.5f), ImVec2(p.x + w, p.y + rowH - 0.5f),
			                                    Tok::Border, 1.0f);
			const bool canPick = selSocket >= 0 && (treeRadius || zorath);
			const char* pickLbl[4] = { u8"全部", u8"大點", u8"一般", u8"清除" };
			float pickW = 0.0f;
			if (canPick) {
				pickW = smallLineH() * 2.5f;
				for (const char* l : pickLbl) pickW += PobUi::ButtonWidth(l, PobUi::BtnSize::Sm) + PobUi::D(4.0f);
			}
			std::string st;
			if (selSocket < 0) st = u8"在樹上點一個珠寶插槽，選擇珠寶放置的位置";
			else {
				const PtNode& sn = ptData.nodes[selSocket];
				st = u8"插槽：" + (sn.nameZh.empty() ? sn.name : sn.nameZh);
				if (detailSeed < 0) st += u8"｜選一個種子即可預覽轉換";
				// The circle is where we are looking for Zorath, not what the jewel
				// hits; the 7-10 passives come from the jewel's own file and are not
				// in a circle at all. Saying "radius" there would describe the wrong
				// mechanic.
				else if (zorath) st += u8"｜種子 " + std::to_string(detailSeed) + u8"｜圈內為判斷用，實際生效看你的配點";
				else if (treeAbyss) st += u8"｜種子 " + std::to_string(detailSeed) + u8"｜此插槽被征服的天賦以金框標示";
				else st += u8"｜種子 " + std::to_string(detailSeed) + u8"｜半徑內受影響節點以金框標示";
			}
			const float textW = (std::max)(PobUi::D(60.0f), w - pickW - PobUi::D(24.0f));
			ImGui::SetCursorScreenPos(ImVec2(p.x + PobUi::D(12.0f), p.y + std::floor((rowH - smallLineH()) * 0.5f)));
			PobUi::Hint(Ellipsize(st, textW).c_str());
			if (ImGui::IsItemHovered() && Ellipsize(st, textW) != st) PobUi::Tooltip(st.c_str());
			if (canPick) {
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
				float x = p.x + w - PobUi::D(12.0f) - pickW + smallLineH() * 2.5f;
				ImGui::SetCursorScreenPos(ImVec2(x - smallLineH() * 2.5f, p.y + std::floor((rowH - smallLineH()) * 0.5f)));
				PobUi::Hint(u8"選取");
				for (int i = 0; i < 4; i++) {
					ImGui::SetCursorScreenPos(ImVec2(x, p.y + std::floor((rowH - smH) * 0.5f)));
					if (PobUi::Button(pickLbl[i], PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
						if (i == 3) setRange(false, 0);
						else setRange(true, i);
					}
					x = ImGui::GetItemRectMax().x + PobUi::D(4.0f);
				}
			}
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
		}

		// recompute highlight + per-node transforms when the input tuple changes
		// (selVersion covers the picked-node set).
		long long sig = ((long long)selSocket * 1000003 + detailSeed) * 97 +
		                (long long)jewelType * 13 + conquerorSel * 131 + selVersion;
		if (sig != hiSig) {
			hiSig = sig;
			recomputeHighlights(treeRadius);
		}

		// per-frame draw highlight. Default: nothing framed (clean tree). Frame
		// only the nodes that MATCH a searched stat; a clicked stat-row narrows it
		// to that one stat's nodes.
		dispHi.assign(ptData.nodes.size(), kPtHiNone);
		if (hlStatGroup >= 0 && hlStatGroup < (int)statGroups.size()) {
			auto mark = [&](const std::vector<int>& v) {
				for (int idx : v) if (idx < (int)dispHi.size()) dispHi[idx] = ptHi[idx] ? ptHi[idx] : kPtHiAffected;
			};
			mark(statGroups[hlStatGroup].notables);
			mark(statGroups[hlStatGroup].smalls);
		} else if (!wants.empty()) {
			// Frame only nodes that really satisfy the query: a node whose roll sits
			// below 最小值 contributed nothing to this seed's score.
			const TJWantMatcher hiMatcher = makeMatcher();
			for (const auto& kv : ptTrans) {
				for (const auto& ln : kv.second.lines)
					if (hiMatcher.Match(ln)) {
						dispHi[kv.first] = ptHi[kv.first] ? ptHi[kv.first] : kPtHiAffected;
						break;
					}
			}
		} else if (detailSeed >= 0) {
			// nothing searched for (a pasted or typed seed): frame every node the
			// seed changes, which is what the legend and the status row promise
			dispHi = ptHi;
		}

		PassiveTreeInput tin;
		tin.selectedSocket = selSocket;
		// Zorath has no radius either, but this ring marks the area being judged,
		// not an area of effect -- the status row says which. Leaving it invisible
		// would hide what the numbers were computed from.
		tin.radiusWorld = (treeRadius || zorath) ? 1800.0f : 0.0f;
		tin.hi = &dispHi;
		tin.selected = ptSelected.empty() ? nullptr : &ptSelected;
		tin.emphasize = emphNode;
		const bool pickable = treeRadius || zorath;
		tin.overlay = [this, pickable](ImDrawList* dl, ImVec2 mn, ImVec2 mx) { drawLegend(dl, mn, mx, pickable); };
		PassiveTreeOutput tout = ptView.Draw(ptData, scale, tin);

		if (centerPending_ && selSocket >= 0) { ptView.CenterOn(ptData, selSocket); centerPending_ = false; }
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
		if (tout.hoveredNode >= 0) drawNodeTooltip(tout.hoveredNode);
	}

	// Bottom-left of the canvas: what the gold ring and the socket mean.
	void drawLegend(ImDrawList* dl, ImVec2 mn, ImVec2 mx, bool pickable)
	{
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		ImFont* f = wf.small ? wf.small : ImGui::GetFont();
		const float px = f->FontSize;
		const float sw = std::floor(px * 0.7f);
		const float gap = PobUi::D(16.0f), padX = PobUi::D(10.0f), padY = PobUi::D(6.0f);
		const char* hitL = u8"種子改變的節點";
		const char* sockL = u8"珠寶插槽";
		const char* pickL = u8"點一般節點可單獨查看";
		float w = padX * 2.0f + sw + PobUi::D(6.0f) + f->CalcTextSizeA(px, FLT_MAX, 0, hitL).x + gap + sw +
		          PobUi::D(6.0f) + f->CalcTextSizeA(px, FLT_MAX, 0, sockL).x;
		if (pickable) w += gap + f->CalcTextSizeA(px, FLT_MAX, 0, pickL).x;
		const float h = px + padY * 2.0f;
		const ImVec2 p(mn.x + PobUi::D(16.0f), mx.y - PobUi::D(14.0f) - h);
		if (p.x + w > mx.x || p.y < mn.y) return;   // canvas too small: no legend
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), (Tok::Canvas & 0x00FFFFFFu) | 0xCC000000u, PobUi::D(6.0f));
		float x = p.x + padX;
		const float cy = p.y + h * 0.5f;
		dl->AddCircle(ImVec2(x + sw * 0.5f, cy), sw * 0.5f, Tok::TreeHit, 16, 2.0f);
		x += sw + PobUi::D(6.0f);
		dl->AddText(f, px, ImVec2(x, p.y + padY), Tok::TextMuted, hitL);
		x += f->CalcTextSizeA(px, FLT_MAX, 0, hitL).x + gap;
		dl->AddCircleFilled(ImVec2(x + sw * 0.5f, cy), sw * 0.5f, PobUi::TreeKindColor(PobUi::TreeKind::Socket), 16);
		x += sw + PobUi::D(6.0f);
		dl->AddText(f, px, ImVec2(x, p.y + padY), Tok::TextMuted, sockL);
		x += f->CalcTextSizeA(px, FLT_MAX, 0, sockL).x + gap;
		if (pickable) dl->AddText(f, px, ImVec2(x, p.y + padY), Tok::TextMuted, pickL);
	}

	static PobUi::TreeKind kindOf(int ptKindV)
	{
		switch (ptKindV) {
		case kPtKeystone: return PobUi::TreeKind::Keystone;
		case kPtNotable: return PobUi::TreeKind::Notable;
		case kPtSocket: return PobUi::TreeKind::Socket;
		default: return PobUi::TreeKind::Small;
		}
	}

	// tooltip for the hovered node: transformed stats if affected, else base
	void drawNodeTooltip(int nodeIdx)
	{
		const PtNode& n = ptData.nodes[nodeIdx];
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * scale, 12 * scale));
		ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(440 * scale, FLT_MAX));
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(400 * scale);
		const ImVec4 text = PobUi::TokV4(Tok::Text);
		auto it = ptTrans.find(nodeIdx);
		if (it != ptTrans.end() && it->second.ok) {
			const TJTransform& t = it->second;
			const std::string& nm = !t.newNameZh.empty() ? t.newNameZh
			                       : !t.newName.empty() ? t.newName
			                       : (n.nameZh.empty() ? n.name : n.nameZh);
			ImGui::TextColored(PobUi::TokV4(Tok::TreeHit), "%s", nm.c_str());
			PobUi::Hint(t.replaced ? u8"（節點被替換）" : u8"（保留原詞綴，珠寶額外加成）");
			ImGui::Separator();
			// An addition leaves the node itself intact, so its own stats still
			// apply and must be shown above what the jewel adds.
			if (!t.replaced) {
				const std::vector<std::string>& base = n.statsZh.empty() ? n.stats : n.statsZh;
				for (const std::string& s : base) ImGui::TextColored(text, "%s", s.c_str());
			}
			for (size_t i = 0; i < t.lines.size(); i++) {
				const std::string& zh = (i < t.linesZh.size() && !t.linesZh[i].empty()) ? t.linesZh[i] : t.lines[i];
				ImGui::TextColored(PobUi::TokV4(Tok::Success), "%s", zh.c_str());
			}
		} else {
			const std::string& nm = n.nameZh.empty() ? n.name : n.nameZh;
			ImGui::TextColored(PobUi::TokV4(PobUi::TreeKindColor(kindOf(n.kind))), "%s", nm.empty() ? "?" : nm.c_str());
			const std::vector<std::string>& lines = n.statsZh.empty() ? n.stats : n.statsZh;
			if (!lines.empty()) ImGui::Separator();
			for (const std::string& s : lines) ImGui::TextColored(text, "%s", s.c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
		ImGui::PopStyleVar();
	}

	// The tuple (socket, seed, jewel, conqueror, picks) changed: transform every
	// candidate node once and rebuild the stat groups the side list shows.
	void recomputeHighlights(bool treeRadius)
	{
		ptHi.assign(ptData.nodes.size(), kPtHiNone);
		ptTrans.clear();
		statGroups.clear();
		emphNode = -1; hlStatGroup = -1;
		const bool abyssDetail = TJIsAbyss(jewelType);
		if (selSocket < 0 || !(treeRadius || abyssDetail)) return;
		const bool haveBin = detailSeed >= 0 && ensureBin(jewelType);

		// Where the two engines part company. A Legion jewel is given a socket and
		// works out which passives are in reach; an Abyss one is TOLD which
		// passives it took, and they are scattered across the tree rather than
		// sitting inside a circle. So one side computes its candidates and the
		// other reads them.
		std::vector<int> cand;
		std::map<int, TJAbyssMod> abyssMods; // node index -> modification
		const bool zorathDetail = TJIsZorath(jewelType);
		if (abyssDetail && !zorathDetail) {
			std::map<int, TJAbyssMod> byId;
			if (haveBin && TJAbyssReadSocket(*ds, *blob, *abyssLut, ptData.nodes[selSocket].id, detailSeed, byId)) {
				for (const auto& kv : byId) {
					const int idx = ptData.IndexOfId(kv.first);
					if (idx < 0) continue; // conquered node absent from our tree copy
					cand.push_back(idx);
					abyssMods[idx] = kv.second;
				}
			}
		} else {
			// Zorath and the Legion jewels both start from the socket's
			// neighbourhood -- but they mean different things by it. For a Legion
			// jewel that IS the affected set; for Zorath it is where we are
			// looking, because the real set follows a path through the character's
			// own allocation.
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
		auto included = [&](int idx) { return !anySel || (idx < (int)ptSelected.size() && ptSelected[idx]); };
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
				const char* nt = n.kind == kPtKeystone ? "Keystone" : n.kind == kPtNotable ? "Notable" : "Normal";
				t = TJApply(*ds, *blob, jewelType, detailSeed, n.id, nt, n.stats, ConquerorType(jewelType),
				            conquerorId_, n.name);
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
			bool big = ptData.nodes[kv.first].kind == kPtNotable || ptData.nodes[kv.first].kind == kPtKeystone ||
			           kv.second.replaced;
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

	// ---- tables (design system: muted header on surface-1, rows split by
	// border-subtle, selected row accent-soft) --------------------------------
	void pushTableStyle()
	{
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, PobUi::TokV4(Tok::Surface1));
		ImGui::PushStyleColor(ImGuiCol_TableBorderLight, PobUi::TokV4(Tok::BorderSubtle));
		ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, PobUi::TokV4(Tok::Border));
		ImGui::PushStyleColor(ImGuiCol_Header, PobUi::TokV4(Tok::AccentSoft));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, PobUi::TokV4(Tok::Surface2));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, PobUi::TokV4(Tok::AccentSoft));
	}
	void popTableStyle() { ImGui::PopStyleColor(6); }

	// Header labels in the small face, muted; numeric columns right-aligned.
	void headerRow(int cols, const bool* rightAlign)
	{
		ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		ImFont* f = wf.small ? wf.small : ImGui::GetFont();
		for (int c = 0; c < cols; c++) {
			if (!ImGui::TableSetColumnIndex(c)) continue;
			const char* label = ImGui::TableGetColumnName(c);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float cellW = ImGui::GetContentRegionAvail().x;
			ImGui::PushID(c);
			ImGui::TableHeader("##hdr");
			ImGui::PopID();
			if (label && label[0] && label[0] != '#') {
				const float tw = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label).x;
				const float x = (rightAlign && rightAlign[c]) ? p.x + cellW - tw : p.x;
				ImGui::GetWindowDrawList()->AddText(f, f->FontSize, ImVec2(x, p.y), Tok::TextMuted, label);
			}
		}
	}

	static void rightText(const char* s, std::uint32_t col)
	{
		const float w = ImGui::CalcTextSize(s).x;
		const float avail = ImGui::GetContentRegionAvail().x;
		if (avail > w) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - w);
		ImGui::TextColored(PobUi::TokV4(col), "%s", s);
	}

	// ---- right: results, then the chosen seed --------------------------------
	void drawRight()
	{
		const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y;
		// The cross-check reminder. Only after the user actually opened a trade
		// search, so it does not add noise to the normal search flow.
		const bool tradeTip = tradeHintShown && !tradeHintClosed_;
		const float reserve = tradeTip ? infoBannerH_ + PobUi::D(8.0f) : 0.0f;
		if (mode == 0) {
			drawResults(bottom - reserve);
			ImGui::Dummy(ImVec2(0, PobUi::D(8.0f)));
		}
		drawDetail(bottom - reserve);
		if (tradeTip) {
			const float y = bottom - infoBannerH_;
			if (y > ImGui::GetCursorScreenPos().y) ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y));
			// No arrow glyphs here: the CJK font atlas does not carry them and they
			// render as tofu (see error_imgui_font_atlas_missing_glyphs).
			if (PobUi::Banner("##pobcheck", PobUi::BannerTone::Info, PobIcon::Info, u8"買之前先在 POB 確認一次",
			                  u8"把交易站上的珠寶複製進 POB 的物品欄，對照天賦加成與這裡列的詞綴；對不上請回報。",
			                  false, nullptr, true) == PobUi::BannerResult::Close)
				tradeHintClosed_ = true;
			infoBannerH_ = ImGui::GetItemRectSize().y;
		}
	}

	void drawResults(float bottom)
	{
		const float startY = ImGui::GetCursorScreenPos().y;
		const float maxH = std::floor((bottom - startY) * 0.46f);
		if (job.running.load()) {
			PobUi::Heading(u8"搜尋中…");
			PobUi::Hint(u8"正在掃描這顆珠寶的所有種子，通常幾秒內完成。", ImGui::GetContentRegionAvail().x);
			return;
		}
		if (job.results.empty()) {
			if (job.done.load())
				PobUi::EmptyState("##noresult", PobIcon::Search, u8"沒有符合條件的種子",
				                  u8"降低最小值或最小總權重、關掉「必須包含全部詞綴」，或把範圍改成全部節點。");
			else
				PobUi::EmptyState("##noresult", PobIcon::Search, u8"還沒有搜尋結果",
				                  u8"在左側加入想要的詞綴、在樹上選好插槽，再按「搜尋這個插槽」。");
			return;
		}

		// head: count + how it is sorted, then the grouping switch
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			PobUi::Heading((u8"找到 " + std::to_string(job.results.size()) + u8" 個種子").c_str());
			const float lineH = ImGui::GetItemRectSize().y;
			const char* sortHint = u8"依權重排序";
			const ImFont* sf = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
			const float hw = ((ImFont*)sf)->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, sortHint).x;
			ImGui::SetCursorScreenPos(ImVec2(p.x + w - hw, p.y + std::floor((lineH - sf->FontSize) * 0.5f)));
			PobUi::Hint(sortHint);
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lineH));
			ImGui::Dummy(ImVec2(w, PobUi::D(4.0f)));
		}
		labelRow(u8"依命中節點數分組", u8"分組後可以把同一組的種子一次送到交易站搜尋（每次最多 40 個）",
		         PobUi::SwitchWidth(), std::floor(PobUi::D(28.0f)), [&]() { PobUi::Switch("##group", &groupResults); });
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
		const float tableH = (std::max)(PobUi::D(120.0f), maxH - (ImGui::GetCursorScreenPos().y - startY));
		if (groupResults) drawGroupedResults(tableH);
		else drawResultTable(tableH);
	}

	void drawResultTable(float height)
	{
		const float smH = std::floor(PobUi::D(28.0f));
		// one height for every row (the clipper needs it): the Sm buttons + cell padding
		const float rowH = smH + ImGui::GetStyle().CellPadding.y * 2.0f;
		auto centreLine = [&]() {
			ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::floor((smH - ImGui::GetTextLineHeight()) * 0.5f));
		};
		const float actW = PobUi::ButtonWidth(u8"交易", PobUi::BtnSize::Sm) + PobUi::D(4.0f) + smH;
		pushTableStyle();
		if (ImGui::BeginTable("##res", 4, ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH, ImVec2(0, height))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn(u8"種子", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(u8"權重", ImGuiTableColumnFlags_WidthFixed, std::floor(PobUi::D(56.0f)));
			ImGui::TableSetupColumn(u8"詞綴", ImGuiTableColumnFlags_WidthFixed, std::floor(PobUi::D(48.0f)));
			ImGui::TableSetupColumn(u8"動作", ImGuiTableColumnFlags_WidthFixed, actW);
			const bool right[4] = { false, true, true, true };
			headerRow(4, right);
			const int nw = (int)wants.size();
			ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
			ImGuiListClipper clip;
			clip.Begin((int)job.results.size(), rowH);
			while (clip.Step()) {
				for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
					const TJSeedHit& h = job.results[r];
					ImGui::TableNextRow(0, rowH);
					ImGui::PushID(h.seed);
					ImGui::TableNextColumn();
					// the whole row selects the seed (replaces the old 查看 button)
					if (ImGui::Selectable(std::to_string(h.seed).c_str(), detailSeed == h.seed,
					                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
					                      ImVec2(0, smH)))
						detailSeed = h.seed;
					ImGui::TableNextColumn();
					char buf[32];
					snprintf(buf, sizeof(buf), "%.1f", h.weight);
					centreLine();
					rightText(buf, Tok::Text);
					ImGui::TableNextColumn();
					// Coverage next to the score: with several stats picked, "weight 3"
					// alone cannot tell all three stats once from one stat three times.
					snprintf(buf, sizeof(buf), "%d/%d", h.distinctWants, nw);
					centreLine();
					rightText(buf, h.distinctWants < nw ? Tok::TextMuted : Tok::Text);
					ImGui::TableNextColumn();
					const float cellX = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x - actW;
					ImGui::SetCursorScreenPos(ImVec2(cellX, ImGui::GetCursorScreenPos().y));
					if (PobUi::Button(u8"交易", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, !tradeOff())) {
						open_trade_search(tradeStatId_, h.seed, tradeLeague, tradePlatform, tradeRealm);
						tradeHintShown = true;
					}
					if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
						PobUi::Tooltip(tradeOff() ? tradeOffWhy() : u8"在交易站搜尋這個種子（即時購買）");
					ImGui::SameLine(0.0f, PobUi::D(4.0f));
					if (PobUi::Button("##copy", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Copy, smH))
						copyForPob(h.seed);
					if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"複製給 POB：貼進 POB 的物品欄 (Ctrl+V)");
					ImGui::PopID();
				}
			}
			ImGui::PopStyleVar();
			ImGui::EndTable();
		}
		popTableStyle();
	}

	// Seeds grouped by how many nodes matched (desc), like Vilsol; each group
	// can go to the trade site in one search.
	void drawGroupedResults(float height)
	{
		// the socket's picked in-radius nodes (or all) + wanted set. Zorath is
		// judged on the same neighbourhood the search used; only 7-10 get their
		// node list from the file instead.
		std::vector<int> inRad;
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
		std::map<int, std::vector<const TJSeedHit*>, std::greater<int>> groups;
		for (const auto& h : job.results) groups[h.matches].push_back(&h);

		ImGui::BeginChild("##resg", ImVec2(0, height), false);
		bool firstGroup = true;
		const int nw = (int)wants.size();
		for (auto& kv : groups) {
			char lbl[64], id[32], note[64];
			snprintf(lbl, sizeof(lbl), u8"命中 %d 個節點", kv.first);
			snprintf(id, sizeof(id), "##grp%d", kv.first);
			snprintf(note, sizeof(note), u8"%d 個種子", (int)kv.second.size());
			const bool open = PobUi::CollapsingSection(lbl, id, nullptr, note, firstGroup);
			firstGroup = false;
			if (!open) continue;
			ImGui::PushID(kv.first);
			if (PobUi::Button(u8"交易查詢整組", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::ExternalLink, 0.0f,
			                  !tradeOff())) {
				std::vector<int> seeds;
				for (auto* h : kv.second) seeds.push_back(h->seed);
				open_trade_search_multi(tradeStatId_, seeds, tradeLeague, tradePlatform, tradeRealm);
				tradeHintShown = true;
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				PobUi::Tooltip(tradeOff() ? tradeOffWhy() : u8"一次搜尋這一組的所有種子");
			if (kv.second.size() > kMaxTradeSeeds) {
				ImGui::SameLine(0.0f, PobUi::D(8.0f));
				ImGui::AlignTextToFramePadding();
				PobUi::Hint(u8"（交易取前 40 個）");
			}
			ImGui::PopID();

			int shown = 0;
			for (auto* h : kv.second) {
				if (++shown > 50) {
					PobUi::Hint((u8"還有 " + std::to_string((int)kv.second.size() - 50) + u8" 個（請縮小條件）").c_str());
					break;
				}
				ImGui::PushID(h->seed);
				char head[96];
				snprintf(head, sizeof(head), u8"種子 %d", h->seed);
				char meta[96];
				snprintf(meta, sizeof(meta), u8"權重 %.1f · 詞綴 %d/%d", h->weight, h->distinctWants, nw);
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float w = ImGui::GetContentRegionAvail().x;
				const bool sel = detailSeed == h->seed;
				const float actW = PobUi::ButtonWidth(u8"交易", PobUi::BtnSize::Sm);
				ImGui::SetNextItemAllowOverlap();
				if (ImGui::Selectable("##row", sel, 0, ImVec2(w, std::floor(PobUi::D(28.0f))))) detailSeed = h->seed;
				ImGui::SetCursorScreenPos(ImVec2(p.x + PobUi::D(4.0f), p.y + std::floor((PobUi::D(28.0f) - ImGui::GetTextLineHeight()) * 0.5f)));
				ImGui::TextColored(PobUi::TokV4(Tok::Text), "%s", head);
				ImGui::SameLine(0.0f, PobUi::D(10.0f));
				PobUi::Hint(meta);
				ImGui::SetCursorScreenPos(ImVec2(p.x + w - actW, p.y));
				if (PobUi::Button(u8"交易", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, !tradeOff())) {
					open_trade_search(tradeStatId_, h->seed, tradeLeague, tradePlatform, tradeRealm);
					tradeHintShown = true;
				}
				ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor(PobUi::D(28.0f)) + ImGui::GetStyle().ItemSpacing.y));
				// this seed's matched affixes only (no node name), on the picked
				// nodes -- keeps the list compact
				auto drawMatched = [&](const TJTransform& t) {
					for (size_t i = 0; i < t.lines.size(); i++) {
						const TJWantStat* wnt = matcher.Match(t.lines[i]);
						if (!wnt) continue; // below 最小值 counts for nothing, so it shows as nothing
						const std::string& zh = (i < t.linesZh.size() && !t.linesZh[i].empty()) ? t.linesZh[i] : t.lines[i];
						ImGui::PushStyleColor(ImGuiCol_Text, colorStats ? stat_color(zh) : PobUi::TokV4(Tok::TextMuted));
						// Show what this line contributed, so the seed's rank is legible.
						if (wnt->weight != 1.0) ImGui::BulletText(u8"%s  [權重 %.1f]", zh.c_str(), wnt->weight);
						else ImGui::BulletText("%s", zh.c_str());
						ImGui::PopStyleColor();
					}
				};
				if (TJIsAbyss(jewelType)) {
					// For 7-10 inRad is empty and always will be -- the conquered
					// passives come from the file. Zorath is the other way round:
					// the file has an answer for every passive, so the
					// neighbourhood decides which answers are worth showing.
					std::map<int, TJAbyssMod> rec;
					bool got = false;
					if (selSocket >= 0) {
						if (TJIsZorath(jewelType)) {
							std::vector<int> ids;
							ids.reserve(inRad.size());
							for (int idx : inRad) ids.push_back(ptData.nodes[idx].id);
							got = TJAbyssReadNodes(*ds, *blob, *abyssLut, ids, h->seed, rec);
						} else {
							got = TJAbyssReadSocket(*ds, *blob, *abyssLut, ptData.nodes[selSocket].id, h->seed, rec);
						}
					}
					if (got) {
						for (const auto& m : rec) {
							if (!TJAbyssInScope(scope, m.first, *ds, &ptKind)) continue;
							TJTransform t = TJAbyssApply(*ds, m.second);
							if (t.ok) drawMatched(t);
						}
					}
				} else {
					for (int idx : inRad) {
						const PtNode& n = ptData.nodes[idx];
						const char* nt = n.kind == kPtKeystone ? "Keystone" : n.kind == kPtNotable ? "Notable" : "Normal";
						TJTransform t = TJApply(*ds, *blob, jewelType, h->seed, n.id, nt, n.stats,
						                        ConquerorType(jewelType), conquerorId_, n.name);
						if (t.ok) drawMatched(t);
					}
				}
				ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
				ImGui::PopID();
			}
			ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
		}
		ImGui::EndChild();
	}

	// The chosen seed: title + copy / trade, then what it changes.
	void drawDetail(float bottom)
	{
		const float availH = bottom - ImGui::GetCursorScreenPos().y;
		if (selSocket < 0) {
			PobUi::EmptyState("##nosocket", PobIcon::Crosshair, u8"還沒選插槽", u8"在中間的天賦樹上點一個珠寶插槽。",
			                  nullptr, 0.0f, mode == 1 ? availH : 0.0f);
			return;
		}
		if (detailSeed < 0) {
			PobUi::EmptyState("##noseed", PobIcon::Gem, u8"還沒選種子",
			                  mode == 0 ? u8"在上方的結果點一列，就會在這裡列出它的變化。"
			                            : u8"在左側輸入種子後按「查詢」，或從遊戲貼上珠寶。",
			                  nullptr, 0.0f, mode == 1 ? availH : 0.0f);
			return;
		}

		PobUi::CardBegin("##detail", nullptr, nullptr, nullptr, true);
		const float innerX = PobUi::CardInnerX();
		const float innerW = PobUi::CardInnerWidth();
		const float smH = std::floor(PobUi::D(28.0f));

		// head: title left, actions right (wrapped under the title when narrow).
		// "in radius" is only true for the Legion jewels: the Abyss ones name
		// their conquered passives in the file and scatter them over the tree.
		char title[96];
		snprintf(title, sizeof(title),
		         TJIsZorath(jewelType) ? u8"種子 %d 對這一帶天賦的效果"
		         : TJIsAbyss(jewelType) ? u8"種子 %d 征服的天賦"
		                                : u8"種子 %d 的變化",
		         detailSeed);
		const char* copyLbl = u8"複製給 POB";
		const char* tradeLbl = u8"交易搜尋";
		const float btnsW = PobUi::ButtonWidth(copyLbl, PobUi::BtnSize::Sm, PobIcon::Copy) + PobUi::D(4.0f) +
		                    PobUi::ButtonWidth(tradeLbl, PobUi::BtnSize::Sm, PobIcon::ExternalLink);
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		const float titleW = wf.heading ? wf.heading->CalcTextSizeA(
			wf.headingPx > 0 ? wf.headingPx : wf.heading->FontSize, FLT_MAX, 0.0f, title).x : ImGui::CalcTextSize(title).x;
		const ImVec2 hp = ImGui::GetCursorScreenPos();
		const bool oneLine = titleW + PobUi::D(12.0f) + btnsW <= innerW;
		const float lineH = (std::max)(smH, ImGui::GetTextLineHeight());
		ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + std::floor((lineH - ImGui::GetTextLineHeight()) * 0.5f)));
		PobUi::Heading(title);
		float bx = oneLine ? innerX + innerW - btnsW : innerX;
		float by = oneLine ? hp.y + std::floor((lineH - smH) * 0.5f) : hp.y + lineH + PobUi::D(6.0f);
		ImGui::SetCursorScreenPos(ImVec2(bx, by));
		// Hand the jewel to PoB the way PoB expects to receive items: as the
		// game's own copy text on the clipboard.
		if (PobUi::Button(copyLbl, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Copy)) copyForPob(detailSeed);
		if (ImGui::IsItemHovered())
			PobUi::Tooltip(u8"複製成遊戲的物品文字格式，貼進 POB「物品」分頁即可建立這顆珠寶");
		ImGui::SameLine(0.0f, PobUi::D(4.0f));
		if (PobUi::Button(tradeLbl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::ExternalLink, 0.0f,
		                  !tradeOff())) {
			open_trade_search(tradeStatId_, detailSeed, tradeLeague, tradePlatform, tradeRealm);
			tradeHintShown = true;
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			PobUi::Tooltip(tradeOff() ? tradeOffWhy() : u8"在交易站搜尋這個種子（即時購買）");
		ImGui::SetCursorScreenPos(ImVec2(innerX, by + smH + PobUi::D(10.0f)));

		// Zorath's Ascendancy pick needs no path, so unlike everything else about
		// this jewel it is exact -- the most trustworthy thing on screen.
		if (TJIsZorath(jewelType) && binOk) {
			if (PobUi::CollapsingSection(u8"昇華天賦選擇（準確）", "##asc", nullptr, nullptr, false)) {
				std::map<std::string, std::vector<int>> asc;
				if (!TJAbyssReadAscendancies(*blob, *abyssLut, detailSeed, asc)) {
					PobUi::Hint(u8"讀取失敗");
				} else {
					ImGui::BeginChild("##asc", ImVec2(innerW, 170 * scale), false);
					for (const auto& kv : asc) {
						ImGui::TextColored(PobUi::TokV4(Tok::TreeHit), "%s", kv.first.c_str());
						if (kv.second.empty()) {
							// Ascendant is the standing case: all of its notables cost five
							// points and the jewel cannot rewrite one costing four or more.
							ImGui::SameLine();
							PobUi::Hint(u8"（不受影響）");
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
								const std::string& z = (i < t.linesZh.size() && !t.linesZh[i].empty()) ? t.linesZh[i] : t.lines[i];
								ImGui::PushStyleColor(ImGuiCol_Text, colorStats ? stat_color(z) : PobUi::TokV4(Tok::Text));
								ImGui::BulletText("%s", z.c_str());
								ImGui::PopStyleColor();
							}
						}
					}
					ImGui::EndChild();
					// PobTools' tree leaves Ascendancy nodes out; what the notable
					// becomes is the part being chosen, so that is what is shown.
					PobUi::Hint(u8"本工具的樹資料不含昇華節點，只顯示它會變成什麼。", innerW);
				}
			}
		}

		// view controls: list kind | colour; for the stat list also sort + split
		{
			const char* views[2] = { u8"詞綴檢視", u8"節點檢視" };
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float H = PobUi::ControlH();
			PobUi::Segmented("##view", &listView, views, 2);
			const char* colLbl = u8"上色";
			const ImFont* sf = wf.small ? wf.small : ImGui::GetFont();
			const float cw = ((ImFont*)sf)->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, colLbl).x;
			float rx = innerX + innerW - PobUi::SwitchWidth();
			ImGui::SetCursorScreenPos(ImVec2(rx, p.y + std::floor((H - std::floor(PobUi::D(22.0f))) * 0.5f)));
			PobUi::Switch("##color", &colorStats);
			if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"依傷害與防禦類型（火、冰、雷…）替詞綴上色");
			ImGui::SetCursorScreenPos(ImVec2(rx - PobUi::D(6.0f) - cw, p.y + std::floor((H - sf->FontSize) * 0.5f)));
			PobUi::Hint(colLbl);
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + H + PobUi::D(6.0f)));
			if (listView == 0) {
				const ImVec2 q = ImGui::GetCursorScreenPos();
				const char* sorts[4] = { u8"依數量", u8"依字母", u8"依稀有度", u8"依數值" };
				PobUi::Select("##statsort", &statSort, sorts, nullptr, 4, std::floor(PobUi::D(120.0f)));
				const char* splitLbl = u8"大點／一般分開";
				const float sw = ((ImFont*)sf)->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, splitLbl).x;
				ImGui::SetCursorScreenPos(ImVec2(rx, q.y + std::floor((H - std::floor(PobUi::D(22.0f))) * 0.5f)));
				PobUi::Switch("##split", &splitList);
				ImGui::SetCursorScreenPos(ImVec2(rx - PobUi::D(6.0f) - sw, q.y + std::floor((H - sf->FontSize) * 0.5f)));
				PobUi::Hint(splitLbl);
				ImGui::SetCursorScreenPos(ImVec2(q.x, q.y + H + PobUi::D(6.0f)));
				if (hlStatGroup >= 0) {
					if (PobUi::Link(u8"顯示全部節點")) hlStatGroup = -1;
					if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"目前只在樹上框出一條詞綴的節點");
				}
			}
		}

		// the list fills the rest of the card
		const float listH = (std::max)(PobUi::D(80.0f), bottom - ImGui::GetCursorScreenPos().y - PobUi::D(16.0f) - 2.0f);
		ImGui::BeginChild("##aff", ImVec2(innerW, listH), false);
		if (listView == 0) drawStatList(innerW);
		else drawNodeList(innerW);
		ImGui::EndChild();
		PobUi::CardEnd();
	}

	void drawAffectedEmpty(float w)
	{
		PobUi::EmptyState("##noaff", PobIcon::Info, u8"沒有受影響的詞綴",
		                  TJIsAbyss(jewelType) ? u8"這個插槽在此種子下沒有被征服的天賦（或資料仍在計算）。"
		                                       : u8"這個範圍內沒有受影響的節點（或資料仍在計算）。",
		                  nullptr, w - PobUi::D(4.0f));
	}

	// Vilsol-style: "(N) stat", click to frame those N nodes on the tree
	void drawStatList(float w)
	{
		if (statGroups.empty()) { drawAffectedEmpty(w); return; }
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
		auto drawGroup = [&](int g, int section) { // section: -1 all, 0 notables, 1 smalls
			int c = section == 0 ? (int)statGroups[g].notables.size()
			      : section == 1 ? (int)statGroups[g].smalls.size() : cnt(g);
			if (c == 0) return;
			char lbl[256];
			snprintf(lbl, sizeof(lbl), "(%d) %s##g%d%d", c, statGroups[g].name.c_str(), g, section + 1);
			ImGui::PushStyleColor(ImGuiCol_Text, colorStats ? stat_color(statGroups[g].name) : PobUi::TokV4(Tok::Text));
			bool sel = ImGui::Selectable(lbl, hlStatGroup == g);
			ImGui::PopStyleColor();
			if (sel) hlStatGroup = (hlStatGroup == g) ? -1 : g;
		};
		if (splitList) {
			PobUi::SectionHeader(u8"大點／關鍵天賦", w - PobUi::D(4.0f));
			for (int g : order) drawGroup(g, 0);
			ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			PobUi::SectionHeader(u8"一般節點", w - PobUi::D(4.0f));
			for (int g : order) drawGroup(g, 1);
		} else {
			for (int g : order) drawGroup(g, -1);
		}
	}

	// node-centric: "(star) name / affix", big ones in their kind colour; click to
	// glide the tree to that node
	void drawNodeList(float w)
	{
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
		if (rows.empty()) { drawAffectedEmpty(w); return; }
		for (const Row& r : rows) {
			const PtNode& n = ptData.nodes[r.idx];
			const TJTransform& t = ptTrans.at(r.idx);
			std::string nm = t.replaced ? (t.newNameZh.empty() ? t.newName : t.newNameZh)
			                            : (n.nameZh.empty() ? n.name : n.nameZh);
			const std::uint32_t nc = r.big ? PobUi::TreeKindColor(n.kind == kPtKeystone ? PobUi::TreeKind::Keystone
			                                                                            : PobUi::TreeKind::Notable)
			                               : Tok::Text;
			ImGui::PushID(r.idx);
			// A star column: ★ = this node is why the seed ranked (it satisfies a
			// wanted stat). Drawn, not typed -- U+2605 is outside the CJK glyph
			// ranges the tool fonts are built with, so as text it would be a '?'.
			const float lineH = ImGui::GetTextLineHeight();
			const float starW = std::floor(lineH * 1.1f);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			bool sel = ImGui::Selectable("##node", emphNode == r.idx, 0, ImVec2(0, lineH));
			ImDrawList* dl = ImGui::GetWindowDrawList();
			if (r.prio) DrawStar(dl, ImVec2(p.x + starW * 0.45f, p.y + lineH * 0.5f), lineH * 0.42f, Tok::TreeHit);
			dl->AddText(ImVec2(p.x + starW, p.y), nc, nm.c_str());
			if (sel) { panToNode = r.idx; emphNode = r.idx; }
			ImGui::Indent(PobUi::D(20.0f));
			for (size_t i = 0; i < t.lines.size(); i++) {
				const std::string& zh = (i < t.linesZh.size() && !t.linesZh[i].empty()) ? t.linesZh[i] : t.lines[i];
				ImGui::PushStyleColor(ImGuiCol_Text, colorStats ? stat_color(zh) : PobUi::TokV4(Tok::TextMuted));
				ImGui::TextWrapped("%s", zh.c_str());
				ImGui::PopStyleColor();
			}
			ImGui::Unindent(PobUi::D(20.0f));
			ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
			ImGui::PopID();
		}
	}

	// ---- POBTOOLS_TJ_STATE (test aid, with POBTOOLS_TOOL_SHOT) ---------------
	// Opens the window in a known state for a hidden-window screenshot: Lethal
	// Pride, the socket with the most notables in reach, three wanted stats.
	//   search  = the form filled in        results = after a search (first seed picked)
	//   seed    = the seed page, a seed looked up       menu = the more menu open
	//   paste   = a jewel "pasted" (TJItemText round-trip, no clipboard)
	//   seederr = the seed page's error banner (empty seed)
	// Nothing is written: under a test aid the league fetch, the tree-update
	// check and tj_ui.json are all off.
	void applyTestState()
	{
		if (testState_.empty() || !ptDataOk) return;
		jewelType = 2;
		conquerorSel = 0;
		templates = TJStatTemplates(*ds, jewelType);
		templatesFor = jewelType;
		ensureBin(jewelType);
		int best = -1, bestN = -1;
		for (int s : ptData.sockets) {
			int n = 0;
			for (int idx : ptData.NodesInRadius(s, 1800.0f))
				if (ptData.nodes[idx].kind == kPtNotable) n++;
			if (n > bestN) { bestN = n; best = s; }
		}
		selSocket = best;
		centerPending_ = true;
		const char* wantEn[3] = { "Attack Speed", "Leech", "maximum Life" };
		const float wantW[3] = { 1.0f, 2.0f, 1.5f };
		for (int k = 0; k < 3; k++)
			for (const auto& t : templates)
				if (contains_ci(t.en, wantEn[k])) {
					bool dup = false;
					for (auto& w : wants) if (w.en == t.en) dup = true;
					if (dup) continue;
					wants.push_back({ t.en, t.zh.empty() ? t.en : t.zh, 0.0f, wantW[k] });
					break;
				}
		const int lo = ds->seedMin.count(jewelType) ? ds->seedMin.at(jewelType) : 10000;
		const int hi = ds->seedMax.count(jewelType) ? ds->seedMax.at(jewelType) : 18000;
		const int mid = lo + (hi - lo) / 2;
		if (testState_ == "results") {
			mode = 0;
			autoPick_ = true;
			tradeHintShown = true;   // as if a trade search had been opened
			runSearch();
		} else if (testState_ == "seed") {
			mode = 1;
			seedText = std::to_string(mid);
			querySeed();
		} else if (testState_ == "menu") {
			openMore_ = true;
		} else if (testState_ == "paste") {
			mode = 0;   // the paste must be what switches to the seed page
			applyPaste(TJItemText(*ds, jewelType, conquerorSel, mid));
		} else if (testState_ == "seederr") {
			mode = 1;
			seedText.clear();
			querySeed();
		}
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
		if (testMode_) return;   // a test run writes nothing
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
	char statFilter_[128] = "";   // the add-a-stat search box
	bool browseAll_ = false;      // show the whole stat list with an empty box
	std::vector<WantRow> wants;
	std::string seedText = "500";
	std::string seedErr_;         // the seed page's own error (banner on that page)
	int detailSeed = -1;          // a result seed to expand
	// this frame's conqueror, for the trade export and TJApply
	std::string conquerorId_ = "1";
	std::string tradeStatId_;
	// results: toast once per finished search; the test aid picks the first seed
	bool jobReported_ = true;
	bool autoPick_ = false;
	// header: the more menu, and whether the tree version moved into it
	bool openMore_ = false;
	ImVec2 moreAnchor_{ 0, 0 };
	bool verInMenu_ = false;
	// banners: a failure that needs the user (paste, copy, data); a dismissed
	// tree-update error stays dismissed until the message changes
	std::string errTitle_, errDesc_;
	bool errWarn_ = false;
	std::string ptErrDismissed_;
	bool tradeHintClosed_ = false;
	float infoBannerH_ = 90.0f;   // last frame's height of the POB cross-check banner
	bool centerPending_ = false;  // glide to the socket once the canvas has a camera
	// POBTOOLS_TJ_STATE / POBTOOLS_TOOL_SHOT
	std::string testState_;
	bool testMode_ = false;
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
	int listView = 1;                            // 0 = stat-centric (Vilsol), 1 = node-centric (the design's default)
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
	bool groupResults = false;                   // group seeds by # of nodes matched (the design: a table)

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
