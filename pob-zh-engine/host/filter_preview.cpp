#include "filter_preview.h"
#include "editor_shell.h"
#include "editor_util.h"
#include "filter_parser.h"
#include "filter_item_import.h"
#include "clipboard_util.h"
#include "audio_player.h"
#include "sound_manager.h"   // GetSoundFolder
#include "ui_theme.h"
#include "ui_widgets.h"
#include "ui_icons.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <memory>
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
		{ "ElderItem", (it.influence & kInfElder) != 0 },
		{ "ShaperItem", (it.influence & kInfShaper) != 0 }, { "Scourged", false },
		{ "TransfiguredGem", false }, { "AlternateQuality", false },
		{ "HasImplicitMod", false }, { "HasCruciblePassiveTree", false },
	};
	for (const BoolMap& m : bools)
		if (kw == m.kw) return valueBool(ln) == m.val;

	if (kw == "HasInfluence") {
		// Game semantics: any listed influence present matches; "None" matches
		// an uninfluenced item; `!`/`!=` negates. With influence == 0 this is
		// exactly the old "only None matches" behaviour.
		bool neg = (ln.op == "!" || ln.op == "!=");
		bool any = false;
		for (const FilterToken& t : ln.values) {
			if (t.text == "None") { if (it.influence == 0) any = true; continue; }
			unsigned bit = 0;
			if (t.text == "Shaper") bit = kInfShaper;
			else if (t.text == "Elder") bit = kInfElder;
			else if (t.text == "Crusader") bit = kInfCrusader;
			else if (t.text == "Redeemer") bit = kInfRedeemer;
			else if (t.text == "Hunter") bit = kInfHunter;
			else if (t.text == "Warlord") bit = kInfWarlord;
			if (bit && (it.influence & bit)) any = true;
		}
		return neg ? !any : any;
	}

	if (kw == "SocketGroup") {
		// Token = optional leading count + colour letters (RGBWAD), e.g. "RGB",
		// "5GGG". Default op: some linked group holds >= count sockets and
		// contains the colours as a multiset. `==`: a group is exactly the
		// multiset. `!`/`!=` negates. Unknown groups (synthetic items) never
		// match — the state a plain drop is in.
		bool neg = (ln.op == "!" || ln.op == "!=");
		bool exact = (ln.op == "==" || ln.op == "=");
		// >=/> keep containment semantics (count applies to the group size);
		// </<= are degenerate in the wild — leave them unmodelled.
		if (!neg && !exact && !ln.op.empty() && ln.op != ">=" && ln.op != ">") {
			*unknown = true;
			return false;
		}
		auto countOf = [](const std::string& g, char c) {
			int n = 0;
			for (char x : g) if (x == c) n++;
			return n;
		};
		bool any = false;
		for (const FilterToken& t : ln.values) {
			size_t i = 0;
			int minCount = 0;
			while (i < t.text.size() && t.text[i] >= '0' && t.text[i] <= '9')
				minCount = minCount * 10 + (t.text[i++] - '0');
			std::string colours = t.text.substr(i);
			bool valid = !t.text.empty();
			for (char c : colours)
				if (!strchr("RGBWAD", c)) valid = false;
			if (!valid) { *unknown = true; return false; }
			for (const std::string& g : it.socketGroups) {
				if (minCount && (int)g.size() < minCount) continue;
				bool fit = true;
				for (char c : "RGBWAD") {
					if (!c) break;
					int need = countOf(colours, c);
					int got = countOf(g, c);
					if (exact ? (got != need) : (got < need)) { fit = false; break; }
				}
				if (exact && (int)g.size() != (int)colours.size()) fit = false;
				if (fit) { any = true; break; }
			}
		}
		return neg ? !any : any;
	}

	// HasExplicitMod / HasEnchantment / ArchnemesisMod /
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
// UI (design: FilterPreview.dc.html)

namespace Tok = PobUi::Tok;

struct PvDropEntry {
	PreviewItem item;
	PreviewResult res;
	float jitter = 0;
	bool imported = false;              // parsed from a real Ctrl+C item text
	std::string labelOverride;          // canvas label ("" = DisplayName(base))
	std::vector<ImportIssue> importWarnings;
};

// Per-panel state (used to be file statics, which two tabs would have shared).
struct PreviewUiState {
	std::vector<PvDropEntry> drops;
	PreviewItem item;                   // the configurable "current item"
	std::string itemZh;                 // display name of item
	char search[128] = "";
	int masterVol = 80;                 // preview master volume %
	bool autoPlay = true;
	std::string soundNote;              // what the last evaluation would play
	ImportedItem imp;                   // last "paste from game" parse result
	bool importTried = false;           // a paste happened (show summary/warnings)
	int testTip = -1;                   // test aid: draw this drop's tooltip
};

namespace {

PreviewUiState& PUI(EditorShell& s)
{
	if (!s.previewUi) s.previewUi = std::make_shared<PreviewUiState>();
	return *s.previewUi;
}

ImU32 col32(const unsigned char c[4]) { return IM_COL32(c[0], c[1], c[2], c[3]); }
ImFont* SmallFace() { const PobUi::WidgetFonts& wf = PobUi::Fonts(); return wf.small ? wf.small : ImGui::GetFont(); }

// The game's rarity colours (Tok::Rarity*).
ImU32 RarityColor(int r)
{
	switch (r) {
		case 1: return Tok::RarityMagic;
		case 2: return Tok::RarityRare;
		case 3: return Tok::RarityUnique;
		default: return Tok::Text;
	}
}

PreviewItem itemFromLib(const PreviewUiState& u, const LibItem& li)
{
	PreviewItem it = u.item;         // keep the user's property tweaks (ilvl 等)
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
	it.rarity = uniq ? 3 : (flat ? 0 : u.item.rarity);
	return it;
}

std::wstring SoundFolder(EditorShell& s)
{
	if (!s.soundsInit) { s.sounds.Init(s.exeDir); s.soundsInit = true; }
	return s.sounds.folder();
}

// Play the sound the evaluation resolved (custom mp3 for real; built-ins are
// game assets we don't have — note them instead).
void playResultSound(EditorShell& s, PreviewUiState& u, const PreviewResult& r)
{
	u.soundNote.clear();
	if (r.hidden) { u.soundNote = u8"隱藏的規則不播音效"; return; }
	if (!r.customSound.empty()) {
		std::wstring file = EdWiden(r.customSound);
		std::wstring path = (file.find(L':') != std::wstring::npos) ? file : (SoundFolder(s) + L"\\" + file);
		int pct = (int)((float)std::clamp(r.customVol, 0, 300) / 300.f * (float)u.masterVol + 0.5f);
		const bool ok = !s.testMode && PlayAudioFileVol(path, pct);
		u.soundNote = ok ? (u8"播放 " + r.customSound)
		                 : (u8"沒辦法播放 " + r.customSound + u8"（確認音效檔在音效資料夾）");
	} else if (r.alertId > 0) {
		u.soundNote = u8"內建音效 " + std::to_string(r.alertId) + u8" 號（遊戲裡的音檔，這裡沒辦法試聽）";
	} else {
		u.soundNote = u8"這條規則沒有音效";
	}
}

void evalDrops(EditorShell& s, PreviewUiState& u)
{
	for (PvDropEntry& d : u.drops) d.res = EvaluatePreview(s.model, d.item, s.i18n);
}

void showSingle(EditorShell& s, PreviewUiState& u)
{
	if (u.item.baseType.empty()) return;
	u.drops.clear();
	PvDropEntry d;
	d.item = u.item;
	d.jitter = 0;
	d.res = EvaluatePreview(s.model, d.item, s.i18n);
	u.drops.push_back(std::move(d));
	if (u.autoPlay) playResultSound(s, u, u.drops[0].res);
}

// Put the last successfully pasted game item on the canvas. The label mirrors
// the game: name line for rare/unique, pasted base line otherwise.
void importShow(EditorShell& s, PreviewUiState& u, bool append)
{
	if (!u.imp.ok) return;
	if (!append) u.drops.clear();
	PvDropEntry d;
	d.item = u.imp.item;
	d.item.areaLevel = u.item.areaLevel;   // FilterBlade-style: UI supplies it
	d.imported = true;
	d.labelOverride = !u.imp.name.empty() ? u.imp.name : u.imp.baseRaw;
	d.importWarnings = u.imp.warnings;
	d.res = EvaluatePreview(s.model, d.item, s.i18n);
	u.drops.push_back(std::move(d));
	if (u.autoPlay) playResultSound(s, u, u.drops.back().res);
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
void randomDrops(EditorShell& s, PreviewUiState& u)
{
	const FilterFile& f = s.model;
	if (f.blocks.empty()) return;
	u.drops.clear();

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
		for (const std::string& x : usedBase) if (x == b) return true;
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
			PvDropEntry d;
			d.item = it;
			d.jitter = (float)((rand() % 240) - 120);
			d.res = EvaluatePreview(f, d.item, s.i18n);
			u.drops.push_back(std::move(d));
			firstTier = tier;
			taken++;
		}
	}

	// 大字在上的視覺排序(貴重的通常字大),被攔截成隱藏的排最後
	std::stable_sort(u.drops.begin(), u.drops.end(), [](const PvDropEntry& a, const PvDropEntry& b) {
		if (a.res.hidden != b.res.hidden) return !a.res.hidden;
		return a.res.fontSize > b.res.fontSize;
	});
	if (u.autoPlay) {
		for (const PvDropEntry& d : u.drops)
			if (!d.res.hidden && !d.res.customSound.empty()) { playResultSound(s, u, d.res); return; }
		u.soundNote = u8"這一批掉落沒有自訂音效";
	}
}

// Section heading with optional right-side buttons: draws the title, returns
// the x where buttons may start (laid out by the caller from the right).
void SectionTitle(const char* t)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImFont* f = SmallFace();
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((std::floor(PobUi::D(28.0f)) - f->FontSize) * 0.5f)));
	ImGui::PushFont(f);
	ImGui::TextUnformatted(t);
	ImGui::PopFont();
	ImGui::SetCursorScreenPos(p);
}

void SectionRule()
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
	const ImVec2 q = ImGui::GetCursorScreenPos();
	ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x - PobUi::D(12.0f), q.y), ImVec2(p.x + ImGui::GetContentRegionAvail().x + PobUi::D(12.0f), q.y),
	                                    Tok::BorderSubtle, 1.0f);
	ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
}

// A labelled property row: 72 design px label, controls after.
void PropLabel(const char* t)
{
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - SmallFace()->FontSize) * 0.5f)));
	PobUi::Hint(t);
	ImGui::SetCursorScreenPos(ImVec2(p.x + std::floor(PobUi::D(76.0f)), p.y));
}

bool SmallInt(const char* id, int* v, int lo, int hi)
{
	PobUi::PushControlFrame();
	ImGui::SetNextItemWidth(std::floor(PobUi::D(60.0f)));
	const bool ch = ImGui::InputInt(id, v, 0, 0);
	PobUi::PopControlFrame();
	if (ch) *v = std::clamp(*v, lo, hi);
	return ch;
}

bool TickLabel(const char* id, const char* label, bool* v)
{
	const float px = std::floor(PobUi::D(15.0f));
	const ImVec2 p = ImGui::GetCursorScreenPos();
	ImFont* f = SmallFace();
	const ImVec2 ts = f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, label);
	const float h = PobUi::ControlH();
	const bool click = ImGui::InvisibleButton(id, ImVec2(px + PobUi::D(6.0f) + ts.x, h));
	if (click) *v = !*v;
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 b(p.x, p.y + std::floor((h - px) * 0.5f));
	if (*v) {
		dl->AddRectFilled(b, b + ImVec2(px, px), Tok::Accent, PobUi::D(4.0f));
		if (PobUi::Fonts().icons)
			PobUi::IconAt(dl, b + ImVec2(std::floor(px * 0.12f), std::floor(px * 0.08f)), PobIcon::Check, Tok::OnAccent, std::floor(px * 0.8f));
	} else {
		dl->AddRect(b, b + ImVec2(px, px), Tok::BorderStrong, PobUi::D(4.0f), 0, 1.5f);
	}
	dl->AddText(f, f->FontSize, ImVec2(p.x + px + PobUi::D(6.0f), p.y + std::floor((h - ts.y) * 0.5f)), Tok::Text, label);
	return click;
}

void DrawDropTooltip(const EditorShell& s, const PvDropEntry& d, const ImVec2* at)
{
	const PreviewResult& r = d.res;
	if (at) ImGui::SetNextWindowPos(*at, ImGuiCond_Always);
	ImGui::BeginTooltip();
	ImGui::PushTextWrapPos(std::floor(PobUi::D(300.0f)));
	const std::string rule = (r.blockIdx >= 0 && r.blockIdx < (int)s.rows.size()) ? s.rows[r.blockIdx].label : std::string();
	if (r.matched) ImGui::TextUnformatted((u8"命中：" + rule).c_str());
	else ImGui::TextUnformatted(u8"沒有規則命中（遊戲預設樣式）");
	ImGui::PushFont(SmallFace());
	std::string line2 = r.hidden ? std::string(u8"隱藏") : (u8"字級 " + std::to_string(r.fontSize));
	if (!r.customSound.empty()) line2 += u8" · 音效 自訂 " + r.customSound;
	else if (r.alertId > 0) line2 += u8" · 音效 內建 " + std::to_string(r.alertId) + u8" 號";
	ImGui::TextUnformatted(line2.c_str());
	if (!r.unknownConds.empty()) {
		std::string u = u8"未模擬的條件：";
		for (size_t k = 0; k < r.unknownConds.size() && k < 6; k++) u += (k ? ", " : "") + r.unknownConds[k];
		u += u8"（視為不符）";
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Tok::Warning));
		ImGui::TextUnformatted(u.c_str());
		ImGui::PopStyleColor();
	}
	for (size_t k = 0; k < d.importWarnings.size() && k < 4; k++) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Tok::Warning));
		ImGui::TextUnformatted((u8"※ " + d.importWarnings[k].msg).c_str());
		ImGui::PopStyleColor();
	}
	if (r.matched) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Tok::TextMuted));
		ImGui::TextUnformatted(u8"點一下跳到這條規則");
		ImGui::PopStyleColor();
	}
	ImGui::PopFont();
	ImGui::PopTextWrapPos();
	ImGui::EndTooltip();
}

} // namespace

void DrawDropPreviewSection(EditorShell& s)
{
	if (!s.loaded) {
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const float w = (std::min)(avail.x - PobUi::D(32.0f), PobUi::D(520.0f));
		ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(std::floor((avail.x - w) * 0.5f), std::floor(avail.y * 0.3f)));
		PobUi::EmptyState("##pvnofile", PobIcon::Funnel, u8"還沒開啟過濾器", u8"開啟一個 .filter 之後，這裡用它判定掉落物品的樣子", nullptr, w);
		return;
	}
	if (s.doc.file() != &s.model) s.doc.Attach(&s.model);
	if (s.rowsVersion != s.doc.structureVersion()) EdRebuildRows(s);
	PreviewUiState& u = PUI(s);

	const float H = ImGui::GetContentRegionAvail().y;
	const float ctrlW = std::floor(PobUi::D(340.0f));
	const ImVec2 origin = ImGui::GetCursorScreenPos();
	ImDrawList* wdl = ImGui::GetWindowDrawList();
	wdl->AddRectFilled(origin, origin + ImVec2(ctrlW, H), Tok::Surface1);
	wdl->AddLine(origin + ImVec2(ctrlW - 1.0f, 0), origin + ImVec2(ctrlW - 1.0f, H), Tok::Border, 1.0f);

	// ---- left: import, item, properties, generate ----
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(12.0f), PobUi::D(10.0f)));
	ImGui::BeginChild("##pvctrl", ImVec2(ctrlW - 1.0f, H), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	const float cw = ImGui::GetContentRegionAvail().x;
	const float smH = std::floor(PobUi::D(28.0f));

	// import from the game
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		SectionTitle(u8"從遊戲匯入物品");
		const float b2 = PobUi::ButtonWidth(u8"加入畫布", PobUi::BtnSize::Sm);
		const float b1 = PobUi::ButtonWidth(u8"貼上並顯示", PobUi::BtnSize::Sm);
		auto pasteImport = [&](bool append) {
			const std::string txt = ReadClipboardUtf8(s.hostHwnd);
			u.imp = ParseGameItemText(txt, s.i18n, s.library.items());
			u.importTried = true;
			if (u.imp.ok) importShow(s, u, append);
			// on failure the previous canvas is kept; warnings explain below
		};
		ImGui::SetCursorScreenPos(ImVec2(p.x + cw - b1 - b2 - PobUi::D(6.0f), p.y));
		if (PobUi::Button(u8"貼上並顯示", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm)) pasteImport(false);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"在遊戲裡對物品按 Ctrl+C，再按這裡");
		ImGui::SameLine(0, PobUi::D(6.0f));
		if (PobUi::Button(u8"加入畫布", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) pasteImport(true);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"不清空畫布，加在現有的樣本旁邊比較");
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + smH + PobUi::D(6.0f)));
		if (u.importTried) {
			if (u.imp.ok) {
				auto kv = [&](const char* k, const std::string& v, ImU32 col) {
					const ImVec2 q = ImGui::GetCursorScreenPos();
					ImGui::PushFont(SmallFace());
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Tok::TextMuted));
					ImGui::TextUnformatted(k);
					ImGui::PopStyleColor();
					ImGui::PopFont();
					ImGui::SetCursorScreenPos(ImVec2(q.x + std::floor(PobUi::D(76.0f)), q.y));
					ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + cw - PobUi::D(76.0f));
					ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
					ImGui::PushFont(SmallFace());
					ImGui::TextUnformatted(v.c_str());
					ImGui::PopFont();
					ImGui::PopStyleColor();
					ImGui::PopTextWrapPos();
				};
				std::string zhBase = s.i18n.DisplayName(u.imp.baseEn);
				std::string name = u.imp.name.empty() ? zhBase : (u.imp.name + u8" · " + zhBase);
				kv(u8"名稱", name, Tok::Text);
				static const char* kRarZh[] = { u8"一般", u8"魔法", u8"稀有", u8"傳奇" };
				kv(u8"稀有度", kRarZh[std::clamp(u.imp.item.rarity, 0, 3)], RarityColor(u.imp.item.rarity));
				kv(u8"物等 / 品質", std::to_string(u.imp.item.itemLevel) + u8" · " + std::to_string(u.imp.item.quality) + "%", Tok::Text);
				if (!u.imp.socketsRaw.empty()) kv(u8"插槽", u.imp.socketsRaw, Tok::Text);
				if (u.imp.item.stackSize > 1) kv(u8"堆疊", std::to_string(u.imp.item.stackSize), Tok::Text);
				std::string flags;
				auto addFlag = [&flags](bool on, const char* zh) {
					if (on) { if (!flags.empty()) flags += u8"、"; flags += zh; }
				};
				addFlag(u.imp.item.corrupted, u8"已汙染");
				addFlag(u.imp.item.mirrored, u8"已鏡像");
				addFlag(u.imp.item.fractured, u8"破裂");
				addFlag(u.imp.item.synthesised, u8"追憶");
				addFlag(!u.imp.item.identified && u.imp.item.rarity >= 1, u8"未鑑定");
				addFlag(u.imp.item.enchanted, u8"附魔");
				addFlag(u.imp.item.replica, u8"贗品");
				addFlag(u.imp.item.blightedMap, u8"凋落");
				addFlag((u.imp.item.influence & kInfShaper) != 0, u8"塑者");
				addFlag((u.imp.item.influence & kInfElder) != 0, u8"尊師");
				addFlag((u.imp.item.influence & kInfCrusader) != 0, u8"聖戰士");
				addFlag((u.imp.item.influence & kInfRedeemer) != 0, u8"救贖者");
				addFlag((u.imp.item.influence & kInfHunter) != 0, u8"狩獵者");
				addFlag((u.imp.item.influence & kInfWarlord) != 0, u8"總督軍");
				addFlag(u.imp.exarch, u8"灼烙");
				addFlag(u.imp.eater, u8"吞噬");
				if (!flags.empty()) kv(u8"狀態", flags, Tok::Text);
				ImGui::Dummy(ImVec2(0, PobUi::D(2.0f)));
				const std::string al = u8"區域等級沿用下方設定的" + std::to_string(u.item.areaLevel);
				PobUi::Banner("##pvarea", PobUi::BannerTone::Warn, PobIcon::TriangleAlert, al.c_str(),
				              u8"物品文字裡沒有區域等級，要用下方「物品 / 區域」的第二格。", false, nullptr, false, true, cw);
			} else {
				PobUi::Banner("##pvfail", PobUi::BannerTone::Bad, PobIcon::CircleX, u8"剪貼簿裡不是遊戲的物品文字",
				              u8"在遊戲裡對物品按 Ctrl+C 之後再試一次。", false, nullptr, false, true, cw);
			}
			for (const ImportIssue& w : u.imp.warnings) {
				const std::string t = u8"※ " + w.msg;
				PobUi::Hint(t.c_str(), cw, Tok::Warning);
			}
		}
	}
	SectionRule();

	// item search
	{
		ImGui::PushFont(SmallFace());
		ImGui::TextUnformatted(u8"物品");
		ImGui::PopFont();
		PobUi::SearchField("##pvsearch", u.search, (int)sizeof(u.search), u8"搜尋物品（中 / 英文）", cw);
		const std::string q = u.search;
		const std::string lower = EdToLowerAscii(q);
		const float rowH = std::floor(ImGui::GetTextLineHeight() + PobUi::D(8.0f));
		if (!q.empty()) {
			ImGui::BeginChild("##pvlist", ImVec2(cw, rowH * 5.0f + PobUi::D(4.0f)), false);
			ImDrawList* dl = ImGui::GetWindowDrawList();
			ImFont* sf = SmallFace();
			int shown = 0;
			for (const LibItem& it : s.library.items()) {
				if (!EdContainsCI(it.en, lower) && it.zh.find(q) == std::string::npos) continue;
				ImGui::PushID(shown);
				const ImVec2 rp = ImGui::GetCursorScreenPos();
				const float rw = ImGui::GetContentRegionAvail().x;
				const bool click = ImGui::InvisibleButton("##it", ImVec2(rw, rowH));
				const bool hov = ImGui::IsItemHovered();
				const bool on = u.item.baseType == it.en;
				if (on || hov) dl->AddRectFilled(rp, rp + ImVec2(rw, rowH), on ? Tok::AccentSoft : Tok::Surface2, PobUi::D(5.0f));
				const std::string zh = it.zh.empty() ? it.en : it.zh;
				dl->AddText(ImVec2(rp.x + PobUi::D(8.0f), rp.y + std::floor((rowH - ImGui::GetTextLineHeight()) * 0.5f)), Tok::Text, zh.c_str());
				const std::string cls = s.i18n.ClassNameZh(it.enClass);
				if (!cls.empty()) {
					const ImVec2 cs = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, cls.c_str());
					dl->AddText(sf, sf->FontSize, ImVec2(rp.x + rw - cs.x - PobUi::D(8.0f), rp.y + std::floor((rowH - cs.y) * 0.5f)),
					            Tok::TextMuted, cls.c_str());
				}
				if (hov && it.zh != it.en) PobUi::Tooltip(it.en.c_str());
				if (click) {
					u.item = itemFromLib(u, it);
					u.itemZh = it.zh;
					showSingle(s, u);
				}
				ImGui::PopID();
				if (++shown >= 60) { PobUi::Hint(u8"…（再多打幾個字縮小範圍）"); break; }
			}
			if (!shown) PobUi::Hint(u8"沒有相符的物品");
			ImGui::EndChild();
		} else if (!u.item.baseType.empty()) {
			const std::string t = u8"目前：" + (u.itemZh.empty() ? u.item.baseType : u.itemZh);
			PobUi::Hint(t.c_str());
		} else {
			PobUi::Hint(u8"輸入名稱，點一下物品就預覽");
		}
	}
	SectionRule();

	// item properties
	{
		const ImVec2 p = ImGui::GetCursorScreenPos();
		SectionTitle(u8"物品屬性");
		const char* reLbl = u8"套用並重新判定";
		const float rb = PobUi::ButtonWidth(reLbl, PobUi::BtnSize::Sm);
		ImGui::SetCursorScreenPos(ImVec2(p.x + cw - rb, p.y));
		if (PobUi::Button(reLbl, PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) {
			for (PvDropEntry& d : u.drops) {
				// Imported items carry their REAL parsed fields — only the area
				// level (which the text never has) follows the panel.
				if (d.imported) { d.item.areaLevel = u.item.areaLevel; continue; }
				PreviewItem& pi = d.item;
				pi.rarity = (pi.rarity == 3) ? 3 : u.item.rarity;
				pi.itemLevel = u.item.itemLevel; pi.areaLevel = u.item.areaLevel;
				pi.stackSize = u.item.stackSize; pi.quality = u.item.quality;
				pi.sockets = u.item.sockets; pi.linkedSockets = u.item.linkedSockets;
				pi.identified = u.item.identified; pi.corrupted = u.item.corrupted;
				pi.fractured = u.item.fractured;
			}
			evalDrops(s, u);
		}
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"把這些屬性套到畫布上的樣本再判定一次（匯入的物品只套區域等級）");
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + smH + PobUi::D(6.0f)));
		static const char* kRar[4] = { u8"一般", u8"魔法", u8"稀有", u8"傳奇" };
		PropLabel(u8"稀有度");
		PobUi::Segmented("##rar", &u.item.rarity, kRar, 4);
		PropLabel(u8"物品 / 區域");
		SmallInt("##ilvl", &u.item.itemLevel, 1, 100);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"物品等級");
		ImGui::SameLine(0, PobUi::D(6.0f));
		SmallInt("##alvl", &u.item.areaLevel, 1, 90);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"區域等級");
		PropLabel(u8"堆疊 / 品質");
		SmallInt("##stack", &u.item.stackSize, 1, 100);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"堆疊數量");
		ImGui::SameLine(0, PobUi::D(6.0f));
		SmallInt("##qual", &u.item.quality, 0, 30);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"品質");
		PropLabel(u8"插槽 / 連結");
		SmallInt("##sock", &u.item.sockets, 0, 6);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"插槽數");
		ImGui::SameLine(0, PobUi::D(6.0f));
		SmallInt("##link", &u.item.linkedSockets, 0, 6);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"最大連結數");
		PropLabel(u8"狀態");
		TickLabel("##idf", u8"已鑑定", &u.item.identified);
		ImGui::SameLine(0, PobUi::D(10.0f));
		TickLabel("##cor", u8"已汙染", &u.item.corrupted);
		ImGui::SameLine(0, PobUi::D(10.0f));
		TickLabel("##frc", u8"破裂", &u.item.fractured);
	}
	SectionRule();

	// generate + sound
	{
		const float g = PobUi::D(6.0f);
		const float bClear = PobUi::ButtonWidth(u8"清除", PobUi::BtnSize::Sm);
		const float bHalf = std::floor((cw - bClear - g * 2.0f) * 0.5f);
		if (PobUi::Button(u8"顯示這個物品", PobUi::BtnKind::Primary, PobUi::BtnSize::Sm, nullptr, bHalf, !u.item.baseType.empty()))
			showSingle(s, u);
		ImGui::SameLine(0, g);
		if (PobUi::Button(u8"隨機掉落一批", PobUi::BtnKind::Secondary, PobUi::BtnSize::Sm, nullptr, bHalf)) randomDrops(s, u);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"每個 NeverSink 類別抽 1~2 條不同階級的規則，物品不重複");
		ImGui::SameLine(0, g);
		if (PobUi::Button(u8"清除", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm)) { u.drops.clear(); u.soundNote.clear(); }

		ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
		const ImVec2 p = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + std::floor((PobUi::ControlH() - SmallFace()->FontSize) * 0.5f)));
		PobUi::Hint(u8"音量");
		ImGui::SetCursorScreenPos(ImVec2(p.x + PobUi::D(40.0f), p.y));
		ImGui::SetNextItemWidth(std::floor(PobUi::D(130.0f)));
		ImGui::SliderInt("##mvol", &u.masterVol, 0, 100, "%d%%");
		const float swW = PobUi::SwitchWidth();
		ImFont* sf = SmallFace();
		const float apW = sf->CalcTextSizeA(sf->FontSize, FLT_MAX, 0.0f, u8"自動播放").x;
		ImGui::SetCursorScreenPos(ImVec2(p.x + cw - swW - apW - PobUi::D(8.0f), p.y + std::floor((PobUi::ControlH() - sf->FontSize) * 0.5f)));
		PobUi::Hint(u8"自動播放");
		ImGui::SetCursorScreenPos(ImVec2(p.x + cw - swW, p.y));
		PobUi::Switch("##autoplay", &u.autoPlay);
		if (ImGui::IsItemHovered()) PobUi::Tooltip(u8"預覽時自動播放命中規則的自訂音效");
		ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + PobUi::ControlH() + PobUi::D(4.0f)));
		if (PobUi::Button(u8"再播一次", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Play)) {
			for (const PvDropEntry& d : u.drops)
				if (!d.res.hidden && !d.res.customSound.empty()) { playResultSound(s, u, d.res); break; }
		}
		ImGui::SameLine(0, g);
		if (PobUi::Button(u8"停止", PobUi::BtnKind::Ghost, PobUi::BtnSize::Sm, PobIcon::Square)) StopAudio();
		if (!u.soundNote.empty()) PobUi::Hint(u.soundNote.c_str(), cw);
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();

	// ---- right: loot canvas ----
	ImGui::SetCursorScreenPos(origin + ImVec2(ctrlW, 0));
	const float canW = ImGui::GetContentRegionAvail().x;
	wdl->AddRectFilled(origin + ImVec2(ctrlW, 0), origin + ImVec2(ctrlW + canW, H), Tok::Canvas);
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(PobUi::D(16.0f), PobUi::D(12.0f)));
	ImGui::BeginChild("##pvcanvas", ImVec2(canW, H), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
	ImGui::PopStyleVar();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	if (u.drops.empty()) {
		const ImVec2 avail = ImGui::GetContentRegionAvail();
		const float w = (std::min)(avail.x, PobUi::D(460.0f));
		ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(std::floor((avail.x - w) * 0.5f), std::floor(avail.y * 0.3f)));
		PobUi::EmptyState("##pvempty", PobIcon::Search, u8"畫布是空的",
		                  u8"左側搜尋並點一個物品，或按「隨機掉落一批」。標籤樣子就是目前過濾器判定的結果。", nullptr, w);
	} else {
		int nHid = 0;
		for (const PvDropEntry& d : u.drops) if (d.res.hidden) nHid++;
		std::string head = std::to_string((int)u.drops.size()) + u8" 件";
		if (nHid) head += u8"（" + std::to_string(nHid) + u8" 件被隱藏）";
		head += u8" · 點標籤跳到命中的規則";
		PobUi::Hint(head.c_str());
		ImGui::Dummy(ImVec2(0, PobUi::D(6.0f)));
		ImFont* font = ImGui::GetFont();
		const float baseFs = ImGui::GetFontSize();
		const float cx = ImGui::GetContentRegionAvail().x * 0.5f;
		int jump = -1;
		for (int i = 0; i < (int)u.drops.size(); i++) {
			PvDropEntry& d = u.drops[i];
			const PreviewResult& r = d.res;
			std::string label = d.labelOverride.empty() ? s.i18n.DisplayName(d.item.baseType) : d.labelOverride;
			if (d.item.stackSize > 1) label += u8" ×" + std::to_string(d.item.stackSize);

			ImGui::PushID(i);
			if (r.hidden) {
				const std::string t = u8"（已隱藏）" + label;
				const float lx = cx - ImGui::CalcTextSize(t.c_str()).x * 0.5f + d.jitter * PobUi::D(1.0f);
				ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x, lx));
				ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(Tok::TextFaint));
				ImGui::TextUnformatted(t.c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) DrawDropTooltip(s, d, nullptr);
				if (ImGui::IsItemClicked()) jump = r.blockIdx;
				if (u.testTip == i) { const ImVec2 at = ImGui::GetItemRectMax() + ImVec2(PobUi::D(12.0f), 0); DrawDropTooltip(s, d, &at); }
				ImGui::PopID();
				ImGui::Dummy(ImVec2(0, PobUi::D(4.0f)));
				continue;
			}

			const float px = baseFs * (float)r.fontSize / 32.0f;
			const ImVec2 tsz = font->CalcTextSizeA(px, FLT_MAX, 0.0f, label.c_str());
			const float padX = 0.45f * px, padY = 0.22f * px;
			const float w = tsz.x + 2 * padX, h = tsz.y + 2 * padY;

			const float lx = cx + d.jitter * PobUi::D(1.0f) - w * 0.5f;
			ImGui::SetCursorPosX((std::max)(ImGui::GetStyle().WindowPadding.x, lx));
			const bool clicked = ImGui::InvisibleButton("##lbl", ImVec2(w, h));
			const bool hov = ImGui::IsItemHovered();
			const ImVec2 pmin = ImGui::GetItemRectMin(), pmax = ImGui::GetItemRectMax();

			// beam behind the label
			if (!r.playEffect.empty()) {
				const ImU32 bc = EdEffectColor(r.playEffect);
				const float bx = (pmin.x + pmax.x) * 0.5f;
				const float top = (std::max)(ImGui::GetWindowPos().y, pmin.y - PobUi::D(90.0f));
				dl->AddRectFilledMultiColor(ImVec2(bx - PobUi::D(2.5f), top), ImVec2(bx + PobUi::D(2.5f), pmin.y),
				                            bc & 0x00FFFFFF, bc & 0x00FFFFFF, (bc & 0x00FFFFFF) | 0x90000000u,
				                            (bc & 0x00FFFFFF) | 0x90000000u);
			}
			dl->AddRectFilled(pmin, pmax, col32(r.back));
			if (r.hasBorder) dl->AddRect(pmin, pmax, col32(r.border), 0.0f, 0, (std::max)(1.5f, 0.06f * px));
			dl->AddText(font, px, ImVec2(pmin.x + padX, pmin.y + padY), col32(r.text), label.c_str());
			if (hov) {
				dl->AddRect(pmin - ImVec2(2, 2), pmax + ImVec2(2, 2), Tok::Accent, 0.0f, 0, 1.5f);
				ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			}
			// minimap icon marker to the right
			if (!r.minimapIcon.empty()) {
				const size_t sp = r.minimapIcon.find(' ');
				const ImU32 mc = EdEffectColor(sp != std::string::npos ? r.minimapIcon.substr(sp + 1) : r.minimapIcon);
				dl->AddCircleFilled(ImVec2(pmax.x + PobUi::D(12.0f), (pmin.y + pmax.y) * 0.5f), PobUi::D(5.0f), mc);
			}
			if (hov) DrawDropTooltip(s, d, nullptr);
			if (u.testTip == i) { const ImVec2 at(pmax.x + PobUi::D(24.0f), pmin.y); DrawDropTooltip(s, d, &at); }
			if (clicked && r.matched) jump = r.blockIdx;
			ImGui::PopID();
			ImGui::Dummy(ImVec2(0, PobUi::D(8.0f)));
		}
		if (jump >= 0 && jump < (int)s.model.blocks.size()) {
			s.scrollToSel = true;
			s.selectedBlock = jump;
			s.selAnchor = s.doc.CaptureAnchor(jump);
			s.section = Section::FilterEdit;
		}
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();
	ImGui::SetCursorScreenPos(origin + ImVec2(0, H));
}

// ---- test aids ----------------------------------------------------------------

void PreviewTestDrops(EditorShell& s, unsigned seed, int tipIdx)
{
	PreviewUiState& u = PUI(s);
	srand(seed);
	u.autoPlay = false;
	randomDrops(s, u);
	u.testTip = tipIdx;
}

void PreviewTestImport(EditorShell& s, const std::string& itemText)
{
	PreviewUiState& u = PUI(s);
	u.imp = ParseGameItemText(itemText, s.i18n, s.library.items());
	u.importTried = true;
	u.autoPlay = false;
	if (u.imp.ok) importShow(s, u, true);
}
