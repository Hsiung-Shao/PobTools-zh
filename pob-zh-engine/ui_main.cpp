// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// Module: UI Main
//

#include "ui_local.h"

#include "translation_manager.h"

// ======
// Locals
// ======

static struct {
	int key;
	const char* str;
} ui_keyNameMap[] = {
	KEY_LMOUSE, "LEFTBUTTON",
	KEY_MMOUSE, "MIDDLEBUTTON", 
	KEY_RMOUSE, "RIGHTBUTTON", 
	KEY_MOUSE4, "MOUSE4",
	KEY_MOUSE5, "MOUSE5",
	KEY_MWHEELUP, "WHEELUP", 
	KEY_MWHEELDOWN, "WHEELDOWN", 
	KEY_BACK, "BACK", 
	KEY_TAB, "TAB", 
	KEY_RETURN, "RETURN", 
	KEY_ESCAPE, "ESCAPE", 
	KEY_SHIFT, "SHIFT",
	KEY_CTRL, "CTRL",
	KEY_ALT, "ALT",
	KEY_PAUSE, "PAUSE",
	KEY_PGUP, "PAGEUP", 
	KEY_PGDN, "PAGEDOWN", 
	KEY_END, "END", 
	KEY_HOME, "HOME",
	KEY_PRINTSCRN, "PRINTSCREEN",
	KEY_INSERT, "INSERT", 
	KEY_DELETE, "DELETE", 
	KEY_UP, "UP", 
	KEY_DOWN, "DOWN", 
	KEY_LEFT, "LEFT", 
	KEY_RIGHT, "RIGHT",
	KEY_F1, "F1",
	KEY_F2, "F2",
	KEY_F3, "F3",
	KEY_F4, "F4",
	KEY_F5, "F5",
	KEY_F6, "F6",
	KEY_F7, "F7",
	KEY_F8, "F8",
	KEY_F9, "F9",
	KEY_F10, "F10",
	KEY_F11, "F11",
	KEY_F12, "F12",
	KEY_F13, "F13",
	KEY_F14, "F14",
	KEY_F15, "F15",
	KEY_NUMLOCK, "NUMLOCK",
	KEY_SCROLL, "SCROLLLOCK",
	0, 0
};

// ==================
// ui_IMain Interface
// ==================

ui_IMain* ui_IMain::GetHandle(sys_IMain* sysHnd, core_IMain* coreHnd)
{
	return new ui_main_c(sysHnd, coreHnd);
}

void ui_IMain::FreeHandle(ui_IMain* hnd)
{
	delete (ui_main_c*)hnd;
}

ui_main_c::ui_main_c(sys_IMain* sysHnd, core_IMain* coreHnd)
	: sys(sysHnd), core(coreHnd), framesSinceWindowHidden(0)
{
	renderer = NULL;
}

// =======================
// Lua Interface Utilities
// =======================

void ui_main_c::LAssert(lua_State* L, int cond, const char* fmt, ...)
{
	if ( !cond ) {
		va_list va;
		va_start(va, fmt);
		lua_pushvfstring(L, fmt, va);
		va_end(va);
		lua_error(L);
	}
}

void ui_main_c::LExpect(lua_State* L, int cond, const char* fmt, ...)
{
	if (!cond) {
		va_list va;
		va_start(va, fmt);
		lua_pushvfstring(L, fmt, va);
		va_end(va);
		throw ui_expectationFailed_s{};
	}
}

int ui_main_c::IsUserData(lua_State* L, int index, const char* metaName)
{
	if (lua_type(L, index) != LUA_TUSERDATA || lua_getmetatable(L, index) == 0) {
		return 0;
	}
	lua_getfield(L, LUA_REGISTRYINDEX, metaName);
	int ret = lua_rawequal(L, -2, -1);
	lua_pop(L, 2);
	return ret;
}

int ui_main_c::PushCallback(const char* name)
{
	lua_getfield(L, LUA_REGISTRYINDEX, "uicallbacks");
	lua_getfield(L, -1, name); // Index callbacks table
	// Stack: -2 = callbacks table, -1 = function or nil
	if (lua_isfunction(L, -1)) {
		lua_remove(L, -2); // Remove callbacks table
		return 0;
	} else {
		lua_pop(L, 1); // Pop nil value
		lua_getfield(L, -1, "MainObject"); // Index callbacks table
		lua_remove(L, -2); // Remove callbacks table
		// Stack: -1 = main object or nil
		if (lua_istable(L, -1)) {
			lua_getfield(L, -1, name); // Index main object
			// Stack: -2 = main object, -1 = function or nil
			if (lua_isfunction(L, -1)) {
				lua_insert(L, -2); // Insert function before main object
				return 1;
			} else {
				lua_pop(L, 2); // Pop main object, nil value
			}
		} else {
			lua_pop(L, 1); // Pop nil value
		}
	}
	return -1;
}

void ui_main_c::PCall(int narg, int nret)
{
	sys->SetWorkDir(scriptWorkDir);
	inLua = true;
	hasActiveCoroutine = false;
	int err = lua_pcall(L, narg, nret, 1);
	lua_getglobal(L, "coroutine");
	lua_getfield(L, -1, "_list");
	lua_pcall(L, 0, 1, 0);

	if (lua_istable(L, -1)) {
		lua_pushnil(L);
		while (lua_next(L, -2)) {
			lua_State* co = lua_tothread(L, -2);
			if (co && lua_status(co) == LUA_YIELD) {
				hasActiveCoroutine = true;
			}
			lua_pop(L, 1);
		}
	}
	lua_pop(L, 2);
	inLua = false;
	sys->SetWorkDir();
	if (err && !didExit) {
		DoError("Runtime error in", lua_tostring(L, -1));
	}
}

void ui_main_c::DoError(const char* msg, const char* error)
{
	auto scriptStr = scriptName.generic_u8string();
	char* errText = AllocStringLen(strlen(msg) + scriptStr.size() + strlen(error) + 30);
	sprintf(errText, "--- SCRIPT ERROR ---\n%s '%s':\n%s\n", msg, scriptStr.c_str(), error);
	sys->Exit(errText);
	FreeString(errText);
	didExit = true;
}

// From lua.c
static int traceback (lua_State *L) {
  if (!lua_isstring(L, 1))  /* 'message' not a string? */
    return 1;  /* keep it intact */
  lua_getglobal(L, "debug");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return 1;
  }
  lua_getfield(L, -1, "traceback");
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 2);
    return 1;
  }
  lua_pushvalue(L, 1);  /* pass error message */
  lua_pushinteger(L, 2);  /* skip this function and traceback */
  lua_call(L, 2, 1);  /* call debug.traceback */
  return 1;
}

static int l_panicFunc(lua_State* L)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, ui_main_c::REGISTRY_KEY);
	ui_main_c* ui = (ui_main_c*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	ui->sys->Error("Unprotected Lua error:\n%s", lua_tostring(L, -1));
	return 0;
}

// ======================
// UI Init/Frame/Shutdown
// ======================

void ui_main_c::Init(int argc, char** argv)
{
	// Find paths
	scriptName = std::filesystem::u8path(argv[0]);
	if (scriptName.is_relative()) {
		scriptName = sys->basePath / scriptName;
	}
	scriptName = canonical(scriptName);

	scriptCfg = scriptName;
	scriptCfg.replace_extension(".cfg");

	auto scriptParent = scriptName.parent_path();
	scriptPath = scriptParent;
	scriptWorkDir = scriptParent;

	scriptArgc = argc;
	scriptArgv = new char*[argc];
	for (int a = 0; a < argc; a++) {
		scriptArgv[a] = AllocString(argv[a]);
	}

	// Load config files (engine-owned: resolved next to the DLL when not
	// present at the CWD-relative legacy location)
	core->config->LoadConfig(ResolveEngineCfg("SimpleGraphic.cfg"));
	core->config->LoadConfig(ResolveEngineCfg("SimpleGraphicAuto.cfg"));
	if (core->config->LoadConfig(scriptCfg)) {
		scriptCfg.clear();
	}

	// Initialise script
	ScriptInit();
	while (restartFlag && !didExit) {
		ScriptShutdown();
		ScriptInit();
	}
}

void ui_main_c::RenderInit(r_featureFlag_e features)
{
	if (renderer) {
		return;
	}

	// Render resources (SimpleGraphic/Fonts/*.tgf) live in the POB folder, not
	// next to this DLL — upstream's exe sits inside the POB folder so basePath
	// works there, but our host exe lives outside it.
	sys->SetWorkDir(scriptWorkDir);

	sys->con->ExecCommands(true);

	// Initialise window
	core->video->Apply();

	// Initialise renderer
	renderer = r_IRenderer::GetHandle(sys);
	renderer->Init(features);

	// Create UI console handler
	conUI = ui_IConsole::GetHandle(this);

	sys->con->Printf("\n");
	sys->SetWorkDir(scriptWorkDir);
}

void ui_main_c::ScriptInit()
{
	sys->con->PrintFunc("UI Init");

	sys->con->Printf("Script: %s\n", scriptName.generic_u8string().c_str());
	if (!scriptPath.empty()) {
		sys->con->Printf("Script working directory: %s\n", scriptWorkDir.generic_u8string().c_str());
	}
	sys->video->SetTitle(scriptName.generic_u8string().c_str());

	restartFlag = false;
	didExit = false;
	renderEnable = false;
	inLua = false;

	// Initialise Lua
	sys->con->Printf("Initialising Lua...\n");
	solState.emplace();
	L = solState->lua_state();
	if ( !L ) sys->Error("Error: unable to create Lua state.");
	lua_atpanic(L, l_panicFunc);
	lua_pushlightuserdata(L, this);
	lua_seti(L, LUA_REGISTRYINDEX, ui_main_c::REGISTRY_KEY);
	lua_pushcfunction(L, traceback);
	lua_pushvalue(L, -1);
	lua_setfield(L, LUA_REGISTRYINDEX, "traceback");

#if _WIN32
	lua_pushboolean(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
#endif

	// Add libraries and APIs
	lua_gc(L, LUA_GCSTOP, 0);
	lua_pushcfunction(L, InitAPI);
	int err = lua_pcall(L, 0, 0, 0);
	if (err) sys->Error("Error initialising Lua environment: \n%s\n", lua_tostring(L, -1));
	lua_gc(L, LUA_GCRESTART, -1);

	// Setup debug system
	debug = ui_IDebug::GetHandle(this);

	// Setup subscript system
	subScriptSize = 16;
	subScriptList = new ui_ISubScript*[subScriptSize];
	for (dword i = 0; i < subScriptSize; i++) {
		subScriptList[i] = NULL;
	}
	
	// Load the script file
	sys->SetWorkDir(scriptWorkDir);
 	err = luaL_loadfile(L, scriptName.filename().generic_u8string().c_str());
	if (err) {
		DoError("Error loading", lua_tostring(L, -1));
		return;
	}
	sys->SetWorkDir();

	// Run the script
	sys->con->Printf("Running script...\n");
	for (int i = 0; i < scriptArgc; i++) {
		lua_pushstring(L, scriptArgv[i]);
	}
	lua_createtable(L, scriptArgc - 1, 1);
	for (int i = 0; i < scriptArgc; i++) {
		lua_pushstring(L, scriptArgv[i]);
		lua_rawseti(L, -2, i);
	}
	lua_setglobal(L, "arg");
	PCall(scriptArgc, 0);

	if ( !didExit && !restartFlag ) {
		// Run initialisation callback
		int extraArgs = PushCallback("OnInit");
		if (extraArgs >= 0) {
			PCall(extraArgs, 0);
		}
	}
	if ( !didExit && !restartFlag ) {
		// PoeCharm: run the engine-owned CJK injection script in POB's Lua state.
		// It monkey-patches POB classes (gem/passive Chinese search & input) at
		// runtime; POB's own files are never touched, so this survives POB
		// self-updates. Loaded from the engine dir, never the POB folder.
		std::filesystem::path injectPath = EngineModuleDir() / "poecharm_inject.lua";
		std::string injectStr = injectPath.generic_u8string();
		sys->SetWorkDir(scriptWorkDir);
		if (luaL_loadfile(L, injectStr.c_str())) {
			sys->con->Printf("PoeCharm inject load error: %s\n", lua_tostring(L, -1));
			lua_pop(L, 1);
		} else {
			PCall(0, 0);
		}
		sys->SetWorkDir();
	}
	if ( !didExit && !restartFlag ) {
		// Check for frame callback
		int extraArgs = PushCallback("OnFrame");
		if (extraArgs >= 0) {
			lua_pop(L, 1 + extraArgs);
		} else {
			sys->con->Printf("\nScript didn't set frame callback, exiting...\n");
			sys->Exit();
		}
	}
}

void ui_main_c::Frame()
{
	// Check for any subscripts we need to run
	bool hasSubscript = false;
	for (dword i = 0; i < subScriptSize; i++) {
		if (subScriptList[i]) {
			hasSubscript = true;
			break;
		}
	}
	// Always runs 10 frames after finishing the boot process
	if (!sys->video->IsVisible() || sys->conWin->IsVisible() || restartFlag || didExit) {
		framesSinceWindowHidden = 0;
	}
	else if (framesSinceWindowHidden <= 10) {
		framesSinceWindowHidden++;
	}
	// Otherwise only runs frames if the mouse is on screen, there is an active coroutine, or there is an active subscript
	else if (!sys->video->IsActive() && !sys->video->IsCursorOverWindow() && !hasActiveCoroutine && !hasSubscript) {
		sys->Sleep(100);
		return;
	}	
	
	if (renderer) {
		// Prepare for rendering
		renderer->BeginFrame();

		sys->video->GetRelativeCursor(cursorX, cursorY);
	}

	renderEnable = true;

	// Run subscript system
	for (dword i = 0; i < subScriptSize; i++) {
		if (subScriptList[i]) {
			subScriptList[i]->SubScriptFrame();
			if ( !subScriptList[i]->IsRunning() ) {
				ui_ISubScript::FreeHandle(subScriptList[i]);
				subScriptList[i] = NULL;
			}
		}
	}

	// Run script
	//sys->con->Printf("OnFrame...\n");
	int extraArgs = PushCallback("OnFrame");
	if (extraArgs >= 0) {
		PCall(extraArgs, 0);
	}

	renderEnable = false;

	if (renderer) {
		// Render console
		//sys->con->Printf("Render console...\n");
		renderer->SetDrawLayer(10000);
		conUI->Render();

		// Finish up
		//sys->con->Printf("EndFrame...\n");
		renderer->EndFrame();
	}

	//sys->con->Printf("Finishing up...\n");
	if ( !sys->video->IsActive() && !hasActiveCoroutine && !hasSubscript ) {
		sys->Sleep(100);
	}

	while (restartFlag) {
		ScriptShutdown();
		if (renderer) {
			renderer->PurgeShaders();
		}
		ScriptInit();
	}
}

void ui_main_c::ScriptShutdown()
{
	// Run exit callback
	int extraArgs = PushCallback("OnExit");
	if (extraArgs >= 0) {
		PCall(extraArgs, 0);
	}

	// Shutdown subscript and debug systems
	for (dword i = 0; i < subScriptSize; i++) {
		if (subScriptList[i]) {
			ui_ISubScript::FreeHandle(subScriptList[i]);
		}
	}
	delete subScriptList;
	ui_IDebug::FreeHandle(debug);

	// PoeCharm: free translation dictionaries (paired with translation_init in InitAPI)
	translation_shutdown();

	// Shutdown Lua
	L = NULL;
	solState.reset();
}

void ui_main_c::Shutdown()
{
	// Shutdown script
	ScriptShutdown();

	if (renderer) {
		ui_IConsole::FreeHandle(conUI);

		// Shutdown renderer
		renderer->Shutdown();
		r_IRenderer::FreeHandle(renderer);	

		// Shutdown window
		sys->video->SetVisible(false);
		core->video->Save();
	}

	// Save config (must resolve the same way LoadConfig did, or the settings
	// silently land in a non-existent CWD-relative folder)
	if (!scriptCfg.empty()) {
		core->config->SaveConfig(scriptCfg);
	} else {
		std::filesystem::path cfg = ResolveEngineCfg("SimpleGraphic.cfg");
		std::error_code ec;
		std::filesystem::create_directories(cfg.parent_path(), ec);
		core->config->SaveConfig(cfg);
	}

	for (int a = 0; a < scriptArgc; a++) {
		FreeString(scriptArgv[a]);
	}
	delete scriptArgv;
}

bool ui_main_c::CanExit()
{
	bool ret = true;
	int extraArgs = PushCallback("CanExit");
	if (extraArgs >= 0) {
		PCall(extraArgs, 1);
		ret = !!lua_toboolean(L, -1);
		lua_pop(L, 1);
	}
	return ret;
}

// ==============
// Input Handling
// ==============

void ui_main_c::KeyEvent(int key, int type)
{
	if (conUI->KeyEvent(key, type)) {
		return;
	}

	switch (type) {
	case KE_CHAR:
		CallKeyHandler("OnChar", key, false);
		break;
	case KE_KEYDOWN:
	case KE_DBLCLK:
		// F2 is an engine-owned hotkey (toggles display translation on KEYUP).
		// Swallow its KEYDOWN, including auto-repeat, so POB never sees it.
		if (key == KEY_F2 && type == KE_KEYDOWN) {
			break;
		}
		CallKeyHandler("OnKeyDown", key, type == KE_DBLCLK);
		break;
	case KE_KEYUP:
		switch (key) {
		case KEY_F2:
			// Toggle display translation (Chinese <-> English original).
			// Handled on KEYUP like F10/PAUSE so key auto-repeat can't flip-flop it.
			translation_set_enabled(!translation_is_enabled());
			break;
		case KEY_F10:
			renderer->ToggleDebugImGui();
			break;
		case KEY_PAUSE:
			if (sys->IsKeyDown(KEY_SHIFT)) {
				debug->ToggleProfiling();
				break;
			}
		default:
			CallKeyHandler("OnKeyUp", key, false);
			break;
		}
		break;
	} 
}

void ui_main_c::CallKeyHandler(const char* hname, int key, bool dblclk)
{
	if ( !L ) return;
	int extraArgs = PushCallback(hname);
	if (extraArgs < 0) {
		return;
	}
	if (key < 128) {
		lua_pushfstring(L, "%c", key);
	} else if (strcmp(hname, "OnChar") == 0) {
		// Unicode codepoint from glfwSetCharCallback → UTF-8
		char utf8[5];
		int len = 0;
		if (key < 0x800) {
			utf8[len++] = (char)(0xC0 | (key >> 6));
			utf8[len++] = (char)(0x80 | (key & 0x3F));
		} else if (key < 0x10000) {
			utf8[len++] = (char)(0xE0 | (key >> 12));
			utf8[len++] = (char)(0x80 | ((key >> 6) & 0x3F));
			utf8[len++] = (char)(0x80 | (key & 0x3F));
		} else if (key < 0x110000) {
			utf8[len++] = (char)(0xF0 | (key >> 18));
			utf8[len++] = (char)(0x80 | ((key >> 12) & 0x3F));
			utf8[len++] = (char)(0x80 | ((key >> 6) & 0x3F));
			utf8[len++] = (char)(0x80 | (key & 0x3F));
		}
		utf8[len] = '\0';
		lua_pushstring(L, utf8);
	} else {
		lua_pushstring(L, NameForKey(key));
	}
	lua_pushboolean(L, dblclk);
	PCall(2 + extraArgs, 0);
}

const char* ui_main_c::NameForKey(int key)
{
	for (int i = 0; ui_keyNameMap[i].key; i++) {
		if (ui_keyNameMap[i].key == key) {
			return ui_keyNameMap[i].str;
		}
	}
	return "?";
}

int ui_main_c::KeyForName(const char* keyName)
{
	if (keyName[1]) {
		// > 1 character in length, do a lookup
		for (int i = 0; ui_keyNameMap[i].key; i++) {
			if ( !_stricmp(keyName, ui_keyNameMap[i].str) ) {
				return ui_keyNameMap[i].key;
			}
		}
	} else {
		if (isalpha(*keyName)) {
			return tolower(*keyName);
		} else if (*keyName == ' ' || isdigit(*keyName)) {
			return *keyName;
		}
	}
	return 0;
}
