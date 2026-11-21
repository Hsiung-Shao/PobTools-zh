// --boss-profit-selftest: the boss-kill profit calculator (boss_profit.h), headless.
//
// Prices come from a local map (the only mocked I/O); the calculator itself is never mocked.
// T13 reads the shipped Data\boss_drops_poe1.json next to the exe. No network, no files written
// except the report itself.
#include "filter_i18n.h"
#include "boss_profit.h"
#include "boss_profit_tool.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
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

void PrintAndSave(const std::wstring& exeDir, const std::string& rep, const wchar_t* fileName)
{
	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	printf("%s", rep.c_str());
	HANDLE h = CreateFileW((exeDir + fileName).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, rep.data(), (DWORD)rep.size(), &w, nullptr);
		CloseHandle(h);
	}
}

bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }
std::string D(double v) { return std::to_string(v); }

using PriceMap = std::map<std::string, NinjaPrice>;
using Lookup = std::function<bool(const std::string&, NinjaPrice*)>;

// A price table standing in for the ninja cache: true + the entry, or false when absent.
Lookup MakeLookup(PriceMap m)
{
	return [m](const std::string& key, NinjaPrice* out) {
		auto it = m.find(key);
		if (it == m.end()) return false;
		*out = it->second;
		return true;
	};
}

NinjaPrice P(double chaos, bool low = false)
{
	NinjaPrice p;
	p.chaos = chaos;
	p.lowConfidence = low;
	return p;
}

BossDrop Drop(const char* key, const char* en, double rate)
{
	BossDrop d;
	d.priceKey = key;
	d.en = en;
	d.ratePct = rate;
	return d;
}

BossTicket Ticket(const char* key, const char* en, double qty = 1.0)
{
	BossTicket t;
	t.priceKey = key;
	t.en = en;
	t.qty = qty;
	return t;
}

Boss MakeBoss(const char* key, std::vector<BossVariant> variants)
{
	Boss b;
	b.key = key;
	b.en = key;
	b.variants = std::move(variants);
	return b;
}

BossVariant MakeVariant(const char* name, std::vector<BossDrop> drops,
                        std::vector<BossTicket> tickets = {})
{
	BossVariant v;
	v.name = name;
	v.drops = std::move(drops);
	v.tickets = std::move(tickets);
	return v;
}

// The shared fixture of T1/T2/T10: 15% x 2000 + 8% x 500 = 340 revenue, tickets 100 + 70.
Boss RevenueBoss(bool withTickets)
{
	std::vector<BossTicket> tk;
	if (withTickets) tk = {Ticket("currency|Fragment A", "Fragment A"),
	                       Ticket("currency|Fragment B", "Fragment B")};
	return MakeBoss("Test Boss", {MakeVariant("Regular",
	                                          {Drop("unique|Alpha", "Alpha", 15.0),
	                                           Drop("card|Beta", "Beta", 8.0)},
	                                          tk)});
}

PriceMap RevenuePrices()
{
	return {{"unique|Alpha", P(2000)},
	        {"card|Beta", P(500)},
	        {"currency|Fragment A", P(100)},
	        {"currency|Fragment B", P(70)}};
}

bool LoadFails(const std::string& json, std::string* err = nullptr)
{
	BossDb db;
	std::string e;
	const bool ok = LoadBossDb(json, &db, &e);
	if (err) *err = e;
	return !ok && !e.empty() && db.bosses.empty();
}

// One boss wrapped in the file envelope; `bossBody` replaces everything inside the boss object.
std::string OneBoss(const std::string& variantsJson, const std::string& keyJson = "\"B\"")
{
	return R"({"schema":1,"source":"s","generatedUtc":1,"bosses":[{"key":)" + keyJson +
	       R"(,"en":"B","zh":"","variants":)" + variantsJson + "}]}";
}

std::string Variant(const std::string& dropsJson)
{
	return R"([{"name":"Regular","uber":false,"tickets":[],"drops":)" + dropsJson + "}]";
}

} // namespace

int RunBossProfitSelftest(const std::wstring& exeDir)
{
	Report rep;

	// T1 expected revenue = sum(rate x price)
	{
		const Boss b = RevenueBoss(false);
		const BossResult r = ComputeBossVariant(b, b.variants[0], MakeLookup(RevenuePrices()), {});
		rep.check(u8"T1a 兩項掉落皆有價時 revenue = 15%×2000 + 8%×500 = 340", Near(r.revenue, 340.0),
		          D(r.revenue));
		rep.check(u8"T1b 逐列 value 依序為 300 與 40、unit 為市價、來源 Market",
		          r.drops.size() == 2 && Near(r.drops[0].value, 300.0) && Near(r.drops[1].value, 40.0) &&
		              Near(r.drops[0].unit, 2000.0) && r.drops[0].src == BossPriceSrc::Market &&
		              r.drops[1].src == BossPriceSrc::Market && r.drops[0].en == "Alpha" &&
		              r.drops[1].en == "Beta",
		          r.drops.size() == 2 ? D(r.drops[0].value) + "/" + D(r.drops[1].value) : "size");
		rep.check(u8"T1c 全部有價時 unpricedDrops = 0", r.unpricedDrops == 0, std::to_string(r.unpricedDrops));
	}

	// T2 profit = revenue - tickets, ROI
	{
		const Boss b = RevenueBoss(true);
		const BossResult r = ComputeBossVariant(b, b.variants[0], MakeLookup(RevenuePrices()), {});
		rep.check(u8"T2a 兩種門票各一張 100 與 70 → ticketCost = 170", Near(r.ticketCost, 170.0),
		          D(r.ticketCost));
		rep.check(u8"T2b 收益 340 扣門票 170 → profit = 170", Near(r.profit, 170.0), D(r.profit));
		rep.check(u8"T2c 有門票且全有價 → hasTicket、hasRoi，roiPct = 100",
		          r.hasTicket && r.hasRoi && Near(r.roiPct, 100.0) && !r.ticketIncomplete,
		          D(r.roiPct));
		rep.check(u8"T2d 門票列依序、value = qty×unit",
		          r.tickets.size() == 2 && Near(r.tickets[0].value, 100.0) &&
		              Near(r.tickets[1].value, 70.0) && r.tickets[0].src == BossPriceSrc::Market);
	}

	// T3 no tickets: revenue only
	{
		const Boss b = RevenueBoss(false);
		const BossResult r = ComputeBossVariant(b, b.variants[0], MakeLookup(RevenuePrices()), {});
		rep.check(u8"T3a 無門票 → hasTicket false、ticketCost 0、不標不完整",
		          !r.hasTicket && Near(r.ticketCost, 0.0) && !r.ticketIncomplete);
		rep.check(u8"T3b 無門票 → profit = revenue 且不算 ROI",
		          Near(r.profit, r.revenue) && Near(r.profit, 340.0) && !r.hasRoi, D(r.profit));
	}

	// T4 low-confidence quote is not counted
	{
		const Boss b = MakeBoss("B", {MakeVariant("Regular", {Drop("unique|Low", "Low", 10.0),
		                                                      Drop("unique|Good", "Good", 50.0)})});
		const BossResult r = ComputeBossVariant(
		    b, b.variants[0], MakeLookup({{"unique|Low", P(9999, true)}, {"unique|Good", P(100)}}), {});
		rep.check(u8"T4a 低信度報價不進 revenue（只剩 50%×100 = 50）", Near(r.revenue, 50.0), D(r.revenue));
		rep.check(u8"T4b 低信度那項 unpricedDrops = 1", r.unpricedDrops == 1, std::to_string(r.unpricedDrops));
		rep.check(u8"T4c 低信度那列 src = None、unit 與 value 為 0",
		          r.drops.size() == 2 && r.drops[0].src == BossPriceSrc::None &&
		              Near(r.drops[0].unit, 0.0) && Near(r.drops[0].value, 0.0));
	}

	// T5 no quote at all
	{
		const Boss b = MakeBoss("B", {MakeVariant("Regular", {Drop("unique|Gone", "Gone", 20.0),
		                                                      Drop("unique|Good", "Good", 50.0)})});
		const BossResult r = ComputeBossVariant(b, b.variants[0], MakeLookup({{"unique|Good", P(100)}}), {});
		rep.check(u8"T5a 查無報價 → src None、unit 0、value 0",
		          r.drops.size() == 2 && r.drops[0].src == BossPriceSrc::None &&
		              Near(r.drops[0].unit, 0.0) && Near(r.drops[0].value, 0.0));
		rep.check(u8"T5b 查無報價 → unpricedDrops = 1 且不進 revenue（= 50）",
		          r.unpricedDrops == 1 && Near(r.revenue, 50.0), D(r.revenue));
	}

	// T6 manual price beats market
	{
		const Boss b = MakeBoss("B", {MakeVariant("Regular", {Drop("unique|Alpha", "Alpha", 10.0)})});
		BossOverrides o;
		o.price["unique|Alpha"] = 800;
		const BossResult r = ComputeBossVariant(b, b.variants[0], MakeLookup({{"unique|Alpha", P(500)}}), o);
		rep.check(u8"T6a 市價 500、手動 800 → unit 800、src Manual",
		          r.drops.size() == 1 && Near(r.drops[0].unit, 800.0) && r.drops[0].src == BossPriceSrc::Manual,
		          r.drops.size() == 1 ? D(r.drops[0].unit) : "size");
		rep.check(u8"T6b 手動價帶入 revenue（10%×800 = 80）", Near(r.revenue, 80.0), D(r.revenue));
	}

	// T7 manual price fills a missing quote
	{
		const Boss b = MakeBoss("B", {MakeVariant("Regular", {Drop("unique|Alpha", "Alpha", 10.0)})});
		BossOverrides o;
		o.price["unique|Alpha"] = 300;
		const BossResult r = ComputeBossVariant(b, b.variants[0], MakeLookup({}), o);
		rep.check(u8"T7a 無報價 + 手動 300 → 計入 revenue 30、src Manual",
		          Near(r.revenue, 30.0) && r.drops.size() == 1 && r.drops[0].src == BossPriceSrc::Manual,
		          D(r.revenue));
		rep.check(u8"T7b 手動補價後 unpricedDrops = 0", r.unpricedDrops == 0, std::to_string(r.unpricedDrops));
		const BossResult r2 =
		    ComputeBossVariant(b, b.variants[0], MakeLookup({{"unique|Alpha", P(9999, true)}}), o);
		rep.check(u8"T7c 低信度市價 + 手動價 → 以手動為準（unit 300、Manual）",
		          r2.drops.size() == 1 && Near(r2.drops[0].unit, 300.0) &&
		              r2.drops[0].src == BossPriceSrc::Manual && r2.unpricedDrops == 0);
	}

	// T8 manual rate beats shipped rate
	{
		const Boss b = MakeBoss("B", {MakeVariant("Regular", {Drop("unique|X", "X Drop", 8.0),
		                                                      Drop("unique|Y", "Y Drop", 5.0)})});
		const std::string vid = BossVariantId(b, b.variants[0]);
		const Lookup lk = MakeLookup({{"unique|X", P(1000)}, {"unique|Y", P(1000)}});
		const BossResult base = ComputeBossVariant(b, b.variants[0], lk, {});
		BossOverrides o;
		o.rate[vid + "|X Drop"] = 10.0;
		const BossResult r = ComputeBossVariant(b, b.variants[0], lk, o);
		rep.check(u8"T8a 沒有覆寫時 rateManual = false、value 依內建 8%（80）",
		          base.drops.size() == 2 && !base.drops[0].rateManual && Near(base.drops[0].ratePct, 8.0) &&
		              Near(base.drops[0].value, 80.0));
		rep.check(u8"T8b 覆寫 10% → ratePct 10、value 100、rateManual true",
		          r.drops.size() == 2 && Near(r.drops[0].ratePct, 10.0) && Near(r.drops[0].value, 100.0) &&
		              r.drops[0].rateManual,
		          r.drops.size() == 2 ? D(r.drops[0].value) : "size");
		rep.check(u8"T8c 只動被覆寫那項，另一項仍 5%、非手動",
		          r.drops.size() == 2 && Near(r.drops[1].ratePct, 5.0) && !r.drops[1].rateManual &&
		              Near(r.drops[1].value, 50.0));
		rep.check(u8"T8d revenue 採覆寫後掉率（100 + 50 = 150）", Near(r.revenue, 150.0), D(r.revenue));
	}

	// T9 unpriced ticket: profit flagged incomplete
	{
		const Boss b = RevenueBoss(true);
		PriceMap pm = RevenuePrices();
		pm.erase("currency|Fragment B");
		const BossResult r = ComputeBossVariant(b, b.variants[0], MakeLookup(pm), {});
		rep.check(u8"T9a 一張門票無報價 → ticketIncomplete true、hasRoi false",
		          r.hasTicket && r.ticketIncomplete && !r.hasRoi);
		rep.check(u8"T9b 無價門票不計入成本（ticketCost 100、profit 240）",
		          Near(r.ticketCost, 100.0) && Near(r.profit, 240.0), D(r.ticketCost) + "/" + D(r.profit));
		rep.check(u8"T9c 無價門票列 src = None",
		          r.tickets.size() == 2 && r.tickets[0].src == BossPriceSrc::Market &&
		              r.tickets[1].src == BossPriceSrc::None);
		pm["currency|Fragment B"] = P(70, true);
		const BossResult r2 = ComputeBossVariant(b, b.variants[0], MakeLookup(pm), {});
		rep.check(u8"T9d 低信度門票同樣視為無價 → ticketIncomplete", r2.ticketIncomplete && !r2.hasRoi);
		BossOverrides o;
		o.price["currency|Fragment B"] = 70;
		const BossResult r3 = ComputeBossVariant(b, b.variants[0], MakeLookup(RevenuePrices()), o);
		rep.check(u8"T9e 手動補上門票價後恢復完整且有 ROI",
		          !r3.ticketIncomplete && r3.hasRoi && Near(r3.roiPct, 100.0));
	}

	// T10 per hour
	{
		const Boss b = RevenueBoss(true);
		const Lookup lk = MakeLookup(RevenuePrices());
		const std::string vid = BossVariantId(b, b.variants[0]);
		BossOverrides o;
		o.killSeconds[vid] = 120;
		const BossResult r = ComputeBossVariant(b, b.variants[0], lk, o);
		rep.check(u8"T10a profit 170、擊殺 120 秒 → hasPerHour、profitPerHour = 5100",
		          r.hasPerHour && Near(r.profitPerHour, 5100.0), D(r.profitPerHour));
		const BossResult none = ComputeBossVariant(b, b.variants[0], lk, {});
		rep.check(u8"T10b 未填擊殺時間 → hasPerHour false", !none.hasPerHour);
		o.killSeconds[vid] = 0;
		const BossResult zero = ComputeBossVariant(b, b.variants[0], lk, o);
		rep.check(u8"T10c 擊殺時間 0 秒不算（不得除以零）→ hasPerHour false", !zero.hasPerHour);
	}

	// T11 ticket quantity
	{
		const Boss b = MakeBoss("B", {MakeVariant("Regular", {Drop("unique|Alpha", "Alpha", 10.0)},
		                                          {Ticket("currency|Key", "Key", 4.0)})});
		const BossResult r = ComputeBossVariant(
		    b, b.variants[0], MakeLookup({{"unique|Alpha", P(1000)}, {"currency|Key", P(25)}}), {});
		rep.check(u8"T11a qty 4 × 25 → ticketCost = 100", Near(r.ticketCost, 100.0), D(r.ticketCost));
		rep.check(u8"T11b 門票列 qty 4、unit 25、value 100",
		          r.tickets.size() == 1 && Near(r.tickets[0].qty, 4.0) && Near(r.tickets[0].unit, 25.0) &&
		              Near(r.tickets[0].value, 100.0));
		rep.check(u8"T11c profit = 100 - 100 = 0、ROI 0%", Near(r.profit, 0.0) && r.hasRoi && Near(r.roiPct, 0.0),
		          D(r.profit));
	}

	// T15 unknown drop rate (shipped as ratePct 0): listed, left out of the sums until the user enters one
	{
		const Boss b = MakeBoss("B", {MakeVariant("Regular", {Drop("unique|X", "X Drop", 10.0),
		                                                      Drop("unique|Y", "Y Drop", 0.0),
		                                                      Drop("unique|Z", "Z Drop", 0.0)})});
		const std::string vid = BossVariantId(b, b.variants[0]);
		const Lookup lk = MakeLookup({{"unique|X", P(1000)}, {"unique|Y", P(500)}});
		const BossResult r = ComputeBossVariant(b, b.variants[0], lk, {});
		rep.check(u8"T15a 掉率未知的掉落不計入收益",
		          r.drops.size() == 3 && Near(r.revenue, 100.0) && Near(r.drops[1].value, 0.0), D(r.revenue));
		rep.check(u8"T15b 掉率未知又無報價不算缺價", r.unpricedDrops == 0, std::to_string(r.unpricedDrops));
		BossOverrides o;
		o.rate[vid + "|Y Drop"] = 20.0;
		o.rate[vid + "|Z Drop"] = 5.0;
		const BossResult r2 = ComputeBossVariant(b, b.variants[0], lk, o);
		rep.check(u8"T15c 手填掉率後計入收益", Near(r2.revenue, 200.0) && r2.drops[1].rateManual, D(r2.revenue));
		rep.check(u8"T15d 手填掉率後無報價就算缺價", r2.unpricedDrops == 1, std::to_string(r2.unpricedDrops));
	}

	// T12 malformed data files
	{
		std::string err;
		rep.check(u8"T12a 截斷的 JSON → 失敗且 err 非空、db 為空",
		          LoadFails(R"({"schema":1,"source":"s","bosses":[{"key":"B","en":)", &err), err);
		rep.check(u8"T12b 空字串 → 失敗且 err 非空", LoadFails(""));
		rep.check(u8"T12c boss key 為 null → 失敗",
		          LoadFails(OneBoss(Variant(R"([{"priceKey":"card|A","en":"A","ratePct":5}])"), "null")));
		rep.check(u8"T12d 掉落 priceKey 為 null → 失敗",
		          LoadFails(OneBoss(Variant(R"([{"priceKey":null,"en":"A","ratePct":5}])"))));
		rep.check(u8"T12e 掉率 0（未知）→ 讀得進來",
		          !LoadFails(OneBoss(Variant(R"([{"priceKey":"card|A","en":"A","ratePct":0}])"))));
		rep.check(u8"T12f 掉率 100.5（>100）→ 失敗",
		          LoadFails(OneBoss(Variant(R"([{"priceKey":"card|A","en":"A","ratePct":100.5}])"))));
		rep.check(u8"T12g 掉率為負 → 失敗",
		          LoadFails(OneBoss(Variant(R"([{"priceKey":"card|A","en":"A","ratePct":-3}])"))));
		rep.check(u8"T12h 掉率型別錯（字串）→ 失敗",
		          LoadFails(OneBoss(Variant(R"([{"priceKey":"card|A","en":"A","ratePct":"5"}])"))));
		rep.check(u8"T12i 缺 variants 欄位 → 失敗",
		          LoadFails(R"({"schema":1,"source":"s","generatedUtc":1,"bosses":[{"key":"B","en":"B","zh":""}]})"));
		rep.check(u8"T12j 門票 qty 0 → 失敗",
		          LoadFails(OneBoss(R"([{"name":"R","uber":false,"tickets":[{"priceKey":"currency|K","en":"K","qty":0}],)"
		                            R"("drops":[{"priceKey":"card|A","en":"A","ratePct":5}]}])")));
		const std::string dupVariant = Variant(R"([{"priceKey":"card|A","en":"A","ratePct":5}])");
		const std::string oneBossJson = R"({"key":"B","en":"B","zh":"","variants":)" + dupVariant + "}";
		rep.check(u8"T12k 重複的 boss key → 失敗",
		          LoadFails(R"({"schema":1,"source":"s","generatedUtc":1,"bosses":[)" + oneBossJson + "," +
		                    oneBossJson + "]}"));

		// A legal minimal file: every field must be read back.
		const std::string good = u8R"json({
 "schema": 1, "source": "測試來源", "generatedUtc": 1,
 "bosses": [
  { "key": "Boss One", "en": "Boss One", "zh": "第一王",
    "variants": [
      { "name": "Uber", "uber": true,
        "tickets": [ { "priceKey": "currency|Key", "en": "Key", "qty": 2.5 } ],
        "drops": [
          { "priceKey": "unique|Alpha|Ring", "en": "Alpha", "ratePct": 12.5, "rateText": "~12", "note": "Guaranteed Unique" },
          { "priceKey": "card|Beta", "en": "Beta", "ratePct": 100 } ] } ] },
  { "key": "Boss Two", "en": "Boss Two", "zh": "",
    "variants": [
      { "name": "Regular", "uber": false, "tickets": [],
        "drops": [ { "priceKey": "gem|Gamma", "en": "Gamma", "ratePct": 0.5 } ] } ] }
 ]})json";
		BossDb db;
		std::string gerr;
		const bool ok = LoadBossDb(good, &db, &gerr);
		rep.check(u8"T12l 合法最小 JSON → 回 true、err 為空、兩個王", ok && db.bosses.size() == 2, gerr);
		if (ok && db.bosses.size() == 2) {
			const Boss& b1 = db.bosses[0];
			const Boss& b2 = db.bosses[1];
			rep.check(u8"T12m source 與王欄位（key/en/zh）讀對、順序保留",
			          db.source == u8"測試來源" && b1.key == "Boss One" && b1.en == "Boss One" &&
			              b1.zh == u8"第一王" && b2.key == "Boss Two" && b2.zh.empty());
			const bool shape = b1.variants.size() == 1 && b1.variants[0].drops.size() == 2 &&
			                   b1.variants[0].tickets.size() == 1;
			rep.check(u8"T12n 變體 name/uber 與門票讀對（qty 2.5）",
			          shape && b1.variants[0].name == "Uber" && b1.variants[0].uber &&
			              b1.variants[0].tickets[0].priceKey == "currency|Key" &&
			              b1.variants[0].tickets[0].en == "Key" && Near(b1.variants[0].tickets[0].qty, 2.5) &&
			              b2.variants.size() == 1 && !b2.variants[0].uber && b2.variants[0].tickets.empty());
			rep.check(u8"T12o 掉落的 rateText 與 note 讀對",
			          shape && b1.variants[0].drops[0].priceKey == "unique|Alpha|Ring" &&
			              b1.variants[0].drops[0].en == "Alpha" && Near(b1.variants[0].drops[0].ratePct, 12.5) &&
			              b1.variants[0].drops[0].rateText == "~12" &&
			              b1.variants[0].drops[0].note == "Guaranteed Unique");
			rep.check(u8"T12p 未給 rateText/note 時為空字串、掉率 100 與 0.5 為合法邊界",
			          shape && b1.variants[0].drops[1].rateText.empty() && b1.variants[0].drops[1].note.empty() &&
			              Near(b1.variants[0].drops[1].ratePct, 100.0) && b2.variants[0].drops.size() == 1 &&
			              Near(b2.variants[0].drops[0].ratePct, 0.5));
		}
	}

	// T13 the shipped data file
	{
		const std::string body = ReadFileForTest(exeDir + L"Data\\boss_drops_poe1.json");
		BossDb db;
		std::string err;
		const bool ok = !body.empty() && LoadBossDb(body, &db, &err);
		rep.check(u8"T13a 出貨資料檔讀得到且 LoadBossDb 成功", ok,
		          body.empty() ? "file missing or empty" : err);
		rep.check(u8"T13b 王數 > 0 且 source 非空", ok && !db.bosses.empty() && !db.source.empty(),
		          std::to_string(db.bosses.size()));
		int badRate = 0, badPrefix = 0, noDrops = 0, dupKey = 0, emptyKey = 0;
		std::set<std::string> seen;
		static const char* kPrefixes[] = {"currency|", "card|", "gem|", "unique|", "map|"};
		for (const Boss& b : db.bosses) {
			if (b.key.empty() || b.en.empty() || b.variants.empty()) emptyKey++;
			if (!seen.insert(b.key).second) dupKey++;
			for (const BossVariant& v : b.variants) {
				if (v.drops.empty()) noDrops++;
				for (const BossDrop& d : v.drops) {
					if (!(d.ratePct >= 0.0 && d.ratePct <= 100.0)) badRate++;
					bool pre = false;
					for (const char* p : kPrefixes)
						if (d.priceKey.rfind(p, 0) == 0) pre = true;
					if (!pre) badPrefix++;
				}
				for (const BossTicket& t : v.tickets) {
					bool pre = false;
					for (const char* p : kPrefixes)
						if (t.priceKey.rfind(p, 0) == 0) pre = true;
					if (!pre || !(t.qty > 0.0)) badPrefix++;
				}
			}
		}
		rep.check(u8"T13c 每個掉落 ratePct 都在 [0,100]（0 = 未知）", ok && badRate == 0, std::to_string(badRate));
		rep.check(u8"T13d 掉落與門票 priceKey 前綴屬於 currency|/card|/gem|/unique|/map|",
		          ok && badPrefix == 0, std::to_string(badPrefix));
		rep.check(u8"T13e boss key 不重複、王有 key/en/變體", ok && dupKey == 0 && emptyKey == 0,
		          std::to_string(dupKey) + "/" + std::to_string(emptyKey));
		rep.check(u8"T13f 每個變體至少一筆掉落", ok && noDrops == 0, std::to_string(noDrops));
		// The list labels a boss's variants 普通 / 超級 from this flag: two alike would read the same.
		std::string sameTier;
		for (const Boss& b : db.bosses) {
			int uber = 0;
			for (const BossVariant& v : b.variants) uber += v.uber ? 1 : 0;
			if (b.variants.size() > 1 && !(b.variants.size() == 2 && uber == 1)) sameTier += b.en + ";";
		}
		rep.check(u8"T13g 多變體的王恰為一普通一 Uber", ok && sameTier.empty(), sameTier);
		// 3.30 moved Atziri into the Temple of Atzoatl (Sacrifice / Mortal fragments are gone) and
		// disabled Velka: the source snapshot predates it.
		std::string stale;
		for (const Boss& b : db.bosses) {
			// Chayula: Breachstones stopped working in 3.28.
			if (b.en.rfind("Velka", 0) == 0 || b.en.rfind("Chayula", 0) == 0) stale += b.en + ";";
			for (const BossVariant& v : b.variants) {
				auto old = [&](const std::string& en) {
					if (en.rfind("Sacrifice at ", 0) == 0 || en.rfind("Mortal ", 0) == 0) stale += b.en + "/" + v.name + ":" + en + ";";
				};
				for (const BossTicket& t : v.tickets) old(t.en);
				for (const BossDrop& d : v.drops) old(d.en);
			}
		}
		rep.check(u8"T13h 不含 3.30 已移除的碎片與停用的王", ok && stale.empty(), stale);
		int atziriChronicle = 0;
		std::string atziriKeys;
		for (const Boss& b : db.bosses)
			if (b.en.rfind("Atziri", 0) == 0)
				for (const BossVariant& v : b.variants)
					// each variant its own room: one shared key would make them share a manual price
					if (v.tickets.size() == 1 && v.tickets[0].en.rfind("Chronicle of Atzoatl", 0) == 0 &&
					    v.tickets[0].priceKey.rfind("currency|Chronicle of Atzoatl (", 0) == 0 &&
					    atziriKeys.find(v.tickets[0].priceKey + ";") == std::string::npos) {
						atziriKeys += v.tickets[0].priceKey + ";";
						atziriChronicle++;
					}
		double zorathEye = 0.0; // poewiki: 20%, the source snapshot said 10
		for (const Boss& b : db.bosses)
			if (b.en.rfind("Ulaman", 0) == 0)
				for (const BossDrop& d : b.variants[0].drops)
					if (d.en == "Zorath's Eye of Authority") zorathEye = d.ratePct;
		// wiki lists these with no rate; the user asked to see them all
		int atziriUnknown = 0;
		for (const Boss& b : db.bosses)
			if (b.en.rfind("Atziri", 0) == 0)
				for (const BossVariant& v : b.variants)
					for (const BossDrop& d : v.drops)
						if (d.ratePct == 0.0 && (d.en == "Soul Core of Instability" || d.en == "Sacrificial Garb" ||
						                         d.en == "Chasing Risk" || (!v.uber && d.en == "Royal Visage")))
							atziriUnknown++;
		rep.check(u8"T13k 阿茲里列出掉率未知的掉落（靈魂核心、祭禮束衣等 6 筆）", ok && atziriUnknown == 6,
		          std::to_string(atziriUnknown));
		// The hidden-notable jewels trade far above the plain ones; a shared key would lend them that price.
		int hiddenOwnKey = 0;
		for (const Boss& b : db.bosses)
			for (const BossVariant& v : b.variants)
				for (const BossDrop& h : v.drops)
					if (h.en.find("(Hidden Ascendancy Notable variant)") != std::string::npos) {
						bool shared = false;
						for (const BossDrop& d : v.drops)
							if (&d != &h && d.priceKey == h.priceKey) shared = true;
						if (!shared) hiddenOwnKey++;
					}
		rep.check(u8"T13n 隱藏昇華版禁忌珠寶的單價鍵不與一般版共用（2 筆）", ok && hiddenOwnKey == 2,
		          std::to_string(hiddenOwnKey));
		// The panel shows names through the bundled item-name table; a drop it lacks reads as English.
		FilterI18n names;
		names.Load(exeDir, "zh-rTW");
		std::set<std::string> untranslated;
		for (const Boss& b : db.bosses)
			for (const BossVariant& v : b.variants) {
				auto see = [&](const std::string& en) {
					const std::string base = en.substr(0, en.find(" ("));
					if (names.DisplayName(base) == base) untranslated.insert(base);
				};
				for (const BossDrop& d : v.drops) see(d.en);
				for (const BossTicket& t : v.tickets) see(t.en);
			}
		std::string untr;
		for (const std::string& n : untranslated) untr += n + ";";
		rep.check(u8"T13l 每個掉落與門票在隨附譯名表都有中文名", ok && untranslated.empty(), untr);
		rep.check(u8"T13m 3.30 改名的 Bound By Destiny 為「命運束縛」",
		          names.DisplayName("Bound By Destiny") == u8"命運束縛", names.DisplayName("Bound By Destiny"));
		rep.check(u8"T13j 烏拉曼的佐拉斯之眼掉率為 20%", ok && zorathEye == 20.0, std::to_string(zorathEye));
		rep.check(u8"T13i 阿茲里兩種變體的門票都是一張阿茲瓦特史記", ok && atziriChronicle == 2,
		          std::to_string(atziriChronicle));
	}

	// T14 overrides persistence + reset
	{
		BossOverrides o;
		o.price["unique|Alpha"] = 800.5;
		o.price["currency|Key"] = 12;
		o.rate["Boss|Regular|Alpha"] = 7.25;
		o.killSeconds["Boss|Regular"] = 95;
		const std::string s = SerializeBossOverrides(o);
		BossOverrides back;
		std::string err;
		const bool ok = ParseBossOverrides(s, &back, &err);
		rep.check(u8"T14a 序列化後可解析", ok, err);
		rep.check(u8"T14b 讀回的 price/rate/killSeconds 與原內容一致",
		          ok && back.price.size() == 2 && Near(back.price["unique|Alpha"], 800.5) &&
		              Near(back.price["currency|Key"], 12.0) && back.rate.size() == 1 &&
		              Near(back.rate["Boss|Regular|Alpha"], 7.25) && back.killSeconds.size() == 1 &&
		              back.killSeconds["Boss|Regular"] == 95);
		BossOverrides emptyBack;
		rep.check(u8"T14c 空覆寫序列化後仍可解析且全空",
		          ParseBossOverrides(SerializeBossOverrides(BossOverrides()), &emptyBack, &err) &&
		              emptyBack.price.empty() && emptyBack.rate.empty() && emptyBack.killSeconds.empty(), err);

		BossOverrides junk;
		std::string jerr;
		rep.check(u8"T14d 壞 JSON 給 ParseBossOverrides → false",
		          !ParseBossOverrides("{ not json", &junk, &jerr));
		rep.check(u8"T14e 空字串給 ParseBossOverrides → false", !ParseBossOverrides("", &junk, &jerr));
		BossOverrides part;
		rep.check(u8"T14k 一筆 null 值只丟那一筆，其餘覆寫保留",
		          ParseBossOverrides(R"({"price":{"a":null,"b":5},"rate":{"r":"x"},"killSeconds":{"k":60}})",
		                             &part, &jerr) &&
		              part.price.size() == 1 && Near(part.price["b"], 5.0) && part.rate.empty() &&
		              part.killSeconds["k"] == 60);

		BossOverrides so, sback, sbad;
		so.sort = { -2, 4 };
		rep.check(u8"T14l 排序欄位存檔後讀回一致",
		          ParseBossOverrides(SerializeBossOverrides(so), &sback, &jerr) && sback.sort == so.sort);
		rep.check(u8"T14m 排序只留 ±1..7 的整數，其餘丟掉",
		          ParseBossOverrides(R"({"sort":[0,8,"x",-3,1.5,-9]})", &sbad, &jerr) &&
		              sbad.sort == std::vector<int>{ -3 });

		// Reset: variant "Regular" is wiped; variant "Regular2" (shares the id prefix) and unrelated
		// prices stay.
		const Boss b = MakeBoss("Boss", {MakeVariant("Regular", {Drop("unique|Alpha", "Alpha", 10.0)},
		                                             {Ticket("currency|Key", "Key")}),
		                                 MakeVariant("Regular2", {Drop("unique|Other", "Other", 10.0)})});
		rep.check(u8"T14f BossVariantId 為 \"<boss.key>|<variant.name>\"",
		          BossVariantId(b, b.variants[0]) == "Boss|Regular" &&
		              BossVariantId(b, b.variants[1]) == "Boss|Regular2");
		BossOverrides r;
		r.price["unique|Alpha"] = 1;         // this variant's drop
		r.price["currency|Key"] = 2;         // this variant's ticket
		r.price["unique|Other"] = 3;         // another variant's drop
		r.price["currency|Unrelated"] = 4;   // nobody's
		r.rate["Boss|Regular|Alpha"] = 9;    // this variant
		r.rate["Boss|Regular2|Other"] = 8;   // the id-prefix neighbour
		r.killSeconds["Boss|Regular"] = 60;  // this variant
		r.killSeconds["Boss|Regular2"] = 70; // the id-prefix neighbour
		r.sort = { 3 };
		ResetBossVariantOverrides(&r, b, b.variants[0]);
		rep.check(u8"T14n 重置此王不動排序", r.sort == std::vector<int>{ 3 });
		rep.check(u8"T14g 重置後該變體的 rate 與 killSeconds 不見",
		          r.rate.count("Boss|Regular|Alpha") == 0 && r.killSeconds.count("Boss|Regular") == 0);
		rep.check(u8"T14h 重置後該變體掉落與門票的 price 不見",
		          r.price.count("unique|Alpha") == 0 && r.price.count("currency|Key") == 0);
		rep.check(u8"T14i 無關的 price 保留（其他變體掉落、完全無關者）",
		          r.price.count("unique|Other") == 1 && Near(r.price["unique|Other"], 3.0) &&
		              r.price.count("currency|Unrelated") == 1 && r.price.size() == 2);
		rep.check(u8"T14j 名稱前綴相同的鄰近變體 rate/killSeconds 不受影響",
		          r.rate.size() == 1 && Near(r.rate["Boss|Regular2|Other"], 8.0) &&
		              r.killSeconds.size() == 1 && r.killSeconds["Boss|Regular2"] == 70);
	}

	rep.text += "\n" + std::to_string(rep.checks) + " checks\n" +
	            (rep.failures == 0 ? "RESULT PASS\n" : "RESULT FAIL\n");
	PrintAndSave(exeDir, rep.text, L"boss_profit_selftest.txt");
	return rep.failures == 0 ? 0 : 1;
}
