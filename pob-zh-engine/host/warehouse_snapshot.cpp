#include "warehouse_snapshot.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>

using nlohmann::ordered_json;

namespace {

const char* GameId(const std::string& game) { return game == "poe2" ? "poe2" : "poe1"; }

// Before per-league files: every league of one game in one file.
std::wstring LegacyPath(const std::wstring& exeDir, const std::string& game)
{
	return exeDir + (game == "poe2" ? L"PobTools\\warehouse_poe2.json"
	                                : L"PobTools\\warehouse_poe1.json");
}

bool ReadAll(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 26)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() ||
		     (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

bool WriteAtomic(const std::wstring& dst, const std::string& body)
{
	// Unique per writer: two panels (two processes) can save the same league's
	// file at once, and a shared ".tmp" would let one truncate the other's
	// half-written copy.
	const std::wstring tmp = dst + L".tmp" + std::to_wstring(GetCurrentProcessId()) + L"_" +
	                         std::to_wstring(GetCurrentThreadId());
	HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD wrote = 0;
	const bool ok = WriteFile(f, body.data(), (DWORD)body.size(), &wrote, nullptr) &&
	                wrote == body.size();
	CloseHandle(f);
	if (!ok) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	if (!MoveFileExW(tmp.c_str(), dst.c_str(),
	                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	return true;
}

void SortByUtc(std::vector<Snapshot>& v)
{
	std::stable_sort(v.begin(), v.end(),
	                 [](const Snapshot& x, const Snapshot& y) { return x.utc < y.utc; });
}

// Shared by the per-league files and the legacy single file (same document).
bool LoadFrom(const std::wstring& path, WarehouseHistory& h)
{
	h.snaps.clear();
	h.sessionStartUtc = 0;
	std::string body;
	if (!ReadAll(path, body)) return false;
	try {
		ordered_json doc = ordered_json::parse(body);
		// A newer schema is a file we cannot promise to preserve on the next
		// Save; leave it alone by loading nothing (Save would then rebuild).
		if (doc.value("schema", 1) > 1) return false;
		h.sessionStartUtc = doc.value("sessionStartUtc", (long long)0);
		auto js = doc.find("snaps");
		if (js != doc.end() && js->is_array()) {
			for (const auto& s : *js) {
				if (!s.is_object()) continue;
				Snapshot snap;
				snap.utc = s.value("utc", (long long)0);
				if (snap.utc <= 0) continue;
				snap.league = s.value("league", std::string());
				auto jt = s.find("tabIds");
				if (jt != s.end() && jt->is_array())
					for (const auto& t : *jt)
						if (t.is_string()) snap.tabIds.push_back(t.get<std::string>());
				snap.totalChaos = s.value("totalChaos", 0.0);
				snap.divineRate = s.value("divineRate", 0.0);
				snap.unpricedKinds = s.value("unpricedKinds", 0);
				snap.summary = s.value("summary", false);
				snap.lineCount = s.value("lineCount", 0);
				auto jl = s.find("lines");
				if (!snap.summary && jl != s.end() && jl->is_array()) {
					for (const auto& l : *jl) {
						if (!l.is_object()) continue;
						SnapshotLine line;
						line.key = l.value("k", std::string());
						if (line.key.empty()) continue;
						line.dispEn = l.value("en", std::string());
						line.icon = l.value("ic", std::string());
						line.count = l.value("n", (long long)0);
						line.chaosEach = l.value("each", 0.0);
						line.chaosTotal = l.value("sum", 0.0);
						line.priced = l.value("p", false);
						line.estimated = l.value("est", false);
						snap.lines.push_back(std::move(line));
					}
				}
				h.snaps.push_back(std::move(snap));
			}
		}
		SortByUtc(h.snaps);
	} catch (const std::exception&) {
		// Corrupt history must not take the tool down; snapshots are convenience
		// data, not the user's work.
		h.snaps.clear();
		h.sessionStartUtc = 0;
		return false;
	}
	return true;
}

std::string Serialize(const WarehouseHistory& h)
{
	ordered_json doc;
	doc["schema"] = 1;
	doc["sessionStartUtc"] = h.sessionStartUtc;
	ordered_json arr = ordered_json::array();
	for (const Snapshot& s : h.snaps) {
		ordered_json o;
		o["utc"] = s.utc;
		o["league"] = s.league;
		o["tabIds"] = s.tabIds;
		o["totalChaos"] = s.totalChaos;
		o["divineRate"] = s.divineRate;
		o["unpricedKinds"] = s.unpricedKinds;
		if (s.summary) {
			o["summary"] = true;
			o["lineCount"] = s.lineCount;
		} else {
			ordered_json lines = ordered_json::array();
			for (const SnapshotLine& l : s.lines) {
				ordered_json jl;
				jl["k"] = l.key;
				jl["en"] = l.dispEn;
				jl["ic"] = l.icon;
				jl["n"] = l.count;
				jl["each"] = l.chaosEach;
				jl["sum"] = l.chaosTotal;
				// Flags only when set: Load defaults both to false.
				if (l.priced) jl["p"] = true;
				if (l.estimated) jl["est"] = true;
				lines.push_back(std::move(jl));
			}
			o["lines"] = std::move(lines);
		}
		arr.push_back(std::move(o));
	}
	doc["snaps"] = std::move(arr);
	// Compact: this file is read by programs only, and indentation was a
	// third of its size.
	return doc.dump();
}

// The rescue copy AppendAndSave falls back to when the file lost everything:
// only this panel's snapshots of THAT league -- *this may hold another one.
WarehouseHistory LeagueSubset(const WarehouseHistory& h, const std::string& league)
{
	WarehouseHistory out;
	for (const Snapshot& s : h.snaps)
		if (s.league == league) out.snaps.push_back(s);
	if (out.FindByUtc(h.sessionStartUtc)) out.sessionStartUtc = h.sessionStartUtc;
	return out;
}

} // namespace

SnapshotDiff DiffSnapshots(const Snapshot& from, const Snapshot& to)
{
	SnapshotDiff d;
	d.dTotalChaos = to.totalChaos - from.totalChaos;
	d.hours = (to.utc - from.utc) / 3600.0;
	d.chaosPerHour = d.hours > 0 ? d.dTotalChaos / d.hours : 0.0;
	if (from.summary || to.summary) {
		d.summaryOnly = true; // no lines to join; the totals above are all there is
		return d;
	}

	// Both line lists are sorted by key; a two-pointer walk joins them.
	size_t a = 0, b = 0;
	std::vector<SnapshotDiffLine> all;
	while (a < from.lines.size() || b < to.lines.size()) {
		const SnapshotLine* fa = a < from.lines.size() ? &from.lines[a] : nullptr;
		const SnapshotLine* tb = b < to.lines.size() ? &to.lines[b] : nullptr;
		int cmp = fa && tb ? fa->key.compare(tb->key) : (fa ? -1 : 1);
		SnapshotDiffLine line;
		if (cmp == 0) {
			line.key = tb->key;
			line.dispEn = tb->dispEn;
			line.icon = tb->icon;
			line.dCount = tb->count - fa->count;
			line.dChaos = tb->chaosTotal - fa->chaosTotal;
			// The count change at today's price; the remainder -- q1 x (p2 - p1)
			// -- is the market re-pricing what was already held. Taken as the
			// remainder so the two parts always add up to dChaos exactly.
			line.dQtyChaos = (double)line.dCount * tb->chaosEach;
			line.dPriceChaos = line.dChaos - line.dQtyChaos;
			line.each = tb->chaosEach;
			line.estimated = tb->estimated;
			a++;
			b++;
		} else if (cmp < 0) {
			// Gone: spent or sold, valued at the last price it had.
			line.key = fa->key;
			line.dispEn = fa->dispEn;
			line.icon = fa->icon;
			line.dCount = -fa->count;
			line.dChaos = -fa->chaosTotal;
			line.dQtyChaos = line.dChaos;
			line.each = fa->chaosEach;
			line.estimated = fa->estimated;
			a++;
		} else {
			line.key = tb->key;
			line.dispEn = tb->dispEn;
			line.icon = tb->icon;
			line.dCount = tb->count;
			line.dChaos = tb->chaosTotal;
			line.dQtyChaos = line.dChaos;
			line.each = tb->chaosEach;
			line.estimated = tb->estimated;
			b++;
		}
		if (line.dCount != 0 || line.dChaos != 0.0) all.push_back(std::move(line));
	}

	for (const SnapshotDiffLine& l : all) {
		if (l.dQtyChaos > 0) d.qtyGain += l.dQtyChaos;
		else d.qtyLoss += l.dQtyChaos;
		d.priceMove += l.dPriceChaos;
	}
	d.farmPerHour = d.hours > 0 ? (d.qtyGain + d.qtyLoss) / d.hours : 0.0;

	// Value first, count as the tie-breaker: an unpriced stack that doubled still
	// deserves a row the player can find.
	std::stable_sort(all.begin(), all.end(),
	                 [](const SnapshotDiffLine& x, const SnapshotDiffLine& y) {
		                 double ax = x.dChaos < 0 ? -x.dChaos : x.dChaos;
		                 double ay = y.dChaos < 0 ? -y.dChaos : y.dChaos;
		                 if (ax != ay) return ax > ay;
		                 long long cx = x.dCount < 0 ? -x.dCount : x.dCount;
		                 long long cy = y.dCount < 0 ? -y.dCount : y.dCount;
		                 return cx > cy;
	                 });
	for (SnapshotDiffLine& l : all) {
		const bool up = l.dChaos > 0 || (l.dChaos == 0 && l.dCount > 0);
		(up ? d.gained : d.lost).push_back(std::move(l));
	}
	return d;
}

std::string WarehouseHistory::LeagueFileStem(const std::string& league)
{
	std::string out;
	bool lossy = false;
	for (unsigned char c : league) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '-') {
			out += (char)c;
		} else if (c == ' ') {
			out += '_'; // reversible: '_' itself is not kept (below)
		} else {
			out += '_';
			lossy = true;
		}
	}
	if (out.empty()) {
		out = "league";
		lossy = true;
	}
	if (out.size() > 60) {
		out.resize(60);
		lossy = true;
	}
	if (lossy) {
		std::uint32_t h = 2166136261u; // FNV-1a of the real name
		for (unsigned char c : league) {
			h ^= c;
			h *= 16777619u;
		}
		char buf[16];
		snprintf(buf, sizeof(buf), "_%08x", (unsigned)h);
		out += buf;
	}
	return out;
}

std::wstring WarehouseHistory::PathOf(const std::wstring& exeDir, const std::string& game,
                                      const std::string& league)
{
	// Only ASCII reaches the name (GameId + LeagueFileStem), so a byte-wise
	// widen is exact.
	const std::string stem = std::string(GameId(game)) + "_" + LeagueFileStem(league);
	return exeDir + L"PobTools\\warehouse\\" + std::wstring(stem.begin(), stem.end()) + L".json";
}

bool WarehouseHistory::Load(const std::wstring& exeDir, const std::string& game,
                            const std::string& league)
{
	return LoadFrom(PathOf(exeDir, game, league), *this);
}

bool WarehouseHistory::Save(const std::wstring& exeDir, const std::string& game,
                            const std::string& league) const
{
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	CreateDirectoryW((exeDir + L"PobTools\\warehouse").c_str(), nullptr);
	return WriteAtomic(PathOf(exeDir, game, league), Serialize(*this));
}

void WarehouseHistory::Prune()
{
	// Hard cap, oldest first -- but never the session start.
	while ((int)snaps.size() > kMaxSnaps) {
		size_t victim = snaps.size(); // sentinel: nothing removable
		for (size_t i = 0; i < snaps.size(); i++) {
			if (snaps[i].utc != sessionStartUtc) {
				victim = i;
				break;
			}
		}
		if (victim >= snaps.size()) break;
		snaps.erase(snaps.begin() + victim);
	}

	// The newest kFullKept keep their lines, and so does the session start (the
	// top card diffs against it); older ones keep only their totals. Snapshots
	// are taken by hand, so this is weeks of play, not hours.
	const size_t fullFrom = snaps.size() > (size_t)kFullKept ? snaps.size() - kFullKept : 0;
	for (size_t i = 0; i < fullFrom; i++) {
		Snapshot& s = snaps[i];
		if (s.summary || s.utc == sessionStartUtc) continue;
		s.lineCount = (int)s.lines.size();
		std::vector<SnapshotLine>().swap(s.lines);
		s.summary = true;
	}
}

const Snapshot* WarehouseHistory::FindByUtc(long long utc) const
{
	for (const Snapshot& s : snaps)
		if (s.utc == utc) return &s;
	return nullptr;
}

bool WarehouseHistory::AppendAndSave(const std::wstring& exeDir, const std::string& game,
                                     const std::string& league, const Snapshot& s)
{
	WarehouseHistory disk;
	disk.Load(exeDir, game, league); // missing or corrupt -> empty, per Load's rule
	// The disk copy is the fresher one (it carries the other panel's shots) --
	// unless it lost everything this panel still holds of that league.
	if (disk.snaps.empty()) disk = LeagueSubset(*this, league);
	if (!disk.FindByUtc(s.utc)) {
		disk.snaps.push_back(s);
		SortByUtc(disk.snaps);
	}
	if (disk.sessionStartUtc == 0) disk.sessionStartUtc = s.utc;
	disk.Prune();
	const bool ok = disk.Save(exeDir, game, league);
	*this = std::move(disk);
	return ok;
}

bool WarehouseHistory::SetSessionStartAndSave(const std::wstring& exeDir, const std::string& game,
                                              const std::string& league, long long utc)
{
	WarehouseHistory disk;
	disk.Load(exeDir, game, league);
	if (disk.snaps.empty()) disk = LeagueSubset(*this, league);
	const Snapshot* target = disk.FindByUtc(utc);
	if (!target || target->summary) {
		*this = std::move(disk); // the other panel pruned it; show what is there
		return false;
	}
	disk.sessionStartUtc = utc;
	const bool ok = disk.Save(exeDir, game, league);
	*this = std::move(disk);
	return ok;
}

int WarehouseHistory::MigrateLegacy(const std::wstring& exeDir, const std::string& game)
{
	const std::wstring legacy = LegacyPath(exeDir, game);
	if (GetFileAttributesW(legacy.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
	WarehouseHistory old;
	// Unreadable (or a newer schema): leave the file alone, never guess.
	if (!LoadFrom(legacy, old)) return -1;

	std::map<std::string, std::vector<const Snapshot*>> byLeague;
	for (const Snapshot& s : old.snaps) byLeague[s.league].push_back(&s);

	int moved = 0;
	for (const auto& kv : byLeague) {
		WarehouseHistory h;
		h.Load(exeDir, game, kv.first); // a per-league file may already exist
		for (const Snapshot* s : kv.second) {
			if (h.FindByUtc(s->utc)) continue;
			h.snaps.push_back(*s);
			moved++;
		}
		SortByUtc(h.snaps);
		// The old file had one session start, and it belongs to one league. The
		// other leagues start at their first snapshot, as a fresh history does.
		if (!h.FindByUtc(h.sessionStartUtc)) {
			h.sessionStartUtc = h.FindByUtc(old.sessionStartUtc) ? old.sessionStartUtc
			                                                      : h.snaps.front().utc;
		}
		h.Prune();
		if (!h.Save(exeDir, game, kv.first)) return -1;
	}
	// Renamed, not deleted: the per-league files are the truth from here on,
	// and the old one stays restorable by hand. A failed rename is retried on
	// the next start; merging again is a no-op (same utc = same snapshot).
	if (!MoveFileExW(legacy.c_str(), (legacy + L".migrated").c_str(), MOVEFILE_REPLACE_EXISTING))
		return -1;
	return moved;
}
