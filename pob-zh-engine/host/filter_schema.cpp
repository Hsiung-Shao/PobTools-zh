#include "filter_schema.h"

#include <unordered_map>

namespace {

// Shared op sets. "" means "no operator written" (the game treats it as equals).
const std::vector<const char*> kOpsAll  = { "", ">=", "<=", "==", "=", "!=", "<", ">" };
const std::vector<const char*> kOpsEq   = { "", "==", "=" };
const std::vector<const char*> kOpsNone = {};

const std::vector<SchemaEnumValue> kRarity = {
	{ "Normal", u8"普通" }, { "Magic", u8"魔法" }, { "Rare", u8"稀有" }, { "Unique", u8"傳奇" },
};
const std::vector<SchemaEnumValue> kInfluence = {
	{ "Shaper", u8"塑者" }, { "Elder", u8"尊師" }, { "Crusader", u8"聖戰軍" },
	{ "Hunter", u8"狩獵者" }, { "Redeemer", u8"救贖者" }, { "Warlord", u8"督軍" },
	{ "None", u8"無" },
};
const std::vector<SchemaEnumValue> kGemQuality = {
	{ "Superior", u8"精良" }, { "Divergent", u8"相異" },
	{ "Anomalous", u8"異常" }, { "Phantasmal", u8"幻影" },
};
const std::vector<SchemaEnumValue> kBool = {
	{ "True", u8"是" }, { "False", u8"否" },
};

const char* const kGrpCommon = u8"物品相關";
const char* const kGrpAdv    = u8"進階條件";
const char* const kGrpMods   = u8"詞綴相關";
const char* const kGrpLook   = u8"顏色圖標";
const char* const kGrpSound  = u8"音效";
const char* const kGrpVisual = u8"視覺提示";

std::vector<CardSchema> build_table()
{
	std::vector<CardSchema> t;
	auto add = [&t](CardSchema c) { t.push_back(std::move(c)); };

	// ---- 物品相關 (conditions) ------------------------------------------
	add({ "Class", u8"物品類別", false, CardKind::StringList, kGrpCommon, kOpsEq, {}, 0, 0,
	      "Class \"Stackable Currency\"", 0, nullptr,
	      u8"物品大類（英文內部名，可部分比對；== 為精確）" });
	add({ "BaseType", u8"物品名稱", false, CardKind::StringList, kGrpCommon, kOpsEq, {}, 0, 0,
	      "BaseType \"Divine Orb\"", 0, nullptr,
	      u8"具體底材名（英文；可輸入中文自動對應；可部分比對）" });
	add({ "Rarity", u8"稀有度", false, CardKind::EnumOp, kGrpCommon, kOpsAll, kRarity, 0, 0,
	      "Rarity Normal", 0, nullptr, u8"普通 < 魔法 < 稀有 < 傳奇" });
	add({ "ItemLevel", u8"物品等級", false, CardKind::IntOp, kGrpCommon, kOpsAll, {}, 0, 100,
	      "ItemLevel >= 84", 0, nullptr, u8"物品階級（掉落區域等級決定）" });
	add({ "DropLevel", u8"掉落等級", false, CardKind::IntOp, kGrpCommon, kOpsAll, {}, 0, 100,
	      "DropLevel >= 65", 0, nullptr, u8"該底材開始掉落的等級" });
	add({ "Quality", u8"品質", false, CardKind::IntOp, kGrpCommon, kOpsAll, {}, 0, 30,
	      "Quality >= 20", 0, nullptr, nullptr });
	add({ "StackSize", u8"堆疊數量", false, CardKind::IntOp, kGrpCommon, kOpsAll, {}, 1, 50000,
	      "StackSize >= 10", 0, nullptr, u8"通貨疊加數量" });
	add({ "AreaLevel", u8"區域等級", false, CardKind::IntOp, kGrpCommon, kOpsAll, {}, 0, 100,
	      "AreaLevel < 68", 0, nullptr, u8"目前區域等級（練功期 / 高等期分段）" });
	add({ "LinkedSockets", u8"連線數", false, CardKind::IntOp, kGrpCommon, kOpsAll, {}, 0, 6,
	      "LinkedSockets 6", 0, nullptr, u8"最大連線數（6 = 六連）" });
	add({ "Sockets", u8"孔數/顏色", false, CardKind::SocketSpec, kGrpCommon, kOpsAll, {}, 0, 6,
	      "Sockets >= 6", 0, nullptr, u8"插槽數與顏色，如 5GGG（R紅 G綠 B藍 W白 A深淵 D掘獄）" });
	add({ "SocketGroup", u8"連線組", false, CardKind::SocketSpec, kGrpCommon, kOpsAll, {}, 0, 6,
	      "SocketGroup >= 5GGG", 0, nullptr, u8"連在一起的一組插槽" });

	// ---- 進階條件 --------------------------------------------------------
	add({ "MapTier", u8"地圖階級", false, CardKind::IntOp, kGrpAdv, kOpsAll, {}, 1, 17,
	      "MapTier >= 15", 0, nullptr, nullptr });
	add({ "GemLevel", u8"寶石等級", false, CardKind::IntOp, kGrpAdv, kOpsAll, {}, 1, 21,
	      "GemLevel >= 20", 0, nullptr, nullptr });
	add({ "Height", u8"物品格高", false, CardKind::IntOp, kGrpAdv, kOpsAll, {}, 1, 4,
	      "Height <= 3", 0, nullptr, nullptr });
	add({ "Width", u8"物品格寬", false, CardKind::IntOp, kGrpAdv, kOpsAll, {}, 1, 2,
	      "Width <= 1", 0, nullptr, nullptr });
	add({ "BaseDefencePercentile", u8"防禦百分位", false, CardKind::IntOp, kGrpAdv, kOpsAll, {}, 0, 100,
	      "BaseDefencePercentile >= 90", 0, nullptr, u8">= 90 即頂級防禦底材" });
	add({ "BaseArmour", u8"基礎護甲", false, CardKind::IntOp, kGrpAdv, kOpsAll, {}, 0, 5000,
	      "BaseArmour >= 500", 0, nullptr, nullptr });
	add({ "BaseEvasion", u8"基礎閃避", false, CardKind::IntOp, kGrpAdv, kOpsAll, {}, 0, 5000,
	      "BaseEvasion >= 500", 0, nullptr, nullptr });
	add({ "BaseEnergyShield", u8"基礎能量護盾", false, CardKind::IntOp, kGrpAdv, kOpsAll, {}, 0, 1000,
	      "BaseEnergyShield >= 100", 0, nullptr, nullptr });
	add({ "BaseWard", u8"基礎守護", false, CardKind::IntOp, kGrpAdv, kOpsAll, {}, 0, 500,
	      "BaseWard >= 50", 0, nullptr, nullptr });
	add({ "Corrupted", u8"已汙染", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "Corrupted True", 0, nullptr, nullptr });
	add({ "CorruptedMods", u8"汙染詞綴數", false, CardKind::IntOp, kGrpAdv, kOpsAll, {}, 0, 2,
	      "CorruptedMods >= 1", 0, nullptr, nullptr });
	add({ "Mirrored", u8"已鏡像", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "Mirrored True", 0, nullptr, nullptr });
	add({ "Identified", u8"已鑑定", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "Identified True", 0, nullptr, nullptr });
	add({ "FracturedItem", u8"破裂物品", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "FracturedItem True", 0, nullptr, nullptr });
	add({ "SynthesisedItem", u8"合成物品", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "SynthesisedItem True", 0, nullptr, nullptr });
	add({ "Replica", u8"複製品傳奇", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "Replica True", 0, nullptr, nullptr });
	add({ "Scourged", u8"天災物品", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "Scourged True", 0, nullptr, nullptr });
	add({ "HasInfluence", u8"勢力影響", false, CardKind::EnumMulti, kGrpAdv, kOpsNone, kInfluence, 0, 0,
	      "HasInfluence Shaper", 0, nullptr, u8"塑界者 / 尊師等勢力影響" });
	add({ "ShaperItem", u8"塑者物品", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "ShaperItem True", 0, nullptr, nullptr });
	add({ "ElderItem", u8"尊師物品", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "ElderItem True", 0, nullptr, nullptr });
	add({ "BlightedMap", u8"凋落地圖", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "BlightedMap True", 0, nullptr, nullptr });
	add({ "UberBlightedMap", u8"高階凋落地圖", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "UberBlightedMap True", 0, nullptr, nullptr });
	add({ "ShapedMap", u8"塑形地圖", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "ShapedMap True", 0, nullptr, nullptr });
	add({ "ElderMap", u8"尊師地圖", false, CardKind::Bool, kGrpAdv, kOpsNone, kBool, 0, 0,
	      "ElderMap True", 0, nullptr, nullptr });
	add({ "Continue", u8"繼續比對（Continue）", false, CardKind::Toggle, kGrpAdv, kOpsNone, {}, 0, 0,
	      "Continue", 0, nullptr, u8"符合此區塊後仍繼續往下比對（疊加樣式用）" });

	// ---- 詞綴相關 --------------------------------------------------------
	add({ "GemQualityType", u8"替代技能", false, CardKind::EnumMulti, kGrpMods, kOpsNone, kGemQuality, 0, 0,
	      "GemQualityType Divergent", 0, nullptr, u8"寶石品質類型（相異 / 異常 / 幻影）" });
	add({ "TransfiguredGem", u8"變體技能", false, CardKind::Bool, kGrpMods, kOpsNone, kBool, 0, 0,
	      "TransfiguredGem True", 0, nullptr, u8"變體技能寶石" });
	add({ "ArchnemesisMod", u8"枷鎖詞綴", false, CardKind::StringList, kGrpMods, kOpsNone, {}, 0, 0,
	      "ArchnemesisMod \"Toxic\"", 0, nullptr, u8"夢魘（Archnemesis）詞綴名" });
	add({ "EnchantmentPassiveNode", u8"星團類別", false, CardKind::StringList, kGrpMods, kOpsNone, {}, 0, 0,
	      "EnchantmentPassiveNode \"Damage\"", 0, nullptr, u8"星團珠寶附魔類型" });
	add({ "EnchantmentPassiveNum", u8"星團天賦數", false, CardKind::IntOp, kGrpMods, kOpsAll, {}, 0, 12,
	      "EnchantmentPassiveNum >= 8", 0, nullptr, u8"星團珠寶「增加 X 天賦」數量" });
	add({ "HasEnchantment", u8"附魔詞綴", false, CardKind::StringList, kGrpMods, kOpsNone, {}, 0, 0,
	      "HasEnchantment \"Enchantment\"", 0, nullptr, u8"指定附魔名（可部分比對）" });
	add({ "AnyEnchantment", u8"任一附魔", false, CardKind::Bool, kGrpMods, kOpsNone, kBool, 0, 0,
	      "AnyEnchantment True", 0, nullptr, nullptr });
	add({ "HasImplicitMod", u8"固定詞綴", false, CardKind::Bool, kGrpMods, kOpsNone, kBool, 0, 0,
	      "HasImplicitMod True", 0, nullptr, nullptr });
	add({ "HasExplicitMod", u8"附魔詞綴（明確）", false, CardKind::ModList, kGrpMods, kOpsAll, {}, 0, 6,
	      "HasExplicitMod \"of Haast\"", 0, nullptr,
	      u8"依詞綴名篩選；op+數字＝至少 N 條相符，如 >=2" });

	// ---- 顏色圖標 (actions) ----------------------------------------------
	add({ "SetBackgroundColor", u8"背景顏色", true, CardKind::Color, kGrpLook, kOpsNone, {}, 0, 255,
	      "SetBackgroundColor 0 0 0 255", 0, nullptr, nullptr });
	add({ "SetBorderColor", u8"邊框顏色", true, CardKind::Color, kGrpLook, kOpsNone, {}, 0, 255,
	      "SetBorderColor 255 255 255 255", 0, nullptr, nullptr });
	add({ "SetTextColor", u8"文字顏色", true, CardKind::Color, kGrpLook, kOpsNone, {}, 0, 255,
	      "SetTextColor 255 255 255 255", 0, nullptr, nullptr });
	add({ "SetFontSize", u8"字體大小", true, CardKind::IntRange, kGrpLook, kOpsNone, {}, 1, 45,
	      "SetFontSize 40", 0, nullptr, u8"1~45；垃圾用小字、重要物品用大字" });

	// ---- 音效 (exclusive group 1) ----------------------------------------
	add({ "PlayAlertSound", u8"內置音效", true, CardKind::SoundBuiltin, kGrpSound, kOpsNone, {}, 1, 16,
	      "PlayAlertSound 1 300", 1, "PlayAlertSoundPositional",
	      u8"內建音效編號 1~16 + 音量 0~300" });
	add({ "CustomAlertSound", u8"自定義音效", true, CardKind::SoundCustom, kGrpSound, kOpsNone, {}, 0, 300,
	      "CustomAlertSound \"alert.mp3\" 300", 1, "CustomAlertSoundOptional",
	      u8"自訂音效檔（放在過濾器同資料夾）" });
	add({ "DisableDropSound", u8"關閉落地音", true, CardKind::Toggle, kGrpSound, kOpsNone, {}, 0, 0,
	      "DisableDropSound", 0, nullptr, u8"關閉該物品的預設落地音" });

	// ---- 視覺提示 --------------------------------------------------------
	add({ "MinimapIcon", u8"物品圖標", true, CardKind::MinimapIcon, kGrpVisual, kOpsNone, {}, 0, 2,
	      "MinimapIcon 1 White Circle", 0, nullptr, u8"小地圖圖示：大小(0 最大) 顏色 形狀" });
	add({ "PlayEffect", u8"物品光柱", true, CardKind::PlayEffect, kGrpVisual, kOpsNone, {}, 0, 0,
	      "PlayEffect White", 0, nullptr, u8"掉落光柱；Temp = 只在掉落瞬間" });

	return t;
}

} // namespace

const std::vector<CardSchema>& FilterSchemaAll()
{
	static const std::vector<CardSchema> t = build_table();
	return t;
}

const CardSchema* FilterSchemaFind(const std::string& keyword)
{
	static const std::unordered_map<std::string, const CardSchema*> idx = [] {
		std::unordered_map<std::string, const CardSchema*> m;
		for (const CardSchema& c : FilterSchemaAll()) {
			m[c.keyword] = &c;
			if (c.alias) m[c.alias] = &c;
		}
		return m;
	}();
	auto it = idx.find(keyword);
	return it != idx.end() ? it->second : nullptr;
}

std::string FilterSchemaKeywordZh(const std::string& keyword)
{
	const CardSchema* c = FilterSchemaFind(keyword);
	return c ? c->zh : keyword;
}

std::string FilterSchemaValueZh(const std::string& keyword, const std::string& value)
{
	if (value == "True") return u8"是";
	if (value == "False") return u8"否";
	const CardSchema* c = FilterSchemaFind(keyword);
	if (c)
		for (const SchemaEnumValue& e : c->enums)
			if (value == e.token) return e.zh;
	return value;
}

const std::vector<const char*>& FilterSchemaGroups()
{
	static const std::vector<const char*> g = {
		kGrpCommon, kGrpAdv, kGrpMods, kGrpLook, kGrpSound, kGrpVisual,
	};
	return g;
}
