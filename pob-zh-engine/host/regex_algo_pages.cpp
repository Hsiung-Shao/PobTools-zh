#include "regex_algo_pages.h"

#include "regex_match.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>

// Port of exile-appraiser regex/src: pages/numeric-pages.ts, pages/vendor-pages.ts,
// pages/index.ts, sections.ts, view.ts, combine.ts. File:line in the
// comments refer to those files at 0155244.

namespace RegexAlgo {

namespace {

using RegexFrag::HasChoice;

const std::string* Label(const RegexLabels& labels, bool zh, const std::string& key)
{
	const std::string* s = labels.Find(zh, key);
	return (s && !s->empty()) ? s : nullptr;   // TS: `labels.zh[key]` truthy
}

bool Usable(const RegexLabels* labels)
{
	return labels && labels->present;   // TS: labels === null -> no pages
}

RegexEntryDef BaseDef(const std::string& id, int g, const std::string& zh, const std::string& en)
{
	RegexEntryDef d;
	d.id = id;
	d.group = g;
	d.zh = {zh};
	d.en = {en};
	return d;
}

const std::vector<RangeOp> kAllOps = {RangeOp::Ge, RangeOp::Le, RangeOp::Range};

// JS Number(str) for the option ids the vendor page feeds linkedSockets: trimmed,
// "" -> 0, anything not a whole decimal literal -> NaN.
double JsNumber(const std::string& s)
{
	size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || (s[a] >= '\t' && s[a] <= '\r'))) a++;
	while (b > a && (s[b - 1] == ' ' || (s[b - 1] >= '\t' && s[b - 1] <= '\r'))) b--;
	if (a == b) return 0;
	const std::string t = s.substr(a, b - a);
	char* end = nullptr;
	const double v = std::strtod(t.c_str(), &end);
	if (!end || *end != '\0') return std::nan("");
	return v;
}

// Number.isInteger + an int the frag functions can take (they range-check).
std::optional<int> AsInt(double v)
{
	if (!std::isfinite(v) || std::floor(v) != v) return std::nullopt;
	if (v > 1e6) return 1000000;
	if (v < -1e6) return -1000000;
	return (int)v;
}

// JS String(number) for the numbers condText prints.
std::string JsNum(double v)
{
	char buf[64];
	if (std::floor(v) == v && std::fabs(v) < 1e21) {
		snprintf(buf, sizeof buf, "%.0f", v);
		return v == 0 ? std::string("0") : std::string(buf);
	}
	for (int p = 1; p <= 17; p++) {
		snprintf(buf, sizeof buf, "%.*g", p, v);
		if (std::strtod(buf, nullptr) == v) break;
	}
	return buf;
}

// ---- numeric-pages.ts --------------------------------------------------------

struct NumSpec {
	const char* id;
	const char* key;
	int digits;
	bool percent;
	double defMin;
	int lo = -1, hi = -1;   // -1 = TS default (0 / 99 or 999)
};

// numeric-pages.ts:21 POE1_MAP
const NumSpec kPoe1Map[] = {
	{"tier", "ItemDisplayMapTier", 2, false, 16, 1, 17},
	{"quantity", "ItemDisplayMapQuantityIncrease", 3, true, 80},
	{"rarity", "ItemDisplayMapRarityIncrease", 3, true, 80},
	{"pack", "ItemDisplayMapPackSizeIncrease", 3, true, 30},
	{"scarabs", "ItemDisplayMapScarabDropBonus", 3, true, 50},
	{"currency", "ItemDisplayMapCurrencyDropBonus", 3, true, 50},
	{"maps", "ItemDisplayMapMapDropBonus", 3, true, 50},
	{"divination", "ItemDisplayMapDivinationCardDropBonus", 3, true, 50},
};

// numeric-pages.ts:32 POE2_WAYSTONE
const NumSpec kPoe2Waystone[] = {
	{"tier", "ItemDisplayMapTier", 2, false, 15, 1, 16},
	{"rarity", "ItemDisplayMapItemRarity", 3, true, 50},
	{"pack", "ItemDisplayMapPack", 3, true, 30},
	{"monster_rarity", "ItemDisplayMapMonsterRarity", 3, true, 30},
	{"waystone_drop", "ItemDisplayMapWaystoneDropChance", 3, true, 100},
	{"magic_monsters", "ItemDisplayMapMagicMonsterQuantityBonus", 3, true, 30},
	{"rare_monsters", "ItemDisplayMapRareMonsterQuantityBonus", 3, true, 30},
	{"experience", "ItemDisplayMapExperienceGained", 3, true, 20},
	{"effectiveness", "ItemDisplayMapMonsterEffectiveness", 3, true, 20},
};

// numeric-pages.ts:49 rarity row
const char* const kRarityLabelKey = "ItemDisplayStringRarity";
struct OptKey { const char* id; const char* key; };
const OptKey kRarityOptions[] = {
	{"normal", "ItemDisplayStringNormal"},
	{"magic", "ItemDisplayStringMagic"},
	{"rare", "ItemDisplayStringRare"},
	{"unique", "ItemDisplayStringUnique"},
};

// numeric-pages.ts:68 numEntry
std::optional<AlgoEntry> NumEntry(const NumSpec& s, const RegexLabels& labels)
{
	const std::string* zh = Label(labels, true, s.key);
	const std::string* en = Label(labels, false, s.key);
	if (!zh || !en) return std::nullopt;
	AlgoEntry e;
	e.def = BaseDef(s.id, 0, RegexFrag::LabelBase(*zh), RegexFrag::LabelBase(*en));
	e.input.kind = InputKind::Range;
	e.input.digits = s.digits;
	e.input.percent = s.percent;
	e.input.ops = kAllOps;
	e.input.lo = s.lo >= 0 ? s.lo : 0;
	e.input.hi = s.hi >= 0 ? s.hi : (s.digits == 3 ? 999 : 99);
	e.input.def.min = s.defMin;
	const int digits = s.digits;
	const bool percent = s.percent;
	if (std::string(s.id) == "tier") {
		// The tier is in the item NAME "（階級 N）", not on a property line (frag.ts mapTierFragment).
		e.fragment = [digits](const AlgoValue& v, Lang lang) { return RegexFrag::MapTierFragment(v, digits, lang); };
		e.ownLine = [](const std::string& l) { return RegexFrag::IsTierNameLine(l); };
	} else {
		const std::string lz = *zh, le = *en;
		e.fragment = [lz, le, digits, percent](const AlgoValue& v, Lang lang) {
			return RegexFrag::StrictPropertyFragment(lang == Lang::Zh ? lz : le, v, digits, percent);
		};
	}
	return e;
}

// numeric-pages.ts:95 rarityEntry
std::optional<AlgoEntry> RarityEntry(const RegexLabels& labels)
{
	const std::string* zh = Label(labels, true, kRarityLabelKey);
	const std::string* en = Label(labels, false, kRarityLabelKey);
	if (!zh || !en) return std::nullopt;
	std::vector<AlgoOption> opts;
	for (const OptKey& o : kRarityOptions) {
		const std::string* oz = Label(labels, true, o.key);
		const std::string* oe = Label(labels, false, o.key);
		if (oz && oe) opts.push_back({o.id, *oz, *oe});
	}
	if (opts.empty()) return std::nullopt;
	AlgoEntry e;
	e.def = BaseDef(kRarityEntryId, 0, RegexFrag::LabelBase(*zh), RegexFrag::LabelBase(*en));
	e.input.kind = InputKind::Select;
	e.input.options = opts;
	bool hasRare = false;
	for (const AlgoOption& o : opts) hasRare |= (o.id == "rare");
	e.input.def.choice = hasRare ? "rare" : opts[0].id;
	const std::string lz = *zh, le = *en;
	e.fragment = [lz, le, opts](const AlgoValue& v, Lang lang) -> std::optional<std::string> {
		if (!HasChoice(v)) return std::nullopt;
		for (const AlgoOption& o : opts)
			if (o.id == v.choice)
				return RegexFrag::RarityFragment(lang == Lang::Zh ? lz : le, lang == Lang::Zh ? o.zh : o.en);
		return std::nullopt;
	};
	return e;
}

// ---- vendor-pages.ts ---------------------------------------------------------

const OptKey kInfluences[] = {
	{"shaper", "ItemPopupShaperItem"},
	{"elder", "ItemPopupElderItem"},
	{"crusader", "ItemPopupCrusaderItem"},
	{"redeemer", "ItemPopupRedeemerItem"},
	{"hunter", "ItemPopupHunterItem"},
	{"warlord", "ItemPopupWarlordItem"},
	{"exarch", "ItemPopupSearingExarchItem"},
	{"eater", "ItemPopupEaterofWorldsItem"},
};

struct ZhEn { std::string zh, en; };

std::optional<ZhEn> L(const RegexLabels& labels, const std::string& key)
{
	const std::string* zh = Label(labels, true, key);
	const std::string* en = Label(labels, false, key);
	if (!zh || !en) return std::nullopt;
	return ZhEn{*zh, *en};
}

std::vector<AlgoOption> ColorOptions()
{
	return {
		{"r", u8"紅 R", "Red R"},
		{"g", u8"綠 G", "Green G"},
		{"b", u8"藍 B", "Blue B"},
		{"w", u8"白 W", "White W"},
	};
}

void Join(std::string& out, const std::vector<std::string>& parts, const char* sep)
{
	for (size_t i = 0; i < parts.size(); i++) {
		if (i) out += sep;
		out += parts[i];
	}
}

std::string QuoteIfNeeded(const std::string& t)
{
	return t.find(' ') != std::string::npos ? "\"" + t + "\"" : t;
}

} // namespace

const char* const kRarityEntryId = "item_rarity_class";

std::vector<std::string> NumericLabelKeys(const std::string& game)
{
	std::vector<std::string> out;
	if (game == "poe1") for (const NumSpec& s : kPoe1Map) out.push_back(s.key);
	else for (const NumSpec& s : kPoe2Waystone) out.push_back(s.key);
	out.push_back(kRarityLabelKey);
	for (const OptKey& o : kRarityOptions) out.push_back(o.key);
	return out;
}

std::vector<std::string> VendorLabelKeys(const std::string& game)
{
	if (game != "poe1") return {"ItemLevelPopup", "Quality", "ItemPopupCorrupted", "Level"};
	std::vector<std::string> out = {"ItemLevelPopup", "Quality", "ItemDisplayStringSockets", "ItemPopupCorrupted", "Level"};
	for (const OptKey& i : kInfluences) out.push_back(i.key);
	return out;
}

// numeric-pages.ts:123 numericPages
std::vector<AlgoPage> NumericPages(const std::string& game, const RegexLabels* labels)
{
	if (!Usable(labels)) return {};
	const bool poe1 = (game == "poe1");
	std::vector<AlgoEntry> entries;
	if (poe1) {
		for (const NumSpec& s : kPoe1Map)
			if (auto e = NumEntry(s, *labels)) entries.push_back(std::move(*e));
	} else {
		for (const NumSpec& s : kPoe2Waystone)
			if (auto e = NumEntry(s, *labels)) entries.push_back(std::move(*e));
	}
	if (auto r = RarityEntry(*labels)) entries.push_back(std::move(*r));
	if (entries.empty()) return {};
	AlgoPage p;
	p.game = game;
	p.id = poe1 ? "map_numeric" : "waystone_numeric";
	p.kind = RegexPageKind::Numeric;
	p.sectionOf = poe1 ? "map_mods" : "waystone_mods";
	p.title = poe1 ? u8"地圖數值條件" : u8"換界石數值條件";
	p.titleEn = poe1 ? "Map values" : "Waystone values";
	p.note = poe1
		? u8"地圖屬性行(階級、物品數量、稀有度…)的數值條件,每條各自一個 term(同時成立)。標籤取自 GGPK clientstrings;寫法依社群實用格式「標籤: +N%」(半形 / 全形冒號、+ 可有可無),階級比對名稱「（階級 N）」。"
		: u8"換界石屬性行(階級、稀有度、怪群大小…)的數值條件,每條各自一個 term(同時成立)。標籤取自 GGPK clientstrings;寫法依社群實用格式「標籤: +N%」(半形 / 全形冒號、+ 可有可無),階級比對名稱「（階級 N）」。";
	p.limit = 250;
	p.groups = {u8"數值"};
	p.groupsEn = {"Values"};
	p.entries = std::move(entries);
	return {std::move(p)};
}

// vendor-pages.ts:36 vendorPages
std::vector<AlgoPage> VendorPages(const std::string& game, const RegexLabels* labels)
{
	if (!Usable(labels)) return {};
	const RegexLabels& lab = *labels;
	std::vector<AlgoEntry> entries;
	const bool poe1 = (game == "poe1");

	const std::optional<ZhEn> sockets = L(lab, "ItemDisplayStringSockets");
	if (poe1) {
		{
			AlgoEntry e;
			e.def = BaseDef("links", 0, u8"連結數(≥)", u8"Linked sockets (≥)");
			e.untested = true;
			e.input.kind = InputKind::Select;
			e.input.options = {{"6", "6L", "6L"}, {"5", "5L", "5L"}, {"4", "4L", "4L"}, {"3", "3L", "3L"}};
			e.input.def.choice = "6";
			e.fragment = [](const AlgoValue& v, Lang) -> std::optional<std::string> {
				const std::optional<int> n = AsInt(JsNumber(v.choice));
				return n ? RegexFrag::LinkedSockets(*n) : std::nullopt;
			};
			entries.push_back(std::move(e));
		}
		{
			AlgoEntry e;
			e.def = BaseDef("link_colors", 0, u8"鏈接顏色(任意順序、相鄰)", "Linked colours (any order, adjacent)");
			e.untested = true;
			e.input.kind = InputKind::Colors;
			e.input.maxTotal = 6;
			e.input.def.choice = "rgb";
			e.fragment = [](const AlgoValue& v, Lang) { return RegexFrag::LinkColors(v.choice); };
			entries.push_back(std::move(e));
		}
		if (sockets) {
			AlgoEntry e;
			e.def = BaseDef("socket_colors", 0, u8"插槽顏色數(≥)", u8"Socket colour count (≥)");
			e.untested = true;
			e.input.kind = InputKind::Count;
			e.input.options = ColorOptions();
			e.input.lo = 1;
			e.input.hi = 6;
			e.input.def.choice = "b";
			e.input.def.min = 3;
			const ZhEn s = *sockets;
			e.fragment = [s](const AlgoValue& v, Lang lang) -> std::optional<std::string> {
				const std::optional<int> n = AsInt(v.min ? *v.min : 0.0);
				if (!n) return std::nullopt;
				return RegexFrag::SocketColorCount(lang == Lang::Zh ? s.zh : s.en, v.choice, *n);
			};
			entries.push_back(std::move(e));
		}
	}

	if (const std::optional<ZhEn> ilvl = L(lab, "ItemLevelPopup")) {
		AlgoEntry e;
		e.def = BaseDef("ilvl", 1, RegexFrag::LabelBase(ilvl->zh), RegexFrag::LabelBase(ilvl->en));
		e.input.kind = InputKind::Range;
		e.input.digits = 3;
		e.input.percent = false;
		e.input.ops = kAllOps;
		e.input.lo = 1;
		e.input.hi = 100;
		e.input.def.min = 86;
		const ZhEn s = *ilvl;
		e.fragment = [s](const AlgoValue& v, Lang lang) {
			return RegexFrag::PropertyFragment(lang == Lang::Zh ? s.zh : s.en, v, 3, false);
		};
		entries.push_back(std::move(e));
	}
	if (const std::optional<ZhEn> quality = L(lab, "Quality")) {
		AlgoEntry e;
		e.def = BaseDef("quality", 1, RegexFrag::LabelBase(quality->zh), RegexFrag::LabelBase(quality->en));
		e.input.kind = InputKind::Range;
		e.input.digits = 2;
		e.input.percent = true;
		e.input.ops = kAllOps;
		e.input.lo = 0;
		e.input.hi = 30;
		e.input.def.min = 20;
		const ZhEn s = *quality;
		e.fragment = [s](const AlgoValue& v, Lang lang) {
			return RegexFrag::PropertyFragment(lang == Lang::Zh ? s.zh : s.en, v, 2, true);
		};
		entries.push_back(std::move(e));
	}
	if (const std::optional<ZhEn> gem = L(lab, "Level")) {
		AlgoEntry e;
		e.def = BaseDef("gem_level", 1, u8"寶石" + RegexFrag::LabelBase(gem->zh) + u8"(≥)",
		                "Gem " + RegexFrag::LabelBase(gem->en) + u8" (≥)");
		e.input.kind = InputKind::Range;
		e.input.digits = 2;
		e.input.percent = false;
		e.input.ops = {RangeOp::Ge};
		e.input.lo = 1;
		e.input.hi = 21;
		e.input.def.min = 20;
		const ZhEn s = *gem;
		// Anchored at the line start: 等級 also occurs in 物品等級 / 怪物等級 / 需求等級.
		e.fragment = [s](const AlgoValue& v, Lang lang) {
			AlgoValue only;
			only.min = v.min;
			return RegexFrag::PropertyFragment(lang == Lang::Zh ? s.zh : s.en, only, 2, false, true);
		};
		entries.push_back(std::move(e));
	}
	if (const std::optional<ZhEn> corrupted = L(lab, "ItemPopupCorrupted")) {
		AlgoEntry e;
		e.def = BaseDef("corrupted", 1, RegexFrag::LabelBase(corrupted->zh), RegexFrag::LabelBase(corrupted->en));
		e.input.kind = InputKind::Select;
		const ZhEn s = *corrupted;
		e.fragment = [s](const AlgoValue&, Lang lang) {
			return RegexFrag::WholeLine({lang == Lang::Zh ? s.zh : s.en});
		};
		entries.push_back(std::move(e));
	}
	if (poe1) {
		struct Infl { std::string id; ZhEn t; };
		std::vector<Infl> infl;
		for (const OptKey& i : kInfluences)
			if (auto t = L(lab, i.key)) infl.push_back({i.id, *t});
		if (!infl.empty()) {
			AlgoEntry e;
			e.def = BaseDef("influence", 2, u8"勢力基底", "Influenced base");
			e.input.kind = InputKind::Select;
			e.input.options.push_back({"any", u8"任一勢力", "Any influence"});
			for (const Infl& x : infl)
				e.input.options.push_back({x.id, RegexFrag::LabelBase(x.t.zh), RegexFrag::LabelBase(x.t.en)});
			e.input.def.choice = "any";
			e.fragment = [infl](const AlgoValue& v, Lang lang) -> std::optional<std::string> {
				std::vector<std::string> pick;
				const bool any = v.choice == "any" || v.choice.empty();
				for (const Infl& x : infl)
					if (any || x.id == v.choice) pick.push_back(lang == Lang::Zh ? x.t.zh : x.t.en);
				if (pick.empty()) return std::nullopt;
				return RegexFrag::WholeLine(pick);
			};
			entries.push_back(std::move(e));
		}
	}
	if (entries.empty()) return {};
	AlgoPage p;
	p.game = game;
	p.id = poe1 ? "vendor_items" : "vendor_items_poe2";
	p.kind = RegexPageKind::Sockets;
	p.title = u8"商店 / 物品條件";
	p.titleEn = "Vendor & item conditions";
	p.note = poe1
		? u8"連結、插槽顏色、物品等級、品質、已汙染、勢力。每條各自一個 term(同時成立)。插槽相關假設繁中客戶端顯示 R-G-B 不翻譯,待實測。"
		: u8"物品等級、品質、已汙染、寶石等級。每條各自一個 term(同時成立)。";
	p.limit = 250;
	if (poe1) {
		p.groups = {u8"插槽與連結", u8"物品屬性", u8"勢力"};
		p.groupsEn = {"Sockets & links", "Item properties", "Influence"};
	} else {
		p.groups = {u8"插槽與連結", u8"物品屬性"};
		p.groupsEn = {"Sockets & links", "Item properties"};
	}
	p.entries = std::move(entries);
	return {std::move(p)};
}

// pages/index.ts:22 algoPages
std::vector<AlgoPage> AlgoPages(const std::string& game, const RegexLabels* labels)
{
	std::vector<AlgoPage> out = NumericPages(game, labels);
	for (AlgoPage& p : VendorPages(game, labels)) out.push_back(std::move(p));
	return out;
}

// ---- sections.ts -------------------------------------------------------------

std::string SectionHostOf(const std::string& pageId)
{
	if (pageId == "map_numeric") return "map_mods";
	if (pageId == "waystone_numeric") return "waystone_mods";
	return std::string();
}

std::string SectionIdOf(const std::string& hostId)
{
	if (hostId == "map_mods") return "map_numeric";
	if (hostId == "waystone_mods") return "waystone_numeric";
	return std::string();
}

std::string NumericKeyOf(const std::string& pageId)
{
	const std::string h = SectionHostOf(pageId);
	return h.empty() ? pageId : h;
}

// ---- pages/index.ts ----------------------------------------------------------

std::vector<PageRef> ListedPages(const std::vector<PageRef>& pages)
{
	std::vector<PageRef> out;
	for (const PageRef& p : pages)
		if (!p.IsSection()) out.push_back(p);
	return out;
}

const AlgoPage* SectionPageOf(const std::vector<PageRef>& pages, const std::string& hostId, const std::string& game)
{
	for (const PageRef& p : pages)
		if (p.IsSection() && p.algo->sectionOf == hostId && (game.empty() || p.algo->game == game)) return p.algo;
	return nullptr;
}

std::string HostIdOf(const std::vector<PageRef>& pages, const std::string& id)
{
	for (const PageRef& p : pages)
		if (p.Id() == id) return p.IsSection() ? p.algo->sectionOf : id;
	return id;
}

std::vector<PageRef> CombineOrder(const std::vector<PageRef>& pages, const std::string* only)
{
	std::vector<PageRef> out;
	for (const PageRef& p : ListedPages(pages)) {
		if (only && p.Id() != *only) continue;
		out.push_back(p);
		// TS passes the page object, so the section must belong to the same game.
		if (const AlgoPage* sec = SectionPageOf(pages, p.Id(), p.Game())) {
			PageRef r;
			r.algo = sec;
			out.push_back(r);
		}
	}
	return out;
}

// ---- values / row editor -----------------------------------------------------

const AlgoValue& ValueOf(const ValueMap& m, const AlgoEntry& e)
{
	auto it = m.find(e.def.id);
	return it != m.end() ? it->second : e.input.def;
}

bool ValueUsable(const AlgoEntry& e, const AlgoValue& v, Lang lang)
{
	return e.fragment && e.fragment(v, lang).has_value();
}

RangeOp OpOf(const AlgoEntry& e, const AlgoValue& v)
{
	const RangeOp op = RegexFrag::RangeOpOf(v);
	if (op != RangeOp::None) {
		if (e.input.kind != InputKind::Range) return op;
		for (RangeOp o : e.input.ops)
			if (o == op) return op;
	}
	if (e.input.kind == InputKind::Range && !e.input.ops.empty()) return e.input.ops[0];
	return RangeOp::Ge;
}

AlgoValue WithOp(const AlgoEntry& e, const AlgoValue& cur, RangeOp op)
{
	if (e.input.kind != InputKind::Range) return cur;
	const AlgoValue& def = e.input.def;
	const double mn = cur.min ? *cur.min : def.min ? *def.min : cur.max ? *cur.max : (double)e.input.lo;
	const double mx = cur.max ? *cur.max : def.max ? *def.max : cur.min ? *cur.min : (double)e.input.hi;
	AlgoValue v;
	if (op == RangeOp::Ge) v.min = mn;
	else if (op == RangeOp::Le) v.max = mx;
	else {
		v.min = std::min(mn, mx);
		v.max = std::max(mn, mx);
	}
	return v;
}

AlgoValue WithNum(const AlgoEntry& e, const AlgoValue& cur, bool isMax, std::optional<double> n)
{
	const RangeOp op = OpOf(e, cur);   // read before the edit, as the Vue handler does
	AlgoValue v = cur;
	std::optional<double>& slot = isMax ? v.max : v.min;
	if (!n || !std::isfinite(*n)) slot.reset();
	else slot = std::trunc(*n);
	if (e.input.kind == InputKind::Range) {
		if (op == RangeOp::Ge) v.max.reset();
		if (op == RangeOp::Le) v.min.reset();
	}
	return v;
}

AlgoValue WithChoice(const AlgoValue& cur, const std::string& id)
{
	AlgoValue v = cur;
	v.choice = id;
	v.hasChoice = true;
	return v;
}

int ColorCount(const AlgoValue& v, char c)
{
	return (int)std::count(v.choice.begin(), v.choice.end(), c);
}

AlgoValue WithColor(const AlgoValue& cur, char c, int n)
{
	n = std::max(0, std::min(6, n));
	int r = ColorCount(cur, 'r'), g = ColorCount(cur, 'g'), b = ColorCount(cur, 'b');
	if (c == 'r') r = n;
	else if (c == 'g') g = n;
	else if (c == 'b') b = n;
	AlgoValue v;
	v.choice = std::string(r, 'r') + std::string(g, 'g') + std::string(b, 'b');
	v.hasChoice = true;
	return v;
}

bool AlgoSelection::SetValue(const AlgoPage& page, int idx, const AlgoValue& v, bool tick)
{
	if (idx < 0 || idx >= (int)page.entries.size()) return false;
	values[page.entries[idx].def.id] = v;
	if (picked.size() != page.entries.size()) picked.resize(page.entries.size(), 0);
	if (tick && !picked[idx]) {
		picked[idx] = 1;
		return true;
	}
	return false;
}

int AlgoSelection::Count() const
{
	int n = 0;
	for (char c : picked) n += c ? 1 : 0;
	return n;
}

std::vector<int> AlgoSelection::Picks() const
{
	std::vector<int> out;
	for (int i = 0; i < (int)picked.size(); i++)
		if (picked[i]) out.push_back(i);
	return out;
}

// ---- view.ts -----------------------------------------------------------------

std::optional<std::string> CondText(const AlgoEntry& e, const AlgoValue& v, Lang lang)
{
	if (!ValueUsable(e, v, lang)) return std::nullopt;
	if (e.input.kind == InputKind::Select) {
		for (const AlgoOption& o : e.input.options)
			if (HasChoice(v) && o.id == v.choice) return lang == Lang::En ? o.en : o.zh;
	}
	if (e.input.kind != InputKind::Range) {
		if (HasChoice(v)) return v.choice;
		if (v.min) return u8"≥" + JsNum(*v.min);
		return std::nullopt;
	}
	const std::string pct = e.input.percent ? "%" : "";
	switch (RegexFrag::RangeOpOf(v)) {
	case RangeOp::Ge: return u8"≥" + JsNum(*v.min) + pct;
	case RangeOp::Le: return u8"≤" + JsNum(*v.max) + pct;
	case RangeOp::Range: return JsNum(*v.min) + u8"–" + JsNum(*v.max) + pct;
	default: return std::nullopt;
	}
}

std::vector<SummaryItem> SectionSummary(const AlgoPage& page, const std::vector<int>& picked,
                                        const ValueMap& values, Lang labelLang)
{
	const std::set<int> set(picked.begin(), picked.end());
	std::vector<SummaryItem> out;
	for (int i = 0; i < (int)page.entries.size(); i++) {
		if (!set.count(i)) continue;
		const AlgoEntry& e = page.entries[i];
		const std::vector<std::string>& names = labelLang == Lang::En ? e.def.en : e.def.zh;
		out.push_back({e.def.id, names.empty() ? e.def.id : names[0], CondText(e, ValueOf(values, e), labelLang)});
	}
	return out;
}

// ---- combine ------------------------------------------------------

void BuildPageCorpus(const RegexPageDef& page, Lang lang, RegexGen::Corpus& out)
{
	const bool zh = (lang == Lang::Zh);
	std::vector<RegexGen::Entry> es;
	es.reserve(page.entries.size());
	for (const RegexEntryDef& d : page.entries) {
		RegexGen::Entry e;
		e.id = d.id;
		// The lines in the language being built for. "No false positives" is a
		// claim about ONE language's list, so the corpus is the one the player
		// will paste into (data.ts entryLines: want, else the other language).
		const std::vector<std::string>& want = zh ? d.zh : d.en;
		const std::vector<std::string>& fallback = zh ? d.en : d.zh;
		const bool useWant = !want.empty();
		e.texts = useWant ? want : fallback;
		// Hidden text follows the language the entry actually ended up in.
		e.hidden = useWant ? (zh ? d.hiddenZh : d.hiddenEn) : (zh ? d.hiddenEn : d.hiddenZh);
		es.push_back(std::move(e));
	}
	RegexGen::Ambient amb;
	amb.lines = zh ? page.ambientZh : page.ambientEn;
	amb.nameLeft = zh ? page.namePrefixZh : page.namePrefixEn;
	amb.nameRight = zh ? page.nameSuffixZh : page.nameSuffixEn;
	out.Reset(std::move(es), std::move(amb));
}

const char* ConflictKindId(ConflictKind k)
{
	switch (k) {
	case ConflictKind::Extra: return "extra";
	case ConflictKind::Missing: return "missing";
	case ConflictKind::Ambient: return "ambient";
	case ConflictKind::Fragment: return "fragment";
	case ConflictKind::Exclude: return "exclude";
	case ConflictKind::Invalid: return "invalid";
	}
	return "?";
}

std::string JsTrim(const std::string& t)
{
	// JS WhiteSpace + LineTerminator, on code points.
	std::u32string cps;
	RxDecodeUtf8(t, cps);
	auto isWs = [](char32_t c) {
		return c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C || c == 0x0D || c == 0x20 || c == 0xA0 ||
		       c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F ||
		       c == 0x205F || c == 0x3000 || c == 0xFEFF;
	};
	size_t a = 0, b = cps.size();
	while (a < b && isWs(cps[a])) a++;
	while (b > a && isWs(cps[b - 1])) b--;
	// Trimming only removes whole code points at the ends, so cut the UTF-8 at
	// the same places: the byte length of the dropped code points.
	auto bytesOf = [](char32_t c) -> size_t { return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4; };
	size_t lead = 0, trail = 0;
	for (size_t i = 0; i < a; i++) lead += bytesOf(cps[i]);
	for (size_t i = b; i < cps.size(); i++) trail += bytesOf(cps[i]);
	return (lead + trail >= t.size()) ? std::string() : t.substr(lead, t.size() - lead - trail);
}

// combine.ts:85 escapeTerm
std::string EscapeTerm(const std::string& s)
{
	// s.replace(/"/g, '').trim()
	std::string q;
	for (char c : s)
		if (c != '"') q += c;
	const std::string t = JsTrim(q);
	// .replace(/[\\^$.|?*+()[\]{}]/g, c => '\\' + c)
	static const std::string kSpecial = "\\^$.|?*+()[]{}";
	std::string out;
	for (char c : t) {
		if (kSpecial.find(c) != std::string::npos) out += '\\';
		out += c;
	}
	return (!out.empty() && out[0] == '!') ? "\\" + out : out;
}

namespace {

// data.ts entryLines: the lines in `lang`, else the other language; hidden text
// follows the language the entry ended up in.
void EntryLines(const RegexEntryDef& d, Lang lang, RegexGen::Entry& e)
{
	const bool zh = (lang == Lang::Zh);
	const std::vector<std::string>& want = zh ? d.zh : d.en;
	const std::vector<std::string>& fallback = zh ? d.en : d.zh;
	const bool useWant = !want.empty();
	e.texts = useWant ? want : fallback;
	e.hidden = useWant ? (zh ? d.hiddenZh : d.hiddenEn) : (zh ? d.hiddenEn : d.hiddenZh);
}

// data.ts pageAmbient, appended (combine.ts:126-129).
void AppendAmbient(const RegexPageDef& p, Lang lang, RegexGen::Ambient& amb)
{
	const bool zh = (lang == Lang::Zh);
	const auto add = [](std::vector<std::string>& to, const std::vector<std::string>& from) {
		to.insert(to.end(), from.begin(), from.end());
	};
	add(amb.lines, zh ? p.ambientZh : p.ambientEn);
	add(amb.nameLeft, zh ? p.namePrefixZh : p.namePrefixEn);
	add(amb.nameRight, zh ? p.nameSuffixZh : p.nameSuffixEn);
}

} // namespace

// combine.ts:108 unionCorpus (the cached, several-page branch; :113 lookup,
// :132 unshift, :133 keep 4)
const UnionCorpusCache::Union& UnionCorpusCache::Get(const std::vector<const RegexPageDef*>& pages, Lang lang)
{
	for (const std::unique_ptr<Union>& u : items_)
		if (u->lang == lang && u->pages == pages) return *u;   // find() does not reorder
	auto u = std::make_unique<Union>();
	u->pages = pages;
	u->lang = lang;
	std::vector<RegexGen::Entry> entries;
	RegexGen::Ambient amb;
	for (const RegexPageDef* p : pages) {
		u->offsets.push_back((int)entries.size());
		for (const RegexEntryDef& d : p->entries) {
			RegexGen::Entry e;
			e.id = p->id + ":" + d.id;
			EntryLines(d, lang, e);
			entries.push_back(std::move(e));
			u->owner.emplace_back(p->id, d.id);
		}
		AppendAmbient(*p, lang, amb);
	}
	u->corpus.Reset(std::move(entries), std::move(amb));
	items_.insert(items_.begin(), std::move(u));
	if (items_.size() > 4) items_.pop_back();
	return *items_.front();
}

// combine.ts:148 combine
CombineResult Combine(Lang lang, RegexGen::Mode mode, const std::vector<CombineSel>& sels,
                      const std::vector<std::string>& customIn, const std::vector<std::string>& excludesIn,
                      UnionCorpusCache* cache)
{
	using RegexGen::Mode;
	CombineResult res;
	std::vector<std::string> anyTokens, allTerms, algoTerms, noneTokens, modTokens;
	struct AlgoFrag { std::string page, entry, frag; OwnLineFn own; };
	std::vector<AlgoFrag> algoFrags;
	// combine.ts:157-158 corpusSels / corpusUnresolved
	struct CorpusPart {
		const RegexPageDef* page;
		const RegexGen::Corpus* corpus;
		std::vector<int> picks, unresolved;
	};
	std::vector<CorpusPart> corpusSels;
	// Corpora built here for a sel that came without one (stable addresses).
	std::vector<std::unique_ptr<RegexGen::Corpus>> owned;
	int limit = 250;

	for (const CombineSel& sel : sels) {
		// combine.ts:163 dedupe, keep integers in range, sort
		const int n = (int)sel.page.Size();
		std::set<int> uniq;
		for (int i : sel.picks)
			if (i >= 0 && i < n) uniq.insert(i);
		const std::vector<int> picks(uniq.begin(), uniq.end());
		if (picks.empty()) continue;
		limit = std::min(limit, sel.page.Limit() ? sel.page.Limit() : 250);
		if (sel.page.algo) {
			// combine.ts:167-186 algorithmic page: each pick is a term of its own
			const AlgoPage& p = *sel.page.algo;
			PageContribution c;
			c.id = p.id;
			c.kind = p.kind;
			c.picked = (int)picks.size();
			for (int i : picks) {
				const AlgoEntry& e = p.entries[i];
				const AlgoValue& v = sel.values ? ValueOf(*sel.values, e) : e.input.def;
				const std::optional<std::string> f = e.fragment ? e.fragment(v, lang) : std::nullopt;
				if (!f) {
					c.unresolved++;
					const std::vector<std::string>& names = lang == Lang::Zh ? e.def.zh : e.def.en;
					res.conflicts.push_back({ConflictKind::Invalid, p.id, e.def.id, names.empty() ? e.def.id : names[0]});
					continue;
				}
				c.fragments.push_back(*f);
				algoTerms.push_back(QuoteIfNeeded(*f));
				algoFrags.push_back({p.id, e.def.id, *f, e.ownLine});
				c.length += RegexGen::CharCount(QuoteIfNeeded(*f)) + 1;
			}
			res.perPage.push_back(std::move(c));
			continue;
		}
		// combine.ts:188-199 corpus page
		const RegexPageDef& p = *sel.page.corpus;
		if (p.kind != RegexPageKind::Mods && p.kind != RegexPageKind::Names) continue;   // isCorpusPage
		const RegexGen::Corpus* corpus = sel.corpus;
		if (!corpus) {
			owned.push_back(std::make_unique<RegexGen::Corpus>());
			BuildPageCorpus(p, lang, *owned.back());
			corpus = owned.back().get();
		}
		RegexGen::Result r = corpus->Build(picks, mode);
		modTokens.insert(modTokens.end(), r.tokens.begin(), r.tokens.end());
		if (mode == Mode::Any) anyTokens.insert(anyTokens.end(), r.tokens.begin(), r.tokens.end());
		else if (mode == Mode::None) noneTokens.insert(noneTokens.end(), r.tokens.begin(), r.tokens.end());
		else for (const std::string& t : r.tokens) allTerms.push_back(QuoteIfNeeded(t));
		PageContribution c;
		c.id = p.id;
		c.kind = p.kind;
		c.picked = (int)picks.size();
		c.unresolved = (int)r.unresolved.size();
		c.fragments = r.tokens;
		for (const std::string& t : r.tokens)
			c.length += RegexGen::CharCount(mode == Mode::All ? QuoteIfNeeded(t) : t) + 1;
		res.perPage.push_back(std::move(c));
		corpusSels.push_back({&p, corpus, picks, r.unresolved});
		if (!res.hasCorpus) {
			res.corpusResult = std::move(r);
			res.hasCorpus = true;
		}
	}

	// combine.ts:202 custom text: escaped, a term of its own, unverified
	for (const std::string& t : customIn) {
		CustomTerm c{t, EscapeTerm(t)};
		if (!c.term.empty()) res.custom.push_back(std::move(c));
	}
	// combine.ts:204-205 excludes join the none term (a leading '!' is escaped
	// too, or the first exclude would stack with the term's own '!' into "!!")
	for (const std::string& t : excludesIn) {
		ExcludeToken x{t, EscapeTerm(t)};
		if (!x.token.empty()) res.excludes.push_back(std::move(x));
	}
	for (const ExcludeToken& x : res.excludes) noneTokens.push_back(x.token);

	// combine.ts:207-211 order: any, all, algorithmic, custom, none
	std::vector<std::string> terms;
	if (!anyTokens.empty()) {
		std::string t = "\"";
		Join(t, anyTokens, "|");
		terms.push_back(t + "\"");
	}
	terms.insert(terms.end(), allTerms.begin(), allTerms.end());
	terms.insert(terms.end(), algoTerms.begin(), algoTerms.end());
	for (const CustomTerm& c : res.custom) terms.push_back(QuoteIfNeeded(c.term));
	if (!noneTokens.empty()) {
		std::string t = "\"!";
		Join(t, noneTokens, "|");
		terms.push_back(t + "\"");
	}
	Join(res.query, terms, " ");

	// combine.ts:91 modQuery
	if (!modTokens.empty()) {
		if (mode == Mode::All) {
			std::vector<std::string> q;
			for (const std::string& t : modTokens) q.push_back(QuoteIfNeeded(t));
			Join(res.verifyQuery, q, " ");
		} else {
			res.verifyQuery = mode == Mode::None ? "\"!" : "\"";
			Join(res.verifyQuery, modTokens, "|");
			res.verifyQuery += "\"";
		}
	}

	// combine.ts:213-266 verification over the union of the corpus pages
	res.check.ok = true;
	if (!corpusSels.empty()) {
		// combine.ts:109-111 one page: its own corpus; several: the (cached) union
		const RegexGen::Corpus* corpus = nullptr;
		std::vector<int> offsets;
		std::vector<std::pair<std::string, std::string>> ownerOne;
		const std::vector<std::pair<std::string, std::string>>* owner = nullptr;
		UnionCorpusCache localCache;
		if (corpusSels.size() == 1) {
			corpus = corpusSels[0].corpus;
			offsets = {0};
			for (const RegexEntryDef& d : corpusSels[0].page->entries) ownerOne.emplace_back(corpusSels[0].page->id, d.id);
			owner = &ownerOne;
		} else {
			std::vector<const RegexPageDef*> pages;
			for (const CorpusPart& s : corpusSels) pages.push_back(s.page);
			const UnionCorpusCache::Union& u = (cache ? *cache : localCache).Get(pages, lang);
			corpus = &u.corpus;
			offsets = u.offsets;
			owner = &u.owner;
		}
		std::vector<int> selected;
		std::set<int> unresolvedSet;
		for (size_t k = 0; k < corpusSels.size(); k++) {
			for (int i : corpusSels[k].picks) selected.push_back(offsets[k] + i);
			for (int i : corpusSels[k].unresolved) unresolvedSet.insert(offsets[k] + i);
		}
		res.check = corpus->Verify(selected, res.verifyQuery);
		// combine.ts:227 `u.corpus.at(i).texts[0] ?? entry`
		auto textOf = [&](int i) {
			const RegexGen::Entry& e = corpus->At(i);
			return e.texts.empty() ? (*owner)[i].second : e.texts[0];
		};
		for (int i : res.check.extra)
			res.conflicts.push_back({ConflictKind::Extra, (*owner)[i].first, (*owner)[i].second, textOf(i)});
		for (int i : res.check.missing) {
			if (unresolvedSet.count(i)) continue;
			res.conflicts.push_back({ConflictKind::Missing, (*owner)[i].first, (*owner)[i].second, textOf(i)});
		}
		for (const std::string& a : res.check.ambient)
			res.conflicts.push_back({ConflictKind::Ambient, std::string(), std::string(), a});

		// combine.ts:238-247 the union's lines with '#' instantiated (:138 SAMPLES,
		// :140 instantiate); only built when something needs them.
		struct Line { int idx; std::u32string cps; std::string text; const std::string* raw; };
		std::vector<Line> lines;
		bool linesReady = false;
		auto getLines = [&]() -> const std::vector<Line>& {
			if (linesReady) return lines;
			linesReady = true;
			static const char* const kSamples[] = {"1", "5", "10", "16", "20", "30", "50", "80", "100", "150", "300"};
			for (size_t i = 0; i < corpus->Size(); i++) {
				const RegexGen::Entry& e = corpus->At(i);
				for (const auto* list : {&e.texts, &e.hidden}) {
					for (const std::string& l : *list) {
						if (l.find('#') == std::string::npos) {
							lines.push_back({(int)i, {}, l, &l});
							continue;
						}
						for (const char* s : kSamples) {
							std::string t;
							for (char ch : l) {
								if (ch == '#') t += s;
								else t += ch;
							}
							lines.push_back({(int)i, {}, std::move(t), &l});
						}
					}
				}
			}
			for (Line& l : lines) RxDecodeUtf8(l.text, l.cps);
			return lines;
		};
		// combine.ts:248-253 an algorithmic fragment that hits a union line (not its own)
		for (const AlgoFrag& f : algoFrags) {
			std::string err;
			const std::optional<Rx> rx = RxCompile(f.frag, &err);   // safeRegExp(src) with 'i'
			if (!rx) continue;
			for (const Line& l : getLines()) {
				if (RxSearchCps(*rx, l.cps) != RxStatus::Match) continue;
				if (f.own && f.own(*l.raw)) continue;
				res.conflicts.push_back({ConflictKind::Fragment, f.page, f.entry, f.frag + u8" ⇐ " + l.text});
				break;
			}
		}
		// combine.ts:254-265 an exclude that hits a ticked line contradicts itself (any / all)
		if (mode != Mode::None) {
			const std::set<int> picked(selected.begin(), selected.end());
			for (const ExcludeToken& x : res.excludes) {
				std::string err;
				const std::optional<Rx> rx = RxCompile(x.token, &err);
				if (!rx) continue;
				for (const Line& l : getLines()) {
					if (!picked.count(l.idx)) continue;
					if (RxSearchCps(*rx, l.cps) != RxStatus::Match) continue;
					res.conflicts.push_back({ConflictKind::Exclude, (*owner)[l.idx].first, (*owner)[l.idx].second,
					                         x.text + u8" ⇐ " + l.text});
					break;
				}
			}
		}
	}

	// combine.ts:268-282
	for (const CustomTerm& c : res.custom) res.customLength += RegexGen::CharCount(QuoteIfNeeded(c.term)) + 1;
	for (const ExcludeToken& x : res.excludes) res.excludesLength += RegexGen::CharCount(x.token) + 1;
	res.length = RegexGen::CharCount(res.query);
	res.limit = limit;
	res.ok = res.conflicts.empty() && res.check.missing.empty();
	return res;
}

} // namespace RegexAlgo
