-- poecharm_inject.lua
--
-- Engine-owned runtime patch for POB CJK search & input. The engine
-- (SimpleGraphic) loads this AFTER POB's OnInit, in POB's own Lua state, every
-- launch. POB's own files are never modified, so POB can self-update freely and
-- this re-applies automatically on the next start.
--
-- Mechanism: POB registers classes in `common.classes` via newClass(); every
-- instance uses metatable `__index = class`, so overriding a class method (or
-- wrapping its `_constructor`) affects existing AND future instances. Target
-- classes load lazily, so we also wrap `newClass` to patch them on load.
--
-- Requires the engine-exported global `PobToolsTranslate(english) -> chinese`.

local function log(msg)
	if ConPrintf then ConPrintf("[PobTools] %s", msg) end
end

if type(PobToolsTranslate) ~= "function" or type(common) ~= "table" or type(common.classes) ~= "table" then
	log("inject skipped (PobToolsTranslate / common.classes unavailable)")
	return
end

local t_insert = table.insert

-- bytes 0x80-0xFF = any UTF-8 lead/continuation byte (i.e. non-ASCII / CJK)
local function hasCJK(s)
	return s ~= nil and s:find("[\128-\255]") ~= nil
end

-- ===== per-class patchers =====
local PATCHES = {}

-- Skill gem dropdown: allow CJK input + match gems by their translated name.
PATCHES["GemSelectControl"] = function(class)
	-- (a) input: the gem box hardcodes an ASCII-only filter ("^ %a':-"), which
	--     strips CJK on insert. Widen it after construction (main.unicode is on
	--     because the engine set _G.utf8).
	local origCtor = class._constructor
	if origCtor then
		class._constructor = function(self, ...)
			origCtor(self, ...)
			if main and main.unicode then
				self.filter = "%c"
				self.filterPattern = "[" .. self.filter .. "]"
			end
		end
	end
	-- (b) search: after POB's normal (English) build, append gems whose
	--     translated name contains the CJK query.
	local origBuild = class.BuildList
	if origBuild then
		class.BuildList = function(self, buf)
			origBuild(self, buf)
			if not hasCJK(buf) then return end
			local q = buf:lower()
			-- POB leaves a single "" placeholder when the English search found nothing
			if self.noMatches or (self.list[1] == "" and #self.list == 1) then
				self.list = {}
			end
			local seen = {}
			for _, id in ipairs(self.list) do seen[id] = true end
			local matchList = {}
			for gemId, gemData in pairs(self.gems) do
				if not seen[gemId] and gemData.name and self:FilterSupport(gemId, gemData) then
					local ch = PobToolsTranslate(gemData.name)
					if ch and ch:lower():find(q, 1, true) then
						t_insert(matchList, gemId)
						seen[gemId] = true
					end
				end
			end
			self:SortGemList(matchList)
			for _, gemId in ipairs(matchList) do
				t_insert(self.list, gemId)
			end
			if self.list[1] then
				self.noMatches = false
			else
				self.list[1] = ""
				self.noMatches = true
			end
		end
	end
end

-- Passive tree search: match the CJK query against the translated node name and
-- stat lines. `node` is the LAST arg in both PoE1 (self, node) and
-- PoE2 (self, build, node).
PATCHES["PassiveTreeView"] = function(class)
	local orig = class.DoesNodeMatchSearchParams
	if orig then
		class.DoesNodeMatchSearchParams = function(self, ...)
			local n = select("#", ...)
			local node = n > 0 and select(n, ...) or nil
			local s = self.searchStr
			if node and hasCJK(s) then
				local q = s:lower()
				if node.dn then
					local dn = PobToolsTranslate(node.dn)
					if dn and dn:lower():find(q, 1, true) then return true end
				end
				if node.sd then
					for _, line in ipairs(node.sd) do
						local t = PobToolsTranslate(line)
						if t and t:lower():find(q, 1, true) then return true end
					end
				end
				return false
			end
			return orig(self, ...)
		end
	end
end

-- ===== apply now (loaded classes) + on future load (wrap LoadModule) =====
-- NOTE: a class file runs `newClass(...)` FIRST and defines its methods AFTER,
-- so methods only exist once the whole file has finished loading. POB loads
-- class files lazily through the global LoadModule (getClass -> LoadModule).
-- We therefore patch right AFTER each LoadModule returns, not at newClass time.
local applied = {}
local function applyPatch(name, class)
	if applied[name] or not class then return end
	applied[name] = true
	local ok, err = pcall(PATCHES[name], class)
	if ok then log("patched " .. name) else log("patch FAILED " .. name .. ": " .. tostring(err)) end
end

local function tryApplyAll()
	for name in pairs(PATCHES) do
		if not applied[name] and common.classes[name] then
			applyPatch(name, common.classes[name])
		end
	end
end

tryApplyAll() -- classes already loaded at injection time

if type(LoadModule) == "function" then
	local origLoadModule = LoadModule
	LoadModule = function(...)
		-- class files return at most a couple of values; preserve them
		local a, b, c, d = origLoadModule(...)
		tryApplyAll()
		return a, b, c, d
	end
end

log("CJK inject ready")
