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

-- ===== source marking =====
-- The dictionary is one flat merged map, so a bare word resolves to whichever
-- file defined it LAST. POB stores support gems under their short name
-- ("Volatility", not "Volatility Support") and passives.json redefines that
-- same key as the passive node, so the skill tab drew 易爆 instead of 易變輔助
-- (issue #3). Telling the engine which POB data file the strings came from lets
-- gems.json win there while the passive tree keeps its own wording.
--
-- pcall is not optional: an error escaping the wrapped Draw would leave every
-- later lookup stuck in gem context for the rest of the session.
-- (Lua 5.1: no table.pack, so return values are forwarded positionally. Draw
-- methods return nothing today; three slots is slack, not a known requirement.)
local function withSource(name, fn, self, ...)
	if type(PobToolsSetSource) ~= "function" then return fn(self, ...) end
	local prev = PobToolsSetSource(name)
	local ok, a, b, c = pcall(fn, self, ...)
	PobToolsSetSource(prev)
	if not ok then error(a, 0) end
	return a, b, c
end

local function wrapDrawWithSource(class, name)
	local orig = class.Draw
	if not orig then return false end
	class.Draw = function(self, ...)
		return withSource(name, orig, self, ...)
	end
	return true
end

-- ===== per-class patchers =====
local PATCHES = {}

-- Socket group list: its row labels are built from grantedEffect.name, i.e.
-- straight out of Gems.lua as well.
PATCHES["SkillListControl"] = function(class)
	if not wrapDrawWithSource(class, "gems") then
		error("SkillListControl has no Draw to wrap")
	end
end

-- Skill gem dropdown: allow CJK input + match gems by their translated name.
PATCHES["GemSelectControl"] = function(class)
	-- (0) every string this control draws is a gem name: the edit box shows the
	--     gem's nameSpec and the dropdown rows show gemData.name, both read from
	--     Gems.lua. Mark the source so gems.json wins over the merged map.
	if not wrapDrawWithSource(class, "gems") then
		error("GemSelectControl has no Draw to wrap")
	end
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

-- Anoint popup (NotableDBControl): match the CJK query against the translated
-- notable name and stat lines. Non-search eligibility (anointable recipe,
-- PoE2 emotion checkboxes) is delegated to the original by probing it with an
-- empty query, so upstream filter changes keep working untouched.
PATCHES["NotableDBControl"] = function(class)
	local orig = class.DoesNotableMatchFilters
	if orig then
		class.DoesNotableMatchFilters = function(self, node)
			local search = self.controls and self.controls.search
			local buf = search and search.buf
			if not hasCJK(buf) then
				return orig(self, node)
			end
			search.buf = ""
			local ok, eligible = pcall(orig, self, node)
			search.buf = buf
			if not ok or not eligible then return false end
			local q = buf:lower()
			local mode = self.controls.searchMode and self.controls.searchMode.selIndex or 1
			if (mode == 1 or mode == 2) and node.dn then
				local dn = PobToolsTranslate(node.dn)
				if dn and dn:lower():find(q, 1, true) then return true end
			end
			if (mode == 1 or mode == 3) and node.sd then
				for _, line in ipairs(node.sd) do
					local t = PobToolsTranslate(line)
					if t and t:lower():find(q, 1, true) then return true end
				end
			end
			return false
		end
	end
end

-- Dropdown typed search (SearchHost mixin; e.g. the enchant popup lists):
-- after POB's English word matching, also accept rows whose translated label
-- contains the CJK query. Highlight ranges are byte offsets into the English
-- label, so translation hits clear them (row is shown, just not highlighted).
PATCHES["SearchHost"] = function(class)
	local orig = class.UpdateSearch
	if orig then
		class.UpdateSearch = function(self)
			orig(self)
			if not hasCJK(self.searchTerm) then return end
			local list = self.searchListAccessor and self.searchListAccessor()
			if not list then return end
			local q = self.searchTerm:lower()
			local changed = false
			for idx, entry in ipairs(list) do
				local info = self.searchInfos[idx]
				if info and not info.matches then
					local value = self.valueAccessor and self.valueAccessor(entry) or entry
					local zh = type(value) == "string" and PobToolsTranslate(value) or nil
					if zh and zh:lower():find(q, 1, true) then
						info.matches = true
						info.ranges = {}
						changed = true
					end
				end
			end
			if changed then self:UpdateMatchCount() end
		end
	end
end

-- Tooltips: translate a line BEFORE POB wraps it.
--
-- TooltipClass:AddLine splits on "\n" and immediately calls main:WrapString,
-- storing each wrapped fragment as its own line (Tooltip.lua:95-107); Draw then
-- issues one DrawString per fragment (Tooltip.lua:597). The engine's DrawString
-- hook therefore only ever sees POST-WRAP pieces, and a piece like
-- "...have a 25% chance to Explode, dealing a tenth of their maximum" is in no
-- dictionary -- so EVERY tooltip line long enough to wrap stayed English, no
-- matter how complete the dictionary was. Translating here fixes the whole
-- class of them, and the wrap then happens at Chinese widths, which is also the
-- correct place to measure.
--
-- PobToolsTranslateDisplay, not PobToolsTranslate: POB colours text with inline
-- "^x7070FF" escapes, and the bare lookup strips them to match and hands back
-- uncoloured text -- which silently repainted every translated tooltip line in
-- the default colour. The Display variant is the exact function DrawString uses.
--
-- Double translation is not a concern: the fragments DrawString later sees are
-- Chinese, and Chinese matches no English key.
PATCHES["Tooltip"] = function(class)
	local orig = class.AddLine
	if not orig then error("Tooltip has no AddLine to wrap") end
	if type(PobToolsTranslateDisplay) ~= "function" then
		error("PobToolsTranslateDisplay unavailable (engine too old)")
	end
	class.AddLine = function(self, size, text, ...)
		-- Tooltip.lua:96 starts a new BLOCK when a line begins with "Equipping"
		-- or "Removing", so translating those would silently change the
		-- tooltip's block structure. Left alone on purpose.
		local structural = type(text) == "string" and
			(text:find("Equipping", 1, true) or text:find("Removing", 1, true))
		if type(text) == "string" and text ~= "" and not structural and not hasCJK(text) then
			-- whole blob first: GGG ships some stat lines WITH their own "\n"
			-- and the dictionary stores those joined, so the multi-line form is
			-- a real key. Falling back per line covers everything else.
			local whole = PobToolsTranslateDisplay(text)
			if whole then
				text = whole
			elseif text:find("\n", 1, true) then
				local parts, changed = {}, false
				for line in (text .. "\n"):gmatch("([^\n]*)\n") do
					local zh = line ~= "" and PobToolsTranslateDisplay(line) or nil
					if zh then changed = true end
					t_insert(parts, zh or line)
				end
				if changed then text = table.concat(parts, "\n") end
			end
		end
		return orig(self, size, text, ...)
	end

	-- F2 has to reach tooltips that are ALREADY on screen.
	--
	-- The lines above are translated when the tooltip is BUILT, and POB rebuilds
	-- a tooltip only when CheckForUpdate sees one of the caller's values change
	-- (Tooltip.lua:67). The item slots pass (item, devModeAlt, outputRevision,
	-- SHIFT) -- F2 changes none of them, so the tooltip under the cursor kept the
	-- language it was built in and only turned English once you hovered something
	-- else. Before the AddLine hook existed, translation happened per-frame inside
	-- DrawString, which is why F2 used to look instant.
	--
	-- POB has the same problem with its own main.notSupportedModTooltips switch
	-- and fixes it inside CheckForUpdate (Tooltip.lua:80); this is that fix for
	-- our switch, kept in the same place and the same shape. The state is stored
	-- in updateParams so that Clear(true) wipes it along with everything else --
	-- an emptied tooltip must rebuild anyway.
	local origCheck = class.CheckForUpdate
	if origCheck and type(PobToolsGetTranslate) == "function" then
		class.CheckForUpdate = function(self, ...)
			-- Always call through first: CheckForUpdate is what RECORDS the
			-- params, so short-circuiting it would make the next real change
			-- go unnoticed. It also creates updateParams.
			local doUpdate = origCheck(self, ...)
			local cur = PobToolsGetTranslate()
			local params = self.updateParams
			if params and params.pobToolsTranslate ~= cur then
				params.pobToolsTranslate = cur
				if not doUpdate then
					self:Clear()   -- keeps updateParams, exactly like line 82
					return true
				end
			end
			return doUpdate
		end
	else
		-- Not fatal: the tooltips still translate, F2 just will not refresh one
		-- that is already open. Worth a line in the log rather than killing the
		-- whole patch over it.
		log("Tooltip: CheckForUpdate/PobToolsGetTranslate missing, F2 will not refresh open tooltips")
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
