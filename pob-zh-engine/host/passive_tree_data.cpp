#include "passive_tree_data.h"
#include "timeless_jewel.h" // selftest: nodeIndex coverage vs the tree

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <cmath>
#include <cstdarg>
#include <cstdio>

using nlohmann::ordered_json;

// ---- file helpers (same conventions as atlas_tree_data.cpp) -----------------

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

// ---- json parsing -----------------------------------------------------------

static bool parse_sprite(const ordered_json& j, PtSprite& out)
{
	if (!j.is_object() || !j.contains("uv") || !j["uv"].is_array() || j["uv"].size() != 4) return false;
	out.sheet = j.value("s", 0);
	for (int i = 0; i < 4; i++) out.uv[i] = j["uv"][i].get<float>();
	out.w = j.value("w", 0.0f);
	out.h = j.value("h", 0.0f);
	out.valid = true;
	return true;
}

static void parse_strings(const ordered_json& parent, const char* key, std::vector<std::string>& out)
{
	if (parent.contains(key) && parent[key].is_array())
		for (const auto& s : parent[key])
			if (s.is_string()) out.push_back(s.get<std::string>());
}

bool PassiveTreeData::Load(const std::wstring& exeDir, std::string* err)
{
	nodes.clear(); edges.clear(); sheets.clear(); sockets.clear();
	masteries.clear(); groupbg.clear();

	std::string content;
	if (!read_file_utf8(exeDir + L"Data\\passive_tree_poe1.json", content)) {
		if (err) *err = "Data/passive_tree_poe1.json not found or unreadable";
		return false;
	}
	try {
		ordered_json doc = ordered_json::parse(content);

		version_ = doc.value("version", std::string());
		treeVersion_ = doc.value("treeVersion", std::string());
		const ordered_json& b = doc.at("bounds");
		minX = b.value("minX", 0.0f); minY = b.value("minY", 0.0f);
		maxX = b.value("maxX", 0.0f); maxY = b.value("maxY", 0.0f);

		for (const auto& js : doc.at("sheets")) {
			PtSheet s;
			s.file = js.value("file", std::string());
			s.w = js.value("w", 0); s.h = js.value("h", 0);
			sheets.push_back(std::move(s));
		}

		nodes.reserve(doc.at("nodes").size());
		for (const auto& jn : doc.at("nodes")) {
			PtNode n;
			n.id = jn.value("id", 0);
			n.name = jn.value("name", std::string());
			n.nameZh = jn.value("nameZh", std::string());
			n.kind = jn.value("kind", 0);
			n.x = jn.value("x", 0.0f); n.y = jn.value("y", 0.0f);
			parse_strings(jn, "stats", n.stats);
			parse_strings(jn, "statsZh", n.statsZh);
			// sockets carry no icon; everything else must have on/off
			if (n.kind != kPtSocket) {
				if (!parse_sprite(jn.at("on"), n.on) || !parse_sprite(jn.at("off"), n.off))
					throw std::runtime_error("bad node sprite");
			}
			nodes.push_back(std::move(n));
		}

		edges.reserve(doc.at("edges").size());
		for (const auto& je : doc.at("edges")) {
			PtEdge e;
			e.a = je.value("a", -1); e.b = je.value("b", -1);
			if (e.a < 0 || e.b < 0 || e.a >= (int)nodes.size() || e.b >= (int)nodes.size())
				throw std::runtime_error("edge endpoint out of range");
			if (je.contains("arc") && je["arc"].is_array() && je["arc"].size() == 5) {
				e.hasArc = true;
				e.cx = je["arc"][0].get<float>(); e.cy = je["arc"][1].get<float>();
				e.r = je["arc"][2].get<float>();
				e.a0 = je["arc"][3].get<float>(); e.sweep = je["arc"][4].get<float>();
			}
			nodes[e.a].adj.push_back(e.b);
			nodes[e.b].adj.push_back(e.a);
			edges.push_back(e);
		}

		if (doc.contains("sockets"))
			for (const auto& js : doc["sockets"]) {
				int i = js.get<int>();
				if (i >= 0 && i < (int)nodes.size()) sockets.push_back(i);
			}

		if (doc.contains("frames"))
			for (const auto& [key, jf] : doc["frames"].items()) {
				int kind = atoi(key.c_str());
				if (kind < 0 || kind > 2) continue;
				parse_sprite(jf.at("off"), frames[kind].off);
				parse_sprite(jf.at("path"), frames[kind].path);
				parse_sprite(jf.at("on"), frames[kind].on);
			}

		auto load_decos = [&](const char* key, std::vector<PtDeco>& out) {
			if (!doc.contains(key)) return;
			for (const auto& jd : doc[key]) {
				PtDeco d;
				if (!parse_sprite(jd, d.spr)) continue;
				d.name = jd.value("n", std::string());
				d.x = jd.value("x", 0.0f); d.y = jd.value("y", 0.0f);
				d.half = jd.value("half", 0) != 0;
				out.push_back(std::move(d));
			}
		};
		load_decos("masteries", masteries);
		load_decos("groupbg", groupbg);
	} catch (const std::exception& e) {
		if (err) *err = std::string("passive_tree_poe1.json parse error: ") + e.what();
		return false;
	}
	if (nodes.empty()) {
		if (err) *err = "passive_tree_poe1.json has no nodes";
		return false;
	}
	return true;
}

std::vector<int> PassiveTreeData::NodesInRadius(int socketNodeIdx, float radiusWorld) const
{
	std::vector<int> out;
	if (socketNodeIdx < 0 || socketNodeIdx >= (int)nodes.size()) return out;
	const PtNode& s = nodes[socketNodeIdx];
	const float r2 = radiusWorld * radiusWorld;
	for (int i = 0; i < (int)nodes.size(); i++) {
		if (i == socketNodeIdx) continue;
		const PtNode& n = nodes[i];
		if (n.kind == kPtSocket) continue;  // POB excludes sockets (and masteries, which we store separately)
		float dx = n.x - s.x, dy = n.y - s.y;
		if (dx * dx + dy * dy <= r2) out.push_back(i);
	}
	return out;
}

int PassiveTreeData::IndexOfId(int id) const
{
	for (int i = 0; i < (int)nodes.size(); i++)
		if (nodes[i].id == id) return i;
	return -1;
}

// ---- selftest ---------------------------------------------------------------

static void ptlog(FILE* f, const char* fmt, ...)
{
	va_list ap; va_start(ap, fmt);
	vprintf(fmt, ap); va_end(ap);
	if (f) { va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); }
}

int RunPassiveTreeSelfTest(const std::wstring& exeDir)
{
	AllocConsole();
	FILE* dummy; freopen_s(&dummy, "CONOUT$", "w", stdout);
	FILE* rep = nullptr;
	_wfopen_s(&rep, (exeDir + L"passive_tree_selftest.txt").c_str(), L"w");

	int pass = 0, fail = 0;
	auto check = [&](bool ok, const char* name, const std::string& detail) {
		ptlog(rep, "[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name,
		      detail.empty() ? "" : "  ", detail.c_str());
		ok ? pass++ : fail++;
	};

	PassiveTreeData d;
	std::string err;
	if (!d.Load(exeDir, &err)) {
		ptlog(rep, "[FAIL] load  %s\n", err.c_str());
		if (rep) fclose(rep);
		return 1;
	}
	ptlog(rep, "tree %s (version %s), %d nodes, %d edges, %d sockets\n",
	      d.TreeVersion().c_str(), d.Version().c_str(),
	      (int)d.nodes.size(), (int)d.edges.size(), (int)d.sockets.size());

	check(d.nodes.size() > 1500, "node count", std::to_string(d.nodes.size()));
	// the PoE1 main tree carries 21 jewel sockets (cluster sockets live in proxy
	// groups and are filtered); 20 leaves headroom for GGG adding one
	check(d.sockets.size() >= 20, "socket count", std::to_string(d.sockets.size()));
	check(!d.sheets.empty() && !d.TreeVersion().empty(), "sheets+version", d.TreeVersion());

	// every edge endpoint valid + adjacency symmetric
	bool adjOk = true;
	for (const PtEdge& e : d.edges)
		if (e.a < 0 || e.b < 0 || e.a >= (int)d.nodes.size() || e.b >= (int)d.nodes.size())
			adjOk = false;
	check(adjOk, "edge endpoints in range", "");

	// icons present for non-socket nodes
	int missingIcon = 0;
	for (const PtNode& n : d.nodes)
		if (n.kind != kPtSocket && (!n.on.valid || !n.off.valid)) missingIcon++;
	check(missingIcon == 0, "non-socket icons present", "missing=" + std::to_string(missingIcon));

	// frames for the three interactive kinds
	bool framesOk = d.frames[kPtNormal].off.valid && d.frames[kPtNotable].off.valid &&
	                d.frames[kPtKeystone].off.valid;
	check(framesOk, "frames present", "");

	// radius query sanity: a typical socket has a plausible in-radius count for
	// the Large (timeless) radius of 1800 world units
	if (!d.sockets.empty()) {
		int total = 0, nonEmpty = 0, maxCnt = 0;
		for (int s : d.sockets) {
			int c = (int)d.NodesInRadius(s, 1800.0f).size();
			total += c; if (c > 0) nonEmpty++; if (c > maxCnt) maxCnt = c;
		}
		check(nonEmpty >= (int)d.sockets.size() - 4 && maxCnt < 120,
		      "radius(1800) query", "nonEmpty=" + std::to_string(nonEmpty) +
		      " max=" + std::to_string(maxCnt));
	}

	// a couple of Traditional-Chinese names baked in
	int zhNames = 0;
	for (const PtNode& n : d.nodes) if (!n.nameZh.empty()) zhNames++;
	check(zhNames > (int)d.nodes.size() * 8 / 10, "zh names >80%",
	      std::to_string(zhNames) + "/" + std::to_string((int)d.nodes.size()));

	// Traditional-Chinese stat-line coverage (baked statsZh; a new league drops
	// this until the dictionaries catch up — the TJ toolbar surfaces the same %)
	{
		int lines = 0, zhLines = 0;
		for (const PtNode& n : d.nodes)
			for (size_t i = 0; i < n.stats.size(); i++) {
				lines++;
				if (i < n.statsZh.size() && !n.statsZh[i].empty()) zhLines++;
			}
		check(lines > 0 && zhLines * 10 >= lines * 8, "zh stat lines >80%",
		      std::to_string(zhLines) + "/" + std::to_string(lines));
	}

	// Timeless-jewel LUT coverage: every non-socket tree node id should appear in
	// timeless_jewels.json's nodeIndex, else jewel search silently skips it (the
	// jewel dataset ships from PoB and can lag a new league's tree). >=90% keeps
	// the drift visible without failing on a handful of brand-new nodes.
	{
		TJDataset ds;
		std::string terr;
		if (ds.Load(exeDir + L"Data\\timeless_jewels.json", &terr)) {
			int total = 0, covered = 0;
			for (const PtNode& n : d.nodes) {
				if (n.kind == kPtSocket) continue;
				total++;
				if (ds.nodeIndex.count(n.id)) covered++;
			}
			check(total > 0 && covered * 10 >= total * 9, "timeless nodeIndex coverage >=90%",
			      std::to_string(covered) + "/" + std::to_string(total));
		} else {
			ptlog(rep, "[note] timeless_jewels.json absent (%s) - coverage check skipped\n", terr.c_str());
		}
	}

	ptlog(rep, "\n%d passed, %d failed\n", pass, fail);
	if (rep) fclose(rep);
	return fail == 0 ? 0 : 1;
}
