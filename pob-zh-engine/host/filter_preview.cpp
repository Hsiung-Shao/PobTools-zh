#include "filter_preview.h"
#include "editor_shell.h"
#include "editor_util.h"
#include "filter_parser.h"
#include "audio_player.h"
#include "sound_manager.h"   // GetSoundFolder
#include "ui_theme.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// evaluator (pure, selftest-covered)

namespace {

bool ciContains(const std::string& hay, const std::string& needleLower)
{
	if (needleLower.empty()) return true;
	std::string h = hay;
	for (char& c : h) if (c >= 'A' && c <= 'Z') c += 32;
	return h.find(needleLower) != std::string::npos;
}

std::string lowerAscii(const std::string& s)
{
	std::string o = s;
	for (char& c : o) if (c >= 'A' && c <= 'Z') c += 32;
	return o;
}

bool numCmp(const std::string& op, int lhs, int rhs)
{
	if (op == ">=") return lhs >= rhs;
	if (op == "<=") return lhs <= rhs;
	if (op == ">") return lhs > rhs;
	if (op == "<") return lhs < rhs;
	if (op == "!=" || op == "!") return lhs != rhs;
	return lhs == rhs;   // "", "=", "=="
}

int rarityTok(const std::string& t)
{
	if (t == "Normal") return 0;
	if (t == "Magic") return 1;
	if (t == "Rare") return 2;
	if (t == "Unique") return 3;
	return -1;
}

// Class/BaseType matching, game semantics: without "==", a value matches when it
// is a substring of the item's string (case-insensitive); "==" is exact;
// "!"/"!=" is exact-not-any.
bool strListMatch(const FilterLine& ln, const std::string& itemStr)
{
	if (ln.op == "!" || ln.op == "!=") {
		for (const FilterToken& t : ln.values)
			if (t.text == itemStr) return false;
		return true;
	}
	bool exact = (ln.op == "==" || ln.op == "=");
	for (const FilterToken& t : ln.values) {
		if (exact ? (t.text == itemStr) : ciContains(itemStr, lowerAscii(t.text)))
			return true;
	}
	return false;
}

// Leading integer of values[idx]; INT_MIN when absent/non-numeric.
bool valueInt(const FilterLine& ln, size_t idx, int* out)
{
	if (idx >= ln.values.size()) return false;
	const std::string& t = ln.values[idx].text;
	if (t.empty()) return false;
	size_t p = 0; int v = 0; bool any = false;
	for (; p < t.size() && t[p] >= '0' && t[p] <= '9'; p++) { v = v * 10 + (t[p] - '0'); any = true; }
	if (!any) return false;
	*out = v;
	return true;
}

bool valueBool(const FilterLine& ln)
{
	return !ln.values.empty() && ln.values[0].text == "True";
}

// Evaluate one condition line. Returns match; *unknown set when the condition
// isn't modelled (caller records + treats as false).
bool evalCond(const FilterLine& ln, const PreviewItem& it, const std::string& className,
              bool* unknown)
{
	*unknown = false;
	const std::string& kw = ln.keyword;

	if (kw == "Class") return strListMatch(ln, className);
	if (kw == "BaseType") return strListMatch(ln, it.baseType);

	if (kw == "Rarity" || kw == "ItemRarity") {
		if (ln.op.empty() || ln.op == "=" || ln.op == "==") {
			for (const FilterToken& t : ln.values)
				if (rarityTok(t.text) == it.rarity) return true;
			return false;
		}
		int rhs = ln.values.empty() ? -1 : rarityTok(ln.values[0].text);
		if (rhs < 0) { *unknown = true; return false; }
		return numCmp(ln.op, it.rarity, rhs);
	}

	// numeric conditions on modelled fields
	struct NumMap { const char* kw; int val; };
	const NumMap nums[] = {
		{ "ItemLevel", it.itemLevel }, { "DropLevel", it.dropLevel },
		{ "AreaLevel", it.areaLevel }, { "StackSize", it.stackSize },
		{ "Quality", it.quality }, { "GemLevel", it.gemLevel },
		{ "MapTier", it.mapTier }, { "WaystoneTier", it.mapTier },
		{ "Width", it.width }, { "Height", it.height },
		{ "Sockets", it.sockets }, { "LinkedSockets", it.linkedSockets },
		{ "CorruptedMods", 0 }, { "EnchantmentPassiveNum", 0 },
		{ "MemoryStrands", 0 },
	};
	for (const NumMap& m : nums) {
		if (kw != m.kw) continue;
		int rhs;
		if (!valueInt(ln, 0, &rhs)) { *unknown = true; return false; }
		return numCmp(ln.op, m.val, rhs);
	}

	// boolean conditions on modelled flags (everything exotic defaults false —
	// the state a plain drop is in)
	struct BoolMap { const char* kw; bool val; };
	const BoolMap bools[] = {
		{ "Identified", it.identified }, { "Corrupted", it.corrupted },
		{ "Mirrored", it.mirrored }, { "FracturedItem", it.fractured },
		{ "SynthesisedItem", it.synthesised }, { "AnyEnchantment", it.enchanted },
		{ "Replica", it.replica }, { "BlightedMap", it.blightedMap },
		{ "UberBlightedMap", false }, { "ElderMap", false }, { "ShapedMap", false },
		{ "ElderItem", false }, { "ShaperItem", false }, { "Scourged", false },
		{ "TransfiguredGem", false }, { "AlternateQuality", false },
		{ "HasImplicitMod", false }, { "HasCruciblePassiveTree", false },
	};
	for (const BoolMap& m : bools)
		if (kw == m.kw) return valueBool(ln) == m.val;

	if (kw == "HasInfluence") {
		// the preview item never has influence: only "None" matches
		for (const FilterToken& t : ln.values)
			if (t.text == "None") return ln.op != "!" && ln.op != "!=";
		return ln.op == "!" || ln.op == "!=";
	}

	// HasExplicitMod / SocketGroup / HasEnchantment / ArchnemesisMod /
	// BaseDefencePercentile / GemQualityType / ... — not modelled.
	*unknown = true;
	return false;
}

void applyColor(const FilterFile& f, int lineIdx, unsigned char out[4])
{
	if (lineIdx < 0) return;
	int r, g, b, a; bool ha;
	FilterGetColor(f.lines[lineIdx], r, g, b, a, ha);
	out[0] = (unsigned char)r; out[1] = (unsigned char)g;
	out[2] = (unsigned char)b; out[3] = (unsigned char)(ha ? a : 255);
}

std::string joinValues(const FilterLine& ln)
{
	std::string o;
	for (const FilterToken& t : ln.values) { if (!o.empty()) o += ' '; o += t.text; }
	return o;
}

// Game default label style before any filter action applies.
void defaultStyle(const PreviewItem& it, const std::string& className, PreviewResult& r)
{
	auto set = [&r](int tr, int tg, int tb) {
		r.text[0] = (unsigned char)tr; r.text[1] = (unsigned char)tg;
		r.text[2] = (unsigned char)tb; r.text[3] = 255;
	};
	if (ciContains(className, "currency")) set(170, 158, 130);
	else if (ciContains(className, "divination")) set(170, 220, 250);
	else if (ciContains(className, "gem")) set(27, 162, 155);
	else if (ciContains(className, "quest")) set(74, 230, 88);
	else if (it.rarity == 1) set(136, 136, 255);
	else if (it.rarity == 2) set(255, 255, 119);
	else if (it.rarity == 3) set(175, 96, 37);
	else set(200, 200, 200);
}

} // namespace

PreviewResult EvaluatePreview(const FilterFile& f, const PreviewItem& it,
                              const FilterI18n& i18n)
{
	PreviewResult r;
	std::string className = i18n.ClassNameEn(it.classId);
	defaultStyle(it, className, r);

	for (int bi = 0; bi < (int)f.blocks.size(); bi++) {
		const FilterBlock& b = f.blocks[bi];
		bool match = true;
		bool hasContinue = false;
		std::vector<std::string> unknownHere;
		for (int li : b.lineIdx) {
			const FilterLine& ln = f.lines[li];
			if (ln.keyword == "Continue") { hasContinue = true; continue; }
			if (ln.kind != FilterLineKind::Condition) continue;
			bool unknown = false;
			if (!evalCond(ln, it, className, &unknown)) {
				if (unknown) unknownHere.push_back(ln.keyword);
				match = false;
				break;
			}
		}
		if (!match) {
			// only surface unknown-condition keywords once per evaluation
			for (const std::string& k : unknownHere)
				if (std::find(r.unknownConds.begin(), r.unknownConds.end(), k) == r.unknownConds.end())
					r.unknownConds.push_back(k);
			continue;
		}

		r.matched = true;
		r.hidden = b.hide;
		r.blockIdx = bi;
		if (b.idxFontSize >= 0) r.fontSize = FilterValueInt(f.lines[b.idxFontSize], 0, 32);
		applyColor(f, b.idxTextColor, r.text);
		applyColor(f, b.idxBgColor, r.back);
		if (b.idxBorderColor >= 0) { applyColor(f, b.idxBorderColor, r.border); r.hasBorder = true; }
		if (b.idxAlertSound >= 0) {
			const FilterLine& ln = f.lines[b.idxAlertSound];
			int id; if (valueInt(ln, 0, &id)) r.alertId = id;
			int v; if (valueInt(ln, 1, &v)) r.alertVol = v;
		}
		if (b.idxCustomSound >= 0) {
			const FilterLine& ln = f.lines[b.idxCustomSound];
			if (!ln.values.empty()) r.customSound = ln.values[0].text;
			int v; if (valueInt(ln, 1, &v)) r.customVol = v;
		}
		if (b.idxPlayEffect >= 0) r.playEffect = joinValues(f.lines[b.idxPlayEffect]);
		if (b.idxMinimapIcon >= 0) r.minimapIcon = joinValues(f.lines[b.idxMinimapIcon]);

		if (!hasContinue) break;   // first non-Continue match decides
	}
	return r;
}

// Build an item satisfying one block's conditions (see header).
bool SynthesizePreviewItem(const FilterFile& f, int blockIdx, const FilterI18n& i18n,
                           const std::vector<LibItem>& lib, PreviewItem* out)
{
	if (blockIdx < 0 || blockIdx >= (int)f.blocks.size()) return false;
	const FilterBlock& b = f.blocks[blockIdx];
	PreviewItem it;
	std::string wantClassTok;      // Class condition token (first), "" if none
	bool haveBase = false;

	auto solveNum = [](const std::string& op, int rhs) {
		if (op == ">") return rhs + 1;
		if (op == "<") return rhs - 1;
		return rhs;   // "", "=", "==", ">=", "<="
	};

	for (int li : b.lineIdx) {
		const FilterLine& ln = f.lines[li];
		if (ln.keyword == "Continue") continue;
		if (ln.kind != FilterLineKind::Condition) continue;
		const std::string& kw = ln.keyword;
		if (ln.op == "!" || ln.op == "!=") continue;   // defaults almost never collide

		if (kw == "BaseType") {
			if (ln.values.empty()) return false;
			const std::string& tok = ln.values[(size_t)rand() % ln.values.size()].text;
			if (ln.op == "==" || ln.op == "=") {
				it.baseType = tok;
			} else {
				// substring pattern ("Essence of") — find a real item containing it
				std::string lower = lowerAscii(tok);
				it.baseType = tok;
				for (size_t k = 0, n = lib.size(); k < n && n; k++) {
					const LibItem& c = lib[((size_t)rand() + k) % n];
					if (ciContains(c.en, lower)) { it.baseType = c.en; it.classId = c.enClass; break; }
				}
			}
			haveBase = true;
		} else if (kw == "Class") {
			if (ln.values.empty()) return false;
			wantClassTok = ln.values[0].text;
		} else if (kw == "Rarity" || kw == "ItemRarity") {
			int rq = ln.values.empty() ? -1 : rarityTok(ln.values[0].text);
			if (rq < 0) return false;
			it.rarity = std::clamp(solveNum(ln.op, rq), 0, 3);
		} else if (kw == "ItemLevel") { int v; if (!valueInt(ln, 0, &v)) return false; it.itemLevel = solveNum(ln.op, v); }
		else if (kw == "DropLevel") { int v; if (!valueInt(ln, 0, &v)) return false; it.dropLevel = solveNum(ln.op, v); }
		else if (kw == "AreaLevel") { int v; if (!valueInt(ln, 0, &v)) return false; it.areaLevel = solveNum(ln.op, v); }
		else if (kw == "StackSize") { int v; if (!valueInt(ln, 0, &v)) return false; it.stackSize = solveNum(ln.op, v); }
		else if (kw == "Quality") { int v; if (!valueInt(ln, 0, &v)) return false; it.quality = solveNum(ln.op, v); }
		else if (kw == "GemLevel") { int v; if (!valueInt(ln, 0, &v)) return false; it.gemLevel = solveNum(ln.op, v); }
		else if (kw == "MapTier" || kw == "WaystoneTier") { int v; if (!valueInt(ln, 0, &v)) return false; it.mapTier = solveNum(ln.op, v); }
		else if (kw == "Width") { int v; if (!valueInt(ln, 0, &v)) return false; it.width = solveNum(ln.op, v); }
		else if (kw == "Height") { int v; if (!valueInt(ln, 0, &v)) return false; it.height = solveNum(ln.op, v); }
		else if (kw == "Sockets") { int v; if (!valueInt(ln, 0, &v)) return false; it.sockets = solveNum(ln.op, v); }
		else if (kw == "LinkedSockets") { int v; if (!valueInt(ln, 0, &v)) return false; it.linkedSockets = solveNum(ln.op, v); }
		else if (kw == "Identified") it.identified = valueBool(ln);
		else if (kw == "Corrupted") it.corrupted = valueBool(ln);
		else if (kw == "Mirrored") it.mirrored = valueBool(ln);
		else if (kw == "FracturedItem") it.fractured = valueBool(ln);
		else if (kw == "SynthesisedItem") it.synthesised = valueBool(ln);
		else if (kw == "AnyEnchantment") it.enchanted = valueBool(ln);
		else if (kw == "Replica") it.replica = valueBool(ln);
		else if (kw == "BlightedMap") it.blightedMap = valueBool(ln);
		else if (kw == "HasInfluence") {
			bool none = false;
			for (const FilterToken& t : ln.values) if (t.text == "None") none = true;
			if (!none) return false;   // we cannot model influenced items
		} else if (kw == "ElderMap" || kw == "ShapedMap" || kw == "UberBlightedMap" ||
		           kw == "ElderItem" || kw == "ShaperItem" || kw == "Scourged" ||
		           kw == "TransfiguredGem" || kw == "AlternateQuality" ||
		           kw == "HasImplicitMod" || kw == "HasCruciblePassiveTree") {
			if (valueBool(ln)) return false;   // requires a state we don't model
		} else {
			return false;   // HasExplicitMod / SocketGroup / ... — unmodelled
		}
	}

	// Resolve base/class when only a Class condition constrains the block.
	if (!haveBase) {
		if (wantClassTok.empty()) return false;   // bare block — nothing meaningful
		std::string lower = lowerAscii(wantClassTok);
		bool found = false;
		for (size_t k = 0, n = lib.size(); k < n && n; k++) {
			const LibItem& c = lib[((size_t)rand() + k) % n];
			if (c.enClass.empty()) continue;
			std::string cn = i18n.ClassNameEn(c.enClass);
			if (cn == wantClassTok || ciContains(cn, lower)) {
				it.baseType = c.en;
				it.classId = c.enClass;
				found = true;
				break;
			}
		}
		if (!found) { it.baseType = wantClassTok; it.classId = wantClassTok; }
	} else if (it.classId.empty()) {
		// exact BaseType token: class from the catalog via i18n meta, else the
		// Class condition token, else unknown.
		std::string cls = i18n.ItemClass(it.baseType);
		it.classId = !cls.empty() ? cls : wantClassTok;
	}

	*out = it;
	return true;
}

// ---------------------------------------------------------------------------
// UI

namespace {

struct DropEntry {
	PreviewItem item;
	PreviewResult res;
	float jitter = 0;
};

// section-local state (single editor window)
std::vector<DropEntry> g_drops;
PreviewItem g_item;                 // the configurable "current item"
std::string g_itemZh;               // display name of g_item
std::string g_search;
int g_masterVol = 80;               // preview master volume %
bool g_autoPlay = true;
std::string g_soundNote;            // what the last evaluation would play

ImU32 col32(const unsigned char c[4]) { return IM_COL32(c[0], c[1], c[2], c[3]); }

// PlayEffect beam colour tokens.
ImU32 effectColor(const std::string& tok)
{
	struct { const char* t; ImU32 c; } k[] = {
		{ "Red", IM_COL32(230, 60, 60, 255) }, { "Green", IM_COL32(80, 220, 100, 255) },
		{ "Blue", IM_COL32(90, 120, 250, 255) }, { "Brown", IM_COL32(160, 110, 60, 255) },
		{ "White", IM_COL32(240, 240, 240, 255) }, { "Yellow", IM_COL32(240, 220, 80, 255) },
		{ "Cyan", IM_COL32(60, 220, 220, 255) }, { "Grey", IM_COL32(130, 130, 130, 255) },
		{ "Orange", IM_COL32(240, 150, 50, 255) }, { "Pink", IM_COL32(240, 120, 200, 255) },
		{ "Purple", IM_COL32(170, 90, 240, 255) },
	};
	for (auto& e : k)
		if (tok.rfind(e.t, 0) == 0) return e.c;
	return IM_COL32(200, 200, 200, 255);
}

PreviewItem itemFromLib(const EditorShell& s, const LibItem& li)
{
	PreviewItem it = g_item;         // keep the user's property tweaks (ilvl 等)
	it.baseType = li.en;
	it.classId = li.enClass;
	bool uniq = false, flat = false;
	for (const std::string& t : li.tags) {
		if (t == "unique") uniq = true;
		if (t == "currency" || t == "divination_card" || t == "gem" || t == "map" ||
		    t == "map_fragment" || t == "quest_item") flat = true;
	}
	if (li.enClass == "StackableCurrency" || li.enClass == "DivinationCard" ||
	    ciContains(li.enClass, "gem") || ciContains(li.enClass, "quest")) flat = true;
	it.rarity = uniq ? 3 : (flat ? 0 : g_item.rarity);
	return it;
}

// Play the sound the evaluation resolved (custom mp3 for real; built-ins are
// game assets we don't have — note them instead).
void playResultSound(const EditorShell& s, const PreviewResult& r)
{
	g_soundNote.clear();
	if (r.hidden) { g_soundNote = u8"（隱藏規則 — 無音效）"; return; }
	if (!r.customSound.empty()) {
		std::wstring file = EdWiden(r.customSound);
		std::wstring path = (file.find(L':') != std::wstring::npos)
			? file : (GetSoundFolder() + L"\\" + file);
		int pct = (int)((float)std::clamp(r.customVol, 0, 300) / 300.f * (float)g_masterVol + 0.5f);
		bool ok = PlayAudioFileVol(path, pct);
		g_soundNote = ok ? (u8"播放 " + r.customSound)
		                 : (u8"找不到/無法播放 " + r.customSound + u8"（確認音效檔在遊戲資料夾）");
	} else if (r.alertId > 0) {
		g_soundNote = u8"內建音效 #" + std::to_string(r.alertId) + u8"（遊戲資產，編輯器無法試聽）";
	} else {
		g_soundNote = u8"此規則沒有音效";
	}
}

void evalDrops(EditorShell& s)
{
	for (DropEntry& d : g_drops) d.res = EvaluatePreview(s.model, d.item, s.i18n);
}

void showSingle(EditorShell& s)
{
	if (g_item.baseType.empty()) return;
	g_drops.clear();
	DropEntry d;
	d.item = g_item;
	d.jitter = 0;
	d.res = EvaluatePreview(s.model, d.item, s.i18n);
	g_drops.push_back(std::move(d));
	if (g_autoPlay) playResultSound(s, g_drops[0].res);
}

// NeverSink 標記解析:區塊 header 的 $type-> 第一節段(分類鍵)與 $tier-> 全路徑。
std::string nsTypeTop(const std::string& header)
{
	size_t p = header.find("$type->");
	if (p == std::string::npos) return std::string();
	p += 7;
	size_t e = p;
	while (e < header.size() && header[e] != ' ' && header[e] != '\t') e++;
	std::string path = header.substr(p, e - p);
	size_t arrow = path.find("->");
	return arrow == std::string::npos ? path : path.substr(0, arrow);
}

std::string nsTierPath(const std::string& header)
{
	size_t p = header.find("$tier->");
	if (p == std::string::npos) return std::string();
	p += 7;
	size_t e = p;
	while (e < header.size() && header[e] != ' ' && header[e] != '\t') e++;
	return header.substr(p, e - p);
}

// 隨機掉落 — 分層取樣:把 Show 區塊按 NeverSink $type-> 第一節段統整,
// 「每個類別都出」;同類別內最多取 2 個「不同 $tier」的區塊(不同階級),
// 物品名稱全批不重複。合成後仍走完整 first-match 判定(可能被更前面的
// 規則攔截 — 那正是遊戲內會發生的事)。
void randomDrops(EditorShell& s)
{
	const FilterFile& f = s.model;
	if (f.blocks.empty()) return;
	g_drops.clear();

	// group Show blocks by top category, keeping the file's category order
	std::vector<std::pair<std::string, std::vector<int>>> groups;
	auto groupOf = [&groups](const std::string& key) -> std::vector<int>& {
		for (auto& g : groups)
			if (g.first == key) return g.second;
		groups.push_back({ key, {} });
		return groups.back().second;
	};
	for (int i = 0; i < (int)f.blocks.size(); i++) {
		if (f.blocks[i].hide) continue;
		std::string key = nsTypeTop(f.blocks[i].headerComment);
		groupOf(key.empty() ? "other" : key).push_back(i);
	}

	std::vector<std::string> usedBase;
	auto baseUsed = [&usedBase](const std::string& b) {
		for (const std::string& u : usedBase) if (u == b) return true;
		return false;
	};

	for (auto& g : groups) {
		std::vector<int>& idxs = g.second;
		for (int i = (int)idxs.size() - 1; i > 0; i--)
			std::swap(idxs[i], idxs[(size_t)rand() % (i + 1)]);

		int taken = 0;
		std::string firstTier;
		for (int bi : idxs) {
			if (taken >= 2) break;
			std::string tier = nsTierPath(f.blocks[bi].headerComment);
			if (taken == 1 && tier == firstTier) continue;   // 第二個要不同階級
			PreviewItem it;
			if (!SynthesizePreviewItem(f, bi, s.i18n, s.library.items(), &it)) continue;
			if (baseUsed(it.baseType)) continue;             // 物品不重複
			usedBase.push_back(it.baseType);
			DropEntry d;
			d.item = it;
			d.jitter = (float)((rand() % 240) - 120);
			d.res = EvaluatePreview(f, d.item, s.i18n);
			g_drops.push_back(std::move(d));
			firstTier = tier;
			taken++;
		}
	}

	// 大字在上的視覺排序(貴重的通常字大),被攔截成隱藏的排最後
	std::stable_sort(g_drops.begin(), g_drops.end(), [](const DropEntry& a, const DropEntry& b) {
		if (a.res.hidden != b.res.hidden) return !a.res.hidden;
		return a.res.fontSize > b.res.fontSize;
	});
	if (g_autoPlay) {
		for (const DropEntry& d : g_drops)
			if (!d.res.hidden && !d.res.customSound.empty()) { playResultSound(s, d.res); return; }
		g_soundNote = u8"（本批掉落沒有自訂音效規則）";
	}
}

} // namespace

void DrawDropPreviewSection(EditorShell& s)
{
	if (!s.loaded) { ImGui::TextDisabled(u8"開啟一個 .filter 後即可預覽掉落。"); return; }
	if (s.doc.file() != &s.model) s.doc.Attach(&s.model);
	if (s.rowsVersion != s.doc.structureVersion()) EdRebuildRows(s);

	const float ctrlW = 330 * s.scale;

	// ---- left: item picker + properties + sound ----
	ImGui::BeginChild("##pvctrl", ImVec2(ctrlW, 0), true);

	ImGui::TextUnformatted(u8"物品");
	ImGui::SetNextItemWidth(-1);
	ImGui::InputTextWithHint("##pvsearch", u8"搜尋物品（中/英文）…", &g_search);
	{
		std::string lower = EdToLowerAscii(g_search);
		ImGui::BeginChild("##pvlist", ImVec2(0, 150 * s.scale), true);
		if (!g_search.empty()) {
			int shown = 0;
			for (const LibItem& it : s.library.items()) {
				if (!EdContainsCI(it.en, lower) && it.zh.find(g_search) == std::string::npos) continue;
				std::string lbl = it.zh == it.en ? it.en : (it.zh + "  (" + it.en + ")");
				if (ImGui::Selectable(lbl.c_str(), g_item.baseType == it.en)) {
					g_item = itemFromLib(s, it);
					g_itemZh = it.zh;
					showSingle(s);
				}
				if (++shown >= 60) { ImGui::TextDisabled(u8"…（縮小關鍵字看更多）"); break; }
			}
			if (!shown) ImGui::TextDisabled(u8"沒有相符物品");
		} else {
			ImGui::TextDisabled(u8"輸入關鍵字，點擊物品即預覽");
		}
		ImGui::EndChild();
	}

	if (!g_item.baseType.empty())
		ImGui::Text(u8"目前：%s", (g_itemZh.empty() ? g_item.baseType : g_itemZh).c_str());

	ImGui::Separator();
	ImGui::TextUnformatted(u8"物品屬性");
	{
		static const char* kRar[] = { u8"普通", u8"魔法", u8"稀有", u8"傳奇" };
		ImGui::SetNextItemWidth(120 * s.scale);
		ImGui::Combo(u8"稀有度", &g_item.rarity, kRar, 4);
		ImGui::SetNextItemWidth(160 * s.scale);
		ImGui::SliderInt(u8"物品等級", &g_item.itemLevel, 1, 100);
		ImGui::SetNextItemWidth(160 * s.scale);
		ImGui::SliderInt(u8"區域等級", &g_item.areaLevel, 1, 90);
		ImGui::SetNextItemWidth(120 * s.scale);
		ImGui::InputInt(u8"堆疊數量", &g_item.stackSize);
		g_item.stackSize = std::clamp(g_item.stackSize, 1, 100);
		ImGui::SetNextItemWidth(120 * s.scale);
		ImGui::SliderInt(u8"品質", &g_item.quality, 0, 30);
		ImGui::SetNextItemWidth(120 * s.scale);
		ImGui::SliderInt(u8"插槽", &g_item.sockets, 0, 6);
		ImGui::SetNextItemWidth(120 * s.scale);
		ImGui::SliderInt(u8"連結", &g_item.linkedSockets, 0, 6);
		ImGui::Checkbox(u8"已鑑定", &g_item.identified);
		ImGui::SameLine();
		ImGui::Checkbox(u8"已汙染", &g_item.corrupted);
		ImGui::SameLine();
		ImGui::Checkbox(u8"破裂", &g_item.fractured);
	}
	if (ImGui::Button(u8"套用屬性重新判定", ImVec2(-1, 0))) {
		for (DropEntry& d : g_drops) {
			// carry the tweaks onto every generated drop of the same base kind
			PreviewItem& pi = d.item;
			pi.rarity = (pi.rarity == 3) ? 3 : g_item.rarity;
			pi.itemLevel = g_item.itemLevel; pi.areaLevel = g_item.areaLevel;
			pi.stackSize = g_item.stackSize; pi.quality = g_item.quality;
			pi.sockets = g_item.sockets; pi.linkedSockets = g_item.linkedSockets;
			pi.identified = g_item.identified; pi.corrupted = g_item.corrupted;
			pi.fractured = g_item.fractured;
		}
		evalDrops(s);
	}

	ImGui::Separator();
	ImGui::TextUnformatted(u8"產生");
	PobUi::PushPrimaryButton();
	if (ImGui::Button(u8"顯示此物品", ImVec2((ctrlW - 30 * s.scale) * 0.5f, 0))) showSingle(s);
	ImGui::SameLine();
	if (ImGui::Button(u8"隨機掉落（全類別）", ImVec2(-1, 0))) randomDrops(s);
	PobUi::PopButtonStyle();
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip(u8"每個 NeverSink 類別各取樣 1-2 條不同階級的規則，物品不重複");
	if (ImGui::Button(u8"清除", ImVec2(-1, 0))) { g_drops.clear(); g_soundNote.clear(); }

	ImGui::Separator();
	ImGui::TextUnformatted(u8"音效");
	ImGui::SetNextItemWidth(-60 * s.scale);
	ImGui::SliderInt(u8"音量", &g_masterVol, 0, 100, "%d%%");
	ImGui::Checkbox(u8"預覽時自動播放", &g_autoPlay);
	if (ImGui::Button(u8"再播一次", ImVec2(120 * s.scale, 0))) {
		for (const DropEntry& d : g_drops)
			if (!d.res.hidden && !d.res.customSound.empty()) { playResultSound(s, d.res); break; }
	}
	ImGui::SameLine();
	if (ImGui::Button(u8"停止")) StopAudio();
	if (!g_soundNote.empty()) {
		ImGui::PushTextWrapPos(ctrlW - 16 * s.scale);
		ImGui::TextDisabled("%s", g_soundNote.c_str());
		ImGui::PopTextWrapPos();
	}

	ImGui::EndChild();
	ImGui::SameLine();

	// ---- right: loot canvas ----
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.055f, 0.07f, 1.0f));
	ImGui::BeginChild("##pvcanvas", ImVec2(0, 0), true);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (g_drops.empty()) {
		ImGui::TextDisabled(u8"左側搜尋並點選物品，或按「隨機掉落」。標籤樣式即當前過濾器判定結果。");
	} else {
		int nHid = 0;
		for (const DropEntry& d : g_drops) if (d.res.hidden) nHid++;
		ImGui::TextDisabled(u8"%d 個掉落樣本%s　· hover 看命中規則，點擊跳至編輯",
			(int)g_drops.size(),
			nHid ? (u8"（含 " + std::to_string(nHid) + u8" 個被隱藏）").c_str() : "");
		ImGui::Spacing();
		ImFont* font = ImGui::GetFont();
		const float baseFs = ImGui::GetFontSize();
		const float cx = ImGui::GetContentRegionAvail().x * 0.5f;
		int jump = -1;
		for (int i = 0; i < (int)g_drops.size(); i++) {
			DropEntry& d = g_drops[i];
			const PreviewResult& r = d.res;
			std::string zh = s.i18n.DisplayName(d.item.baseType);
			std::string label = zh;
			if (d.item.stackSize > 1) label += u8" ×" + std::to_string(d.item.stackSize);

			ImGui::PushID(i);
			if (r.hidden) {
				std::string t = u8"（已隱藏） " + label;
				float lx = cx - ImGui::CalcTextSize(t.c_str()).x * 0.5f + d.jitter * s.scale;
				ImGui::SetCursorPosX(std::max(0.0f, lx));
				ImGui::TextDisabled("%s", t.c_str());
				if (ImGui::IsItemHovered()) {
					ImGui::SetTooltip(u8"%s\n命中 Hide 規則：%s（點擊跳至該規則）", d.item.baseType.c_str(),
						(r.blockIdx >= 0 && r.blockIdx < (int)s.rows.size()) ? s.rows[r.blockIdx].label.c_str() : "?");
				}
				if (ImGui::IsItemClicked()) jump = r.blockIdx;
				ImGui::PopID();
				ImGui::Dummy(ImVec2(0, 4 * s.scale));
				continue;
			}

			const float px = baseFs * (float)r.fontSize / 32.0f;
			ImVec2 tsz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, label.c_str());
			const float padX = 0.45f * px, padY = 0.22f * px;
			const float w = tsz.x + 2 * padX, h = tsz.y + 2 * padY;

			float lx = cx + d.jitter * s.scale - w * 0.5f;
			ImGui::SetCursorPosX(std::max(0.0f, lx));
			bool clicked = ImGui::InvisibleButton("##lbl", ImVec2(w, h));
			bool hov = ImGui::IsItemHovered();
			ImVec2 pmin = ImGui::GetItemRectMin(), pmax = ImGui::GetItemRectMax();

			// beam behind the label
			if (!r.playEffect.empty()) {
				ImU32 bc = effectColor(r.playEffect);
				float bx = (pmin.x + pmax.x) * 0.5f;
				float top = std::max(ImGui::GetWindowPos().y, pmin.y - 90 * s.scale);
				dl->AddRectFilledMultiColor(ImVec2(bx - 2.5f * s.scale, top), ImVec2(bx + 2.5f * s.scale, pmin.y),
					bc & 0x00FFFFFF, bc & 0x00FFFFFF, bc, bc);
			}

			dl->AddRectFilled(pmin, pmax, col32(r.back), 2 * s.scale);
			if (r.hasBorder) dl->AddRect(pmin, pmax, col32(r.border), 2 * s.scale, 0, std::max(1.5f, 0.06f * px));
			dl->AddText(font, px, ImVec2(pmin.x + padX, pmin.y + padY), col32(r.text), label.c_str());
			if (hov) dl->AddRect(pmin, pmax, IM_COL32(255, 255, 255, 110), 2 * s.scale, 0, 1.5f * s.scale);

			// minimap icon marker to the right
			if (!r.minimapIcon.empty()) {
				ImU32 mc = effectColor(r.minimapIcon.find(' ') != std::string::npos
					? r.minimapIcon.substr(r.minimapIcon.find(' ') + 1) : r.minimapIcon);
				dl->AddCircleFilled(ImVec2(pmax.x + 12 * s.scale, (pmin.y + pmax.y) * 0.5f), 5 * s.scale, mc);
			}

			if (hov) {
				std::string tip = d.item.baseType;
				if (r.matched && r.blockIdx >= 0 && r.blockIdx < (int)s.rows.size())
					tip += u8"\n命中規則：" + s.rows[r.blockIdx].label + u8"\n字體 " + std::to_string(r.fontSize);
				else
					tip += u8"\n沒有規則命中（遊戲預設樣式）";
				if (!r.customSound.empty()) tip += u8"\n自訂音效：" + r.customSound;
				else if (r.alertId > 0) tip += u8"\n內建音效 #" + std::to_string(r.alertId);
				if (!r.unknownConds.empty()) {
					tip += u8"\n未模擬條件（視為不符）：";
					for (size_t k = 0; k < r.unknownConds.size() && k < 6; k++)
						tip += (k ? ", " : "") + r.unknownConds[k];
				}
				if (r.matched) tip += u8"\n（點擊跳至該規則編輯）";
				ImGui::SetTooltip("%s", tip.c_str());
			}
			if (clicked && r.matched) jump = r.blockIdx;
			ImGui::PopID();
			ImGui::Dummy(ImVec2(0, 8 * s.scale));
		}
		if (jump >= 0 && jump < (int)s.model.blocks.size()) {
			s.selectedBlock = jump;
			s.selAnchor = s.doc.CaptureAnchor(jump);
			s.section = Section::FilterEdit;
		}
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();
}
