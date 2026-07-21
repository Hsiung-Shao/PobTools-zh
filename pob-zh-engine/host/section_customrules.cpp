#include "editor_shell.h"
#include "editor_util.h"
#include "filter_parser.h"   // FilterGetColor

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>
#include <vector>
#include <unordered_map>

// 客製規則 = the advanced per-block editor. It shows the raw .filter rule logic;
// here we render the conditions in Chinese (display only — output stays English)
// and group the (hundreds of) blocks under collapsible category headers.

static const char* KeywordZh(const std::string& kw)
{
	static const std::unordered_map<std::string, const char*> m = {
		{ "Class", u8"類別" }, { "BaseType", u8"基底" }, { "Rarity", u8"稀有度" },
		{ "ItemLevel", u8"物品等級" }, { "DropLevel", u8"掉落等級" }, { "AreaLevel", u8"區域等級" },
		{ "Quality", u8"品質" }, { "Sockets", u8"插槽" }, { "LinkedSockets", u8"連結孔" },
		{ "SocketGroup", u8"連線組" }, { "Height", u8"高度" }, { "Width", u8"寬度" },
		{ "StackSize", u8"堆疊量" }, { "MapTier", u8"地圖階級" }, { "GemLevel", u8"寶石等級" },
		{ "Corrupted", u8"已汙染" }, { "Mirrored", u8"已鏡像" }, { "Identified", u8"已鑑定" },
		{ "ElderItem", u8"尊師物品" }, { "ShaperItem", u8"塑者物品" }, { "FracturedItem", u8"分裂物品" },
		{ "SynthesisedItem", u8"合成物品" }, { "Replica", u8"複製品" }, { "Scourged", u8"災變" },
		{ "HasInfluence", u8"具影響" }, { "HasExplicitMod", u8"明確詞綴" }, { "HasImplicitMod", u8"固定詞綴" },
		{ "HasEnchantment", u8"附魔" }, { "AnyEnchantment", u8"任一附魔" },
		{ "EnchantmentPassiveNum", u8"賦予天賦數" }, { "BlightedMap", u8"凋落地圖" },
		{ "UberBlightedMap", u8"高階凋落地圖" }, { "ArchnemesisMod", u8"夢魘詞綴" },
		{ "HasSearingExarch", u8"熾炎詞綴" }, { "HasEaterOfWorlds", u8"噬世詞綴" },
		{ "DefencePercentile", u8"防禦百分位" }, { "BaseArmour", u8"基礎護甲" },
		{ "BaseEnergyShield", u8"基礎能量護盾" }, { "BaseEvasion", u8"基礎閃避" }, { "BaseWard", u8"基礎守護" },
		{ "GemQualityType", u8"寶石品質類型" }, { "AlternateQuality", u8"替代品質" },
		{ "TransfiguredGem", u8"變形寶石" }, { "HasCruciblePassiveTree", u8"熔爐天賦樹" },
	};
	auto it = m.find(kw);
	return it != m.end() ? it->second : nullptr;
}

static std::string ValueZh(const std::string& kw, const std::string& v, const FilterI18n& i18n)
{
	if (v == "True") return u8"是";
	if (v == "False") return u8"否";
	if (kw == "Rarity") {
		if (v == "Normal") return u8"普通";
		if (v == "Magic") return u8"魔法";
		if (v == "Rare") return u8"稀有";
		if (v == "Unique") return u8"傳奇";
	}
	if (kw == "HasInfluence") {
		if (v == "Shaper") return u8"塑者";
		if (v == "Elder") return u8"尊師";
		if (v == "Crusader") return u8"聖戰軍";
		if (v == "Hunter") return u8"狩獵者";
		if (v == "Redeemer") return u8"救贖者";
		if (v == "Warlord") return u8"督軍";
		if (v == "None") return u8"無";
	}
	if (kw == "Class") { std::string z = i18n.ClassNameZh(v); if (z != v) return z; }
	if (kw == "BaseType") { std::string z = i18n.DisplayName(v); if (z != v) return z; }
	return v; // numbers / unknown strings pass through
}

// One condition line rendered in Chinese (keyword + operator + translated values).
static std::string ConditionZh(const FilterLine& ln, const FilterI18n& i18n)
{
	const char* kz = KeywordZh(ln.keyword);
	std::string s = kz ? kz : ln.keyword;
	if (!ln.op.empty()) { s += ' '; s += ln.op; }
	for (const FilterToken& v : ln.values) { s += ' '; s += ValueZh(ln.keyword, v.text, i18n); }
	return s;
}

// All of a block's conditions joined (Chinese), for the rule row / search.
static std::string BlockConditionsZh(const FilterFile& f, const FilterBlock& b, const FilterI18n& i18n)
{
	std::string s; int n = 0;
	for (int li : b.lineIdx) {
		const FilterLine& ln = f.lines[li];
		if (ln.kind != FilterLineKind::Condition) continue;
		if (!s.empty()) s += u8"  ·  ";
		s += ConditionZh(ln, i18n);
		if (++n >= 5) { s += u8" …"; break; }
	}
	if (s.empty()) s = u8"（無條件）";
	return s;
}

// Raw English conditions of a block (for the hover tooltip).
static std::string ConditionEn(const FilterLine& ln)
{
	std::string t = ln.keyword;
	if (!ln.op.empty()) { t += ' '; t += ln.op; }
	for (const FilterToken& v : ln.values) {
		t += ' ';
		if (v.quoted) { t += '"'; t += v.text; t += '"'; } else t += v.text;
	}
	return t;
}

void DrawCustomRulesSection(EditorShell& s)
{
	if (!s.loaded) { ImGui::TextDisabled(u8"開啟一個 .filter 後即可在此逐條編輯規則。"); return; }

	ImGui::SetNextItemWidth(300 * s.scale);
	if (ImGui::InputTextWithHint("##search", u8"搜尋條件（中／英）…", &s.search))
		s.searchLower = EdToLowerAscii(s.search);
	ImGui::SameLine();
	ImGui::TextDisabled(u8"依分類摺疊；點規則編輯（共 %d 條）", (int)s.model.blocks.size());
	ImGui::Spacing();

	float leftW = ImGui::GetContentRegionAvail().x * 0.55f;

	ImGui::BeginChild("##rulegroups", ImVec2(leftW, 0), true);
	if (s.tierIndex.categories.empty()) {
		ImGui::TextDisabled(u8"此過濾器沒有可分類的規則。");
	} else {
		const bool searching = !s.searchLower.empty();
		for (const TierCategory& cat : s.tierIndex.categories) {
			std::vector<int> blocks;
			for (int ti : cat.tierEntryIdx) {
				int bi = s.tierIndex.tiers[ti].blockIndex;
				if (searching) {
					const FilterBlock& bb = s.model.blocks[bi];
					std::string en = BlockSummary(s.model, bb) + " " + bb.headerComment;
					std::string zh = BlockConditionsZh(s.model, bb, s.i18n);
					if (!EdContainsCI(en, s.searchLower) && zh.find(s.search) == std::string::npos) continue;
				}
				blocks.push_back(bi);
			}
			if (blocks.empty()) continue;

			char hdr[96];
			snprintf(hdr, sizeof(hdr), u8"%s  (%d)###%s", cat.display.c_str(), (int)blocks.size(), cat.key.c_str());
			ImGuiTreeNodeFlags fl = searching ? ImGuiTreeNodeFlags_DefaultOpen : 0;
			if (ImGui::CollapsingHeader(hdr, fl)) {
				for (int bi : blocks) {
					FilterBlock& b = s.model.blocks[bi];
					ImGui::PushID(bi);
					if (b.idxTextColor >= 0) {
						int r, g, bl, a; bool ha; FilterGetColor(s.model.lines[b.idxTextColor], r, g, bl, a, ha);
						ImGui::ColorButton("##sw", ImVec4(r / 255.f, g / 255.f, bl / 255.f, 1.f),
							ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
							ImVec2(12 * s.scale, 12 * s.scale));
					} else {
						ImGui::Dummy(ImVec2(12 * s.scale, 12 * s.scale));
					}
					ImGui::SameLine();

					bool blockDirty = false;
					for (int li : b.lineIdx) if (s.model.lines[li].dirty) { blockDirty = true; break; }
					std::string label = (b.hide ? u8"隱藏  " : u8"顯示  ") + BlockConditionsZh(s.model, b, s.i18n);
					if (blockDirty) label += u8"  *";

					bool sel = (s.selectedBlock == bi);
					if (b.hide) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.46f, 0.46f, 1.0f));
					if (ImGui::Selectable(label.c_str(), sel)) s.selectedBlock = bi;
					if (b.hide) ImGui::PopStyleColor();
					ImGui::PopID();
				}
			}
		}
	}
	ImGui::EndChild();

	ImGui::SameLine();

	ImGui::BeginChild("##detail", ImVec2(0, 0), true);
	if (s.selectedBlock < 0 || s.selectedBlock >= (int)s.model.blocks.size()) {
		ImGui::TextDisabled(u8"在左側選擇一個規則以編輯。");
	} else {
		FilterBlock& b = s.model.blocks[s.selectedBlock];
		if (!b.headerComment.empty()) ImGui::TextDisabled(u8"# %s", b.headerComment.c_str());

		ImGui::TextDisabled(u8"條件（顯示中文，輸出仍英文；hover 看原始）");
		bool anyCond = false;
		for (int li : b.lineIdx) {
			const FilterLine& ln = s.model.lines[li];
			if (ln.kind != FilterLineKind::Condition) continue;
			anyCond = true;
			ImGui::BulletText("%s", ConditionZh(ln, s.i18n).c_str());
			if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ConditionEn(ln).c_str());
		}
		if (!anyCond) ImGui::TextDisabled(u8"（此區塊無條件）");
		ImGui::Separator();

		ImGui::PushID(s.selectedBlock);
		DrawBlockStyleEditor(s.model, b, s.scale);
		ImGui::PopID();
	}
	ImGui::EndChild();
}
