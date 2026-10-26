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
	if type(PobToolsLogError) == "function" then
		PobToolsLogError("inject", "翻譯注入整個沒有啟動（引擎函式或 common.classes 不存在）")
	end
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

-- ===== wrappers keep the original's parameter count =====
-- The new UI's bridge (host/data/bridge/bridge.lua) checks POB's upstream shape
-- with debug.getinfo(fn, "u").nparams -- PassiveTreeView.AddNodeTooltip must
-- take 4 or 5, BUILD.Init 6 -- and falls back to the classic window when a
-- probe fails. A wrapper written as function(self, ...) reports nparams 1, so
-- every wrapper here goes through keepArity: impl does the work, the function
-- installed has exactly orig's named parameters (plus "..." when orig is
-- vararg) and hands them on to impl unchanged. Without nparams (stock Lua
-- 5.1) or loadstring there is nothing to keep, and impl is installed as is.
local arityMakers = {}
local function keepArity(orig, impl)
	if type(debug) ~= "table" or type(debug.getinfo) ~= "function" or type(loadstring) ~= "function" then
		return impl
	end
	local ok, info = pcall(debug.getinfo, orig, "u")
	local n = ok and type(info) == "table" and info.nparams
	if type(n) ~= "number" then return impl end
	local key = n .. (info.isvararg and "+" or "")
	local maker = arityMakers[key]
	if not maker then
		local names = {}
		for i = 1, n do names[i] = "a" .. i end
		if info.isvararg then names[#names + 1] = "..." end
		local list = table.concat(names, ", ")
		maker = loadstring("local impl = ... return function(" .. list .. ") return impl(" .. list .. ") end",
			"=PobTools keepArity")
		if not maker then return impl end
		arityMakers[key] = maker
	end
	return maker(impl)
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
	class.Draw = keepArity(orig, function(self, ...)
		return withSource(name, orig, self, ...)
	end)
	return true
end

-- ===== tooltip line sources =====
-- Tooltips are TRANSLATED when they are built (Tooltip:AddLine below). Only
-- NAME lines get a source dictionary; every other line (passive stat lines,
-- requirements, quality, ...) keeps the shipping unmarked lookup. The wraps on
-- PassiveTreeView:AddNodeName and ItemsTab:AddItemTooltip only record WHAT is
-- being drawn (nodeCtx / itemCtx); AddLine then picks per line: the passive's
-- name line -> by node.id, else passives; a unique's title -> uniques.json; the
-- base line -> items.json; a magic item's one-line "prefix base suffix" name is
-- split into affixes / items / affixes. A PoE1 mod line (AddLine's 4th arg is
-- the modLine table) is looked up unmarked first, and under stats.json only
-- when that left it English.
--
-- The item being drawn, set by the AddItemTooltip wrap (nil outside it).
-- { item = item, titleDone = bool }: titleDone keeps a unique whose title IS a
-- base name (Kingsguard) from having its base line read as the title again --
-- the title line always comes first.
local itemCtx = nil
local UNIQUE_RARITY = { UNIQUE = true, RELIC = true }

local function trim(s)
	return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- Leading POB colour commands ("^7", "^xRRGGBB") split off the line.
local function splitLeadColour(text)
	local i = 1
	while true do
		local e = text:match("^%^x%x%x%x%x%x%x()", i) or text:match("^%^%d()", i)
		if not e then break end
		i = e
	end
	return text:sub(1, i - 1), text:sub(i)
end

-- POB draws the base line as item.baseName:gsub(" %(.+%)","").
local function cleanBaseName(item)
	return type(item.baseName) == "string" and (item.baseName:gsub(" %(.+%)", "")) or nil
end

-- ===== lookups by stable id =====
-- Same English, different id, different wording: two passives both called
-- "Elemental Overload" (GraphId) or two mods sharing the affix name "Robust"
-- (mod Id). Where the id is at hand it decides the translation
-- (PobToolsTranslateId); without one, or when nothing comes back, the English
-- path runs exactly as before.
--
-- The node whose name PassiveTreeView:AddNodeName is drawing (nil outside it).
-- { node = node, nameDone = bool }: only its first name line is taken.
local nodeCtx = nil
-- The affix a crafting drop-down / range slider tooltip is describing (nil
-- outside it): { id = modId, mod = mod table }.
local affixTipCtx = nil
-- True while PassiveTreeView:AddNodeTooltip runs: its non-name lines are the
-- passive's stat lines, asked of PobToolsTranslateStat under "passive".
local nodeTipActive = false
-- Consecutive node.sd / reminder / flavour lines that are ONE multi-line GGPK
-- description (POB splits it into several entries): { [stripped English line]
-- = Chinese line, "" for a line left blank } (buildSdPairs), built when
-- AddNodeTooltip / the anoint popup's AddValueTooltip starts, nil outside it.
local nodeSdPairs = nil
-- Group map (see addGroups) for a tooltip whose tooltipFunc knows the lines
-- it is about to add (crafting affix drop-down, tattoo drop-down); consulted
-- by Tooltip:AddLine before every other stat path. nil outside such a call.
local lineGroups = nil
-- Group map consulted by main:WrapString (the tattoo popup's description
-- labels), set only while that popup builds its labels.
local labelGroups = nil
-- Stat ids for lines drawn where POB's tree data knows them (statIdSpan):
-- popupStatIds while the Timeless jewel finder's / tattoo popup's drop-down
-- tooltipFunc runs, labelStatIds while the tattoo popup builds its description
-- labels, nodeStatIds while a conquered node's tooltip is built. nil outside.
local popupStatIds, labelStatIds, nodeStatIds = nil, nil, nil

-- The gem GemTooltip.AddGemTooltip is describing (nil outside it):
-- { desc = { [description] = effect id }, names = { [name] = true },
--   statIds = { [describeStats line] = stat id },
--   statScopes = { [describeStats line] = its scope (2nd argument) } }.
-- statIds / statScopes are filled by the data.describeStats wrap while the
-- tooltip is being built.
local gemCtx = nil

-- Engine stat lookup (PobToolsTranslateStat(line, ctx[, ids]), ctx "item" /
-- "passive" / "gem"; ids = the line's stat id(s) when known); nil when the
-- engine lacks it, missed, or gave the line back.
local function zhByStat(text, ctx, ids)
	if type(PobToolsTranslateStat) ~= "function" then return nil end
	local ok, zh = pcall(PobToolsTranslateStat, text, ctx, ids)
	if ok and type(zh) == "string" and zh ~= "" and zh ~= text then return zh end
	return nil
end

local function hasTranslateId()
	return type(PobToolsTranslateId) == "function"
end

-- id lookup of one string; nil unless it produced something different.
local function zhById(source, id, en)
	if not hasTranslateId() or id == nil or type(en) ~= "string" or en == "" then return nil end
	local zh = PobToolsTranslateId(source, id, en)
	if zh and zh ~= en then return zh end
	return nil
end

-- One name under a named source (anoint / tattoo / legion): nil unless that
-- lookup produced something different. The source is restored by withSource.
local function zhInSource(source, name)
	if type(name) ~= "string" or name == "" then return nil end
	local zh = withSource(source, PobToolsTranslateDisplay, name)
	if type(zh) == "string" and zh ~= "" and zh ~= name then return zh end
	return nil
end

-- The sourceless (global merged map) answer for one string, or nil.
local function zhGlobal(s)
	local zh = withSource(nil, PobToolsTranslateDisplay, s)
	if type(zh) == "string" and zh ~= "" and zh ~= s then return zh end
	return nil
end

-- The node's name in the unmodified tree, or nil when it cannot be seen:
-- build.spec.tree.nodes[id] (the tree data; ReplaceNode edits the spec's
-- node, not this), else the table the spec node inherits from.
local function originalNodeName(node, build)
	local spec = type(build) == "table" and build.spec or nil
	local tree = type(spec) == "table" and spec.tree or nil
	local nodes = type(tree) == "table" and tree.nodes or nil
	local orig = type(nodes) == "table" and node.id ~= nil and nodes[node.id] or nil
	if type(orig) == "table" and orig ~= node and type(orig.dn) == "string" then
		return orig.dn
	end
	local mt = getmetatable(node)
	local base = type(mt) == "table" and rawget(mt, "__index") or nil
	if type(base) == "table" and base ~= node and type(rawget(base, "dn")) == "string" then
		return rawget(base, "dn")
	end
	return nil
end

-- Whether a node kept its id under a new name. With the original tree name at
-- hand that decides it. Without it (no tree reached AddNodeName) the id answer
-- is read: none (an engine that refuses a mismatch), or exactly the global
-- merged map's answer for dn (an engine that falls back to the passives
-- lookup of dn, which a legion / tattoo name only reaches through the global
-- map), means renamed; an id answer of its own means the node's own name.
local function nodeRenamed(node, dn, build, idZh)
	local orig = originalNodeName(node, build)
	if orig then return orig ~= dn end
	return idZh == nil or idZh == zhGlobal(dn)
end

-- AddNodeName's name line: [colour lead] node.dn [" [" .. node.id .. "]"]
-- (the suffix only in dev mode, PassiveTreeView.lua). Returns the id hit (or
-- nil) and whether this IS the name line: a name line the id lookup missed is
-- looked up under passives by AddLine; every other line stays unmarked.
local function translateNodeNameLine(text)
	local ctx = nodeCtx
	if not ctx or ctx.nameDone then return nil, false end
	local node = ctx.node
	local dn = node.dn
	if type(dn) ~= "string" or dn == "" then return nil, false end
	local lead, rest = splitLeadColour(text)
	local tail
	if rest == dn then
		tail = ""
	elseif rest:sub(1, #dn) == dn and rest:sub(#dn + 1) == " [" .. tostring(node.id) .. "]" then
		tail = rest:sub(#dn + 1)
	else
		return nil, false
	end
	ctx.nameDone = true
	local zh = node.id ~= nil and zhById("passives", node.id, dn) or nil
	-- A tattoo (isTattoo) / Timeless jewel (conqueredBy) node may keep its id
	-- under a new dn; only a RENAMED node takes its name from tattoo / legion.
	-- isTattoo first: ReplaceNode overwrites it, so when both are set the name
	-- is the tattoo's. Conquered nodes are often not renamed at all (Lethal
	-- Pride: conqueredBy on every node in range), and those keep the id name.
	local replSource = (node.isTattoo and "tattoo") or (node.conqueredBy and "legion") or nil
	if replSource and nodeRenamed(node, dn, ctx.build, zh) then
		-- the replacement source must really have the name: an answer that is
		-- just the global merged map's (the engine's fallback) does not count
		local alt = zhInSource(replSource, dn)
		if alt and alt ~= zhGlobal(dn) then zh = alt end
	end
	if not zh then return nil, true end
	return lead .. zh .. tail, true
end

-- The crafting tooltip's "^7Affix: <mod.affix>" line (ItemsTab.lua). Only that
-- plain form: the "Affix: Tier N (<affix>)" form and everything else keep the
-- ordinary line path. The label goes through the dictionary on its own
-- ("Affix:" is a ui.json key); a full-width colon needs no space after it.
local FULLWIDTH_COLON = "\239\188\154"
local function translateAffixLine(text)
	local ctx = affixTipCtx
	if not ctx then return nil end
	local affix = ctx.mod.affix
	if type(affix) ~= "string" or affix == "" then return nil end
	local lead, rest = splitLeadColour(text)
	if rest ~= "Affix: " .. affix then return nil end
	-- id hit only (no English argument, so no fallback): this line stays
	-- English today, and an unsourced fallback could pick a wrong-context word.
	if not hasTranslateId() then return nil end
	local zh = PobToolsTranslateId("affixes", ctx.id)
	if not zh or zh == affix then return nil end
	local label = PobToolsTranslateDisplay("Affix:") or "Affix:"
	local gap = label:sub(-#FULLWIDTH_COLON) == FULLWIDTH_COLON and "" or " "
	return lead .. label .. gap .. zh
end

-- POB's "not supported" note (main.notSupportedTooltipText, e.g.
-- " ^8(Not supported in PoB yet)") appended to an unsupported stat line when
-- it is drawn. Plain suffix compare, no pattern. Returns body, note-or-"".
local function splitNoteTail(s)
	local note = type(main) == "table" and main.notSupportedTooltipText or nil
	if type(note) == "string" and note ~= "" and #s > #note and s:sub(-#note) == note then
		return s:sub(1, #s - #note), note
	end
	return s, ""
end

-- Grouped-line lookup shared by every group map (node sd / flavour, item mod
-- lines, gem flavour, crafting and tattoo descriptions; see addGroups): colour
-- lead and the unsupported note are split off, the rest looked up, both put
-- back on a hit (the colour also after every "\n" of a multi-line answer). An
-- answer of "" is a line whose text went into the line before it: it is drawn
-- blank, note included -- the sentence it belongs to carries the note already.
-- false (an English line with two different answers) is no answer.
local function pairLookup(map, text)
	if not map then return nil end
	local lead, rest = splitLeadColour(text)
	local body, tail = splitNoteTail(rest)
	local zh = map[trim(body)]
	if not zh then return nil end
	if zh == "" then return "" end
	return lead .. (zh:gsub("\n", "\n" .. lead)) .. tail
end

-- POB's tree data carries the stat ids of every Timeless jewel and tattoo /
-- runegraft node (tree.legion.nodes / .additions, tree.tattoo.nodes:
-- stats[statId]); the engine's stat lookup narrows its candidates to them
-- (使用者裁定 2026-10-26「先做有 stat id 的位置」). Built once per data table
-- (weak cache):
--   byId[legion node.id / tattoo key] = that node's stat ids, sorted, " "-joined
--   byLine[statLineKey(sd line)] = the ids of every node that has that line
-- A line is matched as a whole (its template), never by its position.
local statIdCache = setmetatable({}, { __mode = "k" })
-- An sd line with its numbers and "(a-b)" ranges written as "#": a conquered
-- node's rolled value ("+7") finds the data's template ("+(6-8)").
local function statLineKey(s)
	s = trim(s):gsub("%(%-?[%d%.]+%-%-?[%d%.]+%)", "#")
	return (s:gsub("%d+%.%d+", "#"):gsub("%d+", "#"))
end

local function statIdIndex(tree, which)
	local group = type(tree) == "table" and tree[which] or nil
	if type(group) ~= "table" then return nil end
	local hit = statIdCache[group]
	if hit ~= nil then return hit or nil end
	local byId, sets = {}, {}
	local function addNode(key, node)
		if key == nil or type(node) ~= "table" or type(node.stats) ~= "table" then return end
		local ids = {}
		for id in pairs(node.stats) do
			if type(id) == "string" then ids[#ids + 1] = id end
		end
		if #ids == 0 then return end
		table.sort(ids)
		byId[key] = table.concat(ids, " ")
		local lines = {}
		for _, line in ipairs(type(node.sd) == "table" and node.sd or {}) do
			if type(line) == "string" and line ~= "" then lines[#lines + 1] = line end
		end
		-- the tattoo popup may show all of a node's lines joined by " " (its "combine")
		if #lines > 1 then lines[#lines + 1] = table.concat(lines, " ") end
		for _, line in ipairs(lines) do
			local k = statLineKey(line)
			local set = sets[k]
			if not set then
				set = {}
				sets[k] = set
			end
			for _, id in ipairs(ids) do set[id] = true end
		end
	end
	if which == "legion" then
		for _, name in ipairs({ "nodes", "additions" }) do
			if type(group[name]) == "table" then
				for _, node in pairs(group[name]) do
					if type(node) == "table" then addNode(node.id, node) end
				end
			end
		end
	elseif type(group.nodes) == "table" then
		for key, node in pairs(group.nodes) do addNode(key, node) end
	end
	local byLine, any = {}, false
	for k, set in pairs(sets) do
		local t = {}
		for id in pairs(set) do t[#t + 1] = id end
		table.sort(t)
		byLine[k] = table.concat(t, " ")
		any = true
	end
	local idx = any and { byId = byId, byLine = byLine } or false
	statIdCache[group] = idx
	return idx or nil
end

-- { byLine, ids } for one span (ids = the described node's own, when its key
-- is known), or nil when the tree has no such data.
local function statIdSpan(tree, which, key)
	local ok, idx = pcall(statIdIndex, tree, which)
	if not ok or not idx then return nil end
	return { byLine = idx.byLine, ids = key ~= nil and idx.byId[key] or nil }
end

-- One drawn line asked of the stat lookup under "passive" with the stat ids
-- `span` has for it: the described node's ids when known, else those of every
-- node with that line. A line the data does not have (a header, a reminder, a
-- hand-written description) is not asked. Colour lead and POB's unsupported
-- note are split off and put back. nil = no answer (the caller's path runs).
local function zhByStatIds(text, span)
	if not span then return nil end
	local lead, rest = splitLeadColour(text)
	local body, tail = splitNoteTail(rest)
	local lineIds = span.byLine[statLineKey(body)]
	if not lineIds then return nil end
	local zh = zhByStat(lead .. body, "passive", span.ids or lineIds)
	return zh and zh .. tail or nil
end

-- One line of a gem tooltip (gemCtx set). A grantedEffect's description is
-- looked up by that effect's own id under gems_desc; every other non-name line
-- asks the engine's stat lookup under "gem", with the stat id describeStats
-- reported for that exact line (nil for lines POB rewrote afterwards, e.g. the
-- quality "(0-N)" lines). Name lines and misses return nil: the existing path
-- (the caller's "gems" source) runs as before.
local function translateGemLine(text)
	local ctx = gemCtx
	if not ctx then return nil end
	local lead, rest = splitLeadColour(text)
	-- an unsupported stat line carries POB's note (main.notSupportedTooltipText):
	-- looked up without it, put back after the answer (as the item mod lines do)
	local body, noteTail = splitNoteTail(rest)
	local core = trim(body)
	if core == "" or ctx.names[core] then return nil end
	-- flavour text POB splits over several lines (GemTooltip: one AddLine per
	-- flavourText line), drawn as GGPK's whole paragraph (gemContext groups)
	local grouped = pairLookup(ctx.groups, text)
	if grouped then return grouped end
	local id = ctx.desc[core]
	if id ~= nil then
		local zh = zhById("gems_desc", id, core)
		if zh then return lead .. zh end
		-- id miss (or its English differs): the description is gems.json
		-- text, looked up under that source even where no caller set one
		-- (a gem tooltip inside a node tooltip). A miss keeps the English.
		local whole = withSource("gems", PobToolsTranslateDisplay, text)
		if whole then return whole end
		-- a multi-line description the whole lookup missed: line by line,
		-- still under gems; lines that miss stay English
		if text:find("\n", 1, true) then
			local parts, changed = {}, false
			for line in (text .. "\n"):gmatch("([^\n]*)\n") do
				local z = line ~= "" and withSource("gems", PobToolsTranslateDisplay, line) or nil
				if z then changed = true end
				parts[#parts + 1] = z or line
			end
			if changed then return table.concat(parts, "\n") end
		end
		return text
	end
	-- the skill's own stat description file (describeStats' scope argument;
	-- PoE2 keeps it in statSets, so grantedEffect is not read): "gem:<scope>",
	-- or "gem" (the engine's default chain) when it is unknown. A support gem's
	-- gem_stat_descriptions is passed too: the default chain reads skill_stat
	-- first and would pick the active skill's wording.
	local scope = ctx.statScopes[core]
	local zh = zhByStat(lead .. body, scope and ("gem:" .. scope) or "gem", ctx.statIds[core])
	return zh and (zh .. noteTail) or nil
end

-- Asks a\nb under ctx; only a hit of that joined query counts. A two-line
-- answer gives one Chinese line per English line; a one-line answer (or an
-- empty second line) puts the whole sentence on the first line and leaves the
-- second blank. The first line must not be empty; more than two lines: nil.
local function statPair(a, b, ctx)
	if a == "" or b == "" or a:find("\n", 1, true) or b:find("\n", 1, true) then return nil end
	local zh = zhByStat(a .. "\n" .. b, ctx)
	if not zh then return nil end
	if not zh:find("\n", 1, true) then return zh, "" end
	local z1, z2 = zh:match("^([^\n]*)\n([^\n]*)$")
	if z1 and z1 ~= "" then return z1, z2 end
	return nil
end

-- One list of consecutive lines as POB will draw them (colour lead and note
-- already stripped, trimmed) -> map entries for the lines that are ONE
-- multi-line GGPK description. The engine's line groups
-- (PobToolsTranslateLines, ggpk_groups.json) recognise the run and lay the
-- whole translation out like GGPK's Chinese: its lines in order, "" for a line
-- to leave blank (the whole sentence sits on the first). An engine without it
-- falls back to adjacent pairs asked as "a\nb" (statPair). An English line
-- that gets two different answers, or is also drawn outside any group, maps to
-- false (no answer: it keeps the per-line path). `plain` collects the lines
-- drawn ungrouped across every list of one map. Returns whether a group was
-- found.
local function addGroups(map, plain, shown, ctx)
	local found, answers = false, nil
	local grouped = {}
	local function put(k, z)
		local cur = map[k]
		if cur == nil then map[k] = z elseif cur ~= z then map[k] = false end
		grouped[k] = true
		found = true
	end
	if type(PobToolsTranslateLines) == "function" then
		local ok, res = pcall(PobToolsTranslateLines, shown, ctx)
		if ok and type(res) == "table" then
			answers = res
			for i = 1, #shown do
				if type(res[i]) == "string" then put(shown[i], res[i]) end
			end
		end
	else
		local i = 1
		while shown[i + 1] do
			local z1, z2 = statPair(shown[i], shown[i + 1], ctx)
			if z1 then
				put(shown[i], z1)
				put(shown[i + 1], z2)
				i = i + 2
			else
				i = i + 1
			end
		end
	end
	for _, k in ipairs(shown) do
		if not grouped[k] and k ~= "" then plain[k] = true end
	end
	for k in pairs(plain) do
		if map[k] then map[k] = false end
	end
	return found, answers
end

-- A list of raw lines (colour lead / note allowed) -> stripped, trimmed copy;
-- nil when an entry is not a string.
local function shownLines(list)
	if type(list) ~= "table" then return nil end
	local shown = {}
	for _, s in ipairs(list) do
		if type(s) ~= "string" then return nil end
		local _, r = splitLeadColour(s)
		shown[#shown + 1] = trim((splitNoteTail(r)))
	end
	return shown
end

-- Group map over several independent lists (no group spans two lists; a list
-- may be nil), or nil when nothing grouped.
local function buildGroupMap(ctx, ...)
	local map, plain, found = {}, {}, false
	for i = 1, select("#", ...) do
		local shown = shownLines((select(i, ...)))
		if shown and #shown >= 2 and addGroups(map, plain, shown, ctx) then found = true end
	end
	return found and map or nil
end

-- Item mod lists in POB's AddItemTooltip order (PoE1 enchant / scourge /
-- implicit / explicit / crucible, PoE2 enchant / rune / implicit / explicit).
local ITEM_MOD_LISTS = { "enchantModLines", "scourgeModLines", "runeModLines",
	"implicitModLines", "explicitModLines", "crucibleModLines" }

-- One stat template POB split over adjacent mod lines of the same list. Each
-- list's lines are taken as AddItemTooltip draws them
-- (itemLib.formatModLine(modLine, dbMode); nil = not drawn), colour and note
-- stripped, and grouped under "item" (addGroups). nil when the engine has no
-- stat lookup or nothing grouped.
local function buildItemModPairs(item, dbMode)
	if type(PobToolsTranslateStat) ~= "function" or type(item) ~= "table" then return nil end
	local fmt = type(itemLib) == "table" and itemLib.formatModLine or nil
	if type(fmt) ~= "function" then return nil end
	local map, plain, found = {}, {}, false
	for _, key in ipairs(ITEM_MOD_LISTS) do
		local list = item[key]
		if type(list) == "table" then
			local shown = {}
			for _, modLine in ipairs(list) do
				-- same filter as AddItemTooltip: a line of a variant that is
				-- not selected is not drawn, and is drawn count times if > 1
				local count = 1
				if type(item.GetModLineVariantCount) == "function" then
					local ok, n = pcall(item.GetModLineVariantCount, item, modLine)
					count = ok and type(n) == "number" and n or 0
				end
				local f = count > 0 and fmt(modLine, dbMode) or nil
				if type(f) == "string" then
					local _, rest = splitLeadColour(f)
					local shownLine = trim((splitNoteTail(rest)))
					for _ = 1, count do shown[#shown + 1] = shownLine end
				end
			end
			if #shown >= 2 and addGroups(map, plain, shown, "item") then found = true end
		end
	end
	return found and map or nil
end

-- A passive node's tooltip lines: its stat lines (node.sd), reminder text and
-- flavour text, each list grouped on its own under "passive" (flavour text
-- groups come from the catalog's texts, which ignore ctx). Used by the tree's
-- node tooltip and the anoint popup's.
local function buildSdPairs(node)
	if type(node) ~= "table" then return nil end
	return buildGroupMap("passive", node.sd, node.reminderText, node.flavourText)
end

-- An anointed amulet's "Allocates <notable>" enchant line (ItemsTab
-- anointItem). The notable name is looked up under anoint and the label on its
-- own; a name the anoint lookup misses leaves the line to the existing path
-- (the whole-line lookup).
local function translateAllocatesLine(text)
	local lead, rest = splitLeadColour(text)
	local name = rest:match("^Allocates (.+)$")
	if not name then return nil end
	local zh = zhInSource("anoint", trim(name))
	if not zh then return nil end
	local label = PobToolsTranslateDisplay("Allocates") or "Allocates"
	return lead .. label .. " " .. zh
end

-- The single modId behind a magic item's name prefix / suffix, or nil when
-- there is none (pasted items often carry no ids) or more than one. When the
-- item also knows the mod, its affix name must be the segment being translated.
local function affixModId(item, listKey, seg)
	local list = item[listKey]
	if type(list) ~= "table" then return nil end
	local found
	for _, a in ipairs(list) do
		local id = type(a) == "table" and a.modId or nil
		if type(id) == "string" and id ~= "" and id ~= "None" then
			if found and found ~= id then return nil end
			found = id
		end
	end
	if not found then return nil end
	local mod = type(item.affixes) == "table" and item.affixes[found] or nil
	if type(mod) == "table" and type(mod.affix) == "string" and trim(mod.affix) ~= seg then
		return nil
	end
	return found
end

-- Source for one AddLine call: "modline" / "uniques" / "items" / "split" /
-- nil (nil = leave the caller's scope alone, i.e. the shipping unmarked
-- lookup). Only NAME lines get a source; "modline" = unmarked first, stats
-- only when that left the line English. Also returns the leading colour run.
local function tooltipLineSource(text, a4)
	if type(a4) == "table" and type(a4.line) == "string" then
		-- PoE1 AddLine(size, text, font, modLine, background). PoE2's 4th
		-- parameter is a background (string or image handle), never such a table.
		return "modline"
	end
	local ctx = itemCtx
	if not ctx then return nil end
	local item = ctx.item
	local lead, rest = splitLeadColour(text)
	local core = trim(rest)
	if core == "" then return nil end
	local base = cleanBaseName(item)
	if type(item.title) == "string" then
		if not ctx.titleDone and core == item.title then
			ctx.titleDone = true
			if UNIQUE_RARITY[item.rarity] then return "uniques", lead end
			return nil, lead
		end
		if base and core == base then
			ctx.titleDone = true
			return "items", lead
		end
		return nil, lead
	end
	if base and core == base then return "items", lead end
	local pre, suf = item.namePrefix, item.nameSuffix
	if base and type(pre) == "string" and type(suf) == "string"
	   and (trim(pre) ~= "" or trim(suf) ~= "")
	   and core == trim(pre .. base .. suf) then
		return "split", lead
	end
	return nil, lead
end

-- One name segment under `source`; nil when the dictionary has nothing.
local function zhSegment(source, seg)
	if seg == "" then return "" end
	local zh = withSource(source, PobToolsTranslateDisplay, seg)
	if zh and zh ~= seg then return zh end
	return nil
end

-- A magic item's title "Heavy Glorious Plate of the Whale" is no dictionary
-- key. Translate prefix (affixes), base (items) and suffix (affixes) on their
-- own and keep POB's spacing. All-or-nothing, like the engine's comma path: a
-- half-translated name is worse than an English one, so nil means "use the
-- plain line path instead".
local function translateSplitTitle(lead, item)
	local base = cleanBaseName(item)
	local preRaw, sufRaw = item.namePrefix, item.nameSuffix
	local pre, suf = trim(preRaw), trim(sufRaw)
	-- prefix / suffix by modId when the item has one (see "lookups by stable id")
	local zp = (pre ~= "" and zhById("affixes", affixModId(item, "prefixes", pre), pre))
		or zhSegment("affixes", pre)
	local zb = zp and zhSegment("items", base)
	local zs = zb and ((suf ~= "" and zhById("affixes", affixModId(item, "suffixes", suf), suf))
		or zhSegment("affixes", suf))
	if not (zp and zb and zs) then return nil end
	local preGap = pre ~= "" and preRaw:match("(%s*)$") or ""
	local sufGap = suf ~= "" and sufRaw:match("^(%s*)") or ""
	return lead .. zp .. preGap .. zb .. sufGap .. zs
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
	class[key] = keepArity(orig, function(self, ...)
		local ret = orig(self, ...)
		after(self)
		return ret
	end)
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
		class.BuildList = keepArity(origBuild, function(self, buf)
			origBuild(self, buf)
			if not hasCJK(buf) then return end
			withSource("gems", appendCJKMatches, self, buf)
		end)
	end
end

-- Passive tree search: match the CJK query against the translated node name and
-- stat lines. `node` is the LAST arg in both PoE1 (self, node) and
-- PoE2 (self, build, node).
PATCHES["PassiveTreeView"] = function(class)
	-- Node tooltips: only the NAME line is passives text (by node.id first,
	-- then the passives source). Stat lines ask PobToolsTranslateStat under
	-- "passive" first (nodeTipActive, set while AddNodeTooltip runs), then the
	-- unmarked lookup. nodeCtx tells Tooltip:AddLine which node is being
	-- named. Both restored even on error. Not fatal when absent.
	local origTip = class.AddNodeTooltip
	if type(origTip) == "function" then
		-- Only marks the span (nodeTipActive) for the stat lookup; same arity.
		-- Also pairs two-line stat templates split over node.sd (nodeSdPairs).
		class.AddNodeTooltip = keepArity(origTip, function(self, ...)
			local prev, prevPairs, prevIds = nodeTipActive, nodeSdPairs, nodeStatIds
			local _, node, build = ...
			local pok, pairsMap = pcall(buildSdPairs, node)
			nodeTipActive = true
			nodeSdPairs = pok and pairsMap or nil
			-- a node a Timeless jewel conquered: its lines are tree.legion's (stat ids there)
			local spec = type(build) == "table" and build.spec or nil
			nodeStatIds = type(node) == "table" and node.conqueredBy and type(spec) == "table"
				and statIdSpan(spec.tree, "legion") or nil
			local ok, a, b, c = pcall(origTip, self, ...)
			nodeTipActive, nodeSdPairs, nodeStatIds = prev, prevPairs, prevIds
			if not ok then error(a, 0) end
			return a, b, c
		end)
	end
	local origName = class.AddNodeName
	if type(origName) ~= "function" then
		log("PassiveTreeView: no AddNodeName, node names stay unmarked")
	else
		class.AddNodeName = keepArity(origName, function(self, tooltip, node, ...)
			local prevCtx = nodeCtx
			local build = ...
			nodeCtx = type(node) == "table" and { node = node, build = build } or nil
			local ok, a, b, c = pcall(origName, self, tooltip, node, ...)
			nodeCtx = prevCtx
			if not ok then error(a, 0) end
			return a, b, c
		end)
	end
	local orig = class.DoesNodeMatchSearchParams
	if orig then
		class.DoesNodeMatchSearchParams = keepArity(orig, function(self, ...)
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
		end)
	end

	-- Window opacity (PobTools launcher slider, Windows): below 100% the engine
	-- draws POB's chrome (side bar, top bar, the tree tab's bottom toolbar)
	-- translucent. The user wants the passive tree to be the fixed layer under
	-- that chrome, so on the tree tab the tree is drawn over the WHOLE window
	-- instead of the tab viewport: same on-screen centre, same scale, only the
	-- canvas grows. Input stays confined to the original viewport -- a hover or
	-- click over the side bar must still belong to the side bar -- except while
	-- a drag that started inside is in progress. The compare tab draws viewers
	-- in its own panes and is left alone (build.viewMode ~= "TREE").
	local origDraw = class.Draw
	local origZoom = class.Zoom
	if origDraw and origZoom then
		-- The tree's own tiled backdrop (the layer under the nodes), drawn by us
		-- with an alpha so a custom background image can show through it.
		-- Upstream's copy is suppressed by setting the asset's width to -1
		-- ("loaded, do not draw" for its `== 0` measure / `> 0` draw checks),
		-- and put back to 0 when neither an image nor a reduced opacity is in
		-- play. Same formula as PassiveTreeView.lua:547-555, using the
		-- viewport and zoom the original will see.
		-- The two forks keep the backdrop differently: PoE1 in tree.assets and
		-- pans it with the tree (PassiveTreeView.lua:547-555); PoE2 through
		-- tree:GetAssetByName("Background2") as a static tile (same file, ~576).
		-- Both use the `width == 0` measure / `width > 0` draw checks.
		local isPoe2Fork = type(class.DrawQuadAndRotate) == "function"
		local zoomLevelMax = isPoe2Fork and 20 or 12 -- upstream Zoom() clamp per fork
		local function treeBackdropAssets(tree)
			if not tree then return {} end
			if type(tree.GetAssetByName) == "function" then
				local ok, a = pcall(tree.GetAssetByName, tree, "Background2")
				return (ok and a) and { a } or {}
			end
			if not tree.assets then return {} end
			local out = {}
			if tree.assets.Background2 then out[#out + 1] = tree.assets.Background2 end
			if tree.assets.Background1 then out[#out + 1] = tree.assets.Background1 end
			return out
		end
		local bgMeasured = setmetatable({}, { __mode = "k" })
		local function drawTreeBackdrop(self, build, vp, alpha)
			local tree = build and build.spec and build.spec.tree
			local bg = treeBackdropAssets(tree)[1]
			if not bg or not bg.handle or alpha <= 0 then return end
			local m = bgMeasured[bg]
			if not m then
				local w, h = bg.handle:ImageSize()
				m = { w = w or 0, h = h or 0 }
				bgMeasured[bg] = m
			end
			if m.w <= 0 then return end
			if not m.logged then
				m.logged = true
				log(string.format("tree backdrop: own draw, image %dx%d alpha=%.2f poe2=%s", m.w, m.h, alpha, tostring(isPoe2Fork)))
			end
			SetDrawColor(1, 1, 1, alpha)
			if isPoe2Fork then
				DrawImage(bg.handle, vp.x, vp.y, vp.width, vp.height, 0, 0, vp.width / 100, vp.height / 100)
			else
				local scale = math.min(vp.width, vp.height) / tree.size * self.zoom
				local bgSize = m.w * scale * 1.33 * 2.5
				DrawImage(bg.handle, vp.x, vp.y, vp.width, vp.height,
					(self.zoomX + vp.width / 2) / -bgSize, (self.zoomY + vp.height / 2) / -bgSize,
					(vp.width / 2 - self.zoomX) / bgSize, (vp.height / 2 - self.zoomY) / bgSize)
			end
			SetDrawColor(1, 1, 1)
		end
		class.Draw = keepArity(origDraw, function(self, build, viewPort, inputEvents)
			local bgPath, _, _, treeBg = "", 50, 0, 100
			if type(PobToolsBackground) == "function" then bgPath, _, _, treeBg = PobToolsBackground() end
			bgPath = bgPath or ""
			treeBg = tonumber(treeBg) or 100
			local ownBackdrop = (bgPath ~= "") or (treeBg < 100)
			local tree = build and build.spec and build.spec.tree
			for _, a in ipairs(treeBackdropAssets(tree)) do
				if ownBackdrop then a.width = -1
				elseif a.width == -1 then a.width = 0 end
			end
			local pct = type(PobToolsWindowOpacity) == "function" and PobToolsWindowOpacity() or 100
			if not pct or pct >= 100 or not viewPort or not build or build.viewMode ~= "TREE" then
				if ownBackdrop and viewPort then
					self.zoomX = self.zoomX or 0
					self.zoomY = self.zoomY or 0
					local okB, errB = pcall(drawTreeBackdrop, self, build, viewPort, treeBg / 100)
					if not okB and not self._ptBackdropErr then
						self._ptBackdropErr = true
						log("tree backdrop draw failed: " .. tostring(errB))
						if type(PobToolsLogError) == "function" then PobToolsLogError("background", "天賦樹底圖繪製失敗：" .. tostring(errB)) end
					end
				end
				return origDraw(self, build, viewPort, inputEvents)
			end
			local screenW, screenH = GetScreenSize()
			local full = { x = 0, y = 0, width = screenW, height = screenH }
			local dx = (viewPort.x + viewPort.width / 2) - screenW / 2
			local dy = (viewPort.y + viewPort.height / 2) - screenH / 2
			local k = math.min(viewPort.width, viewPort.height) / math.min(screenW, screenH)
			self.zoomX = (self.zoomX or 0) + dx
			self.zoomY = (self.zoomY or 0) + dy
			self.zoom = 1.2 ^ self.zoomLevel * k
			self._ptExpandK = k
			local realGetCursorPos = GetCursorPos
			GetCursorPos = function()
				local x, y = realGetCursorPos()
				if self.dragX then return x, y end
				if x >= viewPort.x and x < viewPort.x + viewPort.width
				   and y >= viewPort.y and y < viewPort.y + viewPort.height then
					return x, y
				end
				return -1, -1
			end
			if ownBackdrop then
				local okB, errB = pcall(drawTreeBackdrop, self, build, full, treeBg / 100)
				if not okB and not self._ptBackdropErr then
					self._ptBackdropErr = true
					log("tree backdrop draw failed: " .. tostring(errB))
					if type(PobToolsLogError) == "function" then PobToolsLogError("background", "天賦樹底圖繪製失敗：" .. tostring(errB)) end
				end
			end
			local ok, err = pcall(origDraw, self, build, full, inputEvents)
			GetCursorPos = realGetCursorPos
			self._ptExpandK = nil
			self.zoomX = self.zoomX - dx
			self.zoomY = self.zoomY - dy
			self.zoom = 1.2 ^ self.zoomLevel
			if not ok then error(err, 0) end
		end)
		class.Zoom = keepArity(origZoom, function(self, level, viewPort)
			local k = self._ptExpandK
			if not k then return origZoom(self, level, viewPort) end
			-- upstream Zoom() (PassiveTreeView.lua:1307) with the scale factor
			-- kept; the original would drop it and the tree would jump for a frame.
			self.zoomLevel = math.max(0, math.min(zoomLevelMax, self.zoomLevel + level))
			local oldZoom = self.zoom
			self.zoom = 1.2 ^ self.zoomLevel * k
			local factor = self.zoom / oldZoom
			local cursorX, cursorY = GetCursorPos()
			local relX = cursorX - viewPort.x - viewPort.width / 2
			local relY = cursorY - viewPort.y - viewPort.height / 2
			self.zoomX = relX + (self.zoomX - relX) * factor
			self.zoomY = relY + (self.zoomY - relY) * factor
		end)
	end
end

-- Anoint popup (NotableDBControl): match the CJK query against the translated
-- notable name and stat lines. Non-search eligibility (anointable recipe,
-- PoE2 emotion checkboxes) is delegated to the original by probing it with an
-- empty query, so upstream filter changes keep working untouched.
PATCHES["NotableDBControl"] = function(class)
	local orig = class.DoesNotableMatchFilters
	if orig then
		class.DoesNotableMatchFilters = keepArity(orig, function(self, node)
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
		end)
	end
	-- Anoint list rows: colour .. node.dn. The dn is looked up under anoint
	-- (the notable's name as an anoint), the colour kept; a miss is returned
	-- untouched.
	local origRow = class.GetRowValue
	if type(origRow) == "function" then
		class.GetRowValue = keepArity(origRow, function(self, column, index, node, ...)
			local v = origRow(self, column, index, node, ...)
			if type(v) == "string" and type(node) == "table" and type(node.dn) == "string" then
				local lead, rest = splitLeadColour(v)
				if rest == node.dn then
					local zh = zhInSource("anoint", rest)
					if zh then return lead .. zh end
				end
			end
			return v
		end)
	end
	-- Anoint popup node tooltip (AddValueTooltip: name via AddNodeName, then a
	-- line per node.sd / reminder line): marked like PassiveTreeView's
	-- AddNodeTooltip, so its stat lines ask the passive stat lookup and a GGPK
	-- description POB split over several sd lines is drawn whole (nodeSdPairs).
	-- Restored even on error.
	local origValueTip = class.AddValueTooltip
	if type(origValueTip) == "function" then
		class.AddValueTooltip = keepArity(origValueTip, function(self, tooltip, index, node, ...)
			local prev, prevPairs = nodeTipActive, nodeSdPairs
			local pok, pairsMap = pcall(buildSdPairs, node)
			nodeTipActive = true
			nodeSdPairs = pok and pairsMap or nil
			local ok, a, b, c = pcall(origValueTip, self, tooltip, index, node, ...)
			nodeTipActive, nodeSdPairs = prev, prevPairs
			if not ok then error(a, 0) end
			return a, b, c
		end)
	end
end

-- Tree tab popups whose drop-downs list node names: the tattoo picker
-- (ModifyNodePopup, PoE1 only) and the Timeless jewel finder's node list
-- (FindTimelessJewel). Both lists are locals of the popup function, labelled
-- node.dn .. 48 spaces .. stat text, and the legion one is rebuilt in place
-- when the jewel type changes -- so the control's own Draw is wrapped once the
-- popup is open, and every Draw renames the items it has not seen yet (dn part
-- only, under tattoo / legion; the stat part stays as is).
--
-- The English dn comes from the item's id (tree.tattoo.nodes[id] /
-- tree.legion.nodes by .id), not from the label: the engine's treetab.lua
-- source patch already writes the GLOBAL translation into the label's dn part
-- when the list is built, and that Chinese text is no key under tattoo /
-- legion. Without an id match the label's own dn part is used.
local LABEL_PAD = string.rep(" ", 48)
local renamedItems = setmetatable({}, { __mode = "k" })

-- id -> English dn for the popup's source, from build.spec.tree.
local function popupDnById(tree, source)
	local map = {}
	local group = type(tree) == "table" and tree[source] or nil
	local nodes = type(group) == "table" and group.nodes or nil
	if type(nodes) ~= "table" then return map end
	for key, node in pairs(nodes) do
		if type(node) == "table" and type(node.dn) == "string" then
			if source == "tattoo" then
				map[key] = node.dn
			elseif node.id ~= nil then
				map[node.id] = node.dn
			end
		end
	end
	return map
end

local function renameListLabels(list, source, dnById)
	if type(list) ~= "table" then return end
	for _, v in ipairs(list) do
		if type(v) == "table" and not renamedItems[v] and type(v.label) == "string" then
			renamedItems[v] = true
			local s = v.label:find(LABEL_PAD, 1, true)
			local dn = (v.id ~= nil and dnById and dnById[v.id])
				or (s and v.label:sub(1, s - 1) or v.label)
			local zh = zhInSource(source, dn)
			if zh then v.label = zh .. (s and v.label:sub(s) or "") end
		end
	end
end

local function wrapPopupList(controlName, source, tree)
	local popup = type(main) == "table" and type(main.popups) == "table" and main.popups[1] or nil
	local ctl = type(popup) == "table" and type(popup.controls) == "table"
		and popup.controls[controlName] or nil
	if type(ctl) ~= "table" or rawget(ctl, "_ptRenameDraw") then return end
	local origDraw = ctl.Draw
	if type(origDraw) ~= "function" then return end
	ctl._ptRenameDraw = true
	local dnById = popupDnById(tree, source)
	ctl.Draw = keepArity(origDraw, function(self, ...)
		local ok, err = pcall(renameListLabels, self.list, source, dnById)
		if not ok then log("popup list rename failed: " .. tostring(err)) end
		return origDraw(self, ...)
	end)
end

-- Tattoo / runegraft popup (ModifyNodePopup): the description under the
-- drop-down is one LabelControl per WrapString piece of each description
-- line (constructUI: main:WrapString(desc) per node.sd line), and the
-- drop-down's tooltip one AddLine per line, so a GGPK description POB split
-- over two sd lines showed as two halves. labelGroups (every tattoo node's sd
-- grouped once per tree, see tattooGroups) is consulted by the WrapString wrap
-- while constructUI runs -- inside ModifyNodePopup and inside the drop-down's
-- selFunc -- and the tooltip's lines go through lineGroups while its
-- tooltipFunc runs. Nothing is written into POB's tables: the descriptions
-- stay English and only what is drawn changes.
local tattooGroupCache = setmetatable({}, { __mode = "k" })
-- Per group map: { [previous line .. "\1" .. line] = Chinese line or "" } --
-- one English continuation line shared by several tattoos ("a Unique Enemy is
-- in your Presence", three Loyalty Tattoos) has a different GGPK line in each,
-- so the line-only map holds false there; the line before it tells them apart.
-- false = that pair has two answers too. labelPrev: the previous label line
-- WrapString saw while labelGroups is set (nil = none yet).
local labelSeq = setmetatable({}, { __mode = "k" })
local labelPrev = nil
local function tattooGroups(tree)
	local nodes = type(tree) == "table" and type(tree.tattoo) == "table" and tree.tattoo.nodes or nil
	if type(nodes) ~= "table" then return nil end
	-- F2 off: no map (and nothing cached: the answers are translations)
	if type(PobToolsGetTranslate) == "function" and not PobToolsGetTranslate() then return nil end
	local hit = tattooGroupCache[nodes]
	if hit ~= nil then return hit or nil end
	local map, plain, found = {}, {}, false
	local seq = {}
	for _, node in pairs(nodes) do
		local shown = type(node) == "table" and shownLines(node.sd) or nil
		local grouped, res = false, nil
		if shown and #shown >= 2 then grouped, res = addGroups(map, plain, shown, "passive") end
		if grouped then found = true end
		if res then
			for i = 1, #shown do
				if type(res[i]) == "string" then
					local k = (shown[i - 1] or "") .. "\1" .. shown[i]
					if seq[k] == nil then seq[k] = res[i] elseif seq[k] ~= res[i] then seq[k] = false end
				end
			end
		end
	end
	if found then labelSeq[map] = seq end
	tattooGroupCache[nodes] = found and map or false
	return found and map or nil
end

-- Runs fn(...) with labelGroups (and labelStatIds: the tattoo data's stat ids,
-- the node's own when its key is known) set for the tattoo popup's labels.
local function withLabelGroups(tree, key, fn, ...)
	local prev, prevLine, prevIds = labelGroups, labelPrev, labelStatIds
	local okG, g = pcall(tattooGroups, tree)
	labelGroups = okG and g or nil
	labelStatIds = statIdSpan(tree, "tattoo", key)
	labelPrev = nil
	local ok, a, b, c = pcall(fn, ...)
	labelGroups, labelPrev, labelStatIds = prev, prevLine, prevIds
	if not ok then error(a, 0) end
	return a, b, c
end

local function wrapTattooPopup(tree)
	local popup = type(main) == "table" and type(main.popups) == "table" and main.popups[1] or nil
	local ctl = type(popup) == "table" and type(popup.controls) == "table" and popup.controls.modSelect or nil
	if type(ctl) ~= "table" or rawget(ctl, "_ptGroupWrap") then return end
	ctl._ptGroupWrap = true
	local origSel = ctl.selFunc
	if type(origSel) == "function" then
		ctl.selFunc = keepArity(origSel, function(...)
			-- DropDownControl calls selFunc(index, list[index]); value.id = the tattoo key
			local _, value = ...
			return withLabelGroups(tree, type(value) == "table" and value.id or nil, origSel, ...)
		end)
	end
	local origTip = ctl.tooltipFunc
	if type(origTip) == "function" then
		ctl.tooltipFunc = keepArity(origTip, function(tooltip, mode, index, value, ...)
			local prev = lineGroups
			local okG, g = pcall(function()
				return type(value) == "table" and buildGroupMap("passive", value.descriptions) or nil
			end)
			lineGroups = okG and g or nil
			local ok, a, b, c = pcall(origTip, tooltip, mode, index, value, ...)
			lineGroups = prev
			if not ok then error(a, 0) end
			return a, b, c
		end)
	end
end

-- The Timeless jewel finder's / tattoo popup's drop-down tooltip: one AddLine
-- per line of the hovered item's descriptions (value.id = the legion node id /
-- tattoo key). popupStatIds is set while it runs, also restored on error.
local function wrapStatIdTip(controlName, which, tree)
	local popup = type(main) == "table" and type(main.popups) == "table" and main.popups[1] or nil
	local ctl = type(popup) == "table" and type(popup.controls) == "table"
		and popup.controls[controlName] or nil
	if type(ctl) ~= "table" or rawget(ctl, "_ptStatIdTip") then return end
	local origTip = ctl.tooltipFunc
	if type(origTip) ~= "function" then return end
	ctl._ptStatIdTip = true
	ctl.tooltipFunc = keepArity(origTip, function(tooltip, mode, index, value, ...)
		local prev = popupStatIds
		popupStatIds = statIdSpan(tree, which, type(value) == "table" and value.id or nil)
		local ok, a, b, c = pcall(origTip, tooltip, mode, index, value, ...)
		popupStatIds = prev
		if not ok then error(a, 0) end
		return a, b, c
	end)
end

-- Runs fn(...) with the engine's "/"-joined description lookup asking under
-- ctx (PobToolsSetSlashContext; an older engine has none: fn runs as is), the
-- previous ctx put back afterwards, also on error.
local function withSlashCtx(ctx, fn, ...)
	if type(PobToolsSetSlashContext) ~= "function" then return fn(...) end
	local prev = PobToolsSetSlashContext(ctx)
	local ok, a, b, c = pcall(fn, ...)
	PobToolsSetSlashContext(prev)
	if not ok then error(a, 0) end
	return a, b, c
end

-- Mastery popup: each effect is one list row, its stat lines joined by " / "
-- (OpenMasteryPopup: t_concat(effect.stats, " / ")), so a GGPK description POB
-- split over two lines reaches the engine's "/" lookup as one string. Those are
-- passive stats: the lookup asks under "passive" (not the crafting rows'
-- "item") while the popup is built -- the control measures its rows there --
-- and while PassiveMasteryControl draws them.
PATCHES["PassiveMasteryControl"] = function(class)
	local orig = class.Draw
	if type(orig) ~= "function" then error("PassiveMasteryControl has no Draw to wrap") end
	class.Draw = keepArity(orig, function(self, ...)
		return withSlashCtx("passive", orig, self, ...)
	end)
end

PATCHES["TreeTab"] = function(class)
	local origMastery = class.OpenMasteryPopup
	if type(origMastery) == "function" then
		class.OpenMasteryPopup = keepArity(origMastery, function(self, ...)
			return withSlashCtx("passive", origMastery, self, ...)
		end)
	end
	local found = false
	for method, spec in pairs({
		ModifyNodePopup = { "modSelect", "tattoo" },
		FindTimelessJewel = { "nodeSelect", "legion" },
	}) do
		local orig = class[method]
		if type(orig) == "function" then
			found = true
			class[method] = keepArity(orig, function(self, ...)
				local build = type(self) == "table" and self.build or nil
				local tree = type(build) == "table" and type(build.spec) == "table"
					and build.spec.tree or nil
				local a, b, c
				if method == "ModifyNodePopup" then
					a, b, c = withLabelGroups(tree, nil, orig, self, ...)
				else
					a, b, c = orig(self, ...)
				end
				local ok, err = pcall(wrapPopupList, spec[1], spec[2], tree)
				if not ok then log("TreeTab." .. method .. " list wrap failed: " .. tostring(err)) end
				-- the source name is also the tree data's key (tree.tattoo / tree.legion)
				local okS, errS = pcall(wrapStatIdTip, spec[1], spec[2], tree)
				if not okS then log("TreeTab." .. method .. " stat id wrap failed: " .. tostring(errS)) end
				if method == "ModifyNodePopup" then
					local okT, errT = pcall(wrapTattooPopup, tree)
					if not okT then log("TreeTab.ModifyNodePopup description wrap failed: " .. tostring(errT)) end
				end
				return a, b, c
			end)
		end
	end
	if not found then error("TreeTab has neither ModifyNodePopup nor FindTimelessJewel") end
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
	class.DoesItemMatchFilters = keepArity(orig, function(self, item)
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
	end)

	-- List rows: GetRowValue draws colorCodes[rarity] .. item.name, and a
	-- unique's name is "<title>, <base>". Translate the halves under their own
	-- sources (uniques / items) and keep the ", ". All-or-nothing: when either
	-- half is unknown the row is returned untouched, i.e. exactly what the draw
	-- path used to translate on its own. Non-unique rows keep the unmarked
	-- source for the first half. Not fatal when absent.
	local origRow = class.GetRowValue
	if type(origRow) == "function" then
		class.GetRowValue = keepArity(origRow, function(self, column, index, item, ...)
			local v = origRow(self, column, index, item, ...)
			if type(v) ~= "string" or type(item) ~= "table" or hasCJK(v) then return v end
			local lead, rest = splitLeadColour(v)
			local first, second = rest:match("^(.-), (.+)$")
			if not first or first == "" or second:find(", ", 1, true) then return v end
			local zf
			if UNIQUE_RARITY[item.rarity] then
				zf = zhSegment("uniques", first)
			else
				zf = PobToolsTranslateDisplay(first)
				if zf == first then zf = nil end
			end
			local zs = zf and zhSegment("items", second)
			if not zs then return v end
			return lead .. zf .. ", " .. zs
		end)
	else
		log("ItemDBControl: no GetRowValue, list rows stay unmarked")
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
	class.DoesEntryMatchFilters = keepArity(orig, function(self, searchStr, minionId, filterMode)
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
	end)
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
	class.SearchMatch = keepArity(orig, function(self, txt)
		local search = self.controls and self.controls.search
		local buf = search and search.buf
		if type(txt) == "string" and hasCJK(buf) then
			local zh = zhDisplay(txt)
			if zh and zh:lower():find(buf:lower(), 1, true) then return true end
		end
		return orig(self, txt)
	end)
end

-- Dropdown typed search (SearchHost mixin; e.g. the enchant popup lists):
-- after POB's English word matching, also accept rows whose translated label
-- contains the CJK query. Highlight ranges are byte offsets into the English
-- label, so translation hits clear them (row is shown, just not highlighted).
PATCHES["SearchHost"] = function(class)
	local orig = class.UpdateSearch
	if orig then
		class.UpdateSearch = keepArity(orig, function(self)
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
		end)
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
	class.OnKeyDown = keepArity(orig, function(self, key, doubleClick)
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
	end)
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
-- The attribute lines of a pasted "Requirements:" block. A trade-site copy is
--
--   Requirements:
--   Level: 70
--   Dex: 99
--
-- and PoE1's POB consumes all three (Item.lua: specName == "Dex" ... writes
-- self.requirements). The PoE2 fork kept the "Requirements:" header and the
-- Level line but never ported the attribute branch, so "Dex: 99" falls through
-- to the mod parser and the item ends up wearing a modifier that reads
-- "Dex: 99 (Not supported in PoB yet)" -- reported from a Chinese trade-site
-- paste, but it happens with the English text just the same.
--
-- The requirement itself is not lost: POB computes it from the base item and
-- the mods (the tooltip already said "Requires Level 70, 99 Dex"), so dropping
-- the line is exactly what PoE1 ends up showing.
local ATTR_REQ = { Str = true, Strength = true, Dex = true, Dexterity = true,
                   Int = true, Intelligence = true }
PATCHES["Item"] = function(class)
	local orig = class.ParseRaw
	if not orig then error("Item has no ParseRaw to wrap") end
	class.ParseRaw = keepArity(orig, function(self, raw, ...)
		if type(raw) == "string" and raw:find("Requirements:", 1, true) then
			local out, seenBlock = {}, false
			for line in (raw .. "\n"):gmatch("([^\n]*)\n") do
				local trimmed = line:match("^%s*(.-)%s*$")
				if trimmed == "Requirements:" then
					seenBlock = true
					t_insert(out, line)
				elseif seenBlock and ATTR_REQ[(trimmed:match("^(%a+):%s*%d+"))] then
					-- dropped on purpose; see the note above
				else
					t_insert(out, line)
				end
			end
			raw = table.concat(out, "\n")
		end
		return orig(self, raw, ...)
	end)
end

-- The lines the crafting affix drop-down's tooltip draws for a tier range
-- (ItemsTab.lua drop.tooltipFunc, "N Tiers" branch): each line of the lowest
-- and highest tier merged, "(min-max)" where the numbers differ. Only used to
-- recognise those lines for grouping: a POB that draws them differently
-- simply matches nothing and keeps the per-line path.
local function tierLines(minMod, maxMod)
	if type(minMod) ~= "table" or type(maxMod) ~= "table" then return nil end
	local out = {}
	for l, line in ipairs(minMod) do
		local maxRaw = maxMod[l]
		if type(line) ~= "string" or type(maxRaw) ~= "string" then return nil end
		local minLine = line:gsub("%((%d[%d%.]*)%-(%d[%d%.]*)%)", "%1")
		local maxLine = maxRaw:gsub("%((%d[%d%.]*)%-(%d[%d%.]*)%)", "%2")
		if maxLine == maxRaw then
			out[#out + 1] = maxLine
		else
			local start = 1
			out[#out + 1] = (minLine:gsub("%d[%d%.]*", function(min)
				local s, e, max = maxLine:find("(%d[%d%.]*)", start)
				if not s then return min end
				start = e + 1
				if min == max then return min end
				return "(" .. min .. "-" .. max .. ")"
			end))
		end
	end
	return out
end

-- Group map for the crafting affix drop-down's tooltip(tooltip, mode, index,
-- value): one mod's lines, or the merged lines of a tier range.
local function affixDropGroups(item, mode, index, value)
	local modList = type(value) == "table" and value.modList or nil
	local affixes = type(item) == "table" and item.affixes or nil
	if type(modList) ~= "table" or type(affixes) ~= "table" then return nil end
	if value.modId or #modList == 1 then
		return buildGroupMap("item", affixes[value.modId or modList[1]])
	end
	return buildGroupMap("item", tierLines(affixes[modList[1]], affixes[modList[#modList]]))
end

-- PoE2: picking a rune that changes the affix limit ("Allow Suffix +1") left
-- the newly shown affix drop-down empty -- the rune's selFunc rebuilt the item
-- but never called UpdateAffixControls, so the new row's list was stale/empty
-- and merely hovering it crashed POB (ItemsTab.lua tooltipFunc: attempt to
-- index local 'value' (a nil value)). Upstream master 0.23.1 has the bug in
-- English too; their dev branch added exactly this call. Harmless once POB
-- ships the fix (the list is just rebuilt twice). PoE1 has no rune rows.
PATCHES["ItemsTab"] = function(class)
	-- Item tooltips: no outer source (non-name lines keep the unmarked
	-- lookup); the item is kept in itemCtx so Tooltip:AddLine can tell the
	-- title / base / name lines apart. Restored even when the original
	-- throws. Not fatal when absent.
	local origTip = class.AddItemTooltip
	if type(origTip) == "function" then
		class.AddItemTooltip = keepArity(origTip, function(self, tooltip, item, ...)
			local prevCtx = itemCtx
			itemCtx = type(item) == "table" and { item = item } or nil
			if itemCtx then
				-- AddItemTooltip(tooltip, item, slot, dbMode, ...)
				local _, dbMode = ...
				local pok, modPairs = pcall(buildItemModPairs, item, dbMode)
				itemCtx.modPairs = pok and modPairs or nil
			end
			local ok, a, b, c = pcall(origTip, self, tooltip, item, ...)
			itemCtx = prevCtx
			if not ok then error(a, 0) end
			return a, b, c
		end)
		-- The title line reaches AddLine already translated: an engine source
		-- patch on ItemsTab.lua ("物品提示的標題維持英文") runs item.title through
		-- PobToolsItemTitle first. Give that call the uniques source too, only
		-- while this item's tooltip is being built; everywhere else it is the
		-- original function.
		if type(PobToolsItemTitle) == "function" then
			local origTitle = PobToolsItemTitle
			PobToolsItemTitle = keepArity(origTitle, function(s, ...)
				local ctx = itemCtx
				if ctx and not ctx.titleDone and s == ctx.item.title then
					ctx.titleDone = true
					if UNIQUE_RARITY[ctx.item.rarity] then
						return withSource("uniques", origTitle, s, ...)
					end
				end
				return origTitle(s, ...)
			end)
		end
	else
		log("ItemsTab: no AddItemTooltip, item tooltips stay unmarked")
	end
	local ok = wrapConstructor(class, "ItemsTab", function(self)
		-- Crafting affix drop-down / range slider tooltips: tell AddLine which
		-- mod the "Affix: <name>" line belongs to, so it is looked up by mod Id.
		-- Mirrors the id choice in ItemsTab.lua's tooltipFuncs; anything unclear
		-- (no item, no mod, several tiers) leaves affixTipCtx nil.
		-- groupsOf(item, ...) -> group map for the mod lines the tooltip draws
		-- (lineGroups), or nil.
		local function wrapAffixTip(ctl, idOf, groupsOf)
			local orig = type(ctl) == "table" and ctl.tooltipFunc or nil
			if type(orig) ~= "function" then return end
			ctl.tooltipFunc = keepArity(orig, function(tooltip, ...)
				local prev, prevGroups = affixTipCtx, lineGroups
				local okId, id = pcall(idOf, ...)
				local item = self.displayItem
				local mods = okId and id and type(item) == "table" and item.affixes or nil
				local mod = type(mods) == "table" and mods[id] or nil
				affixTipCtx = type(mod) == "table" and { id = id, mod = mod } or nil
				if groupsOf then
					local okG, g = pcall(groupsOf, item, ...)
					lineGroups = okG and g or nil
				end
				local ok2, a, b, c = pcall(orig, tooltip, ...)
				affixTipCtx, lineGroups = prev, prevGroups
				if not ok2 then error(a, 0) end
				return a, b, c
			end)
		end
		if hasTranslateId() and type(self.controls) == "table" then
			local i = 1
			while self.controls["displayItemAffix" .. i] do
				local drop = self.controls["displayItemAffix" .. i]
				wrapAffixTip(drop, function(mode, index, value)
					local modList = type(value) == "table" and value.modList
					if type(modList) ~= "table" then return nil end
					return value.modId or (#modList == 1 and modList[1]) or nil
				end, affixDropGroups)
				wrapAffixTip(self.controls["displayItemAffixRange" .. i], function()
					local sel = drop.list and drop.list[drop.selIndex]
					local modList = type(sel) == "table" and sel.modList
					return type(modList) == "table" and #modList == 1 and modList[1] or nil
				end)
				i = i + 1
			end
		end
		for i = 1, 6 do
			local drop = self.controls and self.controls["displayItemRune" .. i]
			if drop and type(drop.selFunc) == "function" then
				local orig = drop.selFunc
				drop.selFunc = keepArity(orig, function(...)
					orig(...)
					if self.displayItem and self.displayItem.crafted and self.UpdateAffixControls then
						self:UpdateAffixControls()
					end
				end)
			end
		end
	end)
	if not ok then error("ItemsTab has no constructor to wrap") end
end

PATCHES["Tooltip"] = function(class)
	local orig = class.AddLine
	if not orig then error("Tooltip has no AddLine to wrap") end
	if type(PobToolsTranslateDisplay) ~= "function" then
		error("PobToolsTranslateDisplay unavailable (engine too old)")
	end
	-- whole blob first: GGG ships some stat lines WITH their own "\n" and the
	-- dictionary stores those joined, so the multi-line form is a real key.
	-- Falling back per line covers everything else. Runs under whatever source
	-- is active; returns nil when nothing translated.
	local function translateLine(text)
		local whole = PobToolsTranslateDisplay(text)
		if whole then return whole end
		if text:find("\n", 1, true) then
			local parts, changed = {}, false
			for line in (text .. "\n"):gmatch("([^\n]*)\n") do
				local zh = line ~= "" and PobToolsTranslateDisplay(line) or nil
				if zh then changed = true end
				t_insert(parts, zh or line)
			end
			if changed then return table.concat(parts, "\n") end
		end
		return nil
	end
	class.AddLine = keepArity(orig, function(self, size, text, font, a4, ...)
		-- Tooltip.lua:96 starts a new BLOCK when a line begins with "Equipping"
		-- or "Removing", so translating those would silently change the
		-- tooltip's block structure. Left alone on purpose.
		local structural = type(text) == "string" and
			(text:find("Equipping", 1, true) or text:find("Removing", 1, true))
		if type(text) == "string" and text ~= "" and not structural and not hasCJK(text) then
			-- Which data file this line came from (see "tooltip line sources"
			-- above). nil = whatever the caller's scope already set.
			local src, lead = tooltipLineSource(text, a4)
			local zh, isNodeName = translateNodeNameLine(text)
			zh = zh or translateAffixLine(text)
			if not zh and not isNodeName then zh = translateAllocatesLine(text) end
			-- Stat lines ask the engine's stat lookup first (item mod lines
			-- as "item", passive tooltip description lines as "passive");
			-- name lines never. A miss falls through to the path below.
			local statZh
			if not zh and not isNodeName then
				-- a line of a multi-line description the tooltipFunc's own
				-- source lines grouped (lineGroups), then an item mod line
				-- grouped with its neighbours (buildItemModPairs)
				if lineGroups then statZh = pairLookup(lineGroups, text) end
				if not statZh and itemCtx and itemCtx.modPairs then
					statZh = pairLookup(itemCtx.modPairs, text)
				end
				-- a Timeless jewel / tattoo drop-down tooltip line: passive + the node's stat ids
				if not statZh and popupStatIds and src == nil and not itemCtx and not gemCtx then
					statZh = zhByStatIds(text, popupStatIds)
				end
				if statZh then
					-- decided by the pair
				elseif gemCtx and not itemCtx then
					-- a gem tooltip wins over an enclosing node tooltip (its
					-- lines are gem text); an item tooltip nested in it does not
					statZh = translateGemLine(text)
				elseif src == "modline" then
					-- the unsupported note is not part of the stat: asked
					-- without it, put back after the Chinese
					local lead, rest = splitLeadColour(text)
					local body, tail = splitNoteTail(rest)
					statZh = zhByStat(lead .. body, "item")
					statZh = statZh and statZh .. tail
				elseif src == nil and nodeTipActive and not itemCtx then
					-- not inside an item tooltip nested in the node tooltip
					-- (jewel socket): those lines are item text, not the passive's.
					-- The unsupported note (main.notSupportedTooltipText) is not
					-- part of the stat: asked without it, put back after the
					-- Chinese as written (使用者裁定 2026-10-26: stays English)
					statZh = pairLookup(nodeSdPairs, text)
					-- a conquered node's line the Timeless jewel data knows: with its stat ids
					if not statZh then statZh = zhByStatIds(text, nodeStatIds) end
					if not statZh then
						local lead, rest = splitLeadColour(text)
						local body, tail = splitNoteTail(rest)
						statZh = zhByStat(lead .. body, "passive")
						statZh = statZh and statZh .. tail
					end
				end
			end
			if zh then
				-- decided by id (see "lookups by stable id")
			elseif statZh then
				zh = statZh
			elseif isNodeName then
				zh = withSource("passives", translateLine, text)
			elseif src == "modline" then
				-- unmarked first; stats only when that left the line English
				zh = translateLine(text)
				if not zh or not hasCJK(zh) then
					zh = withSource("stats", translateLine, text) or zh
				end
			elseif src == "split" then
				zh = translateSplitTitle(lead, itemCtx.item)
				if not zh then zh = translateLine(text) end
			elseif src then
				zh = withSource(src, translateLine, text)
			else
				zh = translateLine(text)
			end
			if zh then text = zh end
		end
		return orig(self, size, text, font, a4, ...)
	end)

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
		class.CheckForUpdate = keepArity(origCheck, function(self, ...)
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
		end)
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

-- What the user loses when a patch does not apply. The patch name alone ends up
-- in a bug report as "patch FAILED ItemDBControl", which says nothing about what
-- they would actually notice; this table is what turns the log line into a
-- symptom somebody can confirm.
local PATCH_SYMPTOM = {
	SkillListControl        = "技能列表的中文顯示",
	GemSelectControl        = "技能寶石搜尋框打不進中文",
	PassiveTreeView         = "天賦樹節點的中文顯示",
	NotableDBControl        = "塗油／大天賦搜尋框打不進中文",
	ItemDBControl           = "物品資料庫的名稱與詞綴搜尋打不進中文",
	MinionSearchListControl = "召喚物搜尋框打不進中文",
	CalcsTab                = "計算頁搜尋框打不進中文",
	SearchHost              = "天賦樹搜尋框打不進中文",
	EditControl             = "輸入框貼上中文會變成問號",
	Tooltip                 = "提示視窗的中文顯示",
	ItemsTab                = "選了會加詞綴數的符文後，滑過新的詞綴欄會讓 POB 報錯",
	Item                    = "貼上的物品會多出一條「Dex: 99」之類的假詞綴",
	TreeTab                 = "刺青／軍團珠寶下拉清單的節點名稱維持英文",
	PassiveMasteryControl   = "天賦專精選單裡兩行的描述各半句(未整句翻譯)",
}

local function applyPatch(name, class)
	if applied[name] or not class then return end
	applied[name] = true
	local ok, err = pcall(PATCHES[name], class)
	if ok then
		log("patched " .. name)
	else
		log("patch FAILED " .. name .. ": " .. tostring(err))
		-- The trace above only exists when POB_ZH_INJECT_TRACE is set, i.e. never
		-- on a user's machine. This is the copy that survives to be reported.
		if type(PobToolsLogError) == "function" then
			PobToolsLogError("inject",
				(PATCH_SYMPTOM[name] or name) .. "：" .. name ..
				" patch 套用失敗（POB 可能更新過）— " .. tostring(err))
		end
	end
end

-- Custom background image (PobTools launcher: 背景圖片 / 背景亮度). `main` is a
-- plain global table, not a class, so it is patched here rather than through
-- PATCHES. POB draws its striped backdrop from main:DrawBackground(viewPort),
-- sub-layer -100, once over the whole window (Build.lua) and once per tab
-- viewport; both calls are replaced by the same image, mapped to the WHOLE
-- window and cropped to the viewport given, so the two paint seamlessly.
local bgState = { path = nil, handle = nil, w = 0, h = 0, failed = nil }
-- Draws the configured image over `viewPort` (window coordinates), mapped to
-- the whole window (cover crop). Returns false when there is no usable image.
local function drawBackgroundImage(viewPort)
	local path, bright = PobToolsBackground()
	if not path or path == "" or not viewPort then
		bgState.path, bgState.handle = nil, nil
		return false
	end
	if bgState.path ~= path then
			bgState.path, bgState.handle, bgState.w, bgState.h = path, nil, 0, 0
			local h = NewImageHandle()
			h:Load(path, "CLAMP")
			if h:IsValid() then
				local w, hh = h:ImageSize()
				if w and hh and w > 0 and hh > 0 then
					bgState.handle, bgState.w, bgState.h = h, w, hh
				end
			end
			log(string.format("background image %s: valid=%s size=%sx%s", tostring(path),
				tostring(h:IsValid()), tostring(bgState.w), tostring(bgState.h)))
			if not bgState.handle and bgState.failed ~= path then
				bgState.failed = path
				if type(PobToolsLogError) == "function" then
					PobToolsLogError("background", "背景圖片載入失敗，改用內建底圖：" .. tostring(path))
				end
			end
		end
	if not bgState.handle then return false end
	local screenW, screenH = GetScreenSize()
	if not screenW or screenW <= 0 or screenH <= 0 then return false end
	-- cover: scale the image so it fills the window, crop the overflow
	local scale = math.max(screenW / bgState.w, screenH / bgState.h)
	local drawW, drawH = bgState.w * scale, bgState.h * scale
	local offX, offY = (screenW - drawW) / 2, (screenH - drawH) / 2
	local u0 = (viewPort.x - offX) / drawW
	local v0 = (viewPort.y - offY) / drawH
	local u1 = (viewPort.x + viewPort.width - offX) / drawW
	local v1 = (viewPort.y + viewPort.height - offY) / drawH
	local b = math.max(0, math.min(100, tonumber(bright) or 50)) / 100
	SetDrawLayer(nil, -100)
	SetDrawColor(b, b, b)
	DrawImage(bgState.handle, viewPort.x, viewPort.y, viewPort.width, viewPort.height, u0, v0, u1, v1)
	SetDrawColor(1, 1, 1)
	SetDrawLayer(nil, 0)
	return true
end

local function patchMainBackground()
	if applied.__mainBackground or type(main) ~= "table" or type(main.DrawBackground) ~= "function" then return end
	if type(PobToolsBackground) ~= "function" then return end
	applied.__mainBackground = true
	-- 1. The per-tab / conversion-screen call: with an image configured the
	--    whole-window pass below already covers the viewport, so this becomes
	--    a no-op; without one it is upstream's striped backdrop as before.
	local orig = main.DrawBackground
	main.DrawBackground = keepArity(orig, function(self, viewPort)
		local path = PobToolsBackground()
		if path and path ~= "" and bgState.handle and bgState.path == path then return end
		if path and path ~= "" and drawBackgroundImage(viewPort) then return end
		return orig(self, viewPort)
	end)
	-- 2. The whole-window pass. Upstream only paints its backdrop per tab
	--    viewport (and never on the tree tab, which has its own), so nothing
	--    would show under the side bar / top bar / tree otherwise: paint the
	--    image over the full window at the start of every BUILD frame.
	local bm = main.modes and main.modes.BUILD
	if bm and type(bm.OnFrame) == "function" then
		local origOnFrame = bm.OnFrame
		bm.OnFrame = keepArity(origOnFrame, function(self, ...)
			local screenW, screenH = GetScreenSize()
			if screenW and screenW > 0 then
				pcall(drawBackgroundImage, { x = 0, y = 0, width = screenW, height = screenH })
			end
			return origOnFrame(self, ...)
		end)
		log("patched main.DrawBackground + BUILD.OnFrame (custom background image)")
	else
		log("patched main.DrawBackground (custom background image; BUILD mode not found, no whole-window pass)")
	end
end

-- Performance log (launcher: 效能診斷記錄; engine: host/perf_log.h). Only when
-- it is on: each tab's Draw, the build recalculation and both modes' OnFrame
-- are timed and reported by name, so the log can say "the Items tab spends
-- 9 ms a frame" instead of just "Lua is slow". Wrapping is plain and
-- transparent: arguments and every return value pass through untouched.
-- A class or method this POB does not have is noted once in the log and
-- skipped -- a renamed tab must never break POB, only lose its label.
local perfOn = type(PobToolsPerfEnabled) == "function" and PobToolsPerfEnabled()
local perfWrapped = {}
local PERF_DRAW = {
	TreeTab = "tree", PassiveTreeView = "tree_view", ItemsTab = "items", SkillsTab = "skills",
	CalcsTab = "calcs", ConfigTab = "config", NotesTab = "notes", PartyTab = "party", ImportTab = "import",
}
local function perfWrap(owner, method, label, onDone)
	local orig = owner[method]
	if type(orig) ~= "function" then return false end
	owner[method] = keepArity(orig, function(...)
		local t0 = PobToolsPerfNow()
		local r = { orig(...) }
		local ms = PobToolsPerfNow() - t0
		PobToolsPerfMark(label, ms)
		if onDone then onDone(ms) end
		return unpack(r, 1, table.maxn(r))
	end)
	return true
end
local function perfApplyAll()
	if not perfOn then return end
	for cls, label in pairs(PERF_DRAW) do
		if not perfWrapped[cls] and common.classes[cls] then
			perfWrapped[cls] = true
			if not perfWrap(common.classes[cls], "Draw", label) then
				PobToolsPerfEvent("perf: " .. cls .. ".Draw not found, its time stays inside lua")
			end
		end
	end
	if not perfWrapped.BuildOutput and common.classes.CalcsTab then
		perfWrapped.BuildOutput = true
		perfWrap(common.classes.CalcsTab, "BuildOutput", "recalc", function(ms)
			if ms >= 20 then PobToolsPerfEvent(string.format("build recalculated in %.0f ms", ms)) end
		end)
	end
	if type(main) == "table" and type(main.modes) == "table" then
		for mode, label in pairs({ LIST = "list_frame", BUILD = "build_frame" }) do
			local key = "mode:" .. mode
			if not perfWrapped[key] and type(main.modes[mode]) == "table" then
				perfWrapped[key] = true
				perfWrap(main.modes[mode], "OnFrame", label)
				if mode == "BUILD" then
					perfWrap(main.modes[mode], "Init", "build_load", function(ms)
						PobToolsPerfEvent(string.format("build opened in %.0f ms", ms))
					end)
				end
			end
		end
	end
end

-- ===== gem tooltips (Classes/GemTooltip) =====
-- GemTooltip is a module table, not a newClass class, so PATCHES never sees
-- it: PoE1 gets it with require("Classes.GemTooltip") (one cached table),
-- PoE2 with LoadModule("Classes/GemTooltip") (a fresh table per caller). Both
-- are patched where they come back (require / LoadModule wraps below).
-- AddGemTooltip(tooltip, build, gemInstance, options) sets gemCtx for the
-- whole build of the tooltip: every grantedEffect's description -> its own id
-- (PoE1 secondaryGrantedEffect, PoE2 additionalGrantedEffects), and the lines
-- data.describeStats hands back -> their stat id (lineMap). Restored even on
-- error. Without the engine's lookups the lines take the existing path.
local patchedGemMods = setmetatable({}, { __mode = "k" })
local describeWraps = setmetatable({}, { __mode = "k" })

-- data.describeStats(stats, scopeName, quality) -> out, lineMap. Every return
-- value goes back untouched (count included); while gemCtx is set the lineMap
-- (line -> stat id) is recorded for AddLine.
local function pack(...)
	return { n = select("#", ...), ... }
end

local function wrapDescribeStats(dataTbl)
	if type(dataTbl) ~= "table" then return end
	local orig = rawget(dataTbl, "describeStats")
	if type(orig) ~= "function" or describeWraps[orig] then return end
	local wrapped = keepArity(orig, function(...)
		local r = pack(orig(...))
		local ctx = gemCtx
		if ctx and type(r[2]) == "table" then
			for line, id in pairs(r[2]) do
				if type(line) == "string" and type(id) == "string" then
					ctx.statIds[trim(line)] = id
				end
			end
		end
		local _, scope = ...
		if ctx and type(scope) == "string" and scope ~= "" and type(r[1]) == "table" then
			for _, line in ipairs(r[1]) do
				if type(line) == "string" then ctx.statScopes[trim(line)] = scope end
			end
		end
		return unpack(r, 1, r.n)
	end)
	describeWraps[wrapped] = true
	dataTbl.describeStats = wrapped
end

local function gemContext(gemInstance)
	local ctx = { desc = {}, names = {}, statIds = {}, statScopes = {} }
	local gemData = type(gemInstance) == "table" and gemInstance.gemData or nil
	if type(gemData) ~= "table" then return ctx end
	if type(gemData.name) == "string" then ctx.names[trim(gemData.name)] = true end
	local function add(effect)
		if type(effect) ~= "table" then return end
		if type(effect.name) == "string" then ctx.names[trim(effect.name)] = true end
		local d = effect.description
		if type(d) == "string" and d ~= "" and effect.id ~= nil and ctx.desc[trim(d)] == nil then
			ctx.desc[trim(d)] = effect.id
		end
	end
	add(gemData.grantedEffect)
	add(gemData.secondaryGrantedEffect)
	if type(gemData.additionalGrantedEffects) == "table" then
		for _, e in ipairs(gemData.additionalGrantedEffects) do add(e) end
	end
	-- GemTooltip draws the primary effect's flavour text, one line each
	local primary = gemData.grantedEffect
	if type(primary) == "table" and type(primary.flavourText) == "table" then
		local ok, groups = pcall(buildGroupMap, "gem", primary.flavourText)
		ctx.groups = ok and groups or nil
	end
	return ctx
end

local function patchGemTooltipModule(mod)
	if type(mod) ~= "table" or patchedGemMods[mod] then return end
	local orig = rawget(mod, "AddGemTooltip")
	if type(orig) ~= "function" then return end
	patchedGemMods[mod] = true
	mod.AddGemTooltip = keepArity(orig, function(tooltip, build, gemInstance, ...)
		if type(build) == "table" then wrapDescribeStats(build.data) end
		wrapDescribeStats(data)
		local prev = gemCtx
		gemCtx = gemContext(gemInstance)
		local ok, a, b, c = pcall(orig, tooltip, build, gemInstance, ...)
		gemCtx = prev
		if not ok then error(a, 0) end
		return a, b, c
	end)
	log("patched GemTooltip.AddGemTooltip")
end

local function isGemTooltipName(name)
	if type(name) ~= "string" then return false end
	-- "Classes.GemTooltip" (require) / "Classes/GemTooltip[.lua]" (LoadModule)
	local n = name:gsub("%.lua$", ""):gsub("[\\%.]", "/")
	return n == "Classes/GemTooltip" or n:sub(-#"/Classes/GemTooltip") == "/Classes/GemTooltip"
end

-- main:WrapString: while labelGroups is set (the tattoo popup building its
-- description labels), a line that belongs to a grouped GGPK description is
-- wrapped as its share of the whole translation ("" = a blank label). Outside
-- that span it is the original function; the engine's own WrapString source
-- patch (translate before wrapping) still runs inside it.
local function patchMainWrapString()
	if applied.__wrapString or type(main) ~= "table" or type(main.WrapString) ~= "function" then return end
	applied.__wrapString = true
	local orig = main.WrapString
	main.WrapString = keepArity(orig, function(self, str, ...)
		if (labelGroups or labelStatIds) and type(str) == "string" then
			-- the previous line + this one first (labelSeq), then this line alone,
			-- then a line the tattoo data knows by its stat ids
			local lead, rest = splitLeadColour(str)
			local body, tail = splitNoteTail(rest)
			local line = trim(body)
			local seq = labelGroups and labelSeq[labelGroups]
			local z = seq and seq[(labelPrev or "") .. "\1" .. line]
			if z then
				if z ~= "" then z = lead .. (z:gsub("\n", "\n" .. lead)) .. tail end
			else
				z = pairLookup(labelGroups, str)
			end
			if not z then z = zhByStatIds(str, labelStatIds) end
			labelPrev = line
			if z then str = z end
		end
		return orig(self, str, ...)
	end)
end

local function tryApplyAll()
	for name in pairs(PATCHES) do
		if not applied[name] and common.classes[name] then
			applyPatch(name, common.classes[name])
		end
	end
	local ok, err = pcall(patchMainBackground)
	if not ok then log("patch FAILED main.DrawBackground: " .. tostring(err)) end
	local okW, errW = pcall(patchMainWrapString)
	if not okW then log("patch FAILED main.WrapString: " .. tostring(errW)) end
	if perfOn then
		local pok, perr = pcall(perfApplyAll)
		if not pok then log("perf wrap FAILED: " .. tostring(perr)) end
	end
end

tryApplyAll() -- classes already loaded at injection time

-- Gem tooltip module: already required before injection, or required later.
if type(package) == "table" and type(package.loaded) == "table" then
	patchGemTooltipModule(package.loaded["Classes.GemTooltip"])
end
if type(require) == "function" then
	local origRequire = require
	require = keepArity(origRequire, function(name, ...)
		local m = origRequire(name, ...)
		if isGemTooltipName(name) then
			local pok, perr = pcall(patchGemTooltipModule, m)
			if not pok then log("patch FAILED GemTooltip: " .. tostring(perr)) end
		end
		return m
	end)
end

if type(LoadModule) == "function" then
	local origLoadModule = LoadModule
	local depth = 0
	LoadModule = keepArity(origLoadModule, function(...)
		depth = depth + 1
		-- class files return at most a couple of values; preserve them.
		-- pcall so an error inside a module cannot leave depth stuck above zero
		-- (which would silence every later patch); the error is rethrown as-is.
		local ok, a, b, c, d = pcall(origLoadModule, ...)
		depth = depth - 1
		if not ok then error(a, 0) end
		if isGemTooltipName((...)) then
			local pok, perr = pcall(patchGemTooltipModule, a)
			if not pok then log("patch FAILED GemTooltip: " .. tostring(perr)) end
		end
		if depth == 0 then tryApplyAll() end
		return a, b, c, d
	end)
end

log("CJK inject ready")
