// Headless check that a Chinese item copied out of the game survives the paste
// path: every line must come back as ASCII, because POB destroys anything else.
// Runs against the Data\ directory next to the exe. Returns 0 when all checks
// pass. Read-only -- it never writes to the dictionaries.
#pragma once

int RunPasteSelftest();

// PoE2 dictionaries, real PoE2 zh-TW copies (paste_fixtures_poe2.h).
int RunPasteSelftestPoe2();
