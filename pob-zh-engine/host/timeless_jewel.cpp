#include "timeless_jewel.h"
#include "timeless_jewel_abyss.h" // engine boundary assertions in the selftest
#include "launcher_config.h" // FindPoe1Dir
#include "passive_tree_data.h" // node names for the --tj-verify dump

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "timeless_jewel_ui.h" // trade URL assembly (selftest)

#include <json.hpp> // nlohmann::json (deps/nlohmann)
#include <miniz.h>  // raw-zlib inflate for the shipped .zip LUTs

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

// Last-write time as a comparable integer; 0 when the file does not exist.
static uint64_t file_mtime(const std::wstring& path)
{
	WIN32_FILE_ATTRIBUTE_DATA fad{};
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return 0;
	return ((uint64_t)fad.ftLastWriteTime.dwHighDateTime << 32) | fad.ftLastWriteTime.dwLowDateTime;
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
		if (doc.contains("binNames"))
			for (auto it = doc["binNames"].begin(); it != doc["binNames"].end(); ++it)
				binNames[std::stoi(it.key())] = it.value().get<std::string>();
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

	// Non-GV: only notables have LUT entries; small nodes are handled in TJApply.
	// The table holds exactly sizeNotable rows (0 .. sizeNotable-1), so PoB's
	// `index <= sizeNotable` reads one row past the end. Lua turns that into nil
	// and the caller then reports "Missing LUT" and leaves the node alone, so the
	// strict `<` here reproduces PoB's behaviour without relying on a byte that
	// isn't there — reading it would yield 0, which L2G happily maps onto a real
	// keystone.
	if (index < ds.sizeNotable) {
		const size_t off = (size_t)((long long)index * seedSize + seedOffset);
		if (off >= blob.size()) return result; // truncated/stale LUT: no answer, not a wrong one
		result.push_back(ds.L2G(jewelType, ub(blob, off)));
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
//
// The "g"-format branch is not cosmetic. Those stats are stored in game units
// and written in display units: life regeneration is stored per minute and
// printed per second, leech is stored in permyriad and printed as a percent.
// Skipping it turns "Regenerate (0.7-1.2)% of Life per second" into
// "Regenerate 60% of Life per second". 31 entries in the dataset carry such a
// stat, and 11 of them belong to Legion jewels — this was wrong there too, not
// only for the Abyss ones that made it visible.
std::string TJRollStat(const std::string& sdIn, const std::string& statKey,
                       const TJStatMod& m, double value)
{
	std::string sd = sdIn;
	if (m.fmt == "g") {
		if (statKey.find("per_minute") != std::string::npos)
			value = std::floor(value / 60.0 * 10.0 + 0.5) / 10.0; // PoB rounds to 1 dp
		else if (statKey.find("permyriad") != std::string::npos)
			value = value / 100.0;
		else if (statKey.find("_ms") != std::string::npos)
			value = value / 1000.0;
	}
	if (m.min != m.max) {
		replace_all_str(sd, "(" + fmt_num(m.min) + "-" + fmt_num(m.max) + ")", fmt_num(value));
	} else if (m.min != value) {
		replace_all_str(sd, fmt_num(m.min), fmt_num(value));
	}
	return sd;
}

// Older internal callers pass the stat key alongside; keep the short form for
// the places that already looked the mod up by index.
static std::string roll_stat(const std::string& sd, const std::string& statKey,
                             const TJStatMod& m, double value)
{
	return TJRollStat(sd, statKey, m, value);
}

const TJEntry* TJNodeAt(const TJDataset& ds, int global)
{
	const int i = global - ds.additionsOffset; // Lua nodes[global+1-additions] -> [global-additions]
	if (i >= 0 && i < (int)ds.nodes.size()) return &ds.nodes[i];
	return nullptr;
}
const TJEntry* TJAdditionAt(const TJDataset& ds, int global)
{
	if (global >= 0 && global < (int)ds.additions.size()) return &ds.additions[global];
	return nullptr;
}
static const TJEntry* node_at(const TJDataset& ds, int global) { return TJNodeAt(ds, global); }
static const TJEntry* addition_at(const TJDataset& ds, int global) { return TJAdditionAt(ds, global); }

// Emit one stat line (English + baked Chinese) from an entry, optionally rolling
// a range/value into both.
// statKey is needed alongside the mod: the unit scaling for "g"-format stats is
// selected by the key's name, not by anything inside TJStatMod.
static void push_line(TJTransform& out, const TJEntry& e, size_t i,
                      const TJStatMod* sm = nullptr, double value = 0.0,
                      const std::string& statKey = std::string())
{
	std::string en = (i < e.sd.size()) ? e.sd[i] : std::string();
	std::string zh = (i < e.sdZh.size()) ? e.sdZh[i] : std::string();
	if (sm) {
		en = roll_stat(en, statKey, *sm, value);
		if (!zh.empty()) zh = roll_stat(zh, statKey, *sm, value);
	}
	out.lines.push_back(en);
	out.linesZh.push_back(zh);
}

TJPaste TJParsePaste(const TJDataset& ds, const std::string& text)
{
	TJPaste out;
	size_t namePos = std::string::npos;
	// pass 0 = English name, pass 1 = Chinese (only if English found nothing)
	for (int pass = 0; pass < 2 && !out.jewelType; pass++) {
		for (const auto& kv : ds.conquerors) {
			for (int i = 0; i < (int)kv.second.size(); i++) {
				const std::string& nm = pass == 0 ? kv.second[i].name : kv.second[i].nameZh;
				size_t p = nm.empty() ? std::string::npos : text.find(nm);
				if (p != std::string::npos) {
					out.jewelType = kv.first;
					out.conqIndex = i;
					namePos = p;
				}
			}
		}
	}
	// seed = largest number on the line naming the conqueror (whole text if none),
	// which skips item level / "Limited to: 1 Historic" and similar noise
	std::string scan = text;
	if (namePos != std::string::npos) {
		size_t a = text.rfind('\n', namePos);
		a = (a == std::string::npos) ? 0 : a + 1;
		size_t b = text.find('\n', namePos);
		if (b == std::string::npos) b = text.size();
		scan = text.substr(a, b - a);
	}
	for (size_t p = 0; p < scan.size(); p++)
		if (isdigit((unsigned char)scan[p])) {
			int v = atoi(scan.c_str() + p);
			if (v > out.seed) out.seed = v;
			while (p < scan.size() && isdigit((unsigned char)scan[p])) p++;
		}
	return out;
}

// Per-family item wording, transcribed from the game's own stat descriptions
// (GGPK `local_unique_jewel_alternate_tree_*`). Note the two irregular ones:
// the Templar line is plural and the Eternal one names the empire, not a people.
namespace {
struct TJFlavour {
	const char* family;
	const char* seedLine;   // one %d (seed) then one %s (conqueror)
	const char* conquered;  // fills "Passives in radius are Conquered by the ..."
};
const TJFlavour kFlavour[] = {
	{ "vaal",     "Bathed in the blood of %d sacrificed in the name of %s",         "Vaal" },
	{ "karui",    "Commanded leadership over %d warriors under %s",                 "Karui" },
	{ "maraketh", "Denoted service of %d dekhara in the akhara of %s",              "Maraketh" },
	{ "templar",  "Carved to glorify %d new faithful converted by High Templar %s", "Templars" },
	{ "eternal",  "Commissioned %d coins to commemorate %s",                        "Eternal Empire" },
	{ "kalguur",  "Remembrancing %d songworthy deeds by the line of %s",            "Kalguur" },
};

// The Abyss jewels are shaped differently: an Eye Jewel base rather than
// "Timeless Jewel", no radius line, and "Passives affected" instead of
// "Passives in radius". Transcribed from PoB's own
// TimelessJewelListControl.lua, which is the text PoB feeds to its own parser —
// so anything accepted there is accepted here.
struct TJAbyssItem {
	int jewelType;
	const char* name;     // unique name
	const char* base;     // base item type
	const char* seedLine; // one %d (seed)
};
const TJAbyssItem kAbyssItems[] = {
	{ 7,  "Festering Vengeance",    "Murderous Eye Jewel",
	      "Subjugating %d souls in the thrall of Tecrod" },
	{ 8,  "Extinguishing Grasp",    "Searching Eye Jewel",
	      "Subjugating %d souls in the thrall of Ulaman" },
	{ 9,  "Baleful Dominion",       "Hypnotic Eye Jewel",
	      "Subjugating %d souls in the thrall of Kurgal" },
	{ 10, "Destructive Aspiration", "Ghastly Eye Jewel",
	      "Subjugating %d souls in the thrall of Amanamu" },
	{ 11, "Reclaimed Malevolence",  "Assembled Eye Jewel",
	      "Binding %d souls to phylacteries to sustain Zorath" },
};
} // namespace

std::string TJItemText(const TJDataset& ds, int jewelType, int conqIndex, int seed)
{
	auto itType = ds.types.find(jewelType);
	auto itFam = ds.conqType.find(jewelType);
	auto itConq = ds.conquerors.find(jewelType);
	if (itType == ds.types.end() || itFam == ds.conqType.end() || itConq == ds.conquerors.end())
		return std::string();
	if (conqIndex < 0 || conqIndex >= (int)itConq->second.size()) return std::string();

	for (const auto& a : kAbyssItems) {
		if (a.jewelType != jewelType) continue;
		char seedLine[160];
		snprintf(seedLine, sizeof(seedLine), a.seedLine, seed);
		std::string out = "Item Class: Jewels\nRarity: Unique\n";
		out += std::string(a.name) + "\n" + a.base + "\n";
		out += "--------\nLimited to: 1 Historic\n";
		out += "--------\n";
		out += seedLine;
		out += "\nPassives affected are Conquered by the Abyssal\nHistoric\n";
		out += "--------\nPlace into an allocated Jewel Socket on the Passive Skill Tree."
		       " Right click to remove from the Socket.\n";
		return out;
	}

	const TJFlavour* fl = nullptr;
	for (const auto& f : kFlavour) if (itFam->second == f.family) { fl = &f; break; }
	if (!fl) return std::string();

	char line[256];
	snprintf(line, sizeof(line), fl->seedLine, seed, itConq->second[conqIndex].name.c_str());

	// No "Item Level" or "Note" line: both exist on a real copy but neither is
	// ours to invent, and PoB ignores them for jewels.
	std::string out = "Item Class: Jewels\nRarity: Unique\n";
	out += itType->second + "\nTimeless Jewel\n";
	out += "--------\nLimited to: 1 Historic\nRadius: Large\n";
	out += "--------\n";
	out += line;
	out += "\nPassives in radius are Conquered by the ";
	out += fl->conquered;
	out += "\nHistoric\n";
	out += "--------\nPlace into an allocated Jewel Socket on the Passive Skill Tree."
	       " Right click to remove from the Socket.\n";
	return out;
}

TJTransform TJApply(const TJDataset& ds, const std::string& blob,
                    int jewelType, int seed, int nodeId, const std::string& nodeType,
                    const std::vector<std::string>& origSd,
                    const std::string& conquerorType, const std::string& conquerorId,
                    const std::string& nodeName)
{
	// origSd is not consumed here on purpose: `lines` carries only what the jewel
	// grants, so the seed search scores jewel mods rather than the node's own.
	// When `replaced` is false the caller must still show the node's original
	// stats — PoB appends additions to the node instead of wiping it.
	(void)origSd;
	TJTransform out;

	// Callers that only know the jewel (the seed search) may leave the conqueror
	// blank; the family is a property of the jewel, so recover it from the
	// dataset rather than silently skipping the per-family rules below.
	std::string cq = conquerorType;
	if (cq.empty()) {
		auto it = ds.conqType.find(jewelType);
		if (it != ds.conqType.end()) cq = it->second;
	}

	if (nodeType == "Keystone") {
		// Legion jewels have one keystone per conqueror (vaal_keystone_2), the
		// Abyss ones only have a single unsuffixed keystone per jewel.
		std::string m = cq + "_keystone_" + (conquerorId.empty() ? "1" : conquerorId);
		const std::string plain = cq + "_keystone";
		for (int pass = 0; pass < 2; pass++) {
			const std::string& want = pass == 0 ? m : plain;
			for (const auto& n : ds.nodes) {
				if (n.id == want) {
					out.ok = out.replaced = true;
					out.newName = n.dn;
					out.newNameZh = n.dnZh;
					for (size_t i = 0; i < n.sd.size(); i++) push_line(out, n, i);
					return out;
				}
			}
		}
		out.note = std::string("keystone not found: ") + m;
		return out;
	}

	// Small ("Normal") passives are only seed-driven under Glorious Vanity. Every
	// other conqueror applies one fixed rule to every small node in radius, and
	// halves the value on the three bare attribute nodes — PassiveSpec.lua's
	// `isValueInArray(attributes, node.dn)` branch. Tattoos are not modelled here.
	if (nodeType == "Normal" && cq != "vaal") {
		const bool attr = nodeName == "Strength" || nodeName == "Dexterity" ||
		                  nodeName == "Intelligence";
		auto lit = [&out](const char* en, const char* zh) {
			out.ok = true;
			out.lines.push_back(en);
			out.linesZh.push_back(zh);
		};
		auto replaceWith = [&](const char* id) {
			for (const auto& n : ds.nodes) {
				if (n.id != id) continue;
				out.ok = out.replaced = true;
				out.newName = n.dn;
				out.newNameZh = n.dnZh;
				for (size_t i = 0; i < n.sd.size(); i++) push_line(out, n, i);
				return true;
			}
			out.note = std::string("missing legion node: ") + id;
			return false;
		};
		if (cq == "karui")
			lit(attr ? "+2 to Strength" : "+4 to Strength", attr ? u8"+2 力量" : u8"+4 力量");
		else if (cq == "maraketh")
			lit(attr ? "+2 to Dexterity" : "+4 to Dexterity", attr ? u8"+2 敏捷" : u8"+4 敏捷");
		else if (cq == "kalguur")
			lit(attr ? "1% increased Ward" : "2% increased Ward",
			    attr ? u8"增加 1% 保護" : u8"增加 2% 保護");
		else if (cq == "templar") {
			if (attr) replaceWith("templar_devotion_node");
			else lit("+5 to Devotion", u8"+5 奉獻");
		} else if (cq == "eternal") {
			replaceWith("eternal_small_blank"); // smalls go blank under Elegant Hubris
		}
		// Abyss jewels: PoB has no small-node rule for them yet, so emit nothing.
		return out;
	}

	std::vector<int> lut = TJReadLUT(ds, blob, jewelType, seed, nodeId);
	if (lut.empty()) {
		out.note = "no LUT result";
		return out;
	}

	if (jewelType == 1) {
		const int hs = (int)lut.size();
		// Notables roll each stat from its own slot (statMod.index); smalls take
		// a single roll from jewelDataTbl[2] for every line.
		const bool gvSmall = nodeType == "Normal";
		if (hs == 2 || hs == 3) {
			const TJEntry* n = node_at(ds, lut[0]);
			if (!n) { out.note = "GV replace id out of range"; return out; }
			out.ok = out.replaced = true;
			out.newName = n->dn;
			out.newNameZh = n->dnZh;
			for (size_t i = 0; i < n->sd.size(); i++) {
				const TJStatMod* sm = nullptr;
				double val = 0;
				std::string key;
				if (i < n->sortedStats.size()) {
					auto it = n->stats.find(n->sortedStats[i]);
					const int slot = gvSmall ? 1 : (it != n->stats.end() ? it->second.index : -1);
					if (it != n->stats.end() && slot >= 0 && slot < (int)lut.size()) {
						sm = &it->second;
						val = (double)lut[slot];
						key = it->first;
					}
				}
				push_line(out, *n, i, sm, val, key);
			}
			return out;
		} else if (hs == 6 || hs == 8) {
			int bias = 0;
			for (int i = 0; i < hs / 2; i++) bias += (lut[i] <= 21) ? 1 : -1;
			// legionNodes[77]/[78] (Lua 1-based) = Might / Legacy of the Vaal.
			// Look them up by id: their array index is stable but the additions
			// offset in front of them is not (96 -> 337 with the Abyss jewels).
			const char* wantId = (bias >= 0) ? "vaal_notable_random_offense"
			                                 : "vaal_notable_random_defence";
			const TJEntry* n = nullptr;
			for (const auto& e : ds.nodes) if (e.id == wantId) { n = &e; break; }
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
				const std::string key = a->stats.empty() ? std::string() : a->stats.begin()->first;
				for (size_t i = 0; i < a->sd.size(); i++)
					push_line(out, *a, i, sm, (double)kv.second, key);
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
	// An unrolled range and a rolled value name the same stat: "(7-12)% increased
	// Fire Damage" and "9% increased Fire Damage" must land on one key, or a
	// stat picked from the list (built from unrolled templates) would never match
	// a line the Abyss engine produces (which always carries its actual roll).
	// "(#-#)" is the only parenthesised shape in the dataset — 1170 of them, and
	// nothing else — so a plain substitution is enough.
	replace_all_str(out, "(#-#)", "#");
	return out;
}

double TJStatValue(const std::string& s)
{
	for (size_t i = 0; i < s.size(); i++)
		if (isdigit((unsigned char)s[i])) return strtod(s.c_str() + i, nullptr);
	return 0.0;
}

TJWantMatcher::TJWantMatcher(const std::vector<TJWantStat>& wants) : wants_(wants)
{
	// Built after the copy so the indices refer to wants_, not the caller's
	// vector — the matcher has to survive a temporary being passed in.
	for (int i = 0; i < (int)wants_.size(); i++) byTmpl_.emplace(wants_[i].tmpl, i);
}

const TJWantStat* TJWantMatcher::Match(const std::string& line) const
{
	auto it = byTmpl_.find(TJNormalizeStat(line));
	if (it == byTmpl_.end()) return nullptr;
	const TJWantStat& w = wants_[it->second];
	return TJStatValue(line) >= w.minValue ? &w : nullptr;
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

	const TJWantMatcher matcher(q.wants);

	// candidate nodes: the socket's in-radius set if given, else every indexed
	// node. Each carries its type so smalls roll additions, notables roll nodes.
	std::vector<std::pair<int, bool>> scopeNodes; // (nodeId, notable)
	auto consider = [&](int nodeId, int idxInBin) {
		const bool notable = idxInBin < ds.sizeNotable; // rows are 0..sizeNotable-1
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
				if (const TJWantStat* w = matcher.Match(line)) {
					total += w->weight;
					matches++;
					distinct.insert(w);
				}
			}
		}
		const bool covered = !q.requireAll || distinct.size() == q.wants.size();
		if (matches > 0 && covered && total >= q.minTotalWeight)
			hits.push_back({ seed, total, matches, (int)distinct.size() });
	}

	// Weight decides the ranking; the rest are tie-breaks so the same query always
	// produces the same order (std::sort is not stable, and equal-weight seeds are
	// common once weights are whole numbers).
	std::sort(hits.begin(), hits.end(), [](const TJSeedHit& a, const TJSeedHit& b) {
		if (a.weight != b.weight) return a.weight > b.weight;
		if (a.distinctWants != b.distinctWants) return a.distinctWants > b.distinctWants;
		if (a.matches != b.matches) return a.matches > b.matches;
		return a.seed < b.seed;
	});
	if (topN > 0 && (int)hits.size() > topN) hits.resize(topN);
	return hits;
}

// ---- bin loading --------------------------------------------------------------

bool TJLoadBin(const std::wstring& exeDir, const TJDataset& ds, int jewelType,
               std::string& out, std::string* err)
{
	auto it = ds.types.find(jewelType);
	if (it == ds.types.end()) { if (err) *err = u8"未知珠寶型別"; return false; }
	// LUT file stem: the jewel name without spaces, unless the dataset maps this
	// type elsewhere (the Abyss LUTs are named after the Abyssal Lord).
	auto itBin = ds.binNames.find(jewelType);
	const std::string stem = (itBin != ds.binNames.end() && !itBin->second.empty())
	                         ? itBin->second : it->second;
	std::string nameA; // ascii file stem (e.g. "BrutalRestraint", "AbyssTecrod")
	std::wstring nameW;
	for (char c : stem) if (c != ' ') { nameA.push_back(c); nameW.push_back((wchar_t)(unsigned char)c); }
	std::wstring pobDir = FindPoe1Dir(exeDir);
	if (pobDir.empty()) {
		if (err) *err = u8"找不到 PoE1 版 POB 資料夾(需放在 pob-zh.exe 旁,名稱不限,內含 Launch.lua)";
		return false;
	}
	std::wstring base = pobDir + L"Data\\TimelessJewelData\\" + nameW;

	// Same freshness rule as PoB's loadJewelFile: the .bin is only a cache of the
	// shipped .zip, so an older .bin is stale (a game patch that changes the node
	// count shifts every row) and the .zip wins. PoB cannot produce the Abyss
	// .bin files at all yet, so we inflate them ourselves. Despite the name the
	// ".zip" is raw zlib, and Glorious Vanity ships split as .zip.part0..N.
	std::vector<std::wstring> packedParts;
	if (file_mtime(base + L".zip") != 0) {
		packedParts.push_back(base + L".zip");
	} else {
		for (int i = 0; ; i++) {
			std::wstring p = base + L".zip.part" + std::to_wstring(i);
			if (file_mtime(p) == 0) break;
			packedParts.push_back(std::move(p));
		}
	}
	const uint64_t binTime = file_mtime(base + L".bin");
	uint64_t zipTime = 0;
	for (const auto& p : packedParts) zipTime = (std::max)(zipTime, file_mtime(p));

	if (binTime && binTime >= zipTime && read_file_bytes(base + L".bin", out))
		return true;
	std::string packed, chunk;
	for (const auto& p : packedParts) {
		if (!read_file_bytes(p, chunk)) { packed.clear(); break; }
		packed += chunk;
	}
	if (!packed.empty()) {
		size_t len = 0;
		void* p = tinfl_decompress_mem_to_heap(packed.data(), packed.size(), &len,
		                                       TINFL_FLAG_PARSE_ZLIB_HEADER);
		if (p) {
			out.assign((const char*)p, len);
			mz_free(p);
			return true;
		}
	}
	if (binTime && read_file_bytes(base + L".bin", out))
		return true; // stale, but better than nothing if the .zip is unreadable
	if (err) *err = u8"找不到 " + nameA +
	                u8".bin/.zip(需要旁邊有 PoE1 版 POB,且該珠寶的查表檔存在)";
	return false;
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
	std::string nodeType = (ni != ds.nodeIndex.end() && ni->second.first < ds.sizeNotable)
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
	check(ds.additions.size() == (size_t)ds.additionsOffset, "additions count == additionsOffset",
	      std::to_string(ds.additions.size()) + "/" + std::to_string(ds.additionsOffset));
	check(ds.nodes.size() >= 100, "nodes count", std::to_string(ds.nodes.size()));
	check(ds.size == 1937 && ds.sizeNotable == 454, "node index sizes",
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
		// A picked stat has to match a line that carries an actual roll. The
		// Legion engine leaves ranges unrolled and the Abyss one always rolls, so
		// the two forms must reduce to one key or the Abyss search finds nothing.
		check(TJNormalizeStat("(7-12)% increased Fire Damage") ==
		      TJNormalizeStat("9% increased Fire Damage"),
		      "an unrolled range and a rolled value share one key",
		      TJNormalizeStat("(7-12)% increased Fire Damage"));
		check(TJNormalizeStat("Adds (5-7) to (11-13) Fire Damage") == "Adds # to # Fire Damage",
		      "a line with two ranges collapses both");
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

		// --- the minimum and the weight have to actually do something ---------
		// A minimum above every roll of that stat must empty the result set; if
		// it does not, the "minimum" is decorative and the ranking is noise.
		{
			// Uncapped baseline: `hits` above was truncated to topN, so comparing
			// set sizes against it would measure the cap, not the filter.
			auto all = TJSearch(ds, blob, q, 0, nullptr);
			check(!all.empty(), "baseline search returns seeds", std::to_string(all.size()));

			TJSearchQuery qm = q;
			qm.wants[0].minValue = 1000.0;
			check(TJSearch(ds, blob, qm, 0, nullptr).empty(),
			      "an unreachable 最小值 rejects every seed");

			// The poison template always grants 20, so any threshold either keeps
			// every seed or none and "the set shrank" would be vacuously true.
			// A minimum only discriminates on a template several nodes grant at
			// DIFFERENT amounts (Brutal Restraint's "+# to Dexterity" comes as 2,
			// 4 and 20) — find one of those and cut between the extremes.
			std::map<std::string, std::set<double>> byTmpl;
			for (const auto& e : ds.additions) {
				if (e.id.rfind("maraketh", 0) != 0) continue;
				for (const auto& line : e.sd) byTmpl[TJNormalizeStat(line)].insert(TJStatValue(line));
			}
			std::string splitTmpl;
			double slo = 0, shi = 0;
			for (const auto& kv : byTmpl)
				if (kv.second.size() > 1) {
					splitTmpl = kv.first;
					slo = *kv.second.begin();
					shi = *kv.second.rbegin();
					break;
				}
			if (splitTmpl.empty()) {
				check(false, "found a Brutal Restraint stat granted at several amounts");
			} else {
				TJSearchQuery qr;
				qr.jewelType = 3;
				// All nodes, not just notables: the small amounts of a shared
				// template come from SMALL nodes (+2/+4 Dexterity), so a
				// notables-only scan sees only the +20 and the minimum would have
				// nothing to cut.
				qr.scope = 0;
				qr.wants.push_back({ splitTmpl, 0.0, 1.0 });
				auto loose = TJSearch(ds, blob, qr, 0, nullptr);
				qr.wants[0].minValue = (slo + shi) / 2.0;
				auto tight = TJSearch(ds, blob, qr, 0, nullptr);
				// Over the whole tree nearly every seed keeps SOME matching node,
				// so the minimum is not really a seed filter — what it changes is
				// how many nodes each seed matches, and therefore its score and
				// its rank. That is the quantity worth asserting on.
				std::map<int, int> before;
				for (const auto& h : loose) before[h.seed] = h.matches;
				long long lost = 0;
				bool neverGrew = true;
				for (const auto& h : tight) {
					auto it = before.find(h.seed);
					if (it == before.end()) continue;
					neverGrew = neverGrew && h.matches <= it->second;
					lost += it->second - h.matches;
				}
				check(!loose.empty() && !tight.empty() && neverGrew && lost > 0,
				      "最小值 drops the under-minimum nodes from each seed's score",
				      splitTmpl + " [" + std::to_string((int)slo) + ".." + std::to_string((int)shi) +
				      "] " + std::to_string(lost) + " node hits removed across " +
				      std::to_string(tight.size()) + " seeds");
			}

			// Weight scales the score, so the ranking must be reproducible and
			// proportional rather than incidental.
			TJSearchQuery qw = q;
			qw.wants[0].weight = 2.5;
			auto weighted = TJSearch(ds, blob, qw, 2000, nullptr);
			bool scaled = weighted.size() == hits.size();
			for (size_t i = 0; scaled && i < weighted.size(); i++)
				scaled = weighted[i].seed == hits[i].seed &&
				         std::abs(weighted[i].weight - hits[i].weight * 2.5) < 1e-9;
			check(scaled, "weight scales the score and keeps the order");

			// minTotalWeight is a floor on the score. Take the threshold from the
			// population itself (just above the median) so the check proves it
			// splits the set instead of accepting a value that happens to keep
			// everything.
			TJSearchQuery qt = q;
			qt.minTotalWeight = all[all.size() / 2].weight + 0.5;
			auto floored = TJSearch(ds, blob, qt, 0, nullptr);
			bool allAbove = true;
			for (const auto& h : floored) allAbove = allAbove && h.weight >= qt.minTotalWeight;
			check(allAbove && !floored.empty() && floored.size() < all.size(),
			      "最小總權重 splits the population by score",
			      std::to_string(floored.size()) + " of " + std::to_string(all.size()) +
			      " above " + std::to_string(qt.minTotalWeight));

			// --- picking two stats has to mean BOTH -------------------------
			// The reported symptom: with two stats picked, a seed carrying the
			// first one three times scored the same as a seed carrying both, and
			// was listed as an equal hit.
			{
				TJSearchQuery q2;
				q2.jewelType = 3;
				q2.scope = 1;
				q2.wants.push_back({ "#% increased Damage with Poison", 0.0, 1.0 });
				q2.wants.push_back({ "Poisons you inflict deal Damage #% faster", 0.0, 1.0 });

				q2.requireAll = false;
				auto any = TJSearch(ds, blob, q2, 0, nullptr);
				q2.requireAll = true;
				auto both = TJSearch(ds, blob, q2, 0, nullptr);

				bool anyHadPartial = false;
				for (const auto& h : any) if (h.distinctWants < 2) { anyHadPartial = true; break; }
				bool allCovered = !both.empty();
				for (const auto& h : both) allCovered = allCovered && h.distinctWants == 2;

				check(anyHadPartial, "without the flag, partial seeds do get through",
				      std::to_string(any.size()) + " seeds");
				check(allCovered && both.size() < any.size(),
				      "必須包含全部已選詞綴 drops the partial ones",
				      std::to_string(both.size()) + " of " + std::to_string(any.size()));

				// The case the user actually hits: a jewel socket sees ~20 notables,
				// not 452, so partial coverage is the norm rather than a rounding
				// error. Restricting the node set is what makes the difference
				// visible, so assert it there too.
				std::vector<int> few;
				for (const auto& kv : ds.nodeIndex) {
					if (kv.second.first >= ds.sizeNotable) continue;
					few.push_back(kv.first);
					if (few.size() >= 20) break;
				}
				q2.nodeIds = few;
				q2.requireAll = false;
				auto anyFew = TJSearch(ds, blob, q2, 0, nullptr);
				q2.requireAll = true;
				auto bothFew = TJSearch(ds, blob, q2, 0, nullptr);
				bool coveredFew = !bothFew.empty();
				for (const auto& h : bothFew) coveredFew = coveredFew && h.distinctWants == 2;
				check(coveredFew && bothFew.size() * 2 < anyFew.size(),
				      "on a socket-sized node set the flag removes most partial seeds",
				      std::to_string(bothFew.size()) + " of " + std::to_string(anyFew.size()) +
				      " over " + std::to_string(few.size()) + " notables");
			}

			// Ranking must be a total order: equal weights are common, and an
			// unstable sort would shuffle them between runs.
			auto again = TJSearch(ds, blob, q, 2000, nullptr);
			bool same = again.size() == hits.size();
			for (size_t i = 0; same && i < again.size(); i++) same = again[i].seed == hits[i].seed;
			check(same, "the same query ranks identically every run");
			bool ordered = true;
			for (size_t i = 1; i < hits.size(); i++) ordered = ordered && hits[i - 1].weight >= hits[i].weight;
			check(ordered, "results are ordered by weight");
		}

		// --- display and search must agree on what counts as a hit -----------
		// The UI used to compare templates only, so a roll under 最小值 was still
		// drawn as a match. Both sides now go through TJWantMatcher; this checks
		// the matcher enforces exactly what the search counted.
		{
			std::vector<TJWantStat> w{ { "#% increased Damage with Poison", 25.0, 1.0 } };
			TJWantMatcher m(w);
			check(m.Match("30% increased Damage with Poison") != nullptr,
			      "matcher accepts a roll at or above 最小值");
			check(m.Match("20% increased Damage with Poison") == nullptr,
			      "matcher rejects a roll below 最小值");
			check(m.Match("30% increased Damage with Bleeding") == nullptr,
			      "matcher rejects a different stat");
			check(TJWantMatcher().Match("anything") == nullptr, "empty matcher matches nothing");
			check(TJStatValue("Adds 5 to 12 Fire Damage") == 5.0, "value reads the first number");
			check(TJStatValue("Enemies have -2% to all Resistances") == 2.0,
			      "value is the magnitude within its template (the sign lives in the template)");

			// Cross-check the matcher against the search on real seeds: re-count by
			// hand and demand the same number the ranking used. Sampled (each seed
			// is a full 452-node sweep) and asserted non-empty, because a loop that
			// never runs would "pass" while testing nothing.
			TJSearchQuery qc = q;
			qc.wants[0].minValue = 10.0;   // low enough to leave seeds to check
			auto sample = TJSearch(ds, blob, qc, 5, nullptr);
			TJWantMatcher mc(qc.wants);
			bool consistent = !sample.empty();
			for (const auto& h : sample) {
				int counted = 0;
				for (const auto& kv : ds.nodeIndex) {
					if (kv.second.first >= ds.sizeNotable) continue;
					TJTransform t2 = TJApply(ds, blob, qc.jewelType, h.seed, kv.first, "Notable", {});
					if (!t2.ok) continue;
					for (const auto& ln : t2.lines) if (mc.Match(ln)) counted++;
				}
				if (counted != h.matches) { consistent = false; break; }
			}
			check(consistent, "matcher and search count the same hits",
			      std::to_string(sample.size()) + " seeds re-counted");
		}
	} else {
		check(false, "load BrutalRestraint.bin", err);
	}

	// --- Glorious Vanity is the only Legion jewel that rolls a value into a
	// notable, so it is the only one where the "g"-format unit scaling can show.
	// Nothing used to exercise this path end to end: --tj-verify compares LUT
	// bytes, not rendered lines, so "Regenerate 60% of Life per second" would
	// have passed every check we had. Assert the printed number lands inside the
	// range its own template declares.
	{
		std::string gv;
		std::string gerr;
		if (!TJLoadBin(exeDir, ds, 1, gv, &gerr)) {
			check(false, "load GloriousVanity LUT", gerr);
		} else {
			const TJEntry* ritual = nullptr;
			for (const auto& e : ds.nodes) if (e.id == "vaal_notable_life_1") { ritual = &e; break; }
			if (!ritual) {
				check(false, "dataset carries vaal_notable_life_1 (Ritual of Flesh)");
			} else {
				auto sm = ritual->stats.find("life_regeneration_rate_per_minute_%");
				std::string found;
				int scanned = 0;
				for (int seed = 100; seed < 200 && found.empty(); seed++) {
					for (const auto& kv : ds.nodeIndex) {
						if (kv.second.first >= ds.sizeNotable) continue;
						scanned++;
						TJTransform t = TJApply(ds, gv, 1, seed, kv.first, "Notable", {});
						if (!t.ok || !t.replaced || t.newName != ritual->dn) continue;
						for (const auto& ln : t.lines)
							if (ln.find("per second") != std::string::npos) { found = ln; break; }
						if (!found.empty()) break;
					}
				}
				if (found.empty()) {
					// Say so rather than passing: a scan that found no case has
					// tested nothing, and silence here is what let the bug live.
					check(false, "found a Ritual of Flesh roll to check",
					      std::to_string(scanned) + " transforms scanned, none matched");
				} else {
					const double v = TJStatValue(found);
					check(sm != ritual->stats.end() && v >= sm->second.min && v <= sm->second.max,
					      "a rolled per-second regen prints inside its own declared range",
					      found + " (range " + fmt_num(sm->second.min) + "-" +
					      fmt_num(sm->second.max) + ")");
					check(found.find("(0.7-1.2)") == std::string::npos,
					      "the range placeholder was actually substituted", found);
				}
			}
		}
	}

	// Abyss jewels (types 7-11): LUT named after the Abyssal Lord, inflated from
	// the shipped .zip because PoB never writes a .bin for these.
	check(ds.types.count(7) && ds.types.at(7) == "Festering Vengeance", "type 7 name",
	      ds.types.count(7) ? ds.types.at(7) : "(missing)");
	check(ds.binNames.count(11) && ds.binNames.at(11) == "AbyssZorath", "type 11 LUT stem",
	      ds.binNames.count(11) ? ds.binNames.at(11) : "(missing)");
	std::string abyss;
	if (TJLoadBin(exeDir, ds, 7, abyss, &err)) {
	// PoB 2.67 wrapped the Abyss tables in a container: 4-byte magic, format
	// version, jewel type, then seed min/max/increment and a table of block
	// offsets ("ABYS" keys blocks by socket, "ABYN" by node and adds an "ASCS"
	// ascendancy section). See PoB's Modules/DataAbyssJewelLookUpTableHelper.lua.
	//
	// Detected by its magic, not by size: a size check only says "not what I
	// expected", while the magic says what it actually is. TJReadLUT reads a
	// flat notable*seed table from offset 0, so pointing it at a container
	// returns bytes lifted out of the header and the offset tables -- answers
	// that look like real stat ids and are wrong.
	//
	// Reading these files is timeless_jewel_abyss.cpp's job and
	// --abyss-selftest's to check. What belongs here is the boundary between
	// the two engines, so that nothing quietly sends an Abyss file through the
	// Legion reader again.
	const std::string magic = abyss.substr(0, (std::min)((size_t)4, abyss.size()));
	check(magic == "ABYS", "Tecrod ships as an ABYS container", magic);
	check(TJIsAbyss(7) && TJIsAbyss(11) && !TJIsAbyss(6),
	      "the Abyss engine claims exactly types 7-11");
	check(kMaxJewelType == 10,
	      "the calculator offers 7-10 and still withholds Zorath (11)",
	      "kMaxJewelType = " + std::to_string(kMaxJewelType));
	check(TJAbyssUsable(10) && !TJAbyssUsable(11),
	      "Zorath is refused by the engine too, not only hidden by the UI");

	// These do not touch the blob: TJApply's keystone branch answers from the
	// dataset and returns before any lookup, and the template list never reads
	// a LUT at all. So they still say something true about the Abyss data.
	TJTransform k = TJApply(ds, abyss, 7, 100, 6, "Keystone", {}, "abyss_murderous", "");
	check(k.ok && k.newName == "Overwhelming Hate", "abyss keystone replace",
	      k.ok ? k.newName + " / " + k.newNameZh : k.note);
	check(k.newNameZh == u8"壓倒性恨意", "abyss keystone zh name", k.newNameZh);
	auto tmpl = TJStatTemplates(ds, 7);
	check(tmpl.size() > 50, "abyss stat template list", std::to_string(tmpl.size()));
	} else {
		check(false, "load AbyssTecrod LUT", err);
	}
	// paste parsing, against real item text (trade API explicitMods, 3.29 Allflame)
	{
		struct PCase { const char* txt; int type; int seed; const char* what; };
		const PCase pcases[] = {
			{ "Subjugating 6353 souls in the thrall of Kurgal\n"
			  "Passives affected are Conquered by the Abyssal", 9, 6353, "paste Kurgal (en)" },
			{ "Subjugating 6925 souls in the thrall of Ulaman\n"
			  "Passives affected are Conquered by the Abyssal", 8, 6925, "paste Ulaman (en)" },
			{ "Subjugating 5389 souls in the thrall of Amanamu\n"
			  "Passives affected are Conquered by the Abyssal", 10, 5389, "paste Amanamu (en)" },
			// Zorath words its seed line differently from the other four
			{ "Binding 7412 souls to phylacteries to sustain Zorath\n"
			  "Passives affected are Conquered by the Abyssal", 11, 7412, "paste Zorath (en)" },
			// zh client text
			{ u8"在柯戈的奴役下征服 6353 位靈魂\n範圍內的天賦被深淵族所征服",
			  9, 6353, "paste Kurgal (zh)" },
			{ u8"將 7412 位靈魂縛於命匣以維持澤洛斯\n範圍內的天賦被深淵族所征服",
			  11, 7412, "paste Zorath (zh)" },
			// full copy: item level / stack numbers must not win over the seed
			{ "Item Class: Jewels\nRarity: Unique\nBaleful Dominion\nHypnotic Eye Jewel\n"
			  "--------\nAbyss\nLimited to: 1 Historic\n--------\nItem Level: 86\n--------\n"
			  "Subjugating 6353 souls in the thrall of Kurgal\n"
			  "Passives affected are Conquered by the Abyssal\n--------\nHistoric",
			  9, 6353, "paste full item text" },
			// regression: a traditional legion jewel still resolves
			{ "Commanded leadership over 12345 warriors under Kaom\n"
			  "Passives in radius are Conquered by the Karui", 2, 12345, "paste Kaom (en)" },
		};
		for (const PCase& c : pcases) {
			TJPaste p = TJParsePaste(ds, c.txt);
			check(p.jewelType == c.type && p.seed == c.seed, c.what,
			      "type=" + std::to_string(p.jewelType) + " seed=" + std::to_string(p.seed));
		}
	}

	// Item text round-trips: what we hand to PoB must parse back to the same
	// jewel and seed, and must carry the wording PoB's ModParser matches on.
	{
		struct IC { int type; int seed; const char* needle; };
		const IC icases[] = {
			{ 1, 5000,  "Bathed in the blood of 5000 sacrificed in the name of" },
			{ 2, 12703, "Commanded leadership over 12703 warriors under Kaom" },
			{ 3, 4000,  "Denoted service of 4000 dekhara in the akhara of" },
			{ 4, 6000,  "Carved to glorify 6000 new faithful converted by High Templar" },
			{ 5, 80000, "Commissioned 80000 coins to commemorate" },
			{ 6, 4000,  "Remembrancing 4000 songworthy deeds by the line of" },
		};
		for (const IC& c : icases) {
			std::string txt = TJItemText(ds, c.type, 0, c.seed);
			const bool hasLine = txt.find(c.needle) != std::string::npos;
			const bool hasConq = txt.find("Passives in radius are Conquered by the") != std::string::npos;
			TJPaste back = TJParsePaste(ds, txt);
			check(hasLine && hasConq && back.jewelType == c.type && back.seed == c.seed,
			      "item text round-trip type " + std::to_string(c.type),
			      "type=" + std::to_string(back.jewelType) + " seed=" + std::to_string(back.seed) +
			      (hasLine ? "" : " [seed line wrong]") + (hasConq ? "" : " [no conquered line]"));
		}
		// Abyss jewels word their item text differently: an Eye Jewel base rather
		// than "Timeless Jewel", no radius line, and "Passives affected" instead
		// of "Passives in radius". Transcribed from PoB's own
		// TimelessJewelListControl.lua, so a round-trip through our own parser is
		// the weaker half of the claim -- the wording is the part that must be
		// PoB's, which is why the base line is asserted literally.
		{
			struct AC { int type; const char* base; const char* needle; };
			const AC acases[] = {
				{ 7,  "Murderous Eye Jewel", "Subjugating 5000 souls in the thrall of Tecrod" },
				{ 9,  "Hypnotic Eye Jewel",  "Subjugating 5000 souls in the thrall of Kurgal" },
				{ 11, "Assembled Eye Jewel",
				      "Binding 5000 souls to phylacteries to sustain Zorath" },
			};
			for (const AC& c : acases) {
				const std::string txt = TJItemText(ds, c.type, 0, 5000);
				TJPaste back = TJParsePaste(ds, txt);
				check(txt.find(c.needle) != std::string::npos &&
				      txt.find(c.base) != std::string::npos &&
				      txt.find("Passives affected are Conquered by the Abyssal") != std::string::npos &&
				      txt.find("Radius:") == std::string::npos &&
				      back.jewelType == c.type && back.seed == 5000,
				      "abyss item text round-trip type " + std::to_string(c.type),
				      "type=" + std::to_string(back.jewelType) + " seed=" + std::to_string(back.seed));
			}
		}
	}

	// Regression: an addition must leave the node itself alone (PoB appends the
	// stat instead of replacing the node), and index==sizeNotable is past the
	// last LUT row — treating it as a notable used to read a phantom byte 0 and
	// turn a small passive into a keystone.
	std::string lp;
	if (TJLoadBin(exeDir, ds, 2, lp, &err)) {
		auto lut = TJReadLUT(ds, lp, 2, 12637, 21435); // Cloth and Chain
		const TJEntry* a = lut.size() == 1 ? addition_at(ds, lut[0]) : nullptr;
		check(lut.size() == 1 && lut[0] == 50 && a && a->id == "karui_notable_add_damage_from_crits",
		      "LP 12637 Cloth and Chain is addition 50",
		      lut.empty() ? "(none)" : std::to_string(lut[0]) + " " + (a ? a->id : "?"));
		TJTransform t = TJApply(ds, lp, 2, 12637, 21435, "Notable", {}, "karui", "1", "Cloth and Chain");
		check(t.ok && !t.replaced && t.lines.size() == 1 &&
		      t.lines[0] == "You take 10% reduced Extra Damage from Critical Strikes",
		      "LP addition keeps the node (not replaced)",
		      std::string(t.replaced ? "replaced " : "kept ") +
		      (t.lines.empty() ? "(no lines)" : t.lines[0]));
		check(t.linesZh.size() == 1 && t.linesZh[0] == u8"減少 10% 承受的暴擊傷害",
		      "LP addition zh", t.linesZh.empty() ? "(none)" : t.linesZh[0]);

		// node 94 "Evasion" sits at index 454 == sizeNotable: no LUT row exists.
		auto none = TJReadLUT(ds, lp, 2, 12637, 94);
		check(none.empty(), "index == sizeNotable has no LUT row",
		      none.empty() ? "" : std::to_string(none[0]));
		TJTransform sm = TJApply(ds, lp, 2, 12637, 94, "Normal", {}, "karui", "1", "Evasion");
		check(sm.ok && !sm.replaced && sm.lines.size() == 1 && sm.lines[0] == "+4 to Strength",
		      "LP small node gains +4 Strength",
		      sm.lines.empty() ? "(none)" : sm.lines[0] + (sm.replaced ? " REPLACED" : ""));
		TJTransform at = TJApply(ds, lp, 2, 12637, 23027, "Normal", {}, "karui", "1", "Strength");
		check(at.lines.size() == 1 && at.lines[0] == "+2 to Strength",
		      "LP attribute node gains only +2", at.lines.empty() ? "(none)" : at.lines[0]);
	} else {
		check(false, "load LethalPride LUT", err);
	}
	// The other families' fixed small-node rules (PassiveSpec.lua "Normal" branch)
	std::string mf;
	if (TJLoadBin(exeDir, ds, 4, mf, &err)) {
		TJTransform p = TJApply(ds, mf, 4, 2000, 7092, "Normal", {}, "templar", "1", "Physical and Lightning Damage");
		check(p.lines.size() == 1 && p.lines[0] == "+5 to Devotion", "MF small node gains +5 Devotion",
		      p.lines.empty() ? "(none)" : p.lines[0]);
		TJTransform a = TJApply(ds, mf, 4, 2000, 23027, "Normal", {}, "templar", "1", "Strength");
		check(a.replaced && a.newName == "Devotion", "MF attribute node becomes Devotion",
		      a.newName.empty() ? "(none)" : a.newName);
	} else {
		check(false, "load MilitantFaith LUT", err);
	}
	std::string eh;
	if (TJLoadBin(exeDir, ds, 5, eh, &err)) {
		TJTransform p = TJApply(ds, eh, 5, 2000, 7092, "Normal", {}, "eternal", "1", "Physical and Lightning Damage");
		check(p.replaced && p.lines.empty() && p.newName == "Price of Glory",
		      "EH blanks small nodes", p.newName + (p.lines.empty() ? "" : " +lines"));
	} else {
		check(false, "load ElegantHubris LUT", err);
	}
	std::string ht;
	if (TJLoadBin(exeDir, ds, 6, ht, &err)) {
		TJTransform p = TJApply(ds, ht, 6, 100, 7092, "Normal", {}, "kalguur", "1", "Physical and Lightning Damage");
		check(p.lines.size() == 1 && p.lines[0] == "2% increased Ward", "HT small node gains 2% Ward",
		      p.lines.empty() ? "(none)" : p.lines[0]);
	} else {
		check(false, "load HeroicTragedy LUT", err);
	}

	std::string zorath;
	if (TJLoadBin(exeDir, ds, 11, zorath, &err)) {
		// Zorath ships as "ABYN": one block per passive node plus an "ASCS"
		// section, and which nodes it has an answer for depends on the allocated
		// path from the socket to the class start. PobTools has no character, so
		// this one stays out of the calculator even though its container parses
		// -- and staying out is the property worth pinning, because the day
		// someone raises kMaxJewelType to 11 the UI would start showing answers
		// derived from a path nobody supplied.
		const std::string zmagic = zorath.substr(0, (std::min)((size_t)4, zorath.size()));
		check(zmagic == "ABYN", "Zorath ships as an ABYN container", zmagic);
		check(kMaxJewelType < 11 && !TJAbyssUsable(11),
		      "Zorath is withheld by both the UI and the engine",
		      "kMaxJewelType = " + std::to_string(kMaxJewelType));
	} else {
		check(false, "load AbyssZorath LUT", err);
	}

	// --- trade search URL assembly -------------------------------------------
	// The query JSON is language-independent, so the ONLY thing that changes
	// between regions is the host + league. Golden strings here because the
	// international URL must not move a byte when a region is added.
	{
		const std::string kQ =
			"%7B%22query%22%3A%7B%22status%22%3A%7B%22option%22%3A%22securable%22%7D%2C%22stats%22"
			"%3A%5B%7B%22type%22%3A%22and%22%2C%22filters%22%3A%5B%7B%22id%22%3A%22explicit.pseudo"
			"_timeless_jewel_kaom%22%2C%22value%22%3A%7B%22min%22%3A1234%2C%22max%22%3A1234%7D%7D"
			"%5D%7D%5D%7D%2C%22sort%22%3A%7B%22price%22%3A%22asc%22%7D%7D";
		std::string q1 = TradeQueryJson("explicit.pseudo_timeless_jewel_kaom", 1234);

		check(TradeSearchUrl(0, "Standard", 0, q1) ==
		      "https://www.pathofexile.com/trade/search/Standard?q=" + kQ,
		      "international PC url is byte-identical to before");
		check(TradeSearchUrl(0, "Standard", 1, q1) ==
		      "https://www.pathofexile.com/trade/search/xbox/Standard?q=" + kQ,
		      "international xbox url keeps its realm segment");
		check(TradeSearchUrl(0, "Standard", 2, q1).find("/search/sony/") != std::string::npos,
		      "international sony url keeps its realm segment");

		// Taiwan: canonical host (www.pathofexile.tw 301-redirects), CJK league
		// percent-encoded per byte, and never a console segment.
		std::string tw = TradeSearchUrl(1, u8"亡焰咒海", 0, q1);
		check(tw == "https://pathofexile.tw/trade/search/"
		            "%E4%BA%A1%E7%84%B0%E5%92%92%E6%B5%B7?q=" + kQ,
		      "taiwan url: canonical host + encoded CJK league", tw.substr(0, 60));
		check(TradeSearchUrl(1, u8"亡焰咒海", 1, q1) == tw &&
		      TradeSearchUrl(1, u8"亡焰咒海", 2, q1) == tw,
		      "taiwan ignores the console platform entirely");
		check(TradeSearchUrl(1, "Standard", 0, q1).find("www.pathofexile.tw") == std::string::npos,
		      "taiwan never uses the redirecting www host");

		// guards
		check(TradeSearchUrl(0, "", 0, q1).empty(), "empty league yields no url");
		check(TradeSearchUrl(0, "Standard", 0, "").empty(), "empty query yields no url");
		check(TradeSearchUrl(99, "Standard", 0, q1) == TradeSearchUrl(0, "Standard", 0, q1),
		      "out-of-range region falls back to international");

		// multi-seed: one filter per seed, capped, count>=1
		std::vector<int> many;
		for (int i = 0; i < (int)kMaxTradeSeeds + 15; i++) many.push_back(1000 + i);
		std::string qm = TradeQueryJsonMulti("explicit.pseudo_timeless_jewel_kaom", many);
		size_t nFilters = 0;
		for (size_t p = qm.find("\"id\""); p != std::string::npos; p = qm.find("\"id\"", p + 1)) nFilters++;
		check(nFilters == kMaxTradeSeeds, "multi-seed query caps the filter count",
		      std::to_string(nFilters) + "/" + std::to_string(kMaxTradeSeeds));
		check(qm.find("\"type\":\"count\"") != std::string::npos &&
		      qm.find("\"value\":{\"min\":1}") != std::string::npos,
		      "multi-seed query is a count>=1 group");
		check(TradeQueryJsonMulti("x", {}).find("\"filters\":[]") != std::string::npos,
		      "no seeds yields an empty filter list, not garbage");

		// Remembered region/league/platform. Round-tripped through the real file
		// because that is what breaks (encoding, missing dir); the user's own
		// state is saved and put back afterwards.
		{
			std::wstring path = exeDir + L"PobTools\\tj_ui.json";
			TjUiState saved;
			bool had = saved.Load(exeDir);

			TjUiState w;
			w.realm = 1;
			w.platform = 2;
			w.league = u8"亡焰咒海";   // CJK must survive the round trip
			bool wrote = w.Save(exeDir);
			TjUiState rd;
			bool read = rd.Load(exeDir);
			check(wrote && read && rd.realm == 1 && rd.platform == 2 && rd.league == w.league,
			      "trade ui state round-trips (incl. CJK league)",
			      read ? rd.league : std::string("<load failed>"));

			// A corrupt file must not wedge the window at a bogus region.
			HANDLE bad = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
			                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (bad != INVALID_HANDLE_VALUE) {
				const char junk[] = "{not json";
				DWORD n = 0;
				WriteFile(bad, junk, (DWORD)strlen(junk), &n, nullptr);
				CloseHandle(bad);
			}
			TjUiState broken;
			bool loadedBad = broken.Load(exeDir);
			check(!loadedBad && broken.realm == 0 && broken.platform == 0 && broken.league.empty(),
			      "corrupt trade ui state falls back to defaults");

			if (had) saved.Save(exeDir);
			else DeleteFileW(path.c_str());
		}

		// every region must be usable: label/host present, exactly one w/ consoles
		int consoleRealms = 0, bad = 0;
		for (int r = 0; r < kTradeRealmCount; r++) {
			if (kTradeRealms[r].consoles) consoleRealms++;
			if (!kTradeRealms[r].label || !kTradeRealms[r].host || !kTradeRealms[r].hostW) bad++;
			if (TradeSearchUrl(r, "L", 0, q1).find(kTradeRealms[r].host) == std::string::npos) bad++;
		}
		check(bad == 0 && kTradeRealmCount >= 2 && consoleRealms == 1,
		      "region table is well formed",
		      std::to_string(kTradeRealmCount) + " regions, " +
		      std::to_string(consoleRealms) + " with consoles");
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

// Dump every transform we compute for a fixed, deterministic sample so it can be
// diffed against an independent implementation (tools/verify_lut.py replays PoB's
// PassiveSpec.lua straight off the shipped LUTs). Console output is unreliable
// here (AttachConsole eats it), so everything goes to tj_verify.tsv.
int RunTimelessJewelVerify(const std::wstring& exeDir)
{
	ensure_console();
	TJDataset ds;
	std::string err;
	if (!load_dataset(exeDir, ds, &err)) { printf("FAIL load: %s\n", err.c_str()); return 1; }

	PassiveTreeData tree;
	std::string terr;
	const bool haveTree = tree.Load(exeDir, &terr); // node names drive the attribute rule
	std::map<int, const PtNode*> byId;
	if (haveTree) for (const auto& n : tree.nodes) byId[n.id] = &n;

	std::string tsv = "type\tseed\tnode\tnotable\tbytes\treplaced\tnewName\tlines\n";
	long rows = 0;
	for (int jt = 1; jt <= 6; jt++) {
		std::string blob;
		if (!TJLoadBin(exeDir, ds, jt, blob, &err)) {
			printf("skip type %d: %s\n", jt, err.c_str());
			continue;
		}
		const int lo = ds.seedMin[jt], hi = ds.seedMax[jt];
		const int mul = (jt == 5) ? 20 : 1; // Elegant Hubris stores seed/20
		const int picks[5] = { lo, lo + 1, (lo + hi) / 2, hi - 1, hi };
		for (int p = 0; p < 5; p++) {
			const int seed = picks[p] * mul;
			for (const auto& kv : ds.nodeIndex) {
				const int nodeId = kv.first, index = kv.second.first;
				const bool notable = index < ds.sizeNotable;
				if (!notable && (index % 20) != 0) continue; // sample the smalls
				const PtNode* pn = byId.count(nodeId) ? byId[nodeId] : nullptr;
				const char* nt = notable ? "Notable" : "Normal";
				std::vector<int> lut = TJReadLUT(ds, blob, jt, seed, nodeId);
				TJTransform t = TJApply(ds, blob, jt, seed, nodeId, nt, {},
				                        ds.conqType.count(jt) ? ds.conqType[jt] : std::string(),
				                        "", pn ? pn->name : std::string());
				std::string bytes;
				for (size_t i = 0; i < lut.size(); i++)
					bytes += (i ? "," : "") + std::to_string(lut[i]);
				std::string lines;
				for (size_t i = 0; i < t.lines.size(); i++)
					lines += (i ? "|" : "") + t.lines[i];
				tsv += std::to_string(jt) + "\t" + std::to_string(seed) + "\t" +
				       std::to_string(nodeId) + "\t" + (notable ? "1" : "0") + "\t" +
				       bytes + "\t" + (t.replaced ? "1" : "0") + "\t" + t.newName + "\t" +
				       lines + "\n";
				rows++;
			}
		}
		printf("type %d dumped\n", jt);
	}
	printf("%ld rows%s\n", rows, haveTree ? "" : " (no passive tree: attribute rule untested)");

	HANDLE h = CreateFileW((exeDir + L"tj_verify.tsv").c_str(), GENERIC_WRITE, 0,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) { printf("cannot write tj_verify.tsv\n"); return 1; }
	DWORD written = 0;
	WriteFile(h, tsv.data(), (DWORD)tsv.size(), &written, nullptr);
	CloseHandle(h);
	return 0;
}
