// PobTools filter-editor batch modify (批量修改): apply one style change to many
// blocks at once. Every field is tri-state — Keep (leave the block alone), Set
// (update the existing line or insert the action if absent), Remove (disable the
// line as a "#!" comment).
//
// Index note: ApplyBatchStyle only inserts / disables non-header lines, which
// never changes the number or order of blocks — so the caller's block-index list
// stays valid for the whole run; line lookups are re-done per mutation.
#pragma once

#include "editor_shell.h"

struct BatchStyleOp {
	enum class Tri { Keep, Set, Remove };

	Tri showHide = Tri::Keep;   bool hide = false;          // Remove unused
	Tri textColor = Tri::Keep;  int text[4] = { 255, 255, 255, 255 };
	Tri borderColor = Tri::Keep; int border[4] = { 255, 255, 255, 255 };
	Tri bgColor = Tri::Keep;    int bg[4] = { 0, 0, 0, 255 };
	Tri fontSize = Tri::Keep;   int size = 40;
	Tri sound = Tri::Keep;      bool custom = false;        // built-in vs custom
	                            int soundId = 1, volume = 300;
	                            std::string customPath;
	Tri minimapIcon = Tri::Keep; int mmSize = 1;
	                            std::string mmColor = "White", mmShape = "Circle";
	Tri playEffect = Tri::Keep; std::string fxColor = "White"; bool fxTemp = false;
};

// Apply the op to every listed block. Returns the number of lines touched.
int ApplyBatchStyle(EditorShell& s, const std::vector<int>& blockIdx, const BatchStyleOp& op);

// The 批量修改 modal (call every frame; opens via ImGui::OpenPopup("##batchmodal")).
// Applies to the ticked blocks in s.batchSel on confirm.
void DrawBatchModal(EditorShell& s);
