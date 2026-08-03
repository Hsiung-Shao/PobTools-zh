// Headless verification that edits made through the translation editor reach
// the engine. Runs against the Data\ directory next to the exe, so point it at
// a scratch deployment; it restores every value it changes and verifies the
// restore. Returns 0 when all checks pass.
#pragma once

#include <string>

// `games` selects which dictionaries to exercise: "poe1", "poe2", or anything
// else for both. The source-dictionary checks (T14-T23) are poe1-only either
// way -- poe2 has no equivalent data -- and say so in the output.
int RunEditorSelftest(const std::string& games = "both");
