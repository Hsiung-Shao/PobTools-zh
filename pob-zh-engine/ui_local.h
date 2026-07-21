// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// UI Local Header
//

#include "common.h"
#include "system.h"
#include "render.h"
#include "core.h"

#include "ui.h"

#define SOL_ALL_SAFETIES_ON 1
#define SOL_USING_CXX_LUAJIT 1
#include <sol/sol.hpp>

#include "ui_console.h"
#include "ui_debug.h"
#include "ui_subscript.h"

#include "ui_main.h"

#include <filesystem>

// Engine self-location helpers (defined in ui_api.cpp). The host exe may sit
// one level above the DLL (dist root) with all DLLs in engine\, so resources
// owned by the engine resolve relative to the DLL, not basePath/CWD.
std::filesystem::path EngineModuleDir();
std::filesystem::path ResolveEngineCfg(const char* name);