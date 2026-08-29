// --warehouse-selftest: the stash-revenue tool's data layer, headless.
//
// Everything network-shaped is exercised through fixtures: stash JSON, ninja
// overview JSON, rate-limit headers. Files are written only under %TEMP%; the
// real PobTools\ directory is the user's, not ours (same rule as every other
// selftest -- see error_log.h).
//
// --warehouse-probe is the deliberate exception: it DOES talk to the real API,
// which is why it is its own flag and no selftest ever calls it.
#include "warehouse_tool.h"

#include "error_log.h"
#include "warehouse_ninja.h"
#include "warehouse_pricing.h"
#include "warehouse_provider.h"
#include "warehouse_service.h"
#include "warehouse_snapshot.h"
#include "warehouse_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Report {
	std::string text;
	int failures = 0, checks = 0;
	void check(const std::string& name, bool ok, const std::string& detail = "")
	{
		checks++;
		text += std::string(ok ? "PASS " : "FAIL ") + name +
		        (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	}
};

// A trimmed but shape-faithful get-stash-items payload: the tab list, a currency
// stack, a levelled gem, a six-linked unique, a rare map, a card, a frameless
// fragment and a rare glove that must stay unpriceable.
const char kStashFixture[] = R"json({
  "numTabs": 3,
  "tabs": [
    { "n": "通貨", "i": 0, "id": "abc123", "type": "CurrencyStash", "colour": { "r": 213, "g": 159, "b": 0 } },
    { "n": "dump", "i": 1, "id": "def456", "type": "PremiumStash", "colour": { "r": 40, "g": 100, "b": 200 } },
    { "n": "maps", "i": 2, "id": "ghi789", "type": "MapStash", "colour": { "r": 0, "g": 0, "b": 0 } }
  ],
  "items": [
    { "name": "", "typeLine": "Divine Orb", "baseType": "Divine Orb",
      "icon": "https://web.poecdn.com/image/Art/2DItems/Currency/CurrencyModValues.png?scale=1",
      "frameType": 5, "stackSize": 12, "w": 1, "h": 1 },
    { "name": "", "typeLine": "Awakened Multistrike Support", "baseType": "Awakened Multistrike Support",
      "icon": "https://web.poecdn.com/image/Art/2DItems/Gems/Support/SupportPlus/MultipleAttacksPlus.png",
      "frameType": 4, "corrupted": true,
      "properties": [ { "name": "Level", "values": [["5 (Max)", 0]] },
                      { "name": "Quality", "values": [["+20%", 1]] } ] },
    { "name": "<<set:MS>><<set:M>><<set:S>>Shavronne's Wrappings", "typeLine": "Occultist's Vestment",
      "baseType": "Occultist's Vestment", "icon": "https://web.poecdn.com/image/Art/2DItems/Armours/BodyArmours/ShavronnesWrappings.png",
      "frameType": 3, "ilvl": 84,
      "sockets": [ { "group": 0, "sColour": "B" }, { "group": 0, "sColour": "B" },
                   { "group": 0, "sColour": "B" }, { "group": 0, "sColour": "B" },
                   { "group": 0, "sColour": "B" }, { "group": 0, "sColour": "B" } ] },
    { "name": "Doom Refuge", "typeLine": "Sanctuary Map", "baseType": "Sanctuary Map",
      "icon": "https://web.poecdn.com/image/Art/2DItems/Maps/Atlas2Maps/New/Sanctuary.png?scale=1&tier=16",
      "frameType": 2, "ilvl": 83,
      "properties": [ { "name": "Map Tier", "values": [["16", 0]] } ] },
    { "name": "", "typeLine": "The Doctor", "baseType": "The Doctor",
      "icon": "https://web.poecdn.com/image/Art/2DItems/Divination/InventoryIcon.png",
      "frameType": 6, "stackSize": 3 },
    { "name": "", "typeLine": "Sacrifice at Dusk", "baseType": "Sacrifice at Dusk",
      "icon": "https://web.poecdn.com/image/Art/2DItems/Maps/Vaal01.png",
      "frameType": 0, "stackSize": 4 },
    { "name": "Storm Grip", "typeLine": "Sorcerer Gloves", "baseType": "Sorcerer Gloves",
      "icon": "https://web.poecdn.com/image/Art/2DItems/Armours/Gloves/SorcererGloves.png",
      "frameType": 2, "ilvl": 84 }
  ]
})json";

const char kExchangeFixture[] = R"json({
  "items": [
    { "id": 1, "name": "Divine Orb", "image": "/gen/image/divine.png", "category": "Currency" },
    { "id": 2, "name": "Mirror of Kalandra", "image": "/gen/image/mirror.png", "category": "Currency" },
    { "id": 3, "name": "Orphaned Line", "image": "/x.png", "category": "Currency" }
  ],
  "lines": [
    { "id": 1, "primaryValue": 210.5 },
    { "id": 2, "primaryValue": 120000 },
    { "id": 99, "primaryValue": 5 }
  ]
})json";

const char kGemOverviewFixture[] = R"json({
  "lines": [
    { "name": "Awakened Multistrike Support", "icon": "https://web.poecdn.com/x.png",
      "chaosValue": 950.0, "count": 40, "gemLevel": 5, "gemQuality": 20, "corrupted": true },
    { "name": "Empower Support", "chaosValue": 300.0, "count": 3, "gemLevel": 4, "gemQuality": 0, "corrupted": true }
  ]
})json";

std::wstring ScratchDir()
{
	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	std::wstring dir = std::wstring(tmp) + L"pobtools_warehouse_selftest\\";
	CreateDirectoryW(dir.c_str(), nullptr);
	return dir;
}

void PrintAndSave(const std::wstring& exeDir, const std::string& rep,
                  const wchar_t* fileName)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	printf("%s", rep.c_str());
	HANDLE h = CreateFileW((exeDir + fileName).c_str(), GENERIC_WRITE, 0, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, rep.data(), (DWORD)rep.size(), &w, nullptr);
		CloseHandle(h);
	}
}

} // namespace

int RunWarehouseSelfTest(const std::wstring& exeDir)
{
	Report rep;
	const std::wstring scratch = ScratchDir();
	PobLog::SetDirForTest(scratch);

	// ---- stash JSON parsing ----------------------------------------------
	{
		std::vector<StashItemRaw> items;
		std::vector<StashTabInfo> tabs;
		std::string err;
		bool ok = ParseStashTabJson(kStashFixture, 7, &items, &tabs, &err);
		rep.check("T1 stash fixture parses", ok, err);
		rep.check("T1a three tabs", tabs.size() == 3);
		rep.check("T1b tab identity", tabs.size() == 3 && tabs[0].id == "abc123" &&
		                                  tabs[0].name == u8"通貨" && tabs[2].index == 2);
		rep.check("T1c tab colour packed", tabs.size() == 3 && tabs[0].colour == 0xD59F00);
		rep.check("T1d seven items", items.size() == 7);
		if (items.size() == 7) {
			rep.check("T1e stack size", items[0].stackSize == 12 && items[0].frameType == 5);
			rep.check("T1f gem properties", items[1].gemLevel == 5 &&
			                                    items[1].gemQuality == 20 && items[1].corrupted);
			rep.check("T1g unique name unwrapped",
			          items[2].name == "Shavronne's Wrappings" && items[2].links == 6);
			rep.check("T1h map tier", items[3].mapTier == 16 && items[3].frameType == 2);
			rep.check("T1i tabIndex stamped", items[0].tabIndex == 7);
		}
		std::string perr;
		rep.check("T1j garbage body refused",
		          !ParseStashTabJson("<html>login</html>", 0, &items, nullptr, &perr));
		rep.check("T1k error body refused",
		          !ParseStashTabJson(R"({"error":{"message":"x"}})", 0, &items, nullptr, &perr));
	}

	// ---- price keys -------------------------------------------------------
	{
		auto keyOf = [&](const StashItemRaw& it) {
			std::string k, d;
			return BuildPriceKey(it, &k, &d) ? k : std::string("(none)");
		};
		std::vector<StashItemRaw> items;
		ParseStashTabJson(kStashFixture, 0, &items, nullptr, nullptr);
		rep.check("T2 currency key", items.size() == 7 &&
		                                 keyOf(items[0]) == "currency|Divine Orb");
		rep.check("T2a gem key buckets", items.size() == 7 &&
		                                     keyOf(items[1]) ==
		                                         "gem|Awakened Multistrike Support|1|20|c");
		rep.check("T2b unique 6L key",
		          items.size() == 7 &&
		              keyOf(items[2]) == "unique|Shavronne's Wrappings|Occultist's Vestment|6L");
		rep.check("T2c rare map keys by base", items.size() == 7 &&
		                                           keyOf(items[3]) == "map|Sanctuary Map|T16");
		rep.check("T2d card key", items.size() == 7 && keyOf(items[4]) == "card|The Doctor");
		rep.check("T2e frameless fragment keys as currency",
		          items.size() == 7 && keyOf(items[5]) == "currency|Sacrifice at Dusk");
		rep.check("T2f rare gear unpriceable", items.size() == 7 && keyOf(items[6]) == "(none)");

		StashItemRaw gem;
		gem.frameType = 4;
		gem.typeLine = "Empower Support";
		gem.gemLevel = 3;
		gem.gemQuality = 13;
		rep.check("T2g gem below brackets rounds down",
		          keyOf(gem) == "gem|Empower Support|1|0");
		gem.gemLevel = 21;
		gem.gemQuality = 23;
		rep.check("T2h gem 21/23", keyOf(gem) == "gem|Empower Support|21|23");
		StashItemRaw unid;
		unid.frameType = 3;
		unid.typeLine = "Occultist's Vestment";
		rep.check("T2i unidentified unique unpriceable", keyOf(unid) == "(none)");
	}

	// ---- ninja parsers ----------------------------------------------------
	{
		std::vector<std::pair<std::string, NinjaPrice>> out;
		std::string err;
		bool ok = NinjaPriceSource::ParseExchangeOverview(kExchangeFixture, &out, &err);
		rep.check("T3 exchange fixture parses", ok, err);
		rep.check("T3a joined on id, orphans dropped", out.size() == 2);
		bool divineOk = false;
		for (auto& kv : out)
			if (kv.first == "currency|Divine Orb" && kv.second.chaos == 210.5) divineOk = true;
		rep.check("T3b exchange key + primaryValue", divineOk);

		out.clear();
		ok = NinjaPriceSource::ParseItemOverview(kGemOverviewFixture, "SkillGem", &out, &err);
		rep.check("T3c gem overview parses", ok, err);
		rep.check("T3d gem line count", out.size() == 2);
		if (out.size() == 2) {
			rep.check("T3e gem key matches stash side",
			          out[0].first == "gem|Awakened Multistrike Support|1|20|c");
			rep.check("T3f low-confidence flagged", out[1].second.lowConfidence &&
			                                            !out[0].second.lowConfidence);
		}
		rep.check("T3g cache freshness rule",
		          NinjaPriceSource::CacheFresh(1000, 1000 + 14 * 60) &&
		              !NinjaPriceSource::CacheFresh(1000, 1000 + 15 * 60) &&
		              !NinjaPriceSource::CacheFresh(0, 1000));

		// Regression: poe.ninja emits explicit nulls, and value(key, default)
		// throws on them. This once escaped the worker thread and killed the
		// process mid-"取得市場價格".
		const char kNullsItem[] = R"json({"lines":[
		  {"name":null,"chaosValue":5},
		  {"name":"HasNullValue","chaosValue":null},
		  {"name":"Survivor","chaosValue":10,"count":null,"links":null,"baseType":null}
		]})json";
		out.clear();
		ok = NinjaPriceSource::ParseItemOverview(kNullsItem, "UniqueWeapon", &out, &err);
		rep.check("T3h null fields do not throw", ok, err);
		rep.check("T3i null-riddled line still priced",
		          out.size() == 1 && out[0].first == "unique|Survivor|" &&
		              out[0].second.chaos == 10.0);
		const char kNullsExch[] = R"json({"items":[{"id":1,"name":null},{"id":2,"name":"Ok"}],
		  "lines":[{"id":1,"primaryValue":3},{"id":2,"primaryValue":null},{"id":null}]})json";
		out.clear();
		ok = NinjaPriceSource::ParseExchangeOverview(kNullsExch, &out, &err);
		rep.check("T3j exchange nulls do not throw", ok && out.empty(), err);
		// Exchange ids are numbers for currency but SLUG STRINGS for cards, and
		// cards key under card| -- both once fell through the numeric-only join.
		const char kCardExch[] = R"json({"items":[
		  {"id":"abandoned-wealth","name":"Abandoned Wealth","category":"Cards"}],
		  "lines":[{"id":"abandoned-wealth","primaryValue":0.22}]})json";
		out.clear();
		ok = NinjaPriceSource::ParseExchangeOverview(kCardExch, &out, &err, "card|");
		rep.check("T3o string ids join + card prefix",
		          ok && out.size() == 1 && out[0].first == "card|Abandoned Wealth" &&
		              out[0].second.chaos == 0.22, err);

		const char kCurrencyOverview[] = R"json({"lines":[
		  {"currencyTypeName":"Orb of Dominance","chaosEquivalent":804.0,
		   "receive":{"count":11,"listing_count":25}},
		  {"currencyTypeName":"Thin Crowd","chaosEquivalent":40,"receive":{"count":2}},
		  {"currencyTypeName":null,"chaosEquivalent":5},
		  {"currencyTypeName":"No Price","chaosEquivalent":null}
		]})json";
		out.clear();
		ok = NinjaPriceSource::ParseCurrencyOverview(kCurrencyOverview, &out, &err);
		rep.check("T3l currency overview parses (nulls skipped)", ok && out.size() == 2, err);
		if (out.size() == 2) {
			rep.check("T3m currency key + chaosEquivalent",
			          out[0].first == "currency|Orb of Dominance" &&
			              out[0].second.chaos == 804.0 && !out[0].second.lowConfidence);
			rep.check("T3n currency low count flagged", out[1].second.lowConfidence);
		}
		std::vector<StashItemRaw> nitems;
		rep.check("T3k stash null item skipped, not fatal",
		          ParseStashTabJson(R"json({"items":[{"typeLine":null},
		            {"typeLine":"Chaos Orb","frameType":5,"stackSize":2}]})json",
		                            0, &nitems, nullptr, &err) &&
		              nitems.size() == 1 && nitems[0].typeLine == "Chaos Orb");
	}

	// ---- aggregation + pricing -------------------------------------------
	{
		std::vector<StashItemRaw> items;
		ParseStashTabJson(kStashFixture, 0, &items, nullptr, nullptr);
		// A second divine stack must merge into one line.
		StashItemRaw more;
		more.typeLine = more.baseType = "Divine Orb";
		more.frameType = 5;
		more.stackSize = 8;
		items.push_back(more);

		Snapshot snap;
		WarehouseAggregateItems(items, snap);
		rep.check("T4 aggregation merges stacks", snap.lines.size() == 7);
		const SnapshotLine* divine = nullptr;
		for (const SnapshotLine& l : snap.lines)
			if (l.key == "currency|Divine Orb") divine = &l;
		rep.check("T4a merged count", divine && divine->count == 20);
		rep.check("T4b icon reduced to art path",
		          divine && divine->icon == "Art/2DItems/Currency/CurrencyModValues");
		bool sorted = true;
		for (size_t i = 1; i < snap.lines.size(); i++)
			if (snap.lines[i - 1].key >= snap.lines[i].key) sorted = false;
		rep.check("T4c lines sorted by key", sorted);

		auto lookup = [](const std::string& key, NinjaPrice* out) {
			if (key == "currency|Divine Orb") {
				out->chaos = 210.0;
				return true;
			}
			if (key == "card|The Doctor") {
				out->chaos = 400.0;
				out->listingCount = 3;
				out->lowConfidence = true; // must be skipped
				return true;
			}
			return false;
		};
		WarehousePriceSnapshot(snap, lookup, 210.0);
		// The Doctor's quote is low-confidence, so it falls to the 0.5c card
		// floor: counted, but flagged as an estimate.
		rep.check("T4d total = priced lines + card floor",
		          snap.totalChaos == 20 * 210.0 + 3 * 0.5);
		const SnapshotLine* doctor = nullptr;
		for (const SnapshotLine& l : snap.lines)
			if (l.key == "card|The Doctor") doctor = &l;
		rep.check("T4e low-confidence card floored + flagged",
		          doctor && doctor->priced && doctor->estimated &&
		              doctor->chaosEach == 0.5);
		rep.check("T4e2 non-card unpriced stays unpriced",
		          snap.unpricedKinds == (int)snap.lines.size() - 2);
		rep.check("T4f divine rate stored", snap.divineRate == 210.0);
	}

	// ---- diff -------------------------------------------------------------
	{
		Snapshot a, b;
		a.utc = 1000000;
		b.utc = a.utc + 7200; // 2h
		a.lines = { { "currency|Chaos Orb", "Chaos Orb", "", 100, 1.0, 100.0, true },
			        { "currency|Divine Orb", "Divine Orb", "", 10, 200.0, 2000.0, true } };
		a.totalChaos = 2100.0;
		b.lines = { { "card|The Doctor", "The Doctor", "", 1, 500.0, 500.0, true },
			        { "currency|Divine Orb", "Divine Orb", "", 12, 200.0, 2400.0, true } };
		b.totalChaos = 2900.0;
		SnapshotDiff d = DiffSnapshots(a, b);
		rep.check("T5 net change", d.dTotalChaos == 800.0);
		rep.check("T5a chaos per hour", d.chaosPerHour == 400.0 && d.hours == 2.0);
		rep.check("T5b gained rows", d.gained.size() == 2);
		rep.check("T5c lost rows", d.lost.size() == 1 &&
		                               d.lost[0].key == "currency|Chaos Orb" &&
		                               d.lost[0].dCount == -100);
		rep.check("T5d gained sorted by value",
		          d.gained.size() == 2 && d.gained[0].key == "card|The Doctor" &&
		              d.gained[0].dChaos == 500.0 && d.gained[1].dCount == 2);
	}

	// ---- throttle arithmetic ---------------------------------------------
	{
		SimpleThrottle th;
		th.SetMinIntervalMs(1000);
		th.OnRequest(10000);
		rep.check("T6 min spacing", th.NextDelayMs(10400) == 600 && th.NextDelayMs(11000) == 0);

		std::unordered_map<std::string, std::string> h;
		h["x-rate-limit-rules"] = "Account";
		h["x-rate-limit-account"] = "30:60:60";
		h["x-rate-limit-account-state"] = "16:60:60";
		th.OnResponse(10000, 200, h);
		// Past half the bucket: spacing doubles to 2*(60000/30) = 4000 from the
		// last request.
		rep.check("T6a soft backoff", th.NextDelayMs(10000) == 4000);

		SimpleThrottle th2;
		th2.SetMinIntervalMs(1000);
		th2.OnRequest(50000);
		std::unordered_map<std::string, std::string> h2;
		h2["x-rate-limit-rules"] = "Account";
		h2["x-rate-limit-account"] = "30:60:60";
		h2["x-rate-limit-account-state"] = "30:60:60";
		th2.OnResponse(50000, 200, h2);
		rep.check("T6b exhausted bucket waits the penalty",
		          th2.NextDelayMs(50000) == 60000);

		SimpleThrottle th3;
		th3.OnRequest(80000);
		std::unordered_map<std::string, std::string> h3;
		h3["retry-after"] = "10";
		th3.OnResponse(80000, 429, h3);
		rep.check("T6c 429 respects Retry-After (+1s)", th3.NextDelayMs(80000) == 11000);
	}

	// ---- state round-trip + DPAPI ----------------------------------------
	{
		const std::string fakeToken = "selftest-not-a-real-token-0123456789abcd";
		std::string blob = WarehouseProtectSecret(fakeToken);
		rep.check("T7 DPAPI blob differs from plaintext", !blob.empty() && blob != fakeToken);
		rep.check("T7a DPAPI round-trip", WarehouseUnprotectSecret(blob) == fakeToken);
		rep.check("T7b tampered blob loads empty",
		          WarehouseUnprotectSecret("AAAA" + blob).empty());

		WarehouseUiState s;
		s.accountName = "Tester#1234";
		s.league = "Mercenaries";
		s.selectedTabIds = { "abc123", "def456" };
		s.autoMinutes = 10;
		s.showDivine = true;
		s.sessid = fakeToken;
		rep.check("T7c state saves", s.Save(scratch));

		// The file itself must not contain the token in the clear.
		HANDLE f = CreateFileW((scratch + L"PobTools\\warehouse_ui.json").c_str(),
		                       GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
		                       nullptr);
		std::string body;
		if (f != INVALID_HANDLE_VALUE) {
			char buf[8192];
			DWORD rd = 0;
			while (ReadFile(f, buf, sizeof(buf), &rd, nullptr) && rd) body.append(buf, rd);
			CloseHandle(f);
		}
		rep.check("T7d no plaintext token on disk",
		          !body.empty() && body.find(fakeToken) == std::string::npos);

		WarehouseUiState in;
		rep.check("T7e state loads", in.Load(scratch));
		rep.check("T7f fields round-trip", in.accountName == s.accountName &&
		                                       in.league == s.league &&
		                                       in.selectedTabIds == s.selectedTabIds &&
		                                       in.autoMinutes == 10 && in.showDivine);
		rep.check("T7g token round-trips through DPAPI", in.sessid == fakeToken);
	}

	// ---- history round-trip + prune --------------------------------------
	{
		WarehouseHistory h;
		Snapshot s1;
		s1.utc = 1700000000;
		s1.league = "Mercenaries";
		s1.tabIds = { "abc123" };
		s1.totalChaos = 1234.5;
		s1.divineRate = 210.0;
		s1.lines = { { "currency|Divine Orb", "Divine Orb", "Art/x", 5, 210.0, 1050.0, true } };
		Snapshot s2 = s1;
		s2.utc += 600;
		s2.totalChaos = 1300.0;
		h.snaps = { s1, s2 };
		h.sessionStartUtc = s1.utc;
		rep.check("T8 history saves", h.Save(scratch));
		WarehouseHistory in;
		rep.check("T8a history loads", in.Load(scratch));
		rep.check("T8b snapshots round-trip",
		          in.snaps.size() == 2 && in.sessionStartUtc == s1.utc &&
		              in.snaps[0].lines.size() == 1 &&
		              in.snaps[0].lines[0].key == "currency|Divine Orb" &&
		              in.snaps[0].lines[0].chaosTotal == 1050.0 &&
		              in.snaps[1].totalChaos == 1300.0);

		// Prune: 6 days of 10-minute snapshots -> hourly beyond 48h, capped at 200,
		// session start immortal.
		WarehouseHistory big;
		const long long now = 1700000000;
		for (int i = 0; i < 6 * 24 * 6; i++) {
			Snapshot s;
			s.utc = now - (long long)(6 * 24 * 6 - i) * 600;
			s.totalChaos = i;
			big.snaps.push_back(std::move(s));
		}
		big.sessionStartUtc = big.snaps[0].utc;
		const long long startUtc = big.sessionStartUtc;
		big.Prune(now);
		bool startKept = false;
		for (const Snapshot& s : big.snaps)
			if (s.utc == startUtc) startKept = true;
		rep.check("T8c prune caps at 200", big.snaps.size() <= 200,
		          "kept " + std::to_string(big.snaps.size()));
		rep.check("T8d session start survives pruning", startKept);
		bool ascending = true;
		for (size_t i = 1; i < big.snaps.size(); i++)
			if (big.snaps[i - 1].utc > big.snaps[i].utc) ascending = false;
		rep.check("T8e still utc-ascending", ascending);
	}

	PobLog::SetDirForTest(L"");
	rep.text += rep.failures == 0
	                ? "\nALL PASS (" + std::to_string(rep.checks) + " checks)\n"
	                : "\nFAILURES: " + std::to_string(rep.failures) + " / " +
	                      std::to_string(rep.checks) + "\n";
	PrintAndSave(exeDir, rep.text, L"warehouse_selftest.txt");
	return rep.failures == 0 ? 0 : 1;
}

int RunWarehouseProbe(const std::wstring& exeDir)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	std::string out;

	WarehouseUiState st;
	st.Load(exeDir);
	if (st.accountName.empty() || st.sessid.empty()) {
		out += u8"倉庫收益線上探針：尚未設定。\n"
		       u8"先開啟工具（--warehouse）輸入帳號名與 POESESSID 並按「驗證 session」，"
		       u8"或至少讓設定檔存在後再跑。\n";
		printf("%s", out.c_str());
		return 2;
	}
	const std::string league = st.league.empty() ? "Standard" : st.league;
	out += u8"帳號: " + st.accountName + u8"  聯盟: " + league + "\n";

	StashAuth auth;
	auth.accountName = st.accountName;
	auth.secret = st.sessid;
	auto provider = CreateSessidStashProvider(auth);

	std::string err;
	StashError kind = StashError::None;
	DWORD t0 = GetTickCount();
	if (!provider->Verify(&err, &kind, nullptr)) {
		out += u8"Verify: 失敗 [" + std::to_string((int)kind) + "] " + err + "\n";
		printf("%s", out.c_str());
		return 1;
	}
	out += u8"Verify: OK（" + std::to_string(GetTickCount() - t0) + " ms）\n";

	std::vector<StashTabInfo> tabs;
	t0 = GetTickCount();
	if (!provider->ListTabs(league, tabs, &err, &kind, nullptr)) {
		out += u8"ListTabs: 失敗 [" + std::to_string((int)kind) + "] " + err + "\n";
		printf("%s", out.c_str());
		return 1;
	}
	out += u8"ListTabs: OK，" + std::to_string(tabs.size()) + u8" 個分頁（" +
	       std::to_string(GetTickCount() - t0) + " ms）\n";
	for (size_t i = 0; i < tabs.size() && i < 5; i++)
		out += "  #" + std::to_string(tabs[i].index) + " " + tabs[i].name + " [" +
		       tabs[i].type + "]\n";
	if (tabs.size() > 5) out += "  ...\n";
	out += u8"探針結束：sessid 通道可用。\n";
	printf("%s", out.c_str());
	return 0;
}
