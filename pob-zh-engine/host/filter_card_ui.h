// PobTools filter-editor cards: the middle pane (one block's condition rows and
// its look & sound rows, rendered from FilterSchema), the right add-column
// (search, grouped, ticked when the block already has the item) and the
// popovers that edit colours, the minimap icon, the beam and the alert sound.
//
// The draw functions return true when a STRUCTURAL mutation happened (a line
// was inserted / disabled / restored): every cached line/block index is invalid
// from that point, so the caller must stop drawing model-derived UI this frame
// and let the next frame rebuild from FilterDocumentEditor::structureVersion.
#pragma once

#include "editor_shell.h"

// Chinese display of one condition line (keyword + op + translated values).
std::string CardConditionZh(const FilterLine& ln, const FilterI18n& i18n);
// The same without the keyword ("> = 3", "神聖石 崇高石"): what a value reads as.
std::string CardValueZh(const FilterLine& ln, const FilterI18n& i18n);

// A block's conditions joined for row labels / search (capped, Chinese).
std::string CardBlockSummaryZh(const FilterFile& f, const FilterBlock& b, const FilterI18n& i18n);

// "大 · 紅色 · 星形" for a MinimapIcon line; "內建 6 號 · 音量 300" for a sound line.
std::string CardMinimapSummary(const FilterLine& ln);
std::string CardSoundSummary(const FilterLine& ln);
std::string CardEffectSummary(const FilterLine& ln);
const char* CardEffectColorZh(const std::string& token);
const char* CardShapeZh(const std::string& token);

// Middle pane: the 條件 card + the 外觀與音效 card.
bool DrawBlockCards(EditorShell& s, int blockIdx);

// Right pane: the add-column.
bool DrawAddColumn(EditorShell& s, int blockIdx);

// The popovers the cards asked for, submitted at the page's top level (never
// inside a child window or a PushID loop). Returns true after a structural
// mutation.
bool DrawCardPopovers(EditorShell& s);

// ---- editors shared with the batch dialog ----------------------------------
// Colour picker body: SV square + hue, RGBA field, recently used colours.
// rgba is edited in place; true when it changed.
bool CardColorEditor(EditorShell& s, const char* id, int rgba[4]);
// Minimap icon: size Segmented, 11-colour palette, 12-shape grid.
bool CardMinimapEditor(const char* id, int* size, std::string* color, std::string* shape);
// Beam: palette + "只在掉落瞬間".
bool CardEffectEditor(const char* id, std::string* color, bool* temp);
// A popover window (raised surface, 12 px padding). Pair with CardEndPopover.
bool CardBeginPopover(const char* id);
void CardEndPopover();
// The 11-colour palette (beam / minimap tokens). True when a colour was picked.
bool CardEffectPalette(const char* id, std::string* color);
