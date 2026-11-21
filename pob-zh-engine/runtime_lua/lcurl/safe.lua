-- Lua's file loader runs before its C loader. This delegates to the bundled
-- native safe cURL module and applies mirrors only in PoB's update scripts.
-- It covers synchronous first-run checks as well as background checks, in
-- both interfaces and games, without changing any upstream PoB file.
local source = debug.getinfo(1, "S").source
local luaDir = assert(source:match("^@(.*[/\\])lcurl[/\\]safe%.lua$"))
local engineDir = luaDir .. "../"
local open, err = package.loadlib(engineDir .. "lcurl.dll", "luaopen_lcurl_safe")
assert(open, err)
local native = open()
local mirror = dofile(luaDir .. "github_mirror.lua")
local active, lastEnabled, lastStation
local module = setmetatable({}, { __index = native })
function module.easy(...)
	local caller = debug.getinfo(2, "S").source or ""
	-- Return the original userdata everywhere else: trade code can pass it
	-- straight to native cURL multi/share handles.
	local updater = caller:match("[/\\]UpdateCheck%.lua$") or caller == "@UpdateCheck.lua"
	                or caller:find("Module: Update Check", 1, true)
	if not updater then return native.easy(...) end
	local root = os.getenv("POB_ZH_FONTDIR") or (engineDir .. "../")
	local enabled, station = mirror.settings(root .. "/pob-zh.ini")
	if not enabled then return native.easy(...) end
	if not active or enabled ~= lastEnabled or station ~= lastStation then
		active = mirror.wrapRequire(enabled, station, function() return native end)("lcurl.safe")
		lastEnabled, lastStation = enabled, station
	end
	return active.easy(...)
end
return module
