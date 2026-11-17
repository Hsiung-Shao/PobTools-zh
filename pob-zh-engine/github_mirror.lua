-- Shared policy for PoE1/PoE2's UpdateCheck script. Loaded by the engine-owned
-- lcurl.safe adapter; upstream PoB integrity checks still validate the result.
local M = {}
local sites = { "https://gh-proxy.com/", "https://gh-proxy.org/" }
local hosts = {
	["github.com"] = true, ["api.github.com"] = true,
	["raw.githubusercontent.com"] = true, ["codeload.github.com"] = true,
	["objects.githubusercontent.com"] = true, ["release-assets.githubusercontent.com"] = true,
}

function M.settings(path)
	local file = io.open(path, "rb")
	if not file then return false, 0 end
	local text = file:read("*a")
	file:close()
	-- WritePrivateProfileString may preserve an existing UTF-16LE INI. Keys and
	-- the two numeric values are ASCII, so decode just the ASCII code units.
	if text:sub(1, 2) == "\255\254" then
		text = text:sub(3):gsub("(.)%z", "%1")
	end
	text = text:gsub("^\239\187\191", "")
	local section, enabled, station = "", false, 0
	for line in (text .. "\n"):gmatch("([^\r\n]*)[\r\n]+") do
		local name = line:match("^%s*%[([^%]]+)%]")
		if name then section = name:lower() end
		if section == "pobtools" then
			local key, value = line:match("^%s*([%w]+)%s*=%s*([+-]?%d+)%s*$")
			if key and key:lower() == "githubmirror" then enabled = tonumber(value) ~= 0 end
			if key and key:lower() == "githubmirrorstation" then station = tonumber(value) end
		end
	end
	if station ~= 1 and station ~= 2 then station = 0 end
	return enabled, station
end

function M.wrapRequire(enabled, station, originalRequire)
	if not enabled then return originalRequire end
	local preferred = station == 2 and 2 or 1
	return function(name)
		local curl = originalRequire(name)
		if name ~= "lcurl.safe" then return curl end
		local proxy = setmetatable({}, { __index = curl })
		function proxy.easy(...)
			local easy = curl.easy(...)
			if not easy then return easy end
			local url, writer, hasHeaders
			local wrapped = {}
			function wrapped:setopt_url(value)
				url = value
				easy:setopt_url(value)
				return self
			end
			function wrapped:setopt_writefunction(value)
				writer = value
				easy:setopt_writefunction(value)
				return self
			end
			function wrapped:setopt(option, value, ...)
				if option == curl.OPT_URL then return self:setopt_url(value) end
				if option == curl.OPT_WRITEFUNCTION then return self:setopt_writefunction(value) end
				if option == curl.OPT_HTTPHEADER then hasHeaders = true end
				easy:setopt(option, value, ...)
				return self
			end
			function wrapped:perform()
				local host = type(url) == "string" and url:match("^https://([^/]+)/")
				-- Only anonymous GETs issued by the updater. Never proxy trade/API
				-- credentials or loosely matched GitHub-looking host names.
				if not host or not hosts[host:lower()] or hasHeaders or not writer then return easy:perform() end
				local first = station == 1 and 1 or station == 2 and 2 or preferred
				local routes = { sites[first] .. url, sites[3 - first] .. url, url }
				local lastError
				for index, route in ipairs(routes) do
					local spool = io.tmpfile()
					if not spool then error("Unable to create GitHub update download buffer") end
					easy:setopt_url(route)
					easy:setopt(curl.OPT_FOLLOWLOCATION, 1)
					easy:setopt(curl.OPT_FAILONERROR, 1)
					easy:setopt(curl.OPT_CONNECTTIMEOUT, 5)
					easy:setopt(curl.OPT_TIMEOUT, 300)
					easy:setopt_writefunction(function(data)
						return spool:write(data) ~= nil
					end)
					local result, err = easy:perform()
					local size = spool:seek("end")
					spool:seek("set", 0)
					local head = (spool:read(256) or ""):lower():gsub("^%s+", "")
					local valid = result and not err and size > 0 and not head:match("^<html") and not head:match("^<!doctype")
					if valid and url:match("/manifest%.xml$") then
						valid = head:match("^<%?xml") ~= nil or head:match("^<pobversion") ~= nil
					end
					if valid then
						-- Commit only a complete successful attempt. Retried file and
						-- text downloads cannot retain partial bytes from a failed site.
						spool:seek("set", 0)
						while true do
							local chunk = spool:read(65536)
							if not chunk then break end
							local ok = type(writer) == "function" and writer(chunk)
							if type(writer) ~= "function" then ok = writer:write(chunk) end
							if not ok then spool:close(); error("Unable to write GitHub update download") end
						end
						spool:close()
						if index < 3 then preferred = index == 1 and first or 3 - first end
						return result
					end
					spool:close()
					lastError = err or { msg = function() return "Invalid GitHub update response" end }
				end
				return nil, lastError
			end
			return setmetatable(wrapped, { __index = function(_, key)
				local value = easy[key]
				if type(value) == "function" then return function(_, ...) return value(easy, ...) end end
				return value
			end })
		end
		return proxy
	end
end

return M
