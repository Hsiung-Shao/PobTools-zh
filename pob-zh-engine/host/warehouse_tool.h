// 倉庫收益統計 — stash revenue tracker, as a standalone window and a launcher tab.
//
// Same two entry points every other tool has: CreateWarehousePanel() for the
// tabbed launcher, ShowWarehouseTool() for the separate window "--warehouse"
// starts. TEST-CHANNEL feature: authentication is a pasted POESESSID until the
// official OAuth application is approved (see warehouse_provider.h).
#pragma once

#include <string>

class IToolPanel;

IToolPanel* CreateWarehousePanel();

void ShowWarehouseTool(const std::wstring& exeDir, const std::wstring& game,
                       const std::wstring& locale);

// "pob-zh.exe --warehouse-selftest": headless data-layer checks (stash JSON
// fixtures, price keys, ninja parsers, snapshot round-trip, diff and throttle
// arithmetic). Touches no network and never the real PobTools\ directory.
int RunWarehouseSelfTest(const std::wstring& exeDir);

// "pob-zh.exe --warehouse-probe": ONLINE probe of the sessid channel -- reads
// the saved account/session, runs Verify + ListTabs against the real API and
// prints the rate-limit headers' verdict. Deliberately not part of any selftest.
int RunWarehouseProbe(const std::wstring& exeDir);
