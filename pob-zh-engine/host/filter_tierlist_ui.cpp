#include "filter_tierlist_ui.h"
#include "filter_parser.h"
#include "icon_manager.h"
#include "item_card.h"
#include "item_library.h"
#include "economy_service.h"
#include "audio_player.h"
#include "sound_manager.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cfloat>
#include <unordered_map>

#pragma comment(lib, "comdlg32.lib")

// ---- small self-contained helpers ------------------------------------------

static std::string narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

static std::wstring widen_utf8(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static std::wstring FileDialog(bool save)
{
	wchar_t buf[MAX_PATH] = L"";
	OPENFILENAMEW ofn{};
	ofn.lStructSize = sizeof(ofn);
	ofn.hwndOwner = GetActiveWindow();
	ofn.lpstrFilter = L"所有檔案 (*.*)\0*.*\0";
	ofn.lpstrFile = buf;
	ofn.nMaxFile = MAX_PATH;
	ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
	BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
	return ok ? std::wstring(buf) : std::wstring();
}

static std::string to_lower_ascii(const std::string& s)
{
	std::string r = s;
	for (char& c : r) if (c >= 'A' && c <= 'Z') c += 32;
	return r;
}

static bool contains_ci(const std::string& hay, const std::string& needleLower)
{
	if (needleLower.empty()) return true;
	const size_t n = needleLower.size();
	if (hay.size() < n) return false;
	for (size_t i = 0; i + n <= hay.size(); i++) {
		size_t j = 0;
		for (; j < n; j++) {
			char c = hay[i + j];
			if (c >= 'A' && c <= 'Z') c += 32;
			if (c != needleLower[j]) break;
		}
		if (j == n) return true;
	}
	return false;
}

static std::string MoveMsg(TierMoveResult r, const std::string& item)
{
	switch (r) {
		case TierMoveResult::Ok:              return u8"已移動：" + item;
		case TierMoveResult::SameTier:        return u8"已在同一階級";
		case TierMoveResult::DstNotDraggable: return u8"目標階級不接受物品（無 BaseType）";
		case TierMoveResult::ItemMissing:     return u8"找不到該物品";
		case TierMoveResult::Duplicate:       return u8"目標階級已有此物品";
	}
	return std::string();
}

static void StyleSwatch(FilterFile& model, int lineIdx, float scale)
{
	if (lineIdx >= 0) {
		int r, g, b, a; bool h;
		FilterGetColor(model.lines[lineIdx], r, g, b, a, h);
		ImGui::ColorButton("##sw", ImVec4(r / 255.f, g / 255.f, b / 255.f, 1.f),
			ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker | ImGuiColorEditFlags_NoDragDrop,
			ImVec2(12 * scale, 12 * scale));
	} else {
		ImGui::Dummy(ImVec2(12 * scale, 12 * scale));
	}
}

// A block colour as ImU32 (useAlpha keeps the source alpha, else opaque).
static ImU32 BlockColorU32(FilterFile& model, int lineIdx, ImU32 fallback, bool useAlpha)
{
	if (lineIdx < 0) return fallback;
	int r, g, b, a; bool h;
	FilterGetColor(model.lines[lineIdx], r, g, b, a, h);
	return IM_COL32(r, g, b, useAlpha ? a : 255);
}

// Draw a lazily-fetched item icon (or a same-size placeholder) + SameLine, so the
// item name follows on the same row. Icon size tracks the text line height.
static void ItemIcon(IconManager& icons, const std::string& enName)
{
	float sz = ImGui::GetTextLineHeight();
	icons.Request(enName);
	unsigned t = icons.Texture(enName);
	if (t) ImGui::Image((ImTextureID)(intptr_t)t, ImVec2(sz, sz));
	else   ImGui::Dummy(ImVec2(sz, sz));
	ImGui::SameLine();
}

// Compact chaos-value label, e.g. 150000 -> "150k c", 12 -> "12 c", 0.3 -> "0.3 c".
static std::string FormatChaos(double c)
{
	char buf[32];
	if (c >= 10000.0)     snprintf(buf, sizeof(buf), u8"%.0fk c", c / 1000.0);
	else if (c >= 100.0)  snprintf(buf, sizeof(buf), u8"%.0f c", c);
	else if (c >= 1.0)    snprintf(buf, sizeof(buf), u8"%.1f c", c);
	else                  snprintf(buf, sizeof(buf), u8"%.2f c", c);
	return buf;
}

// Flip a block's Show/Hide verb in place (header keyword + b.hide), marking dirty.
static void TierSetHide(FilterFile& model, FilterBlock& b, bool hide)
{
	if (b.hide == hide) return;
	FilterLine& hdr = model.lines[b.headerLineIdx];
	hdr.keyword = hide ? "Hide" : "Show";
	hdr.dirty = true;
	b.hide = hide;
	model.dirty = true;
}

// Curated repoe-tag -> Chinese label for the sub-category dropdown. Returns nullptr
// for tags we don't surface (noise like "default"), so the dropdown stays clean.
static const char* TagLabel(const std::string& tag)
{
	static const std::unordered_map<std::string, const char*> kt = {
		{ "lesser_currency", u8"低階通貨" }, { "mid_tier_currency", u8"中階通貨" },
		{ "high_tier_currency", u8"高階通貨" }, { "quality_currency", u8"品質通貨" },
		{ "currency_shard", u8"通貨碎片" }, { "delve_currency", u8"地心通貨" },
		{ "breach_currency", u8"裂痕通貨" }, { "essence", u8"精髓" },
		{ "fossil", u8"化石" }, { "resonator", u8"共鳴器" }, { "scarab", u8"聖甲蟲" },
		{ "oil", u8"聖油" }, { "incubator", u8"孵化器" }, { "catalyst", u8"催化劑" },
		{ "unique", u8"傳奇" }, { "top_tier_base_item_type", u8"頂級基底" },
		{ "abyss_jewel", u8"深淵珠寶" }, { "cluster_jewel", u8"星團珠寶" },
		{ "map_fragment", u8"地圖碎片" }, { "divination_card", u8"命運卡" },
		{ "gem", u8"寶石" }, { "support_gem", u8"輔助寶石" },
		{ "active_skill_gem", u8"技能寶石" }, { "vaal_gem", u8"瓦爾寶石" },
	};
	auto it = kt.find(tag);
	return it != kt.end() ? it->second : nullptr;
}

// True if `tags` contains the filter tag (empty filter matches everything).
static bool HasTag(const std::vector<std::string>& tags, const std::string& want)
{
	if (want.empty()) return true;
	for (const std::string& t : tags) if (t == want) return true;
	return false;
}

// ---- shared rule-style editor (verb + colours + font + sound) ---------------

bool DrawBlockStyleEditor(FilterFile& model, FilterBlock& b, float scale)
{
	bool changed = false;

	int verbIdx = b.hide ? 1 : 0;
	ImGui::SetNextItemWidth(160 * scale);
	const char* verbs[2] = { u8"Show（顯示）", u8"Hide（隱藏）" };
	if (ImGui::Combo("##verb", &verbIdx, verbs, 2)) {
		FilterLine& hdr = model.lines[b.headerLineIdx];
		hdr.keyword = (verbIdx == 1) ? "Hide" : "Show";
		hdr.dirty = true;
		b.hide = (verbIdx == 1);
		model.dirty = true; changed = true;
	}
	ImGui::Separator();

	ImGui::TextDisabled(u8"顏色");
	auto editColor = [&](const char* label, int lineIdx) {
		if (lineIdx < 0) {
			ImGui::BeginDisabled();
			float dummy[4] = { 0, 0, 0, 1 };
			ImGui::ColorEdit4(label, dummy, ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_NoInputs);
			ImGui::EndDisabled();
			ImGui::SameLine(); ImGui::TextDisabled(u8"（無）");
			return;
		}
		FilterLine& ln = model.lines[lineIdx];
		int r, g, bl, a; bool ha;
		FilterGetColor(ln, r, g, bl, a, ha);
		float col[4] = { r / 255.f, g / 255.f, bl / 255.f, a / 255.f };
		if (ImGui::ColorEdit4(label, col,
				ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
			int nr = (int)(col[0] * 255.f + 0.5f), ng = (int)(col[1] * 255.f + 0.5f);
			int nb = (int)(col[2] * 255.f + 0.5f), na = (int)(col[3] * 255.f + 0.5f);
			FilterSetColor(ln, nr, ng, nb, na, ha || na != 255);
			model.dirty = true; changed = true;
		}
	};
	editColor(u8"文字顏色", b.idxTextColor);
	editColor(u8"邊框顏色", b.idxBorderColor);
	editColor(u8"背景顏色", b.idxBgColor);
	ImGui::Separator();

	ImGui::TextDisabled(u8"字體大小");
	if (b.idxFontSize >= 0) {
		FilterLine& ln = model.lines[b.idxFontSize];
		int sz = FilterValueInt(ln, 0, 32);
		ImGui::SetNextItemWidth(220 * scale);
		if (ImGui::SliderInt("##fontsize", &sz, 18, 45)) { FilterSetValueInt(ln, 0, sz); model.dirty = true; changed = true; }
	} else {
		ImGui::TextDisabled(u8"（無 SetFontSize）");
	}
	ImGui::Separator();

	ImGui::TextDisabled(u8"音效");
	if (b.idxAlertSound >= 0) {
		FilterLine& ln = model.lines[b.idxAlertSound];
		int id = FilterValueInt(ln, 0, 1);
		int vol = FilterValueInt(ln, 1, 100);
		if (id < 1) id = 1; if (id > 16) id = 16;
		ImGui::SetNextItemWidth(160 * scale);
		if (ImGui::SliderInt(u8"內建音效 ID", &id, 1, 16)) { FilterSetValueInt(ln, 0, id); model.dirty = true; changed = true; }
		ImGui::SetNextItemWidth(220 * scale);
		if (ImGui::SliderInt(u8"音量", &vol, 0, 300)) { FilterSetValueInt(ln, 1, vol); model.dirty = true; changed = true; }
	} else if (b.idxCustomSound >= 0) {
		FilterLine& ln = model.lines[b.idxCustomSound];
		std::string path = ln.values.empty() ? std::string() : ln.values[0].text;
		ImGui::SetNextItemWidth(-260 * scale);
		if (ImGui::InputText(u8"自訂音效", &path)) { FilterSetValueStr(ln, 0, path, true); model.dirty = true; changed = true; }
		ImGui::SameLine();
		if (ImGui::Button(u8"播放")) PlayAudioFile(widen_utf8(path));
		ImGui::SameLine();
		if (ImGui::Button(u8"瀏覽…")) {
			std::wstring p = FileDialog(false);
			if (!p.empty()) { FilterSetValueStr(ln, 0, narrow(p), true); model.dirty = true; changed = true; }
		}
		ImGui::SameLine();
		if (ImGui::Button(u8"音效庫…")) ImGui::OpenPopup("##soundlib");
		if (ImGui::BeginPopup("##soundlib")) {
			std::string picked;
			if (DrawSoundLibrary(picked, scale)) {
				FilterSetValueStr(ln, 0, picked, true); model.dirty = true; changed = true;
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
	} else if (b.idxDisableDropSound >= 0) {
		ImGui::TextDisabled(u8"靜音（DisableDropSound）");
	} else {
		ImGui::TextDisabled(u8"（無音效動作）");
	}

	if (b.idxMinimapIcon >= 0 || b.idxPlayEffect >= 0) {
		ImGui::Separator();
		ImGui::TextDisabled(u8"其他動作（唯讀）");
		if (b.idxMinimapIcon >= 0) ImGui::BulletText("%s", model.lines[b.idxMinimapIcon].raw.c_str());
		if (b.idxPlayEffect >= 0)  ImGui::BulletText("%s", model.lines[b.idxPlayEffect].raw.c_str());
	}
	return changed;
}

// ---- three-pane tier-list ---------------------------------------------------

// Add an English item token to a target tier's BaseType line (full-catalog "add").
// Returns a status message. Output stays English; only that one line changes.
static std::string AddItemToTier(FilterFile& model, TierListIndex& index, int targetBlock,
                                 const std::string& en, const std::string& zh)
{
	if (targetBlock < 0 || targetBlock >= (int)index.tiers.size()) return u8"請先點一個階級設為目標";
	TierEntry& te = index.tiers[targetBlock];
	if (te.baseTypeLineIdx < 0) return u8"目標階級不接受物品（無 BaseType）";
	FilterLine& bl = model.lines[te.baseTypeLineIdx];
	if (FilterHasValue(bl, en)) return u8"目標階級已有：" + zh;
	FilterAddValue(bl, en, true);
	model.dirty = true;
	return u8"已加入：" + zh;
}

void DrawTierListView(FilterFile& model, TierListIndex& index, TierListUIState& ui,
                      const FilterI18n& i18n, IconManager& icons,
                      const ItemLibrary& library, const EconomyService& economy, float scale)
{
	if (index.categories.empty()) { ImGui::TextDisabled(u8"此過濾器沒有可分類的規則。"); return; }
	if (ui.selectedCategory < 0 || ui.selectedCategory >= (int)index.categories.size()) ui.selectedCategory = 0;

	// ---- category tabs (replaces the old left category list) ----
	if (ImGui::BeginTabBar("##cattabs", ImGuiTabBarFlags_FittingPolicyScroll | ImGuiTabBarFlags_TabListPopupButton)) {
		for (int c = 0; c < (int)index.categories.size(); c++) {
			std::string lbl = index.categories[c].display + "##cat" + std::to_string(c);
			if (ImGui::BeginTabItem(lbl.c_str())) { ui.selectedCategory = c; ImGui::EndTabItem(); }
		}
		ImGui::EndTabBar();
	}
	const TierCategory& cat = index.categories[ui.selectedCategory];

	// ---- usage hint + category stats (right-aligned) ----
	ImGui::TextDisabled(u8"拖物品換階級／點欄頭設目標再點物品；每階級「顯示」勾選批次開關、右鍵物品可單獨隱藏；hover 看英文（輸出仍英文）。");
	{
		const bool eco = economy.available();
		double total = 0.0;
		if (eco) {
			for (int ti : cat.tierEntryIdx) {
				const TierEntry& te = index.tiers[ti];
				if (!te.draggable()) continue;
				for (const std::string& it : te.items) {
					PriceInfo p = economy.PriceOf(it);
					if (p.known) total += p.chaos;
				}
			}
			// one-click: re-tier every item in this category to its suggested tier
			if (ImGui::SmallButton(u8"套用本分類建議分階")) {
				struct Mv { int src, dst; std::string item; };
				std::vector<Mv> moves;
				for (int ti : cat.tierEntryIdx) {
					const TierEntry& te = index.tiers[ti];
					if (!te.draggable()) continue;
					for (const std::string& it : te.items) {
						int sug = economy.SuggestTier(it, cat, index);
						if (sug >= 0 && sug != te.blockIndex) moves.push_back({ te.blockIndex, sug, it });
					}
				}
				int n = 0;
				for (const Mv& m : moves)
					if (MoveItemToTier(model, m.src, m.dst, m.item) == TierMoveResult::Ok) n++;
				if (n) ui.needsRebuild = true;
				char msg[64]; snprintf(msg, sizeof(msg), u8"已套用 %d 項建議分階", n);
				ui.status = msg;
			}
			ImGui::SameLine();
		}

		char stat[96];
		if (eco) snprintf(stat, sizeof(stat), u8"%d 個項目  ·  總估值 %s", cat.totalItems, FormatChaos(total).c_str());
		else     snprintf(stat, sizeof(stat), u8"%d 個項目  ·  總估值 —", cat.totalItems);
		float tw = ImGui::CalcTextSize(stat).x;
		float rightX = ImGui::GetContentRegionMax().x - tw;
		if (rightX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(rightX);
		ImGui::AlignTextToFramePadding();
		ImGui::TextDisabled("%s", stat);
	}

	std::vector<std::string> classes; // distinct gear base-classes in this category
	for (int ti : cat.tierEntryIdx) {
		const TierEntry& te = index.tiers[ti];
		for (const std::string& it : te.items) {
			std::string cls = i18n.BaseClass(it);
			if (!cls.empty() && std::find(classes.begin(), classes.end(), cls) == classes.end())
				classes.push_back(cls);
		}
	}
	std::sort(classes.begin(), classes.end());
	if (!classes.empty()) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(u8"基底類別："); ImGui::SameLine();
		ImGui::SetNextItemWidth(160 * scale);
		std::string preview = ui.baseClassFilter.empty() ? std::string(u8"全部") : i18n.ClassNameZh(ui.baseClassFilter);
		if (ImGui::BeginCombo("##bcfilter", preview.c_str())) {
			if (ImGui::Selectable(u8"全部", ui.baseClassFilter.empty())) ui.baseClassFilter.clear();
			for (const std::string& cls : classes)
				if (ImGui::Selectable(i18n.ClassNameZh(cls).c_str(), ui.baseClassFilter == cls)) ui.baseClassFilter = cls;
			ImGui::EndCombo();
		}
	} else {
		ui.baseClassFilter.clear();
	}

	// Sub-category by repoe tag (低/中/高階通貨、精髓、聖甲蟲、傳奇…). Only curated
	// tags present in this category are offered, so the dropdown stays meaningful.
	{
		std::vector<std::string> tags;
		for (int ti : cat.tierEntryIdx) {
			const TierEntry& te = index.tiers[ti];
			for (const std::string& it : te.items)
				for (const std::string& t : i18n.Tags(it))
					if (TagLabel(t) && std::find(tags.begin(), tags.end(), t) == tags.end())
						tags.push_back(t);
		}
		if (!tags.empty()) {
			if (!classes.empty()) ImGui::SameLine(); else ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(u8"子分類："); ImGui::SameLine();
			ImGui::SetNextItemWidth(150 * scale);
			const char* prev = ui.tagFilter.empty() ? u8"全部" : TagLabel(ui.tagFilter);
			if (!prev) prev = u8"全部";
			if (ImGui::BeginCombo("##tagfilter", prev)) {
				if (ImGui::Selectable(u8"全部", ui.tagFilter.empty())) ui.tagFilter.clear();
				for (const std::string& t : tags)
					if (ImGui::Selectable(TagLabel(t), ui.tagFilter == t)) ui.tagFilter = t;
				ImGui::EndCombo();
			}
		} else {
			ui.tagFilter.clear();
		}
	}
	ImGui::Separator();

	// Position of each draggable tier within this category (file order, high-value
	// first) — lets a card tell whether its suggested tier is an upgrade or demote.
	std::unordered_map<int, int> dragPos;
	{
		int pos = 0;
		for (int ti : cat.tierEntryIdx)
			if (index.tiers[ti].draggable()) dragPos[index.tiers[ti].blockIndex] = pos++;
	}
	// First draggable Hide / Show tier in this category — targets for the per-item
	// right-click "hide this item" / "move back to shown".
	int catHideBlock = -1, catShowBlock = -1;
	for (int ti : cat.tierEntryIdx) {
		const TierEntry& te2 = index.tiers[ti];
		if (!te2.draggable()) continue;
		if (model.blocks[te2.blockIndex].hide) { if (catHideBlock < 0) catHideBlock = te2.blockIndex; }
		else if (catShowBlock < 0) catShowBlock = te2.blockIndex;
	}

	float fullW = ImGui::GetContentRegionAvail().x;
	float rightW = fullW * 0.24f;
	if (rightW < 220 * scale) rightW = 220 * scale;
	float gap = ImGui::GetStyle().ItemSpacing.x;
	float midW = fullW - rightW - gap;
	if (midW < 240 * scale) midW = 240 * scale;

	// ---- MIDDLE: collapsible vertical tier list (no horizontal scroll) ----
	ImGui::BeginChild("##tiers", ImVec2(midW, 0), true);
	for (int ti : cat.tierEntryIdx) {
		TierEntry& te = index.tiers[ti];
		FilterBlock& b = model.blocks[te.blockIndex];
		ImGui::PushID(te.blockIndex);

		// collapsible header: sample name + count, with overlapping controls.
		std::string sampleZh = (te.draggable() && !te.items.empty()) ? i18n.DisplayName(te.items[0]) : te.tierLabel;
		char hdr[192];
		if (te.draggable())
			snprintf(hdr, sizeof(hdr), u8"%s%s  ·  %d 物###h", b.hide ? u8"[隱藏] " : "", sampleZh.c_str(), (int)te.items.size());
		else
			snprintf(hdr, sizeof(hdr), u8"%s  （非物品階級）###h", sampleZh.c_str());
		bool open = ImGui::CollapsingHeader(hdr, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_AllowOverlap);

		// drag an item onto the header -> add (from catalog) or move (between tiers)
		if (ImGui::BeginDragDropTarget()) {
			if (ImGui::AcceptDragDropPayload("POBTOOLS_ITEM")) {
				if (ui.dragSrcBlock < 0) { ui.status = AddItemToTier(model, index, te.blockIndex, ui.dragItem, i18n.DisplayName(ui.dragItem)); ui.needsRebuild = true; }
				else { TierMoveResult r = MoveItemToTier(model, ui.dragSrcBlock, te.blockIndex, ui.dragItem); ui.status = MoveMsg(r, ui.dragItem); if (r == TierMoveResult::Ok) ui.needsRebuild = true; }
			}
			ImGui::EndDragDropTarget();
		}

		// right-aligned cluster on the header line: colour swatch + 顯示 + 目標 + 樣式
		float cluster = 230 * scale;
		ImGui::SameLine();
		float clusterX = ImGui::GetContentRegionMax().x - cluster;
		if (clusterX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(clusterX);
		StyleSwatch(model, b.idxTextColor, scale);
		ImGui::SameLine();
		{ bool shown = !b.hide; if (ImGui::Checkbox(u8"顯示", &shown)) { TierSetHide(model, b, !shown); ui.needsRebuild = true; } }
		ImGui::SameLine();
		bool isTarget = (ui.activeTargetTier == te.blockIndex);
		if (ImGui::SmallButton(isTarget ? u8"◉目標" : u8"目標")) ui.activeTargetTier = isTarget ? -1 : te.blockIndex;
		ImGui::SameLine();
		if (ImGui::SmallButton(u8"樣式")) ImGui::OpenPopup("##stylepop");
		if (ImGui::BeginPopup("##stylepop")) { if (DrawBlockStyleEditor(model, b, scale)) ui.needsRebuild = true; ImGui::EndPopup(); }

		if (open) {
			if (!te.draggable()) {
				ImGui::TextDisabled(u8"（依條件分類，非物品清單）");
			} else {
				std::vector<int> show;
				show.reserve(te.items.size());
				for (int i = 0; i < (int)te.items.size(); i++) {
					if (!ui.baseClassFilter.empty() && i18n.BaseClass(te.items[i]) != ui.baseClassFilter) continue;
					if (!ui.tagFilter.empty() && !HasTag(i18n.Tags(te.items[i]), ui.tagFilter)) continue;
					show.push_back(i);
				}
				if (te.items.empty())
					ImGui::TextColored(ImVec4(0.94f, 0.5f, 0.3f, 1.f), u8"⚠ 空，存檔前請補物品");
				else if (show.empty())
					ImGui::TextDisabled(u8"（此基底/子分類無物品）");

				// wrap grid: cards flow left-to-right and wrap; row-clipper for big tiers.
				float cardW = 220 * scale;
				float availW = ImGui::GetContentRegionAvail().x;
				int cols = (int)(availW / (cardW + ImGui::GetStyle().ItemSpacing.x));
				if (cols < 1) cols = 1;
				float rowH = ItemCardHeight(scale, economy.available()) + ImGui::GetStyle().ItemSpacing.y;
				int nrows = ((int)show.size() + cols - 1) / cols;
				ImGuiListClipper clip;
				clip.Begin(nrows, rowH);
				while (clip.Step()) {
					for (int gr = clip.DisplayStart; gr < clip.DisplayEnd; gr++) {
						for (int gc = 0; gc < cols; gc++) {
							int idx = gr * cols + gc;
							if (idx >= (int)show.size()) break;
							if (gc > 0) ImGui::SameLine();
							const std::string& item = te.items[show[idx]];
							ImGui::PushID(show[idx]);
							ItemCard card;
							card.en = item;
							card.zh = i18n.DisplayName(item);
							if (economy.available()) {
								PriceInfo p = economy.PriceOf(item);
								if (p.known) card.value = FormatChaos(p.chaos);
								int sug = economy.SuggestTier(item, cat, index);
								if (sug >= 0 && sug != te.blockIndex) {
									auto cu = dragPos.find(te.blockIndex), su = dragPos.find(sug);
									bool up = (su != dragPos.end() && cu != dragPos.end() && su->second < cu->second);
									card.badge = up ? u8"升" : u8"降";
									card.badgeUpgrade = up;
								}
							}
							if (DrawItemCard(card, cardW, scale, icons)) {
								ui.selectedItem = item;
								if (ui.activeTargetTier >= 0 && ui.activeTargetTier != te.blockIndex) {
									TierMoveResult r2 = MoveItemToTier(model, te.blockIndex, ui.activeTargetTier, item);
									ui.status = MoveMsg(r2, item);
									if (r2 == TierMoveResult::Ok) ui.needsRebuild = true;
								}
							}
							if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", item.c_str());
							if (ImGui::BeginDragDropSource()) {
								ui.dragItem = item; ui.dragSrcBlock = te.blockIndex;
								char tag = 1; ImGui::SetDragDropPayload("POBTOOLS_ITEM", &tag, 1);
								ImGui::Text(u8"移動：%s", card.zh.c_str());
								ImGui::EndDragDropSource();
							}
							if (ImGui::BeginPopupContextItem("##ictx")) {
								ui.selectedItem = item;
								if (!b.hide && catHideBlock >= 0) { if (ImGui::MenuItem(u8"隱藏此物品")) { MoveItemToTier(model, te.blockIndex, catHideBlock, item); ui.needsRebuild = true; } }
								else if (b.hide && catShowBlock >= 0) { if (ImGui::MenuItem(u8"移回顯示")) { MoveItemToTier(model, te.blockIndex, catShowBlock, item); ui.needsRebuild = true; } }
								else ImGui::TextDisabled(u8"（此分類無對應顯示/隱藏階級）");
								ImGui::Separator();
								if (ImGui::MenuItem(u8"複製英文名")) ImGui::SetClipboardText(item.c_str());
								ImGui::EndPopup();
							}
							ImGui::PopID();
						}
					}
				}
			}
		}
		ImGui::PopID();
	}
	ImGui::EndChild();
	ImGui::SameLine();

	// ---- RIGHT: item library ----
	// Two sources: items already in this filter (move between tiers) or the whole
	// game catalog (add any item to the active target tier). Both keep output English.
	ImGui::BeginChild("##lib", ImVec2(0, 0), true);

	// ---- item detail panel (the last-clicked card) ----
	if (!ui.selectedItem.empty()) {
		const std::string& en = ui.selectedItem;
		ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
		ImGui::BeginChild("##detail", ImVec2(0, 116 * scale), true);
		ItemIcon(icons, en);
		ImGui::TextUnformatted(i18n.DisplayName(en).c_str());
		ImGui::SameLine(); ImGui::TextDisabled("(%s)", en.c_str());
		std::string cls = i18n.ItemClass(en);
		if (!cls.empty()) ImGui::TextDisabled(u8"類別：%s", i18n.ClassNameZh(cls).c_str());
		const std::vector<std::string>& dtags = i18n.Tags(en);
		if (!dtags.empty()) {
			std::string tl;
			for (const std::string& t : dtags) {
				const char* lb = TagLabel(t);
				if (!tl.empty()) tl += u8"、";
				tl += lb ? lb : t;
			}
			ImGui::TextWrapped(u8"標籤：%s", tl.c_str());
		}
		if (economy.available()) {
			PriceInfo p = economy.PriceOf(en);
			if (p.known) ImGui::TextDisabled(u8"估值：%s", FormatChaos(p.chaos).c_str());
		}
		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::Separator();
	}

	if (library.loaded())
		ImGui::Checkbox(u8"顯示遊戲全部物品（可加入）", &ui.libShowAll);
	else
		ui.libShowAll = false;

	ImGui::SetNextItemWidth(-FLT_MIN);
	if (ImGui::InputTextWithHint("##itemsearch", u8"搜尋物品（中／英）…", &ui.itemSearch))
		ui.itemSearchLower = to_lower_ascii(ui.itemSearch);
	if (ui.activeTargetTier >= 0)
		ImGui::TextColored(ImVec4(0.55f, 0.7f, 1.f, 1.f),
			ui.libShowAll ? u8"點／拖物品 → 加入目前目標階級" : u8"點物品 → 移到目前目標階級");
	else
		ImGui::TextDisabled(u8"先點某階級標題設為目標");
	ImGui::Separator();

	if (ui.libShowAll) {
		// ---- whole-catalog mode: add any game item to the target tier ----
		const std::vector<LibItem>& all = library.items();
		std::vector<int> rows;
		rows.reserve(all.size());
		for (int i = 0; i < (int)all.size(); i++) {
			const LibItem& it = all[i];
			if (!ui.baseClassFilter.empty() && it.enClass != ui.baseClassFilter) continue;
				if (!ui.tagFilter.empty() && !HasTag(it.tags, ui.tagFilter)) continue;
			if (!ui.itemSearchLower.empty() &&
			    !contains_ci(it.en, ui.itemSearchLower) && !contains_ci(it.zh, ui.itemSearchLower))
				continue;
			rows.push_back(i);
		}
		ImGui::TextDisabled(u8"全部物品 · 符合 %d 項", (int)rows.size());
		ImGui::BeginChild("##librows");
		ImGuiListClipper clip;
		clip.Begin((int)rows.size());
		while (clip.Step()) {
			for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
				const LibItem& it = all[rows[row]];
				ImGui::PushID(rows[row]);
				ItemIcon(icons, it.en);
				if (ImGui::Selectable(it.zh.c_str())) {
					ui.selectedItem = it.en;
					if (ui.activeTargetTier >= 0) {
						ui.status = AddItemToTier(model, index, ui.activeTargetTier, it.en, it.zh);
						ui.needsRebuild = true;
					}
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.en.c_str());
				if (ImGui::BeginDragDropSource()) {
					ui.dragItem = it.en; ui.dragSrcBlock = -1;   // -1 => add (not move)
					char tag = 1; ImGui::SetDragDropPayload("POBTOOLS_ITEM", &tag, 1);
					ImGui::Text(u8"加入：%s", it.zh.c_str());
					ImGui::EndDragDropSource();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	} else {
		// ---- in-filter mode: move an existing item between tiers ----
		struct LibRow { const std::string* item; const std::string* tier; int block; };
		std::vector<LibRow> rows;
		for (int ti : cat.tierEntryIdx) {
			TierEntry& te = index.tiers[ti];
			if (!te.draggable()) continue;
			for (const std::string& it : te.items) {
				if (!ui.baseClassFilter.empty() && i18n.BaseClass(it) != ui.baseClassFilter) continue;
				if (!ui.itemSearchLower.empty()) {
					std::string disp = i18n.DisplayName(it);
					if (!contains_ci(it, ui.itemSearchLower) && !contains_ci(disp, ui.itemSearchLower)) continue;
				}
				rows.push_back({ &it, &te.tierLabel, te.blockIndex });
			}
		}
		ImGui::TextDisabled(u8"%s · 符合 %d 項", cat.display.c_str(), (int)rows.size());
		ImGui::BeginChild("##librows");
		ImGuiListClipper clip;
		clip.Begin((int)rows.size());
		while (clip.Step()) {
			for (int row = clip.DisplayStart; row < clip.DisplayEnd; row++) {
				LibRow& lr = rows[row];
				ImGui::PushID(row);
				ItemIcon(icons, *lr.item);
				if (ImGui::Selectable(i18n.DisplayName(*lr.item).c_str())) {
					if (ui.activeTargetTier >= 0 && ui.activeTargetTier != lr.block) {
						TierMoveResult r = MoveItemToTier(model, lr.block, ui.activeTargetTier, *lr.item);
						ui.status = MoveMsg(r, *lr.item);
						if (r == TierMoveResult::Ok) ui.needsRebuild = true;
					}
				}
				if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"%s\n所在階級：%s", lr.item->c_str(), lr.tier->c_str());
				if (ImGui::BeginDragDropSource()) {
					ui.dragItem = *lr.item; ui.dragSrcBlock = lr.block;
					char tag = 1; ImGui::SetDragDropPayload("POBTOOLS_ITEM", &tag, 1);
					ImGui::Text(u8"移動：%s", i18n.DisplayName(*lr.item).c_str());
					ImGui::EndDragDropSource();
				}
				ImGui::PopID();
			}
		}
		ImGui::EndChild();
	}

	ImGui::EndChild();
}
