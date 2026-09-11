#include "warehouse_snapshot.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp>

#include <algorithm>

using nlohmann::ordered_json;

namespace {

std::wstring HistoryPath(const std::wstring& exeDir, const std::string& game)
{
	// Only the two known ids ever reach a file name.
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
	const std::wstring tmp = dst + L".tmp";
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

} // namespace

SnapshotDiff DiffSnapshots(const Snapshot& from, const Snapshot& to)
{
	SnapshotDiff d;
	d.dTotalChaos = to.totalChaos - from.totalChaos;
	d.hours = (to.utc - from.utc) / 3600.0;
	d.chaosPerHour = d.hours > 0 ? d.dTotalChaos / d.hours : 0.0;

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
			line.each = tb->chaosEach;
			line.estimated = tb->estimated;
			a++;
			b++;
		} else if (cmp < 0) {
			line.key = fa->key;
			line.dispEn = fa->dispEn;
			line.icon = fa->icon;
			line.dCount = -fa->count;
			line.dChaos = -fa->chaosTotal;
			line.each = fa->chaosEach;
			line.estimated = fa->estimated;
			a++;
		} else {
			line.key = tb->key;
			line.dispEn = tb->dispEn;
			line.icon = tb->icon;
			line.dCount = tb->count;
			line.dChaos = tb->chaosTotal;
			line.each = tb->chaosEach;
			line.estimated = tb->estimated;
			b++;
		}
		if (line.dCount != 0 || line.dChaos != 0.0) all.push_back(std::move(line));
	}

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

bool WarehouseHistory::Load(const std::wstring& exeDir, const std::string& game)
{
	snaps.clear();
	sessionStartUtc = 0;
	std::string body;
	if (!ReadAll(HistoryPath(exeDir, game), body)) return false;
	try {
		ordered_json doc = ordered_json::parse(body);
		// A newer schema is a file we cannot promise to preserve on the next
		// Save; leave it alone by loading nothing (Save would then rebuild).
		if (doc.value("schema", 1) > 1) return false;
		sessionStartUtc = doc.value("sessionStartUtc", (long long)0);
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
				auto jl = s.find("lines");
				if (jl != s.end() && jl->is_array()) {
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
				snaps.push_back(std::move(snap));
			}
		}
		std::stable_sort(snaps.begin(), snaps.end(),
		                 [](const Snapshot& x, const Snapshot& y) { return x.utc < y.utc; });
	} catch (const std::exception&) {
		// Corrupt history must not take the tool down; snapshots are convenience
		// data, not the user's work.
		snaps.clear();
		sessionStartUtc = 0;
		return false;
	}
	return true;
}

bool WarehouseHistory::Save(const std::wstring& exeDir, const std::string& game) const
{
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);

	ordered_json doc;
	doc["schema"] = 1;
	doc["sessionStartUtc"] = sessionStartUtc;
	ordered_json arr = ordered_json::array();
	for (const Snapshot& s : snaps) {
		ordered_json o;
		o["utc"] = s.utc;
		o["league"] = s.league;
		o["tabIds"] = s.tabIds;
		o["totalChaos"] = s.totalChaos;
		o["divineRate"] = s.divineRate;
		o["unpricedKinds"] = s.unpricedKinds;
		ordered_json lines = ordered_json::array();
		for (const SnapshotLine& l : s.lines) {
			ordered_json jl;
			jl["k"] = l.key;
			jl["en"] = l.dispEn;
			jl["ic"] = l.icon;
			jl["n"] = l.count;
			jl["each"] = l.chaosEach;
			jl["sum"] = l.chaosTotal;
			jl["p"] = l.priced;
			jl["est"] = l.estimated;
			lines.push_back(std::move(jl));
		}
		o["lines"] = std::move(lines);
		arr.push_back(std::move(o));
	}
	doc["snaps"] = std::move(arr);
	return WriteAtomic(HistoryPath(exeDir, game), doc.dump(1, '\t'));
}

void WarehouseHistory::Prune(long long nowUtc)
{
	if (snaps.empty()) return;

	// Beyond 48h: keep the last snapshot of each hour. The curve back there is
	// read at a glance, not zoomed into.
	const long long cutoff = nowUtc - 48 * 3600;
	std::vector<Snapshot> kept;
	kept.reserve(snaps.size());
	for (size_t i = 0; i < snaps.size(); i++) {
		const Snapshot& s = snaps[i];
		if (s.utc >= cutoff || s.utc == sessionStartUtc) {
			kept.push_back(s);
			continue;
		}
		const bool lastOfHour =
		    i + 1 >= snaps.size() || snaps[i + 1].utc / 3600 != s.utc / 3600;
		if (lastOfHour) kept.push_back(s);
	}
	snaps.swap(kept);

	// Hard cap, oldest first -- but never the session start.
	while (snaps.size() > 200) {
		size_t victim = snaps.size(); // sentinel: nothing removable
		for (size_t i = 0; i < snaps.size(); i++) {
			if (snaps[i].utc != sessionStartUtc) { victim = i; break; }
		}
		if (victim >= snaps.size()) break;
		snaps.erase(snaps.begin() + victim);
	}
}

const Snapshot* WarehouseHistory::FindByUtc(long long utc) const
{
	for (const Snapshot& s : snaps)
		if (s.utc == utc) return &s;
	return nullptr;
}
