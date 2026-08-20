#include "filter_item_import.h"

#include "filter_i18n.h"
#include "item_library.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace {

// ---- text utilities --------------------------------------------------------

std::string rtrim(std::string s)
{
	while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
		s.pop_back();
	return s;
}

std::vector<std::string> splitLines(const std::string& text)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= text.size()) {
		size_t nl = text.find('\n', pos);
		if (nl == std::string::npos) {
			out.push_back(rtrim(text.substr(pos)));
			break;
		}
		out.push_back(rtrim(text.substr(pos, nl - pos)));
		pos = nl + 1;
	}
	while (!out.empty() && out.back().empty()) out.pop_back();
	return out;
}

// A separator is a line of nothing but dashes (>= 4; 3.29 varies the width).
bool isSeparator(const std::string& l)
{
	if (l.size() < 4) return false;
	for (char c : l) if (c != '-') return false;
	return true;
}

bool startsWith(const std::string& s, const char* pfx)
{
	size_t n = strlen(pfx);
	return s.size() >= n && memcmp(s.data(), pfx, n) == 0;
}

// First integer in the string (handles "+29% (augmented)", "20 (Max)",
// "1,234/40000"); returns false when there is none.
bool firstInt(const std::string& s, int* out)
{
	size_t i = 0;
	while (i < s.size() && !(s[i] >= '0' && s[i] <= '9')) i++;
	if (i == s.size()) return false;
	long v = 0;
	for (; i < s.size(); i++) {
		if (s[i] >= '0' && s[i] <= '9') v = v * 10 + (s[i] - '0');
		else if (s[i] == ',') continue;      // thousands separator
		else break;
	}
	*out = (int)v;
	return true;
}

// "key: value" split on the FIRST ASCII colon. Returns false for lines
// without one. "Requirements:" yields an empty value.
bool splitKV(const std::string& l, std::string* key, std::string* val)
{
	size_t c = l.find(':');
	if (c == std::string::npos || c == 0) return false;
	*key = rtrim(l.substr(0, c));
	size_t v = c + 1;
	while (v < l.size() && l[v] == ' ') v++;
	*val = l.substr(v);
	return true;
}

// ---- canonical en tables (values match item_metadata.json's en side) -------

int rarityOf(const std::string& en)
{
	if (en == "Normal") return 0;
	if (en == "Magic") return 1;
	if (en == "Rare") return 2;
	if (en == "Unique") return 3;
	return -1;   // currency / gem / divination card / quest -> caller uses 0
}

struct StatusFx { const char* en; void (*apply)(ImportedItem&); };

const StatusFx kStatus[] = {
	{ "Corrupted",        [](ImportedItem& r) { r.item.corrupted = true; } },
	{ "Mirrored",         [](ImportedItem& r) { r.item.mirrored = true; } },
	{ "Fractured Item",   [](ImportedItem& r) { r.item.fractured = true; } },
	{ "Synthesised Item", [](ImportedItem& r) { r.item.synthesised = true; } },
	{ "Split",            [](ImportedItem& r) { (void)r; } },   // cosmetic, no condition
	{ "Foil",             [](ImportedItem& r) { (void)r; } },   // cosmetic, no condition
	{ "Foil Unique",      [](ImportedItem& r) { (void)r; } },   // cosmetic, no condition
	{ "Scourged",         [](ImportedItem& r) {
		r.warnings.push_back({ "", u8"災魘狀態:預覽的 Scourged 條件未模擬,一律視為否" }); } },
	{ "Transfigured",     [](ImportedItem& r) {
		r.warnings.push_back({ "", u8"分化寶石:預覽的 TransfiguredGem 條件未模擬,一律視為否" }); } },
};

struct InfluenceFx { const char* en; unsigned bit; };

const InfluenceFx kInfluence[] = {
	{ "Shaper Item",   kInfShaper },
	{ "Elder Item",    kInfElder },
	{ "Crusader Item", kInfCrusader },
	{ "Redeemer Item", kInfRedeemer },
	{ "Hunter Item",   kInfHunter },
	{ "Warlord Item",  kInfWarlord },
	{ "Searing Exarch Item",  0 },   // implicit-based; flagged, warned, unmodelled
	{ "Eater of Worlds Item", 0 },
};

// ---- base-name resolution ---------------------------------------------------

struct LibIndex {
	std::unordered_map<std::string, const LibItem*> exact;   // en AND zh -> item
	const std::vector<LibItem>* all = nullptr;
};

LibIndex buildIndex(const std::vector<LibItem>& lib)
{
	LibIndex ix;
	ix.all = &lib;
	ix.exact.reserve(lib.size() * 2);
	for (const LibItem& li : lib) {
		ix.exact.emplace(li.en, &li);
		if (li.zh != li.en) ix.exact.emplace(li.zh, &li);
	}
	return ix;
}

// Longest catalog name contained in `line` (magic items paste as one composed
// name). Longer match wins so "Studded Leather Belt" beats "Leather Belt";
// ties prefer a class matching `wantClassEn` (the "Item Class:" line).
const LibItem* longestSubstring(const LibIndex& ix, const std::string& line,
                                const std::string& wantClassEn, const FilterI18n& i18n)
{
	const LibItem* best = nullptr;
	size_t bestLen = 0;
	bool bestClassOk = false;
	for (const LibItem& li : *ix.all) {
		for (const std::string* nm : { &li.en, &li.zh }) {
			if (nm->size() < 4 || nm->size() < bestLen) continue;      // noise guard
			if (line.find(*nm) == std::string::npos) continue;
			bool classOk = !wantClassEn.empty() &&
			               i18n.ClassNameEn(li.enClass) == wantClassEn;
			if (nm->size() > bestLen || (classOk && !bestClassOk)) {
				best = &li;
				bestLen = nm->size();
				bestClassOk = classOk;
			}
		}
	}
	return best;
}

} // namespace

ImportedItem ParseGameItemText(const std::string& utf8Text,
                               const FilterI18n& i18n,
                               const std::vector<LibItem>& lib)
{
	ImportedItem r;
	const FilterI18n::ZhTables& zh = i18n.Zh();

	std::vector<std::string> lines = splitLines(utf8Text);
	if (lines.empty()) {
		r.warnings.push_back({ "", u8"剪貼簿沒有文字" });
		return r;
	}

	// zh key -> canonical en; en input passes through unchanged.
	auto canonKey = [&zh](const std::string& k) {
		auto it = zh.header.find(k);
		return it != zh.header.end() ? it->second : k;
	};
	auto canonClass = [&zh](const std::string& c) {
		auto it = zh.itemClass.find(c);
		return it != zh.itemClass.end() ? it->second : c;
	};

	// ---- sections ---------------------------------------------------------
	std::vector<std::vector<std::string>> sections(1);
	for (const std::string& l : lines) {
		if (isSeparator(l)) sections.emplace_back();
		else sections.back().push_back(l);
	}

	// ---- header section: Item Class / Rarity / name / base ---------------
	std::string rarityRaw;
	std::vector<std::string> plain;
	for (const std::string& l : sections[0]) {
		std::string k, v;
		if (splitKV(l, &k, &v)) {
			std::string ck = canonKey(k);
			if (ck == "Item Class") { r.classRaw = v; continue; }
			if (ck == "Rarity") { rarityRaw = v; continue; }
		}
		plain.push_back(l);
	}
	if (plain.empty()) {
		r.warnings.push_back({ "", u8"看起來不是遊戲物品文字(第一段找不到名稱/基底行)" });
		return r;
	}
	if (plain.size() > 2)
		r.warnings.push_back({ plain[0], u8"第一段超過兩行,以最後一行當基底" });
	if (plain.size() >= 2) r.name = plain[0];
	r.baseRaw = plain.back();

	// Rarity: the four filter values; anything else (Currency / Gem /
	// Divination Card / Quest ...) evaluates as Normal, matching the game.
	std::string rarityEn = rarityRaw;
	{
		auto it = zh.rarity.find(rarityRaw);
		if (it != zh.rarity.end()) rarityEn = it->second;
		int v = rarityOf(rarityEn);
		r.item.rarity = v >= 0 ? v : 0;
	}
	std::string classLineEn = canonClass(r.classRaw);
	bool isGem = rarityEn == "Gem" || rarityRaw == u8"寶石" ||
	             classLineEn == "Skill Gems" || classLineEn == "Support Gems" ||
	             r.classRaw == u8"技能寶石" || r.classRaw == u8"輔助寶石";

	// ---- body sections ----------------------------------------------------
	bool sawItemLevel = false;
	bool sawUnidentified = false;
	for (size_t si = 1; si < sections.size(); si++) {
		const std::vector<std::string>& sec = sections[si];
		if (sec.empty()) continue;

		// Requirements section: its "Level:" is the REQUIRED level, never the
		// gem level — skip the whole section.
		{
			std::string k, v;
			if (splitKV(sec[0], &k, &v) && v.empty() && canonKey(k) == "Requirements")
				continue;
		}

		for (const std::string& l : sec) {
			// Marker lines can share a section with mods (see kFxBoots):
			// match them anywhere, by exact text.
			bool handled = false;
			{
				auto it = zh.status.find(l);
				const std::string& en = it != zh.status.end() ? it->second : l;
				if (en == "Unidentified") { sawUnidentified = true; handled = true; }
				for (const StatusFx& s : kStatus) {
					if (handled) break;
					if (en == s.en) { s.apply(r); handled = true; }
				}
			}
			if (!handled) {
				auto it = zh.influence.find(l);
				const std::string& en = it != zh.influence.end() ? it->second : l;
				for (const InfluenceFx& f : kInfluence) {
					if (en != f.en) continue;
					if (f.bit) r.item.influence |= f.bit;
					else if (en == "Searing Exarch Item") r.exarch = true;
					else r.eater = true;
					handled = true;
					break;
				}
			}
			if (handled) continue;

			if (l.size() > 10 && l.compare(l.size() - 10, 10, " (enchant)") == 0) {
				r.item.enchanted = true;
				continue;
			}

			std::string k, v;
			if (!splitKV(l, &k, &v) || v.empty()) continue;
			std::string ck = canonKey(k);
			int n = 0;
			if (ck == "Item Level") {
				if (firstInt(v, &n)) { r.item.itemLevel = n; sawItemLevel = true; }
			} else if (ck == "Quality" || startsWith(ck, "Quality (") ||
			           startsWith(k, u8"品質")) {
				// "品質 (能量護盾): +24%" keeps its parenthesis inside the key.
				if (firstInt(v, &n)) r.item.quality = n;
			} else if (ck == "Sockets") {
				r.socketsRaw = v;
			} else if (ck == "Stack Size") {
				if (firstInt(v, &n)) r.item.stackSize = n;
			} else if (ck == "Map Tier") {
				if (firstInt(v, &n)) r.item.mapTier = n;
			} else if (ck == "Level") {
				if (isGem && firstInt(v, &n)) r.item.gemLevel = n;
			}
			// everything else (Armour, Evasion, flavour with a colon, ...) is
			// irrelevant to filter matching — ignored without a warning.
		}
	}

	// ---- sockets ----------------------------------------------------------
	if (!r.socketsRaw.empty()) {
		std::string group;
		bool bad = false;
		auto flush = [&r, &group]() {
			if (!group.empty()) { r.item.socketGroups.push_back(group); group.clear(); }
		};
		for (char c : r.socketsRaw) {
			if (c == ' ') flush();
			else if (c == '-') continue;               // link inside the group
			else if (strchr("RGBWAD", c)) { group += c; r.item.sockets++; }
			else bad = true;
		}
		flush();
		if (bad)
			r.warnings.push_back({ r.socketsRaw, u8"插槽字串有無法辨識的字元" });
		for (const std::string& g : r.item.socketGroups)
			r.item.linkedSockets = std::max(r.item.linkedSockets, (int)g.size());
	}

	// ---- base resolution --------------------------------------------------
	LibIndex ix = buildIndex(lib);
	std::string base = r.baseRaw;
	bool blight = false, uberBlight = false;

	// Name prefixes (GGPK clientstrings: QualityItem / DivergentItem /
	// InfectedMap / UberInfectedMap). Order matters: 凋落蔓延的 before 凋落的.
	struct Pfx { const char* p; bool* flag; };
	bool dummy = false;
	const Pfx kPfx[] = {
		{ u8"精良的 ", &dummy },       { "Superior ", &dummy },
		{ u8"殘存 ", &dummy },         { "Vestigial ", &dummy },
		{ u8"凋落蔓延的 ", &uberBlight }, { "Blight-ravaged ", &uberBlight },
		{ u8"凋落的 ", &blight },      { "Blighted ", &blight },
	};

	const LibItem* hit = nullptr;
	{
		// Prefixes can chain ("Superior Vestigial X"), so strip repeatedly.
		std::string probe = base;
		for (int pass = 0; pass < 4; pass++) {
			auto it = ix.exact.find(probe);
			if (it != ix.exact.end()) { hit = it->second; break; }
			const Pfx* found = nullptr;
			for (const Pfx& p : kPfx)
				if (startsWith(probe, p.p)) { found = &p; break; }
			if (!found) break;
			probe = probe.substr(strlen(found->p));
			*found->flag = true;
		}
	}
	if (!hit)   // magic/normal compose the base into one line; also a last resort
		hit = longestSubstring(ix, base, classLineEn, i18n);

	if (r.name == u8"贗品．" || startsWith(r.name, u8"贗品．") || startsWith(r.name, "Replica "))
		r.item.replica = true;

	if (!hit) {
		r.warnings.push_back({ r.baseRaw, u8"無法辨識基底名稱,不能進行判定" });
		return r;   // ok stays false
	}
	r.baseEn = hit->en;
	r.item.baseType = hit->en;

	// ---- class ------------------------------------------------------------
	std::string cls = i18n.ItemClass(r.baseEn);
	if (!cls.empty()) {
		r.item.classId = cls;
		// Cross-check against the "Item Class:" line when it resolved to en.
		if (!classLineEn.empty() && classLineEn != r.classRaw &&
		    i18n.ClassNameEn(cls) != classLineEn)
			r.warnings.push_back({ r.classRaw, u8"物品種類行與基底資料的類別不一致,以基底資料為準" });
	} else if (!classLineEn.empty() && classLineEn != r.classRaw) {
		r.item.classId = classLineEn;   // game class name; ClassNameEn passes through
	} else {
		r.item.classId = r.classRaw;    // may be zh: Class conditions won't match
		r.warnings.push_back({ r.classRaw, u8"無法判定物品類別,Class 條件將不會命中" });
	}

	// ---- fields the text does not carry -----------------------------------
	int dl = i18n.DropLevelOf(r.baseEn);
	if (dl >= 0) r.item.dropLevel = dl;
	else r.warnings.push_back({ "", u8"無此基底的掉落等級資料,DropLevel 以預設值 1 判定" });
	int w = 0, h = 0;
	if (i18n.SizeOf(r.baseEn, &w, &h)) { r.item.width = w; r.item.height = h; }
	else r.warnings.push_back({ "", u8"無此基底的尺寸資料,Width/Height 以 1x1 判定" });

	if (!sawItemLevel)
		r.warnings.push_back({ "", u8"文字裡沒有物品等級(寶石/通貨屬正常),以預設值判定" });

	// ---- statuses that key off the name/base ------------------------------
	if (blight || uberBlight) {
		r.item.blightedMap = true;
		if (uberBlight)
			r.warnings.push_back({ "", u8"凋落蔓延地圖:預覽的 UberBlightedMap 條件未模擬,以 BlightedMap 判定" });
	}
	// Magic/rare/unique text without an Unidentified marker shows its mods —
	// it IS identified. Normal items stay unidentified (they cannot be).
	r.item.identified = r.item.rarity >= 1 && !sawUnidentified;

	if (r.exarch)
		r.warnings.push_back({ "", u8"灼烙總督烙印:HasSearingExarchImplicit 條件未模擬,一律視為否" });
	if (r.eater)
		r.warnings.push_back({ "", u8"吞噬天地烙印:HasEaterOfWorldsImplicit 條件未模擬,一律視為否" });

	r.ok = true;
	return r;
}
