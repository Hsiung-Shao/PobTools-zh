// Timeless Jewel calculator window (Phase 3): a Vilsol-style search form in
// Traditional Chinese. Standalone GLFW+ImGui window, like the atlas planner.
#pragma once

#include <string>
#include <vector>

// Blocking window; fully creates and tears down its own GLFW/ImGui/GL context.
void ShowTimelessJewel(const std::wstring& exeDir, const std::wstring& locale);

// ---- trade site regions -----------------------------------------------------
//
// The trade site exists per region with the same API shape and the same
// language-independent stat ids; only the host, the league names and the
// available console realms differ. Adding a region is one row here plus a
// --tj-realm-check run to confirm its stat ids line up.
struct TradeRealm {
	const char* label;    // UI label (zh-TW)
	const char* host;     // for URL assembly
	const wchar_t* hostW; // for HttpsClient
	bool consoles;        // false => no xbox/sony, the platform row is hidden
};
extern const TradeRealm kTradeRealms[];
extern const int kTradeRealmCount;

// One search cannot carry more than this many seed filters.
constexpr size_t kMaxTradeSeeds = 40;

// The ?q= payload. Language-independent: the same JSON works on every region.
std::string TradeQueryJson(const std::string& tradeStatId, int seed);
std::string TradeQueryJsonMulti(const std::string& tradeStatId, const std::vector<int>& seeds);

// Pure URL assembly (no browser launch) so --tj-selftest can assert on it.
// Returns "" when league or query is empty. platform: 0 pc, 1 xbox, 2 sony,
// ignored by regions without consoles.
std::string TradeSearchUrl(int realmIdx, const std::string& league, int platform,
                           const std::string& queryJson);

// Online cross-region check ("--tj-realm-check"): every conqueror the
// calculator can offer must have its pseudo stat id present on every region.
// Needs network, so it is deliberately NOT part of --tj-selftest.
int RunTradeRealmCheck(const std::wstring& exeDir);

// Trade-export choices, remembered across sessions in PobTools/tj_ui.json
// (same tiny-state pattern as AtlasUiState). Without this a .tw player would
// re-pick their region every single time the window opens.
struct TjUiState {
	int realm = 0;
	int platform = 0;
	std::string league;
	bool Load(const std::wstring& exeDir);
	bool Save(const std::wstring& exeDir) const;
};

// Debug: render one frame of the passive-tree canvas to pt_render.bmp next to
// the exe, headless ("--pt-render [zoom cx cy]"; zoom<=0 = auto-fit).
int RunPassiveTreeRender(const std::wstring& exeDir, float zoom, float cx, float cy);
