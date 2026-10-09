#include "regex_algo_pages.h"

#include "regex_match.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <set>

// Port of exile-appraiser regex/src: pages/numeric-pages.ts, pages/vendor-pages.ts,
// pages/index.ts, sections.ts, view.ts, combine.ts, rarity.ts. File:line in the
// comments refer to those files at 0155244, except the step-40 parts (rarity.ts,
// the rarity | corruption row, condition sections, multi-term rows in combine):
// those cite exile-appraiser d5ccb47 (B worktree branch
// claude/realtime-currency-rates-60c13d, not yet on B main as of 2026-10-07).

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

struct OptKey { const char* id; const char* key; };

// rarity.ts:19-27 (step 40, B d5ccb47)
const char* const kRarityLabelKey = "ItemDisplayStringRarity";
const char* const kCorruptedLabelKey = "ItemPopupCorrupted";
struct RarityOpt { const char* id; char letter; const char* key; };
const RarityOpt kRarityOptions[] = {
	{"normal", 'n', "ItemDisplayStringNormal"},
	{"magic", 'm', "ItemDisplayStringMagic"},
	{"rare", 'r', "ItemDisplayStringRare"},
	{"unique", 'u', "ItemDisplayStringUnique"},
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
		const int top = s.hi;   // R10: >= N becomes N..top
		e.fragment = [digits, top](const AlgoValue& v, Lang lang) { return RegexFrag::MapTierFragment(v, digits, lang, top); };
		e.ownLine = [](const std::string& l) { return RegexFrag::IsTierNameLine(l); };
	} else {
		const std::string lz = *zh, le = *en;
		e.fragment = [lz, le, digits, percent](const AlgoValue& v, Lang lang) {
			return RegexFrag::StrictPropertyFragment(lang == Lang::Zh ? lz : le, v, digits, percent);
		};
	}
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

// combine.ts:81 quoteIfNeeded (step 40, B d5ccb47): a term starting
// with '!' (the "uncorrupted" row) is quoted too, so the '!' covers the whole term.
std::string QuoteIfNeeded(const std::string& t)
{
	return (t.find(' ') != std::string::npos || (!t.empty() && t[0] == '!')) ? "\"" + t + "\"" : t;
}

} // namespace

const char* const kRarityEntryId = "item_rarity_class";

// numeric-pages.ts:52 NUMERIC_LABEL_KEYS (step 40: ...RARITY_LABEL_KEYS, B d5ccb47)
std::vector<std::string> NumericLabelKeys(const std::string& game)
{
	std::vector<std::string> out;
	if (game == "poe1") for (const NumSpec& s : kPoe1Map) out.push_back(s.key);
	else for (const NumSpec& s : kPoe2Waystone) out.push_back(s.key);
	for (const std::string& k : RarityLabelKeys()) out.push_back(k);
	return out;
}

// vendor-pages.ts:21 VENDOR_LABEL_KEYS (step 40, B d5ccb47)
std::vector<std::string> VendorLabelKeys(const std::string& game)
{
	const std::vector<std::string>& rk = RarityLabelKeys();
	if (game != "poe1") {
		std::vector<std::string> out = {"ItemLevelPopup", "Quality", "Level"};
		out.insert(out.end(), rk.begin(), rk.end());
		return out;
	}
	std::vector<std::string> out = {"ItemLevelPopup", "Quality", "ItemDisplayStringSockets", "Level"};
	out.insert(out.end(), rk.begin(), rk.end());
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
	// numeric-pages.ts:89 (step 40, B d5ccb47): the rarity row is the
	// rarity | corruption condition row now; id and default unchanged -> old values
	// read the same and give the same output.
	if (auto r = RarityConditionEntry(labels, kRarityEntryId, "rare")) entries.push_back(std::move(*r));
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
	// vendor-pages.ts:105 (step 40, B d5ccb47): the old "corrupted"
	// tick row became the rarity | corruption row; id still `corrupted`, default
	// "|c" (only corrupted) -> old bookmarks / codes give the same "^已汙染$".
	if (auto cond = RarityConditionEntry(labels, "corrupted", "|c", 1)) entries.push_back(std::move(*cond));
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

// pages/index.ts:23 algoPages (step 40, B d5ccb47: + condition sections)
std::vector<AlgoPage> AlgoPages(const std::string& game, const RegexLabels* labels)
{
	std::vector<AlgoPage> out = NumericPages(game, labels);
	for (AlgoPage& p : VendorPages(game, labels)) out.push_back(std::move(p));
	for (AlgoPage& p : ConditionSections(game, labels)) out.push_back(std::move(p));
	return out;
}

// ---- sections.ts -------------------------------------------------------------

namespace {
// sections.ts:20 SECTION_HOSTS (step 40, B d5ccb47: + the four
// condition sections), in the TS object's order.
const std::pair<const char*, const char*> kSectionHosts[] = {
	{"map_numeric", "map_mods"},
	{"waystone_numeric", "waystone_mods"},
	{"vendor_bases_cond", "vendor_bases"},
	{"tablet_mods_cond", "tablet_mods"},
	{"item_mod_values_cond", "item_mod_values"},
	{"item_mod_values_poe2_cond", "item_mod_values_poe2"},
};
} // namespace

// sections.ts:30 sectionHostOf
std::string SectionHostOf(const std::string& pageId)
{
	for (const auto& h : kSectionHosts)
		if (pageId == h.first) return h.second;
	return std::string();
}

// sections.ts:35 sectionIdOf: the first section naming this host.
std::string SectionIdOf(const std::string& hostId)
{
	for (const auto& h : kSectionHosts)
		if (hostId == h.second) return h.first;
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
	// view.ts:112 (step 40, B d5ccb47): "魔法、稀有 · 未汙染"
	if (e.input.kind == InputKind::Rarity) return RarityConditionText(e, v, lang);
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
		// Other wordings of the same modifier (RegexGen::Entry::alts), same language rule.
		e.alts = useWant ? (zh ? d.altZh : d.altEn) : (zh ? d.altEn : d.altZh);
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
	case ConflictKind::ConditionClash: return "conditionClash";
	}
	return "?";
}

// R10: one search string describes one kind of item; equipment pages (bases,
// vendor conditions, modifier values, flasks / charms) describe the same items.
std::string ItemGroupOf(const std::string& pageId)
{
	static const char* const kEquipment[] = {"vendor_bases",         "vendor_items", "vendor_items_poe2", "item_mod_values",
	                                         "item_mod_values_poe2", "flask_mods",   "flask_charm_mods",
	                                         "gem_names"};   // gems: with the vendor page's gem level / quality
	const std::string host = NumericKeyOf(pageId);
	for (const char* e : kEquipment)
		if (host == e) return "equipment";
	return host;
}

MergePlan PlanMerge(const std::string& currentId, const std::vector<std::string>& pickedIds)
{
	MergePlan p;
	const std::string group = ItemGroupOf(currentId);
	std::vector<std::string> skippedHosts;
	for (const std::string& id : pickedIds) {
		if (ItemGroupOf(id) == group) {
			p.merged.push_back(id);
			continue;
		}
		const std::string host = NumericKeyOf(id);   // host + its section = one page
		if (std::find(skippedHosts.begin(), skippedHosts.end(), host) == skippedHosts.end()) skippedHosts.push_back(host);
	}
	p.skippedPages = (int)skippedHosts.size();
	return p;
}

// class-term.ts (exile-appraiser 01ac96e): a page's fragments only promise not to
// hit the page's OTHER modifiers, but a stash / shop search scans every item (a
// jewel's 「範圍效果」 holds 「圍」). So a page whose items all carry a word in their
// name gets that word as one more AND term. Only the tablet page so far: the eight
// tablet bases all end in 「碑牌」 / "Tablet".
// class-term.ts:10 CLASS_TERMS
std::string ClassTermOf(const RegexPageDef& page, Lang lang)
{
	if (page.game == "poe2" && page.id == "tablet_mods") return lang == Lang::Zh ? u8"碑牌" : "tablet";
	return std::string();
}

// class-term.ts:20 sharedClassTerm
std::string SharedClassTerm(const std::vector<const RegexPageDef*>& pages, Lang lang)
{
	if (pages.empty()) return std::string();
	const std::string first = ClassTermOf(*pages[0], lang);
	if (first.empty()) return std::string();
	for (const RegexPageDef* p : pages)
		if (ClassTermOf(*p, lang) != first) return std::string();
	return first;
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
	e.alts = useWant ? (zh ? d.altZh : d.altEn) : (zh ? d.altEn : d.altZh);
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
	// Verify-only (Combine never calls Build on the union; each page builds its
	// tokens from its own corpus), so no token index: re-indexing every ticked
	// page whenever the set of ticked pages changed was the half-second freeze
	// on ticking a box once a gem list was in the merge.
	RegexGen::Options opt;
	opt.index = false;
	u->corpus.Reset(std::move(entries), std::move(amb), opt);
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
		bool hasTokens;   // produced at least one token (the class term only counts these)
	};
	std::vector<CorpusPart> corpusSels;
	// Corpora built here for a sel that came without one (stable addresses).
	std::vector<std::unique_ptr<RegexGen::Corpus>> owned;
	int limit = 250;
	// R10: the rarity | corruption rows of every page taking part, merged into
	// one choice after the loop; their terms go where the first one stood.
	// The merged rarity term goes where the first row restricting rarity stood, the
	// corruption term where the first row with a corruption answer stood (so a
	// single row prints exactly where it always did).
	struct CondRow {
		const AlgoEntry* e;
		std::string page;
		RarityChoice choice;
		std::string text;
		size_t termPos;    // index into algoTerms
		size_t perPage;    // index into res.perPage
		size_t fragPos;    // index into that page's fragments
	};
	std::vector<CondRow> condRows;
	// R10: every corpus page in `sels`, ticked or not (the host of a ticked
	// section takes part with no picks): what the short condition terms are
	// checked against.
	std::vector<const RegexPageDef*> guardPages;
	for (const CombineSel& sel : sels)
		if (sel.page.corpus && std::find(guardPages.begin(), guardPages.end(), sel.page.corpus) == guardPages.end())
			guardPages.push_back(sel.page.corpus);

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
				if (e.condTerms) {
					// R10: a rarity | corruption row that says nothing is invalid as
					// before; one that does waits for the merge.
					if (!e.terms || !e.terms(v, lang)) {
						c.unresolved++;
						const std::vector<std::string>& names = lang == Lang::Zh ? e.def.zh : e.def.en;
						res.conflicts.push_back({ConflictKind::Invalid, p.id, e.def.id, names.empty() ? e.def.id : names[0]});
						continue;
					}
					condRows.push_back({&e, p.id, ParseRarityChoice(v.choice), RarityConditionText(e, v, lang).value_or(std::string()),
					                    algoTerms.size(), res.perPage.size(), c.fragments.size()});
					continue;
				}
				// combine.ts:174-181 (step 40, B d5ccb47): a row may give
				// several terms (rarity | corruption), each an AND term of its own
				std::optional<std::vector<std::string>> ts;
				if (e.terms) ts = e.terms(v, lang);
				else if (e.fragment) {
					if (std::optional<std::string> f = e.fragment(v, lang)) ts = std::vector<std::string>{*f};
				}
				if (!ts || ts->empty()) {
					c.unresolved++;
					const std::vector<std::string>& names = lang == Lang::Zh ? e.def.zh : e.def.en;
					res.conflicts.push_back({ConflictKind::Invalid, p.id, e.def.id, names.empty() ? e.def.id : names[0]});
					continue;
				}
				for (const std::string& f : *ts) {
					// R10: the same term twice (two pages asking the same) is said once
					if (std::find(algoTerms.begin(), algoTerms.end(), QuoteIfNeeded(f)) != algoTerms.end()) continue;
					c.fragments.push_back(f);
					algoTerms.push_back(QuoteIfNeeded(f));
					algoFrags.push_back({p.id, e.def.id, f, e.ownLine});
					c.length += RegexGen::CharCount(QuoteIfNeeded(f)) + 1;
				}
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
		corpusSels.push_back({&p, corpus, picks, r.unresolved, !r.tokens.empty()});
		if (!res.hasCorpus) {
			res.corpusResult = std::move(r);
			res.hasCorpus = true;
		}
	}

	// R10: the condition rows merged. Rarity: the sets intersected (a row with
	// none or all four does not restrict); corruption: one answer. A clash is a
	// conflict and no string is made.
	bool condClash = false;
	if (!condRows.empty()) {
		const size_t nRarity = condRows[0].e->input.options.size();
		RarityChoice merged;
		bool restricted = false;
		const CondRow* firstRestrict = nullptr;
		const CondRow* firstCorrupt = nullptr;
		for (const CondRow& r : condRows) {
			const RarityChoice& ch = r.choice;
			if (!ch.rarity.empty() && ch.rarity.size() < nRarity) {
				if (!restricted) {
					merged.rarity = ch.rarity;
					restricted = true;
					firstRestrict = &r;
				} else {
					std::vector<std::string> keep;
					for (const std::string& id : merged.rarity)
						if (std::find(ch.rarity.begin(), ch.rarity.end(), id) != ch.rarity.end()) keep.push_back(id);
					if (keep.empty() && !condClash) {
						condClash = true;
						res.conflicts.push_back({ConflictKind::ConditionClash, r.page, r.e->def.id, firstRestrict->text + u8" ↔ " + r.text});
					}
					merged.rarity = std::move(keep);
				}
			}
			if (ch.corruption != Corruption::None) {
				if (merged.corruption == Corruption::None) {
					merged.corruption = ch.corruption;
					firstCorrupt = &r;
				} else if (merged.corruption != ch.corruption && !condClash) {
					condClash = true;
					res.conflicts.push_back({ConflictKind::ConditionClash, r.page, r.e->def.id, firstCorrupt->text + u8" ↔ " + r.text});
				}
			}
		}
		if (!condClash) {
			// The guard: a candidate hits a line of the corpus pages (the row's own
			// "已汙染" line aside), '#' tried with the usual sample numbers.
			std::vector<std::pair<std::u32string, std::string>> glines;   // instantiated, raw
			bool itemText = false;
			for (const RegexPageDef* gp : guardPages) {
				itemText |= gp->kind == RegexPageKind::Mods;
				std::vector<std::string> raw;
				for (const RegexEntryDef& d : gp->entries) {
					RegexGen::Entry e;
					EntryLines(d, lang, e);
					raw.insert(raw.end(), e.texts.begin(), e.texts.end());
					raw.insert(raw.end(), e.hidden.begin(), e.hidden.end());
					raw.insert(raw.end(), e.alts.begin(), e.alts.end());   // the other wordings print on items too
				}
				RegexGen::Ambient amb;
				AppendAmbient(*gp, lang, amb);
				raw.insert(raw.end(), amb.lines.begin(), amb.lines.end());
				raw.insert(raw.end(), amb.nameLeft.begin(), amb.nameLeft.end());
				raw.insert(raw.end(), amb.nameRight.begin(), amb.nameRight.end());
				static const char* const kSamples[] = {"1", "5", "10", "16", "20", "30", "50", "80", "100", "150", "300"};
				for (const std::string& l : raw) {
					if (l.find('#') == std::string::npos) {
						glines.emplace_back(std::u32string(), l);
						RxDecodeUtf8(l, glines.back().first);
						continue;
					}
					for (const char* smp : kSamples) {
						std::string t;
						for (char ch : l) {
							if (ch == '#') t += smp;
							else t += ch;
						}
						glines.emplace_back(std::u32string(), l);
						RxDecodeUtf8(t, glines.back().first);
					}
				}
			}
			const AlgoEntry* ce = condRows[0].e;
			CondGuard guard;
			guard.itemText = itemText;
			guard.hits = [&glines, ce](const std::string& frag) {
				const std::optional<Rx> rx = RxCompile(frag, nullptr);
				if (!rx) return true;   // not a usable fragment: take the long form
				for (const auto& gl : glines) {
					if (ce->ownLine && ce->ownLine(gl.second)) continue;
					if (RxSearchCps(*rx, gl.first) == RxStatus::Match) return true;
				}
				return false;
			};
			if (const std::optional<std::vector<std::string>> ts = ce->condTerms(merged, lang, guardPages.empty() ? nullptr : &guard)) {
				// condTerms answers [rarity?, corruption?]
				const bool hasRarity = restricted && !merged.rarity.empty();
				struct Put { std::string f; const CondRow* at; bool rarity; };
				std::vector<Put> puts;
				for (size_t k = 0; k < ts->size(); k++) {
					const bool isRarity = hasRarity && k == 0;
					const CondRow* at = isRarity ? firstRestrict : firstCorrupt;
					puts.push_back({(*ts)[k], at ? at : &condRows[0], isRarity});
				}
				// later positions first, so the earlier indices stay valid; on a tie the
				// later row goes in first (ends up after), and within one row the
				// corruption term goes in first (ends up after the rarity term)
				std::sort(puts.begin(), puts.end(), [](const Put& a, const Put& b) {
					if (a.at->termPos != b.at->termPos) return a.at->termPos > b.at->termPos;
					if (a.at != b.at) return a.at > b.at;
					return !a.rarity && b.rarity;
				});
				for (const Put& u : puts) {
					const std::string q = QuoteIfNeeded(u.f);
					if (std::find(algoTerms.begin(), algoTerms.end(), q) != algoTerms.end()) continue;
					algoTerms.insert(algoTerms.begin() + (std::ptrdiff_t)std::min(u.at->termPos, algoTerms.size()), q);
					PageContribution& c = res.perPage[u.at->perPage];
					c.fragments.insert(c.fragments.begin() + (std::ptrdiff_t)std::min(u.at->fragPos, c.fragments.size()), u.f);
					c.length += RegexGen::CharCount(q) + 1;
					algoFrags.push_back({u.at->page, u.at->e->def.id, u.f, ce->ownLine});
				}
			}
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

	// combine.ts:223-224 the ticked corpus pages' shared item-class term, quoted
	{
		std::vector<const RegexPageDef*> classPages;
		// only pages that produced a token: a page whose picks all went unresolved
		// must not leave a bare "碑牌" that lights every tablet
		for (const CorpusPart& s : corpusSels)
			if (s.hasTokens) classPages.push_back(s.page);
		const std::string shared = SharedClassTerm(classPages, lang);
		if (!shared.empty()) res.classTerm = "\"" + shared + "\"";
	}
	// combine.ts:225-231 order: any, all, algorithmic, class, custom, none
	std::vector<std::string> terms;
	if (!anyTokens.empty()) {
		std::string t = "\"";
		Join(t, anyTokens, "|");
		terms.push_back(t + "\"");
	}
	terms.insert(terms.end(), allTerms.begin(), allTerms.end());
	terms.insert(terms.end(), algoTerms.begin(), algoTerms.end());
	if (!res.classTerm.empty()) terms.push_back(res.classTerm);
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
			// combine.ts:261 (step 40, B d5ccb47): a negated term ("!...") never "hits" a line
			if (!f.frag.empty() && f.frag[0] == '!') continue;
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
	if (condClash) res.query.clear();   // R10: conditions that cannot both hold: no string
	res.length = RegexGen::CharCount(res.query);
	res.limit = limit;
	res.ok = res.conflicts.empty() && res.check.missing.empty();
	return res;
}

// ---- rarity.ts (step 40) ------------------------------------------------------------
//
// exile-appraiser regex/src/rarity.ts @ d5ccb47 (B worktree branch
// claude/realtime-currency-rates-60c13d, not yet on B main as of 2026-10-07). One row, two button groups
// "普通 魔法 稀有 傳奇 | 未汙染 已汙染": rarities multi-select, corruption one of
// two (or neither). Value in AlgoValue.choice: rarity letters n / m / r / u (in
// that order) + optional "|u" (uncorrupted) / "|c" (corrupted); the step-35 single
// words normal / magic / rare / unique read as that one. Two AND terms:
//   rarity:     one picked = rarityFragment (step 35 as is); several =
//               "稀有度[:：] *(魔法|稀有)" in the fixed order; all four = no term
//   corruption: corrupted = "^已汙染$" (whole line); uncorrupted = "!^已汙染$"

// rarity.ts:28 RARITY_LABEL_KEYS
const std::vector<std::string>& RarityLabelKeys()
{
	static const std::vector<std::string> keys = [] {
		std::vector<std::string> k = {kRarityLabelKey};
		for (const RarityOpt& o : kRarityOptions) k.push_back(o.key);
		k.push_back(kCorruptedLabelKey);
		return k;
	}();
	return keys;
}

const char* CorruptionId(Corruption c)
{
	return c == Corruption::Uncorrupted ? "uncorrupted" : c == Corruption::Corrupted ? "corrupted" : "";
}

// rarity.ts:39 parseRarityChoice
RarityChoice ParseRarityChoice(const std::string& choice)
{
	RarityChoice out;
	const std::string c = JsTrim(choice);
	for (const RarityOpt& o : kRarityOptions)
		if (c == o.id) {
			out.rarity.push_back(o.id);
			return out;
		}
	// Only the complete format /^[nmru]*(\|[uc])?$/: a bad value is not read in
	// part ("nope" must not become "normal").
	size_t i = 0;
	while (i < c.size() && (c[i] == 'n' || c[i] == 'm' || c[i] == 'r' || c[i] == 'u')) i++;
	const std::string letters = c.substr(0, i);
	char corr = 0;
	if (i < c.size()) {
		if (!(c.size() == i + 2 && c[i] == '|' && (c[i + 1] == 'u' || c[i + 1] == 'c'))) return out;
		corr = c[i + 1];
	}
	for (const RarityOpt& o : kRarityOptions)
		if (letters.find(o.letter) != std::string::npos) out.rarity.push_back(o.id);
	out.corruption = corr == 'u' ? Corruption::Uncorrupted : corr == 'c' ? Corruption::Corrupted : Corruption::None;
	return out;
}

// rarity.ts:52 encodeRarityChoice
std::string EncodeRarityChoice(const RarityChoice& c)
{
	std::string letters;
	for (const RarityOpt& o : kRarityOptions)
		if (std::find(c.rarity.begin(), c.rarity.end(), o.id) != c.rarity.end()) letters += o.letter;
	if (c.corruption == Corruption::None) return letters;
	return letters + (c.corruption == Corruption::Uncorrupted ? "|u" : "|c");
}

// rarity.ts:58 toggleRarityIn
std::string ToggleRarityIn(const std::string& choice, const std::string& id)
{
	RarityChoice c = ParseRarityChoice(choice);
	auto it = std::find(c.rarity.begin(), c.rarity.end(), id);
	if (it != c.rarity.end()) c.rarity.erase(it);
	else c.rarity.push_back(id);
	return EncodeRarityChoice(c);
}

// rarity.ts:65 toggleCorruptionIn
std::string ToggleCorruptionIn(const std::string& choice, Corruption corruption)
{
	RarityChoice c = ParseRarityChoice(choice);
	c.corruption = c.corruption == corruption ? Corruption::None : corruption;
	return EncodeRarityChoice(c);
}

namespace {

// rarity.ts:70 ConditionLabels / :76 conditionLabels
struct ConditionLabels {
	ZhEn label;
	std::vector<AlgoOption> rarity;
	ZhEn corrupted;
};

std::optional<ConditionLabels> ConditionLabelsOf(const RegexLabels* labels)
{
	if (!Usable(labels)) return std::nullopt;
	const std::optional<ZhEn> label = L(*labels, kRarityLabelKey);
	const std::optional<ZhEn> corrupted = L(*labels, kCorruptedLabelKey);
	if (!label || !corrupted) return std::nullopt;
	ConditionLabels out;
	out.label = *label;
	out.corrupted = *corrupted;
	for (const RarityOpt& o : kRarityOptions)
		if (const std::optional<ZhEn> t = L(*labels, o.key)) out.rarity.push_back({o.id, t->zh, t->en});
	if (out.rarity.empty()) return std::nullopt;
	return out;
}

void AppendCp(std::string& out, char32_t c)
{
	if (c < 0x80) {
		out += (char)c;
	} else if (c < 0x800) {
		out += (char)(0xC0 | (c >> 6));
		out += (char)(0x80 | (c & 0x3F));
	} else if (c < 0x10000) {
		out += (char)(0xE0 | (c >> 12));
		out += (char)(0x80 | ((c >> 6) & 0x3F));
		out += (char)(0x80 | (c & 0x3F));
	} else {
		out += (char)(0xF0 | (c >> 18));
		out += (char)(0x80 | ((c >> 12) & 0x3F));
		out += (char)(0x80 | ((c >> 6) & 0x3F));
		out += (char)(0x80 | (c & 0x3F));
	}
}

char32_t LowerCp(char32_t c) { return (c >= U'A' && c <= U'Z') ? c + 32 : c; }

// A code point that is itself in a fragment (no regex syntax, no space).
bool PlainCp(char32_t c)
{
	static const std::u32string kSyntax = U"\\^$.|?*+()[]{}-! \"";
	return c > 0x20 && kSyntax.find(c) == std::u32string::npos;
}

// R10 short rarity term: "<last char of the label>: <first char of the value>"
// ("度: 稀", "y: r"), several values as a class in the fixed order ("度: [魔稀]").
// The game prints "稀有度: 稀有" / "Rarity: Rare" in both games. nullopt when the
// four values do not start with four different plain characters.
std::optional<std::string> ShortRarity(const ConditionLabels& l, const std::string& label, const RarityChoice& c, Lang lang)
{
	std::u32string base;
	RxDecodeUtf8(RegexFrag::LabelBase(label), base);
	if (base.empty() || !PlainCp(base.back())) return std::nullopt;
	std::u32string firsts, picked;
	for (const AlgoOption& o : l.rarity) {
		std::u32string v;
		RxDecodeUtf8(JsTrim(lang == Lang::Zh ? o.zh : o.en), v);
		if (v.empty() || !PlainCp(v[0])) return std::nullopt;
		const char32_t f = LowerCp(v[0]);
		if (firsts.find(f) != std::u32string::npos) return std::nullopt;
		firsts += f;
		if (std::find(c.rarity.begin(), c.rarity.end(), o.id) != c.rarity.end()) picked += f;
	}
	if (picked.empty()) return std::nullopt;
	std::string out;
	AppendCp(out, LowerCp(base.back()));
	out += ": ";
	if (picked.size() > 1) out += '[';
	for (char32_t f : picked) AppendCp(out, f);
	if (picked.size() > 1) out += ']';
	return out;
}

// R10 short corruption term: the shortest prefix of "已汙染" / "Corrupted" (at
// least 2 CJK / 4 Latin characters: a shorter one is in too much other item text)
// that hits no other line of the corpus ("已汙", "corr"); nullopt = none is safe.
std::optional<std::string> ShortCorrupted(const std::string& label, const CondGuard& g)
{
	std::u32string base;
	RxDecodeUtf8(RegexFrag::LabelBase(label), base);
	bool ascii = true;
	for (char32_t ch : base) ascii &= ch < 0x80;
	const size_t minLen = ascii ? 4 : 2;
	std::string cand;
	for (size_t k = 0; k + 1 < base.size(); k++) {
		if (!PlainCp(base[k])) return std::nullopt;
		AppendCp(cand, LowerCp(base[k]));
		if (k + 1 < minLen) continue;
		if (!g.hits || !g.hits(cand)) return cand;
	}
	return std::nullopt;
}

// rarity.ts:94 conditionTerms: the rarity term, the corruption term; neither = nullopt.
// R10: shortened to what the game prints, unless the guard says the short form
// hits a corpus line ("退回完整寫法"); the corruption prefix only against a
// modifier corpus (the item's own lines), else the whole line "^已汙染$".
std::optional<std::vector<std::string>> ConditionTermsFor(const ConditionLabels& l, const RarityChoice& c, Lang lang, const CondGuard* g)
{
	std::vector<std::string> out;
	std::vector<std::string> vals;
	for (const AlgoOption& o : l.rarity)
		if (std::find(c.rarity.begin(), c.rarity.end(), o.id) != c.rarity.end()) vals.push_back(lang == Lang::Zh ? o.zh : o.en);
	if (!vals.empty() && vals.size() < l.rarity.size()) {
		const std::string& label = lang == Lang::Zh ? l.label.zh : l.label.en;
		std::optional<std::string> f = ShortRarity(l, label, c, lang);
		if (f && g && g->hits && g->hits(*f)) f.reset();
		if (!f) {
			if (vals.size() == 1) {
				f = RegexFrag::RarityFragment(label, vals[0]);
			} else {
				std::string alt;
				for (size_t i = 0; i < vals.size(); i++) alt += (i ? "|" : "") + JsTrim(vals[i]);
				f = RegexFrag::LabelBase(label) + u8"[:：] *(" + alt + ")";
			}
		}
		if (f) out.push_back(*f);
	}
	if (c.corruption != Corruption::None) {
		const std::string& label = lang == Lang::Zh ? l.corrupted.zh : l.corrupted.en;
		std::optional<std::string> f;
		if (g && g->itemText) f = ShortCorrupted(label, *g);
		if (!f) f = RegexFrag::WholeLine({label});
		if (f) out.push_back(c.corruption == Corruption::Uncorrupted ? "!" + *f : *f);
	}
	if (out.empty()) return std::nullopt;
	return out;
}

// The step-40 terms of one row on its own, no corpus: rarity short, corruption
// as the whole line. Used for the row display and the "does it say anything" test.
std::optional<std::vector<std::string>> ConditionTerms(const ConditionLabels& l, const AlgoValue& v, Lang lang)
{
	return ConditionTermsFor(l, ParseRarityChoice(v.choice), lang, nullptr);
}

} // namespace

// rarity.ts:115 rarityConditionEntry
std::optional<AlgoEntry> RarityConditionEntry(const RegexLabels* labels, const std::string& id, const std::string& def, int g)
{
	const std::optional<ConditionLabels> lo = ConditionLabelsOf(labels);
	if (!lo) return std::nullopt;
	const ConditionLabels l = *lo;
	const std::string corruptedZh = RegexFrag::LabelBase(l.corrupted.zh);
	const std::string corruptedEn = RegexFrag::LabelBase(l.corrupted.en);
	AlgoEntry e;
	e.def = BaseDef(id, g, RegexFrag::LabelBase(l.label.zh), RegexFrag::LabelBase(l.label.en));
	e.input.kind = InputKind::Rarity;
	e.input.options = l.rarity;
	// "未汙染" is interface text (the game only prints "已汙染"): drop a leading 已, add 未
	const std::string yi = u8"已";
	const std::string zhBase = corruptedZh.compare(0, yi.size(), yi) == 0 ? corruptedZh.substr(yi.size()) : corruptedZh;
	e.input.corruption = {
		{"uncorrupted", u8"未" + zhBase, "Not " + corruptedEn},
		{"corrupted", corruptedZh, corruptedEn},
	};
	e.input.def.choice = def;
	e.input.def.hasChoice = true;
	e.terms = [l](const AlgoValue& v, Lang lang) { return ConditionTerms(l, v, lang); };
	e.condTerms = [l](const RarityChoice& c, Lang lang, const CondGuard* g) { return ConditionTermsFor(l, c, lang, g); };
	e.fragment = [l](const AlgoValue& v, Lang lang) -> std::optional<std::string> {
		const std::optional<std::vector<std::string>> ts = ConditionTerms(l, v, lang);
		if (!ts) return std::nullopt;
		std::string out;
		Join(out, *ts, " ");
		return out;
	};
	// The corruption term matches the whole "已汙染" line: that corpus line itself
	// (ambient / hidden) is what it is meant to match.
	e.ownLine = [l](const std::string& line) { return line == l.corrupted.zh || line == l.corrupted.en; };
	return e;
}

// rarity.ts:147 CONDITION_SECTIONS
const std::vector<std::string>& ConditionSectionIds(const std::string& game)
{
	static const std::vector<std::string> poe1 = {"vendor_bases_cond", "item_mod_values_cond"};
	static const std::vector<std::string> poe2 = {"vendor_bases_cond", "tablet_mods_cond", "item_mod_values_poe2_cond"};
	return game == "poe2" ? poe2 : poe1;
}

// rarity.ts:153 isConditionSectionId
bool IsConditionSectionId(const std::string& id)
{
	for (const char* g : {"poe1", "poe2"}) {
		const std::vector<std::string>& ids = ConditionSectionIds(g);
		if (std::find(ids.begin(), ids.end(), id) != ids.end()) return true;
	}
	return false;
}

// rarity.ts:158 conditionSections: built even when the host is missing (combine
// order only follows hosts that exist).
std::vector<AlgoPage> ConditionSections(const std::string& game, const RegexLabels* labels)
{
	const std::optional<AlgoEntry> e = RarityConditionEntry(labels, kRarityEntryId, "r");
	if (!e) return {};
	std::vector<AlgoPage> out;
	for (const std::string& id : ConditionSectionIds(game)) {
		AlgoPage p;
		p.game = game;
		p.id = id;
		p.kind = RegexPageKind::Numeric;
		p.sectionOf = SectionHostOf(id);
		p.title = u8"稀有度 / 汙染";
		p.titleEn = "Rarity / corruption";
		p.note = u8"物品稀有度(可多選)與是否汙染,各自一個 term(同時成立);與這一頁的勾選合成同一條字串。";
		p.limit = 250;
		p.groups = {u8"條件"};
		p.groupsEn = {"Conditions"};
		p.entries = {*e};
		out.push_back(std::move(p));
	}
	return out;
}

// rarity.ts:183 rarityConditionText
std::optional<std::string> RarityConditionText(const AlgoEntry& e, const AlgoValue& v, Lang lang)
{
	if (e.input.kind != InputKind::Rarity) return std::nullopt;
	const RarityChoice c = ParseRarityChoice(v.choice);
	auto name = [lang](const AlgoOption& o) { return lang == Lang::En ? o.en : o.zh; };
	std::vector<std::string> parts;
	std::string r;
	int n = 0;
	for (const AlgoOption& o : e.input.options)
		if (std::find(c.rarity.begin(), c.rarity.end(), o.id) != c.rarity.end())
			r += (n++ ? std::string(lang == Lang::En ? ", " : u8"、") : std::string()) + name(o);
	if (n) parts.push_back(r);
	for (const AlgoOption& o : e.input.corruption)
		if (o.id == CorruptionId(c.corruption)) {
			parts.push_back(name(o));
			break;
		}
	if (parts.empty()) return std::nullopt;
	std::string out;
	Join(out, parts, u8" · ");
	return out;
}

} // namespace RegexAlgo
