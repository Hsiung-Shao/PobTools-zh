// Headless verification that edits made through the translation editor reach
// the engine. Runs against the Data\ directory next to the exe, so point it at
// a scratch deployment; it restores every value it changes and verifies the
// restore. Returns 0 when all checks pass.
#pragma once

int RunEditorSelftest();
