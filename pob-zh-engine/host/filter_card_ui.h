// PobTools filter-editor cards: the middle pane (one block's condition / action
// rows, rendered from FilterSchema) and the right add-column (tick a card to
// insert its default line, untick to disable it as a "#!" comment).
//
// Both draw functions return true when a STRUCTURAL mutation happened (a line
// was inserted / disabled / restored): every cached line/block index is invalid
// from that point, so the caller must stop drawing model-derived UI this frame
// and let the next frame rebuild from FilterDocumentEditor::structureVersion.
#pragma once

#include "editor_shell.h"

// Chinese display of one condition line (keyword + op + translated values).
std::string CardConditionZh(const FilterLine& ln, const FilterI18n& i18n);

// A block's conditions joined for row labels / search (capped, Chinese).
std::string CardBlockSummaryZh(const FilterFile& f, const FilterBlock& b, const FilterI18n& i18n);

// Middle pane: Show/Hide toggle + condition cards + action cards.
bool DrawBlockCards(EditorShell& s, int blockIdx);

// Right pane: add-column checkboxes grouped per FilterSchemaGroups().
bool DrawAddColumn(EditorShell& s, int blockIdx);

// Colour / shape combos shared with the batch-modify modal. Draw a combo showing
// the current token; return the newly selected token, or "" when unchanged.
std::string CardEffectColorCombo(const char* id, const std::string& cur, float scale);
std::string CardIconShapeCombo(const char* id, const std::string& cur, float scale);
