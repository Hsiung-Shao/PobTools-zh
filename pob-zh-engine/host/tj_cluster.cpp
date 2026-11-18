#include "tj_cluster.h"

#include "headless_proc.h"
#include "pob_sandbox.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cmath>
#include <map>
#include <memory>

using json = nlohmann::json;

namespace {

constexpr float kPi = 3.14159265358979f;

// bridge tree_assets rect -> our sheet slot + uv; invalid when the file is not one of tree.sheets
PtSprite resolve_sprite(const json& bucket, const std::string& name, const PassiveTreeData& tree)
{
	PtSprite s;
	if (name.empty() || !bucket.is_object()) return s;
	const auto it = bucket.find(name);
	if (it == bucket.end() || !it->is_object()) return s;
	const json& r = *it;
	std::string file = r.value("file", std::string());
	const size_t slash = file.find_last_of("/\\");
	if (slash != std::string::npos) file = file.substr(slash + 1);
	for (size_t i = 0; i < tree.sheets.size(); i++) {
		const PtSheet& sh = tree.sheets[i];
		if (sh.file != file || sh.w <= 0 || sh.h <= 0) continue;
		const float x = r.value("x", 0.0f), y = r.value("y", 0.0f), w = r.value("w", 0.0f), h = r.value("h", 0.0f);
		s.sheet = (int)i;
		s.uv[0] = x / sh.w; s.uv[1] = y / sh.h;
		s.uv[2] = (x + w) / sh.w; s.uv[3] = (y + h) / sh.h;
		s.w = w * tree.worldPerPx; s.h = h * tree.worldPerPx;
		s.valid = w > 0 && h > 0;
		return s;
	}
	return s;
}

std::string str_or(const json& j, const char* key)
{
	const auto it = j.find(key);
	return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

void strings_of(const json& j, const char* key, std::vector<std::string>& out)
{
	const auto it = j.find(key);
	if (it == j.end() || !it->is_array()) return;
	for (const auto& s : *it) out.push_back(s.is_string() ? s.get<std::string>() : std::string());
}

} // namespace

bool ParseClusterGraph(const json& treeState, const json& treeAssets, const PassiveTreeData& tree,
                       TJClusterGraph& out, std::string* err)
{
	out = {};
	const auto dn = treeState.is_object() ? treeState.find("dynamicNodes") : treeState.end();
	if (!treeState.is_object() || dn == treeState.end() || !dn->is_array()) {
		if (err) *err = "tree state has no dynamicNodes";
		return false;
	}
	static const json kEmpty = json::object();
	const json& assets = treeAssets.is_object() && treeAssets.contains("assets") ? treeAssets["assets"] : kEmpty;
	const json& disabled = treeAssets.is_object() && treeAssets.contains("disabled") ? treeAssets["disabled"] : kEmpty;

	std::map<int, int> idx;     // POB id -> out.nodes index
	std::map<int, int> grpOf;   // POB id -> subgraph group id
	for (const json& jn : *dn) {
		if (!jn.is_object() || !jn.value("id", json()).is_number() || !jn.value("x", json()).is_number()) continue;
		const std::string type = str_or(jn, "type");
		if (type == "Mastery") continue;
		TJClusterNode n;
		n.id = jn["id"].get<int>();
		n.kind = type == "Notable" ? kPtNotable : type == "Keystone" ? kPtKeystone : type == "Socket" ? kPtSocket : kPtNormal;
		n.name = str_or(jn, "name");
		n.nameZh = str_or(jn, "nameZh");
		n.x = jn["x"].get<float>();
		n.y = jn.value("y", 0.0f);
		strings_of(jn, "stats", n.stats);
		strings_of(jn, "statsZh", n.statsZh);
		n.alloc = jn.value("allocated", false);
		const std::string icon = str_or(jn, "icon");
		n.on = resolve_sprite(assets, icon, tree);
		n.off = resolve_sprite(disabled, icon, tree);
		const auto fr = jn.find("frames");
		if (fr != jn.end() && fr->is_object()) {
			n.frameOn = resolve_sprite(assets, str_or(*fr, "alloc"), tree);
			n.frameOff = resolve_sprite(assets, str_or(*fr, "unalloc"), tree);
		}
		if (jn.value("group", json()).is_number()) grpOf[n.id] = jn["group"].get<int>();
		idx[n.id] = (int)out.nodes.size();
		out.nodes.push_back(std::move(n));
	}

	std::map<int, std::pair<float, float>> groups;
	if (treeState.contains("dynamicGroups") && treeState["dynamicGroups"].is_array())
		for (const json& g : treeState["dynamicGroups"])
			if (g.is_object() && g.value("id", json()).is_number())
				groups[g["id"].get<int>()] = { g.value("x", 0.0f), g.value("y", 0.0f) };

	if (treeState.contains("dynamicConnectors") && treeState["dynamicConnectors"].is_array()) {
		for (const json& c : treeState["dynamicConnectors"]) {
			if (!c.is_object() || !c.value("a", json()).is_number() || !c.value("b", json()).is_number()) continue;
			TJClusterEdge e;
			// a cluster node wins over a main-tree node of the same id (inner sockets are tree ids)
			auto end = [&](int id, int& i, bool& main) {
				const auto it = idx.find(id);
				if (it != idx.end()) { i = it->second; main = false; return true; }
				i = tree.IndexOfId(id); main = true;
				return i >= 0;
			};
			const int ida = c["a"].get<int>(), idb = c["b"].get<int>();
			if (!end(ida, e.a, e.aMain) || !end(idb, e.b, e.bMain)) continue;
			const int orbit = c.value("orbit", json()).is_number() ? c["orbit"].get<int>() : 0;
			const auto ga = grpOf.find(ida), gb = grpOf.find(idb);
			if (orbit > 0 && !e.aMain && !e.bMain && ga != grpOf.end() && gb != grpOf.end() && ga->second == gb->second) {
				const auto g = groups.find(ga->second);
				if (g != groups.end()) {
					const TJClusterNode &na = out.nodes[(size_t)e.a], &nb = out.nodes[(size_t)e.b];
					const float cx = g->second.first, cy = g->second.second;
					float a0 = std::atan2(na.y - cy, na.x - cx);
					const float a1 = std::atan2(nb.y - cy, nb.x - cx);
					float d = std::fmod(a1 - a0, 2.0f * kPi);
					if (d < -kPi) d += 2.0f * kPi;
					else if (d > kPi) d -= 2.0f * kPi;
					if (d < 0) { a0 += d; d = -d; }
					e.hasArc = true;
					e.cx = cx; e.cy = cy;
					e.r = std::hypot(na.x - cx, na.y - cy);
					e.a0 = a0; e.sweep = d;
				}
			}
			out.edges.push_back(e);
		}
	}
	return true;
}

bool TJClusterFetchSync(const std::wstring& exeDir, const std::wstring& pobDir, const std::wstring& buildPath,
                        json& treeState, json& treeAssets, std::string* err)
{
	std::string e;
	auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
	if (pobDir.empty() || !PobSandbox::IsDir(pobDir)) return fail("Path of Building folder not found");
	if (GetFileAttributesW(buildPath.c_str()) == INVALID_FILE_ATTRIBUTES) return fail("build file not found");

	// one folder per fetch: the UI may start a new one before the last child is gone
	static std::atomic<int> seq{ 0 };
	const std::wstring root = exeDir + L"PobTools\\tj_sandbox";
	const std::wstring own = root + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++seq);
	PobSandbox::MkdirP(root);
	// leftovers of processes that are gone (folder name starts with their pid)
	{
		WIN32_FIND_DATAW fd{};
		HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			std::vector<std::wstring> stale;
			do {
				const std::wstring n = fd.cFileName;
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n == L"." || n == L"..") continue;
				const DWORD pid = (DWORD)_wtoi(n.c_str());
				if (pid == GetCurrentProcessId()) continue;
				HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
				const bool alive = p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
				if (p) CloseHandle(p);
				if (!alive) stale.push_back(n);
			} while (FindNextFileW(h, &fd));
			FindClose(h);
			for (const auto& n : stale) PobSandbox::Rmtree(root + L"\\" + n);
		}
	}

	std::unique_ptr<HeadlessProc::Child> child;
	bool ok = HeadlessProc::BootSandboxed(exeDir, pobDir, own + L"\\PathOfBuildingCommunity", true, L"", child, e);
	if (ok) {
		std::string path;
		{
			const int n = WideCharToMultiByte(CP_UTF8, 0, buildPath.c_str(), (int)buildPath.size(), nullptr, 0, nullptr, nullptr);
			path.resize((size_t)n);
			WideCharToMultiByte(CP_UTF8, 0, buildPath.c_str(), (int)buildPath.size(), path.data(), n, nullptr, nullptr);
		}
		json r;
		if (!child->Call("load_build_file", json{ { "path", path } }, r, 120000)) { ok = false; e = "load_build_file: " + r.dump().substr(0, 300); }
		else if (!child->Call("get_tree_state", json::object(), treeState, 60000)) { ok = false; e = "get_tree_state: " + treeState.dump().substr(0, 300); }
		else if (!child->Call("tree_assets", json::object(), treeAssets, 60000)) { ok = false; e = "tree_assets: " + treeAssets.dump().substr(0, 300); }
	}
	if (child) child->Stop(1000);
	child.reset();
	PobSandbox::Rmtree(own);
	return ok ? true : fail(e);
}
