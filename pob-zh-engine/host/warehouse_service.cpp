#include "warehouse_service.h"

#include "warehouse_state.h" // the shared request guard

#include "error_log.h"
#include "warehouse_pricing.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <map>

namespace {

long long NowUtc()
{
	FILETIME ft;
	GetSystemTimeAsFileTime(&ft);
	unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
	return (long long)((t - 116444736000000000ull) / 10000000ull);
}

// The poecdn URL from the API, reduced to the key IconManager fetches by.
// Classic: ".../image/Art/2DItems/X.png?scale=1" -> "Art/2DItems/X".
// Modern stash icons are generator URLs: ".../gen/image/<b64>/<sig>/X.png"
// -> "gen/image/<b64>/<sig>/X" (IconManager knows to fetch these verbatim).
std::string IconArtPath(const std::string& url)
{
	size_t at = url.find("/gen/image/");
	size_t skip = 1; // keep "gen/..." itself
	if (at == std::string::npos) {
		at = url.find("/image/");
		skip = 7; // drop the "/image/" prefix: the classic key starts at "Art/"
	}
	if (at == std::string::npos) return std::string();
	std::string p = url.substr(at + skip);
	size_t q = p.find('?');
	if (q != std::string::npos) p.resize(q);
	if (p.size() > 4 && p.compare(p.size() - 4, 4, ".png") == 0) p.resize(p.size() - 4);
	return p;
}

// The legacy endpoints' realm parameter for a game id.
std::string RealmOf(const std::string& game)
{
	return game == "poe2" ? "poe2" : "pc";
}

} // namespace

void WarehouseAggregateItems(const std::vector<StashItemRaw>& items, Snapshot& snap)
{
	// std::map: aggregation and the sorted-by-key invariant in one pass.
	std::map<std::string, SnapshotLine> byKey;
	for (const StashItemRaw& it : items) {
		std::string key, disp;
		if (!BuildPriceKey(it, &key, &disp)) {
			// Unkeyable kinds still deserve a counted row; "other|" cannot collide
			// with a price key, so they can never be priced by accident.
			key = "other|" + (it.name.empty() ? it.typeLine : it.name);
			disp = it.name.empty() ? it.typeLine : it.name;
		}
		SnapshotLine& line = byKey[key];
		if (line.key.empty()) {
			line.key = key;
			line.dispEn = disp;
			line.icon = IconArtPath(it.icon);
		}
		line.count += it.stackSize;
	}
	snap.lines.clear();
	snap.lines.reserve(byKey.size());
	for (auto& kv : byKey) snap.lines.push_back(std::move(kv.second));
}

void WarehousePriceSnapshot(Snapshot& snap,
                            const std::function<bool(const std::string&, NinjaPrice*)>& lookup,
                            double divineRate)
{
	// Cards the bulk exchange does not list are the sub-1c leftovers; 0.5c is a
	// deliberate conservative floor, flagged so the UI can draw it as "~0.5"
	// and never pass it off as a quote. ONLY cards: a floor on arbitrary
	// unpriced stacks (7000 Rogue's Markers...) would fabricate wealth.
	constexpr double kCardFloorChaos = 0.5;

	snap.totalChaos = 0.0;
	snap.unpricedKinds = 0;
	snap.divineRate = divineRate;
	const bool haveMarket = lookup != nullptr;
	for (SnapshotLine& line : snap.lines) {
		NinjaPrice p;
		bool found = lookup && lookup(line.key, &p);
		// PoE2 support gems trade on the Currency Exchange, so their price sits
		// under the exchange namespace by plain name, not under a bracket key.
		if (!found && lookup && line.key.rfind("gem|", 0) == 0)
			found = lookup(NinjaExchangeKey(line.dispEn), &p);
		if (found && !p.lowConfidence && p.chaos > 0) {
			line.chaosEach = p.chaos;
			line.chaosTotal = p.chaos * (double)line.count;
			line.priced = true;
			line.estimated = false;
			snap.totalChaos += line.chaosTotal;
		} else if (haveMarket && line.key.rfind("card|", 0) == 0) {
			line.chaosEach = kCardFloorChaos;
			line.chaosTotal = kCardFloorChaos * (double)line.count;
			line.priced = true;
			line.estimated = true;
			snap.totalChaos += line.chaosTotal;
		} else {
			line.chaosEach = 0.0;
			line.chaosTotal = 0.0;
			line.priced = false;
			line.estimated = false;
			snap.unpricedKinds++;
		}
	}
}

void WarehouseService::Init(const std::wstring& exeDir)
{
	exeDir_ = exeDir;
	ninja_.Init(exeDir);
	stop_ = false;
	worker_ = std::thread([this] { workerLoop(); });
}

void WarehouseService::Shutdown()
{
	{
		std::lock_guard<std::mutex> lk(cmdMx_);
		stop_ = true;
	}
	cmdCv_.notify_all();
	if (worker_.joinable()) worker_.join();
}

void WarehouseService::SetAuth(const StashAuth& a)
{
	std::lock_guard<std::mutex> lk(authMx_);
	auth_ = a;
}

void WarehouseService::ForgetAuth()
{
	{
		std::lock_guard<std::mutex> lk(authMx_);
		// Overwrite before releasing: "cleared" should not leave the bytes
		// readable in whatever memory the string had.
		WarehouseWipe(auth_.secret);
		auth_ = StashAuth{};
	}
	std::lock_guard<std::mutex> lk(stMx_);
	st_.authOk = false;
	st_.authFailed = false;
}

StashAuth WarehouseService::authCopy()
{
	std::lock_guard<std::mutex> lk(authMx_);
	return auth_;
}

void WarehouseService::SetGame(const std::string& game)
{
	{
		std::lock_guard<std::mutex> lk(cmdMx_);
		game_ = game == "poe2" ? "poe2" : "poe1";
	}
	// Everything the status holds is per realm: a PoE1 tab list, league list or
	// session check says nothing about PoE2.
	std::lock_guard<std::mutex> lk(stMx_);
	st_.tabs.clear();
	st_.tabsReady = false;
	st_.leagues.clear();
	st_.leaguesReady = false;
	st_.authOk = false;
	st_.authFailed = false;
	st_.blocked = false;
}

void WarehouseService::RequestVerify()
{
	std::lock_guard<std::mutex> lk(cmdMx_);
	Cmd c;
	c.kind = Cmd::Kind::Verify;
	c.game = game_;
	cmdQ_.push_back(std::move(c));
	cmdCv_.notify_all();
}

void WarehouseService::RequestLeagues()
{
	std::lock_guard<std::mutex> lk(cmdMx_);
	Cmd c;
	c.kind = Cmd::Kind::Leagues;
	c.game = game_;
	cmdQ_.push_back(std::move(c));
	cmdCv_.notify_all();
}

void WarehouseService::RequestTabList(const std::string& league)
{
	Cmd c;
	c.kind = Cmd::Kind::ListTabs;
	c.league = league;
	std::lock_guard<std::mutex> lk(cmdMx_);
	c.game = game_;
	cmdQ_.push_back(std::move(c));
	cmdCv_.notify_all();
}

void WarehouseService::RequestSnapshot(const std::string& league,
                                       const std::vector<std::string>& tabIds,
                                       bool withPricing)
{
	Cmd c;
	c.kind = Cmd::Kind::Snapshot;
	c.league = league;
	c.tabIds = tabIds;
	c.withPricing = withPricing;
	std::lock_guard<std::mutex> lk(cmdMx_);
	c.game = game_;
	cmdQ_.push_back(std::move(c));
	cmdCv_.notify_all();
}

WarehouseService::Status WarehouseService::Poll()
{
	std::lock_guard<std::mutex> lk(stMx_);
	return st_;
}

bool WarehouseService::TakeSnapshot(Snapshot* out)
{
	std::lock_guard<std::mutex> lk(stMx_);
	if (!st_.snapshotReady) return false;
	*out = std::move(pending_);
	pending_ = Snapshot{};
	st_.snapshotReady = false;
	return true;
}

void WarehouseService::AckDone()
{
	std::lock_guard<std::mutex> lk(stMx_);
	if (st_.phase == WarehousePhase::Done || st_.phase == WarehousePhase::Error) {
		st_.phase = WarehousePhase::Idle;
		st_.message.clear();
	}
}

void WarehouseService::setPhase(WarehousePhase p, const std::string& msg)
{
	std::lock_guard<std::mutex> lk(stMx_);
	st_.phase = p;
	st_.message = msg;
}

void WarehouseService::noteError(StashError kind, const std::string& err)
{
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.phase = WarehousePhase::Error;
		st_.message = err;
		st_.authFailed = kind == StashError::Auth;
		st_.blocked = kind == StashError::Blocked;
		if (kind == StashError::Auth) st_.authOk = false;
	}
	// err carries a fixed message plus at most an HTTP status -- never a header,
	// never the session id (see warehouse_provider_sessid.cpp's contract).
	PobLog::Error("warehouse", err);
}

void WarehouseService::workerLoop()
{
	for (;;) {
		Cmd cmd;
		{
			std::unique_lock<std::mutex> lk(cmdMx_);
			cmdCv_.wait(lk, [this] { return stop_ || !cmdQ_.empty(); });
			if (stop_) return;
			cmd = std::move(cmdQ_.front());
			cmdQ_.pop_front();
		}
		const StashAuth auth = authCopy();
		// Last line of defence: an exception leaving a worker thread terminates
		// the whole process. Whatever slips through the parsers' own guards
		// becomes an Error phase the UI can show, not a crash.
		try {
			switch (cmd.kind) {
			case Cmd::Kind::Verify: doVerify(auth, cmd); break;
			case Cmd::Kind::Leagues: doLeagues(cmd); break;
			case Cmd::Kind::ListTabs: doListTabs(auth, cmd); break;
			case Cmd::Kind::Snapshot: doSnapshot(auth, cmd); break;
			}
		} catch (const std::exception& e) {
			noteError(StashError::Parse, std::string(u8"內部錯誤: ") + e.what());
		} catch (...) {
			noteError(StashError::Parse, u8"內部錯誤（未知例外）");
		}
	}
}

bool WarehouseService::guardRefuses()
{
	const long long until = WarehouseGuardLoad(exeDir_).blockedUntilUtc;
	const long long now = NowUtc();
	if (now >= until) return false;
	// Not logged: this is the guard working, and a retry-happy click would
	// otherwise fill the error log.
	std::lock_guard<std::mutex> lk(stMx_);
	st_.phase = WarehousePhase::Error;
	st_.message = u8"帳號請求額度冷卻中，約 " + std::to_string(until - now) + u8" 秒後再試";
	return true;
}

void WarehouseService::noteBackoff(const IStashProvider& p)
{
	// Runs from a destructor: nothing may escape.
	try {
		const int ms = p.RemainingBackoffMs();
		// The 2 s spacing is our own pacing, not the server's; only a real pause
		// (429, exhausted bucket) is worth carrying to the next job.
		if (ms > kStashMinSpacingMs) WarehouseNoteBlocked(exeDir_, NowUtc() + (ms + 999) / 1000);
	} catch (...) {
		// Swallowed on purpose. This runs from ~BackoffCarry, i.e. during
		// stack unwinding, where an escaping exception calls std::terminate:
		// losing one backoff note costs the next job a wait it did not need,
		// while letting it out costs the whole process.
	}
}

void WarehouseService::doVerify(const StashAuth& auth, const Cmd& cmd)
{
	if (guardRefuses()) return;
	setPhase(WarehousePhase::Verifying, u8"驗證 session 中…");
	auto provider = CreateSessidStashProvider(auth, RealmOf(cmd.game));
	BackoffCarry carry{ this, provider.get() };
	std::string err;
	StashError kind = StashError::None;
	if (!provider->Verify(&err, &kind, &stop_)) {
		noteError(kind, err);
		return;
	}
	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.phase = WarehousePhase::Done;
		st_.message = u8"session 有效";
		st_.authOk = true;
		st_.authFailed = false;
		st_.blocked = false;
	}
}

void WarehouseService::doLeagues(const Cmd& cmd)
{
	setPhase(WarehousePhase::ListingTabs, u8"取得聯盟清單中…");
	std::vector<std::string> leagues;
	std::string err;
	if (!FetchNinjaLeagues(cmd.game, leagues, &err, &stop_)) {
		noteError(StashError::Network, err);
		return;
	}
	std::lock_guard<std::mutex> lk(stMx_);
	st_.leagues = std::move(leagues);
	st_.leaguesReady = true;
	st_.phase = WarehousePhase::Done;
	st_.message = u8"聯盟清單已更新";
}

void WarehouseService::doListTabs(const StashAuth& auth, const Cmd& cmd)
{
	if (guardRefuses()) return;
	setPhase(WarehousePhase::ListingTabs, u8"取得倉庫分頁清單中…");
	auto provider = CreateSessidStashProvider(auth, RealmOf(cmd.game));
	BackoffCarry carry{ this, provider.get() };
	std::vector<StashTabInfo> tabs;
	std::string err;
	StashError kind = StashError::None;
	if (!provider->ListTabs(cmd.league, tabs, &err, &kind, &stop_)) {
		noteError(kind, err);
		return;
	}
	std::lock_guard<std::mutex> lk(stMx_);
	st_.tabs = std::move(tabs);
	st_.tabsReady = true;
	st_.authOk = true;
	st_.authFailed = false;
	st_.blocked = false;
	st_.phase = WarehousePhase::Done;
	st_.message = u8"分頁清單已更新";
}

void WarehouseService::doSnapshot(const StashAuth& auth, const Cmd& cmd)
{
	if (guardRefuses()) return;
	setPhase(WarehousePhase::ListingTabs, u8"取得倉庫分頁清單中…");
	auto provider = CreateSessidStashProvider(auth, RealmOf(cmd.game));
	BackoffCarry carry{ this, provider.get() };
	std::vector<StashTabInfo> tabs;
	std::string err;
	StashError kind = StashError::None;
	if (!provider->ListTabs(cmd.league, tabs, &err, &kind, &stop_)) {
		noteError(kind, err);
		return;
	}

	// The saved selection is tab IDS; indexes are re-resolved per run because
	// dragging a tab renumbers everything after it.
	std::vector<const StashTabInfo*> wanted;
	for (const std::string& id : cmd.tabIds)
		for (const StashTabInfo& t : tabs)
			if (t.id == id) { wanted.push_back(&t); break; }
	if (wanted.empty()) {
		noteError(StashError::Parse, u8"選擇的分頁在此聯盟找不到（分頁被移除？請重新選擇）");
		return;
	}

	{
		std::lock_guard<std::mutex> lk(stMx_);
		st_.tabs = tabs;
		st_.tabsReady = true;
		st_.authOk = true;
		st_.authFailed = false;
		st_.blocked = false;
		st_.phase = WarehousePhase::FetchingTabs;
		st_.tabsDone = 0;
		st_.tabsTotal = (int)wanted.size();
		st_.message = u8"抓取倉庫分頁中…";
	}

	std::vector<StashItemRaw> all;
	for (size_t i = 0; i < wanted.size(); i++) {
		if (stop_) return;
		std::vector<StashItemRaw> items;
		if (!provider->FetchTab(cmd.league, wanted[i]->index, items, &err, &kind, &stop_)) {
			noteError(kind, err);
			return;
		}
		all.insert(all.end(), items.begin(), items.end());
		std::lock_guard<std::mutex> lk(stMx_);
		st_.tabsDone = (int)(i + 1);
	}

	Snapshot snap;
	snap.utc = NowUtc();
	snap.league = cmd.league;
	for (const StashTabInfo* t : wanted) snap.tabIds.push_back(t->id);
	WarehouseAggregateItems(all, snap);

	double divineRate = 0.0;
	if (cmd.withPricing) {
		setPhase(WarehousePhase::Pricing, u8"取得市場價格中…");
		std::string nerr;
		// A pricing failure downgrades the snapshot to counts-only; it is still a
		// snapshot, and the totals stay honestly at zero.
		if (!ninja_.Refresh(cmd.game, cmd.league, false, &nerr, &stop_))
			PobLog::Error("warehouse", u8"估價來源失敗（快照退化為僅數量）: " + nerr);
		divineRate = ninja_.DivineRate();
	}
	WarehousePriceSnapshot(
	    snap,
	    [this](const std::string& key, NinjaPrice* out) { return ninja_.PriceOf(key, out); },
	    divineRate);

	{
		std::lock_guard<std::mutex> lk(stMx_);
		pending_ = std::move(snap);
		st_.snapshotReady = true;
		st_.phase = WarehousePhase::Done;
		st_.message = u8"快照完成";
	}
}
