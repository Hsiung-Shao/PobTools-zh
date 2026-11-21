-- Run from pob-zh-engine with Lua 5.1/LuaJIT. Offline fake cURL regression.
local mirror = dofile("github_mirror.lua")
local count = 0
local function check(ok, name)
	assert(ok, name)
	count = count + 1
end
local ini = os.tmpname()
local function config(text)
	local f = assert(io.open(ini, "wb")); f:write(text); f:close()
	return mirror.settings(ini)
end
local on, station = config("[PobTools]\nGitHubMirror=1\nGitHubMirrorStation=2\n[Other]\nGitHubMirror=0\n")
check(on and station == 2, "INI selection and section isolation")
on, station = config("[PobTools]\nGitHubMirror=1\nGitHubMirrorStation=999\n")
check(on and station == 0, "unknown station becomes automatic")
on, station = config("\255\254" .. ("[PobTools]\r\nGitHubMirror=1\r\nGitHubMirrorStation=1\r\n"):gsub(".", function(c) return c .. "\0" end))
check(on and station == 1, "UTF-16LE Windows INI")
os.remove(ini)
check(not mirror.settings(ini), "missing settings defaults off")

local curl = { OPT_FOLLOWLOCATION = 1, OPT_FAILONERROR = 2, OPT_CONNECTTIMEOUT = 3,
	OPT_TIMEOUT = 4, OPT_URL = 5, OPT_WRITEFUNCTION = 6, OPT_HTTPHEADER = 7 }
local replies, requests = {}, {}
function curl.easy()
	local easy = {}
	function easy:setopt_url(value) self.url = value; return self end
	function easy:setopt_writefunction(value) self.writer = value; return self end
	function easy:setopt(option, value) self[option] = value; return self end
	function easy:escape(value) return value:gsub(" ", "%%20") end
	function easy:close() self.closed = true end
	function easy:perform()
		requests[#requests + 1] = self.url
		local reply = replies[self.url] or { "complete" }
		if type(self.writer) == "function" then self.writer(reply[1]) else self.writer:write(reply[1]) end
		if reply[2] then return nil, { msg = function() return "fake transfer failed" end } end
		return self
	end
	return easy
end
local function requireFake(name) return name == "lcurl.safe" and curl or "other module" end
check(mirror.wrapRequire(false, 2, requireFake) == requireFake, "disabled wrapper preserves original require")
local function run(selected, url, outputFile, headers)
	requests = {}
	local requireProxy = mirror.wrapRequire(true, selected, requireFake)
	check(requireProxy("xml") == "other module", "other modules unchanged")
	local easy = requireProxy("lcurl.safe").easy()
	easy:setopt_url(url)
	local text = ""
	easy:setopt_writefunction(outputFile or function(data) text = text .. data; return true end)
	if headers then easy:setopt(curl.OPT_HTTPHEADER, headers) end
	local ok, err = easy:perform()
	easy:close()
	return text, ok, err
end
local url = "https://raw.githubusercontent.com/test/repo/master/manifest.xml"
local a, b = "https://gh-proxy.com/" .. url, "https://gh-proxy.org/" .. url
replies = { [a] = { "<PoBVersion>one</PoBVersion>" }, [b] = { "<PoBVersion>two</PoBVersion>" } }
local text, ok = run(2, url)
check(ok and text:find("two") and #requests == 1 and requests[1] == b, "selected mirror handles PoB manifest")
replies[b] = { "partial-corrupt-long-data", true }
text, ok = run(2, url)
check(ok and text == replies[a][1] and #requests == 2, "partial text does not leak into fallback result")
replies[b] = { "<!doctype html><html>mirror error</html>" }
text, ok = run(2, url)
check(ok and text == replies[a][1] and #requests == 2, "HTTP 200 HTML error falls back")
replies[b] = { "not a manifest" }
text, ok = run(2, url)
check(ok and text == replies[a][1], "invalid manifest falls back")
replies[a] = { "also failed", true }; replies[b] = { "partial", true }; replies[url] = { "<PoBVersion>direct</PoBVersion>" }
text, ok = run(1, url)
check(ok and text == replies[url][1] and requests[3] == url, "both mirrors fail then use direct")

local binaryUrl = "https://github.com/test/repo/releases/download/v1/content.zip"
replies = { ["https://gh-proxy.org/" .. binaryUrl] = { "very long partial content", true },
	["https://gh-proxy.com/" .. binaryUrl] = { "PK\0good" } }
local out = assert(io.tmpfile())
text, ok = run(2, binaryUrl, out)
out:seek("set", 0)
check(ok and out:read("*a") == "PK\0good", "file retry cannot leave partial bytes or trailing garbage")
out:close()
for _, other in ipairs({ "https://www.pathofexile.com/api", "https://github.com.evil.test/a", "http://github.com/a" }) do
	replies = {}; text, ok = run(1, other)
	check(ok and #requests == 1 and requests[1] == other, "non-GitHub service stays direct")
end
replies = {}; run(2, binaryUrl, nil, { "Authorization: secret" })
check(#requests == 1 and requests[1] == binaryUrl, "requests carrying headers stay direct")

print("PASS: " .. count .. " GitHub mirror Lua checks")
