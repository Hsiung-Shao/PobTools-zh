#include "timeless_jewel_abyss.h"

#include <algorithm>
#include <set>
#include <stdio.h>
#include <string>

#include <windows.h>

#include "passive_tree_data.h"

// ---- byte readers -------------------------------------------------------------
//
// Every read is bounds-checked and reports failure rather than clamping. These
// files are 70-100 MB of variable-length records: one bad length walks the
// cursor into the middle of another record and everything after it decodes into
// plausible-looking nonsense. Stopping at the first impossible read is the only
// way that stays detectable.

namespace {

bool rd_u8(const std::string& b, size_t& o, int& v)
{
	if (o >= b.size()) return false;
	v = (unsigned char)b[o++];
	return true;
}

bool rd_u16(const std::string& b, size_t& o, int& v)
{
	if (o + 1 >= b.size()) return false;
	v = (unsigned char)b[o] | ((unsigned char)b[o + 1] << 8);
	o += 2;
	return true;
}

// Advance past one modification without decoding it.
bool skip_modification(const std::string& b, size_t& o)
{
	int componentCount = 0;
	if (!rd_u8(b, o, componentCount)) return false;
	for (int i = 0; i < componentCount; i++) {
		if (o + 2 >= b.size()) return false;
		const int statCount = (unsigned char)b[o + 2];
		o += 3 + (size_t)statCount * 2;
		if (o > b.size()) return false;
	}
	return true;
}

// ABYS: one record = the conquered nodes for one (socket, seed).
bool skip_socket_record(const std::string& b, size_t& o)
{
	int affected = 0;
	if (!rd_u8(b, o, affected)) return false;
	for (int i = 0; i < affected; i++) {
		o += 2; // node id
		if (o > b.size()) return false;
		if (!skip_modification(b, o)) return false;
	}
	return true;
}

// ABYN: one record = the nodes this seed picks inside one Ascendancy.
bool skip_ascendancy_record(const std::string& b, size_t& o)
{
	int selected = 0;
	if (!rd_u8(b, o, selected)) return false;
	o += (size_t)selected * 2;
	return o <= b.size();
}

bool read_modification(const TJDataset& ds, const std::string& b, size_t& o,
                       int jewelType, TJAbyssMod& out)
{
	int componentCount = 0;
	if (!rd_u8(b, o, componentCount)) return false;
	out.clear();
	out.reserve((size_t)componentCount);
	for (int i = 0; i < componentCount; i++) {
		int type = 0, localId = 0, statCount = 0;
		if (!rd_u8(b, o, type) || !rd_u8(b, o, localId) || !rd_u8(b, o, statCount))
			return false;
		TJAbyssComponent c;
		c.type = type;
		c.globalId = ds.L2G(jewelType, localId);
		c.rolls.reserve((size_t)statCount);
		for (int s = 0; s < statCount; s++) {
			int raw = 0;
			if (!rd_u16(b, o, raw)) return false;
			c.rolls.push_back(raw >= 32768 ? raw - 65536 : raw);
		}
		out.push_back(c);
	}
	return true;
}

} // namespace

bool TJIsAbyss(int jewelType) { return jewelType >= 7 && jewelType <= 11; }
bool TJAbyssUsable(int jewelType) { return jewelType >= 7 && jewelType <= 11; }
bool TJIsZorath(int jewelType) { return jewelType == 11; }

// ---- container parse ----------------------------------------------------------

bool TJAbyssParse(const std::string& blob, int jewelType, TJAbyssLUT& out, std::string* err)
{
	out = TJAbyssLUT();
	auto fail = [&](const std::string& m) {
		if (err) *err = m;
		return false;
	};
	if (blob.size() < 12) return fail(u8"深淵查表檔太短,不是有效的容器");

	out.fmt = blob.substr(0, 4);
	const int formatVersion = (unsigned char)blob[4];
	const int storedType = (unsigned char)blob[5];
	size_t o = 6;
	int lo = 0, hi = 0, inc = 0;
	if (!rd_u16(blob, o, lo) || !rd_u16(blob, o, hi) || !rd_u16(blob, o, inc))
		return fail(u8"深淵查表檔標頭讀取失敗");

	if (out.fmt != "ABYS" && out.fmt != "ABYN")
		return fail(u8"未知的深淵容器格式:" + out.fmt);
	// PoB errors out on each of these rather than carrying on, and so do we: a
	// header that disagrees means the file is not the one we were asked for.
	if (formatVersion != 1)
		return fail(u8"深淵容器版本 " + std::to_string(formatVersion) + u8",本程式只認得版本 1");
	if (storedType != jewelType)
		return fail(u8"深淵查表檔屬於珠寶型別 " + std::to_string(storedType) +
		            u8",但要求的是 " + std::to_string(jewelType));
	if (inc == 0) return fail(u8"深淵查表檔的種子間隔為 0");

	out.jewelType = jewelType;
	out.seedMin = lo;
	out.seedMax = hi;
	out.seedInc = inc;
	out.seedCount = (hi - lo) / inc + 1;
	if (out.seedCount <= 0) return fail(u8"深淵查表檔的種子範圍無效");

	if (out.fmt == "ABYS") {
		int socketCount = 0, size = 0;
		if (!rd_u8(blob, o, socketCount) || !rd_u8(blob, o, size))
			return fail(u8"深淵查表檔插槽表頭讀取失敗");
		out.abyssSize = size;
		out.socketIds.reserve((size_t)socketCount);
		for (int i = 0; i < socketCount; i++) {
			int id = 0;
			if (!rd_u16(blob, o, id)) return fail(u8"深淵查表檔插槽清單讀取失敗");
			out.socketIds.push_back(id);
		}
		for (size_t i = 0; i < out.socketIds.size(); i++) {
			out.blockOffsets[out.socketIds[i]] = o;
			for (int s = 0; s < out.seedCount; s++)
				if (!skip_socket_record(blob, o))
					return fail(u8"深淵查表檔在插槽 " + std::to_string(out.socketIds[i]) +
					            u8" 的第 " + std::to_string(s) + u8" 個種子處截斷");
		}
	} else {
		int nodeCount = 0;
		if (!rd_u16(blob, o, nodeCount)) return fail(u8"深淵查表檔節點表頭讀取失敗");
		std::vector<int> nodeIds;
		nodeIds.reserve((size_t)nodeCount);
		for (int i = 0; i < nodeCount; i++) {
			int id = 0;
			if (!rd_u16(blob, o, id)) return fail(u8"深淵查表檔節點清單讀取失敗");
			nodeIds.push_back(id);
		}
		for (size_t i = 0; i < nodeIds.size(); i++) {
			out.blockOffsets[nodeIds[i]] = o;
			for (int s = 0; s < out.seedCount; s++)
				if (!skip_modification(blob, o)) return fail(u8"深淵查表檔節點區塊截斷");
		}
		if (blob.compare(o, 4, "ASCS") != 0) return fail(u8"深淵查表檔缺少昇華區段(ASCS)");
		o += 4;
		int ascCount = 0;
		if (!rd_u16(blob, o, ascCount)) return fail(u8"深淵查表檔昇華表頭讀取失敗");
		for (int i = 0; i < ascCount; i++) {
			int nameLen = 0;
			if (!rd_u8(blob, o, nameLen)) return fail(u8"深淵查表檔昇華名稱長度讀取失敗");
			if (o + (size_t)nameLen > blob.size()) return fail(u8"深淵查表檔昇華名稱截斷");
			const std::string name = blob.substr(o, (size_t)nameLen);
			o += (size_t)nameLen;
			out.ascOffsets[name] = o;
			for (int s = 0; s < out.seedCount; s++)
				if (!skip_ascendancy_record(blob, o)) return fail(u8"深淵查表檔昇華區塊截斷");
		}
	}

	// The walk is the check. Every record is variable length, so landing exactly
	// on the end of the file means no length was misread anywhere; landing
	// anywhere else means the layout is not what this code believes it is, and
	// the block offsets above are already worthless.
	if (o != blob.size())
		return fail(u8"深淵查表檔走訪結束於位元組 " + std::to_string(o) + u8",但檔案長 " +
		            std::to_string(blob.size()) + u8" —— 格式與本程式認知不符");

	out.ok = true;
	return true;
}

int TJAbyssSeedIndex(const TJAbyssLUT& lut, int seed)
{
	if (!lut.ok || lut.seedInc == 0) return -1;
	const int off = seed - lut.seedMin;
	if (off < 0 || seed > lut.seedMax || off % lut.seedInc != 0) return -1;
	return off / lut.seedInc;
}

// Where each seed's record starts inside one block. Records vary in length, so
// this is a linear count; PoB caches it the same way and for the same reason.
static const std::vector<size_t>* block_seed_offsets(const std::string& blob, TJAbyssLUT& lut,
                                                     int blockKey,
                                                     bool (*skipper)(const std::string&, size_t&))
{
	auto cached = lut.seedOffsets.find(blockKey);
	if (cached != lut.seedOffsets.end()) return &cached->second;
	auto it = lut.blockOffsets.find(blockKey);
	if (it == lut.blockOffsets.end()) return nullptr;

	std::vector<size_t> offs;
	offs.reserve((size_t)lut.seedCount);
	size_t o = it->second;
	for (int s = 0; s < lut.seedCount; s++) {
		offs.push_back(o);
		if (!skipper(blob, o)) return nullptr;
	}
	return &(lut.seedOffsets[blockKey] = offs);
}

bool TJAbyssReadSocket(const TJDataset& ds, const std::string& blob, TJAbyssLUT& lut,
                       int socketId, int seed, std::map<int, TJAbyssMod>& out)
{
	out.clear();
	if (!lut.ok || lut.fmt != "ABYS") return false;
	const int si = TJAbyssSeedIndex(lut, seed);
	if (si < 0) return false;
	const std::vector<size_t>* offs = block_seed_offsets(blob, lut, socketId, skip_socket_record);
	if (!offs || si >= (int)offs->size()) return false;

	size_t o = (*offs)[(size_t)si];
	int affected = 0;
	if (!rd_u8(blob, o, affected)) return false;
	for (int i = 0; i < affected; i++) {
		int nodeId = 0;
		if (!rd_u16(blob, o, nodeId)) return false;
		TJAbyssMod mod;
		if (!read_modification(ds, blob, o, lut.jewelType, mod)) return false;
		out[nodeId] = mod;
	}
	return true;
}

bool TJAbyssReadNode(const TJDataset& ds, const std::string& blob, TJAbyssLUT& lut,
                     int nodeId, int seed, TJAbyssMod& out)
{
	out.clear();
	if (!lut.ok || lut.fmt != "ABYN") return false;
	const int si = TJAbyssSeedIndex(lut, seed);
	if (si < 0) return false;
	const std::vector<size_t>* offs = block_seed_offsets(blob, lut, nodeId, skip_modification);
	if (!offs || si >= (int)offs->size()) return false;
	size_t o = (*offs)[(size_t)si];
	return read_modification(ds, blob, o, lut.jewelType, out);
}

bool TJAbyssReadNodes(const TJDataset& ds, const std::string& blob, TJAbyssLUT& lut,
                      const std::vector<int>& nodeIds, int seed,
                      std::map<int, TJAbyssMod>& out)
{
	out.clear();
	if (!lut.ok || lut.fmt != "ABYN") return false;
	if (TJAbyssSeedIndex(lut, seed) < 0) return false;
	for (size_t i = 0; i < nodeIds.size(); i++) {
		// A passive the file has no block for is not an error: the tree carries
		// sockets and class starts this jewel never touches.
		if (lut.blockOffsets.find(nodeIds[i]) == lut.blockOffsets.end()) continue;
		TJAbyssMod mod;
		if (TJAbyssReadNode(ds, blob, lut, nodeIds[i], seed, mod) && !mod.empty())
			out[nodeIds[i]] = mod;
	}
	return true;
}

bool TJAbyssReadAscendancies(const std::string& blob, TJAbyssLUT& lut, int seed,
                             std::map<std::string, std::vector<int> >& out)
{
	out.clear();
	if (!lut.ok || lut.fmt != "ABYN") return false;
	const int si = TJAbyssSeedIndex(lut, seed);
	if (si < 0) return false;
	for (std::map<std::string, size_t>::const_iterator it = lut.ascOffsets.begin();
	     it != lut.ascOffsets.end(); ++it) {
		// Ascendancy blocks are keyed by name, not by an int, so they cannot use
		// the same offset cache as the node blocks. They are 21 short records
		// each, so walking them is cheap enough not to need one.
		size_t o = it->second;
		for (int s = 0; s < si; s++)
			if (!skip_ascendancy_record(blob, o)) return false;
		int selected = 0;
		if (!rd_u8(blob, o, selected)) return false;
		std::vector<int> ids;
		for (int k = 0; k < selected; k++) {
			int nid = 0;
			if (!rd_u16(blob, o, nid)) return false;
			ids.push_back(nid);
		}
		out[it->first] = ids;
	}
	return true;
}

double TJAbyssRollFor(const TJAbyssComponent& c, const TJStatMod& m)
{
	const int i = m.index - 1;
	if (i < 0 || i >= (int)c.rolls.size()) return 0.0;
	double v = (double)c.rolls[i];
	// "(15-20)% reduced Critical Strike Chance" stores its roll as -16: the
	// direction is already in the word "reduced", so printing the sign as well
	// would state the opposite. Only flip when the stat's own range is
	// non-negative — a genuinely negative range keeps its sign.
	if (v < 0 && m.min >= 0) v = -v;
	return v;
}

TJTransform TJAbyssApply(const TJDataset& ds, const TJAbyssMod& mod)
{
	TJTransform out;
	for (size_t ci = 0; ci < mod.size(); ci++) {
		const TJAbyssComponent& c = mod[ci];
		const TJEntry* e = nullptr;
		bool replaces = false;
		if (c.type == 1) {
			e = TJNodeAt(ds, c.globalId);
			replaces = true;
		} else if (c.type == 2) {
			e = TJAdditionAt(ds, c.globalId);
		}
		if (!e) {
			out.note = "unhandled Abyss component id " + std::to_string(c.globalId) +
			           " (type " + std::to_string(c.type) + ")";
			continue;
		}
		out.ok = true;
		if (replaces) {
			out.replaced = true;
			// Abyss additions carry the game's own placeholder name ("Notable 39")
			// and no Chinese one, so only a replacement contributes a name. The
			// caller shows the stat lines for the rest, which is what the game
			// shows too.
			out.newName = e->dn;
			out.newNameZh = e->dnZh;
		}
		// PoB applies every one of the entry's stats to every one of its lines
		// and lets the pattern decide which fires — a line can hold a range that
		// belongs to a different stat index than its own position.
		for (size_t li = 0; li < e->sd.size(); li++) {
			std::string en = e->sd[li];
			std::string zh = li < e->sdZh.size() ? e->sdZh[li] : std::string();
			for (std::map<std::string, TJStatMod>::const_iterator it = e->stats.begin();
			     it != e->stats.end(); ++it) {
				const double v = TJAbyssRollFor(c, it->second);
				en = TJRollStat(en, it->first, it->second, v);
				if (!zh.empty()) zh = TJRollStat(zh, it->first, it->second, v);
			}
			out.lines.push_back(en);
			out.linesZh.push_back(zh);
		}
	}
	return out;
}

std::vector<TJStatTemplate> TJAbyssStatTemplates(const TJDataset& ds, int jewelType)
{
	// Reuse the Legion picker: it already filters by the conqueror-type prefix,
	// and the Abyss ids carry theirs the same way ("abyss_murderous_...").
	return TJStatTemplates(ds, jewelType);
}

bool TJAbyssInScope(int scope, int nodeId, const TJDataset& ds,
                    const std::map<int, int>* nodeKind)
{
	if (scope == 0) return true;
	bool big; // notable or keystone
	if (nodeKind) {
		std::map<int, int>::const_iterator k = nodeKind->find(nodeId);
		if (k == nodeKind->end()) return false; // not a passive we can classify
		big = (k->second == kPtNotable || k->second == kPtKeystone);
	} else {
		// Index-only fallback. Keystones are not in the Legion node index at all,
		// so they land here as "small" — see the header.
		std::map<int, std::pair<int, int> >::const_iterator ni = ds.nodeIndex.find(nodeId);
		big = ni != ds.nodeIndex.end() && ni->second.first < ds.sizeNotable;
	}
	return scope == 1 ? big : !big;
}

std::vector<TJSeedHit> TJAbyssSearch(const TJDataset& ds, const std::string& blob,
                                     TJAbyssLUT& lut, const TJSearchQuery& q,
                                     int socketId, int topN,
                                     const std::map<int, int>* nodeKind,
                                     const volatile bool* cancel)
{
	std::vector<TJSeedHit> hits;
	if (!lut.ok || q.wants.empty()) return hits;
	const bool byNode = (lut.fmt == "ABYN");
	if (byNode) {
		// Zorath scores whatever the caller nominated. Refusing an empty list is
		// the point: "no candidates" means the caller has not said what the seed
		// should be judged on, and scoring every passive in the tree instead
		// would answer a question nobody asked.
		if (q.nodeIds.empty()) return hits;
	} else if (lut.fmt != "ABYS" || lut.blockOffsets.find(socketId) == lut.blockOffsets.end()) {
		return hits;
	}

	const TJWantMatcher matcher(q.wants);
	std::map<int, TJAbyssMod> affected;
	for (int seed = lut.seedMin; seed <= lut.seedMax; seed += lut.seedInc) {
		if (cancel && *cancel) break;
		const bool read = byNode
			? TJAbyssReadNodes(ds, blob, lut, q.nodeIds, seed, affected)
			: TJAbyssReadSocket(ds, blob, lut, socketId, seed, affected);
		if (!read) continue;

		double total = 0;
		int matches = 0;
		std::set<const TJWantStat*> distinct;
		for (std::map<int, TJAbyssMod>::const_iterator it = affected.begin();
		     it != affected.end(); ++it) {
			if (!TJAbyssInScope(q.scope, it->first, ds, nodeKind)) continue;
			TJTransform t = TJAbyssApply(ds, it->second);
			if (!t.ok) continue;
			for (size_t li = 0; li < t.lines.size(); li++) {
				if (const TJWantStat* w = matcher.Match(t.lines[li])) {
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

	std::sort(hits.begin(), hits.end(), [](const TJSeedHit& a, const TJSeedHit& b) {
		if (a.weight != b.weight) return a.weight > b.weight;
		if (a.distinctWants != b.distinctWants) return a.distinctWants > b.distinctWants;
		if (a.matches != b.matches) return a.matches > b.matches;
		return a.seed < b.seed;
	});
	if (topN > 0 && (int)hits.size() > topN) hits.resize((size_t)topN);
	return hits;
}

// ---- headless entry points ----------------------------------------------------

static void abyss_console()
{
	if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
		freopen_s(&f, "CONOUT$", "w", stderr);
	}
}

// Write a UTF-8 report next to the exe. This is a WIN32 GUI binary: it has no
// stdout of its own, and the console it attaches to belongs to whoever launched
// it, so piped output is lost. Every headless check here goes to a file for
// that reason.
static void abyss_write_report(const std::wstring& exeDir, const wchar_t* name,
                               const std::string& body)
{
	HANDLE h = CreateFileW((exeDir + name).c_str(), GENERIC_WRITE, 0,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD written = 0;
	WriteFile(h, body.data(), (DWORD)body.size(), &written, nullptr);
	CloseHandle(h);
}

// "--abyss <jewelType> <socketId> <seed>": report what one jewel does in one
// socket, to abyss_cli.txt. This runs the same composition the detail panel
// does -- read the record, map each conquered id onto the tree, render its
// lines -- so what lands in the file is what the window shows.
int RunAbyssCli(const std::wstring& exeDir, int jewelType, int socketId, int seed)
{
	abyss_console();
	std::string out;
	auto emit = [&](const std::string& line) {
		out += line + "\n";
		printf("%s\n", line.c_str());
	};
	auto finish = [&](int code) {
		abyss_write_report(exeDir, L"abyss_cli.txt", out);
		return code;
	};

	TJDataset ds;
	std::string err;
	if (!ds.Load(exeDir + L"Data\\timeless_jewels.json", &err)) {
		emit("load dataset: " + err);
		return finish(1);
	}
	if (!TJAbyssUsable(jewelType)) {
		emit("jewel type " + std::to_string(jewelType) + " is not one this reads (7-10)");
		return finish(1);
	}
	PassiveTreeData tree;
	std::string terr;
	const bool haveTree = tree.Load(exeDir, &terr);
	if (!haveTree) emit("(no passive tree: " + terr + " -- ids only)");

	std::string blob;
	if (!TJLoadBin(exeDir, ds, jewelType, blob, &err)) { emit(err); return finish(1); }
	TJAbyssLUT lut;
	if (!TJAbyssParse(blob, jewelType, lut, &err)) { emit(err); return finish(1); }

	if (socketId == 0) {
		std::string s = "sockets:";
		for (size_t i = 0; i < lut.socketIds.size(); i++)
			s += " " + std::to_string(lut.socketIds[i]);
		emit(s);
		emit("seeds " + std::to_string(lut.seedMin) + ".." + std::to_string(lut.seedMax) +
		     " step " + std::to_string(lut.seedInc));
		return finish(0);
	}

	std::map<int, TJAbyssMod> got;
	if (TJIsZorath(jewelType)) {
		// Zorath is keyed by passive, not by socket, so "what does this socket
		// take" has no answer. Report what the seed does to the passives around
		// the socket instead — and say that is what it is.
		if (!haveTree) {
			emit("Zorath needs the passive tree to pick the passives around a socket");
			return finish(1);
		}
		const int sIdx = tree.IndexOfId(socketId);
		if (sIdx < 0) {
			emit("socket " + std::to_string(socketId) + " is not in the tree");
			return finish(1);
		}
		std::vector<int> ids;
		// not `near`: windows.h still defines that as a macro
		std::vector<int> around = tree.NodesInRadius(sIdx, 1800.0f);
		for (size_t i = 0; i < around.size(); i++)
			ids.push_back(tree.nodes[(size_t)around[i]].id);
		if (!TJAbyssReadNodes(ds, blob, lut, ids, seed, got)) {
			emit("no record for seed " + std::to_string(seed));
			return finish(1);
		}
		emit((ds.types.count(jewelType) ? ds.types.at(jewelType) : "?") +
		     "  seed " + std::to_string(seed) + "  -> " + std::to_string(got.size()) +
		     " passives near socket " + std::to_string(socketId) +
		     " (ESTIMATE: which ones apply follows the character's allocated path)");

		std::map<std::string, std::vector<int> > asc;
		if (TJAbyssReadAscendancies(blob, lut, seed, asc)) {
			emit("  -- Ascendancy picks for this seed (exact; no path involved) --");
			for (std::map<std::string, std::vector<int> >::const_iterator a = asc.begin();
			     a != asc.end(); ++a) {
				std::string line = "  " + a->first + ": ";
				if (a->second.empty()) {
					line += "(none)";
				} else {
					for (size_t i = 0; i < a->second.size(); i++) {
						// The Ascendancy notable's own name is not available:
						// PobTools' tree deliberately leaves Ascendancy nodes out.
						// What it BECOMES is, and that is the part being chosen.
						if (i) line += " ; ";
						line += "#" + std::to_string(a->second[i]) + " -> ";
						TJAbyssMod m;
						if (!TJAbyssReadNode(ds, blob, lut, a->second[i], seed, m)) {
							line += "(no block)";
							continue;
						}
						TJTransform t = TJAbyssApply(ds, m);
						if (t.replaced) line += (t.newNameZh.empty() ? t.newName : t.newNameZh) + " ";
						for (size_t k = 0; k < t.lines.size(); k++) {
							const std::string& z = (k < t.linesZh.size() && !t.linesZh[k].empty())
							                       ? t.linesZh[k] : t.lines[k];
							line += (k ? " / " : "") + z;
						}
					}
				}
				emit(line);
			}
			emit("  -- passives near the socket --");
		}
	} else if (!TJAbyssReadSocket(ds, blob, lut, socketId, seed, got)) {
		emit("no record for socket " + std::to_string(socketId) +
		     " seed " + std::to_string(seed));
		return finish(1);
	} else {
		emit((ds.types.count(jewelType) ? ds.types.at(jewelType) : "?") +
		     "  socket " + std::to_string(socketId) + "  seed " + std::to_string(seed) +
		     "  -> " + std::to_string(got.size()) + " conquered passives");
	}
	for (std::map<int, TJAbyssMod>::const_iterator it = got.begin(); it != got.end(); ++it) {
		std::string name = std::to_string(it->first);
		if (haveTree) {
			const int idx = tree.IndexOfId(it->first);
			if (idx >= 0) {
				const PtNode& n = tree.nodes[(size_t)idx];
				name = (n.nameZh.empty() ? n.name : n.nameZh) + " (" + std::to_string(n.id) + ")";
			}
		}
		TJTransform t = TJAbyssApply(ds, it->second);
		std::string line = "  " + name + (t.replaced ? "  => " : "  += ");
		if (t.replaced) line += (t.newNameZh.empty() ? t.newName : t.newNameZh) + "  ";
		for (size_t i = 0; i < t.lines.size(); i++) {
			const std::string& s = (i < t.linesZh.size() && !t.linesZh[i].empty())
			                       ? t.linesZh[i] : t.lines[i];
			line += (i ? " / " : "") + s;
		}
		if (!t.note.empty()) line += "   [" + t.note + "]";
		emit(line);
	}
	return finish(0);
}

// ---- selftest -----------------------------------------------------------------
//
// The pinned values below were read off PoB 2.67.1's shipped containers and
// cross-checked against Parazeya/abyss-jewels' independent walk (4494 of 4494
// (socket, seed) node sets identical; the same comparison with the seed shifted
// by one matched 0 of 4494, so it can tell seeds apart). If the game's passive
// tree changes these move, and the failure text says so — a stale pin should
// read as "the data moved", not as "the reader broke".

static std::string join_lines(const std::vector<std::string>& v)
{
	std::string s;
	for (size_t i = 0; i < v.size(); i++) {
		if (i) s += " | ";
		s += v[i];
	}
	return s;
}

int RunAbyssSelfTest(const std::wstring& exeDir)
{
	abyss_console();
	TJDataset ds;
	std::string err;
	if (!ds.Load(exeDir + L"Data\\timeless_jewels.json", &err)) {
		printf("FAIL load dataset: %s\n", err.c_str());
		return 1;
	}

	int failures = 0, checks = 0;
	std::string report;
	auto check = [&](bool ok, const std::string& what, const std::string& detail = "") {
		checks++;
		const std::string line = std::string(ok ? "PASS  " : "FAIL  ") + what +
		                         (detail.empty() ? "" : "  -> " + detail) + "\n";
		report += line;
		printf("%s", line.c_str());
		if (!ok) failures++;
	};

	// A1 the type gate: 7-10 usable, 11 parsed but not offered.
	check(TJIsAbyss(7) && TJIsAbyss(11) && !TJIsAbyss(6), "types 7-11 are the Abyss ones");
	check(TJAbyssUsable(10) && TJAbyssUsable(11) && !TJAbyssUsable(6),
	      "all five Abyss jewels are readable");
	check(TJIsZorath(11) && !TJIsZorath(10),
	      "only Zorath is keyed by passive node rather than by socket");

	// A2 the "g"-format regression that the Abyss rolls exposed. Life regen is
	// stored per minute; without the scaling this prints 60 instead of 1.
	{
		TJStatMod g;
		g.fmt = "g"; g.index = 1; g.min = 0.7; g.max = 1.2;
		const std::string tmpl = "Regenerate (0.7-1.2)% of Life per second";
		const std::string got = TJRollStat(tmpl, "life_regeneration_rate_per_minute_%", g, 60.0);
		check(got == "Regenerate 1% of Life per second",
		      "a per-minute roll is divided by 60 before it is printed", got);

		TJStatMod p;
		p.fmt = "g"; p.index = 1; p.min = 0.2; p.max = 0.2;
		const std::string pg = TJRollStat("0.2% of Fire Damage Leeched as Life",
		                                  "base_life_leech_from_fire_damage_permyriad", p, 40.0);
		check(pg == "0.4% of Fire Damage Leeched as Life",
		      "a permyriad roll is divided by 100 before it is printed", pg);

		TJStatMod d;
		d.fmt = "d"; d.index = 1; d.min = 2; d.max = 4;
		check(TJRollStat("(2-4)% increased Impale Effect", "impale_debuff_effect_+%", d, 3.0) ==
		      "3% increased Impale Effect", "a plain roll is substituted unscaled");
	}

	// The tree supplies the node kinds the scope filter needs; without it the
	// keystone checks below cannot run and say so instead of being skipped.
	PassiveTreeData tree;
	std::map<int, int> nodeKind;
	{
		std::string terr;
		if (!tree.Load(exeDir, &terr)) {
			check(false, "load the passive tree", terr);
		} else {
			for (size_t i = 0; i < tree.nodes.size(); i++)
				nodeKind[tree.nodes[i].id] = tree.nodes[i].kind;
		}
	}

	std::string blob;
	TJAbyssLUT lut;
	if (!TJLoadBin(exeDir, ds, 7, blob, &err)) {
		check(false, "load AbyssTecrod container", err);
	} else if (!TJAbyssParse(blob, 7, lut, &err)) {
		check(false, "parse AbyssTecrod container", err);
	} else {
		// A3 header. The walk inside TJAbyssParse already proved every record
		// length is right by landing exactly on the end of the file.
		check(lut.fmt == "ABYS" && lut.jewelType == 7, "Tecrod is an ABYS container", lut.fmt);
		check(lut.seedMin == 100 && lut.seedMax == 8000 && lut.seedInc == 1 &&
		      lut.seedCount == 7901,
		      "seed range 100..8000 step 1",
		      std::to_string(lut.seedMin) + ".." + std::to_string(lut.seedMax) + " step " +
		      std::to_string(lut.seedInc) + " = " + std::to_string(lut.seedCount));
		check(lut.socketIds.size() == 21, "21 jewel sockets",
		      std::to_string(lut.socketIds.size()));

		// A4 two independent sources for the socket list: the container header,
		// and the tree PobTools generates from GGG's export. They must agree.
		{
			std::set<int> fromTree, fromFile;
			for (size_t i = 0; i < tree.sockets.size(); i++)
				fromTree.insert(tree.nodes[(size_t)tree.sockets[i]].id);
			for (size_t i = 0; i < lut.socketIds.size(); i++) fromFile.insert(lut.socketIds[i]);
			check(!fromTree.empty() && fromTree == fromFile,
			      "the container's sockets are exactly the tree's jewel sockets",
			      std::to_string(fromTree.size()) + " tree / " +
			      std::to_string(fromFile.size()) + " file");
		}

		// A4b the scope filter must see keystones. The Abyss walk conquers them,
		// but the Legion node index does not carry keystones at all, so the
		// index-only classification files every one of them under "small". That
		// is the bug this asserts against: same node, two answers.
		{
			std::map<int, TJAbyssMod> rec;
			int keystones = 0, keystonesBigWithTree = 0, keystonesBigWithoutTree = 0;
			for (size_t si = 0; si < lut.socketIds.size(); si++) {
				if (!TJAbyssReadSocket(ds, blob, lut, lut.socketIds[si], 4096, rec)) continue;
				for (std::map<int, TJAbyssMod>::const_iterator it = rec.begin();
				     it != rec.end(); ++it) {
					std::map<int, int>::const_iterator k = nodeKind.find(it->first);
					if (k == nodeKind.end() || k->second != kPtKeystone) continue;
					keystones++;
					if (TJAbyssInScope(1, it->first, ds, &nodeKind)) keystonesBigWithTree++;
					if (TJAbyssInScope(1, it->first, ds, nullptr)) keystonesBigWithoutTree++;
				}
			}
			check(keystones > 0, "seed 4096 conquers at least one keystone somewhere",
			      std::to_string(keystones) + " across 21 sockets");
			check(keystones > 0 && keystonesBigWithTree == keystones,
			      "with tree kinds, every conquered keystone counts as a big passive",
			      std::to_string(keystonesBigWithTree) + "/" + std::to_string(keystones));
			check(keystones > 0 && keystonesBigWithoutTree == 0,
			      "without them it drops every one — which is why the map is passed in",
			      std::to_string(keystonesBigWithoutTree) + "/" + std::to_string(keystones));
			check(TJAbyssInScope(0, 999999, ds, &nodeKind), "scope 0 keeps everything");
		}

		// A5 seed addressing rejects what it cannot answer instead of guessing.
		check(TJAbyssSeedIndex(lut, 100) == 0 && TJAbyssSeedIndex(lut, 8000) == 7900,
		      "seed index maps the ends of the range");
		check(TJAbyssSeedIndex(lut, 99) < 0 && TJAbyssSeedIndex(lut, 8001) < 0,
		      "seeds outside the range have no index");

		// A6 the pinned record: socket 2491, seed 100.
		std::map<int, TJAbyssMod> got;
		if (!TJAbyssReadSocket(ds, blob, lut, 2491, 100, got)) {
			check(false, "read socket 2491 seed 100");
		} else {
			check(got.size() == 45, "socket 2491 seed 100 conquers 45 passives",
			      std::to_string(got.size()) + " (game data may have changed)");

			// a replacement: the passive becomes a different one
			std::map<int, TJAbyssMod>::const_iterator it = got.find(2092);
			if (it == got.end() || it->second.size() != 1) {
				check(false, "node 2092 has one component");
			} else {
				const TJAbyssComponent& c = it->second[0];
				const TJEntry* e = TJNodeAt(ds, c.globalId);
				check(c.type == 1 && e && e->id == "abyss_murderous_small_attribute9",
				      "node 2092 is replaced by abyss_murderous_small_attribute9",
				      e ? e->id : "unresolved");
				TJTransform t = TJAbyssApply(ds, it->second);
				check(t.ok && t.replaced && t.newName == "Impale Effect" &&
				      t.lines.size() == 1 && t.lines[0] == "3% increased Impale Effect",
				      "node 2092 rolls 3 into its own range", join_lines(t.lines));
				check(!t.newNameZh.empty(), "a replacement carries a Chinese name", t.newNameZh);
			}

			// an addition: the passive keeps itself and gains stats. Both of its
			// two rolls must land in the right line, which is what would break if
			// the rolls were read in the wrong order.
			it = got.find(5126);
			if (it == got.end()) {
				check(false, "node 5126 present");
			} else {
				TJTransform t = TJAbyssApply(ds, it->second);
				check(t.ok && !t.replaced && t.lines.size() == 2 &&
				      t.lines[0] == "10 to 0 Added Cold Damage with Axe Attacks" &&
				      t.lines[1] == "0 to 18 Added Cold Damage with Axe Attacks",
				      "node 5126 puts each of its two rolls in its own line",
				      join_lines(t.lines));
			}
		}

		// A7 the search agrees with the reader about what a hit is.
		{
			// Pick a template this jewel really produces, so the query is not
			// vacuous by construction.
			std::vector<TJStatTemplate> tmpl = TJAbyssStatTemplates(ds, 7);
			check(tmpl.size() > 20, "Tecrod offers a stat picker list",
			      std::to_string(tmpl.size()));
			std::string want;
			for (size_t i = 0; i < tmpl.size() && want.empty(); i++)
				if (tmpl[i].en.find("increased Impale Effect") != std::string::npos)
					want = tmpl[i].en;
			if (want.empty()) {
				check(false, "found the Impale Effect template in the picker");
			} else {
				TJSearchQuery q;
				q.jewelType = 7;
				q.scope = 0;
				q.wants.push_back({ want, 0.0, 1.0 });
				// Timed, because a search is a full sweep of 7901 seeds and the UI
				// runs it on a worker while the window keeps drawing: if this
				// ever creeps into tens of seconds that is a product problem, not
				// a detail. Reported, not asserted — machines differ.
				const DWORD t0 = GetTickCount();
				std::vector<TJSeedHit> hits = TJAbyssSearch(ds, blob, lut, q, 2491, 20, &nodeKind, nullptr);
				const DWORD dt = GetTickCount() - t0;
				check(!hits.empty(), "a reachable stat finds seeds",
				      std::to_string(hits.size()) + " hits, full 7901-seed sweep in " +
				      std::to_string(dt) + " ms");

				bool ordered = true;
				for (size_t i = 1; i < hits.size(); i++)
					ordered = ordered && hits[i - 1].weight >= hits[i].weight;
				check(ordered, "results are ordered by weight");

				// Re-count one hit by hand through the reader. If the search and
				// the reader ever disagree the number the UI shows is fiction.
				if (!hits.empty()) {
					std::map<int, TJAbyssMod> rec;
					TJAbyssReadSocket(ds, blob, lut, 2491, hits[0].seed, rec);
					TJWantMatcher m(q.wants);
					int counted = 0;
					for (std::map<int, TJAbyssMod>::const_iterator r = rec.begin();
					     r != rec.end(); ++r) {
						TJTransform t = TJAbyssApply(ds, r->second);
						for (size_t li = 0; li < t.lines.size(); li++)
							if (m.Match(t.lines[li])) counted++;
					}
					check(counted == hits[0].matches,
					      "the search's hit count is reproducible from the reader",
					      std::to_string(counted) + " vs " + std::to_string(hits[0].matches));
				}

				TJSearchQuery qm = q;
				qm.wants[0].minValue = 100000.0;
				check(TJAbyssSearch(ds, blob, lut, qm, 2491, 0, &nodeKind, nullptr).empty(),
				      "an unreachable minimum rejects every seed");

				check(TJAbyssSearch(ds, blob, lut, q, 999999, 20, &nodeKind, nullptr).empty(),
				      "an unknown socket returns nothing rather than reading elsewhere");
			}
		}

		// A8 a damaged container must be refused, not half-read. Chopping the
		// tail cannot change any header field, so only the end-of-walk check can
		// catch it — this is what proves that check is load-bearing.
		{
			TJAbyssLUT bad;
			std::string berr;
			const bool parsed = TJAbyssParse(blob.substr(0, blob.size() - 1), 7, bad, &berr);
			check(!parsed && !bad.ok, "a truncated container is refused", berr);

			std::string wrongType = blob;
			wrongType[5] = (char)8;
			TJAbyssLUT bad2;
			check(!TJAbyssParse(wrongType, 7, bad2, &berr),
			      "a container for another jewel type is refused", berr);
		}
	}

	// A9 the node set must not depend on the jewel type — the walk that picks
	// the passives is shared, only what they become differs. Checked against a
	// second file so a shared bug in one decode path cannot hide it.
	{
		std::string ulaman;
		TJAbyssLUT lut8;
		if (!TJLoadBin(exeDir, ds, 8, ulaman, &err) || !TJAbyssParse(ulaman, 8, lut8, &err)) {
			check(false, "load and parse AbyssUlaman container", err);
		} else if (lut.ok) {
			const int seeds[] = { 100, 137, 2000, 4096, 7629, 8000 };
			bool identical = true;
			int compared = 0;
			for (size_t si = 0; si < lut.socketIds.size() && identical; si++) {
				for (size_t k = 0; k < sizeof(seeds) / sizeof(seeds[0]) && identical; k++) {
					std::map<int, TJAbyssMod> a, b;
					if (!TJAbyssReadSocket(ds, blob, lut, lut.socketIds[si], seeds[k], a) ||
					    !TJAbyssReadSocket(ds, ulaman, lut8, lut.socketIds[si], seeds[k], b)) {
						identical = false;
						break;
					}
					compared++;
					if (a.size() != b.size()) { identical = false; break; }
					std::map<int, TJAbyssMod>::const_iterator ia = a.begin(), ib = b.begin();
					for (; ia != a.end(); ++ia, ++ib)
						if (ia->first != ib->first) { identical = false; break; }
				}
			}
			check(identical && compared == 126,
			      "Tecrod and Ulaman conquer the same passives for the same (socket, seed)",
			      std::to_string(compared) + " pairs compared");

			// A10 the negative-roll rule. Stored negatives only occur on two of
			// the four jewels, so a test written against Tecrod alone would never
			// reach it. Assert the stored value really is negative first, or the
			// rule below is being "confirmed" by a case that never needed it.
			std::map<int, TJAbyssMod> rec;
			if (!TJAbyssReadSocket(ds, ulaman, lut8, 2491, 120, rec) || !rec.count(34601)) {
				check(false, "read Ulaman socket 2491 seed 120 node 34601");
			} else {
				const TJAbyssMod& mod = rec[34601];
				bool storedNegative = false;
				for (size_t i = 0; i < mod.size(); i++)
					for (size_t r = 0; r < mod[i].rolls.size(); r++)
						if (mod[i].rolls[r] < 0) storedNegative = true;
				check(storedNegative, "this fixture really does store a negative roll");

				TJTransform t = TJAbyssApply(ds, mod);
				const std::string joined = join_lines(t.lines);
				check(t.ok && joined.find("16% reduced") != std::string::npos &&
				      joined.find("-16") == std::string::npos,
				      "a negative roll on a non-negative range prints as a positive "
				      "(the word 'reduced' already carries the direction)", joined);
			}
		}
	}

	// A11 Zorath. Its container answers a different question, so it gets a
	// different set of checks: what one passive becomes is exact, which passives
	// apply is not knowable here, and the Ascendancy pick needs no path at all.
	{
		std::string zorath;
		TJAbyssLUT lz;
		if (!TJLoadBin(exeDir, ds, 11, zorath, &err)) {
			check(false, "load AbyssZorath container", err);
		} else if (!TJAbyssParse(zorath, 11, lz, &err)) {
			check(false, "parse AbyssZorath container", err);
		} else {
			check(lz.fmt == "ABYN" && lz.blockOffsets.size() > 2000,
			      "Zorath is an ABYN container keyed by passive node",
			      lz.fmt + ", " + std::to_string(lz.blockOffsets.size()) + " node blocks");
			check(lz.ascOffsets.size() == 21 && lz.ascOffsets.count("Ascendant") &&
			      lz.ascOffsets.count("Deadeye"),
			      "Zorath carries a per-Ascendancy section",
			      std::to_string(lz.ascOffsets.size()) + " ascendancies");

			std::map<int, TJAbyssMod> none;
			check(!TJAbyssReadSocket(ds, zorath, lz, 2491, 100, none) && none.empty(),
			      "the socket reader refuses an ABYN container rather than misreading it");

			// A11a one passive, exactly. Same value the pre-container selftest
			// pinned for (seed 100, node 6) — the container reorganised the file
			// without changing the answer.
			TJAbyssMod mod;
			if (!TJAbyssReadNode(ds, zorath, lz, 6, 100, mod) || mod.size() != 1) {
				check(false, "read Zorath node 6 seed 100");
			} else {
				const TJEntry* e = TJAdditionAt(ds, mod[0].globalId);
				check(mod[0].type == 2 && mod[0].globalId == 314 && e &&
				      e->id == "abyss_special_notable_53",
				      "Zorath node 6 seed 100 maps localId 52 -> global 314",
				      e ? e->id : "unresolved");
				TJTransform t = TJAbyssApply(ds, mod);
				check(t.ok && !t.replaced && t.lines.size() == 1 &&
				      t.lines[0] == "25% increased Evasion Rating while moving",
				      "and rolls 25 into its own range", join_lines(t.lines));
			}

			// A11b every passive has an answer for every seed. This is what makes
			// the socket a place to look rather than a lookup key, so it is worth
			// asserting rather than remembering.
			{
				int have = 0;
				const int probe[] = { 6, 94, 127, 223, 224, 238 };
				for (size_t i = 0; i < sizeof(probe) / sizeof(probe[0]); i++) {
					TJAbyssMod m;
					if (TJAbyssReadNode(ds, zorath, lz, probe[i], 4096, m) && !m.empty()) have++;
				}
				check(have == 6, "every sampled passive has a modification under one seed",
				      std::to_string(have) + "/6");
			}

			// A11c a passive the file has no block for is skipped, not invented.
			{
				std::vector<int> ids;
				ids.push_back(6);
				ids.push_back(999999);
				std::map<int, TJAbyssMod> got;
				check(TJAbyssReadNodes(ds, zorath, lz, ids, 100, got) && got.size() == 1 &&
				      got.count(6) == 1,
				      "an unknown passive id is skipped rather than read from elsewhere",
				      std::to_string(got.size()) + " of 2 returned");
			}

			// A11d the Ascendancy pick. No path is involved, so this half is
			// exact. Ascendant getting nothing is the discriminating case: all of
			// its notables cost five points and the jewel cannot rewrite one
			// costing four or more, which is also what Parazeya's in-game
			// observations show.
			std::map<std::string, std::vector<int> > a100, a368;
			if (!TJAbyssReadAscendancies(zorath, lz, 100, a100) ||
			    !TJAbyssReadAscendancies(zorath, lz, 368, a368)) {
				check(false, "read the Ascendancy section");
			} else {
				check(a100.size() == 21, "every Ascendancy has an entry",
				      std::to_string(a100.size()));
				check(a100.count("Ascendant") && a100["Ascendant"].empty() &&
				      a368["Ascendant"].empty(),
				      "Ascendant is picked for nothing — its notables all cost 5 points");
				check(a100.count("Deadeye") && a100["Deadeye"].size() == 1 &&
				      a100["Deadeye"][0] == 2872,
				      "Deadeye seed 100 picks node 2872",
				      a100["Deadeye"].empty() ? "(none)" : std::to_string(a100["Deadeye"][0]));
				// Anti-vacuity: if the picks never moved with the seed, the reader
				// could be returning the same record every time and still "pass".
				check(a100["Berserker"] != a368["Berserker"],
				      "a different seed picks a different Ascendancy notable");
			}

			// A11e the search must refuse to guess. With no candidate passives it
			// has not been told what to judge, and scoring the whole tree instead
			// would answer a question nobody asked.
			{
				TJSearchQuery qz;
				qz.jewelType = 11;
				qz.scope = 0;
				qz.wants.push_back({ "#% increased Attack Speed", 0.0, 1.0 });
				check(TJAbyssSearch(ds, zorath, lz, qz, 2491, 10, &nodeKind, nullptr).empty(),
				      "a Zorath search with no candidate passives returns nothing");

				qz.nodeIds.push_back(6);
				qz.nodeIds.push_back(94);
				qz.nodeIds.push_back(127);
				qz.nodeIds.push_back(223);
				std::vector<TJSeedHit> zh =
					TJAbyssSearch(ds, zorath, lz, qz, 2491, 10, &nodeKind, nullptr);
				check(!zh.empty(), "given passives to judge, it finds seeds",
				      std::to_string(zh.size()) + " hits");
			}
		}
	}

	// A12 this run must have actually tested something. A file that fails to
	// load would otherwise "pass" with two checks and no coverage.
	check(checks >= 25, "the run performed its full set of checks",
	      std::to_string(checks) + " checks");

	const std::string tail = failures == 0
		? "\nALL PASS (" + std::to_string(checks) + " checks)\n"
		: "\nFAILURES: " + std::to_string(failures) + " of " + std::to_string(checks) + "\n";
	report += tail;
	printf("%s", tail.c_str());

	HANDLE h = CreateFileW((exeDir + L"abyss_selftest.txt").c_str(), GENERIC_WRITE, 0,
	                       nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD written = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &written, nullptr);
		CloseHandle(h);
	}
	return failures == 0 ? 0 : 1;
}
