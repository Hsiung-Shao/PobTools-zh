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

-- POB_ZH_INJECT_TRACE=1 also appends every log line to poecharm_inject_trace.txt
-- next to this file. The engine console that ConPrintf feeds is hidden in a
-- normal run, so without this a field report has nothing to quote.
local traceFile
do
	local v = os and os.getenv and os.getenv("POB_ZH_INJECT_TRACE")
	if v and v ~= "" and v ~= "0" then
		local src = debug and debug.getinfo and debug.getinfo(1, "S").source or ""
		local dir = src:match("^@(.*[/\\])") or ""
		traceFile = io.open(dir .. "poecharm_inject_trace.txt", "ab")
	end
end
local function log(msg)
	if ConPrintf then ConPrintf("[PobTools] %s", msg) end
	if traceFile then
		traceFile:write(msg, "\n")
		traceFile:flush()
	end
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

-- What a search has to match is what the user SEES, and POB's inline "^7" /
-- "^xRRGGBB" runs are colour commands, not glyphs. Dictionary values carry them
-- too -- "Is the enemy Shocked?" comes back as
-- "^xFFFFFF敵人是否被^xADAA47【感電】^7?" -- so a query typed straight off the
-- screen ("敵人是否被感電") would fall in the gap between two coloured runs and
-- find nothing. StripEscapes is POB's own function for this; it is absent only
-- if POB moved it, and then the raw value is still better than no value.
local function visible(text)
	if text == nil then return nil end
	if type(StripEscapes) == "function" then return StripEscapes(text) end
	return text
end

-- The two lookups, normalised to visible text. `zhDisplay` is the one that
-- reproduces what DrawString drew; `zhRaw` is the bare dictionary hit, used
-- where the source has been marked (gems) or where the caller wants a name
-- rather than a rendered line.
local function zhDisplay(text)
	if type(PobToolsTranslateDisplay) ~= "function" then return nil end
	return visible(PobToolsTranslateDisplay(text))
end

local function zhRaw(text)
	return visible(PobToolsTranslate(text))
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

-- ===== constructor wrapping =====
-- The two POB forks store a class's constructor in DIFFERENT places, and there
-- is nothing at the call site to tell them apart:
--
--   PoE2 fork      newClass(name, parents..., ctorFunc)  -> class._constructor
--                  and new() calls class._constructor(object, ...)
--   PoE1 community newClass(name, parents...)            -> the ctor is a plain
--                  method NAMED AFTER THE CLASS (class[className], Common.lua:185),
--                  and _constructor does not exist anywhere in that fork.
--
-- The gem box widened its input filter through `class._constructor` alone, so on
-- PoE1 that branch was simply nil and skipped in silence -- Chinese stayed
-- unenterable while the trace still printed "patched GemSelectControl". Hence
-- `after` runs through this helper, which reports whether it found a slot at
-- all so a caller can fail loudly instead.
--
-- Patch time is right after the class file finished loading, i.e. before the
-- first new(), so the PoE1 wrapper installed by new() (Common.lua:186) wraps
-- OURS and its "constructor did not return a value" check still sees the
-- original's return value, which is why `ret` is forwarded untouched.
local function wrapConstructor(class, className, after)
	local key = rawget(class, "_constructor") and "_constructor"
		or (rawget(class, className) and className)
	if not key then return false end
	local orig = class[key]
	class[key] = function(self, ...)
		local ret = orig(self, ...)
		after(self)
		return ret
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
	-- (a) input: the gem box hardcodes an ASCII-only filter ("^ %a':-") and
	--     EditControl:Insert runs text:gsub(filterPattern,"") on every typed
	--     character, so each CJK byte was deleted on the way in -- the box could
	--     never hold a Chinese query and (b) below never had anything to match.
	--     Widen it after construction (main.unicode is on because the engine set
	--     _G.utf8). Fatal when no constructor slot is found: silence here is
	--     exactly how this went unnoticed on PoE1.
	if not wrapConstructor(class, "GemSelectControl", function(self)
		if main and main.unicode then
			self.filter = "%c"
			self.filterPattern = "[" .. self.filter .. "]"
		end
		if traceFile then
			log("GemSelectControl filter=" .. tostring(self.filter))
		end
	end) then
		error("GemSelectControl has no constructor to wrap")
	end
	-- (b) search: after POB's normal (English) build, append gems whose
	--     translated name contains the CJK query.
	--
	--     Under source "gems" for the same reason the Draw wrap above is: the
	--     merged map answers "Volatility" with 易爆 (the passive), while the row
	--     the user is looking at says 易變輔助 out of gems.json. Matching the
	--     unmarked lookup meant typing exactly what the dropdown showed found
	--     nothing.
	local origBuild = class.BuildList
	if origBuild then
		local function appendCJKMatches(self, buf)
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
					local ch = zhRaw(gemData.name)
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
		class.BuildList = function(self, buf)
			origBuild(self, buf)
			if not hasCJK(buf) then return end
			withSource("gems", appendCJKMatches, self, buf)
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
					local dn = zhRaw(node.dn)
					if dn and dn:lower():find(q, 1, true) then return true end
				end
				if node.sd then
					for _, line in ipairs(node.sd) do
						local t = zhRaw(line)
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
				local dn = zhRaw(node.dn)
				if dn and dn:lower():find(q, 1, true) then return true end
			end
			if (mode == 1 or mode == 3) and node.sd then
				for _, line in ipairs(node.sd) do
					local t = zhRaw(line)
					if t and t:lower():find(q, 1, true) then return true end
				end
			end
			return false
		end
	end
end

-- Item database (Uniques / Rare templates): match the CJK query against the
-- translated item name AND the translated modifier lines. Same probe-the-
-- original-with-an-empty-query shape as the notable popup above, because
-- DoesItemMatchFilters mixes eligibility (slot, type, league, obtainable,
-- requirements) with the text search in one boolean.
--
-- The name half used to live in a C++ source patch on itemdbcontrol.lua; it is
-- here now so one function owns the whole rule. The mod-line half never existed,
-- which is why searching a Chinese affix returned nothing while searching a
-- Chinese item name worked.
PATCHES["ItemDBControl"] = function(class)
	if type(PobToolsTranslateDisplay) ~= "function" then
		error("PobToolsTranslateDisplay unavailable (engine too old)")
	end
	local orig = class.DoesItemMatchFilters
	if not orig then error("ItemDBControl has no DoesItemMatchFilters to wrap") end
	-- Every mod-line table the two forks keep. PoE2 added runeModLines and PoE1
	-- has no such field, so a name that is absent is skipped rather than assumed.
	local MOD_LINE_FIELDS = { "enchantModLines", "runeModLines", "implicitModLines", "explicitModLines" }
	class.DoesItemMatchFilters = function(self, item)
		local search = self.controls and self.controls.search
		local buf = search and search.buf
		if not hasCJK(buf) then
			return orig(self, item)
		end
		search.buf = ""
		local ok, eligible = pcall(orig, self, item)
		search.buf = buf
		if not ok or not eligible then return false end
		local q = buf:lower()
		local mode = self.controls.searchMode and self.controls.searchMode.selIndex or 1
		if (mode == 1 or mode == 2) and item.name then
			-- Display first: the list row is drawn as colorCode .. item.name and
			-- goes through the same tr_display, so this is literally the text on
			-- screen. The bare lookup is the fallback for names the display path
			-- returns unchanged.
			local zh = zhDisplay(item.name) or zhRaw(item.name)
			if zh and zh:lower():find(q, 1, true) then return true end
		end
		if mode == 1 or mode == 3 then
			for _, field in ipairs(MOD_LINE_FIELDS) do
				local lines = item[field]
				if lines then
					for _, line in pairs(lines) do
						local t = line.line and zhDisplay(line.line)
						if t and t:lower():find(q, 1, true) then return true end
					end
				end
			end
		end
		return false
	end
end

-- Minion list search (spectres, and the minion pickers): match the CJK query
-- against the translated minion name and its skill names.
--
-- The searchStr the caller passes has already been pattern-escaped, so the raw
-- box is read instead -- a plain find on what the user actually typed.
PATCHES["MinionSearchListControl"] = function(class)
	if type(PobToolsTranslateDisplay) ~= "function" then
		error("PobToolsTranslateDisplay unavailable (engine too old)")
	end
	local orig = class.DoesEntryMatchFilters
	if not orig then error("MinionSearchListControl has no DoesEntryMatchFilters to wrap") end
	class.DoesEntryMatchFilters = function(self, searchStr, minionId, filterMode)
		if orig(self, searchStr, minionId, filterMode) then return true end
		local search = self.controls and self.controls.searchText
		local buf = search and search.buf
		if not hasCJK(buf) then return false end
		local minion = self.data and self.data.minions and self.data.minions[minionId]
		if not minion then return false end
		local q = buf:lower()
		-- dropdown is { "Names", "Skills", "Both" }
		if (filterMode == 1 or filterMode == 3) and minion.name then
			local zh = zhDisplay(minion.name)
			if zh and zh:lower():find(q, 1, true) then return true end
		end
		if (filterMode == 2 or filterMode == 3) and minion.skillList then
			for _, skillId in ipairs(minion.skillList) do
				local skill = self.data.skills and self.data.skills[skillId]
				local zh = skill and skill.name and zhDisplay(skill.name)
				if zh and zh:lower():find(q, 1, true) then return true end
			end
		end
		return false
	end
end

-- Calcs tab search box: it highlights sections and rows whose label matches, so
-- the whole feature is one predicate over a label. Additive -- an English match
-- is still an English match, the translated text is just also considered.
PATCHES["CalcsTab"] = function(class)
	if type(PobToolsTranslateDisplay) ~= "function" then
		error("PobToolsTranslateDisplay unavailable (engine too old)")
	end
	local orig = class.SearchMatch
	if not orig then error("CalcsTab has no SearchMatch to wrap") end
	class.SearchMatch = function(self, txt)
		local search = self.controls and self.controls.search
		local buf = search and search.buf
		if type(txt) == "string" and hasCJK(buf) then
			local zh = zhDisplay(txt)
			if zh and zh:lower():find(buf:lower(), 1, true) then return true end
		end
		return orig(self, txt)
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
					local zh = type(value) == "string" and zhRaw(value) or nil
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

-- Edit boxes: keep CJK when PASTING.
-- EditControl:OnKeyDown handles Ctrl+V / right-click by reading Paste() and
-- then running text:gsub("[\128-\255]", "?") -- every non-ASCII byte becomes
-- '?', so "我愛POE" pasted into the Notes tab came out as "??????POE" while
-- TYPING the same text works (OnChar never passes that line). The replacement
-- exists because stock POB cannot draw those bytes; this engine can, and the
-- same box already accepts them when typed.
--
-- The paste branch is re-done here minus that one line. Everything else -- the
-- pasteFilter hook (ItemsTab installs sanitiseText there, so item text still
-- gets POB's own cleanup), selection replacement, the per-box filter inside
-- Insert/ReplaceSel, the length limit -- is still POB's own code, and every
-- other key goes straight to the original.
PATCHES["EditControl"] = function(class)
	local orig = class.OnKeyDown
	if not orig then error("EditControl has no OnKeyDown to wrap") end
	class.OnKeyDown = function(self, key, doubleClick)
		-- same condition as upstream; disableRightClickPaste exists in PoE2 only
		-- (nil in PoE1, so the extra test is harmless there)
		local paste = (key == "v" and IsKeyDown("CTRL"))
			or (key == "RIGHTBUTTON" and self.Object:IsMouseOver() and not self.disableRightClickPaste)
		if not paste then
			return orig(self, key, doubleClick)
		end
		-- same guards as the original, in the same order
		if not self:IsShown() or not self:IsEnabled() then
			return
		end
		local mOverControl = self:GetMouseOverControl()
		if mOverControl and mOverControl.OnKeyDown then
			self.selControl = mOverControl
			return mOverControl:OnKeyDown(key) and self
		else
			self.selControl = nil
		end
		local text = Paste()
		if traceFile then
			log(string.format("EditControl paste key=%s filter=%s pasteFilter=%s bytes=%s", tostring(key),
				tostring(self.filter), tostring(self.pasteFilter ~= nil),
				text and (text:gsub(".", function(c) return string.format("%02X ", c:byte()) end)) or "nil"))
		end
		if text then
			if self.pasteFilter then
				text = self.pasteFilter(text)
			end
			-- upstream: text = text:gsub("[\128-\255]", "?")  -- deliberately dropped
			if self.sel and self.sel ~= self.caret then
				self:ReplaceSel(text)
			else
				self:Insert(text)
			end
		end
		return self
	end
	-- Subclasses (ResizableEditControl, GemSelectControl) inherit through a
	-- CACHING __index (Common.lua newClass): the first lookup copies the parent
	-- method into the subclass table, so a subclass that already resolved
	-- OnKeyDown would keep the old one. Swap those copies too.
	for _, other in pairs(common.classes) do
		if other ~= class and rawget(other, "OnKeyDown") == orig then
			other.OnKeyDown = class.OnKeyDown
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
--
-- ...and only after the OUTERMOST one. newClass("EditControl", ..., "UndoHandler")
-- calls getClass on each parent, and a parent that is not loaded yet goes
-- through LoadModule *from inside EditControl.lua, before any of its methods
-- exist*. Patching when that inner call returned found common.classes.EditControl
-- registered but empty, failed with "has no OnKeyDown to wrap", and the failure
-- was final because the name was already marked applied. A depth counter makes
-- the patch wait until the whole nest has finished loading. (Found at runtime
-- with POB_ZH_INJECT_TRACE; the offline harness had pre-loaded every parent.)
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
	local depth = 0
	LoadModule = function(...)
		depth = depth + 1
		-- class files return at most a couple of values; preserve them.
		-- pcall so an error inside a module cannot leave depth stuck above zero
		-- (which would silence every later patch); the error is rethrown as-is.
		local ok, a, b, c, d = pcall(origLoadModule, ...)
		depth = depth - 1
		if not ok then error(a, 0) end
		if depth == 0 then tryApplyAll() end
		return a, b, c, d
	end
end

log("CJK inject ready")
