// PobTools drop-preview (掉落預覽): evaluate the currently-edited filter for a
// synthetic item exactly like the game does — first matching Show/Hide block
// wins, blocks with Continue apply their style and keep evaluating — then render
// the in-game-style label (font size / text / border / background colours, beam,
// minimap icon) and play the block's custom alert sound.
//
// Conditions the preview item does not model (HasExplicitMod, SocketGroup,
// BaseDefencePercentile, ...) are treated as NOT matching — the same outcome a
// plain drop without those mods would get — and reported in unknownConds so the
// UI can disclose the approximation.
#pragma once

#include "filter_model.h"
#include "filter_i18n.h"

#include <string>
#include <vector>

struct EditorShell;

// The synthetic item the evaluator sees. Defaults describe a plain fresh drop.
struct PreviewItem {
	std::string baseType;      // English base name ("Divine Orb")
	std::string classId;       // repoe class id/name; FilterI18n::ClassNameEn resolves
	int rarity = 0;            // 0 Normal / 1 Magic / 2 Rare / 3 Unique
	int itemLevel = 83;
	int dropLevel = 1;
	int areaLevel = 83;
	int stackSize = 1;
	int quality = 0;
	int gemLevel = 1;
	int mapTier = 16;
	int width = 1, height = 1;
	int sockets = 0, linkedSockets = 0;
	bool identified = false, corrupted = false, mirrored = false;
	bool fractured = false, synthesised = false, enchanted = false;
	bool replica = false, blightedMap = false;
};

// The resolved display decision (game defaults pre-filled, actions override).
struct PreviewResult {
	bool matched = false;              // some block decided this item
	bool hidden = false;               // deciding block was Hide
	int blockIdx = -1;                 // last styling block (jump-to-edit target)
	int fontSize = 32;
	unsigned char text[4] = { 200, 200, 200, 255 };
	unsigned char border[4] = { 0, 0, 0, 0 };
	bool hasBorder = false;
	unsigned char back[4] = { 0, 0, 0, 180 };
	int alertId = -1, alertVol = 300;          // PlayAlertSound (built-in)
	std::string customSound; int customVol = 300;
	std::string playEffect;                    // beam colour token(s), "" = none
	std::string minimapIcon;                   // "size Colour Shape", "" = none
	std::vector<std::string> unknownConds;     // condition keywords treated as false
};

// Evaluate `f` for `it` (see file header). Pure — no UI, unit-testable.
PreviewResult EvaluatePreview(const FilterFile& f, const PreviewItem& it,
                              const FilterI18n& i18n);

struct LibItem;

// Build an item that satisfies blockIdx's conditions (BaseType token picked at
// random, numeric conditions solved minimally, rarity/flags set as required).
// Returns false when the block has conditions the preview cannot model
// (HasExplicitMod, SocketGroup, ...) — caller should sample another block.
// `lib` resolves Class-only blocks to a concrete base name (may be empty).
bool SynthesizePreviewItem(const FilterFile& f, int blockIdx, const FilterI18n& i18n,
                           const std::vector<LibItem>& lib, PreviewItem* out);

// 掉落預覽 section (left controls + in-game-style loot canvas).
void DrawDropPreviewSection(EditorShell& s);
