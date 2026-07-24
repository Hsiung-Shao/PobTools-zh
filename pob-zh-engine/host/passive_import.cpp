#include "passive_import.h"
#include "launcher_config.h" // FindPoe1Dir

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h> // SHCreateDirectoryExW

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#pragma comment(lib, "shell32.lib")

#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using nlohmann::ordered_json;

// KEEP IN SYNC with tools/gen_passive_tree.py (--ggg mode): same zoom tier,
// world scale, node filtering, coordinate math and output schema.
static const char* kZoom = "0.3835";     // the "-3" sheets (GGG_ZOOM)
static const double kWorldPerPx = 2.66;  // WORLD_PER_PX
static const double kPi = 3.14159265358979323846;
static const int kGlesMax = 4096;        // texture dimension ceiling

// kind values shared with host/passive_tree_data.h
enum { K_NORMAL = 0, K_NOTABLE = 1, K_KEYSTONE = 2, K_SOCKET = 3 };

// ---- small helpers (same conventions as atlas_import.cpp) -------------------

static bool read_file_utf8(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 30)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

static bool write_file_utf8(const std::wstring& path, const std::string& content)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = content.empty() ||
		(WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr) && written == content.size());
	CloseHandle(h);
	return ok;
}

static std::wstring widen(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static bool file_exists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static double r2(double v) { return std::round(v * 100.0) / 100.0; }
static double r5(double v) { return std::round(v * 100000.0) / 100000.0; }

// PoB PassiveTree.lua:CalcOrbitAngles (16/40-node orbits are irregular);
// identical to the atlas_import.cpp copy.
static std::vector<double> calc_orbit_angles(int n)
{
	static const double d16[] = { 0, 30, 45, 60, 90, 120, 135, 150, 180, 210, 225, 240, 270, 300, 315, 330 };
	static const double d40[] = { 0, 10, 20, 30, 40, 45, 50, 60, 70, 80, 90, 100, 110, 120, 130, 135,
	                              140, 150, 160, 170, 180, 190, 200, 210, 220, 225, 230, 240, 250, 260,
	                              270, 280, 290, 300, 310, 315, 320, 330, 340, 350 };
	std::vector<double> out;
	if (n == 16) out.assign(d16, d16 + 16);
	else if (n == 40) out.assign(d40, d40 + 40);
	else for (int i = 0; i < n; i++) out.push_back(360.0 * i / n);
	for (double& a : out) a = a * kPi / 180.0;
	return out;
}

// literal "\n" escape <-> real newline (dictionary keys use the escaped form)
static std::string escape_newlines(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		if (c == '\n') out += "\\n";
		else out.push_back(c);
	}
	return out;
}

static std::string unescape_newlines(std::string s)
{
	size_t pos = 0;
	while ((pos = s.find("\\n", pos)) != std::string::npos) {
		s.replace(pos, 2, "\n");
		pos += 1;
	}
	return s;
}

// ---- Traditional-Chinese template translator --------------------------------
// KEEP IN SYNC with gen_passive_tree.py's Translator/_normalize/_TOKEN: numeric
// tokens (ranges first) become '#' for the lookup key; the zh template's "{N}"
// placeholders are refilled with the English line's tokens in order.
//
// Hand-rolled scanners instead of std::regex: MSVC's regex is pathologically
// slow over the multi-MB dictionaries (minutes vs milliseconds). Each helper
// mirrors one Python pattern exactly; all matched bytes are ASCII, so scanning
// UTF-8 byte-wise is safe (CJK bytes are >= 0x80 and never match).

namespace {

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// digits, or fail: advances p past \d+ and returns true when at least one seen
static bool scan_digits(const std::string& s, size_t& p)
{
	size_t st = p;
	while (p < s.size() && is_digit(s[p])) p++;
	return p > st;
}

// _RANGE = \+?\(\d+(\.\d+)?-\d+(\.\d+)?\)  -> match length at i, or 0
static size_t match_range(const std::string& s, size_t i)
{
	size_t p = i;
	if (p < s.size() && s[p] == '+') p++;
	if (p >= s.size() || s[p] != '(') return 0;
	p++;
	auto numpart = [&](size_t& q) {
		if (!scan_digits(s, q)) return false;
		if (q + 1 < s.size() && s[q] == '.' && is_digit(s[q + 1])) { q++; scan_digits(s, q); }
		return true;
	};
	if (!numpart(p) || p >= s.size() || s[p] != '-') return 0;
	p++;
	if (!numpart(p) || p >= s.size() || s[p] != ')') return 0;
	return (p + 1) - i;
}

// _NUM = \+?\d+([.,]\d+)*  -> match length at i, or 0
static size_t match_num(const std::string& s, size_t i)
{
	size_t p = i;
	if (p < s.size() && s[p] == '+') p++;
	if (!scan_digits(s, p)) return 0;
	while (p + 1 < s.size() && (s[p] == '.' || s[p] == ',') && is_digit(s[p + 1])) {
		p++;
		scan_digits(s, p);
	}
	return p - i;
}

// {\d+} -> match length at i (0 when not a brace placeholder); *idx gets N
static size_t match_brace(const std::string& s, size_t i, size_t* idx)
{
	if (i >= s.size() || s[i] != '{') return 0;
	size_t p = i + 1, st = p;
	size_t v = 0;
	while (p < s.size() && is_digit(s[p])) { v = v * 10 + (size_t)(s[p] - '0'); p++; }
	if (p == st || p >= s.size() || s[p] != '}') return 0;
	if (idx) *idx = v;
	return (p + 1) - i;
}

// mirrors Python _normalize(): three sequential passes ({N}, ranges, numbers),
// each replacing a whole match with a single '#'
std::string tr_normalize(const std::string& s)
{
	std::string t;
	t.reserve(s.size());
	for (size_t i = 0; i < s.size();) {                    // pass 1: {N} -> '#'
		size_t len = match_brace(s, i, nullptr);
		if (len) { t += '#'; i += len; } else t += s[i++];
	}
	std::string u;
	u.reserve(t.size());
	for (size_t i = 0; i < t.size();) {                    // pass 2: ranges -> '#'
		size_t len = match_range(t, i);
		if (len) { u += '#'; i += len; } else u += t[i++];
	}
	std::string v;
	v.reserve(u.size());
	for (size_t i = 0; i < u.size();) {                    // pass 3: numbers -> '#'
		size_t len = match_num(u, i);
		if (len) { v += '#'; i += len; } else v += u[i++];
	}
	return v;
}

// mirrors _TOKEN.findall(): range-first alternation, left to right
std::vector<std::string> token_findall(const std::string& s)
{
	std::vector<std::string> out;
	for (size_t i = 0; i < s.size();) {
		size_t len = match_range(s, i);
		if (!len) len = match_num(s, i);
		if (len) { out.push_back(s.substr(i, len)); i += len; } else i++;
	}
	return out;
}

struct ZhTranslator {
	std::unordered_map<std::string, std::string> statIdx; // normalized en -> zh template
	std::unordered_map<std::string, std::string> names;   // exact en -> zh

	// Data/poe1/zh-rTW/{stats,ui,passives}.json — entries: { en: zh }
	void Load(const std::wstring& exeDir)
	{
		static const wchar_t* kFiles[] = { L"stats.json", L"ui.json", L"passives.json" };
		for (int i = 0; i < 3; i++) {
			std::string content;
			if (!read_file_utf8(exeDir + L"Data\\poe1\\zh-rTW\\" + kFiles[i], content)) continue;
			try {
				ordered_json doc = ordered_json::parse(content);
				if (!doc.contains("entries") || !doc["entries"].is_object()) continue;
				for (const auto& [en, zh] : doc["entries"].items()) {
					if (!zh.is_string()) continue;
					// stats.json only: first-seen template wins (setdefault)
					if (i == 0) statIdx.emplace(tr_normalize(en), zh.get<std::string>());
					// names: later files override (passives.json most authoritative)
					names[en] = zh.get<std::string>();
				}
			} catch (...) {
			}
		}
	}

	std::string Stat(const std::string& enLine) const
	{
		auto it = statIdx.find(tr_normalize(enLine));
		if (it == statIdx.end()) return std::string();
		// collect the English line's numeric tokens, then refill the template's
		// "{N}" placeholders (out-of-range N keeps the placeholder, as Python does)
		std::vector<std::string> nums = token_findall(enLine);
		const std::string& tmpl = it->second;
		std::string out;
		out.reserve(tmpl.size() + 16);
		for (size_t i = 0; i < tmpl.size();) {
			size_t idx = 0;
			size_t len = match_brace(tmpl, i, &idx);
			if (len) {
				if (idx < nums.size()) out += nums[idx];
				else out.append(tmpl, i, len);
				i += len;
			} else {
				out += tmpl[i++];
			}
		}
		return out;
	}

	std::string Name(const std::string& en) const
	{
		auto it = names.find(en);
		return it != names.end() ? it->second : std::string();
	}
};

// ---- sprite sheet index (zoom-tier section already selected) ----------------

struct SheetIndex {
	const ordered_json& sprites;                // data.json "sprites" (zoom dicts)
	std::vector<std::string> baseNames;
	std::vector<std::pair<int, int>> dims;
	std::map<std::string, int> byBase;
	std::string err;

	explicit SheetIndex(const ordered_json& sp) : sprites(sp) {}

	static std::string baseName(const std::string& url)
	{
		std::string s = url;
		size_t q = s.find('?');
		if (q != std::string::npos) s = s.substr(0, q);
		size_t sl = s.find_last_of('/');
		if (sl != std::string::npos) s = s.substr(sl + 1);
		return s;
	}

	const ordered_json* section(const std::string& cat)
	{
		if (!sprites.contains(cat) || !sprites[cat].contains(kZoom)) {
			err = u8"sprites 段缺少類別 " + cat + u8"（zoom 0.3835）";
			return nullptr;
		}
		return &sprites[cat][kZoom];
	}

	int sheetFor(const std::string& cat)
	{
		const ordered_json* sec = section(cat);
		if (!sec) return -1;
		std::string base = baseName(sec->value("filename", std::string()));
		auto it = byBase.find(base);
		if (it != byBase.end()) return it->second;
		int w = sec->value("w", 0), h = sec->value("h", 0);
		if (w <= 0 || h <= 0 || w > kGlesMax || h > kGlesMax) {
			err = u8"圖集 " + base + u8" 尺寸異常或超過 4096";
			return -1;
		}
		int slot = (int)baseNames.size();
		byBase[base] = slot;
		baseNames.push_back(base);
		dims.push_back({ w, h });
		return slot;
	}

	// {"s","uv","w","h"} or null when the coord key is absent
	ordered_json uv(const std::string& cat, const std::string& key)
	{
		const ordered_json* sec = section(cat);
		if (!sec) return nullptr;
		if (!sec->contains("coords") || !(*sec)["coords"].contains(key)) return nullptr;
		int slot = sheetFor(cat);
		if (slot < 0) return nullptr;
		const ordered_json& c = (*sec)["coords"][key];
		double W = dims[slot].first, H = dims[slot].second;
		double x = c.value("x", 0.0), y = c.value("y", 0.0);
		double w = c.value("w", 0.0), h = c.value("h", 0.0);
		ordered_json o;
		o["s"] = slot;
		o["uv"] = { r5(x / W), r5(y / H), r5((x + w) / W), r5((y + h) / H) };
		o["w"] = r2(w * kWorldPerPx);
		o["h"] = r2(h * kWorldPerPx);
		return o;
	}
};

// nodes we deliberately drop (not jewel targets; art may be .webp) — mirrors
// gen_passive_tree.py's is_skipped()
bool is_skipped(const ordered_json& v)
{
	if (v.contains("ascendancyName") && !v["ascendancyName"].is_null()) {
		if (!v["ascendancyName"].is_string() || !v["ascendancyName"].get<std::string>().empty())
			return true;
	}
	return v.value("isProxy", false) || v.value("isBlighted", false) ||
	       v.value("isMastery", false) || v.value("isAscendancyStart", false) ||
	       v.value("isMultipleChoice", false) || v.value("isMultipleChoiceOption", false) ||
	       v.contains("classStartIndex");
}

} // namespace

// ---- conversion (port of gen_passive_tree.py --ggg main()) ------------------

bool ImportPassiveTreeData(const std::wstring& dataJsonPath, const std::string& ver,
                           const std::wstring& exeDir, std::string* err, std::string* summary)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	if (ver.empty()) return fail(u8"缺少樹版本（例如 3_29）");

	std::string content;
	if (!read_file_utf8(dataJsonPath, content))
		return fail(u8"無法讀取 data.json");
	ordered_json d;
	try {
		d = ordered_json::parse(content);
	} catch (const std::exception& e) {
		return fail(std::string(u8"data.json 解析失敗: ") + e.what());
	}

	try {
		if (!d.contains("nodes") || !d.contains("groups") || !d.contains("constants") || !d.contains("sprites"))
			return fail(u8"data.json 缺少必要欄位（nodes/groups/constants/sprites）——確定是 skilltree-export 的 data.json 嗎？");
		// a character tree has classes filled in; an atlas export does not
		if (!d.contains("classes") || d["classes"].empty())
			return fail(u8"這不是角色天賦樹的匯出（缺 classes；atlas 樹請用圖譜的匯入）");

		const ordered_json& nodesIn = d["nodes"];
		const ordered_json& groups = d["groups"];

		std::vector<double> orbitRadii;
		for (const auto& v : d["constants"]["orbitRadii"]) orbitRadii.push_back(v.get<double>());
		std::vector<std::vector<double>> orbitAngles;
		for (const auto& v : d["constants"]["skillsPerOrbit"])
			orbitAngles.push_back(calc_orbit_angles(v.get<int>()));

		std::set<std::string> jewelSlots;
		if (d.contains("jewelSlots"))
			for (const auto& v : d["jewelSlots"])
				jewelSlots.insert(v.is_string() ? v.get<std::string>() : std::to_string(v.get<long long>()));

		ZhTranslator tr;
		tr.Load(exeDir);
		SheetIndex sheets(d["sprites"]);

		struct Keep { std::string grp; int orb = 0; double ang = 0; };
		std::map<std::string, int> id2idx;
		ordered_json outNodes = ordered_json::array();
		std::vector<Keep> keepMeta;
		std::vector<int> sockets;
		std::map<std::string, int> groupMaxOrbit;

		for (const auto& [key, v] : nodesIn.items()) {
			if (key == "root" || !v.contains("group") || v["group"].is_null()) continue;
			if (is_skipped(v)) continue;

			std::string gid = std::to_string(v["group"].get<long long>());
			int orbit = v.value("orbit", 0);
			int orbitIndex = v.value("orbitIndex", 0);
			if (!groups.contains(gid)) return fail(u8"節點 " + key + u8" 的 group 不存在");
			const ordered_json& grp = groups[gid];
			if (orbit >= (int)orbitAngles.size() || orbitIndex >= (int)orbitAngles[orbit].size())
				return fail(u8"節點 " + key + u8" 的 orbit/orbitIndex 超出範圍");
			double ang = orbitAngles[orbit][orbitIndex];
			double x = grp.value("x", 0.0) + std::sin(ang) * orbitRadii[orbit];
			double y = grp.value("y", 0.0) - std::cos(ang) * orbitRadii[orbit];

			bool isSocket = v.value("isJewelSocket", false) || jewelSlots.count(key) != 0;
			int kind = isSocket ? K_SOCKET
			         : v.value("isKeystone", false) ? K_KEYSTONE
			         : v.value("isNotable", false) ? K_NOTABLE : K_NORMAL;

			ordered_json nd;
			nd["id"] = v.value("skill", 0);
			nd["name"] = v.value("name", std::string());
			nd["kind"] = kind;
			nd["x"] = r2(x);
			nd["y"] = r2(y);
			std::string nz = tr.Name(v.value("name", std::string()));
			if (!nz.empty()) nd["nameZh"] = nz;
			// stats: data.json carries real newlines; the zh dictionary keys use
			// the literal "\n" escape — normalize the lookup key, display real \n
			if (v.contains("stats") && v["stats"].is_array() && !v["stats"].empty()) {
				ordered_json st = ordered_json::array(), stZh = ordered_json::array();
				bool anyZh = false;
				for (const auto& sv : v["stats"]) {
					std::string s = sv.get<std::string>();
					st.push_back(unescape_newlines(s));
					std::string zh = unescape_newlines(tr.Stat(escape_newlines(s)));
					if (!zh.empty()) anyZh = true;
					stZh.push_back(std::move(zh));
				}
				nd["stats"] = std::move(st);
				if (anyZh) nd["statsZh"] = std::move(stZh);
			}
			if (kind != K_SOCKET) {
				static const char* actCat[] = { "normalActive", "notableActive", "keystoneActive" };
				static const char* inaCat[] = { "normalInactive", "notableInactive", "keystoneInactive" };
				ordered_json on = sheets.uv(actCat[kind], v.value("icon", std::string()));
				ordered_json off = sheets.uv(inaCat[kind], v.value("icon", std::string()));
				if (on.is_null() || off.is_null())
					return fail(sheets.err.empty()
						? u8"節點 " + key + u8" 的圖示在 sprite 表中缺失: " + v.value("icon", std::string())
						: sheets.err);
				nd["on"] = std::move(on);
				nd["off"] = std::move(off);
			}
			auto gm = groupMaxOrbit.find(gid);
			if (gm == groupMaxOrbit.end() || orbit > gm->second) groupMaxOrbit[gid] = orbit;
			id2idx[key] = (int)outNodes.size();
			if (kind == K_SOCKET) sockets.push_back((int)outNodes.size());
			outNodes.push_back(std::move(nd));
			keepMeta.push_back({ gid, orbit, ang });
		}

		// undirected edges between kept nodes
		std::set<std::pair<int, int>> edgeSet;
		for (const auto& [key, v] : nodesIn.items()) {
			auto ai = id2idx.find(key);
			if (ai == id2idx.end()) continue;
			auto addNb = [&](const std::string& nb) {
				auto bi = id2idx.find(nb);
				if (bi == id2idx.end()) return;
				int a = ai->second, b = bi->second;
				if (a != b) edgeSet.insert({ std::min(a, b), std::max(a, b) });
			};
			if (v.contains("out")) for (const auto& nb : v["out"]) addNb(nb.get<std::string>());
			if (v.contains("in")) for (const auto& nb : v["in"]) addNb(nb.get<std::string>());
		}

		struct Edge { int a, b; bool hasArc = false; double cx = 0, cy = 0, r = 0, a0 = 0, sweep = 0; };
		std::vector<Edge> edges;
		int arcs = 0;
		for (const auto& [a, b] : edgeSet) {
			Edge e{ a, b };
			const Keep& ka = keepMeta[a];
			const Keep& kb = keepMeta[b];
			if (ka.grp == kb.grp && ka.orb == kb.orb && ka.orb > 0) {
				const ordered_json& grp = groups[ka.grp];
				double a0 = ka.ang - kPi / 2.0, a1 = kb.ang - kPi / 2.0;
				double delta = std::fmod(a1 - a0, 2.0 * kPi);
				if (delta < -kPi) delta += 2.0 * kPi;
				else if (delta > kPi) delta -= 2.0 * kPi;
				if (delta < 0) { a0 += delta; delta = -delta; }
				e.hasArc = true;
				e.cx = grp.value("x", 0.0); e.cy = grp.value("y", 0.0);
				e.r = orbitRadii[ka.orb]; e.a0 = a0; e.sweep = delta;
				arcs++;
			}
			edges.push_back(e);
		}

		// keep only the largest connected component (dropping class starts
		// orphans the attribute clusters that sat at each start)
		std::vector<std::vector<int>> adj(outNodes.size());
		for (const Edge& e : edges) {
			adj[e.a].push_back(e.b);
			adj[e.b].push_back(e.a);
		}
		std::vector<int> comp(outNodes.size(), -1);
		std::vector<std::vector<int>> comps;
		for (int s = 0; s < (int)outNodes.size(); s++) {
			if (comp[s] != -1) continue;
			std::vector<int> stack{ s }, members;
			comp[s] = (int)comps.size();
			while (!stack.empty()) {
				int u = stack.back(); stack.pop_back();
				members.push_back(u);
				for (int nb : adj[u])
					if (comp[nb] == -1) { comp[nb] = (int)comps.size(); stack.push_back(nb); }
			}
			comps.push_back(std::move(members));
		}
		size_t best = 0;
		for (size_t i = 1; i < comps.size(); i++)
			if (comps[i].size() > comps[best].size()) best = i;
		std::set<int> keep(comps[best].begin(), comps[best].end());
		int dropped = (int)outNodes.size() - (int)keep.size();

		std::vector<int> remap(outNodes.size(), -1);
		ordered_json newNodes = ordered_json::array();
		std::set<std::string> keptGroups;
		std::vector<Keep> newMeta;
		for (int old = 0; old < (int)outNodes.size(); old++) {
			if (!keep.count(old)) continue;
			remap[old] = (int)newNodes.size();
			keptGroups.insert(keepMeta[old].grp);
			newNodes.push_back(std::move(outNodes[old]));
			newMeta.push_back(keepMeta[old]);
		}
		ordered_json outEdges = ordered_json::array();
		for (const Edge& e : edges) {
			if (!keep.count(e.a) || !keep.count(e.b)) continue;
			ordered_json je;
			je["a"] = remap[e.a];
			je["b"] = remap[e.b];
			if (e.hasArc)
				je["arc"] = { r2(e.cx), r2(e.cy), e.r, r5(e.a0), r5(e.sweep) };
			outEdges.push_back(std::move(je));
		}
		ordered_json outSockets = ordered_json::array();
		for (int s : sockets)
			if (keep.count(s)) outSockets.push_back(remap[s]);

		// frames
		static const struct { int kind; const char* off; const char* path; const char* on; } kFrames[] = {
			{ K_NORMAL,   "PSSkillFrame", "PSSkillFrameHighlighted", "PSSkillFrameActive" },
			{ K_NOTABLE,  "NotableFrameUnallocated", "NotableFrameCanAllocate", "NotableFrameAllocated" },
			{ K_KEYSTONE, "KeystoneFrameUnallocated", "KeystoneFrameCanAllocate", "KeystoneFrameAllocated" },
		};
		ordered_json outFrames;
		for (const auto& fr : kFrames) {
			ordered_json f;
			f["off"] = sheets.uv("frame", fr.off);
			f["path"] = sheets.uv("frame", fr.path);
			f["on"] = sheets.uv("frame", fr.on);
			if (f["off"].is_null() || f["path"].is_null() || f["on"].is_null())
				return fail(u8"frame sprite 缺失（kind " + std::to_string(fr.kind) + u8"）");
			outFrames[std::to_string(fr.kind)] = std::move(f);
		}

		// masteries (decorative; kept groups only, never ascendancy)
		ordered_json outMast = ordered_json::array();
		for (const auto& [key, v] : nodesIn.items()) {
			if (key == "root" || !v.value("isMastery", false)) continue;
			if (!v.contains("group") || v["group"].is_null()) continue;
			if (v.contains("ascendancyName") && v["ascendancyName"].is_string() &&
			    !v["ascendancyName"].get<std::string>().empty()) continue;
			std::string gid = std::to_string(v["group"].get<long long>());
			if (!keptGroups.count(gid)) continue;
			int orbit = v.value("orbit", 0);
			int orbitIndex = v.value("orbitIndex", 0);
			if (orbit >= (int)orbitAngles.size() || orbitIndex >= (int)orbitAngles[orbit].size()) continue;
			double ang = orbitAngles[orbit][orbitIndex];
			const ordered_json& grp = groups[gid];
			double x = grp.value("x", 0.0) + std::sin(ang) * orbitRadii[orbit];
			double y = grp.value("y", 0.0) - std::cos(ang) * orbitRadii[orbit];
			ordered_json spr = sheets.uv("mastery", v.value("icon", std::string()));
			if (spr.is_null()) continue;
			ordered_json m;
			m["n"] = v.value("name", std::string());
			m["x"] = r2(x); m["y"] = r2(y);
			for (auto& [k2, v2] : spr.items()) m[k2] = v2;
			outMast.push_back(std::move(m));
		}

		// group backgrounds (size by the group's largest occupied orbit)
		ordered_json outGroupBg = ordered_json::array();
		for (const auto& [gid, mo] : groupMaxOrbit) {
			if (!keptGroups.count(gid)) continue;
			const ordered_json& gv = groups[gid];
			if (gv.value("isProxy", false)) continue;
			const char* key = mo >= 3 ? "PSGroupBackground3" : mo == 2 ? "PSGroupBackground2" : "PSGroupBackground1";
			int half = mo >= 3 ? 1 : 0;
			ordered_json spr = sheets.uv("groupBackground", key);
			if (spr.is_null()) continue;
			ordered_json g;
			g["x"] = r2(gv.value("x", 0.0)); g["y"] = r2(gv.value("y", 0.0));
			g["half"] = half;
			for (auto& [k2, v2] : spr.items()) g[k2] = v2;
			outGroupBg.push_back(std::move(g));
		}

		// ---- assemble output (field order mirrors the Python generator) -----
		ordered_json out;
		out["version"] = ver + "/" + std::to_string(newNodes.size()) + "n";
		out["treeVersion"] = ver;
		out["worldPerPx"] = kWorldPerPx;
		{
			ordered_json b;
			b["minX"] = d.value("min_x", 0.0); b["minY"] = d.value("min_y", 0.0);
			b["maxX"] = d.value("max_x", 0.0); b["maxY"] = d.value("max_y", 0.0);
			out["bounds"] = std::move(b);
		}
		{
			ordered_json arr = ordered_json::array();
			for (size_t i = 0; i < sheets.baseNames.size(); i++) {
				ordered_json s;
				s["file"] = sheets.baseNames[i];
				s["w"] = sheets.dims[i].first; s["h"] = sheets.dims[i].second;
				arr.push_back(std::move(s));
			}
			out["sheets"] = std::move(arr);
		}
		out["nodes"] = std::move(newNodes);
		out["edges"] = std::move(outEdges);
		out["frames"] = std::move(outFrames);
		out["sockets"] = std::move(outSockets);
		out["masteries"] = std::move(outMast);
		out["groupbg"] = std::move(outGroupBg);

		// ---- everything validated: bundle sheets, then overwrite the JSON ----
		// sheet source: local PoB TreeData/<ver>/ when that league exists (no
		// download needed), else the assets/ directory next to data.json
		std::wstring srcDir = dataJsonPath;
		size_t slash = srcDir.find_last_of(L"\\/");
		srcDir = (slash == std::wstring::npos) ? L"" : srcDir.substr(0, slash + 1);
		std::wstring poe1Base = FindPoe1Dir(exeDir); // L"" when no PoB install detected
		std::wstring pobDir = poe1Base.empty() ? L"" : poe1Base + L"TreeData\\" + widen(ver) + L"\\";
		SHCreateDirectoryExW(nullptr, (exeDir + L"Data\\tree").c_str(), nullptr);
		for (const std::string& base : sheets.baseNames) {
			std::wstring dst = exeDir + L"Data\\tree\\" + widen(base);
			std::wstring pobSrc = pobDir.empty() ? L"" : pobDir + widen(base);
			std::wstring gggSrc = srcDir + L"assets\\" + widen(base);
			const std::wstring& src = (!pobSrc.empty() && file_exists(pobSrc)) ? pobSrc : gggSrc;
			if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
				return fail(u8"複製圖集失敗: " + base);
		}
		if (!write_file_utf8(exeDir + L"Data\\passive_tree_poe1.json", out.dump()))
			return fail(u8"寫入 Data/passive_tree_poe1.json 失敗");

		if (summary) {
			int zhLines = 0, lines = 0;
			for (const auto& n : out["nodes"]) {
				if (!n.contains("stats")) continue;
				lines += (int)n["stats"].size();
				if (n.contains("statsZh"))
					for (const auto& z : n["statsZh"])
						if (z.is_string() && !z.get<std::string>().empty()) zhLines++;
			}
			int pct = lines > 0 ? (int)(100.0 * zhLines / lines + 0.5) : 0;
			*summary = u8"匯入完成：" + std::string(ver) + u8" 樹 " +
			           std::to_string(out["nodes"].size()) + u8" 節點、" +
			           std::to_string(out["sockets"].size()) + u8" 插槽、繁中詞條 " +
			           std::to_string(pct) + u8"%（捨棄 " + std::to_string(dropped) + u8" 個孤立節點）";
		}
		return true;
	} catch (const std::exception& e) {
		return fail(std::string(u8"匯入過程發生例外: ") + e.what());
	}
}

int RunPassiveImportCli(const std::wstring& dataJsonPath, const std::wstring& ver,
                        const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	std::string verUtf8(ver.begin(), ver.end()); // versions are ASCII ("3_29")
	std::string err, summary;
	bool ok = ImportPassiveTreeData(dataJsonPath, verUtf8, exeDir, &err, &summary);
	printf("%s\n", ok ? summary.c_str() : ("FAIL: " + err).c_str());
	return ok ? 0 : 1;
}

// ---- self-test (--pt-import-selftest) ---------------------------------------
// Regression guard for the hand-rolled scanner port of the Python regexes; the
// full pipeline is separately parity-tested against gen_passive_tree.py output.

int RunPassiveImportSelfTest(const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	std::string rep;
	int fails = 0;
	auto check = [&](bool ok, const char* what) {
		rep += ok ? "PASS  " : "FAIL  ";
		rep += what;
		rep += "\n";
		if (!ok) fails++;
	};

	// tr_normalize: {N} / ranges / numbers each collapse to '#'
	check(tr_normalize("8% increased Lightning Damage") == "#% increased Lightning Damage",
	      "normalize: plain number");
	check(tr_normalize("+(10-20) to maximum Life") == "# to maximum Life",
	      "normalize: +(a-b) range collapses whole");
	check(tr_normalize("Grants {0} passive {1}") == "Grants # passive #",
	      "normalize: {N} placeholders");
	check(tr_normalize("gain 5,000 gold and 2.5% speed") == "gain # gold and #% speed",
	      "normalize: thousands comma + decimal");
	check(tr_normalize(u8"增加 15% 傷害") == u8"增加 #% 傷害",
	      "normalize: UTF-8 CJK bytes untouched");

	// token_findall: range-first alternation, left to right (Python _TOKEN)
	{
		std::vector<std::string> t = token_findall("Adds 5 to 10 damage, +(3-4)% more");
		check(t.size() == 3 && t[0] == "5" && t[1] == "10" && t[2] == "+(3-4)",
		      "token_findall: numbers then a +(a-b) range");
	}
	check(token_findall("no numbers at all").empty(), "token_findall: none");

	// ZhTranslator::Stat: template refill by {N}
	{
		ZhTranslator tr;
		tr.statIdx[tr_normalize("15% increased Quantity of Items")] = u8"增加 {0}% 物品數量";
		tr.statIdx[tr_normalize("Adds 5 to 10 damage")] = u8"附加 {0} 至 {1} 傷害";
		tr.statIdx[tr_normalize("boolean effect line")] = u8"布林效果行";
		check(tr.Stat("20% increased Quantity of Items") == u8"增加 20% 物品數量",
		      "Stat: value refilled with the line's own number");
		check(tr.Stat("Adds 7 to 13 damage") == u8"附加 7 至 13 傷害",
		      "Stat: two placeholders in order");
		check(tr.Stat("boolean effect line") == u8"布林效果行", "Stat: no-number line");
		check(tr.Stat("completely unknown line").empty(), "Stat: unknown template -> empty");
	}

	// newline escape round-trip (GGG real \n <-> dictionary literal \\n keys)
	check(escape_newlines("a\nb") == "a\\nb" && unescape_newlines("a\\nb") == "a\nb",
	      "newline escape round-trip");

	rep += fails == 0 ? "\nALL PASS\n" : "\nFAILURES: " + std::to_string(fails) + "\n";
	printf("%s", rep.c_str());
	write_file_utf8(exeDir + L"pt_import_selftest.txt", rep);
	return fails == 0 ? 0 : 1;
}
