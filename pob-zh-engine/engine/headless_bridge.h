// Headless mode's Lua side: the Draw*/screen stubs that let POB's Lua run with
// no renderer, the two globals the bridge script uses to talk to the host
// (PobToolsBridgeSetDispatcher / PobToolsBridgeEmit), and the request pump the
// frame loop calls.
//
// Kept out of ui_api.cpp on purpose: that file is the SimpleGraphic API POB was
// written against, and headless changes none of it -- it only re-points a set
// of globals after InitAPI registered the real ones.
#pragma once

struct lua_State;
class ui_main_c;

namespace HeadlessBridge {

// Re-point every renderer-dependent global at a headless version. Called at the
// end of InitAPI when ui->headless is set; the real functions stay registered
// under no name, so nothing else in the API changes.
void InstallStubs(lua_State* L, ui_main_c* ui);

// Pop every queued host request and run the bridge's dispatcher on it, writing
// one response per request. Called from ui_main_c::Frame before OnFrame.
void DispatchPending(ui_main_c* ui);

} // namespace HeadlessBridge
