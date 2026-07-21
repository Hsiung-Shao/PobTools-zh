#include "filter_i18n.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

using nlohmann::ordered_json;

// Read a whole file (wide path; the exe may live in a non-ASCII directory).
static bool read_file_utf8(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 30)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

static std::wstring widen(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

// Merge an "entries" dictionary file (key=English, value=Chinese string) into dst.
static void load_entries(const std::wstring& path, std::unordered_map<std::string, std::string>& dst)
{
	std::string content;
	if (!read_file_utf8(path, content)) return;
	ordered_json doc;
	try { doc = ordered_json::parse(content); } catch (...) { return; }
	if (!doc.contains("entries") || !doc["entries"].is_object()) return;
	for (auto& [key, val] : doc["entries"].items()) {
		if (val.is_string()) {
			// Don't overwrite an existing translation (earlier dicts win).
			dst.emplace(key, val.get<std::string>());
		} else if (val.is_object() && val.contains("\xe7\xbf\xbb\xe8\xad\xaf") &&
		           val["\xe7\xbf\xbb\xe8\xad\xaf"].is_string()) {
			dst.emplace(key, val["\xe7\xbf\xbb\xe8\xad\xaf"].get<std::string>());
		}
	}
}

// Merge a flat { "English": "中文" } object into dst (first value wins).
static void load_flat(const std::wstring& path, std::unordered_map<std::string, std::string>& dst)
{
	std::string content;
	if (!read_file_utf8(path, content)) return;
	ordered_json doc;
	try { doc = ordered_json::parse(content); } catch (...) { return; }
	if (!doc.is_object()) return;
	for (auto& [key, val] : doc.items())
		if (val.is_string()) dst.emplace(key, val.get<std::string>());
}

void FilterI18n::Load(const std::wstring& exeDir, const std::string& locale)
{
	// 1. Complete bundled TC item names (repoe-fork derived) — primary source.
	load_flat(exeDir + L"Data\\filter_items_zh.json", names_);

	// 2. Engine dictionaries fill anything the bundled set lacks (first value wins).
	std::wstring dir = exeDir + L"Data\\poe1\\" + widen(locale) + L"\\";
	for (const wchar_t* f : { L"items.json", L"uniques.json", L"gems.json", L"ui.json" })
		load_entries(dir + f, names_);

	// 3. Per-item class + tags (repoe-fork) for ALL items.
	{
		std::string content;
		if (read_file_utf8(exeDir + L"Data\\item_meta.json", content)) {
			try {
				ordered_json doc = ordered_json::parse(content);
				if (doc.is_object())
					for (auto& [name, v] : doc.items()) {
						if (!v.is_object()) continue;
						Meta m;
						if (v.contains("class") && v["class"].is_string()) m.cls = v["class"].get<std::string>();
						if (v.contains("tags") && v["tags"].is_array())
							for (auto& t : v["tags"]) if (t.is_string()) m.tags.push_back(t.get<std::string>());
						meta_.emplace(name, std::move(m));
					}
			} catch (...) {}
		}
	}

	// 4. Legacy base type -> item class (gear; kept for the base-class sub-filter).
	{
		std::string content;
		if (read_file_utf8(exeDir + L"Data\\base_classes.json", content)) {
			try {
				ordered_json doc = ordered_json::parse(content);
				if (doc.is_object())
					for (auto& [k, v] : doc.items())
						if (v.is_string()) baseClass_.emplace(k, v.get<std::string>());
			} catch (...) {}
		}
	}

	// 5. Item-class Chinese labels: bundled repoe-fork map (complete) first, then
	// the engine's item_metadata.json as a fallback.
	load_flat(exeDir + L"Data\\item_classes_zh.json", classZh_);
	// 5b. class id -> in-game English class name (what the Class condition matches).
	load_flat(exeDir + L"Data\\item_classes_en.json", classEn_);
	{
		std::string content;
		if (read_file_utf8(dir + L"item_metadata.json", content)) {
			try {
				ordered_json doc = ordered_json::parse(content);
				if (doc.contains("item_classes") && doc["item_classes"].is_array())
					for (auto& e : doc["item_classes"])
						if (e.contains("en") && e.contains("zh") && e["en"].is_string() && e["zh"].is_string())
							classZh_.emplace(e["en"].get<std::string>(), e["zh"].get<std::string>());
			} catch (...) {}
		}
	}

	loaded_ = !names_.empty() || !baseClass_.empty();
}

std::string FilterI18n::DisplayName(const std::string& en) const
{
	auto it = names_.find(en);
	return it != names_.end() ? it->second : en;
}

std::string FilterI18n::BaseClass(const std::string& en) const
{
	auto it = baseClass_.find(en);
	return it != baseClass_.end() ? it->second : std::string();
}

std::string FilterI18n::ItemClass(const std::string& en) const
{
	auto it = meta_.find(en);
	return it != meta_.end() ? it->second.cls : std::string();
}

const std::vector<std::string>& FilterI18n::Tags(const std::string& en) const
{
	static const std::vector<std::string> kEmpty;
	auto it = meta_.find(en);
	return it != meta_.end() ? it->second.tags : kEmpty;
}

std::string FilterI18n::ClassNameZh(const std::string& enClass) const
{
	// POB's Data/Bases uses singular class names ("Bow", "One Handed Sword") that
	// differ from item_metadata.json ("Bows", "One Hand Swords"), so use a built-in
	// table covering exactly the 26 classes that appear in base_classes.json.
	static const std::unordered_map<std::string, std::string> kZh = {
		{ "Amulet", u8"項鍊" }, { "Belt", u8"腰帶" }, { "Body Armour", u8"胸甲" },
		{ "Boots", u8"鞋子" }, { "Bow", u8"弓" }, { "Claw", u8"爪" }, { "Dagger", u8"匕首" },
		{ "Fishing Rod", u8"魚竿" }, { "Flask", u8"藥劑" }, { "Gloves", u8"手套" },
		{ "Graft", u8"移植物" }, { "Helmet", u8"頭盔" }, { "Jewel", u8"珠寶" },
		{ "One Handed Axe", u8"單手斧" }, { "One Handed Mace", u8"單手鎚" },
		{ "One Handed Sword", u8"單手劍" }, { "Quiver", u8"箭袋" }, { "Ring", u8"戒指" },
		{ "Sceptre", u8"權杖" }, { "Shield", u8"盾" }, { "Staff", u8"長杖" },
		{ "Tincture", u8"酊劑" }, { "Two Handed Axe", u8"雙手斧" },
		{ "Two Handed Mace", u8"雙手鎚" }, { "Two Handed Sword", u8"雙手劍" }, { "Wand", u8"法杖" },
	};
	auto it = kZh.find(enClass);
	if (it != kZh.end()) return it->second;
	auto m = classZh_.find(enClass);
	return m != classZh_.end() ? m->second : enClass;
}

// ---------------------------------------------------------------------------
// NeverSink 標記標題中文化（display only）。
// 節段表對齊 Garena 台服譯名（精髓/培育器/聖甲蟲/魔符/魔偶/萃取物…），
// 覆蓋 NeverSink 8.x $type-> 全部節段;未知節段保留英文。
// $tier-> 細分規格（六百餘種裝備規格）只翻通用字,其餘保留英文。

static const char* ns_type_seg_zh(const std::string& seg)
{
	static const std::unordered_map<std::string, const char*> k = {
		{ "3l", u8"三連" }, { "4l", u8"四連" }, { "6l", u8"六連" },
		{ "abyss", u8"深淵" },
		{ "act1", u8"第一章" }, { "act2", u8"第二章" }, { "otheracts", u8"其他章節" },
		{ "all", u8"全部" },
		{ "amuring", u8"項鍊戒指" }, { "belts", u8"腰帶" },
		{ "animatedweapons", u8"幻化武器" },
		{ "anyremaining", u8"其餘所有" }, { "remaining", u8"其餘" },
		{ "archer", u8"弓系" }, { "caster", u8"法系" },
		{ "melee1h", u8"單手近戰" }, { "melee2h", u8"雙手近戰" },
		{ "minion", u8"召喚" }, { "universal", u8"通用" },
		{ "armours", u8"防具" },
		{ "artefact", u8"古物" }, { "sanctifiedrelics", u8"神化聖物" },
		{ "blighted", u8"凋落" },
		{ "breachrings", u8"裂痕戒指" },
		{ "chancing", u8"機會石基底" },
		{ "cluster", u8"星團珠寶" }, { "clustereco", u8"星團珠寶·高價" },
		{ "corpses", u8"屍體" },
		{ "corruptedid", u8"汙染已鑑定" }, { "corruptedimplicit", u8"汙染固定詞綴" },
		{ "corruptedspecial", u8"特殊汙染" }, { "corruptions", u8"汙染" },
		{ "crafting", u8"製作" }, { "normalcraft", u8"普通製作" },
		{ "qualityperfection", u8"高品質" }, { "expensive", u8"高價" },
		{ "crucible", u8"坩堝" },
		{ "currency", u8"通貨" },
		{ "decorators", u8"分隔線" },
		{ "deliriumorbs", u8"譫妄玉" },
		{ "divination", u8"命運卡" },
		{ "droppeditems", u8"掉落裝備" },
		{ "eater", u8"吞噬天地" }, { "exarch", u8"灼烙總督" },
		{ "enchanted", u8"附魔" },
		{ "endgameflasks", u8"終局藥劑" }, { "endgamergb", u8"終局三色" },
		{ "endgametinctures", u8"終局萃取物" },
		{ "essence", u8"精髓" },
		{ "event", u8"活動" }, { "idols", u8"魔偶" },
		{ "exceptional", u8"特級" },
		{ "exotic", u8"特異" }, { "exotics", u8"特異物品" },
		{ "exoticbases", u8"特異基底" }, { "exoticbaseslower", u8"特異基底·低" },
		{ "exoticmap", u8"特異地圖" }, { "exoticmods", u8"特異詞綴" },
		{ "expedition", u8"探險" }, { "logbook", u8"探險日誌" },
		{ "extra", u8"額外" },
		{ "firstlevels", u8"開荒初期" },
		{ "flasks", u8"藥劑" }, { "life", u8"生命" }, { "mana", u8"魔力" },
		{ "hybrid", u8"複合" }, { "utility", u8"功能" }, { "quality", u8"品質" },
		{ "fossil", u8"化石" },
		{ "foulborn", u8"穢生" },
		{ "fractured", u8"破裂" },
		{ "fragments", u8"碎片" }, { "scarabs", u8"聖甲蟲" },
		{ "gear", u8"裝備" }, { "generalgear", u8"一般裝備" },
		{ "gems", u8"寶石" }, { "generic", u8"一般" }, { "special", u8"特殊" },
		{ "gold", u8"金幣" },
		{ "harvest", u8"豐收" },
		{ "heist", u8"劫盜" }, { "heisttarget", u8"劫盜目標" },
		{ "cloak", u8"披風" }, { "brooch", u8"胸針" }, { "tool", u8"工具" },
		{ "contract", u8"契約書" }, { "blueprint", u8"藍圖" },
		{ "hidelayer", u8"隱藏層" }, { "maphiders", u8"地圖隱藏" },
		{ "implicitmod", u8"固定詞綴" },
		{ "incubators", u8"培育器" },
		{ "influenced", u8"影響裝備" },
		{ "jewels", u8"珠寶" },
		{ "leagueexclusive", u8"賽季限定" },
		{ "leveling", u8"練等" }, { "levelingstacked", u8"練等堆疊" },
		{ "magic", u8"魔法" }, { "magicid", u8"魔法已鑑定" },
		{ "rare", u8"稀有" }, { "rareid", u8"稀有已鑑定" },
		{ "rareblendid", u8"稀有混合鑑定" }, { "rareeg", u8"稀有·終局" },
		{ "rareoptional", u8"稀有·可選" },
		{ "rr", u8"終局稀有裝備" },
		{ "memorystrand", u8"記憶絲縷" },
		{ "normalmagic", u8"普通魔法" },
		{ "maps", u8"地圖" }, { "nightmare", u8"夢魘" }, { "vaaltemple", u8"瓦爾神殿" },
		{ "misc", u8"雜項" }, { "miscendgamerules", u8"終局雜項" },
		{ "miscmapitems", u8"地圖雜項" }, { "miscmapitemsextra", u8"地圖雜項·額外" },
		{ "oil", u8"油瓶" },
		{ "omen", u8"預兆" }, { "trial", u8"試煉" }, { "tattoo", u8"紋身" },
		{ "others", u8"其他" },
		{ "questlike", u8"任務物品" }, { "questlikeexception", u8"任務物品·例外" },
		{ "replicas", u8"贗品" },
		{ "rgb", u8"三色連結" },
		{ "runesgrafts", u8"符文之結" },
		{ "sanctum", u8"聖域" },
		{ "simulacrum", u8"幻像" },
		{ "sockets", u8"插槽" }, { "socketslinks", u8"插槽連結" },
		{ "splinter", u8"裂片" },
		{ "stacked", u8"堆疊" }, { "stackedsix", u8"堆疊·六張" }, { "stackedthree", u8"堆疊·三張" },
		{ "stackedsplintershigh", u8"堆疊裂片·高" }, { "stackedsplinterslow", u8"堆疊裂片·低" },
		{ "stackedsupplieshigh", u8"堆疊補給·高" }, { "stackedsupplieslow", u8"堆疊補給·低" },
		{ "stackedsuppliesportal", u8"堆疊傳送門" }, { "stackedsupplieswisdom", u8"堆疊知識卷軸" },
		{ "synthesised", u8"追憶" },
		{ "talisman", u8"魔符" },
		{ "tincture", u8"萃取物" },
		{ "uniques", u8"傳奇" },
		{ "veiled", u8"隱匿" },
		{ "vials", u8"祭罈" },
		{ "wandprogression", u8"法杖成長" }, { "weaponprogression", u8"武器成長" },
		{ "wombgifts", u8"胎贈" },
	};
	auto it = k.find(seg);
	return it == k.end() ? nullptr : it->second;
}

static std::string ns_tier_seg_zh(const std::string& seg)
{
	static const std::unordered_map<std::string, const char*> k = {
		{ "any", u8"任意" }, { "restex", u8"其餘" }, { "general", u8"通用" },
		{ "final", u8"最終" }, { "anyhigh", u8"任意·高" },
	};
	auto it = k.find(seg);
	if (it != k.end()) return it->second;
	// "t1".."t17" -> "T1"（純數字階級代碼統一大寫）
	if (seg.size() >= 2 && seg[0] == 't' &&
	    seg.find_first_not_of("0123456789", 1) == std::string::npos)
		return "T" + seg.substr(1);
	return seg;
}

// "a->b->c" -> 各節段翻譯後以「·」相接;未知節段原樣保留。
static std::string ns_join_path(const std::string& path, bool tierPath)
{
	std::string out;
	size_t p = 0;
	for (;;) {
		size_t q = path.find("->", p);
		std::string seg = (q == std::string::npos) ? path.substr(p) : path.substr(p, q - p);
		std::string zh;
		if (tierPath) {
			zh = ns_tier_seg_zh(seg);
		} else if (const char* t = ns_type_seg_zh(seg)) {
			zh = t;
		}
		if (zh.empty()) zh = seg;
		if (!out.empty()) out += u8"·";
		out += zh;
		if (q == std::string::npos) break;
		p = q + 2;
	}
	return out;
}

std::string NeverSinkHeaderZh(const std::string& header)
{
	if (header.find("$type->") == std::string::npos) return header;
	std::string out;
	size_t p = 0;
	bool lastWasType = false;
	while (p < header.size()) {
		size_t sp = header.find_first_not_of(" \t", p);
		if (sp == std::string::npos) break;
		size_t e = header.find_first_of(" \t", sp);
		if (e == std::string::npos) e = header.size();
		std::string tok = header.substr(sp, e - sp);
		std::string piece;
		bool isType = tok.rfind("$type->", 0) == 0;
		bool isTier = tok.rfind("$tier->", 0) == 0;
		if (isType)
			piece = u8"【" + ns_join_path(tok.substr(7), false) + u8"】";
		else if (isTier)
			piece = ns_join_path(tok.substr(7), true);
		else
			piece = tok;
		// 「【…】T2」— tier 緊貼 type 區塊,不留空格。
		if (!out.empty() && !(isTier && lastWasType)) out += ' ';
		out += piece;
		lastWasType = isType;
		p = e;
	}
	return out;
}

std::string FilterI18n::ClassNameEn(const std::string& classId) const
{
	auto it = classEn_.find(classId);
	return it != classEn_.end() ? it->second : classId;
}
