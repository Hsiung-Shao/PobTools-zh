#include "timeless_jewel_ui.h"
#include "tool_panel.h"
#include "tool_window.h"

#include "launcher_config.h"
#include "http_client.h"
#include "passive_tree_data.h"
#include "passive_tree_update.h"
#include "passive_tree_view.h"
#include "timeless_jewel.h"
#include "error_log.h"
#include "timeless_jewel_abyss.h"
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_icons.h"
#include "clipboard_util.h"

#include <json.hpp>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include "win_util.h"

#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <unordered_map>
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

} // namespace

// trade realms: every conqueror the calculator offers exists on both regions with the same id (--tj-realm-check re-proves it).
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

size_t TradeSeedsPerSearch(size_t nIds)
{
	return nIds ? (std::max)((size_t)1, kMaxTradeSeeds / nIds) : kMaxTradeSeeds;
}

std::string TradeQueryJsonAny(const std::vector<std::string>& statIds, const std::vector<int>& seeds)
{
	std::string filters;
	size_t n = 0;
	for (int s : seeds)
		for (const std::string& id : statIds) {
			if (n >= kMaxTradeSeeds) break;
			char f[256];
			snprintf(f, sizeof(f), "%s{\"id\":\"%s\",\"value\":{\"min\":%d,\"max\":%d}}", n ? "," : "", id.c_str(), s, s);
			filters += f;
			n++;
		}
	return "{\"query\":{\"status\":{\"option\":\"securable\"},\"stats\":[{\"type\":\"count\","
	       "\"value\":{\"min\":1},\"filters\":[" + filters + "]}]"
	       "},\"sort\":{\"price\":\"asc\"}}";
}

TradeBatch TradeNextBatch(size_t total, size_t next, size_t per)
{
	const size_t begin = next < total ? next : 0;
	return { begin, (std::min)(total, begin + (per ? per : kMaxTradeSeeds)) };
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

// Pure URL assembly (kept apart from ShellExecute so --tj-selftest can assert on it).
// platform: 0 pc, 1 xbox, 2 sony — ignored by realms without consoles.
std::string TradeSearchUrl(int realmIdx, const std::string& league, int platform,
                           const std::string& queryJson)
{
	if (league.empty() || queryJson.empty()) return std::string();
	if (realmIdx < 0 || realmIdx >= kTradeRealmCount) realmIdx = 0;
	const TradeRealm& r = kTradeRealms[realmIdx];
	const char* console = "";
	if (r.consoles) console = platform == 1 ? "xbox/" : platform == 2 ? "sony/" : "";
	return "https://" + std::string(r.host) + "/trade/search/" + console +
	       url_encode(league) + "?q=" + url_encode(queryJson);
}

// TjUiState

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
		lastLegion = doc.value("lastLegion", 3);
		lastAbyss = doc.value("lastAbyss", 8);
		abyssMode = doc.value("abyssMode", false);
		classId = doc.value("classId", -1);
		asc = doc.value("asc", std::string());
		return true;
	} catch (...) {
		realm = 0; platform = 0; league.clear(); lastLegion = 3; lastAbyss = 8; abyssMode = false; classId = -1; asc.clear();
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
	doc["lastLegion"] = lastLegion;
	doc["lastAbyss"] = lastAbyss;
	doc["abyssMode"] = abyssMode;
	if (classId >= 0) doc["classId"] = classId;
	if (!asc.empty()) doc["asc"] = asc;
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

// Background one-shot fetch of the current trade leagues. The API lists every
// realm in one array ({"id":"Allflame","realm":"pc",...}), current league first,
// so keep the realm alongside the id and preserve that order.
struct LeagueFetch {
	std::atomic<bool> running{ false }, done{ false };
	std::vector<std::pair<std::string, std::string>> all; // (realm, id), API order
	std::thread th;
	const wchar_t* host = kTradeRealms[0].hostW;
	~LeagueFetch() { if (th.joinable()) th.join(); }
	// Switching region refetches; join first so `all` isn't written by the outgoing thread.
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

// Filled five-point star ("node matched" mark): pentagon plus five convex tips.
void DrawStar(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
	ImVec2 outer[5], inner[5];
	const float ri = r * 0.40f;
	for (int i = 0; i < 5; i++) {
		const float a = -1.5707963f + i * 1.2566371f;
		const float b = a + 0.6283185f;
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

// kMaxJewelType lives in timeless_jewel_ui.h so both selftests can assert against it.
// Only Zorath stays hidden, for a PobTools-side reason — see the header.

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
	return PobUi::TokV4(PobUi::Tok::Text);
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
			auto r = TJSearch(*ds, *blob, q, q.wants.empty() && q.requiredAscOutcome.empty() ? 0 : 500, &cancel); // nothing scored: every seed
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
			auto r = TJAbyssSearch(*ds, *blob, lut, q, socketId, q.wants.empty() && q.requiredAscOutcome.empty() ? 0 : 500, &nodeKind, &cancel);
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
	for (int y = 0; y < h; y++) {
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
	PassiveTreeInput tin;
	if (!ptData.sockets.empty()) {
		tin.selectedSocket = ptData.sockets[ptData.sockets.size() / 2];
		std::vector<int> inr = ptData.NodesInRadius(tin.selectedSocket, 1800.0f);
		for (size_t k = 0; k < inr.size(); k++) {
			if (k % 3 == 0) ptHi[inr[k]] = kPtHiAffected;
			if (k % 4 == 0) ptSel[inr[k]] = 1;
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

// timeless jewel calculator, as a panel
//
// The window / GL context / font atlas / main loop belong to whichever host is
// drawing this (RunToolWindow or the launcher's tab body). See tool_panel.h.

namespace {

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
			const int t = tjUi.abyssMode ? tjUi.lastAbyss : tjUi.lastLegion;
			if (t >= 1 && t <= kMaxJewelType && TJIsAbyss(t) == tjUi.abyssMode) jewelType = t;
			classId_ = tjUi.classId;
			asc_ = tjUi.asc;
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
		// The host's font-size zoom can change while the panel is open (the
		// launcher's slider; a tool window follows the ini): read it per frame.
		scale = host_->scale;
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
			ptProtected.clear();
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
			(TJIsAbyss(jewelType) ? tjUi.lastAbyss : tjUi.lastLegion) = jewelType;
			tjUi.abyssMode = TJIsAbyss(jewelType);
			saveTjUi();
		}
		updateAbyssDerived();

		// conqueror (affects keystones + trade export)
		const std::vector<TJConqueror>* conqs = conqListFor(jewelType);
		const int conqN = conqs ? (int)conqs->size() : 0;
		deriveConqueror(conqs, conqN);

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
		// POBTOOLS_TJ_CONQ_SWITCH: after the first recompute, change the
		// conqueror exactly where the header's picker would (after conquerorId_
		// was derived this frame) — the path a real click takes.
		if (testConqSwitch_ >= 0 && hiSig != -1) { conquerorSel = testConqSwitch_; testConqSwitch_ = -1; }

		ImGui::PushID("tjbanners");
		drawBanners(ptUst);
		ImGui::PopID();

		// Up to four columns: the form | the tree | the chosen seed | the results.
		// The seed column only exists while a seed is chosen (the tree takes its
		// room back otherwise). On the seed page there are no results: the chosen
		// seed is the right-hand column and is always there.
		const bool showResults = mode == 0;
		const bool showDetail = mode == 1 || detailSeed >= 0;
		const bool four = showResults && showDetail;
		const float availW = ImGui::GetContentRegionAvail().x;
		float leftW = std::floor(std::clamp(availW * (four ? 0.23f : 0.264f), PobUi::D(260.0f), PobUi::D(380.0f)));
		float colW = std::floor(std::clamp(availW * (four ? 0.235f : 0.278f), PobUi::D(280.0f), PobUi::D(400.0f)));
		const int sideCols = (showResults ? 1 : 0) + (showDetail ? 1 : 0);
		float midW = availW - leftW - colW * sideCols;
		// Keep the tree usable: give back from the side columns first, then the form.
		const float minTree = PobUi::D(300.0f);
		if (midW < minTree && sideCols > 0) {
			const float give = (std::min)((minTree - midW) / sideCols, colW - PobUi::D(250.0f));
			if (give > 0.0f) { colW = std::floor(colW - give); midW = availW - leftW - colW * sideCols; }
		}
		if (midW < minTree) {
			const float give = (std::min)(minTree - midW, leftW - PobUi::D(240.0f));
			if (give > 0.0f) { leftW = std::floor(leftW - give); midW = availW - leftW - colW * sideCols; }
		}
		if (midW < PobUi::D(160.0f)) midW = PobUi::D(160.0f);
		midW = std::floor(midW);

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

		std::vector<float> rules = { colTop.x + leftW - 0.5f, colTop.x + leftW + midW + 0.5f };
		if (showDetail) {
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::PushID("tjdetail");
			// the right-most column fills the rest (no rounding gap at the edge)
			beginSidePanel("##detailcol", showResults ? colW : 0.0f);
			drawDetailColumn(!showResults);
			endSidePanel();
			ImGui::PopID();
			if (showResults) rules.push_back(colTop.x + leftW + midW + colW + 0.5f);
		}
		if (showResults) {
			ImGui::SameLine(0.0f, 0.0f);
			ImGui::PushID("tjright");
			beginSidePanel("##right", 0.0f);
			drawResultsColumn();
			endSidePanel();
			ImGui::PopID();
		}
		ImGui::PopStyleVar();

		// the column rules (border token), over the children's edges
		ImDrawList* dl = ImGui::GetWindowDrawList();
		for (float x : rules) dl->AddLine(ImVec2(x, colTop.y), ImVec2(x, colTop.y + colH), Tok::Border, 1.0f);

		// Popups opened from inside the columns are begun here, at the panel's
		// top level, never inside a child window.
		ImGui::PushID("tjtrade");
		drawTradePopup();
		ImGui::PopID();

		// Test aid (POBTOOLS_TJ_STATE=tradedump): once the search is in, send the
		// first group and then the whole list through the same path as the
		// buttons; under a test aid that path writes the URL and the decoded query
		// to POBTOOLS_TJ_TRADE_DUMP instead of opening a browser.
		if (testState_ == "tradedump" && !testDumped_ && job.done.load() && !job.running.load() &&
		    !job.results.empty()) {
			testDumped_ = true;
			std::map<int, std::vector<int>, std::greater<int>> g;
			for (const auto& h : job.results) g[h.matches].push_back(h.seed);
			// the first group with a few seeds in it (a one-seed group shows little)
			auto pick = g.begin();
			for (auto it = g.begin(); it != g.end(); ++it)
				if (it->second.size() >= 3) { pick = it; break; }
			tradeSeeds(pick->second, "group");
			std::vector<int> all;
			for (const auto& h : job.results) all.push_back(h.seed);
			tradeSeeds(all, "all");
		}
	}

	// header: one row (ToolHeader): icon + name | paste | jewel | conqueror | mode ...
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
		// 永恆 (1-6) and 深淵 (7-11) search differently, so each mode lists only its own jewels
		const bool abyssKind = TJIsAbyss(jewelType);
		const int tLo = abyssKind ? 7 : 1, tHi = abyssKind ? kMaxJewelType : 6;
		const char* kindLabels[2] = { u8"永恆珠寶", u8"深淵珠寶" };
		std::vector<std::string> jewelLbl, jewelNote;
		for (int t = tLo; t <= tHi; t++) { jewelLbl.push_back(JewelZhShort(t)); jewelNote.push_back(JewelEn(t)); }
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
		if (conqAnyOk(conqN)) { conqLbl.push_back(u8"任意"); conqNote.push_back(u8"不限征服者，只看詞綴"); }
		const int conqItems = (int)conqLbl.size();
		std::vector<const char*> conqP, conqNP;
		for (int i = 0; i < conqItems; i++) { conqP.push_back(conqLbl[i].c_str()); conqNP.push_back(conqNote[i].c_str()); }
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
		                    gap + pasteW + gap + PobUi::SegmentedWidth(kindLabels, 2) + gap + jewelW + gap + conqW + gap + PobUi::SegmentedWidth(modeLabels, 2);

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

		// kind, then the jewel within it; each kind comes back to its last jewel
		at(x, H);
		int ksel = abyssKind ? 1 : 0;
		if (PobUi::Segmented("##kind", &ksel, kindLabels, 2) && ksel != (abyssKind ? 1 : 0)) {
			jewelType = ksel ? tjUi.lastAbyss : tjUi.lastLegion;
			if (TJIsAbyss(jewelType) != (ksel == 1)) jewelType = ksel ? 8 : 3;
			conquerorSel = 0;
			ensureBin(jewelType);
		}
		if (ImGui::IsItemHovered())
			PobUi::Tooltip(u8"深淵珠寶的計算與查詢照 POB 2.68，分數為權重 × 數值");
		x += PobUi::SegmentedWidth(kindLabels, 2) + gap;

		at(x, H);
		int jsel = jewelType - tLo;
		if (jsel < 0 || jsel >= (int)jewelP.size()) jsel = 0;
		if (PobUi::Select("##jewel", &jsel, jewelP.data(), jewelN.data(), (int)jewelP.size(), jewelW) &&
		    jsel + tLo != jewelType) {
			jewelType = jsel + tLo;
			conquerorSel = 0;
			ensureBin(jewelType);
		}
		x += jewelW + gap;

		// conqueror (keystones + trade export)
		at(x, H);
		int csel = conquerorSel;
		if (PobUi::Select("##conq", &csel, conqP.data(), conqNP.data(), conqItems, conqW, conqN > 0))
			conquerorSel = csel;
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"征服者決定關鍵天賦會變成哪一個，也是交易站搜尋用的詞綴；選「任意」不限征服者，只找種子對應的詞綴");
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
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"更多：天賦樹更新");

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

	// The more menu: the tree update (the trade settings have their own row above
	// the results). Opened and begun here, at the panel's top level -- never from
	// inside a child window.
	void drawMoreMenu(const PassiveTreeUpdater::Status& ust, bool updBusy, const std::string& verText)
	{
		if (openMore_) {
			ImGui::OpenPopup("##tjmore");
			openMore_ = false;
		}
		ImGui::SetNextWindowPos(moreAnchor_, ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
		if (!PobUi::BeginMenuPopup("##tjmore")) return;
		const float padX = PobUi::D(10.0f);
		if (PobUi::MenuRow(PobIcon::Refresh, u8"檢查天賦樹更新", nullptr, !updBusy && !testMode_))
			ptUpdater.RequestCheck(true);
		if (verInMenu_ && !verText.empty()) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
			PobUi::Numeric(verText.c_str());
		}
		(void)ust;
		PobUi::EndMenuPopup();
	}

	// trade settings: a standing row over the results ("交易站：國際服 · <league> · PC"): every
	// trade button depends on it, so it is in sight rather than in a menu. A
	// click opens the editor below it; with no league it is a warning.
	static const char* platformLabel(int p) { return p == 1 ? "Xbox" : p == 2 ? "PlayStation" : "PC"; }

	void drawTradeRow()
	{
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		ImFont* small = wf.small ? wf.small : ImGui::GetFont();
		ImFont* body = wf.body ? wf.body : ImGui::GetFont();
		const bool noLeague = tradeLeague.empty();
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		const float h = std::floor(PobUi::D(34.0f));
		const bool click = ImGui::InvisibleButton("##traderow", ImVec2(w, h));
		const bool hov = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const float r = PobUi::D(6.0f);
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h),
		                  noLeague ? Tok::WarningSoft : (hov ? Tok::Surface3 : Tok::Surface2), r);
		dl->AddRect(p, ImVec2(p.x + w, p.y + h), noLeague ? Tok::BannerWarnEdge : Tok::Border, r);

		const float px = body->FontSize;
		const float padX = PobUi::D(10.0f);
		float x = p.x + padX;
		const float ty = p.y + std::floor((h - px) * 0.5f);
		const char* icon = noLeague ? PobIcon::TriangleAlert : PobIcon::Globe;
		PobUi::IconAt(dl, ImVec2(x, ty), icon, noLeague ? Tok::Warning : Tok::TextMuted, px);
		x += PobUi::IconWidth(icon, px) + (wf.icons ? PobUi::D(6.0f) : 0.0f);
		std::string val = std::string(kTradeRealms[tradeRealm].label) + u8" · " +
		                  (noLeague ? std::string(u8"未設定聯盟，點此設定") : tradeLeague);
		if (kTradeRealms[tradeRealm].consoles) val += std::string(u8" · ") + platformLabel(tradePlatform);
		const float chevW = PobUi::IconWidth(PobIcon::ChevronDown, px);
		// the "交易站" caption goes first when the column is too narrow for both
		const char* lbl = u8"交易站";
		const float lblW = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, lbl).x + PobUi::D(8.0f);
		const float valW = body->CalcTextSizeA(px, FLT_MAX, 0.0f, val.c_str()).x;
		if (x + lblW + valW <= p.x + w - padX - chevW - PobUi::D(6.0f)) {
			const float sy = p.y + std::floor((h - small->FontSize) * 0.5f);
			dl->AddText(small, small->FontSize, ImVec2(x, sy), noLeague ? Tok::Warning : Tok::TextMuted, lbl);
			x += lblW;
		}
		const float maxW = p.x + w - padX - chevW - PobUi::D(6.0f) - x;
		dl->PushClipRect(ImVec2(x, p.y), ImVec2(x + (std::max)(0.0f, maxW), p.y + h), true);
		dl->AddText(body, px, ImVec2(x, ty), noLeague ? Tok::Warning : Tok::Text, val.c_str());
		dl->PopClipRect();
		PobUi::IconAt(dl, ImVec2(p.x + w - padX - chevW, ty), PobIcon::ChevronDown, Tok::TextMuted, px);

		if (hov) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			const std::string tip = noLeague ? std::string(u8"還沒設定聯盟，交易按鈕無法使用。點擊設定區域與聯盟。")
			                                 : u8"交易站：" + val + u8"。點擊修改區域、聯盟與平台（會記住，下次沿用）";
			PobUi::Tooltip(tip.c_str());
		}
		if (click) openTrade_ = true;
		tradeAnchor_ = ImVec2(p.x, p.y + h + PobUi::D(4.0f));
		tradeRowW_ = w;
	}

	// The editor the row opens. Begun at the panel's top level (Frame), with its
	// OpenPopup in the same ID scope.
	void drawTradePopup()
	{
		if (openTrade_) {
			ImGui::OpenPopup("##tjtradeset");
			openTrade_ = false;
		}
		ImGui::SetNextWindowPos(tradeAnchor_, ImGuiCond_Appearing);
		if (!PobUi::BeginMenuPopup("##tjtradeset")) return;
		const float padX = PobUi::D(10.0f);
		const float w = std::floor((std::max)(PobUi::D(260.0f), tradeRowW_ - padX * 2.0f));
		auto section = [&](const char* t) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX);
			PobUi::Overline(t);
		};
		auto indent = [&]() { ImGui::SetCursorPosX(ImGui::GetCursorPosX() + padX); };

		section(u8"區域");
		indent();
		{
			const char* realms[2] = { kTradeRealms[0].label, kTradeRealmCount > 1 ? kTradeRealms[1].label : "" };
			int sel = tradeRealm;
			if (PobUi::Segmented("##realm", &sel, realms, (std::min)(kTradeRealmCount, 2)) && sel != tradeRealm) {
				tradeRealm = sel;
				// League names are region-specific, so the current pick and the
				// cached list are both meaningless now: refetch and re-default.
				if (!kTradeRealms[sel].consoles) tradePlatform = 0;
				leagues.SetHost(kTradeRealms[sel].hostW);
				leagueUserSet = false;
				saveTjUi();
			}
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));

		section(u8"聯盟");
		{
			const char* refLbl = u8"重新取得";
			const float bw = PobUi::ButtonWidth(refLbl, PobUi::BtnSize::Sm, PobIcon::Refresh);
			const float fw = w - bw - PobUi::D(6.0f);
			const float H = PobUi::ControlH();
			const float smH = std::floor(PobUi::D(28.0f));
			indent();
			const ImVec2 rp = ImGui::GetCursorScreenPos();
			std::vector<std::string> lgList = leagues.ForPlatform(tradePlatform);
			if (!lgList.empty()) {
				std::vector<const char*> lp;
				int sel = -1;
				for (size_t i = 0; i < lgList.size(); i++) {
					lp.push_back(lgList[i].c_str());
					if (lgList[i] == tradeLeague) sel = (int)i;
				}
				if (PobUi::Select("##league", &sel, lp.data(), nullptr, (int)lp.size(), fw) && sel >= 0) {
					tradeLeague = lgList[sel];
					leagueUserSet = true;
					saveTjUi();
				}
			} else {
				PobUi::PushControlFrame();
				ImGui::SetNextItemWidth(fw);
				if (ImGui::InputTextWithHint("##league", u8"聯盟名稱（例：Standard）", &tradeLeague)) leagueUserSet = true;
				if (ImGui::IsItemDeactivatedAfterEdit()) saveTjUi();
				PobUi::PopControlFrame();
			}
			ImGui::SetCursorScreenPos(ImVec2(rp.x + fw + PobUi::D(6.0f), rp.y + std::floor((H - smH) * 0.5f)));
			const bool canFetch = !leagues.running.load() && !testMode_;
			if (PobUi::Button(leagues.running.load() ? u8"取得中…" : refLbl, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm,
			                  PobIcon::Refresh, 0.0f, canFetch)) {
				leagues.done = false;
				leagues.start();
			}
			if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				PobUi::Tooltip(u8"從交易站重新取得目前的聯盟清單");
			ImGui::SetCursorScreenPos(ImVec2(rp.x - padX, rp.y + H));
			ImGui::Dummy(ImVec2(0, 0));
		}
		// Consoles are an international-realm concept; the .tw site has none.
		if (kTradeRealms[tradeRealm].consoles) {
			ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
			section(u8"平台");
			indent();
			const char* plats[3] = { "PC", "Xbox", "PlayStation" };
			int sel = tradePlatform;
			// Only remember the choice. Deliberately NOT clearing leagueUserSet:
			// switching platform never re-defaulted the league before.
			if (PobUi::Segmented("##plat", &sel, plats, 3) && sel != tradePlatform) {
				tradePlatform = sel;
				saveTjUi();
			}
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
		indent();
		PobUi::Hint(u8"設定會記住，下次開啟沿用。", w);
		ImGui::Dummy(ImVec2(w + padX * 2.0f, PobUi::D(2.0f)));
		PobUi::EndMenuPopup();
	}

	// Every trade search goes through here. Under a test aid nothing is opened:
	// the URL and its decoded query go to POBTOOLS_TJ_TRADE_DUMP instead.
	void openTrade(const std::string& url, const char* tag)
	{
		if (url.empty()) return;
		tradeHintShown = true;
		if (!testMode_) { open_url(url); return; }
		wchar_t path[MAX_PATH] = L"";
		const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_TJ_TRADE_DUMP", path, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) return;
		std::wstring file = path;
		if (tag && *tag) file += L"." + widen(tag) + L".txt";
		// decode the q= parameter back out of the URL, so the file shows what the
		// trade site will receive and not what we meant to send
		std::string q;
		const size_t qp = url.find("?q=");
		if (qp != std::string::npos) {
			for (size_t i = qp + 3; i < url.size(); i++) {
				if (url[i] == '%' && i + 2 < url.size()) {
					q += (char)strtol(url.substr(i + 1, 2).c_str(), nullptr, 16);
					i += 2;
				} else q += url[i];
			}
		}
		std::string pretty;
		try { pretty = nlohmann::json::parse(q).dump(2); } catch (...) { pretty = "INVALID JSON: " + q; }
		const std::string out = "URL:\n" + url + "\n\nDecoded q=:\n" + pretty + "\n";
		HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE) return;
		DWORD wr = 0;
		WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr);
		CloseHandle(f);
	}

	void tradeSeed(int seed)
	{
		if (tradeOff()) return;
		const std::string q = conqAny_ ? TradeQueryJsonAny(tradeIds_, { seed }) : TradeQueryJson(tradeStatId_, seed);
		openTrade(TradeSearchUrl(tradeRealm, tradeLeague, tradePlatform, q), "seed");
	}

	// Several seeds in one search: a "count >= 1" stat group, one filter per seed
	// (min = max = seed) on the chosen conqueror's seed stat. Capped at
	// kMaxTradeSeeds; the caller says so next to the button.
	void tradeSeeds(const std::vector<int>& seeds, const char* tag)
	{
		if (tradeOff() || seeds.empty()) return;
		const std::string q = conqAny_ ? TradeQueryJsonAny(tradeIds_, seeds) : TradeQueryJsonMulti(tradeStatId_, seeds);
		openTrade(TradeSearchUrl(tradeRealm, tradeLeague, tradePlatform, q), tag);
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

	// side panels
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
			? u8"計算方式同原版POB。重奪惡意只改插槽到職業起點的路徑，加上昇華的一個天賦；"
			  u8"這裡的路徑取整棵樹上最短的一條，實際配點繞路時會不同。"
			: u8"計算方式同原版POB：分數為「權重×數值」，鑰石不計分。實測樣本仍少，請以遊戲內實際結果為準。";
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
			return TJIsZorath(jewelType) ? u8"請先在天賦樹上點一個珠寶插槽"
			       : TJIsAbyss(jewelType) ? u8"請先在天賦樹上點一個珠寶插槽（深淵珠寶的結果由插槽決定，沒有半徑）"
			                              : u8"請先在天賦樹上點一個珠寶插槽（搜尋只計算該插槽半徑內的節點）";
		}
		const PtNode& sn = ptData.nodes[selSocket];
		std::string s = u8"插槽：" + (sn.nameZh.empty() ? sn.name : sn.nameZh);
		if (TJIsAbyss(jewelType)) {
			const std::string why = abyssBlocker();
			if (!why.empty()) { *missing = true; return s + u8"｜" + why; }
			if (TJIsZorath(jewelType)) s += u8"（路徑 " + std::to_string(zorathPath_.size()) + u8" 個天賦）";
			return s;
		}
		int picked = 0;
		for (char c : ptSelected) if (c) picked++;
		if (picked > 0) s += u8"（只看已選的 " + std::to_string(picked) + u8" 個節點）";
		else s += u8"（已在樹上選取）";
		const size_t prot = protectedIds().size();
		if (prot > 0) s += u8"｜保護 " + std::to_string(prot) + u8" 個大點不被替換";
		return s;
	}

	// Only Militant Faith decides per seed whether a notable is replaced or kept
	// (--tj-selftest T6 measures this), so it is the only jewel where protecting
	// one can change which seeds qualify.
	bool canProtect() const { return jewelType == 4; }

	// Ids of the protected notables inside the selected socket's radius. Picking
	// another socket clears the marks, so the radius test is only a backstop.
	std::vector<int> protectedIds() const
	{
		std::vector<int> ids;
		if (!canProtect() || selSocket < 0) return ids;
		for (int idx : ptData.NodesInRadius(selSocket, 1800.0f))
			if (idx < (int)ptProtected.size() && ptProtected[idx] && ptData.nodes[idx].kind == kPtNotable)
				ids.push_back(ptData.nodes[idx].id);
		return ids;
	}

	// A Legion jewel turns every keystone in radius into its conqueror's, whatever
	// the seed -- no search can avoid it, so say it where the socket is chosen.
	std::string keystoneNote() const
	{
		if (selSocket < 0 || TJIsAbyss(jewelType)) return {};
		static const std::string kNoBlob;
		std::string lost, becomes;
		for (int idx : ptData.NodesInRadius(selSocket, 1800.0f)) {
			const PtNode& n = ptData.nodes[idx];
			if (n.kind != kPtKeystone) continue;
			TJTransform t = applyNode(kNoBlob, -1, n, "Keystone");
			if (!t.ok || !t.replaced) continue;
			lost += u8"「" + (n.nameZh.empty() ? n.name : n.nameZh) + u8"」";
			if (conqAny_) {
				becomes.clear();
				for (size_t i = 0; i < t.linesZh.size(); i++) becomes += (i ? u8"、" : "") + t.linesZh[i];
			} else {
				becomes = u8"「" + (t.newNameZh.empty() ? t.newName : t.newNameZh) + u8"」";
			}
		}
		if (lost.empty()) return {};
		if (conqAny_) return u8"此插槽的鑰石" + lost + u8"依征服者而定：" + becomes + u8"，與種子無關";
		return u8"此插槽的鑰石" + lost + u8"會變成" + becomes + u8"，與種子無關";
	}

	// "任意" sits after the conquerors (saved / pasted indices keep their meaning); Legion only.
	bool conqAnyOk(int conqN) const { return conqN > 1 && !TJIsAbyss(jewelType); }

	void deriveConqueror(const std::vector<TJConqueror>* conqs, int conqN)
	{
		const int items = conqN + (conqAnyOk(conqN) ? 1 : 0);
		if (conquerorSel >= items || conquerorSel < 0) conquerorSel = 0;
		conqAny_ = conqAnyOk(conqN) && conquerorSel == conqN;
		tradeIds_.clear();
		if (conqAny_) {
			conquerorId_ = "*"; // keystones go through applyNode; other nodes ignore the conqueror
			for (const TJConqueror& c : *conqs)
				if (!c.trade.empty() && std::find(tradeIds_.begin(), tradeIds_.end(), c.trade) == tradeIds_.end())
					tradeIds_.push_back(c.trade);
			tradeStatId_ = tradeIds_.empty() ? std::string() : tradeIds_.front();
		} else {
			conquerorId_ = conqN ? (*conqs)[conquerorSel].id : std::string("1");
			tradeStatId_ = conqN ? (*conqs)[conquerorSel].trade : std::string();
		}
	}

	// Seeds one trade search carries for the current conqueror pick.
	size_t tradePerSearch() const { return conqAny_ ? TradeSeedsPerSearch(tradeIds_.size()) : kMaxTradeSeeds; }

	// TJApply for the picked conqueror; under "任意" a keystone lists every conqueror's ("卡奧姆 → X").
	TJTransform applyNode(const std::string& blobRef, int seed, const PtNode& n, const char* nodeType) const
	{
		const std::vector<TJConqueror>* conqs = conqListFor(jewelType);
		if (!conqAny_ || !conqs || n.kind != kPtKeystone)
			return TJApply(*ds, blobRef, jewelType, seed, n.id, nodeType, n.stats, ConquerorType(jewelType),
			               conquerorId_, n.name);
		TJTransform out;
		for (const TJConqueror& c : *conqs) {
			const TJTransform t = TJApply(*ds, blobRef, jewelType, seed, n.id, nodeType, n.stats,
			                              ConquerorType(jewelType), c.id, n.name);
			if (!t.ok || !t.replaced) continue;
			out.ok = out.replaced = true;
			std::string who = c.nameZh.empty() ? c.name : c.nameZh;
			if (c.id.find("_v2") != std::string::npos) who += u8"（舊版）";
			out.lines.push_back(c.name + " -> " + t.newName);
			out.linesZh.push_back(who + u8" → " + (t.newNameZh.empty() ? t.newName : t.newNameZh));
		}
		out.newName = "Depends on conqueror";
		out.newNameZh = u8"依征服者而定";
		return out;
	}

	// The pinned block under both forms: the socket line, then the keystone
	// warning when there is one.
	float hintBlockH(const std::string& hint, const std::string& note, float w) const
	{
		ImFont* sf = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
		float h = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, w, hint.c_str()).y;
		if (!note.empty()) h += ImGui::GetStyle().ItemSpacing.y + sf->CalcTextSizeA(sf->FontSize, FLT_MAX, w, note.c_str()).y;
		return h;
	}
	void drawHintBlock(const std::string& hint, const std::string& note, float w, bool noSocket) const
	{
		PobUi::Hint(hint.c_str(), w, noSocket ? Tok::Warning : 0);
		if (!note.empty()) PobUi::Hint(note.c_str(), w, Tok::Warning);
	}

	// Zorath: the class start its path runs to (shortest over the whole tree, no build needed) and the
	// Ascendancy whose passives it can take (PoB 2.68: required outcome, protected passives).
	void drawBuildSection()
	{
		if (!TJIsZorath(jewelType)) return;
		const float w = ImGui::GetContentRegionAvail().x;
		const float H = PobUi::ControlH();
		PobUi::Heading(u8"職業起點");
		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		if (ptData.classStarts.empty()) {
			PobUi::Hint(u8"天賦樹資料太舊（缺職業起點），請更新天賦樹", w, Tok::Warning);
			ImGui::Dummy(ImVec2(0, PobUi::D(8.0f)));
			return;
		}
		loadClassNames();
		const float sw = std::floor(PobUi::D(170.0f));
		std::vector<const PassiveTreeData::ClassStart*> cls;
		for (const auto& cs : ptData.classStarts) cls.push_back(&cs);
		std::sort(cls.begin(), cls.end(), [](auto* a, auto* b) { return a->classId < b->classId; });
		std::vector<std::string> cl;
		int sel = -1;
		for (size_t i = 0; i < cls.size(); i++) {
			cl.push_back(zhOr(classZh_, cls[i]->name));
			if (cls[i]->classId == classId_) sel = (int)i;
		}
		std::vector<const char*> cp;
		for (const auto& l : cl) cp.push_back(l.c_str());
		labelRow(u8"職業", u8"重奪惡意的路徑＝插槽到這個職業起點的最短路徑（整棵樹上最短的一條）",
		         sw, H, [&]() {
			if (PobUi::Select("##class", &sel, cp.data(), nullptr, (int)cp.size(), sw) && sel >= 0 &&
			    cls[(size_t)sel]->classId != classId_) {
				classId_ = cls[(size_t)sel]->classId;
				asc_.clear(); protectAsc_.clear(); reqAscSel_ = 0;
				tjUi.classId = classId_; tjUi.asc = asc_; saveTjUi();
			}
		});
		if (sel < 0) classId_ = -1; // saved class not in this tree: ask again instead of blaming the socket
		if (classId_ < 0) {
			PobUi::Hint(u8"選一個職業：路徑取插槽到該職業起點的最短距離", w);
			ImGui::Dummy(ImVec2(0, PobUi::D(8.0f)));
			return;
		}
		std::vector<const PassiveTreeData::AscClass*> ascs;
		for (const auto& a : ptData.ascClasses) if (a.classId == classId_) ascs.push_back(&a);
		std::vector<std::string> al{ u8"不計算昇華" };
		int asel = 0;
		for (size_t i = 0; i < ascs.size(); i++) {
			al.push_back(zhOr(ascZh_, ascs[i]->id, ascs[i]->name));
			if (ascs[i]->id == asc_) asel = (int)i + 1;
		}
		std::vector<const char*> ap;
		for (const auto& l : al) ap.push_back(l.c_str());
		labelRow(u8"昇華", u8"重奪惡意也會依種子改掉這個昇華的一個昇華天賦", sw, H, [&]() {
			if (PobUi::Select("##asc", &asel, ap.data(), nullptr, (int)ap.size(), sw)) {
				const std::string a = asel > 0 ? ascs[(size_t)asel - 1]->id : std::string();
				if (a != asc_) { asc_ = a; protectAsc_.clear(); reqAscSel_ = 0; tjUi.asc = asc_; saveTjUi(); }
			}
		});
		PobUi::Hint(u8"實際配點繞遠路、或同樣短的路線不只一條時，路徑上的天賦會不同", w);
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
		if (!ascClass().empty()) {
			{
				std::vector<std::string> lbl{ u8"不限" };
				for (const auto& o : ascOutcomes_) lbl.push_back(o.second);
				std::vector<const char*> lp;
				for (const auto& l : lbl) lp.push_back(l.c_str());
				labelRow(u8"必要昇華結果", u8"同 POB：昇華天賦必須變成這一個，其餘種子不列出",
				         sw, H, [&]() { PobUi::Select("##reqasc", &reqAscSel_, lp.data(), nullptr, (int)lp.size(), sw); });
				if (!ascTargets_.empty()) {
					PobUi::Hint(u8"保護昇華天賦（勾選的天賦被改掉的種子不列出）：", w);
					for (int id : ascTargets_) {
						bool on = protectAsc_.count(id) != 0;
						ImGui::PushID(id);
						if (ImGui::Checkbox(ascName(id).c_str(), &on)) {
							if (on) protectAsc_.insert(id); else protectAsc_.erase(id);
						}
						ImGui::PopID();
					}
				}
			}
		}
		ImGui::Dummy(ImVec2(0, PobUi::D(8.0f)));
	}

	// left: find seeds
	void drawSearchForm()
	{
		const bool busy = job.running.load();
		bool noSocket = false;
		const std::string hint = socketHint(&noSocket);
		const float w = ImGui::GetContentRegionAvail().x;
		// the bottom block (socket hint + the search button) is pinned; measure it
		const std::string ksNote = keystoneNote();
		const float hintH = hintBlockH(hint, ksNote, w);
		const float bottomH = hintH + PobUi::D(6.0f) + std::floor(PobUi::D(44.0f));
		const float scrollH = ImGui::GetContentRegionAvail().y - bottomH - PobUi::D(12.0f);

		const float formH = (std::max)(scrollH, PobUi::D(80.0f));
		ImGui::BeginChild("##formscroll", ImVec2(0, formH), false);
		const float formTop = ImGui::GetCursorScreenPos().y;
		drawAbyssBanner();
		drawBuildSection();
		PobUi::Heading(u8"想要的詞綴");
		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));

		// add a stat: the whole pool is always listed; typing filters it
		const float fw = ImGui::GetContentRegionAvail().x;
		PobUi::SearchField("##statfilter", statFilter_, (int)sizeof(statFilter_), u8"輸入關鍵字篩選詞綴…", fw);
		const std::string filter = statFilter_;
		std::vector<const TJStatTemplate*> shownT;
		shownT.reserve(templates.size());
		for (const auto& t : templates) {
			const std::string& disp = t.zh.empty() ? t.en : t.zh;
			if (contains_ci(disp, filter) || contains_ci(t.en, filter)) shownT.push_back(&t);
		}
		{
			const std::string cnt = filter.empty()
				? u8"全部 " + std::to_string(templates.size()) + u8" 條，點選加入"
				: u8"符合 " + std::to_string(shownT.size()) + u8" / " + std::to_string(templates.size()) + u8" 條";
			PobUi::Hint(cnt.c_str());
		}
		// The list takes whatever height the rest of the form leaves (measured on
		// the previous frame), but never less than a few rows: below that the
		// whole form scrolls instead.
		const float rowH = ImGui::GetTextLineHeightWithSpacing();
		const float listMin = std::floor(rowH * 5.0f + PobUi::D(12.0f));
		const float listH = std::floor((std::max)(listMin, formH - formOtherH_ - PobUi::D(4.0f)));
		const float listTop = ImGui::GetCursorScreenPos().y;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, PobUi::TokV4(Tok::Surface2));
		ImGui::PushStyleColor(ImGuiCol_Border, PobUi::TokV4(Tok::Border));
		ImGui::BeginChild("##statpick", ImVec2(0, listH), true);
		if (shownT.empty())
			PobUi::Hint((u8"沒有符合「" + filter + u8"」的詞綴").c_str(), ImGui::GetContentRegionAvail().x);
		ImGuiListClipper clip;
		clip.Begin((int)shownT.size(), rowH);
		while (clip.Step()) {
			for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
				const TJStatTemplate& t = *shownT[i];
				const std::string& disp = t.zh.empty() ? t.en : t.zh;
				bool exists = false;
				for (auto& wr : wants) if (wr.en == t.en) exists = true;
				ImGui::PushID(t.en.c_str());
				ImGui::BeginDisabled(exists);
				if (ImGui::Selectable(disp.c_str(), exists)) {
					wants.push_back({ t.en, disp, 0.0f, 1.0f });
					statFilter_[0] = '\0';   // added: clear the box for the next one
				}
				ImGui::EndDisabled();
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && exists)
					PobUi::Tooltip(u8"已在下方的詞綴表中");
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
		ImGui::PopStyleColor(2);
		const float listBottom = ImGui::GetCursorScreenPos().y;
		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));

		// the picked stats: 詞綴 / 最小值 / 權重 / 移除
		if (wants.empty()) {
			const bool reqAsc = TJIsZorath(jewelType) && reqAscSel_ > 0; // required outcome still scores by its roll
			PobUi::Hint(reqAsc ? u8"還沒有詞綴：在上方輸入關鍵字，從清單點選加入。"
			                   : u8"還沒有詞綴：在上方輸入關鍵字，從清單點選加入。不加詞綴直接搜尋會依編號列出全部種子（最小總權重與範圍不作用）。",
			            ImGui::GetContentRegionAvail().x);
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
		// Abyss: keystones never score (PoB scoreNode), so "big" is notables (and Zorath's Ascendancy pick).
		const char* scopeLbl[2] = { TJIsAbyss(jewelType) ? u8"只看大天賦" : u8"只看大點", u8"全部節點" };
		int scopeSel = scope == 1 ? 0 : 1;
		labelRow(u8"範圍", nullptr, PobUi::SegmentedWidth(scopeLbl, 2), H, [&]() {
			if (PobUi::Segmented("##scope", &scopeSel, scopeLbl, 2)) scope = scopeSel == 0 ? 1 : 0;
		});
		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		labelRow(u8"必須包含全部詞綴",
		         u8"開啟時，只滿足其中一條的種子不會出現。關閉後只要命中任一條就算，權重高的仍排前面。",
		         PobUi::SwitchWidth(), H, [&]() { PobUi::Switch("##requireall", &requireAll); });
		// everything in the form except the list, for next frame's list height
		formOtherH_ = (ImGui::GetCursorScreenPos().y - formTop) - (listBottom - listTop);
		ImGui::EndChild(); // ##formscroll

		// pinned: where the jewel sits + the one primary action
		ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x,
		                                 ImGui::GetWindowPos().y + ImGui::GetWindowHeight() -
		                                     ImGui::GetStyle().WindowPadding.y - bottomH));
		drawHintBlock(hint, ksNote, w, noSocket);
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f) - ImGui::GetStyle().ItemSpacing.y));
		const std::string abyssWhy = abyssBlocker();
		const bool canSearch = !busy && selSocket >= 0 && abyssWhy.empty();
		if (PobUi::Button(busy ? u8"搜尋中…" : u8"搜尋這個插槽", PobUi::BtnKind::Primary, PobUi::BtnSize::Lg, nullptr, w,
		                  canSearch))
			runSearch();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canSearch && !busy)
			PobUi::Tooltip(selSocket < 0 ? u8"先在天賦樹上點一個珠寶插槽" : abyssWhy.c_str());
	}

	void runSearch()
	{
		if (job.running.load() || selSocket < 0) return;
		if (!ensureBin(jewelType)) {
			showError(u8"讀不到這顆珠寶的查表資料", binErr, false);
			return;
		}
		updateAbyssDerived();
		if (!abyssBlocker().empty()) return;
		tradeNext_.clear();   // every search starts its groups at the first batch
		const bool isAbyss = TJIsAbyss(jewelType);
		const TJSearchQuery q = makeQuery();
		searchNoWants_ = q.wants.empty() && q.requiredAscOutcome.empty(); // nothing scored: listed by seed number
		detailSeed = -1;
		searchProtected_ = !q.protectIds.empty();
		jobReported_ = false;
		if (isAbyss)
			job.startAbyss(ds.get(), blob, *abyssLut, q, ptData.nodes[selSocket].id, ptKind);
		else
			job.start(ds.get(), blob, q);
	}

	// left: look up a known seed
	void drawSeedForm()
	{
		drawAbyssBanner();
		if (TJIsZorath(jewelType)) drawBuildSection(); // the path decides what the seed changes
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
		const std::string ksNote = keystoneNote();
		const float hintH = hintBlockH(hint, ksNote, w);
		const float y = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - hintH;
		if (y > ImGui::GetCursorScreenPos().y) ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y));
		drawHintBlock(hint, ksNote, w, noSocket);
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
		const int seed = atoi(s.c_str());
		// Out of range is a wrong seed (a misread paste, a typo), not "no change":
		// say so instead of listing whatever the table happens to hold there.
		{
			const auto lo = ds->seedMin.find(jewelType), hi = ds->seedMax.find(jewelType);
			const int mul = jewelType == 5 ? 20 : 1;   // Elegant Hubris: item seed = table seed x 20
			if (lo != ds->seedMin.end() && hi != ds->seedMax.end() &&
			    (seed < lo->second * mul || seed > hi->second * mul || seed % mul != 0)) {
				seedErr_ = u8"種子 " + std::to_string(seed) + u8" 不在" + JewelZhShort(jewelType) + u8"的範圍（" +
				           std::to_string(lo->second * mul) + "-" + std::to_string(hi->second * mul) +
				           (mul > 1 ? u8"，且須是 20 的倍數" : "") + u8"）內，請確認珠寶上的數字。";
				detailSeed = -1;
				return;
			}
		}
		seedErr_.clear();
		detailSeed = seed;
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
		// POB needs one conqueror on the item: under "任意" the first one (the seed's mods do not depend on it)
		std::string t = TJItemText(*ds, jewelType, conqAny_ ? 0 : conquerorSel, seed);
		if (!t.empty() && WriteClipboardUtf8(nullptr, t))
			PobUi::ShowToast(conqAny_ ? u8"已複製物品文字（征服者用第一位，鑰石以實際珠寶為準），可在 POB 的物品欄貼上 (Ctrl+V)"
			                          : u8"已複製物品文字，可在 POB 的物品欄貼上 (Ctrl+V)", PobUi::Tone::Ok);
		else
			showError(u8"複製失敗", u8"無法寫入剪貼簿，請再試一次。", false);
	}

	bool tradeOff() const { return tradeStatId_.empty() || tradeLeague.empty(); }
	const char* tradeOffWhy() const
	{
		return tradeStatId_.empty() ? u8"這個征服者沒有交易站詞綴" : u8"先在結果欄上方的「交易站」設定聯盟";
	}

	// middle: the tree
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
			const bool canPick = selSocket >= 0 && treeRadius;
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
				else if (zorath) st += u8"｜種子 " + std::to_string(detailSeed) + u8"｜亮起的是插槽到職業起點的最短路徑";
				else if (treeAbyss) st += u8"｜種子 " + std::to_string(detailSeed) + u8"｜此插槽被征服的天賦以金框標示";
				else st += u8"｜種子 " + std::to_string(detailSeed) + u8"｜半徑內受影響節點以金框標示";
			}
			const float textW = (std::max)(PobUi::D(60.0f), w - pickW - PobUi::D(40.0f));
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
		                (long long)jewelType * 13 + conquerorSel * 131 + selVersion + (long long)abyssVer_ * 7919;
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
		// Zorath has no radius: what it judges is the build's path to the class
		// start, shown in the "selected" style instead of a ring.
		tin.radiusWorld = treeRadius ? 1800.0f : 0.0f;
		tin.hi = &dispHi;
		tin.selected = treeAbyss ? (pathMask_.size() == ptData.nodes.size() ? &pathMask_ : nullptr)
		                      : ptSelected.empty() ? nullptr : &ptSelected;
		tin.protectedMark = (canProtect() && !ptProtected.empty()) ? &ptProtected : nullptr;
		tin.emphasize = emphNode;
		tin.showAsc = treeAbyss;
		tin.ascName = ascClass();
		tin.ascHit = &ascHit_;
		const bool pickable = treeRadius;
		tin.overlay = [this, pickable](ImDrawList* dl, ImVec2 mn, ImVec2 mx) { drawLegend(dl, mn, mx, pickable); };
		// Test aid (POBTOOLS_TJ_STATE=protectclick): pin a notable under the canvas
		// centre, right-click it through ImGui's own input queue (no OS input), then
		// write what the click changed to POBTOOLS_TJ_CLICK_DUMP.
		if (testClickNode_ >= 0) ptView.SetCamera(0.3f, ptData.nodes[testClickNode_].x, ptData.nodes[testClickNode_].y);
		if (testCam_.x > 0) { ptView.SetCamera(testCam_.x, testCam_.y, testCam_.z); centerPending_ = false; }
		PassiveTreeOutput tout = ptView.Draw(ptData, scale, tin);
		if (testClickNode_ >= 0 && testClickFrame_ >= 0) {
			if (testClickFrame_ == 2) {
				ImGuiIO& io = ImGui::GetIO();
				io.AddMousePosEvent((tout.canvasMin.x + tout.canvasMax.x) * 0.5f, (tout.canvasMin.y + tout.canvasMax.y) * 0.5f);
				io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
				io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			}
			if (++testClickFrame_ > 12) {
				testClickFrame_ = -1;
				wchar_t path[MAX_PATH] = L"";
				const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_TJ_CLICK_DUMP", path, MAX_PATH);
				HANDLE f = (n > 0 && n < MAX_PATH) ? CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
				                                                 FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
				if (f != INVALID_HANDLE_VALUE) {
					const std::string out = "selected=" + std::to_string(std::count(ptSelected.begin(), ptSelected.end(), 1)) +
					                        " protected=" + std::to_string(std::count(ptProtected.begin(), ptProtected.end(), 1)) + "\n";
					DWORD wr = 0;
					WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr);
					CloseHandle(f);
				}
			}
		}

		if (centerPending_ && selSocket >= 0) { ptView.CenterOn(ptData, selSocket); centerPending_ = false; }
		if (panToNode >= 0) { ptView.CenterOn(ptData, panToNode); panToNode = -1; }

		if (tout.clickedSocket >= 0 && tout.clickedSocket != selSocket) {
			selSocket = tout.clickedSocket;
			ptProtected.clear(); // a mark the new socket cannot reach would still claim to filter
			hiSig = -1; // force recompute next frame
			ptView.CenterOn(ptData, selSocket);
		}
		// click a non-socket node to add/remove it from the picked focus set
		if (tout.clickedNode >= 0) {
			if (ptSelected.size() != ptData.nodes.size()) ptSelected.assign(ptData.nodes.size(), 0);
			ptSelected[tout.clickedNode] = ptSelected[tout.clickedNode] ? 0 : 1;
			selVersion++;
		}
		// right-click a notable in radius to protect it from being replaced
		if (tout.rightClickedNode >= 0 && canProtect() && selSocket >= 0 &&
		    ptData.nodes[tout.rightClickedNode].kind == kPtNotable) {
			const std::vector<int> inRad = ptData.NodesInRadius(selSocket, 1800.0f);
			if (std::find(inRad.begin(), inRad.end(), tout.rightClickedNode) != inRad.end()) {
				if (ptProtected.size() != ptData.nodes.size()) ptProtected.assign(ptData.nodes.size(), 0);
				ptProtected[tout.rightClickedNode] = ptProtected[tout.rightClickedNode] ? 0 : 1;
			}
		}
		if (tout.hoveredNode >= 0) drawNodeTooltip(tout.hoveredNode);
		else if (tout.hoveredAsc >= 0) drawAscTooltip(tout.hoveredAsc);
		else if (testHoverNode_ >= 0) { drawNodeTooltip(testHoverNode_); dumpKeystoneOnce(); } // POBTOOLS_TJ_STATE=keystone
	}

	// Test aid (POBTOOLS_TJ_STATE=keystone): the socket note and the hovered keystone's lines go to POBTOOLS_TJ_KS_DUMP.
	void dumpKeystoneOnce()
	{
		if (testKsDumped_ || hiSig == -1) return;
		testKsDumped_ = true;
		wchar_t path[MAX_PATH] = L"";
		const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_TJ_KS_DUMP", path, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) return;
		std::string out = "note=" + keystoneNote() + "\n";
		const auto it = ptTrans.find(testHoverNode_);
		out += std::string("replaced=") + (it != ptTrans.end() && it->second.replaced ? "1" : "0") + "\n";
		if (it != ptTrans.end()) {
			out += "name=" + it->second.newNameZh + "\n";
			for (const auto& l : it->second.linesZh) out += "line=" + l + "\n";
		}
		HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (f == INVALID_HANDLE_VALUE) return;
		DWORD wr = 0;
		WriteFile(f, out.data(), (DWORD)out.size(), &wr, nullptr);
		CloseHandle(f);
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
		const char* clL = u8"星團珠寶格";
		const char* pickL = canProtect() ? u8"左鍵點節點可單獨查看｜右鍵大點＝保護不被替換" : u8"點一般節點可單獨查看";
		if (TJIsZorath(jewelType) && !zorathPath_.empty()) {   // the lit nodes/links are Zorath's path
			pickable = true;
			pickL = u8"亮起＝插槽到職業起點的最短路徑";
		}
		float w = padX * 2.0f + sw + PobUi::D(6.0f) + f->CalcTextSizeA(px, FLT_MAX, 0, hitL).x + gap + sw +
		          PobUi::D(6.0f) + f->CalcTextSizeA(px, FLT_MAX, 0, sockL).x +
		          gap + sw + PobUi::D(6.0f) + f->CalcTextSizeA(px, FLT_MAX, 0, clL).x;
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
		dl->AddCircle(ImVec2(x + sw * 0.5f, cy), sw * 0.5f, Tok::FxPurple, 16, 2.0f);
		x += sw + PobUi::D(6.0f);
		dl->AddText(f, px, ImVec2(x, p.y + padY), Tok::TextMuted, clL);
		x += f->CalcTextSizeA(px, FLT_MAX, 0, clL).x + gap;
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
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::floor(16 * scale), std::floor(12 * scale))); // whole px, see AtlasView::drawTooltip
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
			if (t.replaced) {
				const std::string& was = n.nameZh.empty() ? n.name : n.nameZh;
				PobUi::Hint((u8"（節點被替換，原本是「" + was + u8"」）").c_str());
			} else {
				PobUi::Hint(u8"（保留原詞綴，珠寶額外加成）");
			}
			ImGui::Separator();
			// An addition leaves the node itself intact, so its own stats still
			// apply and must be shown above what the jewel adds.
			if (!t.replaced)
				for (const std::string& s : PtDisplayStats(n)) ImGui::TextColored(text, "%s", s.c_str());
			for (size_t i = 0; i < t.lines.size(); i++) {
				const std::string& zh = (i < t.linesZh.size() && !t.linesZh[i].empty()) ? t.linesZh[i] : t.lines[i];
				ImGui::TextColored(PobUi::TokV4(Tok::Success), "%s", zh.c_str());
			}
		} else {
			const std::string& nm = n.nameZh.empty() ? n.name : n.nameZh;
			ImGui::TextColored(PobUi::TokV4(PobUi::TreeKindColor(kindOf(n.kind))), "%s", nm.empty() ? "?" : nm.c_str());
			const std::vector<std::string> lines = PtDisplayStats(n);
			if (!lines.empty()) ImGui::Separator();
			for (const std::string& s : lines) ImGui::TextColored(text, "%s", s.c_str());
			if (n.cluster) PobUi::Hint(u8"大型星團珠寶格（也可放深淵／永恆珠寶）");
		}
		if (canProtect() && nodeIdx < (int)ptProtected.size() && ptProtected[nodeIdx]) {
			ImGui::Separator();
			ImGui::TextColored(PobUi::TokV4(Tok::Success), "%s", u8"已保護：搜尋會排除替換這個大點的種子（右鍵取消）");
		}
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
		ImGui::PopStyleVar();
	}

	void drawAscTooltip(int ascIdx)
	{
		const PtNode& n = ptData.ascNodes[ascIdx];
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(std::floor(16 * scale), std::floor(12 * scale)));
		ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(440 * scale, FLT_MAX));
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(400 * scale);
		const bool hit = ascHit_.count(n.id) != 0;
		const std::string& nm = n.nameZh.empty() ? n.name : n.nameZh;
		ImGui::TextColored(PobUi::TokV4(hit ? Tok::TreeHit : PobUi::TreeKindColor(kindOf(n.kind))), "%s", nm.c_str());
		if (hit) PobUi::Hint(u8"（這個種子會改掉這個昇華天賦，詳見右側「昇華天賦選擇」）");
		const std::vector<std::string> lines = PtDisplayStats(n);
		if (!lines.empty()) ImGui::Separator();
		for (const std::string& s : lines) ImGui::TextColored(PobUi::TokV4(Tok::Text), "%s", s.c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
		ImGui::PopStyleVar();
	}

	// The tuple (socket, seed, jewel, conqueror, picks) changed: transform every
	// candidate node once and rebuild the stat groups the side list shows.
	void recomputeHighlights(bool treeRadius)
	{
		// The header (drawn earlier this frame) may just have changed the jewel or
		// the conqueror, after Frame() derived conquerorId_ from the old pick.
		// Re-derive it here so this recompute uses the pick that triggered it.
		{
			const std::vector<TJConqueror>* conqs = conqListFor(jewelType);
			const int conqN = conqs ? (int)conqs->size() : 0;
			deriveConqueror(conqs, conqN);
		}
		ptHi.assign(ptData.nodes.size(), kPtHiNone);
		ptTrans.clear();
		ascHit_.clear();
		statGroups.clear();
		emphNode = -1; hlStatGroup = -1;
		const bool abyssDetail = TJIsAbyss(jewelType);
		if (selSocket < 0 || !(treeRadius || abyssDetail)) return;
		const bool haveBin = detailSeed >= 0 && ensureBin(jewelType);

		// Legion computes its candidates from the socket; Abyss is TOLD which passives it took (scattered over the tree).
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
			// Legion: the socket's neighbourhood IS the affected set; Zorath: the shortest path to the chosen class start.
			if (zorathDetail) {
				for (int id : zorathPath_) {
					const int idx = ptData.IndexOfId(id);
					if (idx >= 0 && ptData.nodes[idx].kind != kPtSocket) cand.push_back(idx);
				}
			} else {
				cand = ptData.NodesInRadius(selSocket, 1800.0f);
			}
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
				// every Ascendancy passive the seed takes: the same read as the 昇華天賦選擇 section (no query, so
				// protection / required picks never hide one)
				std::map<std::string, std::vector<int>> picks;
				if (TJAbyssReadAscendancies(*blob, *abyssLut, detailSeed, picks))
					for (const auto& kv : picks)
						// the chosen Ascendancy only (all of them when none is chosen); both are disc ids
						if (ascClass().empty() || kv.first == ascClass()) ascHit_.insert(kv.second.begin(), kv.second.end());
			}
		}

		bool anySel = false;
		for (int idx : cand)
			if (!abyssDetail && idx < (int)ptSelected.size() && ptSelected[idx]) { anySel = true; break; }
		auto included = [&](int idx) { return !anySel || (idx < (int)ptSelected.size() && ptSelected[idx]); };
		for (int idx : cand) {
			const PtNode& n = ptData.nodes[idx];
			if (detailSeed < 0 || !haveBin) {
				// A Legion keystone turns into the conqueror's keystone whatever
				// the seed, and TJApply answers that without the lookup table —
				// so preview it now and let the conqueror picker change it.
				if (!abyssDetail && n.kind == kPtKeystone) {
					static const std::string kNoBlob;
					TJTransform t = applyNode(kNoBlob, -1, n, "Keystone");
					if (t.ok && t.replaced) {
						ptHi[idx] = kPtHiReplaced;
						ptTrans[idx] = std::move(t);
						continue;
					}
				}
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
				t = applyNode(*blob, detailSeed, n, nt);
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

	// tables
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

	// POB cross-check reminder at the right-most column's bottom; shown only after a trade search was opened.
	// Returns the height to keep free for it.
	float pobTipReserve() const
	{
		return (tradeHintShown && !tradeHintClosed_) ? infoBannerH_ + PobUi::D(8.0f) : 0.0f;
	}
	void drawPobTip(float bottom)
	{
		if (!(tradeHintShown && !tradeHintClosed_)) return;
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

	// right: the trade row, then the results at full height
	void drawResultsColumn()
	{
		const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y;
		drawTradeRow();
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
		drawResults(bottom - pobTipReserve());
		drawPobTip(bottom);
	}

	void drawResults(float bottom)
	{
		if (job.running.load()) {
			PobUi::Heading(u8"搜尋中…");
			PobUi::Hint(u8"正在掃描這顆珠寶的所有種子，通常幾秒內完成。", ImGui::GetContentRegionAvail().x);
			return;
		}
		if (job.results.empty()) {
			if (job.done.load() && searchNoWants_)
				PobUi::EmptyState("##noresult", PobIcon::Search, u8"沒有符合條件的種子",
				                  u8"每個種子都會改掉你保護的天賦。取消幾個保護再搜尋一次。");
			else if (job.done.load())
				PobUi::EmptyState("##noresult", PobIcon::Search, u8"沒有符合條件的種子",
				                  searchProtected_
				                      ? u8"符合詞綴的種子都會替換掉你保護的大點。在樹上右鍵取消幾個保護，或放寬詞綴條件。"
				                      : u8"降低最小值或最小總權重、關掉「必須包含全部詞綴」，或把範圍改成全部節點。");
			else
				PobUi::EmptyState("##noresult", PobIcon::Search, u8"還沒有搜尋結果",
				                  u8"在樹上選好插槽，再按「搜尋這個插槽」。左側加入想要的詞綴會依權重排序；不加詞綴就列出全部種子。");
			return;
		}

		// head: count + how it is sorted, then the grouping switch
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			PobUi::Heading((u8"找到 " + std::to_string(job.results.size()) + u8" 個種子").c_str());
			const float lineH = ImGui::GetItemRectSize().y;
			const char* sortHint = searchNoWants_ ? u8"依種子編號排序" : u8"依權重排序";
			const ImFont* sf = PobUi::Fonts().small ? PobUi::Fonts().small : ImGui::GetFont();
			const float hw = ((ImFont*)sf)->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, sortHint).x;
			ImGui::SetCursorScreenPos(ImVec2(p.x + w - hw, p.y + std::floor((lineH - sf->FontSize) * 0.5f)));
			PobUi::Hint(sortHint);
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lineH));
			ImGui::Dummy(ImVec2(w, PobUi::D(4.0f)));
		}
		labelRow(u8"依命中節點數分組", u8"分組後每組標頭有「交易查詢整組」，把那一組的種子送到交易站（每次最多 40 個，超過的再按一次送下一批）",
		         PobUi::SwitchWidth(), std::floor(PobUi::D(28.0f)), [&]() { PobUi::Switch("##group", &groupResults); });
		ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		if (!groupResults) {
			// ungrouped: one search for the whole list, from the top
			std::vector<int> seeds;
			for (const auto& h : job.results) seeds.push_back(h.seed);
			drawTradeGroupButton(seeds, u8"交易查詢全部", "all", 0.0f);
			ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
		}
		const float listH = (std::max)(PobUi::D(120.0f), bottom - ImGui::GetCursorScreenPos().y);
		if (groupResults) drawGroupedResults(listH);
		else drawResultList(listH);
	}

	// A trade button for several seeds, with what happens past the cap said in
	// its tooltip (and in a hint beside it when the cap actually bites).
	// The button's label: past the cap it names the batch the next click sends
	// ("… 41-80"). Call under the same ID stack as drawTradeGroupButton.
	std::string tradeGroupLabel(const std::vector<int>& seeds, const char* base, const char* tag)
	{
		const size_t per = tradePerSearch();
		if (seeds.size() <= per) return base;
		const TradeBatch b = TradeNextBatch(seeds.size(), tradeNext_[ImGui::GetID(tag)], per);
		return std::string(base) + " " + std::to_string(b.begin + 1) + "-" + std::to_string(b.end);
	}

	void drawTradeGroupButton(const std::vector<int>& seeds, const char* base, const char* tag, float minW)
	{
		const size_t per = tradePerSearch();
		const bool capped = seeds.size() > per;
		size_t& next = tradeNext_[ImGui::GetID(tag)];
		const TradeBatch b = TradeNextBatch(seeds.size(), next, per);
		// the ID stays `tag` while the visible label moves on to the next batch
		const std::string label = tradeGroupLabel(seeds, base, tag) + "###" + tag;
		if (PobUi::Button(label.c_str(), PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::ExternalLink, minW, !tradeOff())) {
			tradeSeeds(std::vector<int>(seeds.begin() + b.begin, seeds.begin() + b.end), tag);
			next = b.end;
		}
		// Test aid (POBTOOLS_TJ_STATE=tradeclick): synthetic left clicks on the first
		// trade button that has more than one batch -- POBTOOLS_TJ_CLICKS of them
		// (default 1), 12 frames apart so each lands as its own click.
		if (testState_ == "tradeclick" && capped && (testTradeId_ == 0 || testTradeId_ == ImGui::GetItemID()) &&
		    testTradeClicks_ < testTradeWant_ && ImGui::GetFrameCount() >= testTradeAt_) {
			testTradeId_ = ImGui::GetItemID();
			testTradeClicks_++;
			testTradeAt_ = ImGui::GetFrameCount() + 12;
			const ImVec2 a = ImGui::GetItemRectMin(), z = ImGui::GetItemRectMax();
			ImGuiIO& io = ImGui::GetIO();
			io.AddMousePosEvent((a.x + z.x) * 0.5f, (a.y + z.y) * 0.5f);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
		}
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
			std::string tip;
			if (tradeOff()) tip = tradeOffWhy();
			else {
				tip = u8"一次在交易站搜尋 " + std::to_string(b.end - b.begin) + u8" 個種子（任一符合即列出，即時購買）。";
				if (conqAny_) tip += u8"征服者選「任意」：每個種子對每位征服者各佔一條條件。";
				if (capped)
					tip += u8"交易站一次查詢最多 " + std::to_string(per) + u8" 個種子，這裡共 " +
					       std::to_string(seeds.size()) + u8" 個：這次送排名第 " + std::to_string(b.begin + 1) + "-" +
					       std::to_string(b.end) + u8" 個，再按一次送下一批，送完最後一批會從頭開始。";
			}
			PobUi::Tooltip(tip.c_str());
		}
	}

	// result list: one block per seed, showing what it does for the picked stats only.
	// Same stat on several nodes is merged: identical line "x N"; single-number line with different
	// rolls shows the sum + node count; a multi-number line is never summed (no honest total).

	struct SeedLine {
		std::string pre, num, post;   // num = the summed value (empty = none)
		std::string note;             // muted tail: "x6" / "3 個節點合計"
		std::string colorKey;
		bool big = false;             // a notable / keystone contributed
	};
	struct SeedSummary { std::vector<SeedLine> lines; };

	static int countNumbers(const std::string& s)
	{
		int n = 0;
		for (size_t i = 0; i < s.size(); i++)
			if (isdigit((unsigned char)s[i])) {
				n++;
				while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.')) i++;
			}
		return n;
	}
	static std::string fmtSum(double v)
	{
		char b[32];
		if (std::fabs(v - std::round(v)) < 1e-6) snprintf(b, sizeof(b), "%.0f", v);
		else {
			snprintf(b, sizeof(b), "%.2f", v);
			std::string s = b;
			while (!s.empty() && s.back() == '0') s.pop_back();
			if (!s.empty() && s.back() == '.') s.pop_back();
			return s;
		}
		return b;
	}

	// The nodes the result list judges a seed on: the socket's in-radius nodes
	// (the picked subset when any are picked) -- the same set the search used.
	// Zorath is judged on that neighbourhood too; 7-10 get theirs from the file.
	std::vector<int> resultNodes() const
	{
		std::vector<int> inRad;
		if (selSocket >= 0 && !TJIsAbyss(jewelType)) {
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
		return inRad;
	}

	std::string summarySig() const
	{
		std::string s = std::to_string(jewelType) + "|" + conquerorId_ + "|" + std::to_string(selSocket) + "|" +
		                std::to_string(scope) + "|" + std::to_string(job.results.size()) + "|" +
		                (job.results.empty() ? std::string() : std::to_string(job.results[0].seed)) + "|";
		for (const auto& w : wants) {
			char b[64];
			snprintf(b, sizeof(b), "|%.3f|%.3f|", w.minValue, w.weight);
			s += w.en + b;
		}
		unsigned h = 2166136261u;
		for (size_t i = 0; i < ptSelected.size(); i++) h = (h ^ (ptSelected[i] ? 1u : 0u) ^ (unsigned)i) * 16777619u;
		s += std::to_string(h);
		// abyss-only inputs: keep the eternal (1-6) key exactly as it was
		if (TJIsAbyss(jewelType)) {
			s += "|" + std::to_string(abyssVer_) + "|" + std::to_string(reqAscSel_) + "|";
			for (int id : protectAsc_) s += std::to_string(id) + ",";
		}
		return s;
	}

	SeedSummary buildSummary(int seed)
	{
		SeedSummary out;
		const TJWantMatcher matcher = makeMatcher();
		struct Acc { std::vector<std::string> zh; std::vector<double> val; bool single = true; bool big = false; };
		std::vector<std::pair<const TJWantStat*, Acc>> acc;   // first-seen order
		auto take = [&](const TJTransform& t, bool big) {
			for (size_t i = 0; i < t.lines.size(); i++) {
				const TJWantStat* w = matcher.Match(t.lines[i]);
				if (!w) continue;   // below 最小值 counts for nothing, so it shows as nothing
				const std::string& zh = (i < t.linesZh.size() && !t.linesZh[i].empty()) ? t.linesZh[i] : t.lines[i];
				Acc* a = nullptr;
				for (auto& kv : acc) if (kv.first == w) { a = &kv.second; break; }
				if (!a) { acc.push_back({ w, Acc() }); a = &acc.back().second; }
				a->zh.push_back(zh);
				a->val.push_back(TJStatValue(t.lines[i]));
				if (countNumbers(t.lines[i]) != 1 || countNumbers(zh) != 1) a->single = false;
				a->big = a->big || big;
			}
		};
		auto isBig = [](int kind) { return kind == kPtNotable || kind == kPtKeystone; };
		if (TJIsAbyss(jewelType)) {
			// exactly what the search scored: same query, same per-seed scorer
			std::vector<TJAbyssScored> det;
			TJSeedHit hit;
			if (selSocket >= 0 && abyssBlocker().empty() &&
			    TJAbyssScoreSeed(*ds, *blob, *abyssLut, makeQuery(), ptData.nodes[selSocket].id, seed, &ptKind, hit, &det))
				for (const TJAbyssScored& d : det) {
					if (!d.scored || !d.t.ok) continue;
					const auto k = ptKind.find(d.nodeId);
					take(d.t, d.ascendancy || (k != ptKind.end() && isBig(k->second)));
				}
		} else {
			for (int idx : resultNodesCache_) {
				const PtNode& n = ptData.nodes[idx];
				const char* nt = n.kind == kPtKeystone ? "Keystone" : n.kind == kPtNotable ? "Notable" : "Normal";
				TJTransform t = applyNode(*blob, seed, n, nt);
				if (t.ok) take(t, isBig(n.kind));
			}
		}
		for (auto& kv : acc) {
			Acc& a = kv.second;
			SeedLine L;
			L.big = a.big;
			L.colorKey = a.zh.front();
			const int n = (int)a.zh.size();
			bool same = true;
			for (const auto& z : a.zh) if (z != a.zh.front()) { same = false; break; }
			if (same) {
				L.pre = a.zh.front();
				if (n > 1) L.note = u8"×" + std::to_string(n);
			} else if (a.single) {
				const std::string& z = a.zh.front();
				size_t p = 0;
				while (p < z.size() && !isdigit((unsigned char)z[p])) p++;
				size_t e = p;
				while (e < z.size() && (isdigit((unsigned char)z[e]) || z[e] == '.')) e++;
				double sum = 0;
				for (double v : a.val) sum += v;
				L.pre = z.substr(0, p);
				L.num = fmtSum(sum);
				L.post = z.substr(e);
				L.note = std::to_string(n) + u8" 個節點合計";
			} else {
				// several numbers per line: no single total to show, so the template
				std::string zh;
				for (const auto& w : wants) if (TJNormalizeStat(w.en) == kv.first->tmpl || w.en == kv.first->tmpl) { zh = w.zh; break; }
				L.pre = zh.empty() ? a.zh.front() : zh;
				L.note = std::to_string(n) + u8" 個節點，數值不同";
			}
			out.lines.push_back(std::move(L));
		}
		return out;
	}

	const SeedSummary& summaryFor(int seed)
	{
		auto it = sumCache_.find(seed);
		if (it != sumCache_.end()) return it->second;
		return sumCache_.emplace(seed, buildSummary(seed)).first->second;
	}

	void refreshSummaryCache()
	{
		const std::string sig = summarySig();
		if (sig == sumSig_) return;
		sumSig_ = sig;
		sumCache_.clear();
		tradeNext_.clear();   // a new result list starts every group at its first batch
		resultNodesCache_ = resultNodes();
	}

	float blockHeaderH() const { return std::floor(PobUi::D(28.0f)); }
	float blockLineH() const { return std::floor(ImGui::GetTextLineHeight() + PobUi::D(3.0f)); }
	float blockH(int lines) const
	{
		return blockHeaderH() + (float)lines * blockLineH() + std::floor(PobUi::D(8.0f));
	}

	// One seed's block at the cursor, exactly blockH(lines) tall.
	void drawSeedBlock(const TJSeedHit& h, const SeedSummary& sum)
	{
		ImGui::PushID(h.seed);
		const float headH = blockHeaderH();
		const float lineH = blockLineH();
		const float total = blockH((int)sum.lines.size());
		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		const bool sel = detailSeed == h.seed;
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		ImFont* small = wf.small ? wf.small : ImGui::GetFont();
		ImFont* body = wf.body ? wf.body : ImGui::GetFont();

		ImGui::SetNextItemAllowOverlap();
		ImGui::PushStyleColor(ImGuiCol_Header, PobUi::TokV4(Tok::AccentSoft));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, PobUi::TokV4(Tok::Surface2));
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, PobUi::TokV4(Tok::AccentSoft));
		if (ImGui::Selectable("##blk", sel, 0, ImVec2(w, total - PobUi::D(2.0f)))) detailSeed = sel ? -1 : h.seed;
		ImGui::PopStyleColor(3);
		if (sel) dl->AddRectFilled(p, ImVec2(p.x + std::floor(PobUi::D(3.0f)), p.y + total - PobUi::D(2.0f)), Tok::Accent);

		const float padX = std::floor(PobUi::D(10.0f));
		char head[48];
		snprintf(head, sizeof(head), u8"種子 %d", h.seed);
		const float ty = p.y + std::floor((headH - ImGui::GetTextLineHeight()) * 0.5f);
		dl->AddText(body, body->FontSize, ImVec2(p.x + padX, ty), sel ? Tok::AccentText : Tok::Text, head);
		const float titleEnd = p.x + padX + body->CalcTextSizeA(body->FontSize, FLT_MAX, 0.0f, head).x;
		const int nw = (int)wants.size();
		char wmeta[48], cmeta[48];
		snprintf(wmeta, sizeof(wmeta), u8"權重 %.1f", h.weight);
		snprintf(cmeta, sizeof(cmeta), u8"詞綴 %d/%d", h.distinctWants, nw);
		if (searchNoWants_) wmeta[0] = cmeta[0] = 0; // nothing was scored
		const float metaW = small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, wmeta).x + PobUi::D(10.0f) +
		                    small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, cmeta).x;

		// actions on the right. In a narrow column 查看 goes first (clicking the
		// block does the same), so the numbers never run under a button.
		const float smH = std::floor(PobUi::D(28.0f));
		const float gapA = PobUi::D(4.0f);
		const float viewW = PobUi::ButtonWidth(u8"查看", PobUi::BtnSize::Sm) + gapA;
		const float baseW = PobUi::ButtonWidth(u8"交易", PobUi::BtnSize::Sm) + gapA + smH;
		const bool showView = titleEnd + PobUi::D(10.0f) + metaW + PobUi::D(10.0f) <= p.x + w - gapA - baseW - viewW;
		const float actW = baseW + (showView ? viewW : 0.0f);
		const float actLeft = p.x + w - actW - gapA;

		const float sy = p.y + std::floor((headH - small->FontSize) * 0.5f);
		dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(actLeft - PobUi::D(6.0f), p.y + headH), true);
		float x = titleEnd + PobUi::D(10.0f);
		dl->AddText(small, small->FontSize, ImVec2(x, sy), Tok::TextMuted, wmeta);
		x += small->CalcTextSizeA(small->FontSize, FLT_MAX, 0.0f, wmeta).x + PobUi::D(10.0f);
		dl->AddText(small, small->FontSize, ImVec2(x, sy), h.distinctWants < nw ? Tok::TextMuted : Tok::Text, cmeta);
		dl->PopClipRect();

		ImGui::SetCursorScreenPos(ImVec2(actLeft, p.y + std::floor((headH - smH) * 0.5f)));
		if (showView) {
			if (PobUi::Button(u8"查看", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) detailSeed = h.seed;
			if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"在天賦樹旁展開這個種子的全部變化，並在樹上標示");
			ImGui::SameLine(0.0f, gapA);
		}
		if (PobUi::Button(u8"交易", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, 0.0f, !tradeOff()))
			tradeSeed(h.seed);
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			PobUi::Tooltip(tradeOff() ? tradeOffWhy() : u8"在交易站搜尋這個種子（即時購買）");
		ImGui::SameLine(0.0f, PobUi::D(4.0f));
		if (PobUi::Button("##copy", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Copy, smH)) copyForPob(h.seed);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"複製給 POB：貼進 POB 的物品欄 (Ctrl+V)");

		// matched stats, one per wanted stat; gold dot = a notable / keystone gave it
		const float lx = p.x + padX + PobUi::D(14.0f);
		const float clipR = p.x + w - PobUi::D(6.0f);
		dl->PushClipRect(ImVec2(p.x, p.y), ImVec2(clipR, p.y + total), true);
		for (size_t i = 0; i < sum.lines.size(); i++) {
			const SeedLine& L = sum.lines[i];
			const float ly = p.y + headH + (float)i * lineH;
			const float cy = ly + std::floor(ImGui::GetTextLineHeight() * 0.5f);
			dl->AddCircleFilled(ImVec2(p.x + padX + PobUi::D(4.0f), cy), (std::max)(2.0f, PobUi::D(2.5f)),
			                    L.big ? Tok::TreeHit : Tok::TextFaint, 12);
			const ImU32 col = colorStats ? ImGui::ColorConvertFloat4ToU32(stat_color(L.colorKey)) : Tok::Text;
			float tx = lx;
			auto put = [&](ImFont* f, const std::string& s, ImU32 c) {
				if (s.empty()) return;
				dl->AddText(f, f->FontSize, ImVec2(tx, f == small ? ly + std::floor((ImGui::GetTextLineHeight() - small->FontSize) * 0.5f) : ly),
				            c, s.c_str());
				tx += f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, s.c_str()).x;
			};
			put(body, L.pre, col);
			put(body, L.num, Tok::TreeHit);
			put(body, L.post, col);
			if (!L.note.empty()) {
				tx += PobUi::D(8.0f);
				put(small, L.note, Tok::TextMuted);
			}
		}
		dl->PopClipRect();

		ImGui::SetCursorScreenPos(p);
		ImGui::Dummy(ImVec2(w, total));
		ImGui::PopID();
	}

	// Virtualised run of seed blocks: only blocks intersecting the visible area are built.
	void drawSeedList(const std::vector<const TJSeedHit*>& list)
	{
		const ImVec2 clipMin = ImGui::GetWindowDrawList()->GetClipRectMin();
		const ImVec2 clipMax = ImGui::GetWindowDrawList()->GetClipRectMax();
		float skipped = 0.0f;
		auto flush = [&]() {
			if (skipped > 0.0f) { ImGui::Dummy(ImVec2(1.0f, skipped)); skipped = 0.0f; }
		};
		const float gap = ImGui::GetStyle().ItemSpacing.y;
		for (const TJSeedHit* h : list) {
			auto it = sumCache_.find(h->seed);
			const int lines = it != sumCache_.end() ? (int)it->second.lines.size() : h->distinctWants;
			const float bh = blockH(lines) + gap;
			const float y = ImGui::GetCursorScreenPos().y + skipped;
			if (y + bh < clipMin.y || y > clipMax.y) { skipped += bh; continue; }
			flush();
			drawSeedBlock(*h, summaryFor(h->seed));
		}
		flush();
	}

	void drawResultList(float height)
	{
		refreshSummaryCache();
		std::vector<const TJSeedHit*> all;
		all.reserve(job.results.size());
		for (const auto& h : job.results) all.push_back(&h);
		ImGui::BeginChild("##resl", ImVec2(0, height), false);
		drawSeedList(all);
		ImGui::EndChild();
	}

	// Seeds grouped by how many nodes matched (desc), like Vilsol. Each group's
	// header carries its own trade button: the whole group in one search.
	void drawGroupedResults(float height)
	{
		refreshSummaryCache();
		std::map<int, std::vector<const TJSeedHit*>, std::greater<int>> groups;
		for (const auto& h : job.results) groups[h.matches].push_back(&h);

		ImGui::BeginChild("##resg", ImVec2(0, height), false);
		bool firstGroup = true;
		for (auto& kv : groups) {
			ImGui::PushID(kv.first);
			std::vector<int> seeds;
			for (auto* h : kv.second) seeds.push_back(h->seed);
			const bool open = groupHeader(kv.first, (int)kv.second.size(), seeds, firstGroup);
			firstGroup = false;
			if (open) {
				drawSeedList(kv.second);
				ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
			}
			ImGui::PopID();
		}
		ImGui::EndChild();
	}

	// "命中 N 個節點 · M 個種子" + a chevron that folds the group, and on the
	// right the group's trade button. Open state kept in ImGui storage, like
	// PobUi::CollapsingSection (which has no room for a button).
	bool groupHeader(int matches, int count, const std::vector<int>& seeds, bool defaultOpen)
	{
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		ImFont* body = wf.body ? wf.body : ImGui::GetFont();
		ImFont* small = wf.small ? wf.small : ImGui::GetFont();
		const ImGuiID gid = ImGui::GetID("##grpopen");
		ImGuiStorage* st = ImGui::GetStateStorage();
		bool open = st->GetInt(gid, defaultOpen ? 1 : 0) != 0;

		const ImVec2 p = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		const float h = std::floor(PobUi::D(36.0f));
		const float smH = std::floor(PobUi::D(28.0f));
		const char* tlbl = u8"交易查詢整組";
		const float bw = PobUi::ButtonWidth(tradeGroupLabel(seeds, tlbl, "group").c_str(), PobUi::BtnSize::Sm, PobIcon::ExternalLink);
		const float toggleW = (std::max)(PobUi::D(40.0f), w - bw - PobUi::D(8.0f));

		ImGui::SetNextItemAllowOverlap();
		if (ImGui::InvisibleButton("##grptoggle", ImVec2(toggleW, h))) {
			open = !open;
			st->SetInt(gid, open ? 1 : 0);
		}
		const bool hov = ImGui::IsItemHovered();
		ImDrawList* dl = ImGui::GetWindowDrawList();
		dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), hov ? Tok::Surface3 : Tok::Surface2, PobUi::D(5.0f));
		float x = p.x + PobUi::D(6.0f);
		const float px = body->FontSize;
		const float ty = p.y + std::floor((h - px) * 0.5f);
		if (wf.icons) {
			PobUi::IconAt(dl, ImVec2(x, ty), open ? PobIcon::ChevronDown : PobIcon::ChevronRight, Tok::TextMuted, px);
			x += PobUi::IconWidth(PobIcon::ChevronDown, px) + PobUi::D(4.0f);
		}
		char lbl[64], note[64];
		if (searchNoWants_ && matches == 0) snprintf(lbl, sizeof(lbl), u8"符合條件的種子");
		else snprintf(lbl, sizeof(lbl), u8"命中 %d 個節點", matches);
		// past the cap the note says how many go to the trade site per click (the
		// button names the batch, its tooltip says why)
		if (seeds.size() > tradePerSearch()) snprintf(note, sizeof(note), u8"%d 個種子・每次查 %d 個", count, (int)tradePerSearch());
		else snprintf(note, sizeof(note), u8"%d 個種子", count);
		dl->PushClipRect(p, ImVec2(p.x + toggleW, p.y + h), true);
		dl->AddText(body, px, ImVec2(x, ty), Tok::Text, lbl);
		x += body->CalcTextSizeA(px, FLT_MAX, 0.0f, lbl).x + PobUi::D(8.0f);
		dl->AddText(small, small->FontSize, ImVec2(x, p.y + std::floor((h - small->FontSize) * 0.5f)), Tok::TextMuted, note);
		dl->PopClipRect();
		if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

		ImGui::SetCursorScreenPos(ImVec2(p.x + w - bw - PobUi::D(4.0f), p.y + std::floor((h - smH) * 0.5f)));
		drawTradeGroupButton(seeds, tlbl, "group", 0.0f);
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + PobUi::D(4.0f)));
		ImGui::Dummy(ImVec2(w, 0.0f));
		return open;
	}

	// The chosen seed's column, between the tree and the results (or, on the
	// seed page, the right-most column -- which then carries the trade row).
	void drawDetailColumn(bool rightmost)
	{
		const float bottom = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y;
		if (rightmost) {
			drawTradeRow();
			ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
		}
		drawDetail(bottom - (rightmost ? pobTipReserve() : 0.0f));
		if (rightmost) drawPobTip(bottom);
	}

	// The chosen seed: title (+ fold), copy / trade, then what it changes.
	void drawDetail(float bottom)
	{
		const float availH = bottom - ImGui::GetCursorScreenPos().y;
		if (selSocket < 0) {
			// A seed that is already known (pasted / looked up) only needs a socket;
			// say that, with the seed, instead of a generic empty panel.
			const std::string t = detailSeed >= 0
				? u8"種子 " + std::to_string(detailSeed) + u8" 已載入，還沒選插槽" : std::string(u8"還沒選插槽");
			PobUi::EmptyState("##nosocket", PobIcon::Crosshair, t.c_str(),
			                  detailSeed >= 0 ? u8"在天賦樹上點一個珠寶插槽（你放這顆珠寶的位置），就會列出這個種子對插槽範圍內天賦的變化。"
			                                  : u8"在天賦樹上點一個珠寶插槽。",
			                  nullptr, 0.0f, availH);
			return;
		}
		if (detailSeed < 0) {
			PobUi::EmptyState("##noseed", PobIcon::Gem, u8"還沒選種子",
			                  mode == 0 ? u8"在右側的結果點一個種子，就會在這裡列出它的變化。"
			                            : u8"在左側輸入種子後按「查詢」，或從遊戲貼上珠寶。",
			                  nullptr, 0.0f, availH);
			return;
		}

		const float innerX = ImGui::GetCursorScreenPos().x;
		const float innerW = ImGui::GetContentRegionAvail().x;
		const PobUi::WidgetFonts& wf = PobUi::Fonts();
		const float smH = std::floor(PobUi::D(28.0f));

		// head: title left, a fold button right (search page only: on the seed
		// page this column is the page). "in radius" is only true for the Legion
		// jewels: the Abyss ones name their conquered passives in the file and
		// scatter them over the tree.
		char title[96];
		snprintf(title, sizeof(title),
		         TJIsZorath(jewelType) ? u8"種子 %d 改變的路徑天賦"
		         : TJIsAbyss(jewelType) ? u8"種子 %d 征服的天賦"
		                                : u8"種子 %d 的變化",
		         detailSeed);
		const char* copyLbl = u8"複製給 POB";
		const char* tradeLbl = u8"交易搜尋";
		const ImVec2 hp = ImGui::GetCursorScreenPos();
		const float lineH = (std::max)(smH, ImGui::GetTextLineHeight());
		const bool closable = mode == 0;
		const float closeW = closable ? smH + PobUi::D(4.0f) : 0.0f;
		ImGui::SetCursorScreenPos(ImVec2(hp.x, hp.y + std::floor((lineH - ImGui::GetTextLineHeight()) * 0.5f)));
		{
			const float tw = innerW - closeW;
			ImGui::PushClipRect(ImVec2(hp.x, hp.y), ImVec2(hp.x + tw, hp.y + lineH), true);
			PobUi::Heading(title);
			ImGui::PopClipRect();
		}
		if (closable) {
			ImGui::SetCursorScreenPos(ImVec2(innerX + innerW - smH, hp.y + std::floor((lineH - smH) * 0.5f)));
			if (PobUi::Button("##closedetail", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::X, smH))
				detailSeed = -1;
			if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"收起這一欄（再點結果中的種子可重新展開）");
		}
		const float by = hp.y + lineH + PobUi::D(6.0f);
		ImGui::SetCursorScreenPos(ImVec2(innerX, by));
		// Hand the jewel to PoB the way PoB expects to receive items: as the
		// game's own copy text on the clipboard.
		if (PobUi::Button(copyLbl, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Copy)) copyForPob(detailSeed);
		if (ImGui::IsItemHovered())
			PobUi::Tooltip(u8"複製成遊戲的物品文字格式，貼進 POB「物品」分頁即可建立這顆珠寶");
		ImGui::SameLine(0.0f, PobUi::D(4.0f));
		if (PobUi::Button(tradeLbl, PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, PobIcon::ExternalLink, 0.0f,
		                  !tradeOff()))
			tradeSeed(detailSeed);
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
					PobUi::Hint(u8"被改掉的昇華天賦在天賦樹的昇華盤上以金圈標出。", innerW);
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
				PobUi::Select("##statsort", &statSort, sorts, nullptr, 4,
				              (std::max)(std::floor(PobUi::D(120.0f)), PobUi::SelectFitWidth(sorts, 4)));
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
		const float listH = (std::max)(PobUi::D(80.0f), bottom - ImGui::GetCursorScreenPos().y);
		ImGui::BeginChild("##aff", ImVec2(innerW, listH), false);
		if (listView == 0) drawStatList(innerW);
		else drawNodeList(innerW);
		ImGui::EndChild();
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
			if (t.replaced) {   // show what the seed took away, not just what it gives
				const std::string was = u8"（原：" + (n.nameZh.empty() ? n.name : n.nameZh) + u8"）";
				dl->AddText(ImVec2(p.x + starW + ImGui::CalcTextSize(nm.c_str()).x + PobUi::D(6.0f), p.y), Tok::TextMuted, was.c_str());
			}
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

	// POBTOOLS_TJ_STATE (test aid, with POBTOOLS_TOOL_SHOT): opens the window in a known state for a hidden-window screenshot: Lethal
	// Pride, the socket with the most notables in reach, three wanted stats.
	//   search  = the form filled in        results = after a search (no seed picked)
	//   resultsseed = after a search, first seed picked (its column open)
	//   tradepanel = the trade settings editor open   noleague = no league set (warning row)
	//   tradedump = a search, then a group and the whole list "sent" to the trade
	//               site: written to POBTOOLS_TJ_TRADE_DUMP(.group/.all.txt), never opened
	//   seed    = the seed page, a seed looked up       menu = the more menu open
	//   paste   = a jewel "pasted" (TJItemText round-trip, no clipboard)
	//   seederr = the seed page's error banner (empty seed)
//   resultsflat = results without grouping   jewelmenu = the jewel Select open
//   pastenosock = paste with no socket picked; POBTOOLS_TJ_PASTE_FILE = text to paste
//   keystone = Glorious Vanity, no seed, Bitter Frost's tooltip forced open
//              (POBTOOLS_TJ_CONQ = conqueror index)
//   protect = Militant Faith, two notables protected, a search run
//   tradeclick = a search, then one synthetic click on the first trade button
//                with more than 40 seeds (POBTOOLS_TJ_TRADE_DUMP gets what it sent)
//   protectclick = Militant Faith, one synthetic right click on a notable;
//                  POBTOOLS_TJ_CLICK_DUMP = file that gets "selected=N protected=N"
	// Nothing is written: under a test aid the league fetch, the tree-update
	// check and tj_ui.json are all off.
	void applyTestState()
	{
		if (testState_.empty() || !ptDataOk) return;
		jewelType = 2;
		conquerorSel = 0;
		{
			wchar_t cq[8] = L""; // POBTOOLS_TJ_CONQ = conqueror index, for every state
			if (GetEnvironmentVariableW(L"POBTOOLS_TJ_CONQ", cq, 8) > 0) conquerorSel = (std::max)(0, _wtoi(cq));
		}
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
		// POBTOOLS_TJ_CAM="zoom,x,y": pin the tree camera (whole tree / an ascendancy disc) instead of centring the socket
		if (const char* cam = std::getenv("POBTOOLS_TJ_CAM")) {
			float z = 0, x = 0, y = 0;
			if (sscanf(cam, "%f,%f,%f", &z, &x, &y) == 3) testCam_ = ImVec4(z, x, y, 0);
		}
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
		if (testState_ == "results" || testState_ == "resultsflat" || testState_ == "resultsseed" ||
		    testState_ == "tradepanel" || testState_ == "noleague" || testState_ == "tradedump" ||
		    testState_ == "tradeclick") {
			if (const char* c = std::getenv("POBTOOLS_TJ_CLICKS")) testTradeWant_ = (std::max)(1, atoi(c));
			mode = 0;
			groupResults = testState_ != "resultsflat";
			autoPick_ = testState_ == "resultsseed";
			tradeHintShown = testState_ == "resultsseed";
			if (testState_ == "tradepanel") openTrade_ = true;
			if (testState_ == "noleague") tradeLeague.clear();
			runSearch();
		} else if (testState_ == "nowants") {   // search with no stats: every seed, by number
			mode = 0;
			groupResults = true;
			autoPick_ = true;
			wants.clear();
			runSearch();
		} else if (testState_ == "seed") {
			mode = 1;
			seedText = std::to_string(mid);
			querySeed();
		} else if (testState_ == "menu") {
			openMore_ = true;
		} else if (testState_ == "paste" || testState_ == "pastenosock") {
			mode = 0;   // the paste must be what switches to the seed page
			std::string txt = TJItemText(*ds, jewelType, conqAny_ ? 0 : conquerorSel, mid);
			wchar_t path[MAX_PATH] = L"";
			const DWORD n = GetEnvironmentVariableW(L"POBTOOLS_TJ_PASTE_FILE", path, MAX_PATH);
			if (n > 0 && n < MAX_PATH) {
				HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
				if (f != INVALID_HANDLE_VALUE) {
					std::string buf(1 << 16, '\0');
					DWORD got = 0;
					if (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr)) { buf.resize(got); txt = buf; }
					CloseHandle(f);
				}
			}
			if (testState_ == "pastenosock") selSocket = -1;
			applyPaste(txt);
		} else if (testState_ == "keystone") {
			// Glorious Vanity, no seed, the socket that reaches Bitter Frost, its
			// tooltip forced open. POBTOOLS_TJ_CONQ = conqueror index (default 0).
			jewelType = 1;
			templates = TJStatTemplates(*ds, jewelType);
			templatesFor = jewelType;
			wchar_t cq[8] = L"";
			if (GetEnvironmentVariableW(L"POBTOOLS_TJ_CONQ", cq, 8) > 0) conquerorSel = (std::max)(0, _wtoi(cq));
			if (GetEnvironmentVariableW(L"POBTOOLS_TJ_CONQ_SWITCH", cq, 8) > 0) testConqSwitch_ = (std::max)(0, _wtoi(cq));
			for (int i = 0; i < (int)ptData.nodes.size(); i++)
				if (ptData.nodes[i].name == "Bitter Frost") testHoverNode_ = i;
			selSocket = -1;
			for (int s : ptData.sockets)
				for (int idx : ptData.NodesInRadius(s, 1800.0f))
					if (idx == testHoverNode_ && selSocket < 0) selSocket = s;
			centerPending_ = true;
			mode = 0;
			wants.clear();
		} else if (testState_ == "abyss" || testState_ == "zorath") {
			// Abyss mode: Ulaman (or Zorath) on the first socket, one wanted stat, a search run and the first
			// seed opened. Zorath: POBTOOLS_TJ_CLASS = class id (default 1 Marauder), its first Ascendancy.
			jewelType = testState_ == "zorath" ? 11 : 8;
			templates = TJStatTemplates(*ds, jewelType);
			templatesFor = jewelType;
			ensureBin(jewelType);
			wchar_t cn[16] = L"";
			classId_ = GetEnvironmentVariableW(L"POBTOOLS_TJ_CLASS", cn, 16) > 0 ? _wtoi(cn) : 1;
			asc_.clear(); protectAsc_.clear(); reqAscSel_ = 0;
			for (const auto& a : ptData.ascClasses)
				if (asc_.empty() && a.classId == classId_) asc_ = a.id;
			selSocket = ptData.sockets.empty() ? -1 : ptData.sockets[0];
			centerPending_ = true;
			wants.clear();
			const char* en = jewelType == 11 ? "+# to maximum Life" : "#% increased Impale Effect";
			for (const auto& t : templates)
				if (t.en == en) wants.push_back({ t.en, t.zh.empty() ? t.en : t.zh, 0.0f, 2.0f });
			scope = 0;
			mode = 0;
			autoPick_ = true;
			updateAbyssDerived();
			runSearch();
		} else if (testState_ == "protect") {
			// Militant Faith on the socket already chosen above, two notables
			// protected, searching for Devotion, the first seed opened.
			jewelType = 4;
			templates = TJStatTemplates(*ds, jewelType);
			templatesFor = jewelType;
			ensureBin(jewelType);
			wants.clear();
			for (const auto& t : templates)
				if (t.en == "+# to Devotion") wants.push_back({ t.en, t.zh.empty() ? t.en : t.zh, 0.0f, 1.0f });
			ptProtected.assign(ptData.nodes.size(), 0);
			int marked = 0;
			for (int idx : ptData.NodesInRadius(selSocket, 1800.0f))
				if (ptData.nodes[idx].kind == kPtNotable && marked < 2) { ptProtected[idx] = 1; marked++; }
			mode = 0;
			autoPick_ = true;
			runSearch();
		} else if (testState_ == "protectclick") {
			// Militant Faith, nothing marked: the canvas code right-clicks the
			// first notable in reach.
			jewelType = 4;
			templates = TJStatTemplates(*ds, jewelType);
			templatesFor = jewelType;
			wants.clear();
			for (int idx : ptData.NodesInRadius(selSocket, 1800.0f))
				if (ptData.nodes[idx].kind == kPtNotable && testClickNode_ < 0) testClickNode_ = idx;
			centerPending_ = false;
			mode = 0;
		} else if (testState_ == "jewelmenu") {
			PobUi::TestOpenSelect("##jewel");
		} else if (testState_ == "seederr") {
			mode = 1;
			seedText.clear();
			querySeed();
		}
	}

	ToolCloseState RequestClose() override
	{
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
	// The search criterion. Everything that shows or highlights a match goes through
	// TJWantMatcher so the display can never disagree with the ranking (最小值 included).
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

	// ---- Abyss mode helpers ----
	// The chosen Ascendancy's disc id, as Zorath's ASCS section keys it ("Raider", not the display name "Warden");
	// "" = none or not of the chosen class.
	std::string ascClass() const
	{
		for (const auto& a : ptData.ascClasses)
			if (!asc_.empty() && a.id == asc_ && a.classId == classId_) return a.id;
		return {};
	}

	// GGPK names for the class / Ascendancy pickers (ui.json, ascendancy_by_id.json); English when missing.
	void loadClassNames()
	{
		if (classNamesLoaded_) return;
		classNamesLoaded_ = true;
		auto load = [&](const wchar_t* file, std::map<std::string, std::string>& out, bool onlyClasses) {
			std::string content;
			if (!read_file_utf8(exeDir + L"Data\\poe1\\zh-rTW\\" + file, content)) return;
			try {
				const nlohmann::json d = nlohmann::json::parse(content);
				const nlohmann::json& e = d.at("entries");
				if (!onlyClasses) {
					for (const auto& [k, v] : e.items()) if (v.is_string()) out[k] = v.get<std::string>();
					return;
				}
				for (const auto& cs : ptData.classStarts) {
					auto it = e.find(cs.name);
					if (it != e.end() && it->is_string()) out[cs.name] = it->get<std::string>();
				}
			} catch (...) {}   // names are cosmetic: a bad file just shows English
		};
		load(L"ui.json", classZh_, true);
		load(L"ascendancy_by_id.json", ascZh_, false);
	}
	static std::string zhOr(const std::map<std::string, std::string>& m, const std::string& key, const std::string& en = {})
	{
		auto it = m.find(key);
		return it != m.end() && !it->second.empty() ? it->second : en.empty() ? key : en;
	}

	std::string ascName(int id) const
	{
		auto it = passiveZh_.find(id);
		return it != passiveZh_.end() ? it->second : std::to_string(id);
	}

	void loadAbyssNames()
	{
		if (!ascOutcomes_.empty()) return;
		for (const TJEntry& e : ds->nodes)
			if (e.id.rfind("abyss_special_ascendancy_notable_", 0) == 0)
				ascOutcomes_.push_back({ e.id, e.dnZh.empty() ? e.dn : e.dnZh });
		std::sort(ascOutcomes_.begin(), ascOutcomes_.end(),
		          [](const std::pair<std::string, std::string>& a, const std::pair<std::string, std::string>& b) { return a.second < b.second; });
		std::string content;
		if (read_file_utf8(exeDir + L"Data\\poe1\\zh-rTW\\passives_by_id.json", content)) {
			try {
				const nlohmann::json d = nlohmann::json::parse(content);
				for (const auto& [k, v] : d.at("entries").items())
					if (v.is_string()) passiveZh_[atoi(k.c_str())] = v.get<std::string>();
			} catch (...) {}   // names are cosmetic: a bad file just shows ids
		}
	}

	// Rebuild Zorath's path / the protect candidates when anything they depend on changes.
	// Cheap: one BFS over ~2000 nodes, one ASCS walk.
	void updateAbyssDerived()
	{
		if (!TJIsAbyss(jewelType)) return;
		const std::string key = std::to_string(jewelType) + "|" + std::to_string(selSocket) + "|" +
		                        std::to_string(classId_) + "|" + asc_ + "|" +
		                        std::to_string(binOk && loadedBinType == jewelType) + "|" + ptData.Version();
		if (key == abyssKey_) return;
		abyssKey_ = key;
		abyssVer_++;
		zorathPath_.clear(); ascTargets_.clear();
		pathMask_.assign(ptData.nodes.size(), 0);
		if (!TJIsZorath(jewelType) || classId_ < 0) return;   // 7-10 light nothing: the jewel's file lists its passives
		if (selSocket >= 0) {
			std::set<int> all;   // every passive "allocated" = the shortest path over the whole tree
			for (const PtNode& n : ptData.nodes) all.insert(n.id);
			zorathPath_ = PtShortestPathToClassStart(ptData, all, ptData.nodes[selSocket].id, classId_);
		}
		for (int id : zorathPath_) {
			const int idx = ptData.IndexOfId(id);
			if (idx >= 0) pathMask_[idx] = 1;
		}
		loadAbyssNames();
		if (binOk && loadedBinType == jewelType && !ascClass().empty())
			ascTargets_ = TJAbyssAscendancyTargets(*blob, *abyssLut, ascClass());
		for (auto it = protectAsc_.begin(); it != protectAsc_.end();)
			it = std::find(ascTargets_.begin(), ascTargets_.end(), *it) == ascTargets_.end() ? protectAsc_.erase(it) : ++it;
	}

	// Why an Abyss search cannot run yet; "" when it can.
	std::string abyssBlocker() const
	{
		if (!TJIsAbyss(jewelType)) return {};
		if (TJIsZorath(jewelType)) {
			if (ptData.classStarts.empty()) return u8"天賦樹資料太舊（缺職業起點），請更新天賦樹";
			if (classId_ < 0) return u8"重奪惡意要先在左側選職業起點";
			if (selSocket >= 0 && zorathPath_.empty()) return u8"這個插槽沒有路徑通到所選的職業起點";
		}
		return {};
	}

	// The query both the search and the result summaries use, so a listed score
	// and its breakdown come from the same rules.
	TJSearchQuery makeQuery() const
	{
		TJSearchQuery q;
		q.jewelType = jewelType;
		q.scope = scope;
		q.minTotalWeight = minTotalWeight;
		q.requireAll = requireAll;
		for (auto& w : wants) q.wants.push_back({ w.en, w.minValue, w.weight });
		if (!TJIsAbyss(jewelType)) {
			// Legion: the socket's neighbourhood, narrowed to the user's picks when there are any.
			for (int idx : resultNodes()) q.nodeIds.push_back(ptData.nodes[idx].id);
			q.protectIds = protectedIds();
		} else if (TJIsZorath(jewelType)) {
			q.nodeIds.assign(zorathPath_.begin(), zorathPath_.end());
			q.ascClass = ascClass();
			if (reqAscSel_ > 0 && reqAscSel_ <= (int)ascOutcomes_.size()) q.requiredAscOutcome = ascOutcomes_[reqAscSel_ - 1].first;
			q.protectAscIds.assign(protectAsc_.begin(), protectAsc_.end());
		}
		return q;
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
	std::vector<WantRow> wants;
	std::string seedText = "500";
	std::string seedErr_;         // the seed page's own error (banner on that page)
	int detailSeed = -1;          // a result seed to expand
	// this frame's conqueror, for the trade export and TJApply
	std::string conquerorId_ = "1";
	std::string tradeStatId_;
	bool conqAny_ = false;               // conqueror "任意": keystones list every conqueror, trade ORs all
	std::vector<std::string> tradeIds_;  // every conqueror's trade stat (deduped) under "任意"
	// results: toast once per finished search; the test aid picks the first seed
	bool jobReported_ = true;
	bool autoPick_ = false;
	// header: the more menu, and whether the tree version moved into it
	bool openMore_ = false;
	ImVec2 moreAnchor_{ 0, 0 };
	// the trade settings row and the editor it opens
	bool openTrade_ = false;
	ImVec2 tradeAnchor_{ 0, 0 };
	float tradeRowW_ = 0.0f;
	// the search form, minus the stat list, last frame (sizes the list)
	float formOtherH_ = 0.0f;
	std::map<ImGuiID, size_t> tradeNext_;   // per trade button: where its last batch stopped
	ImGuiID testTradeId_ = 0;     // POBTOOLS_TJ_STATE=tradeclick: the button being clicked
	int testTradeClicks_ = 0, testTradeWant_ = 1, testTradeAt_ = 0;
	bool testDumped_ = false;    // POBTOOLS_TJ_STATE=tradedump: done once
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
	bool testKsDumped_ = false;   // POBTOOLS_TJ_STATE=keystone: POBTOOLS_TJ_KS_DUMP written
	int testHoverNode_ = -1;      // POBTOOLS_TJ_STATE=keystone: tooltip shown without a mouse
	int testConqSwitch_ = -1;     // POBTOOLS_TJ_CONQ_SWITCH: conqueror picked after the first recompute
	int testClickNode_ = -1;      // POBTOOLS_TJ_STATE=protectclick: the notable right-clicked
	int testClickFrame_ = 0;      // frames drawn since; -1 once the dump is written
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
	std::vector<char> ptProtected;               // per-node: notable the search must not let be replaced
	bool searchProtected_ = false;               // the last search carried a protection (empty-state wording)
	bool searchNoWants_ = false;                 // the last search had no stats: every seed, by number
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
	bool groupResults = true;                    // group seeds by # of nodes matched (the default before the redesign)
	// result-list summaries (matched stats per seed), rebuilt when anything they
	// depend on changes (summarySig)
	std::unordered_map<int, SeedSummary> sumCache_;
	std::string sumSig_;
	std::vector<int> resultNodesCache_;

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

	// ---- Abyss mode: Zorath's class start and Ascendancy (PoB 2.68 searchSingleSocket) ----
	int classId_ = -1;            // Zorath: class start the path runs to; -1 = none chosen
	std::string asc_;             // Zorath: Ascendancy disc id ("Raider"); "" = none
	bool classNamesLoaded_ = false;
	std::map<std::string, std::string> classZh_, ascZh_; // English class / Ascendancy id -> GGPK name
	int reqAscSel_ = 0;           // Zorath: 0 = any, else 1 + index into ascOutcomes_
	std::vector<std::pair<std::string, std::string>> ascOutcomes_; // legion id, display name
	std::set<int> protectAsc_;    // Zorath: Ascendancy passives a seed must not conquer
	// derived from the above + socket + jewel; rebuilt when abyssKey_ changes
	std::string abyssKey_;
	int abyssVer_ = 0;
	std::set<int> zorathPath_;    // ids, socket and class start included
	std::vector<char> pathMask_;  // node index -> lit on the tree: Zorath's path
	ImVec4 testCam_{ 0, 0, 0, 0 }; // POBTOOLS_TJ_CAM: zoom, x, y pinned every frame (screenshot aid)
	std::set<int> ascHit_;        // Zorath: ascendancy node ids the detail seed conquers (gold ring on the disc)
	std::vector<int> ascTargets_; // Zorath: the chosen Ascendancy's passives the jewel can take
	std::map<int, std::string> passiveZh_; // passives_by_id.json, for Ascendancy names
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


// cross-region stat id check (--tj-realm-check)

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

	line(failures == 0 ? "\nRESULT PASS" : "\nRESULT FAIL");
	HANDLE h = CreateFileW((exeDir + L"tj_realm_check.txt").c_str(), GENERIC_WRITE, 0,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD wrote = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &wrote, nullptr);
		CloseHandle(h);
	}
	return failures == 0 ? 0 : 1;
}
