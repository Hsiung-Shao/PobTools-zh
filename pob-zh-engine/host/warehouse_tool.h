// 倉庫收益統計 — stash revenue tracker, as a standalone window and a launcher tab.
//
// Same two entry points every other tool has: CreateWarehousePanel() for the
// tabbed launcher, ShowWarehouseTool() for the separate window "--warehouse"
// starts. TEST-CHANNEL feature: authentication is a pasted POESESSID until the
// official OAuth application is approved (see warehouse_provider.h).
#pragma once

#include <functional>
#include <string>

class IToolPanel;

IToolPanel* CreateWarehousePanel();

// What a host embedding the revenue panel adds -- the atlas planner's 收益 and
// 設定 tabs. Every member is optional.
struct WarehouseEmbed {
	// Both drawn inside the panel's frame and ID scope. topCard is a first card
	// in the revenue page's top row (the project's per-map cost), bordered and
	// sized like the other two; topCardHeight says how tall its content wants
	// to be, and the row takes the tallest. settingsBottom closes the settings
	// page's left column (the revenue-record buttons).
	std::function<void()> topCard, settingsBottom;
	std::function<float()> topCardHeight;
	// Set: the host's own tab bar picks the page (0 = 收益, 1 = 說明, 2 = 設定;
	// anything else reads as 收益) and the panel draws no tab bar of its own.
	// Null: the panel offers 「收益 / 說明 / 設定」 itself.
	const int* page = nullptr;
};

// CreateWarehousePanel() keeps its plain signature: the launcher and the panel
// selftest take its address.
IToolPanel* CreateWarehousePanelEmbedded(const WarehouseEmbed& embed);

void ShowWarehouseTool(const std::wstring& exeDir, const std::wstring& game,
                       const std::wstring& locale);

// "pob-zh.exe --warehouse-selftest": headless data-layer checks (stash JSON
// fixtures, price keys, ninja parsers, snapshot round-trip, diff and throttle
// arithmetic). Touches no network and never the real PobTools\ directory.
int RunWarehouseSelfTest(const std::wstring& exeDir);

// "pob-zh.exe --warehouse-probe [realm] [league]": ONLINE probe of the sessid
// channel -- reads the saved account/session, runs Verify + ListTabs + one
// FetchTab against the real API. realm defaults to "pc" (PoE1); "poe2" asks the
// PoE2 realm. league defaults to the saved one (or Standard). Report goes to
// stdout and <exeDir>\warehouse_probe.txt (never contains the session id).
// Deliberately not part of any selftest.
int RunWarehouseProbe(const std::wstring& exeDir, const std::wstring& realm = L"",
                      const std::wstring& league = L"");
