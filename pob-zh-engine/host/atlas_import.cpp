#include "atlas_import.h"
#include "atlas_version_index.h" // resolve the active season folder for the CLI

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h> // SHCreateDirectoryExW (nested season folders)

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#pragma comment(lib, "shell32.lib")

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

using nlohmann::ordered_json;

// Zoom tier used for every sprite lookup; matches tools/gen_atlas_tree.py.
static const char* kZoom = "0.5";
static const double kWorldPerPx = 2.0; // sprite px at zoom 0.5 -> world units
static const double kPi = 3.14159265358979323846;

// ---- small helpers ------------------------------------------------------------

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

static double r2(double v) { return std::round(v * 100.0) / 100.0; }
static double r5(double v) { return std::round(v * 100000.0) / 100000.0; }

// PoB PassiveTree.lua:CalcOrbitAngles (16/40-node orbits are irregular).
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

// kind values shared with atlas_tree_data.h / gen_atlas_tree.py
enum { K_NORMAL = 0, K_NOTABLE = 1, K_KEYSTONE = 2, K_START = 3, K_WORMHOLE = 4 };

// ---- sprite sheet index ---------------------------------------------------------

namespace {

struct SheetIndex {
	const ordered_json& sprites;
	std::wstring srcAssets;                 // source assets dir with trailing backslash
	std::vector<std::string> baseNames;     // slot -> file base name
	std::vector<std::pair<int, int>> dims;  // slot -> declared w/h
	std::map<std::string, int> byBase;
	std::string err;

	explicit SheetIndex(const ordered_json& sp) : sprites(sp) {}

	// cdn filename ("https://.../atlas-skills-4.jpg?bcad") -> "atlas-skills-4.jpg"
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
			err = u8"sprites 段缺少類別 " + cat + u8"（zoom 0.5）";
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

		if (base.size() >= 5 && base.compare(base.size() - 5, 5, ".webp") == 0) {
			err = u8"圖集 " + base + u8" 是 webp 格式，目前不支援（需要 png/jpg）";
			return -1;
		}
		int w = sec->value("w", 0), h = sec->value("h", 0);
		if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
			err = u8"圖集 " + base + u8" 尺寸異常或超過 4096";
			return -1;
		}
		DWORD attr = GetFileAttributesW((srcAssets + widen(base)).c_str());
		if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) {
			err = u8"assets 資料夾內找不到圖集 " + base;
			return -1;
		}
		int slot = (int)baseNames.size();
		byBase[base] = slot;
		baseNames.push_back(base);
		dims.push_back({ w, h });
		return slot;
	}

	// Builds {"s","uv","w","h"} or null when the coord key is absent.
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

} // namespace

// ---- conversion (port of gen_atlas_tree.py main()) --------------------------------

bool ImportAtlasTreeData(const std::wstring& dataJsonPath, const std::wstring& destDir,
                         std::string* err, std::string* summary)
{
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};

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
		if (!d.contains("nodes") || !d.contains("groups") || !d.contains("constants") ||
		    !d.contains("sprites") || !d.contains("points"))
			return fail(u8"data.json 缺少必要欄位（nodes/groups/constants/sprites/points）——確定選的是 atlastree-export 的 data.json 嗎？");

		const ordered_json& nodesIn = d["nodes"];
		const ordered_json& groups = d["groups"];

		// atlas tree only; a character skilltree-export has classes filled in
		if (d["constants"].contains("classes") && d["constants"]["classes"].is_array() &&
		    !d["constants"]["classes"].empty())
			return fail(u8"這是角色天賦樹的匯出，不是圖譜（atlas）天賦樹");

		std::vector<double> orbitRadii;
		for (const auto& v : d["constants"]["orbitRadii"]) orbitRadii.push_back(v.get<double>());
		std::vector<std::vector<double>> orbitAngles;
		for (const auto& v : d["constants"]["skillsPerOrbit"])
			orbitAngles.push_back(calc_orbit_angles(v.get<int>()));

		if (!nodesIn.contains("root") || !nodesIn["root"].contains("out") ||
		    nodesIn["root"]["out"].size() != 1)
			return fail(u8"找不到 root 節點或其起點連線數不是 1（schema 可能已變動）");
		std::string startId = nodesIn["root"]["out"][0].get<std::string>();

		// source assets live next to data.json
		std::wstring srcDir = dataJsonPath;
		size_t slash = srcDir.find_last_of(L"\\/");
		srcDir = (slash == std::wstring::npos) ? L"" : srcDir.substr(0, slash + 1);
		SheetIndex sheets(d["sprites"]);
		sheets.srcAssets = srcDir + L"assets\\";

		static const char* kRejectFlags[] = {
			"isProxy", "isBlighted", "isJewelSocket", "isAscendancyStart",
			"isMultipleChoice", "isMultipleChoiceOption", "isBloodline",
		};

		std::set<std::string> masteryIds;
		std::map<std::string, int> id2idx;
		ordered_json outNodes = ordered_json::array();
		ordered_json outMast = ordered_json::array();
		std::vector<int> nGroup, nOrbit;   // per out-node, for arc detection
		std::vector<double> nAng;

		for (const auto& [key, v] : nodesIn.items()) {
			if (key == "root") continue;
			for (const char* f : kRejectFlags)
				if (v.value(f, false))
					return fail(u8"節點 " + key + u8" 帶有未支援的旗標 " + f + u8"（schema 已變動，需更新匯入器）");

			int grpId = v.value("group", -1);
			int orbit = v.value("orbit", 0);
			int orbitIndex = v.value("orbitIndex", 0);
			const ordered_json& grp = groups.at(std::to_string(grpId));
			if (orbit >= (int)orbitAngles.size() || orbitIndex >= (int)orbitAngles[orbit].size())
				return fail(u8"節點 " + key + u8" 的 orbit/orbitIndex 超出範圍");
			double ang = orbitAngles[orbit][orbitIndex];
			double x = grp.value("x", 0.0) + std::sin(ang) * orbitRadii[orbit];
			double y = grp.value("y", 0.0) - std::cos(ang) * orbitRadii[orbit];

			if (v.value("isMastery", false)) {
				masteryIds.insert(key);
				ordered_json spr = sheets.uv("mastery", v.value("icon", std::string()));
				if (spr.is_null())
					return fail(sheets.err.empty() ? u8"mastery 圖示缺失: " + key : sheets.err);
				ordered_json m;
				m["n"] = v.value("name", std::string());
				m["x"] = r2(x); m["y"] = r2(y);
				for (auto& [k2, v2] : spr.items()) m[k2] = v2;
				outMast.push_back(std::move(m));
				continue;
			}

			int kind = K_NORMAL;
			if (key == startId) kind = K_START;
			else if (v.value("isWormhole", false)) kind = K_WORMHOLE;
			else if (v.value("isKeystone", false)) kind = K_KEYSTONE;
			else if (v.value("isNotable", false)) kind = K_NOTABLE;

			ordered_json on, off;
			if (kind == K_START) {
				on = sheets.uv("startNode", "AtlasPassiveSkillScreenStart");
				off = on;
			} else if (kind == K_WORMHOLE) {
				on = sheets.uv("wormholeActive", "Wormhole");
				off = sheets.uv("wormholeInactive", "Wormhole");
			} else {
				static const char* actCat[] = { "normalActive", "notableActive", "keystoneActive" };
				static const char* inaCat[] = { "normalInactive", "notableInactive", "keystoneInactive" };
				on = sheets.uv(actCat[kind], v.value("icon", std::string()));
				off = sheets.uv(inaCat[kind], v.value("icon", std::string()));
			}
			if (on.is_null() || off.is_null())
				return fail(sheets.err.empty()
					? u8"節點 " + key + u8" 的圖示在 sprite 表中缺失: " + v.value("icon", std::string())
					: sheets.err);

			ordered_json n;
			n["id"] = v.value("skill", 0);
			n["name"] = v.value("name", std::string());
			n["kind"] = kind;
			n["x"] = r2(x); n["y"] = r2(y);
			n["stats"] = v.contains("stats") ? v["stats"] : ordered_json::array();
			n["on"] = std::move(on);
			n["off"] = std::move(off);
			id2idx[key] = (int)outNodes.size();
			outNodes.push_back(std::move(n));
			nGroup.push_back(grpId);
			nOrbit.push_back(orbit);
			nAng.push_back(ang);
		}
		if (id2idx.find(startId) == id2idx.end())
			return fail(u8"起點節點不在輸出集合中");

		// undirected edges, skipping root and masteries
		std::set<std::pair<int, int>> edgeSet;
		for (const auto& [key, v] : nodesIn.items()) {
			if (key == "root" || masteryIds.count(key)) continue;
			auto addNb = [&](const std::string& nb) -> bool {
				if (nb == "root" || masteryIds.count(nb)) return true;
				auto it = id2idx.find(nb);
				if (it == id2idx.end()) { fail(u8"連線指向未知節點 " + nb); return false; }
				int a = id2idx[key], b = it->second;
				if (a != b) edgeSet.insert({ (std::min)(a, b), (std::max)(a, b) });
				return true;
			};
			if (v.contains("out"))
				for (const auto& nb : v["out"]) if (!addNb(nb.get<std::string>())) return false;
			if (v.contains("in"))
				for (const auto& nb : v["in"]) if (!addNb(nb.get<std::string>())) return false;
		}

		ordered_json outEdges = ordered_json::array();
		std::vector<std::vector<int>> adj(outNodes.size());
		for (const auto& [a, b] : edgeSet) {
			adj[a].push_back(b);
			adj[b].push_back(a);
			ordered_json e;
			e["a"] = a; e["b"] = b;
			int ka = outNodes[a]["kind"].get<int>(), kb = outNodes[b]["kind"].get<int>();
			if (ka == K_WORMHOLE && kb == K_WORMHOLE) {
				e["w"] = 1;
			} else if (nGroup[a] == nGroup[b] && nOrbit[a] == nOrbit[b] && nOrbit[a] > 0) {
				const ordered_json& grp = groups.at(std::to_string(nGroup[a]));
				double a0 = nAng[a] - kPi / 2.0, a1 = nAng[b] - kPi / 2.0;
				double delta = std::fmod(a1 - a0, 2.0 * kPi);
				if (delta < -kPi) delta += 2.0 * kPi;
				else if (delta > kPi) delta -= 2.0 * kPi;
				if (delta < 0) { a0 += delta; delta = -delta; }
				e["arc"] = { r2(grp.value("x", 0.0)), r2(grp.value("y", 0.0)),
				             orbitRadii[nOrbit[a]], r5(a0), r5(delta) };
			}
			outEdges.push_back(std::move(e));
		}

		// reachability from the start node
		{
			std::vector<char> seen(outNodes.size(), 0);
			std::vector<int> stack{ id2idx[startId] };
			seen[id2idx[startId]] = 1;
			int reached = 1;
			while (!stack.empty()) {
				int cur = stack.back(); stack.pop_back();
				for (int nb : adj[cur])
					if (!seen[nb]) { seen[nb] = 1; reached++; stack.push_back(nb); }
			}
			if (reached != (int)outNodes.size())
				return fail(u8"有 " + std::to_string((int)outNodes.size() - reached) +
				            u8" 個節點無法從起點抵達，資料異常，已中止匯入");
		}

		// frames
		static const struct { int kind; const char* off; const char* path; const char* on; } kFrames[] = {
			{ K_NORMAL,   "PSSkillFrame", "PSSkillFrameHighlighted", "PSSkillFrameActive" },
			{ K_NOTABLE,  "NotableFrameUnallocated", "NotableFrameCanAllocate", "NotableFrameAllocated" },
			{ K_KEYSTONE, "KeystoneFrameUnallocated", "KeystoneFrameCanAllocate", "KeystoneFrameAllocated" },
			{ K_WORMHOLE, "WormholeFrameUnallocated", "WormholeFrameCanAllocate", "WormholeFrameAllocated" },
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

		// group backgrounds
		ordered_json outGroupBg = ordered_json::array();
		for (const auto& [gid, gv] : groups.items()) {
			if (!gv.contains("background")) continue;
			const ordered_json& bg = gv["background"];
			ordered_json spr = sheets.uv("groupBackground", bg.value("image", std::string()));
			if (spr.is_null())
				return fail(u8"群組背景 sprite 缺失: " + bg.value("image", std::string()));
			ordered_json g;
			g["x"] = r2(gv.value("x", 0.0)); g["y"] = r2(gv.value("y", 0.0));
			g["half"] = bg.value("isHalfImage", false) ? 1 : 0;
			for (auto& [k2, v2] : spr.items()) g[k2] = v2;
			outGroupBg.push_back(std::move(g));
		}

		// full-tree background art, stretched over bounds by the renderer
		ordered_json outBg = sheets.uv("atlasBackground", "AtlasPassiveBackground");
		if (outBg.is_null())
			return fail(sheets.err.empty() ? u8"atlasBackground sprite 缺失" : sheets.err);

		int totalPoints = d["points"].value("totalPoints", 0);
		if (totalPoints < 50 || totalPoints > 500)
			return fail(u8"totalPoints=" + std::to_string(totalPoints) + u8" 不合理，已中止");

		ordered_json out;
		out["version"] = d.value("tree", std::string("Default")) + "/" +
		                 std::to_string(outNodes.size()) + "n/" + std::to_string(totalPoints) + "p";
		out["totalPoints"] = totalPoints;
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
				s["file"] = "atlas/" + sheets.baseNames[i];
				s["w"] = sheets.dims[i].first; s["h"] = sheets.dims[i].second;
				arr.push_back(std::move(s));
			}
			out["sheets"] = std::move(arr);
		}
		out["root"] = id2idx[startId];
		out["nodes"] = std::move(outNodes);
		out["edges"] = std::move(outEdges);
		out["frames"] = std::move(outFrames);
		out["masteries"] = std::move(outMast);
		out["groupbg"] = std::move(outGroupBg);
		out["bg"] = std::move(outBg);

		// everything validated: copy sheets first, then overwrite the JSON.
		// destDir may be nested (Data/atlas_versions/<tag>/) — create the whole
		// path including the atlas/ sprite subfolder.
		SHCreateDirectoryExW(nullptr, (destDir + L"atlas").c_str(), nullptr);
		for (const std::string& base : sheets.baseNames) {
			std::wstring src = sheets.srcAssets + widen(base);
			std::wstring dst = destDir + L"atlas\\" + widen(base);
			if (!CopyFileW(src.c_str(), dst.c_str(), FALSE))
				return fail(u8"複製圖集失敗: " + base);
		}
		if (!write_file_utf8(destDir + L"atlas_tree_poe1.json", out.dump()))
			return fail(u8"寫入 atlas_tree_poe1.json 失敗");

		if (summary)
			*summary = u8"匯入完成：" + std::to_string(out["nodes"].size()) + u8" 節點、" +
			           std::to_string(out["edges"].size()) + u8" 連線、" +
			           std::to_string(totalPoints) + u8" 點上限、" +
			           std::to_string(sheets.baseNames.size()) + u8" 張圖集";
		return true;
	} catch (const std::exception& e) {
		return fail(std::string(u8"匯入過程發生例外: ") + e.what());
	}
}

int RunAtlasImportCli(const std::wstring& dataJsonPath, const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	// import into the active season folder (falls back to the flat Data/ layout)
	AtlasVersionIndex idx;
	idx.Load(exeDir);
	std::wstring dest = idx.ResolveDataDir(exeDir, idx.Active());
	std::string err, summary;
	bool ok = ImportAtlasTreeData(dataJsonPath, dest, &err, &summary);
	printf("%s\n", ok ? summary.c_str() : ("FAIL: " + err).c_str());
	return ok ? 0 : 1;
}
