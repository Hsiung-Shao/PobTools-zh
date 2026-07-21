#include "timeless_jewel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::json (deps/nlohmann)

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <unordered_map>

using nlohmann::json;

// ---- file IO ------------------------------------------------------------------

static bool read_file_bytes(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 31)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		// ReadFile caps at ~4 GB; our files are < 2 GB and read in one call is fine here,
		// but loop to be safe on large .bin files.
		size_t done = 0;
		ok = true;
		while (done < out.size()) {
			DWORD chunk = (DWORD)((out.size() - done > 0x40000000) ? 0x40000000 : (out.size() - done));
			if (!ReadFile(h, &out[done], chunk, &read, nullptr) || read == 0) { ok = false; break; }
			done += read;
		}
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

// ---- dataset ------------------------------------------------------------------

static TJEntry parse_entry(const json& e)
{
	TJEntry o;
	o.dn = e.value("dn", std::string());
	o.dnZh = e.value("dnZh", std::string());
	o.id = e.value("id", std::string());
	if (e.contains("sd") && e["sd"].is_array())
		for (auto& s : e["sd"]) if (s.is_string()) o.sd.push_back(s.get<std::string>());
	if (e.contains("sdZh") && e["sdZh"].is_array())
		for (auto& s : e["sdZh"]) if (s.is_string()) o.sdZh.push_back(s.get<std::string>());
	if (e.contains("sortedStats") && e["sortedStats"].is_array())
		for (auto& s : e["sortedStats"]) if (s.is_string()) o.sortedStats.push_back(s.get<std::string>());
	o.ks = e.value("ks", false);
	if (e.contains("stats") && e["stats"].is_object()) {
		for (auto it = e["stats"].begin(); it != e["stats"].end(); ++it) {
			const json& sm = it.value();
			TJStatMod m;
			m.fmt = sm.value("fmt", std::string("d"));
			m.index = sm.value("index", 1);
			m.min = sm.value("min", 0.0);
			m.max = sm.value("max", 0.0);
			o.stats[it.key()] = m;
		}
	}
	return o;
}

bool TJDataset::Load(const std::wstring& jsonPath, std::string* err)
{
	std::string content;
	if (!read_file_bytes(jsonPath, content)) {
		if (err) *err = u8"找不到 timeless_jewels.json";
		return false;
	}
	try {
		json doc = json::parse(content);
		additionsOffset = doc.value("/meta/additionsOffset"_json_pointer, 96);
		for (auto it = doc["types"].begin(); it != doc["types"].end(); ++it)
			types[std::stoi(it.key())] = it.value().get<std::string>();
		if (doc.contains("conqType"))
			for (auto it = doc["conqType"].begin(); it != doc["conqType"].end(); ++it)
				conqType[std::stoi(it.key())] = it.value().get<std::string>();
		if (doc.contains("conquerors"))
			for (auto it = doc["conquerors"].begin(); it != doc["conquerors"].end(); ++it) {
				auto& vec = conquerors[std::stoi(it.key())];
				for (const auto& c : it.value()) {
					TJConqueror q;
					q.id = c.value("id", std::string());
					q.name = c.value("name", std::string());
					q.nameZh = c.value("nameZh", std::string());
					q.trade = c.value("trade", std::string());
					vec.push_back(std::move(q));
				}
			}
		for (auto it = doc["seedMin"].begin(); it != doc["seedMin"].end(); ++it)
			seedMin[std::stoi(it.key())] = it.value().get<int>();
		for (auto it = doc["seedMax"].begin(); it != doc["seedMax"].end(); ++it)
			seedMax[std::stoi(it.key())] = it.value().get<int>();

		const json& ni = doc["nodeIndex"];
		size = ni.value("size", 0);
		sizeNotable = ni.value("sizeNotable", 0);
		for (auto it = ni["map"].begin(); it != ni["map"].end(); ++it) {
			const auto& v = it.value();
			nodeIndex[std::stoi(it.key())] = { v[0].get<int>(), v[1].get<int>() };
		}
		if (ni.contains("localToGlobal")) {
			for (auto jt = ni["localToGlobal"].begin(); jt != ni["localToGlobal"].end(); ++jt) {
				auto& m = localToGlobal[std::stoi(jt.key())];
				for (auto p = jt.value().begin(); p != jt.value().end(); ++p)
					m[std::stoi(p.key())] = p.value().get<int>();
			}
		}
		for (auto& e : doc["additions"]) additions.push_back(e.is_null() ? TJEntry{} : parse_entry(e));
		for (auto& e : doc["nodes"]) nodes.push_back(e.is_null() ? TJEntry{} : parse_entry(e));
		return true;
	} catch (const std::exception& ex) {
		if (err) *err = std::string(u8"解析 timeless_jewels.json 失敗: ") + ex.what();
		return false;
	}
}

int TJDataset::L2G(int jewelType, int localId) const
{
	auto jt = localToGlobal.find(jewelType);
	if (jt != localToGlobal.end()) {
		auto p = jt->second.find(localId);
		if (p != jt->second.end()) return p->second;
	}
	return localId; // identity when not remapped
}

// ---- LUT read -----------------------------------------------------------------

static inline int ub(const std::string& s, size_t i)
{
	return i < s.size() ? (unsigned char)s[i] : 0;
}

std::vector<int> TJReadLUT(const TJDataset& ds, const std::string& blob,
                           int jewelType, int seed, int nodeId)
{
	std::vector<int> result;
	auto itMin = ds.seedMin.find(jewelType), itMax = ds.seedMax.find(jewelType);
	auto itNode = ds.nodeIndex.find(nodeId);
	if (itMin == ds.seedMin.end() || itMax == ds.seedMax.end() || itNode == ds.nodeIndex.end())
		return result;

	if (jewelType == 5) seed = seed / 20; // Elegant Hubris
	const int seedMin = itMin->second, seedMax = itMax->second;
	const int seedSize = seedMax - seedMin + 1;
	const int seedOffset = seed - seedMin;
	if (seedOffset < 0 || seedOffset >= seedSize) return result;
	const int index = itNode->second.first;

	if (jewelType == 1) {
		// Glorious Vanity: variable-length per (node, seed) chunk.
		const long long headerLen = (long long)ds.size * seedSize; // one size-byte per node*seed
		// node data blobs are concatenated in index order; find this node's start.
		std::vector<int> byteSizeByIndex(ds.size, 0);
		for (const auto& kv : ds.nodeIndex)
			if (kv.second.first >= 0 && kv.second.first < ds.size)
				byteSizeByIndex[kv.second.first] = kv.second.second;
		long long nodeStart = headerLen;
		for (int i = 0; i < index; i++) nodeStart += byteSizeByIndex[i];
		// within the node blob, sum chunk lengths up to this seed
		long long chunkOffset = 0;
		for (int k = 0; k < seedOffset; k++)
			chunkOffset += ub(blob, (size_t)((long long)index * seedSize + k));
		const int dataLength = ub(blob, (size_t)((long long)index * seedSize + seedOffset));
		for (int i = 0; i < dataLength; i++)
			result.push_back(ub(blob, (size_t)(nodeStart + chunkOffset + i)));
		// map local ids to global (replacement in first byte, or first half for might/legacy)
		if (dataLength == 2 || dataLength == 3) {
			result[0] = ds.L2G(jewelType, result[0]);
		} else if (dataLength == 6 || dataLength == 8) {
			for (int i = 0; i < dataLength / 2; i++) result[i] = ds.L2G(jewelType, result[i]);
		}
		return result;
	}

	// Non-GV: only notables have LUT entries; small nodes are handled elsewhere.
	if (index <= ds.sizeNotable) {
		const int localId = ub(blob, (size_t)((long long)index * seedSize + seedOffset));
		result.push_back(ds.L2G(jewelType, localId));
	}
	return result;
}

// ---- transform application ----------------------------------------------------

static std::string fmt_num(double v)
{
	if (std::fabs(v - std::round(v)) < 1e-9) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%lld", (long long)std::llround(v));
		return buf;
	}
	char buf[32];
	snprintf(buf, sizeof(buf), "%g", v);
	return buf;
}

static void replace_all_str(std::string& s, const std::string& from, const std::string& to)
{
	if (from.empty()) return;
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
}

// Substitute a roll value into a stat template, mirroring PoB's replaceHelperFunc.
static std::string roll_stat(std::string sd, const TJStatMod& m, double value)
{
	if (m.min != m.max) {
		replace_all_str(sd, "(" + fmt_num(m.min) + "-" + fmt_num(m.max) + ")", fmt_num(value));
	} else if (m.min != value) {
		replace_all_str(sd, fmt_num(m.min), fmt_num(value));
	}
	return sd;
}

static const TJEntry* node_at(const TJDataset& ds, int global)
{
	const int i = global - ds.additionsOffset; // Lua nodes[global+1-96] -> [global-96]
	if (i >= 0 && i < (int)ds.nodes.size()) return &ds.nodes[i];
	return nullptr;
}
static const TJEntry* addition_at(const TJDataset& ds, int global)
{
	if (global >= 0 && global < (int)ds.additions.size()) return &ds.additions[global];
	return nullptr;
}

// Emit one stat line (English + baked Chinese) from an entry, optionally rolling
// a range/value into both.
static void push_line(TJTransform& out, const TJEntry& e, size_t i,
                      const TJStatMod* sm = nullptr, double value = 0.0)
{
	std::string en = (i < e.sd.size()) ? e.sd[i] : std::string();
	std::string zh = (i < e.sdZh.size()) ? e.sdZh[i] : std::string();
	if (sm) {
		en = roll_stat(en, *sm, value);
		if (!zh.empty()) zh = roll_stat(zh, *sm, value);
	}
	out.lines.push_back(en);
	out.linesZh.push_back(zh);
}

TJTransform TJApply(const TJDataset& ds, const std::string& blob,
                    int jewelType, int seed, int nodeId, const std::string& nodeType,
                    const std::vector<std::string>& origSd,
                    const std::string& conquerorType, const std::string& conquerorId,
                    const std::string& nodeName)
{
	(void)origSd; (void)nodeName;
	TJTransform out;

	if (nodeType == "Keystone") {
		std::string m = conquerorType + "_keystone_" + (conquerorId.empty() ? "1" : conquerorId);
		for (const auto& n : ds.nodes) {
			if (n.id == m) {
				out.ok = out.replaced = true;
				out.newName = n.dn;
				out.newNameZh = n.dnZh;
				for (size_t i = 0; i < n.sd.size(); i++) push_line(out, n, i);
				return out;
			}
		}
		out.note = std::string("keystone not found: ") + m;
		return out;
	}

	std::vector<int> lut = TJReadLUT(ds, blob, jewelType, seed, nodeId);
	if (lut.empty()) {
		out.note = "no LUT result";
		return out;
	}

	if (jewelType == 1) {
		const int hs = (int)lut.size();
		if (hs == 2 || hs == 3) {
			const TJEntry* n = node_at(ds, lut[0]);
			if (!n) { out.note = "GV replace id out of range"; return out; }
			out.ok = out.replaced = true;
			out.newName = n->dn;
			out.newNameZh = n->dnZh;
			for (size_t i = 0; i < n->sd.size(); i++) {
				const TJStatMod* sm = nullptr;
				double val = 0;
				if (i < n->sortedStats.size()) {
					auto it = n->stats.find(n->sortedStats[i]);
					if (it != n->stats.end() && it->second.index < (int)lut.size()) {
						sm = &it->second;
						val = (double)lut[it->second.index];
					}
				}
				push_line(out, *n, i, sm, val);
			}
			return out;
		} else if (hs == 6 || hs == 8) {
			int bias = 0;
			for (int i = 0; i < hs / 2; i++) bias += (lut[i] <= 21) ? 1 : -1;
			const TJEntry* n = (bias >= 0) ? node_at(ds, 96 + 77 - 1) : node_at(ds, 96 + 78 - 1);
			// legionNodes[77]/[78] (Lua 1-based) -> nodes[76]/[77]; node_at expects global,
			// so pass global = 96 + (index-1). 77 -> global 96+76.
			out.ok = out.replaced = true;
			out.newName = n ? n->dn : (bias >= 0 ? "Might of the Vaal" : "Legacy of the Vaal");
			if (n) out.newNameZh = n->dnZh;
			// combine additions (first half = ids, second half = rolls)
			std::map<int, int> adds;
			for (int i = 0; i < hs / 2; i++) {
				int add = lut[i], roll = lut[i + hs / 2];
				adds[add] = adds.count(add) ? adds[add] + roll : roll;
			}
			for (auto& kv : adds) {
				const TJEntry* a = addition_at(ds, kv.first);
				if (!a) continue;
				const TJStatMod* sm = a->stats.empty() ? nullptr : &a->stats.begin()->second;
				for (size_t i = 0; i < a->sd.size(); i++) push_line(out, *a, i, sm, (double)kv.second);
			}
			return out;
		}
		out.note = "unhandled GV headerSize " + std::to_string(hs);
		return out;
	}

	// Non-GV notable: each byte is either a replacement (>= offset) or an addition.
	out.ok = true;
	for (int g : lut) {
		if (g >= ds.additionsOffset) {
			const TJEntry* n = node_at(ds, g);
			if (n) {
				out.replaced = true;
				out.newName = n->dn;
				out.newNameZh = n->dnZh;
				for (size_t i = 0; i < n->sd.size(); i++) push_line(out, *n, i);
			} else {
				out.note += "unhandled replace id " + std::to_string(g) + "; ";
			}
		} else {
			const TJEntry* a = addition_at(ds, g);
			if (a) for (size_t i = 0; i < a->sd.size(); i++) push_line(out, *a, i);
			else out.note += "unhandled add id " + std::to_string(g) + "; ";
		}
	}
	return out;
}

// ---- seed search --------------------------------------------------------------

std::string TJNormalizeStat(const std::string& s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size();) {
		if (isdigit((unsigned char)s[i])) {
			out.push_back('#');
			while (i < s.size() && (isdigit((unsigned char)s[i]) || s[i] == '.')) i++;
		} else {
			out.push_back(s[i]);
			i++;
		}
	}
	return out;
}

static double first_num(const std::string& s)
{
	for (size_t i = 0; i < s.size(); i++)
		if (isdigit((unsigned char)s[i])) return strtod(s.c_str() + i, nullptr);
	return 0.0;
}

std::vector<TJStatTemplate> TJStatTemplates(const TJDataset& ds, int jewelType)
{
	// each jewel only produces entries whose id carries its conqueror-type prefix
	// (e.g. Brutal Restraint -> "maraketh_..."); Heroic Tragedy uses "kalguuran_"
	// which still begins with the "kalguur" token.
	std::string prefix;
	auto itT = ds.conqType.find(jewelType);
	if (jewelType != 0 && itT != ds.conqType.end()) prefix = itT->second;

	std::map<std::string, std::string> uniq; // en template -> zh template (first seen)
	auto add = [&](const std::vector<TJEntry>& v) {
		for (const auto& e : v) {
			if (!prefix.empty() && e.id.rfind(prefix, 0) != 0) continue; // other jewel
			for (size_t i = 0; i < e.sd.size(); i++) {
				if (e.sd[i].empty()) continue;
				std::string en = TJNormalizeStat(e.sd[i]);
				std::string zh = (i < e.sdZh.size() && !e.sdZh[i].empty())
				                 ? TJNormalizeStat(e.sdZh[i]) : std::string();
				auto it = uniq.find(en);
				if (it == uniq.end()) uniq[en] = zh;
				else if (it->second.empty() && !zh.empty()) it->second = zh;
			}
		}
	};
	add(ds.additions);
	add(ds.nodes);
	std::vector<TJStatTemplate> out;
	out.reserve(uniq.size());
	for (auto& kv : uniq) out.push_back({ kv.first, kv.second });
	// sort by Chinese (fallback English) for a friendlier picker
	std::sort(out.begin(), out.end(), [](const TJStatTemplate& a, const TJStatTemplate& b) {
		const std::string& ka = a.zh.empty() ? a.en : a.zh;
		const std::string& kb = b.zh.empty() ? b.en : b.zh;
		return ka < kb;
	});
	return out;
}

std::vector<TJSeedHit> TJSearch(const TJDataset& ds, const std::string& blob,
                                const TJSearchQuery& q, int topN, const volatile bool* cancel)
{
	std::vector<TJSeedHit> hits;
	auto itMin = ds.seedMin.find(q.jewelType), itMax = ds.seedMax.find(q.jewelType);
	if (itMin == ds.seedMin.end() || itMax == ds.seedMax.end() || q.wants.empty())
		return hits;

	std::unordered_map<std::string, const TJWantStat*> want;
	for (const auto& w : q.wants) want[w.tmpl] = &w;

	// candidate nodes: the socket's in-radius set if given, else every indexed
	// node. Each carries its type so smalls roll additions, notables roll nodes.
	std::vector<std::pair<int, bool>> scopeNodes; // (nodeId, notable)
	auto consider = [&](int nodeId, int idxInBin) {
		const bool notable = idxInBin <= ds.sizeNotable;
		if (q.scope == 1 && !notable) return;
		if (q.scope == 2 && notable) return;
		scopeNodes.push_back({ nodeId, notable });
	};
	if (!q.nodeIds.empty()) {
		for (int id : q.nodeIds) {
			auto it = ds.nodeIndex.find(id);
			if (it != ds.nodeIndex.end()) consider(id, it->second.first);
		}
	} else {
		for (const auto& kv : ds.nodeIndex) consider(kv.first, kv.second.first);
	}

	const bool eh = (q.jewelType == 5);
	const int lo = eh ? itMin->second * 20 : itMin->second;
	const int hi = eh ? itMax->second * 20 : itMax->second;
	const int step = eh ? 20 : 1;

	for (int seed = lo; seed <= hi; seed += step) {
		if (cancel && *cancel) break;
		double total = 0;
		int matches = 0;
		std::set<const TJWantStat*> distinct;
		for (const auto& nd : scopeNodes) {
			TJTransform t = TJApply(ds, blob, q.jewelType, seed, nd.first,
			                        nd.second ? "Notable" : "Normal", {});
			if (!t.ok) continue;
			for (const auto& line : t.lines) {
				auto it = want.find(TJNormalizeStat(line));
				if (it != want.end() && first_num(line) >= it->second->minValue) {
					total += it->second->weight;
					matches++;
					distinct.insert(it->second);
				}
			}
		}
		if (matches > 0 && total >= q.minTotalWeight)
			hits.push_back({ seed, total, matches, (int)distinct.size() });
	}

	std::sort(hits.begin(), hits.end(),
	          [](const TJSeedHit& a, const TJSeedHit& b) { return a.weight > b.weight; });
	if (topN > 0 && (int)hits.size() > topN) hits.resize(topN);
	return hits;
}

// ---- bin loading --------------------------------------------------------------

bool TJLoadBin(const std::wstring& exeDir, const TJDataset& ds, int jewelType,
               std::string& out, std::string* err)
{
	auto it = ds.types.find(jewelType);
	if (it == ds.types.end()) { if (err) *err = u8"未知珠寶型別"; return false; }
	std::string nameA; // ascii jewel name without spaces (e.g. "BrutalRestraint")
	std::wstring nameW;
	for (char c : it->second) if (c != ' ') { nameA.push_back(c); nameW.push_back((wchar_t)(unsigned char)c); }
	std::wstring path = exeDir + L"PathOfBuildingCommunity\\Data\\TimelessJewelData\\" + nameW + L".bin";
	if (!read_file_bytes(path, out)) {
		if (err) *err = u8"找不到 " + nameA +
		                u8".bin(需要旁邊有 PathOfBuildingCommunity;Glorious Vanity 首次需先讓 POB 解壓 .zip 產生 .bin)";
		return false;
	}
	return true;
}

// ---- CLIs ---------------------------------------------------------------------

static void ensure_console()
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
}

static bool load_dataset(const std::wstring& exeDir, TJDataset& ds, std::string* err)
{
	return ds.Load(exeDir + L"Data\\timeless_jewels.json", err);
}

int RunTimelessJewelCli(const std::wstring& exeDir, int jewelType, int seed, int nodeId)
{
	ensure_console();
	TJDataset ds;
	std::string err;
	if (!load_dataset(exeDir, ds, &err)) { printf("%s\n", err.c_str()); return 1; }
	std::string blob;
	if (!TJLoadBin(exeDir, ds, jewelType, blob, &err)) { printf("%s\n", err.c_str()); return 1; }

	std::string type = ds.types.count(jewelType) ? ds.types[jewelType] : "?";
	auto ni = ds.nodeIndex.find(nodeId);
	std::string nodeType = (ni != ds.nodeIndex.end() && ni->second.first <= ds.sizeNotable)
	                       ? "Notable" : "Normal";
	printf("%s  seed=%d  node=%d (%s)\n", type.c_str(), seed, nodeId, nodeType.c_str());

	std::vector<int> lut = TJReadLUT(ds, blob, jewelType, seed, nodeId);
	printf("  LUT bytes:");
	for (int b : lut) printf(" %d", b);
	printf("\n");

	TJTransform t = TJApply(ds, blob, jewelType, seed, nodeId, nodeType, {});
	if (!t.ok) { printf("  (no transform: %s)\n", t.note.c_str()); return 0; }
	if (t.replaced) printf("  -> %s\n", t.newName.c_str());
	for (const auto& l : t.lines) printf("     %s\n", l.c_str());
	if (!t.note.empty()) printf("  note: %s\n", t.note.c_str());
	return 0;
}

int RunTimelessJewelSelfTest(const std::wstring& exeDir)
{
	ensure_console();
	TJDataset ds;
	std::string err;
	if (!load_dataset(exeDir, ds, &err)) { printf("FAIL load: %s\n", err.c_str()); return 1; }

	int failures = 0;
	std::string report;
	auto check = [&](bool ok, const std::string& what, const std::string& detail = "") {
		std::string line = std::string(ok ? "PASS  " : "FAIL  ") + what +
		                   (detail.empty() ? "" : "  -> " + detail) + "\n";
		report += line;
		printf("%s", line.c_str());
		if (!ok) failures++;
	};

	// dataset shape
	check(ds.additions.size() == 96, "additions count", std::to_string(ds.additions.size()));
	check(ds.nodes.size() >= 100, "nodes count", std::to_string(ds.nodes.size()));
	check(ds.size == 1931 && ds.sizeNotable == 452, "node index sizes",
	      std::to_string(ds.size) + "/" + std::to_string(ds.sizeNotable));
	check(ds.additions.size() > 79 && ds.additions[79].dn == "Add Poison Damage",
	      "addition 79 = Add Poison Damage");

	// Brutal Restraint byte-math must match the Python prototype exactly.
	std::string blob;
	if (TJLoadBin(exeDir, ds, 3, blob, &err)) {
		struct Case { int seed; int node; int expectByte; const char* expectDn; };
		const Case cases[] = {
			{ 500, 6, 79, "Add Poison Damage" },
			{ 5000, 6, 70, "Add Flask Charges" },
			{ 8000, 6, 68, "Add Percent Dexterity" },
			{ 500, 529, 85, "Add Ailment Duration" },
			{ 5000, 529, 67, "Add Dexterity" },
			{ 500, 544, 78, "Add Global Crit Chance" },
		};
		for (const Case& c : cases) {
			auto lut = TJReadLUT(ds, blob, 3, c.seed, c.node);
			bool ok = lut.size() == 1 && lut[0] == c.expectByte;
			std::string dn = (ok && lut[0] < (int)ds.additions.size()) ? ds.additions[lut[0]].dn : "?";
			check(ok && dn == c.expectDn,
			      std::string("BR seed ") + std::to_string(c.seed) + " node " + std::to_string(c.node),
			      "byte=" + (lut.empty() ? std::string("none") : std::to_string(lut[0])) + " " + dn);
		}
		// full transform (TJApply) must emit the addition's stat line
		TJTransform t = TJApply(ds, blob, 3, 500, 6, "Notable", {});
		bool tok = t.ok && !t.lines.empty() && t.lines[0] == "20% increased Damage with Poison";
		check(tok, "TJApply BR 500 node6 stat line (en)",
		      t.lines.empty() ? "(no lines)" : t.lines[0]);
		bool tzh = !t.linesZh.empty() && t.linesZh[0] == u8"增加 20% 中毒傷害";
		check(tzh, "TJApply BR 500 node6 stat line (zh)",
		      t.linesZh.empty() ? "(no zh)" : t.linesZh[0]);

		// normalization + stat-template list
		check(TJNormalizeStat("20% increased Damage with Poison") == "#% increased Damage with Poison",
		      "normalize turns numbers into #");
		check(TJStatTemplates(ds).size() > 50, "stat template list built",
		      std::to_string(TJStatTemplates(ds).size()));

		// batch search: BR for the poison template must rank seed 500 as a hit
		TJSearchQuery q;
		q.jewelType = 3;
		q.scope = 1; // notables
		q.wants.push_back({ "#% increased Damage with Poison", 0.0, 1.0 });
		ULONGLONG t0 = GetTickCount64();
		auto hits = TJSearch(ds, blob, q, 2000, nullptr);
		ULONGLONG dt = GetTickCount64() - t0;
		bool has500 = false;
		for (const auto& h : hits) if (h.seed == 500) has500 = true;
		check(!hits.empty() && has500, "search BR poison finds seed 500",
		      std::to_string(hits.size()) + " hits in " + std::to_string(dt) + " ms");
	} else {
		check(false, "load BrutalRestraint.bin", err);
	}

	std::string tail = failures == 0 ? "\nALL PASS\n" : "\nFAILURES: " + std::to_string(failures) + "\n";
	report += tail;
	printf("%s", tail.c_str());

	HANDLE h = CreateFileW((exeDir + L"tj_selftest.txt").c_str(), GENERIC_WRITE, 0,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &written, nullptr);
		CloseHandle(h);
	}
	return failures == 0 ? 0 : 1;
}
