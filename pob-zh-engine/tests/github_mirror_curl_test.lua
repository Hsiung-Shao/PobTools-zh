-- Actual packaged cURL module; no network requests. Ensures the adapter keeps
-- native userdata for trade/multi code while covering both update entry modes.
local engine = assert(os.getenv("POBTOOLS_TEST_ENGINE")):gsub("\\", "/")
package.path = engine .. "/lua/?.lua;" .. package.path
package.cpath = engine .. "/?.dll;" .. package.cpath
local originalOpen = io.open
-- Exercise the checked-out adapter and policy, using only the native DLL from
-- the supplied runtime. A previously packaged adapter must not mask regressions.
local originalLoadlib, originalDofile = package.loadlib, dofile
package.loadlib = function(path, symbol)
	assert(path == "runtime_lua/../lcurl.dll" and symbol == "luaopen_lcurl_safe")
	return originalLoadlib(engine .. "/lcurl.dll", symbol)
end
dofile = function(path)
	assert(path == "runtime_lua/github_mirror.lua")
	return originalDofile("github_mirror.lua")
end
package.preload["lcurl.safe"] = assert(loadfile("runtime_lua/lcurl/safe.lua"))
local enabled = true
io.open = function(path, mode)
	if path:match("/pob%-zh%.ini$") then
		local f = assert(io.tmpfile())
		f:write("[PobTools]\nGitHubMirror=" .. (enabled and "1" or "0") .. "\nGitHubMirrorStation=2\n")
		f:seek("set", 0)
		return f
	end
	return originalOpen(path, mode)
end
local curl = require("lcurl.safe")
package.loadlib, dofile = originalLoadlib, originalDofile
local raw = curl.easy()
assert(type(raw) == "userdata", "non-updater callers must keep native userdata")
assert(raw:escape("a b") == "a%20b")
raw:close()
local synchronous = assert(loadstring("local e = require('lcurl.safe').easy(); return e", "@UpdateCheck.lua"))
local asynchronous = assert(loadstring("-- Module: Update Check\nlocal e = require('lcurl.safe').easy(); return e"))
for _, make in ipairs({ synchronous, asynchronous }) do
	local wrapped = make()
	assert(type(wrapped) == "table", "updater receives mirror adapter")
	assert(wrapped:escape("a b") == "a%20b", "native methods forwarded")
	assert(wrapped:setopt_url("https://github.com/test/repo") == wrapped)
	assert(wrapped:setopt(curl.OPT_ACCEPT_ENCODING, "") == wrapped)
	wrapped:setopt_writefunction(function() return true end)
	wrapped:close()
end
enabled = false
local disabled = synchronous()
assert(type(disabled) == "userdata", "disabled setting takes effect in existing state")
disabled:close()
io.open = originalOpen
print("PASS: actual cURL adapter, both PoB update entry modes, live disable and native userdata")
