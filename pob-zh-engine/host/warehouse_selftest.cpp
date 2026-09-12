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

#include "atlas_cost.h"
#include "error_log.h"
#include "warehouse_format.h"
#include "warehouse_ninja.h"
#include "warehouse_pricing.h"
#include "warehouse_provider.h"
#include "warehouse_service.h"
#include "warehouse_snapshot.h"
#include "warehouse_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <cmath>
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

std::string ReadFileForTest(const std::wstring& path)
{
	std::string body;
	HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
	                       nullptr);
	if (f == INVALID_HANDLE_VALUE) return body;
	char buf[8192];
	DWORD rd = 0;
	while (ReadFile(f, buf, sizeof(buf), &rd, nullptr) && rd) body.append(buf, rd);
	CloseHandle(f);
	return body;
}

bool WriteFileForTest(const std::wstring& path, const std::string& body)
{
	HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	const bool ok = WriteFile(f, body.data(), (DWORD)body.size(), &w, nullptr) && w == body.size();
	CloseHandle(f);
	return ok;
}

long long UnixOfForTest(const FILETIME& ft)
{
	ULARGE_INTEGER t;
	t.LowPart = ft.dwLowDateTime;
	t.HighPart = ft.dwHighDateTime;
	return (long long)((t.QuadPart - 116444736000000000ull) / 10000000ull);
}

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

		// PoE2 quotes in DIVINE (core.primary); converted to chaos at parse time
		// via core.rates, or every PoE2 price would be off by ~11x.
		const char kPoe2Exch[] = R"json({"core":{"primary":"divine","secondary":"chaos",
		  "rates":{"exalted":256.8,"chaos":11.31}},
		  "items":[{"id":"exalted","name":"Exalted Orb"}],
		  "lines":[{"id":"exalted","primaryValue":0.1}]})json";
		out.clear();
		double cpd = 0.0;
		ok = NinjaPriceSource::ParseExchangeOverview(kPoe2Exch, &out, &err, "currency|", &cpd);
		rep.check("T3p PoE2 divine quote converted to chaos, rate from core",
		          ok && out.size() == 1 && std::fabs(out[0].second.chaos - 1.131) < 1e-9 &&
		              cpd == 11.31,
		          err);
		const char kPoe1Core[] = R"json({"core":{"primary":"chaos","secondary":"divine",
		  "rates":{"divine":0.0025}},"items":[{"id":1,"name":"Divine Orb"}],
		  "lines":[{"id":1,"primaryValue":400}]})json";
		out.clear();
		cpd = 0.0;
		ok = NinjaPriceSource::ParseExchangeOverview(kPoe1Core, &out, &err, "currency|", &cpd);
		rep.check("T3q PoE1 chaos quote untouched, divine rate from core",
		          ok && out.size() == 1 && out[0].second.chaos == 400.0 &&
		              std::fabs(cpd - 400.0) < 1e-6,
		          err);
		const char kPoe2Item[] = R"json({"core":{"primary":"divine","rates":{"chaos":10}},
		  "lines":[{"name":"Redbeak","baseType":"Shortsword","primaryValue":0.5,"listingCount":40},
		           {"name":"Troll","baseType":"Shortsword","primaryValue":2171,"listingCount":2}]})json";
		out.clear();
		ok = NinjaPriceSource::ParseItemOverview(kPoe2Item, "UniqueWeapons", &out, &err);
		rep.check("T3r PoE2 unique: primaryValue->chaos, listingCount confidence",
		          ok && out.size() == 2 && out[0].first == "unique|Redbeak|Shortsword" &&
		              out[0].second.chaos == 5.0 && !out[0].second.lowConfidence &&
		              out[1].second.lowConfidence,
		          err);

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

		// PoE2 support gems: no bracket price, but an exchange price under the
		// gem's plain name.
		Snapshot g2;
		SnapshotLine gl;
		gl.key = "gem|Lineage Support|20|0";
		gl.dispEn = "Lineage Support";
		gl.count = 2;
		g2.lines = { gl };
		WarehousePriceSnapshot(
		    g2,
		    [](const std::string& key, NinjaPrice* o) {
			    if (key != "currency|Lineage Support") return false;
			    o->chaos = 30.0;
			    return true;
		    },
		    0.0);
		rep.check("T4g gem falls back to its exchange price",
		          g2.lines[0].priced && g2.totalChaos == 60.0);
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
		// The split: count changes (farmed / spent) apart from the market.
		rep.check("T5e split: gained = new card + 2 divines, lost = the chaos, no market move",
		          d.qtyGain == 900.0 && d.qtyLoss == -100.0 && d.priceMove == 0.0);
		rep.check("T5f the split adds up to the net change",
		          std::fabs(d.qtyGain + d.qtyLoss + d.priceMove - d.dTotalChaos) < 1e-9);
		rep.check("T5g farm rate counts quantity only", d.farmPerHour == 400.0);
	}
	{
		// Held stock re-priced, and a quote that vanished.
		Snapshot a, b;
		a.utc = 1000000;
		b.utc = a.utc + 3600;
		a.lines = { { "card|Rain of Chaos", "Rain of Chaos", "", 5, 3.0, 15.0, true },
			        { "currency|Divine Orb", "Divine Orb", "", 10, 200.0, 2000.0, true } };
		a.totalChaos = 2015.0;
		b.lines = { { "card|Rain of Chaos", "Rain of Chaos", "", 5, 0.0, 0.0, false },
			        { "currency|Divine Orb", "Divine Orb", "", 12, 210.0, 2520.0, true } };
		b.totalChaos = 2520.0;
		const SnapshotDiff d = DiffSnapshots(a, b);
		const SnapshotDiffLine* dv = nullptr;
		const SnapshotDiffLine* cd = nullptr;
		for (const SnapshotDiffLine& l : d.gained)
			if (l.key == "currency|Divine Orb") dv = &l;
		for (const SnapshotDiffLine& l : d.lost)
			if (l.key == "card|Rain of Chaos") cd = &l;
		// Divine: 2 more at 210 = +420 (count); the 10 held x +10 = +100 (market).
		rep.check("T5h both sides: count at the new price, held stock at the price change",
		          dv && dv->dQtyChaos == 420.0 && dv->dPriceChaos == 100.0);
		rep.check("T5i a vanished quote is a market move, not spending",
		          cd && cd->dQtyChaos == 0.0 && cd->dPriceChaos == -15.0 && d.qtyLoss == 0.0);
		rep.check("T5j totals by cause",
		          d.qtyGain == 420.0 && d.priceMove == 85.0 &&
		              std::fabs(d.qtyGain + d.qtyLoss + d.priceMove - d.dTotalChaos) < 1e-9);
		Snapshot sa = a;
		sa.summary = true;
		sa.lineCount = 2;
		sa.lines.clear();
		const SnapshotDiff ds = DiffSnapshots(sa, b);
		rep.check("T5k a summary side: totals only, no lines, no split",
		          ds.summaryOnly && ds.gained.empty() && ds.lost.empty() && ds.dTotalChaos == 505.0 &&
		              ds.qtyGain == 0.0 && ds.priceMove == 0.0);
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
		rep.check("T6d the pause is readable, to carry past the provider",
		          th3.BlockedUntilMs() == 91000);
		rep.check("T6e requests are spaced at least 1.5 s by default", kStashMinSpacingMs >= 1500);
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
		s.game = "poe2";
		s.poe1.league = "Mercenaries";
		s.poe1.tabIds = { "abc123", "def456" };
		s.poe2.league = "Forbidden Rites";
		s.poe2.tabIds = { "p2tab" };
		s.autoMinutes = 120;
		s.showDivine = true;
		s.sessid = fakeToken;
		s.rememberSessid = true; // the blob is written only when the box is ticked
		rep.check("T7c state saves", s.Save(scratch, /*writeSecret=*/true));

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
		rep.check("T7f fields round-trip (both games' picks kept apart)",
		          in.accountName == s.accountName && in.game == "poe2" &&
		              in.poe1.league == s.poe1.league && in.poe1.tabIds == s.poe1.tabIds &&
		              in.poe2.league == s.poe2.league && in.poe2.tabIds == s.poe2.tabIds &&
		              in.autoMinutes == 120 && in.showDivine);
		rep.check("T7g token round-trips through DPAPI",
		          in.sessid == fakeToken && in.rememberSessid);

		// 「不記住」 is not just "do not write it": it must also erase what an
		// earlier save left behind. Save over the file above with the box off.
		{
			WarehouseUiState off = s;
			off.rememberSessid = false;
			rep.check("T7i state saves with remember off", off.Save(scratch, /*writeSecret=*/true));
			const std::string body2 =
			    ReadFileForTest(scratch + L"PobTools\\warehouse_ui.json");
			rep.check("T7j remember off leaves an empty blob field",
			          body2.find("\"sessidDpapi\": \"\"") != std::string::npos &&
			              body2.find(fakeToken) == std::string::npos);
			WarehouseUiState back;
			rep.check("T7k remember off loads no token",
			          back.Load(scratch) && back.sessid.empty() && !back.rememberSessid &&
			              back.accountName == s.accountName);
		}

		// The 清除 button's file side: a ticked box with nothing to remember
		// still writes the field away.
		{
			WarehouseUiState cleared = s;
			cleared.sessid.clear();
			rep.check("T7l cleared token saves", cleared.Save(scratch, /*writeSecret=*/true));
			WarehouseUiState back;
			rep.check("T7m cleared token is gone from the file",
			          back.Load(scratch) && back.sessid.empty());
		}

		// A file written before the setting existed: a blob and no flag means the
		// player had chosen to save it, so an upgrade must keep honouring that.
		{
			const std::string pre = std::string(R"({"schema":1,"accountName":"Old#2",)") +
			                        R"("sessidDpapi":")" + blob + R"("})";
			WriteFileForTest(scratch + L"PobTools\\warehouse_ui.json", pre);
			WarehouseUiState up;
			rep.check("T7n pre-setting file with a blob loads as remembered",
			          up.Load(scratch) && up.sessid == fakeToken && up.rememberSessid);
		}

		// Several panels can be open at once (the standalone tool and the atlas
		// planner's embed, this process or another), each holding the copy it
		// loaded at its own Init, and a save rewrites the whole file. So a panel
		// that saves an unrelated setting -- or simply closes -- must not write
		// its stale session id back over another panel's 清除.
		{
			WarehouseUiState clearing = s;
			clearing.sessid.clear();
			rep.check("T7p the clear lands", clearing.Save(scratch, /*writeSecret=*/true));

			WarehouseUiState stale = s; // still holds the token in memory
			stale.poe1.league = "AnotherLeague";
			rep.check("T7q a stale panel's ordinary save still writes", stale.Save(scratch));

			WarehouseUiState after;
			// The tick stays on: 清除 drops the token, it does not change the
			// player's answer to "may this be saved" -- the next paste is kept.
			rep.check("T7r an ordinary save does not resurrect a cleared token",
			          after.Load(scratch) && after.sessid.empty() && after.rememberSessid &&
			              after.poe1.league == "AnotherLeague");
		}

		// The mirror image: an ordinary save must not DROP what another panel
		// saved either, or a panel that never had a session id would quietly
		// erase one every time it wrote anything.
		{
			rep.check("T7s a remembered token saves", s.Save(scratch, /*writeSecret=*/true));
			WarehouseUiState blank; // a panel with nothing in its own copy
			blank.poe1.league = "ThirdLeague";
			rep.check("T7t a panel with no token saves", blank.Save(scratch));
			WarehouseUiState after;
			rep.check("T7u an ordinary save keeps another panel's token",
			          after.Load(scratch) && after.sessid == fakeToken &&
			              after.rememberSessid && after.poe1.league == "ThirdLeague");
		}

		std::string wipe = fakeToken;
		WarehouseWipe(wipe);
		rep.check("T7v WarehouseWipe empties the string", wipe.empty());

		// A settings file from before PoE2 support: its top-level picks are
		// PoE1's, and nothing may leak into the PoE2 side.
		const char kLegacy[] = R"({"schema":1,"accountName":"Old#1","league":"Allflame",)"
		                       R"("selectedTabIds":["t1"],"autoMinutes":0})";
		HANDLE lf = CreateFileW((scratch + L"PobTools\\warehouse_ui.json").c_str(),
		                        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
		                        FILE_ATTRIBUTE_NORMAL, nullptr);
		if (lf != INVALID_HANDLE_VALUE) {
			DWORD w = 0;
			WriteFile(lf, kLegacy, (DWORD)(sizeof(kLegacy) - 1), &w, nullptr);
			CloseHandle(lf);
		}
		WarehouseUiState old;
		rep.check("T7h pre-PoE2 file: top-level picks become PoE1's",
		          old.Load(scratch) && old.poe1.league == "Allflame" &&
		              old.poe1.tabIds.size() == 1 && old.poe2.league.empty() &&
		              old.poe2.tabIds.empty() && old.game.empty());
		rep.check("T7h2 a file with no blob and no flag does not remember",
		          !old.rememberSessid);
	}

	// ---- history round-trip, per-league files, summaries ---------------------
	{
		const std::string L = "Mercenaries";
		WarehouseHistory h;
		Snapshot s1;
		s1.utc = 1700000000;
		s1.league = L;
		s1.tabIds = { "abc123" };
		s1.totalChaos = 1234.5;
		s1.divineRate = 210.0;
		s1.lines = { { "currency|Divine Orb", "Divine Orb", "Art/x", 5, 210.0, 1050.0, true } };
		Snapshot s2 = s1;
		s2.utc += 600;
		s2.totalChaos = 1300.0;
		h.snaps = { s1, s2 };
		h.sessionStartUtc = s1.utc;
		rep.check("T8 history saves", h.Save(scratch, "poe1", L));
		WarehouseHistory in;
		rep.check("T8a history loads", in.Load(scratch, "poe1", L));
		rep.check("T8b snapshots round-trip",
		          in.snaps.size() == 2 && in.sessionStartUtc == s1.utc &&
		              in.snaps[0].lines.size() == 1 &&
		              in.snaps[0].lines[0].key == "currency|Divine Orb" &&
		              in.snaps[0].lines[0].chaosTotal == 1050.0 && in.snaps[0].lines[0].priced &&
		              !in.snaps[0].lines[0].estimated && in.snaps[1].totalChaos == 1300.0);
		const std::string body = ReadFileForTest(WarehouseHistory::PathOf(scratch, "poe1", L));
		rep.check("T8c saved compact (no indentation)",
		          !body.empty() && body.find('\n') == std::string::npos);

		// One file per league and per game: saving one leaves the others alone.
		WarehouseHistory other;
		Snapshot os;
		os.utc = 1700005555;
		os.league = "Standard";
		os.totalChaos = 9.0;
		other.snaps = { os };
		WarehouseHistory p2;
		Snapshot ps;
		ps.utc = 1700009999;
		ps.league = "Forbidden Rites";
		ps.totalChaos = 5.0;
		p2.snaps = { ps };
		rep.check("T8d another league / another game saves",
		          other.Save(scratch, "poe1", "Standard") &&
		              p2.Save(scratch, "poe2", "Forbidden Rites"));
		WarehouseHistory back1, back2, back3;
		rep.check("T8e each league keeps its own file",
		          back1.Load(scratch, "poe1", L) && back1.snaps.size() == 2 &&
		              back2.Load(scratch, "poe1", "Standard") && back2.snaps.size() == 1 &&
		              back3.Load(scratch, "poe2", "Forbidden Rites") && back3.snaps.size() == 1 &&
		              back3.snaps[0].league == "Forbidden Rites");

		rep.check("T8f plain league names stay readable in the file name",
		          WarehouseHistory::LeagueFileStem("Mercenaries") == "Mercenaries" &&
		              WarehouseHistory::LeagueFileStem("Hardcore Mercenaries") ==
		                  "Hardcore_Mercenaries");
		const std::string n1 = WarehouseHistory::LeagueFileStem(u8"私人聯盟 (PL123)");
		const std::string n2 = WarehouseHistory::LeagueFileStem(u8"私人聯盟 (PL124)");
		bool asciiOnly = !n1.empty();
		for (char c : n1)
			if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
			      c == '-' || c == '_'))
				asciiOnly = false;
		rep.check("T8g other names: ASCII only, and hashed apart", asciiOnly && n1 != n2,
		          n1 + " / " + n2);
		rep.check("T8h 'A B' and 'A_B' never share a file",
		          WarehouseHistory::LeagueFileStem("A B") != WarehouseHistory::LeagueFileStem("A_B"));

		// Prune: 1100 snapshots -> 1000; the newest 20 and the session start keep
		// their lines, everything older is thinned to its totals.
		WarehouseHistory big;
		const long long now = 1700000000;
		for (int i = 0; i < 1100; i++) {
			Snapshot s;
			s.utc = now - (long long)(1100 - i) * 600;
			s.totalChaos = i;
			s.lines = { { "currency|Chaos Orb", "Chaos Orb", "", (long long)i, 1.0, (double)i, true } };
			big.snaps.push_back(std::move(s));
		}
		big.sessionStartUtc = big.snaps[0].utc;
		const long long startUtc = big.sessionStartUtc;
		big.Prune();
		int full = 0;
		bool startFull = false, newestFull = true, ascending = true;
		const Snapshot* aSummary = nullptr;
		for (size_t i = 0; i < big.snaps.size(); i++) {
			const Snapshot& s = big.snaps[i];
			if (!s.summary) full++;
			else if (!aSummary) aSummary = &s;
			if (s.utc == startUtc) startFull = !s.summary && s.lines.size() == 1;
			if (i + WarehouseHistory::kFullKept >= big.snaps.size() && s.summary) newestFull = false;
			if (i > 0 && big.snaps[i - 1].utc > s.utc) ascending = false;
		}
		rep.check("T8i prune caps at 1000", big.snaps.size() == 1000,
		          "kept " + std::to_string(big.snaps.size()));
		rep.check("T8j the session start survives, with its lines", startFull);
		rep.check("T8k the newest 20 keep their lines, older ones are summaries",
		          newestFull && full == 21, "full " + std::to_string(full));
		rep.check("T8l still utc-ascending", ascending);
		rep.check("T8m a summary keeps its totals, drops its lines",
		          aSummary && aSummary->lines.empty() && aSummary->lineCount == 1 &&
		              aSummary->totalChaos > 0);
		WarehouseHistory bigBack;
		const bool saved = big.Save(scratch, "poe1", "Prune") && bigBack.Load(scratch, "poe1", "Prune");
		int summaries = 0;
		for (const Snapshot& s : bigBack.snaps)
			if (s.summary && s.lineCount == 1 && s.lines.empty()) summaries++;
		rep.check("T8n summaries round-trip", saved && bigBack.snaps.size() == 1000 && summaries == 979,
		          std::to_string(summaries));
	}

	// ---- 403 classification ----------------------------------------------
	{
		// GGG's real "Permission Denied" page carries Cloudflare's JS-detection
		// script (challenge-platform/scripts/jsd) like every page on the site.
		// It once read as "blocked by Cloudflare" while the session had simply
		// expired (2026-09-11).
		const char kGggDenied[] =
		    "<!DOCTYPE html><html lang=\"en\"><head><title>Path of Exile</title></head>"
		    "<body><h2>Permission Denied</h2><p>You don't have permission to access this "
		    "area.</p><script>var a=document.createElement('script');"
		    "a.src='/cdn-cgi/challenge-platform/scripts/jsd/main.js';</script></body></html>";
		const char kCfChallenge[] =
		    "<!DOCTYPE html><html lang=\"en-US\"><head><title>Just a moment...</title></head>"
		    "<body><div id=\"cf-chl-widget\"></div></body></html>";
		const char kOtherHtml[] =
		    "<html><head><title>Maintenance</title></head><body>Down</body></html>";
		std::string e;
		rep.check("T9 GGG Permission Denied page -> Auth (not Cloudflare)",
		          ClassifyStash403(kGggDenied, &e) == StashError::Auth, e);
		rep.check("T9a real Cloudflare challenge -> Blocked",
		          ClassifyStash403(kCfChallenge, &e) == StashError::Blocked, e);
		rep.check("T9b JSON 403 -> Auth",
		          ClassifyStash403(R"({"error":{"code":6,"message":"Forbidden"}})", &e) ==
		              StashError::Auth,
		          e);
		const bool other = ClassifyStash403(kOtherHtml, &e) == StashError::Forbidden;
		rep.check("T9c other HTML -> Forbidden, title in message",
		          other && e.find("Maintenance") != std::string::npos, e);
		const std::string why =
		    StashApiErrorText(R"({"error":{"code":2,"message":"Invalid query"}})");
		rep.check("T9d GGG JSON error surfaced as text", why == "Invalid query (code 2)", why);
		rep.check("T9e non-JSON body yields no text", StashApiErrorText("<html></html>").empty());
	}

	// ---- atlas map cost ---------------------------------------------------
	{
		const std::vector<MapCostInput> slots = {
			{ "S/Abyss", "Abyss Scarab", u8"深淵聖甲蟲", "Art/a", true },
			{ "S/Abyss", "Abyss Scarab", u8"深淵聖甲蟲", "Art/a", true },
			{ "S/Rare", "Rare Scarab", u8"稀有聖甲蟲", "Art/r", true },   // low confidence
			{ "S/None", "Unlisted Scarab", u8"無價聖甲蟲", "Art/n", true }, // no price at all
			{ "F/Gone", "Gone Fragment", u8"消失碎片", "Art/g", false },   // untradable
		};
		auto lookup = [](const std::string& k, NinjaPrice* o) {
			if (k == "currency|Abyss Scarab") {
				o->chaos = 12.0;
				return true;
			}
			if (k == "currency|Rare Scarab") {
				o->chaos = 99.0;
				o->lowConfidence = true;
				return true;
			}
			return false;
		};
		const MapCostSummary c = ComputeMapCost(slots, 50.0, lookup);
		rep.check("T10 map cost folds repeats, keeps placement order",
		          c.lines.size() == 4 && c.lines[0].id == "S/Abyss" && c.lines[0].qty == 2);
		rep.check("T10a priced line total", c.lines.size() == 4 && c.lines[0].priced &&
		                                        c.lines[0].chaosTotal == 24.0);
		rep.check("T10b low confidence + missing are unpriced, untradable counted apart",
		          c.unpricedKinds == 2 && c.untradableKinds == 1);
		rep.check("T10c manual map price counted", c.mapIncluded && c.totalChaos == 74.0);
		const MapCostSummary c0 = ComputeMapCost(slots, 0.0, lookup);
		rep.check("T10d no manual map price -> map left out",
		          !c0.mapIncluded && c0.totalChaos == 24.0);

		// What the player paid wins over the market (kept for reference), and a
		// typed price makes even an untradable item count.
		const std::map<std::string, double> basis = { { "S/Abyss", 10.0 }, { "F/Gone", 3.0 } };
		const MapCostSummary cb = ComputeMapCost(slots, 0.0, lookup, &basis);
		rep.check("T10e recorded price overrides the market, market kept for reference",
		          cb.lines.size() == 4 && cb.lines[0].fromBasis && cb.lines[0].chaosEach == 10.0 &&
		              cb.lines[0].marketEach == 12.0 && cb.lines[0].chaosTotal == 20.0);
		rep.check("T10f a typed price makes an untradable item count",
		          cb.lines.size() == 4 && cb.lines[3].priced && cb.untradableKinds == 0 &&
		              cb.unpricedKinds == 2 && cb.totalChaos == 23.0);
	}

	// ---- saved league, read without DPAPI --------------------------------
	{
		WarehouseUiState s;
		s.poe1.league = "Allflame";
		s.Save(scratch);
		rep.check("T11 the cost card can read the panel's league",
		          WarehouseSavedLeague(scratch) == "Allflame");
		const char kLegacyLeague[] = R"({"schema":1,"league":"Settlers"})";
		HANDLE lf = CreateFileW((scratch + L"PobTools\\warehouse_ui.json").c_str(), GENERIC_WRITE,
		                        0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (lf != INVALID_HANDLE_VALUE) {
			DWORD w = 0;
			WriteFile(lf, kLegacyLeague, (DWORD)(sizeof(kLegacyLeague) - 1), &w, nullptr);
			CloseHandle(lf);
		}
		rep.check("T11a pre-PoE2 file's top-level league", WarehouseSavedLeague(scratch) == "Settlers");
	}

	// ---- two panels on one history file ------------------------------------
	{
		const std::string L = "Mercenaries";
		DeleteFileW(WarehouseHistory::PathOf(scratch, "poe1", L).c_str());
		WarehouseHistory a, b; // b loads before a's shot, so it is stale
		a.Load(scratch, "poe1", L);
		b.Load(scratch, "poe1", L);
		Snapshot s1;
		s1.utc = 1800000000;
		s1.league = L;
		s1.totalChaos = 1.0;
		Snapshot s2;
		s2.utc = 1800000600;
		s2.league = L;
		s2.totalChaos = 2.0;
		rep.check("T12 first panel appends", a.AppendAndSave(scratch, "poe1", L, s1));
		rep.check("T12a stale second panel appends", b.AppendAndSave(scratch, "poe1", L, s2));
		WarehouseHistory disk;
		disk.Load(scratch, "poe1", L);
		rep.check("T12b both snapshots survive", disk.snaps.size() == 2 &&
		                                             disk.snaps[0].utc == s1.utc &&
		                                             disk.snaps[1].utc == s2.utc);
		rep.check("T12c the writer's copy equals the file", b.snaps.size() == 2);
		rep.check("T12d the first ever shot starts the session", disk.sessionStartUtc == s1.utc);
		rep.check("T12e set-start is read-modify-write too",
		          a.SetSessionStartAndSave(scratch, "poe1", L, s2.utc) && a.snaps.size() == 2 &&
		              a.sessionStartUtc == s2.utc);
		rep.check("T12f the same shot is not appended twice",
		          b.AppendAndSave(scratch, "poe1", L, s2) && b.snaps.size() == 2);
		// A panel holding this league's history appends a shot of another league
		// (the picker moved mid-fetch): only that league's file changes.
		const std::string L2 = "Selftest Other";
		DeleteFileW(WarehouseHistory::PathOf(scratch, "poe1", L2).c_str());
		Snapshot s3;
		s3.utc = 1800001200;
		s3.league = L2;
		WarehouseHistory holder = b; // holds L's two snapshots
		WarehouseHistory lDisk, l2Disk;
		rep.check("T12g a shot lands in its own league's file, nothing else leaks",
		          holder.AppendAndSave(scratch, "poe1", L2, s3) &&
		              l2Disk.Load(scratch, "poe1", L2) && l2Disk.snaps.size() == 1 &&
		              lDisk.Load(scratch, "poe1", L) && lDisk.snaps.size() == 2);
		// A summary cannot become the session start: nothing to diff against.
		Snapshot sm;
		sm.utc = 1800001800;
		sm.league = L;
		sm.summary = true;
		sm.lineCount = 3;
		WarehouseHistory withSummary;
		withSummary.Load(scratch, "poe1", L);
		withSummary.snaps.push_back(sm);
		withSummary.Save(scratch, "poe1", L);
		rep.check("T12h a summary is refused as the session start",
		          !withSummary.SetSessionStartAndSave(scratch, "poe1", L, sm.utc) &&
		              withSummary.sessionStartUtc == s2.utc);
	}

	// ---- snapshot-card money ("14d 64c", the Wealthy-Exile format) ---------
	{
		const std::string a = WhFmt::FormatDivChaos(14 * 200.0 + 64.0, 200.0, true);
		rep.check("T13 whole divines + leftover chaos", a == "+14d 64c", a);
		rep.check("T13a zero", WhFmt::FormatDivChaos(0.0, 200.0, false) == "0d 0c");
		const std::string b = WhFmt::FormatDivChaos(-182.0, 200.0, true);
		rep.check("T13b sub-divine negative", b == "-0d 182c", b);
		const std::string c = WhFmt::FormatDivChaos(399.7, 200.0, false);
		rep.check("T13c rounding carries a whole divine", c == "2d 0c", c);
		rep.check("T13d no rate: chaos only", WhFmt::FormatDivChaos(326.0, 0.0, true) == "+326c");
	}

	// ---- one-time move to per-league files ---------------------------------
	{
		// A pre-split file: two leagues, the session start in the second one.
		CreateDirectoryW((scratch + L"PobTools").c_str(), nullptr);
		const std::wstring legacy = scratch + L"PobTools\\warehouse_poe1.json";
		const std::wstring done = legacy + L".migrated";
		DeleteFileW(done.c_str());
		DeleteFileW(WarehouseHistory::PathOf(scratch, "poe1", "Settlers").c_str());
		DeleteFileW(WarehouseHistory::PathOf(scratch, "poe1", "Mercenaries").c_str());
		const char kLegacyHist[] =
		    R"({"schema":1,"sessionStartUtc":1700000600,"snaps":[)"
		    R"({"utc":1700000000,"league":"Settlers","tabIds":["a"],"totalChaos":10,"divineRate":200,)"
		    R"("unpricedKinds":0,"lines":[{"k":"currency|Chaos Orb","en":"Chaos Orb","ic":"","n":10,)"
		    R"("each":1,"sum":10,"p":true,"est":false}]},)"
		    R"({"utc":1700000600,"league":"Mercenaries","tabIds":["a"],"totalChaos":20,"lines":[]},)"
		    R"({"utc":1700001200,"league":"Mercenaries","tabIds":["a"],"totalChaos":30,"lines":[]}]})";
		WriteFileForTest(legacy, kLegacyHist);
		// Mercenaries already has a file of its own, sharing one snapshot.
		WarehouseHistory pre;
		Snapshot dup;
		dup.utc = 1700001200;
		dup.league = "Mercenaries";
		dup.totalChaos = 30.0;
		Snapshot extra;
		extra.utc = 1700002000;
		extra.league = "Mercenaries";
		extra.totalChaos = 40.0;
		pre.snaps = { dup, extra };
		pre.Save(scratch, "poe1", "Mercenaries");

		const int moved = WarehouseHistory::MigrateLegacy(scratch, "poe1");
		rep.check("T14 migration moves the snapshots not already there", moved == 2,
		          std::to_string(moved));
		WarehouseHistory se, me;
		se.Load(scratch, "poe1", "Settlers");
		me.Load(scratch, "poe1", "Mercenaries");
		rep.check("T14a split by league, lines kept",
		          se.snaps.size() == 1 && se.snaps[0].lines.size() == 1);
		rep.check("T14b merged without duplicates",
		          me.snaps.size() == 3 && me.snaps[0].utc == 1700000600 &&
		              me.snaps[2].utc == 1700002000);
		rep.check("T14c the session start went with its league; the other starts at its first shot",
		          me.sessionStartUtc == 1700000600 && se.sessionStartUtc == 1700000000);
		rep.check("T14d the old file is renamed, not deleted",
		          GetFileAttributesW(legacy.c_str()) == INVALID_FILE_ATTRIBUTES &&
		              GetFileAttributesW(done.c_str()) != INVALID_FILE_ATTRIBUTES);
		rep.check("T14e a second run has nothing to do",
		          WarehouseHistory::MigrateLegacy(scratch, "poe1") == 0);
		WriteFileForTest(legacy, "{not json");
		rep.check("T14f an unreadable old file is left alone",
		          WarehouseHistory::MigrateLegacy(scratch, "poe1") < 0 &&
		              GetFileAttributesW(legacy.c_str()) != INVALID_FILE_ATTRIBUTES);
		DeleteFileW(legacy.c_str());
	}

	// ---- 收益紀錄: a stretch bound to an atlas project ----------------------
	{
		Snapshot a, b; // T5's pair
		a.utc = 1000000;
		b.utc = a.utc + 7200;
		a.league = b.league = "Mercenaries";
		b.divineRate = 200.0;
		a.lines = { { "currency|Chaos Orb", "Chaos Orb", "", 100, 1.0, 100.0, true },
			        { "currency|Divine Orb", "Divine Orb", "", 10, 200.0, 2000.0, true } };
		a.totalChaos = 2100.0;
		b.lines = { { "card|The Doctor", "The Doctor", "", 1, 500.0, 500.0, true },
			        { "currency|Divine Orb", "Divine Orb", "", 12, 200.0, 2400.0, true } };
		b.totalChaos = 2900.0;
		auto zh = [](const std::string& en) {
			return en == "The Doctor" ? std::string(u8"博士") : en;
		};
		const AtlasProfitRecord r = MakeProfitRecord(a, b, zh, 36.456);
		rep.check("T15 record totals by cause",
		          r.qtyGain == 900.0 && r.qtyLoss == -100.0 && r.priceMove == 0.0 && r.net == 800.0 &&
		              r.hours == 2.0 && r.league == "Mercenaries" && r.divineRate == 200.0);
		rep.check("T15a top gains by count value, display names stored",
		          r.top.size() == 2 && r.top[0].en == "The Doctor" && r.top[0].zh == u8"博士" &&
		              r.top[0].chaos == 500.0 && r.top[1].en == "Divine Orb" && r.top[1].dCount == 2);
		rep.check("T15b cost per map rounded to a cent", r.costPerMap == 36.46);
		Snapshot sum = a;
		sum.summary = true;
		sum.lines.clear();
		rep.check("T15c a summary cannot be bound", MakeProfitRecord(sum, b, zh, 0.0).empty());
		rep.check("T15d the end must come after the start", MakeProfitRecord(b, a, zh, 0.0).empty());
	}

	// ---- stale poe.ninja caches ---------------------------------------------
	{
		const std::wstring dir = scratch + L"PobTools\\cache\\ninja\\";
		CreateDirectoryW((scratch + L"PobTools").c_str(), nullptr);
		CreateDirectoryW((scratch + L"PobTools\\cache").c_str(), nullptr);
		CreateDirectoryW(dir.c_str(), nullptr);
		FILETIME nowFt{};
		GetSystemTimeAsFileTime(&nowFt);
		auto touch = [&](const wchar_t* name, int ageDays) {
			HANDLE f = CreateFileW((dir + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
			                       FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE) return;
			ULARGE_INTEGER t;
			t.LowPart = nowFt.dwLowDateTime;
			t.HighPart = nowFt.dwHighDateTime;
			t.QuadPart -= (unsigned long long)ageDays * 86400ull * 10000000ull;
			FILETIME ft;
			ft.dwLowDateTime = t.LowPart;
			ft.dwHighDateTime = t.HighPart;
			SetFileTime(f, nullptr, nullptr, &ft);
			CloseHandle(f);
		};
		touch(L"poe1_Settlers.json", 40);             // a finished league
		touch(L"poe1_Settlers.json.tmp123_456", 40);  // a crashed writer's leftover
		touch(L"poe1_Mercenaries.json", 0);           // the league in use
		touch(L"notes.json", 40);                     // not this cache's file
		auto exists = [&](const wchar_t* n) {
			return GetFileAttributesW((dir + n).c_str()) != INVALID_FILE_ATTRIBUTES;
		};
		const int removed = PruneNinjaCache(scratch, UnixOfForTest(nowFt));
		rep.check("T16 a finished league's cache (and its leftover) is removed",
		          removed >= 2 && !exists(L"poe1_Settlers.json") &&
		              !exists(L"poe1_Settlers.json.tmp123_456"),
		          std::to_string(removed));
		rep.check("T16a the league in use and foreign files are kept",
		          exists(L"poe1_Mercenaries.json") && exists(L"notes.json"));
		DeleteFileW((dir + L"notes.json").c_str());
		DeleteFileW((dir + L"poe1_Mercenaries.json").c_str());
	}

	// ---- request guard, shared by every panel and process -----------------
	{
		const std::wstring guardFile = scratch + L"PobTools\\warehouse_guard.json";
		DeleteFileW(guardFile.c_str());
		const long long t = 1900000000;
		long long next = 0;
		rep.check("T17 the first snapshot claim is granted",
		          WarehouseClaimSnapshot(scratch, t, &next) && next == t + 600);
		rep.check("T17a a second claim inside 10 minutes is refused, saying when",
		          !WarehouseClaimSnapshot(scratch, t + 100, &next) && next == t + 600);
		rep.check("T17b granted again once the cooldown has passed",
		          WarehouseClaimSnapshot(scratch, t + 600, &next));
		WarehouseNoteBlocked(scratch, t + 2000);
		WarehouseNoteBlocked(scratch, t + 1500); // an earlier end never shortens a pause
		const WarehouseGuard g = WarehouseGuardLoad(scratch);
		rep.check("T17c a server pause is kept at its latest end",
		          g.blockedUntilUtc == t + 2000 && g.lastSnapshotStartUtc == t + 600);
		rep.check("T17d the pause outranks the cooldown",
		          WarehouseNextSnapshotUtc(g) == t + 2000 &&
		              !WarehouseClaimSnapshot(scratch, t + 1300, &next) && next == t + 2000);
		DeleteFileW(guardFile.c_str());

		// Auto: counts from the later of opening and the newest snapshot since.
		const WarehouseGuard none{};
		rep.check("T17e auto: nothing since opening -> opening + interval",
		          WarehouseAutoDueUtc(t - 5000, t, 60, none) == t + 3600);
		rep.check("T17f auto: a snapshot since opening -> that snapshot + interval",
		          WarehouseAutoDueUtc(t + 1000, t, 60, none) == t + 1000 + 3600);
		WarehouseGuard hold;
		hold.blockedUntilUtc = t + 99999;
		rep.check("T17g auto: never before the guard allows",
		          WarehouseAutoDueUtc(t + 1000, t, 60, hold) == t + 99999);
		rep.check("T17h auto: the interval scales the wait; off = 0",
		          WarehouseAutoDueUtc(t, t, 180, none) == t + 3 * 3600 &&
		              WarehouseAutoDueUtc(t, t, 0, none) == 0);
		rep.check("T17i saved intervals: under an hour = off, else the nearest hour up to 6",
		          WarehouseNormalizeAutoMinutes(10) == 0 && WarehouseNormalizeAutoMinutes(0) == 0 &&
		              WarehouseNormalizeAutoMinutes(60) == 60 &&
		              WarehouseNormalizeAutoMinutes(90) == 120 &&
		              WarehouseNormalizeAutoMinutes(500) == 360);
	}

	PobLog::SetDirForTest(L"");
	rep.text += rep.failures == 0
	                ? "\nALL PASS (" + std::to_string(rep.checks) + " checks)\n"
	                : "\nFAILURES: " + std::to_string(rep.failures) + " / " +
	                      std::to_string(rep.checks) + "\n";
	PrintAndSave(exeDir, rep.text, L"warehouse_selftest.txt");
	return rep.failures == 0 ? 0 : 1;
}

int RunWarehouseProbe(const std::wstring& exeDir, const std::wstring& realmArg,
                      const std::wstring& leagueArg)
{
	auto utf8 = [](const std::wstring& w) {
		if (w.empty()) return std::string();
		int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
		                            nullptr, nullptr);
		std::string s(n, '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr,
		                    nullptr);
		return s;
	};
	const std::string realm = realmArg.empty() ? std::string("pc") : utf8(realmArg);
	std::string out;
	auto finish = [&](int code) {
		PrintAndSave(exeDir, out, L"warehouse_probe.txt");
		return code;
	};

	WarehouseUiState st;
	st.Load(exeDir);
	if (st.accountName.empty() || st.sessid.empty()) {
		out += u8"倉庫收益線上探針：尚未設定。\n"
		       u8"先開啟工具（--warehouse）輸入帳號名與 POESESSID 並按「驗證 session」，"
		       u8"或至少讓設定檔存在後再跑。\n"
		       u8"若設定裡沒有勾「記住」，session id 不會留在檔案裡，這個探針就讀不到。\n";
		return finish(2);
	}
	// This report gets pasted into bug threads. It never held the session id;
	// the account name is masked too, because the report says nothing that
	// needs it.
	auto maskAccount = [](const std::string& name) {
		std::string out;
		size_t kept = 0;
		for (size_t i = 0; i < name.size(); i++) {
			const unsigned char c = (unsigned char)name[i];
			if (c == '#') break;
			// Two leading characters stay, so the line still identifies WHICH
			// account was probed for the person who ran it. ASCII only: half a
			// UTF-8 sequence is not a character.
			if (kept < 2 && c < 0x80) {
				out += (char)c;
				kept++;
			}
		}
		return out + "***#****";
	};
	out += u8"帳號: " + maskAccount(st.accountName) + u8"  realm: " + realm + "\n";

	StashAuth auth;
	auth.accountName = st.accountName;
	auth.secret = st.sessid;
	auto provider = CreateSessidStashProvider(auth, realm);

	std::string err;
	StashError kind = StashError::None;
	std::vector<std::string> charLeagues;
	DWORD t0 = GetTickCount();
	if (!provider->Verify(&err, &kind, nullptr, &charLeagues)) {
		out += u8"Verify: 失敗 [" + std::to_string((int)kind) + "] " + err + "\n";
		return finish(1);
	}
	std::string joined;
	for (const std::string& l : charLeagues) joined += (joined.empty() ? "" : ", ") + l;
	out += u8"Verify: OK（" + std::to_string(GetTickCount() - t0) + u8" ms），角色所在聯盟: " +
	       (joined.empty() ? std::string(u8"（無角色）") : joined) + "\n";

	// League candidates: explicit arg > the leagues this realm has characters in
	// > the saved league (PoE1 only -- a PoE1 league means nothing on poe2) >
	// Standard.
	std::vector<std::string> candidates;
	auto add = [&](const std::string& l) {
		if (!l.empty() && std::find(candidates.begin(), candidates.end(), l) == candidates.end())
			candidates.push_back(l);
	};
	if (!leagueArg.empty()) {
		add(utf8(leagueArg));
	} else {
		for (const std::string& l : charLeagues) add(l);
		add(realm == "pc" ? st.poe1.league : st.poe2.league);
		add("Standard");
	}

	std::vector<StashTabInfo> tabs;
	std::string league;
	for (const std::string& cand : candidates) {
		std::vector<StashTabInfo> t;
		t0 = GetTickCount();
		if (provider->ListTabs(cand, t, &err, &kind, nullptr)) {
			out += u8"ListTabs [" + cand + u8"]: OK，" + std::to_string(t.size()) +
			       u8" 個分頁（" + std::to_string(GetTickCount() - t0) + " ms）\n";
			tabs = std::move(t);
			league = cand;
			break;
		}
		out += u8"ListTabs [" + cand + u8"]: 失敗 [" + std::to_string((int)kind) + "] " + err +
		       "\n";
		// Not "wrong league" answers: more candidates would only burn rate limit.
		if (kind == StashError::Auth || kind == StashError::Blocked ||
		    kind == StashError::RateLimited)
			break;
	}
	if (tabs.empty()) {
		out += u8"結論: realm=" + realm + u8" 取不到任何倉庫分頁。\n";
		return finish(1);
	}
	for (size_t i = 0; i < tabs.size() && i < 8; i++)
		out += "  #" + std::to_string(tabs[i].index) + " " + tabs[i].name + " [" +
		       tabs[i].type + "]\n";
	if (tabs.size() > 8) out += "  ...\n";

	// One real tab fetch: proves items come back and shows their JSON shape.
	// A currency tab is the most informative (stackables, the pricing core).
	const StashTabInfo* pick = &tabs[0];
	for (const StashTabInfo& t : tabs)
		if (t.type == "CurrencyStash") { pick = &t; break; }
	std::vector<StashItemRaw> items;
	t0 = GetTickCount();
	if (!provider->FetchTab(league, pick->index, items, &err, &kind, nullptr)) {
		out += u8"FetchTab #" + std::to_string(pick->index) + u8": 失敗 [" +
		       std::to_string((int)kind) + "] " + err + "\n";
		return finish(1);
	}
	int keyed = 0;
	for (const StashItemRaw& it : items) {
		std::string k, d;
		if (BuildPriceKey(it, &k, &d)) keyed++;
	}
	out += u8"FetchTab #" + std::to_string(pick->index) + " " + pick->name + " [" + pick->type +
	       u8"]: " + std::to_string(items.size()) + u8" 件物品，" + std::to_string(keyed) +
	       u8" 件可建 price-key（" + std::to_string(GetTickCount() - t0) + " ms）\n";
	for (size_t i = 0; i < items.size() && i < 12; i++) {
		const StashItemRaw& it = items[i];
		std::string k, d;
		const bool ok = BuildPriceKey(it, &k, &d);
		out += "  [frame " + std::to_string(it.frameType) + "] " +
		       (it.name.empty() ? std::string() : it.name + " / ") + it.typeLine + " x" +
		       std::to_string(it.stackSize) + "  -> " + (ok ? k : std::string(u8"(不可估)")) +
		       "\n";
	}
	// A realm the server does not know is silently IGNORED -- it answers for
	// PoE1. Getting tabs back therefore proves nothing by itself: compare with
	// the same league on pc; identical tab ids mean the realm was dropped.
	// (2026-09-11: realm=poe2 returned the PoE1 stash verbatim, and the first
	// version of this probe reported "PoE2 stash available".)
	if (realm != "pc") {
		Sleep(1000); // a second provider has a fresh throttle; stay polite
		auto pcProvider = CreateSessidStashProvider(auth, "pc");
		std::vector<StashTabInfo> pcTabs;
		if (pcProvider->ListTabs(league, pcTabs, &err, &kind, nullptr)) {
			bool same = pcTabs.size() == tabs.size();
			for (size_t i = 0; same && i < tabs.size(); i++) same = pcTabs[i].id == tabs[i].id;
			if (same) {
				out += u8"結論: realm=" + realm + u8" 被伺服器忽略——回傳的 " +
				       std::to_string(tabs.size()) + u8" 個分頁 id 與 PoE1 完全相同，這不是 " +
				       realm + u8" 的倉庫。\n";
				return finish(3);
			}
			out += u8"對照: PoE1 同聯盟的分頁與此不同，realm 參數有作用。\n";
		} else {
			out += u8"對照: PoE1 沒有「" + league + u8"」的倉庫（" + err + u8"），無法比對。\n";
		}
	}
	out += u8"結論: realm=" + realm + u8" 倉庫可取得。\n";
	return finish(0);
}
