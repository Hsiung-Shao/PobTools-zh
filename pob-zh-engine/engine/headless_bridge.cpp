#include "headless_bridge.h"

#include "host/error_log.h"
#include "host/hang_watch.h"
#include "ui_local.h"

#include "engine/headless_ipc.h"
#include "engine/texture_atlas.h"

#include <json.hpp> // nlohmann::json (deps/nlohmann)

#include <cmath>
#include <cstring>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using json = nlohmann::json;

namespace {

ui_main_c* GetUI(lua_State* L)
{
	lua_geti(L, LUA_REGISTRYINDEX, ui_main_c::REGISTRY_KEY);
	ui_main_c* ui = (ui_main_c*)lua_touserdata(L, -1);
	lua_pop(L, 1);
	return ui;
}

// ---------------------------------------------------------------------------
// JSON <-> Lua
// ---------------------------------------------------------------------------

void PushJson(lua_State* L, const json& j)
{
	switch (j.type()) {
	case json::value_t::null: lua_pushnil(L); break;
	case json::value_t::boolean: lua_pushboolean(L, j.get<bool>()); break;
	case json::value_t::number_integer: lua_pushnumber(L, (lua_Number)j.get<long long>()); break;
	case json::value_t::number_unsigned: lua_pushnumber(L, (lua_Number)j.get<unsigned long long>()); break;
	case json::value_t::number_float: lua_pushnumber(L, j.get<double>()); break;
	case json::value_t::string: {
		const std::string& s = j.get_ref<const std::string&>();
		lua_pushlstring(L, s.data(), s.size());
		break;
	}
	case json::value_t::array: {
		lua_createtable(L, (int)j.size(), 0);
		int i = 1;
		for (const auto& v : j) {
			PushJson(L, v);
			lua_rawseti(L, -2, i++);
		}
		break;
	}
	case json::value_t::object: {
		lua_createtable(L, 0, (int)j.size());
		for (auto it = j.begin(); it != j.end(); ++it) {
			lua_pushlstring(L, it.key().data(), it.key().size());
			PushJson(L, it.value());
			lua_rawset(L, -3);
		}
		break;
	}
	default: lua_pushnil(L); break;
	}
}

// A Lua table is an array when its keys are exactly 1..n. An empty table is
// an empty array unless the bridge tagged it with __object = true; JSON has to
// pick one and "[]" is what a list-shaped field looks like when nothing is in
// it, which is by far the more common case in this bridge.
bool IsArray(lua_State* L, int idx, size_t& n)
{
	n = lua_objlen(L, idx);
	size_t count = 0;
	lua_pushnil(L);
	while (lua_next(L, idx)) {
		lua_pop(L, 1);
		count++;
		if (lua_type(L, -1) != LUA_TNUMBER) {
			lua_pop(L, 1);
			return false;
		}
		lua_Number k = lua_tonumber(L, -1);
		if (k != std::floor(k) || k < 1 || (size_t)k > n) {
			lua_pop(L, 1);
			return false;
		}
	}
	return count == n;
}

json ToJson(lua_State* L, int idx, int depth)
{
	idx = lua_absindex(L, idx);
	switch (lua_type(L, idx)) {
	case LUA_TNIL: return nullptr;
	case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
	case LUA_TNUMBER: {
		lua_Number v = lua_tonumber(L, idx);
		if (std::isnan(v) || std::isinf(v)) return nullptr; // JSON has no spelling for these
		if (v == std::floor(v) && std::fabs(v) < 9007199254740992.0) return (long long)v;
		return v;
	}
	case LUA_TSTRING: {
		size_t len = 0;
		const char* s = lua_tolstring(L, idx, &len);
		return std::string(s, len);
	}
	case LUA_TTABLE: {
		if (depth > 32) return "<too deep>"; // a cycle, or POB's tree graph by accident
		size_t n = 0;
		bool obj = false;
		if (lua_getmetatable(L, idx)) {
			lua_getfield(L, -1, "__object");
			obj = lua_toboolean(L, -1) != 0;
			lua_pop(L, 2);
		}
		if (!obj && IsArray(L, idx, n)) {
			json arr = json::array();
			for (size_t i = 1; i <= n; i++) {
				lua_rawgeti(L, idx, (int)i);
				arr.push_back(ToJson(L, -1, depth + 1));
				lua_pop(L, 1);
			}
			return arr;
		}
		json o = json::object();
		lua_pushnil(L);
		while (lua_next(L, idx)) {
			std::string key;
			if (lua_type(L, -2) == LUA_TSTRING) {
				size_t len = 0;
				const char* s = lua_tolstring(L, -2, &len);
				key.assign(s, len);
			} else if (lua_type(L, -2) == LUA_TNUMBER) {
				char buf[64];
				snprintf(buf, sizeof(buf), "%.14g", lua_tonumber(L, -2));
				key = buf;
			} else {
				lua_pop(L, 1);
				continue; // function / userdata keys have no JSON name
			}
			if (lua_type(L, -1) == LUA_TFUNCTION || lua_type(L, -1) == LUA_TUSERDATA ||
			    lua_type(L, -1) == LUA_TTHREAD) {
				lua_pop(L, 1);
				continue;
			}
			o[key] = ToJson(L, -1, depth + 1);
			lua_pop(L, 1);
		}
		return o;
	}
	default: return nullptr;
	}
}

// ---------------------------------------------------------------------------
// Globals the bridge script uses
// ---------------------------------------------------------------------------

// PobToolsBridgeSetDispatcher(fn): fn(method, params) -> result | error()
static int l_PobToolsBridgeSetDispatcher(lua_State* L)
{
	ui_main_c* ui = GetUI(L);
	luaL_checktype(L, 1, LUA_TFUNCTION);
	if (ui->bridgeDispatchRef != LUA_NOREF) {
		luaL_unref(L, LUA_REGISTRYINDEX, ui->bridgeDispatchRef);
	}
	lua_pushvalue(L, 1);
	ui->bridgeDispatchRef = luaL_ref(L, LUA_REGISTRYINDEX);
	return 0;
}

// PobToolsBridgeEmit(event, table|nil)
static int l_PobToolsBridgeEmit(lua_State* L)
{
	const char* ev = luaL_checkstring(L, 1);
	std::string data = "null";
	if (lua_gettop(L) >= 2 && !lua_isnil(L, 2)) {
		data = ToJson(L, 2, 0).dump();
	}
	HeadlessIpc::SendEvent(ev, data);
	return 0;
}

// PobToolsHeadless() -> true. Lets poecharm_inject.lua / bridge.lua ask.
static int l_PobToolsHeadless(lua_State* L)
{
	ui_main_c* ui = GetUI(L);
	lua_pushboolean(L, ui && ui->headless);
	return 1;
}

// DrawStringWidth(height, font, text) with no font loaded: a proportional
// estimate. ASCII glyphs in POB's VAR font average ~0.55 of the height, CJK
// ideographs are square. Colour escapes take no space. This keeps POB's own
// wrapping and truncation roughly honest; redux hard-codes 1 and every long
// line wraps after one character.
static int l_DrawStringWidthHeadless(lua_State* L)
{
	lua_Number height = luaL_checknumber(L, 1);
	const char* text = luaL_checkstring(L, 3);
	double w = 0;
	const unsigned char* p = (const unsigned char*)text;
	while (*p) {
		if (*p == '^') {
			int esc = IsColorEscape((const char*)p);
			if (esc) { p += esc; continue; }
		}
		if (*p < 0x80) { w += 0.55; p++; }
		else if ((*p & 0xE0) == 0xC0) { w += 0.6; p += 2; }
		else if ((*p & 0xF0) == 0xE0) { w += 1.0; p += 3; }
		else if ((*p & 0xF8) == 0xF0) { w += 1.0; p += 4; }
		else { p++; }
	}
	lua_pushnumber(L, w * height);
	return 1;
}

} // namespace

namespace HeadlessBridge {

void InstallStubs(lua_State* L, ui_main_c* ui)
{
	int w = 1920, h = 1080;
#ifdef _WIN32
	char buf[16] = {};
	if (GetEnvironmentVariableA("POB_ZH_HEADLESS_W", buf, sizeof(buf))) w = atoi(buf) > 0 ? atoi(buf) : w;
	buf[0] = 0;
	if (GetEnvironmentVariableA("POB_ZH_HEADLESS_H", buf, sizeof(buf))) h = atoi(buf) > 0 ? atoi(buf) : h;
#endif
	lua_pushcfunction(L, l_PobToolsBridgeSetDispatcher);
	lua_setglobal(L, "PobToolsBridgeSetDispatcher");
	lua_pushcfunction(L, l_PobToolsBridgeEmit);
	lua_setglobal(L, "PobToolsBridgeEmit");
	lua_pushcfunction(L, l_PobToolsHeadless);
	lua_setglobal(L, "PobToolsHeadless");
	lua_pushcfunction(L, l_DrawStringWidthHeadless);
	lua_setglobal(L, "DrawStringWidth");
	// PoE2's tree art is DDS arrays; the page draws PNG (texture_atlas.cpp).
	TextureAtlas::Register(L);

	// The rest are plain enough to state in Lua. Every one of these asserts on
	// ui->renderer in ui_api.cpp, and RenderInit is what would have created it.
	const char* stubs =
		"local W, H = ...\n"
		"local function noop() end\n"
		"RenderInit = noop\n"
		"SetClearColor = noop\n"
		"SetDrawLayer = noop\n"
		"SetViewport = noop\n"
		"SetBlendMode = noop\n"
		"SetDrawColor = noop\n"
		"DrawImage = noop\n"
		"DrawImageQuad = noop\n"
		"DrawString = noop\n"
		"SetDPIScaleOverridePercent = noop\n"
		"TakeScreenshot = noop\n"
		"SetCursorPos = noop\n"
		"ShowCursor = noop\n"
		"SetProfiling = noop\n"
		"SetForeground = noop\n"
		"GetScreenSize = function() return W, H end\n"
		"GetScreenScale = function() return 1 end\n"
		"GetDrawLayer = function() return 0, 0 end\n"
		"GetDrawColor = function() return 1, 1, 1, 1 end\n"
		"GetDPIScaleOverridePercent = function() return 0 end\n"
		"GetAsyncCount = function() return 0 end\n"
		"GetCursorPos = function() return 0, 0 end\n"
		"IsKeyDown = function() return false end\n"
		"DrawStringCursorIndex = function() return 1 end\n";
	if (luaL_loadstring(L, stubs) == 0) {
		lua_pushinteger(L, w);
		lua_pushinteger(L, h);
		if (lua_pcall(L, 2, 0, 0) != 0) {
			PobLog::Error("headless", std::string("stub install failed: ") + lua_tostring(L, -1));
			lua_pop(L, 1);
		}
	} else {
		PobLog::Error("headless", std::string("stub chunk did not compile: ") + lua_tostring(L, -1));
		lua_pop(L, 1);
	}
}

void DispatchPending(ui_main_c* ui)
{
	lua_State* L = ui->L;
	std::string line;
	int handled = 0;
	// POB addresses its own files relative to its folder ("TreeData/3_29/…");
	// ui_main's PCall sets that working directory for OnFrame and resets it
	// after, so a bridge call landing between frames would run in the engine's
	// directory and see no tree at all.
	ui->sys->SetWorkDir(ui->scriptWorkDir);
	// Bounded per frame so a flood of requests cannot starve OnFrame (and with
	// it POB's own update check) forever.
	while (handled < 64 && HeadlessIpc::Pop(line)) {
		handled++;
		json req;
		try {
			req = json::parse(line);
		} catch (const std::exception& e) {
			HeadlessIpc::SendError(-1, "bad_json", e.what());
			continue;
		}
		long long id = req.value("id", -1LL);
		std::string method = req.value("method", "");
		if (method.empty()) {
			HeadlessIpc::SendError(id, "bad_request", "missing method");
			continue;
		}
		if (ui->bridgeDispatchRef == LUA_NOREF) {
			HeadlessIpc::SendError(id, "no_bridge", "bridge.lua did not register a dispatcher");
			continue;
		}
		int top = lua_gettop(L);
		lua_rawgeti(L, LUA_REGISTRYINDEX, ui->bridgeDispatchRef);
		lua_pushlstring(L, method.data(), method.size());
		if (req.contains("params")) PushJson(L, req["params"]); else lua_newtable(L);
		{
			HangWatch::Scope watch("lua:bridge-dispatch");
			// Same traceback handler PCall uses (registry "traceback", stack slot 1
			// is only valid inside ui_main's own pcall); errors come back as text.
			lua_getfield(L, LUA_REGISTRYINDEX, "traceback");
			lua_insert(L, top + 1);
			ui->inLua = true;
			int err = lua_pcall(L, 2, 1, top + 1);
			ui->inLua = false;
			if (err) {
				size_t len = 0;
				const char* msg = lua_tolstring(L, -1, &len);
				HeadlessIpc::SendError(id, "lua_error", msg ? std::string(msg, len) : "(no message)");
			} else {
				HeadlessIpc::SendResult(id, ToJson(L, -1, 0).dump());
			}
		}
		lua_settop(L, top);
	}
}

} // namespace HeadlessBridge
