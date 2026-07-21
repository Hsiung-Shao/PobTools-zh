// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// Module: UI Sub Script
//

#include "ui_local.h"

// =======
// Classes
// =======

struct ssTweenData_s {
	ssTweenData_s* next;
	enum {
		NIL,
		BOOLEAN,
		NUM,
		STRING
	} type;
	union {
		bool boolean;
		double num;
		char* string;
	};
};

struct ssCall_s {
	ssCall_s* next = nullptr;
	const char* name = nullptr;
	ssTweenData_s* data = nullptr;
};

// =======================
// ui_ISubScript Interface
// =======================

class ui_subscript_c: public ui_ISubScript, public thread_c {
public:
	// Interface
	bool	Start();
	void	SubScriptFrame();
	bool	IsRunning();
	size_t	GetScriptMemory();

	// Encapsulated
	ui_subscript_c(ui_main_c* ui, dword id);
	~ui_subscript_c();

	ui_main_c* ui = nullptr;
	dword	id = 0;

	lua_State* L = nullptr;
	bool	running = false;
	bool	finished = false;
	bool	subWriting = false;
	ssCall_s* subCalls = nullptr;
	bool	funcWaiting = false;
	ssCall_s funcCall;
	char*	errorStr = nullptr;

	void	Stop();

	void	ThreadProc();

	void	LAssert(int cond, const char* fmt, ...);
};

ui_ISubScript* ui_ISubScript::GetHandle(ui_main_c* ui, dword id)
{
	return new ui_subscript_c(ui, id);
}

void ui_ISubScript::FreeHandle(ui_ISubScript* hnd)
{
	delete (ui_subscript_c*)hnd;
}

ui_subscript_c::ui_subscript_c(ui_main_c* ui, dword id)
	: thread_c(ui->sys), ui(ui), id(id)
{
	L = NULL;
	errorStr = NULL;
	running = false;
	finished = false;
}

ui_subscript_c::~ui_subscript_c()
{
	Stop();

	FreeString(errorStr);
	if (L) {
		lua_close(L);
	}
}

// =======================
// Lua Interface Utilities
// =======================

static ui_subscript_c* GetSSPtr(lua_State* L)
{
	lua_rawgeti(L, LUA_REGISTRYINDEX, 0);
	ui_subscript_c* ss = (ui_subscript_c*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	return ss;
}

void ui_subscript_c::LAssert(int cond, const char* fmt, ...)
{
	if ( !cond ) {
		va_list va;
		va_start(va, fmt);
		lua_pushvfstring(L, fmt, va);
		va_end(va);
		lua_error(L);
	}
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
	ui_subscript_c* ss = GetSSPtr(L);
	ss->ui->sys->Error("Unprotected Lua error:\n%s", lua_tostring(L, -1));
	return 0;
}

// ==================
// Tween Data Helpers
// ==================

static ssTweenData_s* ssBuildData(lua_State* L, int start)
{
	ssTweenData_s* ret = NULL;
	ssTweenData_s* last = NULL;
	int n = lua_gettop(L);
	for (int a = start; a <= n; a++) {
		ssTweenData_s* d = new ssTweenData_s;
		d->next = NULL;
		if (last) {
			last->next = d;
			last = d;
		} else {
			ret = last = d;
		}
		switch (lua_type(L, a)) {
		case LUA_TNIL:
			d->type = ssTweenData_s::NIL;
			break;
		case LUA_TBOOLEAN:
			d->type = ssTweenData_s::BOOLEAN;
			d->boolean = (lua_toboolean(L, a) != 0);
			break;
		case LUA_TNUMBER:
			d->type = ssTweenData_s::NUM;
			d->num = lua_tonumber(L, a);
			break;
		case LUA_TSTRING:
			d->type = ssTweenData_s::STRING;
			d->string = AllocString(lua_tostring(L, a));
			break;
		}
	}
	lua_settop(L, start - 1);
	return ret;
}

static void ssWipeData(ssTweenData_s* list)
{
	while (list) {
		ssTweenData_s* data = list;
		if (data->type == ssTweenData_s::STRING) {
			delete data->string;
		}
		list = data->next;
		delete data;
	}
}

static int ssPushData(lua_State* L, ssTweenData_s* list)
{
	int numdat = 0;
	for ( ; list; numdat++) {
		ssTweenData_s* data = list;
		lua_checkstack(L, 1);
		switch (data->type) {
		case ssTweenData_s::NIL:
			lua_pushnil(L);
			break;
		case ssTweenData_s::BOOLEAN:
			lua_pushboolean(L, data->boolean);
			break;
		case ssTweenData_s::NUM:
			lua_pushnumber(L, data->num);
			break;
		case ssTweenData_s::STRING:
			lua_pushstring(L, data->string);
			delete data->string;
			break;
		}
		list = data->next;
		delete data;
	}
	return numdat;
}

// ============================
// Sub Script API and Utilities
// ============================

static int l_SubScriptFunc(lua_State* L)
{
	ui_subscript_c* ss = GetSSPtr(L);
	int n = lua_gettop(L);
	const char* funcName = lua_tostring(L, lua_upvalueindex(1)); 
	for (int i = 1; i <= n; i++) {
		ss->LAssert(lua_isnil(L, i) || lua_isboolean(L, i) || lua_isnumber(L, i) || lua_isstring(L, i),
			"%s() argument %d: only nil, boolean, number and string can be passed to the main script", funcName, i);
	}
	ss->funcCall.name = funcName;
	ss->funcCall.data = ssBuildData(L, 1);
	ss->funcWaiting = true;
	while (ss->funcWaiting) ss->ui->sys->Sleep(1);
	return ssPushData(L, ss->funcCall.data);
}

static int l_SubScriptSub(lua_State* L)
{
	ui_subscript_c* ss = GetSSPtr(L);
	int n = lua_gettop(L);
	const char* subName = lua_tostring(L, lua_upvalueindex(1));
	for (int i = 1; i <= n; i++) {
		ss->LAssert(lua_isnil(L, i) || lua_isboolean(L, i) || lua_isnumber(L, i) || lua_isstring(L, i),
			"%s() argument %d: only nil, boolean, number and string can be passed to the main script", subName, i);
	}
	ss->subWriting = true;
	ssCall_s* calls = ss->subCalls;
	ssCall_s* call = new ssCall_s;
	call->name = subName;
	call->data = ssBuildData(L, 1);
	call->next = NULL;
	if (calls) {
		while (calls->next) {
			calls = calls->next;
		}
		calls->next = call;
	} else {
		ss->subCalls = call;
	}
	ss->subWriting = false;
	return 0;
}

static int l_os_exit(lua_State* L)
{
	return 0;
}

static void parseSubScriptList(lua_State* L, const char* clist, lua_CFunction func)
{
	char* list = AllocString(clist);
	char* tok = strtok(list, ",");
	while (tok) {
		lua_pushstring(L, tok);
		lua_pushcclosure(L, func, 1);
		lua_setglobal(L, tok);
		tok = strtok(NULL, ",");
	}
	delete list;
}

static void l_hookStop(lua_State* L, lua_Debug* dbg)
{
	lua_pushstring(L, "dummy");
	lua_error(L);
}

// ===================
// UI Sub Script Class
// ===================

bool ui_subscript_c::Start()
{
	subWriting = false;
	subCalls = NULL;
	funcWaiting = false;
	errorStr = NULL;

	// Initialise Lua
	L = luaL_newstate();
	if ( !L ) return false;
	lua_atpanic(L, l_panicFunc);
	lua_pushlightuserdata(L, this);
	lua_rawseti(L, LUA_REGISTRYINDEX, 0);
	lua_pushcfunction(L, traceback);

#ifdef _WIN32
	lua_pushboolean(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");
#endif

	// Add libraries and APIs
	lua_gc(L, LUA_GCSTOP, 0);
	luaL_openlibs(L);

	// Add the POB "lua/" subdir like InitAPI does, but as absolute paths:
	// this thread races the main thread's PCall SetWorkDir toggling, so
	// CWD-relative entries are unreliable here.
	{
		std::string wd = ui->scriptWorkDir.generic_u8string();
		lua_getglobal(L, "package");
		lua_getfield(L, -1, "path");
		std::string path = lua_tostring(L, -1);
		lua_pop(L, 1);
		path += ";" + wd + "/lua/?.lua;" + wd + "/lua/?/init.lua";
		lua_pushstring(L, path.c_str());
		lua_setfield(L, -2, "path");

		// Lua C modules (lcurl etc.) ship next to the engine DLL; PREPEND so
		// the POB folder's own bundled copies don't win the cpath search
		// (same reasoning as InitAPI). UpdateCheck require()s lcurl.safe
		// inside this state.
		std::string dllDir = EngineModuleDir().generic_string();
		if (!dllDir.empty()) {
			lua_getfield(L, -1, "cpath");
			std::string cpath = lua_tostring(L, -1);
			lua_pop(L, 1);
			cpath = dllDir + "/?.dll;" + cpath;
			lua_pushstring(L, cpath.c_str());
			lua_setfield(L, -2, "cpath");
		}
		lua_pop(L, 1);
	}
	lua_getglobal(L, "os");
	lua_pushcfunction(L, l_os_exit);
	lua_setfield(L, -2, "exit");
	lua_pop(L, 1);
	parseSubScriptList(L, lua_tostring(ui->L, 2), l_SubScriptFunc);
	parseSubScriptList(L, lua_tostring(ui->L, 3), l_SubScriptSub);
	lua_gc(L, LUA_GCRESTART, -1);

	// Load the script
	int err = luaL_loadstring(L, lua_tostring(ui->L, 1));
	if (err) {
		lua_pushstring(ui->L, lua_tostring(L, -1));
		lua_error(ui->L);
	}

	// Copy arguments and launch script thread
	lua_pushinteger(L, ssPushData(L, ssBuildData(ui->L, 4)));
	ThreadStart();
	running = true;

	return true;
}

void ui_subscript_c::Stop()
{
	if (running) {
		// Set hook to stop script on the next line
		lua_sethook(L, l_hookStop, LUA_MASKLINE, 0);
		while ( !finished && !funcWaiting ) ui->sys->Sleep(0);
	}

	if (funcWaiting) {
		// Script is waiting on function call; discard and wait for script to stop
		ssWipeData(funcCall.data);
		funcCall.data = NULL;
		funcWaiting = false;
		while ( !finished ) ui->sys->Sleep(0);
	}

	// Discard data for any pending sub calls
	while (subCalls) {
		ssCall_s* call = subCalls;
		subCalls = call->next;
		ssWipeData(call->data);
		delete call;
	}
}

void ui_subscript_c::ThreadProc()
{
	int numarg = (int)lua_tointeger(L, -1);
	lua_pop(L, 1);
	if (lua_pcall(L, numarg, LUA_MULTRET, 1)) {
		errorStr = AllocString(lua_tostring(L, -1));
	}
	finished = true;
}

void ui_subscript_c::SubScriptFrame()
{
	bool didFinish = finished;
	if (running) {
		// Check for sub calls
		ssCall_s* itterSubCalls = subCalls;
		subCalls = NULL;
		while (subWriting) ui->sys->Sleep(0);
		while (itterSubCalls) {
			ssCall_s* call = itterSubCalls;
			int extraArgs = ui->PushCallback("OnSubCall");
			if (extraArgs >= 0) {
				// Run the main script
				lua_pushstring(ui->L, call->name);
				int numdat = ssPushData(ui->L, call->data);
				ui->PCall(extraArgs + numdat + 1, 0);
			} else {
				ssWipeData(call->data);
			}
			itterSubCalls = call->next;
			delete call;
		}
		
		if (funcWaiting) {
			// Process function call
			int retStart = lua_gettop(ui->L) + 1;
			bool doRet = false;
			int extraArgs = ui->PushCallback("OnSubCall");
			if (extraArgs >= 0) {
				// Run the main script
				lua_pushstring(ui->L, funcCall.name);
				int numdat = ssPushData(ui->L, funcCall.data);
				ui->PCall(extraArgs + numdat + 1, LUA_MULTRET);
				doRet = true;

				// Validate return value types
				int n = lua_gettop(ui->L);
				for (int i = retStart; i <= n; i++) {
					if ( !(lua_isnil(ui->L, i) || lua_isboolean(ui->L, i) || lua_isnumber(ui->L, i) || lua_isstring(ui->L, i)) ) {
						char* msg = AllocStringLen(128);
						sprintf(msg, "OnSubCall() return %d: only nil, boolean, number and string can be returned to sub script", i - retStart + 1);
						ui->DoError("Runtime error in", msg);
						FreeString(msg);
						doRet = false;
					}
				}
			}
			if (doRet) {
				// Grab return values from main script
				funcCall.data = ssBuildData(ui->L, retStart);
			} else {
				ssWipeData(funcCall.data);
				funcCall.data = NULL;
			}
			funcWaiting = false;
		}
	}
	if (didFinish) {
		running = false;
		finished = false;
		if (errorStr) {
			int extraArgs = ui->PushCallback("OnSubError");
			if (extraArgs >= 0) {
				lua_pushlightuserdata(ui->L, (void*)(uintptr_t)id);
				lua_pushstring(ui->L, errorStr);
				ui->PCall(extraArgs + 2, 0);
			}
			FreeString(errorStr);
			errorStr = nullptr;
		} else {
			int extraArgs = ui->PushCallback("OnSubFinished");
			if (extraArgs >= 0) {
				// Validate return value types
				int n = lua_gettop(L);
				for (int i = 2; i <= n; i++) {
					if ( !(lua_isnil(L, i) || lua_isboolean(L, i) || lua_isnumber(L, i) || lua_isstring(L, i)) ) {
						char* msg = AllocStringLen(128);
						sprintf(msg, "Subscript return %d: only nil, boolean, number and string can be returned from sub script", i - 1);
						ui->DoError("Runtime error in", msg);
						FreeString(msg);
						lua_settop(L, 1);
						break;
					}
				}
				lua_pushlightuserdata(ui->L, (void*)(uintptr_t)id);
				ui->PCall(extraArgs + 1 + ssPushData(ui->L, ssBuildData(L, 2)), 0);
			}
		}
	}
}

bool ui_subscript_c::IsRunning()
{
	return running;
}

size_t ui_subscript_c::GetScriptMemory()
{
	return running? lua_gc(L, LUA_GCCOUNT, 0) : 0;
}
