#include "data_update_selftest.h"
#include "atlas_update.h"
#include "passive_tree_update.h"
#include "http_client.h"
#include "error_log.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include "win_util.h"
#include "selftest_report.h"

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#pragma comment(lib, "shell32.lib")

#include <atomic>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

using nlohmann::ordered_json;

// Both updaters share one phase order, so one test body can drive either through IUpd.
static_assert((int)AtlasUpdatePhase::Error == (int)PassiveUpdatePhase::Error &&
              (int)AtlasUpdatePhase::Done == (int)PassiveUpdatePhase::Done &&
              (int)AtlasUpdatePhase::UpdateAvailable == (int)PassiveUpdatePhase::UpdateAvailable &&
              (int)AtlasUpdatePhase::UpToDate == (int)PassiveUpdatePhase::UpToDate &&
              (int)AtlasUpdatePhase::Checking == (int)PassiveUpdatePhase::Checking,
              "phase enums diverged: update IUpd mapping");

namespace {

// ---- sandbox files ---------------------------------------------------------------------------------------------

void rmtree(const std::wstring& dir) // dir without trailing backslash; only ever our own %TEMP% sandboxes
{
	if (dir.find(L"pobtools_dataupd_") == std::wstring::npos) return;
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			std::wstring n = fd.cFileName;
			if (n == L"." || n == L"..") continue;
			std::wstring p = dir + L"\\" + n;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) rmtree(p);
			else { SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL); DeleteFileW(p.c_str()); }
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

std::wstring make_sandbox_dir()
{
	static std::atomic<int> seq{ 0 };
	wchar_t t[MAX_PATH] = {};
	GetTempPathW(MAX_PATH, t);
	std::wstring d = std::wstring(t) + L"pobtools_dataupd_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
	                 std::to_wstring(++seq) + L"\\";
	rmtree(d.substr(0, d.size() - 1));
	SHCreateDirectoryExW(nullptr, d.c_str(), nullptr);
	return d;
}

// Fixed old mtime: a later rewrite of the file (even with identical bytes) shows up as a changed mtime.
void set_old_mtime(const std::wstring& path)
{
	HANDLE h = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
	                       OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	SYSTEMTIME st{};
	st.wYear = 2020; st.wMonth = 1; st.wDay = 1;
	FILETIME ft{};
	SystemTimeToFileTime(&st, &ft);
	SetFileTime(h, nullptr, nullptr, &ft);
	CloseHandle(h);
}

void put(const std::wstring& path, const std::string& content)
{
	size_t s = path.find_last_of(L'\\');
	SHCreateDirectoryExW(nullptr, path.substr(0, s).c_str(), nullptr);
	write_file_utf8(path, content);
	set_old_mtime(path);
}

struct Snap {
	bool exists = false;
	std::string bytes;
	unsigned long long mtime = 0;
	bool operator==(const Snap& o) const { return exists == o.exists && bytes == o.bytes && mtime == o.mtime; }
};

Snap snap(const std::wstring& p)
{
	Snap s;
	s.exists = file_exists(p);
	if (!s.exists) return s;
	read_file_utf8(p, s.bytes);
	WIN32_FILE_ATTRIBUTE_DATA a{};
	if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a))
		s.mtime = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
	return s;
}

std::string slurp(const std::wstring& p)
{
	std::string s;
	read_file_utf8(p, s);
	return s;
}

bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }

std::string replace_all(std::string s, const std::string& from, const std::string& to)
{
	for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
	return s;
}

// ---- fake network ---------------------------------------------------------------------------------------------

struct FakeNet {
	std::mutex mx;
	std::vector<std::string> calls; // "host/path", in arrival order
	int unexpected = 0;             // requests outside the known set

	std::string tagsPath, rawPrefix; // the exact request shapes the updater is expected to use
	std::string tagsBody;
	bool tagsFail = false;
	std::string repoeVer = "r1";
	std::string dataBody;
	bool dataFail = false, dataHang = false;
	std::map<std::string, std::string> assets; // base name -> bytes
	std::set<std::string> assetFail;
	std::string tcAtlas;
	std::atomic<bool> hanging{ false };

	std::vector<std::string> allCalls() { std::lock_guard<std::mutex> lk(mx); return calls; }
	size_t count() { std::lock_guard<std::mutex> lk(mx); return calls.size(); }
	int unexpectedCount() { std::lock_guard<std::mutex> lk(mx); return unexpected; }

	bool handle(const std::wstring& host, const std::wstring& path, std::vector<unsigned char>& out,
	            std::string* err, const std::atomic<bool>* cancel)
	{
		const std::string h = narrow(host), p = narrow(path);
		{
			std::lock_guard<std::mutex> lk(mx);
			calls.push_back(h + p);
		}
		auto ok = [&](const std::string& s) { out.assign(s.begin(), s.end()); return true; };
		auto no = [&](const char* m, bool unexp) {
			if (unexp) { std::lock_guard<std::mutex> lk(mx); unexpected++; }
			if (err) *err = m;
			return false;
		};
		auto endsWith = [&](const std::string& suf) {
			return p.size() >= suf.size() && p.compare(p.size() - suf.size(), suf.size(), suf) == 0;
		};

		if (h == "api.github.com") {
			if (p != tagsPath) return no("unexpected api path", true);
			if (tagsFail) return no("fake: tags unreachable", false);
			return ok(tagsBody);
		}
		if (h == "repoe-fork.github.io") {
			if (p == "/version.txt") return ok(repoeVer + "\n");
			if (p == "/Traditional%20Chinese/passive_skill_trees/Atlas.json") return ok(tcAtlas);
			return no("unexpected repoe path", true);
		}
		if (h == "raw.githubusercontent.com") {
			if (p.compare(0, rawPrefix.size(), rawPrefix) != 0) return no("unexpected raw path", true);
			if (endsWith("/data.json")) {
				if (dataHang) {
					hanging = true;
					const ULONGLONG t0 = GetTickCount64();
					while (!(cancel && cancel->load()) && GetTickCount64() - t0 < 20000) Sleep(5);
					return no("fake: cancelled", false);
				}
				if (dataFail) return no("fake: data.json download failed", false);
				return ok(dataBody);
			}
			size_t a = p.find("/assets/");
			if (a != std::string::npos) {
				const std::string base = p.substr(a + 8);
				if (assetFail.count(base)) return no("fake: asset download failed", false);
				auto it = assets.find(base);
				if (it == assets.end()) return no("unknown asset", true);
				return ok(it->second);
			}
		}
		return no("unexpected host", true);
	}
};

// calls whose path ends with /data.json
int data_calls(const std::vector<std::string>& calls)
{
	int n = 0;
	for (const auto& c : calls)
		if (c.size() >= 10 && c.compare(c.size() - 10, 10, "/data.json") == 0) n++;
	return n;
}

std::set<std::string> asset_calls(const std::vector<std::string>& calls)
{
	std::set<std::string> s;
	for (const auto& c : calls) {
		size_t a = c.find("/assets/");
		if (a != std::string::npos) s.insert(c.substr(a + 8));
	}
	return s;
}

bool has_call(const std::vector<std::string>& calls, const std::string& exact)
{
	for (const auto& c : calls) if (c == exact) return true;
	return false;
}

std::string join(const std::set<std::string>& s)
{
	std::string o;
	for (const auto& x : s) o += (o.empty() ? "" : ",") + x;
	return o;
}

// ---- synthetic data.json ---------------------------------------------------------------------------------------

std::string coords_obj(const std::vector<std::string>& keys, int sz)
{
	std::string o = "{";
	for (size_t i = 0; i < keys.size(); i++)
		o += (i ? "," : "") + std::string("\"") + keys[i] + "\":{\"x\":0,\"y\":0,\"w\":" + std::to_string(sz) +
		     ",\"h\":" + std::to_string(sz) + "}";
	return o + "}";
}

std::string sprite(const char* zoom, const std::string& file, int dim, const std::string& coords)
{
	return std::string("{\"") + zoom + "\":{\"filename\":\"https://cdn.example/x/" + file + "?v1\",\"w\":" +
	       std::to_string(dim) + ",\"h\":" + std::to_string(dim) + ",\"coords\":" + coords + "}}";
}

// Smallest atlastree-export that ImportAtlasTreeData accepts: a start node + one normal node, 4 sheets
// (startNode/normalActive share sheetA), plus a category with no 0.5 level that must never be downloaded.
std::string atlas_data_good()
{
	const std::string frame = coords_obj({ "PSSkillFrame", "PSSkillFrameHighlighted", "PSSkillFrameActive",
		"NotableFrameUnallocated", "NotableFrameCanAllocate", "NotableFrameAllocated",
		"KeystoneFrameUnallocated", "KeystoneFrameCanAllocate", "KeystoneFrameAllocated",
		"WormholeFrameUnallocated", "WormholeFrameCanAllocate", "WormholeFrameAllocated" }, 8);
	return std::string(R"({"tree":"T329","min_x":-100,"min_y":-100,"max_x":100,"max_y":100,)") +
	       R"("points":{"totalPoints":123},)" +
	       R"("constants":{"orbitRadii":[0,82],"skillsPerOrbit":[1,6]},)" +
	       R"("groups":{"1":{"x":0,"y":0}},)" +
	       R"("nodes":{"root":{"out":["1"]},)" +
	       R"("1":{"skill":1,"name":"Start","group":1,"orbit":0,"orbitIndex":0,"out":["2"]},)" +
	       R"("2":{"skill":2,"name":"Node Two","group":1,"orbit":1,"orbitIndex":0,"icon":"ic","in":["1"],"stats":["+1 to test"]}},)" +
	       "\"sprites\":{" +
	       "\"startNode\":" + sprite("0.5", "sheetA.png", 64, coords_obj({ "AtlasPassiveSkillScreenStart" }, 16)) + "," +
	       "\"normalActive\":" + sprite("0.5", "sheetA.png", 64, coords_obj({ "ic" }, 16)) + "," +
	       "\"normalInactive\":" + sprite("0.5", "sheetB.png", 64, coords_obj({ "ic" }, 16)) + "," +
	       "\"frame\":" + sprite("0.5", "sheetF.png", 64, frame) + "," +
	       "\"atlasBackground\":" + sprite("0.5", "bg.png", 1024, coords_obj({ "AtlasPassiveBackground" }, 512)) + "," +
	       "\"unusedCat\":" + sprite("1", "unused.png", 8, "{}") + "}}";
}

const char* kAtlasTc = R"({"passives":{"1":{"name":"起點","stat_text":[]},"2":{"name":"測試節點","stat_text":["測試 +1"]}}})";

// Smallest skilltree-export ImportPassiveTreeData accepts: two connected normal nodes, 4 used sheets, plus a
// category outside the updater's used list that must never be downloaded.
std::string pt_data_good()
{
	const std::string frame = coords_obj({ "PSSkillFrame", "PSSkillFrameHighlighted", "PSSkillFrameActive",
		"NotableFrameUnallocated", "NotableFrameCanAllocate", "NotableFrameAllocated",
		"KeystoneFrameUnallocated", "KeystoneFrameCanAllocate", "KeystoneFrameAllocated" }, 8);
	return std::string(R"({"tree":"Default","min_x":-100,"min_y":-100,"max_x":100,"max_y":100,)") +
	       R"("classes":[{"name":"Scion"}],)" +
	       R"("constants":{"orbitRadii":[0,82],"skillsPerOrbit":[1,6]},)" +
	       R"("groups":{"1":{"x":0,"y":0}},)" +
	       R"("nodes":{"root":{"out":[]},)" +
	       R"("10":{"skill":10,"name":"Node A","group":1,"orbit":1,"orbitIndex":0,"icon":"ic","out":["11"]},)" +
	       R"("11":{"skill":11,"name":"Node B","group":1,"orbit":1,"orbitIndex":1,"icon":"ic","in":["10"],"stats":["+1 to test"]}},)" +
	       "\"sprites\":{" +
	       "\"normalActive\":" + sprite("0.3835", "a-3.png", 64, coords_obj({ "ic" }, 16)) + "," +
	       "\"normalInactive\":" + sprite("0.3835", "b-3.png", 64, coords_obj({ "ic" }, 16)) + "," +
	       "\"frame\":" + sprite("0.3835", "f-3.png", 64, frame) + "," +
	       "\"groupBackground\":" + sprite("0.3835", "g-3.png", 64, coords_obj({ "PSGroupBackground1" }, 32)) + "," +
	       "\"ascendancyBackground\":" + sprite("0.3835", "unused-3.png", 8, "{}") + "}}";
}

// valid JSON with a sprites section (so the updater downloads everything) but missing the key the importer requires
std::string without_key(const std::string& good, const char* key)
{
	ordered_json d = ordered_json::parse(good);
	d.erase(key);
	return d.dump();
}

// ---- updater driver (one test body for both) ----------------------------------------------------------------

enum class Ph { Idle, Checking, UpToDate, UpdateAvailable, Downloading, Importing, Done, Error };

struct View {
	Ph phase = Ph::Idle;
	std::string latest, message;
	bool reload = false;
};

const char* ph_name(Ph p)
{
	static const char* n[] = { "Idle", "Checking", "UpToDate", "UpdateAvailable", "Downloading", "Importing", "Done", "Error" };
	return n[(int)p];
}

struct IUpd {
	virtual ~IUpd() {}
	virtual void init(const std::wstring& dir) = 0;
	virtual void shutdown() = 0;
	virtual void check(bool force) = 0;
	virtual void update() = 0;
	virtual View poll() = 0;
	virtual void ack() = 0;
};

struct AtlasDrv : IUpd {
	AtlasUpdater u;
	void init(const std::wstring& d) override { u.Init(d); }
	void shutdown() override { u.Shutdown(); }
	void check(bool f) override { u.RequestCheck(f); }
	void update() override { u.StartUpdate(); }
	View poll() override
	{
		auto s = u.Poll();
		return { (Ph)(int)s.phase, s.latestTag, s.message, s.reloadPending };
	}
	void ack() override { u.AckReload(); }
};

struct PtDrv : IUpd {
	PassiveTreeUpdater u;
	void init(const std::wstring& d) override { u.Init(d); }
	void shutdown() override { u.Shutdown(); }
	void check(bool f) override { u.RequestCheck(f); }
	void update() override { u.StartUpdate(); }
	View poll() override
	{
		auto s = u.Poll();
		return { (Ph)(int)s.phase, s.latestVer, s.message, s.reloadPending };
	}
	void ack() override { u.AckReload(); }
};

// ---- flavors: everything that differs between the atlas and the passive-tree updater ---------------------------

struct Flavor {
	const char* name;
	int baseId;     // first test number (4 for atlas, 10 for passive tree)
	bool atlas;
	std::string curTag, newTag, curLatest, newLatest; // curTag/newTag = GitHub tags; *Latest = Status::latest form
	std::string newSha;
	std::string tagsPath, rawPrefix;
	std::string dataGood;
	std::wstring treeFile;                     // an existing tree file an up-to-date check must not rewrite
	std::vector<std::wstring> guard;           // files that a failed update must leave byte-identical (record last)
	std::vector<std::wstring> absent;          // files a failed update must never create
	std::set<std::string> neededAssets;        // distinct sheets the good data.json needs
	const char* breakKey;                      // key whose removal passes the updater's own checks but fails the importer
	void (*seed)(const std::wstring& dir);
};

void seed_atlas(const std::wstring& d)
{
	put(d + L"Data\\atlas_versions\\3.28.0\\atlas_tree_poe1.json", "OLD-ATLAS-TREE-3.28");
	put(d + L"Data\\atlas_versions\\3.28.0\\atlas_tree_zh.json", "OLD-ATLAS-ZH-3.28");
	put(d + L"Data\\atlas_versions\\3.28.0\\atlas\\old.png", "OLD-PNG");
	put(d + L"Data\\atlas_index.json",
	    R"({"format":"pobtools-atlas-index","active":"3.28.0","compareBase":"","versions":[{"tag":"3.28.0","sha":"oldsha","repoe":"r1"}],"lastCheckUtc":0})");
	put(d + L"Data\\atlas_version.json", R"({"tag":"3.28.0","sha":"oldsha","repoe":"r1","lastCheckUtc":0})");
}

void seed_pt(const std::wstring& d)
{
	put(d + L"Data\\passive_tree_poe1.json", R"({"version":"3_28/9n","treeVersion":"3_28","marker":"OLD-PT"})");
	put(d + L"Data\\passive_update.json", R"({"lastCheckUtc":0})");
}

const Flavor& atlas_flavor()
{
	static Flavor f = [] {
		Flavor x;
		x.name = u8"輿圖"; x.baseId = 4; x.atlas = true;
		x.curTag = "3.28.0"; x.newTag = "3.29.0"; x.curLatest = x.curTag; x.newLatest = x.newTag;
		x.newSha = "newsha";
		x.tagsPath = "/repos/grindinggear/atlastree-export/tags?per_page=100";
		x.rawPrefix = "/grindinggear/atlastree-export/";
		x.dataGood = atlas_data_good();
		x.treeFile = L"Data\\atlas_versions\\3.28.0\\atlas_tree_poe1.json";
		x.guard = { L"Data\\atlas_versions\\3.28.0\\atlas_tree_poe1.json", L"Data\\atlas_versions\\3.28.0\\atlas_tree_zh.json",
		            L"Data\\atlas_versions\\3.28.0\\atlas\\old.png", L"Data\\atlas_index.json", L"Data\\atlas_version.json" };
		x.absent = { L"Data\\atlas_versions\\3.29.0\\atlas_tree_poe1.json", L"Data\\atlas_versions\\3.29.0\\atlas_tree_zh.json" };
		x.neededAssets = { "bg.png", "sheetA.png", "sheetB.png", "sheetF.png" };
		x.breakKey = "nodes";
		x.seed = seed_atlas;
		return x;
	}();
	return f;
}

const Flavor& pt_flavor()
{
	static Flavor f = [] {
		Flavor x;
		x.name = u8"天賦樹"; x.baseId = 10; x.atlas = false;
		x.curTag = "3.28.0"; x.newTag = "3.29.0"; x.curLatest = "3_28"; x.newLatest = "3_29";
		x.newSha = "";
		x.tagsPath = "/repos/grindinggear/skilltree-export/tags?per_page=100";
		x.rawPrefix = "/grindinggear/skilltree-export/";
		x.dataGood = pt_data_good();
		x.treeFile = L"Data\\passive_tree_poe1.json";
		x.guard = { L"Data\\passive_tree_poe1.json", L"Data\\passive_update.json" };
		x.absent = { L"Data\\tree\\a-3.png", L"Data\\tree\\b-3.png" };
		x.neededAssets = { "a-3.png", "b-3.png", "f-3.png", "g-3.png" };
		x.breakKey = "classes";
		x.seed = seed_pt;
		return x;
	}();
	return f;
}

// ---- one scenario's world: sandbox + fake net + running updater -----------------------------------------------

struct Opt {
	bool newer = true;    // remote has a newer tag than the installed data
	bool tagsFail = false;
	bool hang = false;    // data.json download blocks until cancelled
	std::string pobVer;   // non-empty: a local PoB install with an (empty-ish) TreeData\<pobVer>\a-3.png
};

struct Ctx {
	const Flavor& fl;
	std::wstring dir;
	FakeNet net;
	std::unique_ptr<IUpd> u;

	Ctx(const Flavor& f, const Opt& o) : fl(f)
	{
		dir = make_sandbox_dir();
		fl.seed(dir);
		if (!o.pobVer.empty()) {
			const std::wstring pob = dir + L"PathOfBuildingCommunity\\";
			put(pob + L"Launch.lua", "-- fake PoB");
			put(pob + L"TreeData\\" + widen(o.pobVer) + L"\\a-3.png", "POB-A3-SHEET");
		}
		net.tagsPath = fl.tagsPath;
		net.rawPrefix = fl.rawPrefix;
		net.tagsBody = o.newer
			? R"([{"name":"3.30.0-ruthless","commit":{"sha":"ruthsha"}},{"name":"3.29.0","commit":{"sha":"newsha"}},{"name":"3.28.0","commit":{"sha":"oldsha"}}])"
			: R"([{"name":"3.30.0-ruthless","commit":{"sha":"ruthsha"}},{"name":"3.28.0","commit":{"sha":"oldsha"}},{"name":"3.27.0","commit":{"sha":"olds"}}])";
		net.tagsFail = o.tagsFail;
		net.repoeVer = o.newer ? "r2" : "r1";
		net.dataBody = fl.dataGood;
		net.dataHang = o.hang;
		net.tcAtlas = kAtlasTc;
		for (const std::string& a : fl.neededAssets) net.assets[a] = "ASSET:" + a;
		PobLog::SetDirForTest(dir + L"PobTools\\logs\\");
		HttpSetTestSource([this](const std::wstring& h, const std::wstring& p, std::vector<unsigned char>& out,
		                         std::string* err, const std::atomic<bool>* c) { return net.handle(h, p, out, err, c); });
		if (fl.atlas) u.reset(new AtlasDrv); else u.reset(new PtDrv);
		u->init(dir);
	}
	~Ctx()
	{
		u->shutdown();
		HttpSetTestSource(nullptr);
		PobLog::SetDirForTest(L"");
		rmtree(dir.substr(0, dir.size() - 1));
	}

	// Queue a check and wait until the worker has issued a request AND left Checking. ok=false on timeout
	// (e.g. the check was throttled away and never reached the network).
	View check(bool force, bool* ok, int ms = 30000)
	{
		const size_t prev = net.count();
		u->check(force);
		const ULONGLONG t0 = GetTickCount64();
		while (GetTickCount64() - t0 < (ULONGLONG)ms) {
			if (net.count() > prev) {
				View v = u->poll();
				if (v.phase != Ph::Checking) { *ok = true; return v; }
			}
			Sleep(10);
		}
		*ok = false;
		return u->poll();
	}

	// Queue a check and report how many requests it caused after a settle time (for "must not connect").
	size_t check_settle(bool force)
	{
		const size_t prev = net.count();
		u->check(force);
		Sleep(600);
		return net.count() - prev;
	}

	View wait_update(bool* ok, int ms = 30000)
	{
		const ULONGLONG t0 = GetTickCount64();
		while (GetTickCount64() - t0 < (ULONGLONG)ms) {
			View v = u->poll();
			if (v.phase == Ph::Done || v.phase == Ph::Error) { *ok = true; return v; }
			Sleep(10);
		}
		*ok = false;
		return u->poll();
	}

	std::vector<std::pair<std::wstring, Snap>> snap_guard()
	{
		std::vector<std::pair<std::wstring, Snap>> v;
		for (const auto& r : fl.guard) v.push_back({ r, snap(dir + r) });
		return v;
	}
	// names of guarded files whose bytes/mtime/existence changed since `before`
	std::string changed_since(const std::vector<std::pair<std::wstring, Snap>>& before)
	{
		std::string o;
		for (const auto& b : before)
			if (!(snap(dir + b.first) == b.second)) o += (o.empty() ? "" : ",") + narrow(b.first);
		return o;
	}
	std::string created_absent()
	{
		std::string o;
		for (const auto& r : fl.absent)
			if (file_exists(dir + r)) o += (o.empty() ? "" : ",") + narrow(r);
		return o;
	}
};

struct Rep {
	TestReport& r;
	const Flavor& fl;
	void chk(int off, bool ok, const std::string& what, const std::string& detail = std::string())
	{
		chk_id("T" + std::to_string(fl.baseId + off), ok, what, detail);
	}
	void chk_id(const std::string& id, bool ok, const std::string& what, const std::string& detail = std::string())
	{
		r.check(ok, (std::string(fl.name) + " " + id + "：" + what).c_str(), detail);
	}
};

std::string vdesc(const View& v) { return std::string("phase=") + ph_name(v.phase) + " latest=" + v.latest + " msg=" + v.message; }

ordered_json parse_file(const std::wstring& p)
{
	return ordered_json::parse(slurp(p), nullptr, false);
}

// ---- T4 / T10: newer tag -> UpdateAvailable -> update -> Done ----------------------------------------------------

void verify_atlas_installed(Ctx& c, Rep& R)
{
	const std::wstring& d = c.dir;
	ordered_json rec = parse_file(d + L"Data\\atlas_version.json");
	R.chk(0, rec.is_object() && rec.value("tag", "") == "3.29.0" && rec.value("sha", "") == "newsha" &&
	             rec.value("repoe", "") == "r2" && rec.value("lastCheckUtc", 0ll) > 0,
	      u8"版本紀錄 atlas_version.json 變成新版（tag/sha/repoe/檢查時間）", slurp(d + L"Data\\atlas_version.json"));

	ordered_json idx = parse_file(d + L"Data\\atlas_index.json");
	bool hasOld = false, hasNew = false;
	if (idx.is_object() && idx.contains("versions") && idx["versions"].is_array())
		for (const auto& v : idx["versions"]) {
			if (v.value("tag", "") == "3.28.0") hasOld = true;
			if (v.value("tag", "") == "3.29.0") hasNew = true;
		}
	R.chk(0, idx.is_object() && idx.value("active", "") == "3.29.0" && hasOld && hasNew,
	      u8"atlas_index.json 啟用新賽季並保留上一季", slurp(d + L"Data\\atlas_index.json"));

	const std::wstring nd = d + L"Data\\atlas_versions\\3.29.0\\";
	R.chk(0, contains(slurp(nd + L"atlas_tree_poe1.json"), "\"T329/2n/123p\""),
	      u8"新賽季樹檔是新版內容（version 字串來自新 data.json）");
	R.chk(0, contains(slurp(nd + L"atlas_tree_zh.json"), u8"測試節點"), u8"新賽季繁中對照 atlas_tree_zh.json 已產生");
	bool sheetsOk = true;
	for (const std::string& a : c.fl.neededAssets)
		if (slurp(nd + L"atlas\\" + widen(a)) != "ASSET:" + a) sheetsOk = false;
	R.chk(0, sheetsOk, u8"4 張圖集都複製進新賽季 atlas\\（內容與下載相同）");
	R.chk(0, !dir_exists(d + L"PobTools\\cache\\atlas_update\\3.29.0\\assets"), u8"成功後清掉下載快取");
}

void verify_pt_installed(Ctx& c, Rep& R)
{
	const std::wstring& d = c.dir;
	const std::string tree = slurp(d + L"Data\\passive_tree_poe1.json");
	R.chk(0, contains(tree, "\"treeVersion\":\"3_29\"") && contains(tree, "\"3_29/2n\"") && !contains(tree, "OLD-PT"),
	      u8"樹檔 passive_tree_poe1.json 是新版內容（treeVersion 3_29、2 個節點）", tree.substr(0, 120));
	bool sheetsOk = true;
	for (const std::string& a : c.fl.neededAssets)
		if (slurp(d + L"Data\\tree\\" + widen(a)) != "ASSET:" + a) sheetsOk = false;
	R.chk(0, sheetsOk, u8"4 張圖集都複製進 Data\\tree\\（內容與下載相同）");
	ordered_json rec = parse_file(d + L"Data\\passive_update.json");
	R.chk(0, rec.is_object() && rec.value("lastCheckUtc", 0ll) > 0, u8"節流紀錄 passive_update.json 寫入檢查時間",
	      slurp(d + L"Data\\passive_update.json"));
	R.chk(0, !dir_exists(d + L"PobTools\\cache\\pt_update\\3.29.0\\assets"), u8"成功後清掉下載快取");
}

void test_update(Rep& R)
{
	const Flavor& fl = R.fl;
	Ctx c(fl, Opt{});
	bool ok = false;
	View v = c.check(true, &ok);
	R.chk(0, ok && v.phase == Ph::UpdateAvailable && v.latest == fl.newLatest,
	      u8"強制檢查發現較新版本（並略過 -ruthless 變體 tag）→ UpdateAvailable", vdesc(v));
	if (!ok || v.phase != Ph::UpdateAvailable) return;

	std::vector<std::pair<std::wstring, Snap>> before = c.snap_guard();
	c.u->update();
	View d = c.wait_update(&ok);
	R.chk(0, ok && d.phase == Ph::Done, u8"StartUpdate 後 30 秒內到 Done", vdesc(d));
	R.chk(0, d.reload, u8"Done 時 reloadPending 為真");
	if (!ok || d.phase != Ph::Done) return;

	if (fl.atlas) {
		verify_atlas_installed(c, R);
		// the previous season must survive side by side (compare view)
		std::string chg = c.changed_since(std::vector<std::pair<std::wstring, Snap>>(before.begin(), before.begin() + 3));
		R.chk(0, chg.empty(), u8"上一季資料夾（樹檔/繁中/圖集）原封不動", chg);
	} else {
		verify_pt_installed(c, R);
	}

	std::vector<std::string> calls = c.net.allCalls();
	R.chk(0, data_calls(calls) == 1 && has_call(calls, "raw.githubusercontent.com" + fl.rawPrefix + fl.newTag + "/data.json"),
	      u8"只下載一次新 tag 的 data.json（路徑正確）");
	R.chk(0, asset_calls(calls) == fl.neededAssets, u8"只下載資料實際引用的圖集（不多不少、共用的只抓一次）",
	      "got=" + join(asset_calls(calls)));
	R.chk(0, c.net.unexpectedCount() == 0, u8"沒有預期外的主機/路徑請求");

	c.u->ack();
	View a = c.u->poll();
	R.chk(0, !a.reload && a.phase == Ph::Idle, u8"AckReload 後 reloadPending 清除、回到 Idle", vdesc(a));
}

// ---- T5 / T11: same tag -> UpToDate, nothing downloaded or rewritten -------------------------------------------

void test_uptodate(Rep& R)
{
	const Flavor& fl = R.fl;
	Ctx c(fl, Opt{ false });
	Snap before = snap(c.dir + fl.treeFile);
	bool ok = false;
	View v = c.check(true, &ok);
	R.chk(1, ok && v.phase == Ph::UpToDate, u8"tag 與目前相同，強制檢查 → UpToDate", vdesc(v));
	std::vector<std::string> calls = c.net.allCalls();
	R.chk(1, data_calls(calls) == 0 && asset_calls(calls).empty(), u8"沒有下載 data.json 或圖集");
	Snap after = snap(c.dir + fl.treeFile);
	R.chk(1, after == before, u8"既有樹檔內容與修改時間不變");
	if (fl.atlas) {
		ordered_json idx = parse_file(c.dir + L"Data\\atlas_index.json");
		R.chk(1, idx.is_object() && idx.value("active", "") == "3.28.0" && !dir_exists(c.dir + L"Data\\atlas_versions\\3.29.0"),
		      u8"沒有建出新賽季資料夾、index 仍啟用舊季");
	}
}

// ---- T6 / T12 and T7 / T13: update fails -> Error, nothing changed ---------------------------------------------

void fail_case(Rep& R, int off, const std::string& label, const std::function<void(FakeNet&)>& setup)
{
	const Flavor& fl = R.fl;
	Ctx c(fl, Opt{});
	setup(c.net);
	bool ok = false;
	View v = c.check(true, &ok);
	if (!ok || v.phase != Ph::UpdateAvailable) {
		R.chk(off, false, label + u8"：前置檢查未進入 UpdateAvailable", vdesc(v));
		return;
	}
	std::vector<std::pair<std::wstring, Snap>> before = c.snap_guard(); // taken after the check (which rewrites the record)
	c.u->update();
	View e = c.wait_update(&ok);
	R.chk(off, ok && e.phase == Ph::Error && !e.message.empty() && !e.reload,
	      label + u8"：狀態 Error、有錯誤訊息、不要求重載", vdesc(e));
	std::string chg = c.changed_since(before);
	std::string made = c.created_absent();
	R.chk(off, chg.empty() && made.empty(), label + u8"：既有樹檔與版本紀錄不變、沒裝出半套新版",
	      "changed=" + chg + " created=" + made);
}

void test_bad_data(Rep& R)
{
	const Flavor& fl = R.fl;
	fail_case(R, 2, u8"data.json 不是 JSON", [](FakeNet& n) { n.dataBody = "<html>rate limited</html>"; });
	fail_case(R, 2, u8"data.json 缺 sprites 段", [](FakeNet& n) { n.dataBody = R"({"nodes":{},"groups":{}})"; });
	const std::string broken = without_key(fl.dataGood, fl.breakKey);
	fail_case(R, 2, std::string(u8"data.json 缺必要欄位 ") + fl.breakKey + u8"（匯入驗證擋下）",
	          [broken](FakeNet& n) { n.dataBody = broken; });
}

void test_net_fail(Rep& R)
{
	const Flavor& fl = R.fl;
	fail_case(R, 3, u8"下載 data.json 失敗", [](FakeNet& n) { n.dataFail = true; });
	const std::string lastAsset = *fl.neededAssets.rbegin();
	fail_case(R, 3, u8"下載圖集 " + lastAsset + u8" 失敗", [lastAsset](FakeNet& n) { n.assetFail.insert(lastAsset); });
}

// ---- T8 / T14: throttle ----------------------------------------------------------------------------------------

void test_throttle(Rep& R)
{
	const Flavor& fl = R.fl;
	{
		Ctx c(fl, Opt{ false });
		bool ok = false;
		View v = c.check(true, &ok);
		R.chk(4, ok && v.phase == Ph::UpToDate, u8"節流前置：強制檢查成功一次", vdesc(v));
		const size_t extra = c.check_settle(false);
		R.chk(4, extra == 0, u8"剛成功檢查完，RequestCheck(false) 不連網（一天節流）", "requests=" + std::to_string(extra));
		bool ok2 = false;
		c.check(true, &ok2, 5000);
		R.chk(4, ok2, u8"RequestCheck(true) 不受節流、會再連網");
	}
	{
		Opt o; o.newer = false; o.tagsFail = true;
		Ctx c(fl, o);
		bool ok1 = false, ok2 = false;
		View v1 = c.check(false, &ok1, 5000);
		R.chk(4, ok1, u8"從未成功檢查過：RequestCheck(false) 會連網", vdesc(v1));
		c.check(false, &ok2, 3000);
		R.chk(4, ok2, u8"檢查「失敗」後 RequestCheck(false) 仍會再連網（失敗不計入節流）",
		      ok2 ? "" : std::string(u8"第二次沒有任何請求；失敗後狀態=") + ph_name(v1.phase));
	}
}

// ---- T9 / T15: Shutdown while a download is stuck --------------------------------------------------------------

void test_shutdown(Rep& R)
{
	const Flavor& fl = R.fl;
	Opt o; o.hang = true;
	Ctx c(fl, o);
	bool ok = false;
	View v = c.check(true, &ok);
	if (!ok || v.phase != Ph::UpdateAvailable) {
		R.chk(5, false, u8"卡住下載前置：檢查未進入 UpdateAvailable", vdesc(v));
		return;
	}
	std::vector<std::pair<std::wstring, Snap>> before = c.snap_guard();
	c.u->update();
	const ULONGLONG t0 = GetTickCount64();
	while (!c.net.hanging.load() && GetTickCount64() - t0 < 30000) Sleep(5);
	R.chk(5, c.net.hanging.load(), u8"更新確實進入下載（假來源 data.json 卡住）");
	const ULONGLONG s0 = GetTickCount64();
	c.u->shutdown();
	const ULONGLONG ms = GetTickCount64() - s0;
	R.chk(5, ms < 2000, u8"下載卡住時 Shutdown() 在 2 秒內返回（取消旗標有傳到下載）", std::to_string(ms) + " ms");
	std::string chg = c.changed_since(before);
	R.chk(5, chg.empty() && c.created_absent().empty(), u8"Shutdown 後既有樹檔與版本紀錄不變、沒裝出新版", "changed=" + chg);
}

// ---- T10 補：本機 POB TreeData 較新 ------------------------------------------------------------------------

void test_pob_local(Rep& R)
{
	const Flavor& fl = R.fl;
	{
		Opt o; o.newer = false; o.pobVer = "3_29";
		Ctx c(fl, o);
		bool ok = false;
		View v = c.check(true, &ok);
		R.chk_id("T10 補 a", ok && v.phase == Ph::UpdateAvailable && v.latest == "3_29",
		         u8"遠端沒有更新、本機 POB 有 TreeData\\3_29 → 檢查結果指向 3_29", vdesc(v));
		if (!ok || v.phase != Ph::UpdateAvailable) return;
		c.u->update();
		View d = c.wait_update(&ok);
		R.chk_id("T10 補 a", ok && d.phase == Ph::Done, u8"以本機 POB 為來源的更新走到 Done", vdesc(d));
		std::vector<std::string> calls = c.net.allCalls();
		R.chk_id("T10 補 a", has_call(calls, "raw.githubusercontent.com" + fl.rawPrefix + "3.29.0/data.json"),
		         u8"data.json 仍從 GitHub 取、tag 由版本推得為 3.29.0");
		std::set<std::string> want = fl.neededAssets;
		want.erase("a-3.png");
		R.chk_id("T10 補 a", asset_calls(calls) == want, u8"POB 已有的圖集 a-3.png 不再下載，其餘照下載", "got=" + join(asset_calls(calls)));
		R.chk_id("T10 補 a", slurp(c.dir + L"Data\\tree\\a-3.png") == "POB-A3-SHEET" &&
		                       slurp(c.dir + L"Data\\tree\\b-3.png") == "ASSET:b-3.png",
		         u8"a-3.png 取自 POB、b-3.png 取自下載");
	}
	{
		Opt o; o.newer = false; o.pobVer = "3_27";
		Ctx c(fl, o);
		bool ok = false;
		View v = c.check(true, &ok);
		R.chk_id("T10 補 b", ok && v.phase == Ph::UpToDate, u8"本機 POB 的 TreeData 比目前樹舊 → UpToDate", vdesc(v));
	}
}

int run(const std::wstring& exeDir, const Flavor& fl, const wchar_t* reportName)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	TestReport rep;
	Rep R{ rep, fl };
	test_update(R);
	test_uptodate(R);
	test_bad_data(R);
	test_net_fail(R);
	test_throttle(R);
	test_shutdown(R);
	if (!fl.atlas) test_pob_local(R);
	else rep.note(u8"輿圖更新器不讀本機 POB，無對應補充項");
	return rep.finish(exeDir + reportName);
}

} // namespace

int RunAtlasUpdateSelfTest(const std::wstring& exeDir) { return run(exeDir, atlas_flavor(), L"atlas_update_selftest.txt"); }

int RunPassiveUpdateSelfTest(const std::wstring& exeDir) { return run(exeDir, pt_flavor(), L"pt_update_selftest.txt"); }
