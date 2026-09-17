-- PobTools headless bridge: the Lua side of the new UI.
--
-- Loaded by the engine (ui_main.cpp HeadlessLoadBridge) into POB's own Lua
-- state after Launch.lua's OnInit and after poecharm_inject.lua, only when the
-- engine runs headless (POB_ZH_HEADLESS=1). The host sends
-- {"id","method","params"} lines on stdin; the dispatcher registered below
-- answers each one. Everything on screen in the new UI comes through here.
--
-- Rules this file lives by (they are what keeps it alive across POB's own
-- self-updates, which the classic mode already survives):
--
--   1. Never copy a POB algorithm. If the number is not already in a POB table,
--      call the POB function that computes it; if there is no such function,
--      the new UI does not show that number yet.
--   2. Read only named, public-ish state (build.calcsTab.mainOutput, the
--      statBox rows POB itself renders, launch.versionNumber...). No poking at
--      a control's private fields to reconstruct what its Draw would show.
--   3. Intercept with a wrapper (call the original, observe, hand back its
--      result), the same way poecharm_inject.lua does -- never a rewrite.
--   4. Every POB surface touched is registered in `probes`, and self_check runs
--      them all. The host refuses to open the new UI on a failed gate and
--      falls back to the classic window, so an upstream refactor is a banner,
--      not a silent wrong number.
--   5. Never compare English UI text. "^7Minion:" is a label, not a key.
--
-- Phase 0 covers: boot, version, load a build, sidebar, flat stats, save to
-- XML (the golden oracle), synchronous update check, update status.

local BRIDGE_VERSION = "0.2.0"

local emit = PobToolsBridgeEmit
local setDispatcher = PobToolsBridgeSetDispatcher
if type(emit) ~= "function" or type(setDispatcher) ~= "function" then
	-- Not the headless engine. Loaded by mistake; do nothing rather than error.
	return
end

-- Translation: the same dictionaries the classic window draws with, applied at
-- the point text leaves POB. Returns the input when there is nothing to say.
local function tr(s)
	if type(s) ~= "string" then return s end
	local zh = PobToolsTranslateDisplay and PobToolsTranslateDisplay(s)
	return zh or s
end

-- Which game this POB is for. The host names it (POB_GAME, the same variable
-- that picks the translation dictionaries); PoE2's POB also numbers its tree
-- versions from 0_1, which settles it when the variable is absent.
local GAME = os.getenv("POB_GAME") == "poe2" and "poe2"
	or (type(latestTreeVersion) == "string" and latestTreeVersion:match("^0_") and "poe2")
	or "poe1"

local function log_error(msg)
	if PobToolsLogError then PobToolsLogError("bridge", msg) end
end

-- ---------------------------------------------------------------------------
-- Probes: every POB surface the bridge depends on, each checked by name.
-- ---------------------------------------------------------------------------

local probes = {}
-- probe(name, fn[, cap]): without `cap` a failure fails the gate (the new UI
-- falls back to the classic window); with one, the failure only switches that
-- capability off (capabilities()) and the page hides the feature. PoE2's
-- Path of Building is a fork that never gained some PoE1 surfaces (item
-- influences, enchant/crucible dialogs, the account-name import); those are
-- capabilities, not reasons to refuse the whole interface.
local function probe(name, fn, cap) probes[#probes + 1] = { name = name, fn = fn, cap = cap } end

local function nparams(fn)
	local info = debug.getinfo(fn, "u")
	return info and info.nparams
end

probe("launch.main", function() return type(launch) == "table" and type(launch.main) == "table" end)
probe("launch.versionNumber", function() return type(launch.versionNumber) == "string" end)
probe("launch.CheckForUpdate", function() return type(launch.CheckForUpdate) == "function" end)
probe("launch.ApplyUpdate", function() return type(launch.ApplyUpdate) == "function" end)
probe("launch.OnFrame", function() return type(launch.OnFrame) == "function" end)
probe("main.modes.BUILD", function() return type(launch.main.modes) == "table" and type(launch.main.modes.BUILD) == "table" end)
probe("main.SetMode", function() return type(launch.main.SetMode) == "function" end)
probe("main.buildPath", function() return type(launch.main.buildPath) == "string" end)
probe("buildMode.Init(5 params)", function() return nparams(launch.main.modes.BUILD.Init) == 6 end) -- self + 5
-- master 2.67.2: (statList, actor); beta: (statList, actor, actorName). The
-- wrapper below takes either.
probe("buildMode.AddDisplayStatList(statList, actor[, actorName])", function()
	local n = nparams(launch.main.modes.BUILD.AddDisplayStatList)
	return n == 3 or n == 4
end)
probe("buildMode.RefreshStatList", function() return type(launch.main.modes.BUILD.RefreshStatList) == "function" end)
probe("buildMode.SaveDB", function() return type(launch.main.modes.BUILD.SaveDB) == "function" end)
probe("buildMode.LoadDB", function() return type(launch.main.modes.BUILD.LoadDB) == "function" end)
probe("common.classes", function() return type(common) == "table" and type(common.classes) == "table" end)

-- POB loads Classes/<Name>.lua on the first new("<Name>") (Common.lua getClass);
-- at boot most classes are not in yet. Loading one here the same way is safe:
-- it is what the next new() would have done.
local function class_of(name)
	local c = common.classes[name]
	if c == nil then
		pcall(LoadModule, "Classes/" .. name)
		c = common.classes[name]
	end
	return c
end
-- POB's object construction differs between branches: master 2.67.2 runs the
-- class constructor inside new(className, ...); beta returns the bare object
-- and the caller runs it (new("X"):X(...)). make() uses whichever form this
-- POB's own new() has, so every object is built the way POB builds it.
local function make(className, ...)
	local info = debug.getinfo(new, "u")
	if info and info.isvararg then
		return new(className, ...)
	end
	local obj = new(className)
	return obj[className](obj, ...)
end
probe("new() builds objects (new(name, ...) or new(name):Name(...))", function()
	local t = make("Tooltip")
	return type(t) == "table" and type(t.AddLine) == "function"
end)
probe("classes.CalcsTab.BuildOutput", function() local c = class_of("CalcsTab"); return type(c) == "table" and type(c.BuildOutput) == "function" end)
probe("classes.CalcBreakdownControl.SetBreakdownData", function() local c = class_of("CalcBreakdownControl"); return type(c) == "table" and type(c.SetBreakdownData) == "function" end)
probe("Modules.BuildListHelpers.ScanFolder/SortList (+FilterList on PoE1)", function()
	local ok, h = pcall(require, "Modules.BuildListHelpers")
	return ok and type(h) == "table" and type(h.ScanFolder) == "function" and type(h.SortList) == "function"
end)
probe("classes.PassiveSpec.CountAllocNodes", function() local c = class_of("PassiveSpec"); return type(c) == "table" and type(c.CountAllocNodes) == "function" end)
probe("classes.PassiveSpec.AllocNode", function() local c = class_of("PassiveSpec"); return type(c) == "table" and type(c.AllocNode) == "function" and type(c.DeallocNode) == "function" end)
probe("UpdateCheck.lua present", function() local f = io.open("UpdateCheck.lua", "r"); if f then f:close() return true end return false end)

-- What this POB offers beyond the gate. A missing capability turns one
-- feature off on the page instead of refusing the whole interface: the
-- sidebar breakdown (Build.lua GetSidebarBreakdown) is on the beta branch
-- only, so on master the rows simply have no breakdown to open.
local capMissing = {}
local function capabilities()
	local B = launch and launch.main and launch.main.modes and launch.main.modes.BUILD
	local caps = {
		sidebarBreakdown = type(B) == "table" and type(B.GetSidebarBreakdown) == "function",
	}
	for _, p in ipairs(probes) do
		if p.cap then caps[p.cap] = not capMissing[p.cap] end
	end
	return caps
end

local gate = { ok = false, failed = {}, checked = 0 }

local function run_probes()
	gate = { ok = true, failed = {}, checked = 0, optional = {} }
	capMissing = {}
	if type(launch) == "table" and launch.promptMsg then
		-- POB's own boot error (the popup nobody can see headless).
		gate.ok = false
		gate.failed[#gate.failed + 1] = "POB reported: " .. tostring(launch.promptMsg)
	end
	for _, p in ipairs(probes) do
		gate.checked = gate.checked + 1
		local ok, res = pcall(p.fn)
		if not ok or not res then
			local why = p.name .. (ok and "" or (": " .. tostring(res)))
			if p.cap then
				capMissing[p.cap] = true
				gate.optional[#gate.optional + 1] = why
			else
				gate.ok = false
				gate.failed[#gate.failed + 1] = why
			end
		end
	end
	return gate
end

-- ---------------------------------------------------------------------------
-- Helpers
-- ---------------------------------------------------------------------------

local function main() return launch.main end
local function build()
	local m = main()
	return m and m.modes and m.modes.BUILD
end

-- One POB frame: exactly what the engine's frame loop calls. POB rebuilds
-- calculation output, the sidebar and any pending mode switch inside it.
local function frame()
	launch:OnFrame()
end

local function ensure_build()
	local b = build()
	if not b or not b.calcsTab or not b.calcsTab.mainOutput then
		error("no build is loaded; call load_build_file first", 0)
	end
	return b
end

local function is_scalar(v)
	local t = type(v)
	return t == "number" or t == "string" or t == "boolean"
end

-- Sidebar rows carry only text. Wrapping AddDisplayStatList (rule 3) feeds it
-- one stat entry at a time and tags every row that appended with the entry's
-- stat key and the actor, so the UI can group by key and never has to read
-- "^7Minion:" (rule 5). POB's own spacer logic only looks at the previous row,
-- so splitting the list changes nothing it produces.
local wrapped = false
local function wrap_add_display_stat_list()
	local B = build()
	if wrapped or not B or type(B.AddDisplayStatList) ~= "function" then return end
	local orig = B.AddDisplayStatList
	B.AddDisplayStatList = function(self, statList, actor, actorName)
		local list = self.controls and self.controls.statBox and self.controls.statBox.list
		-- Before actorName was passed in (master 2.67.2) RefreshStatList handed
		-- over the actor itself; which one it is says the same thing.
		if actorName == nil and type(actor) == "table" then
			local env = self.calcsTab and self.calcsTab.mainEnv
			if env and actor == env.minion then
				actorName = "minion"
			elseif env and actor == env.player then
				actorName = "player"
			end
		end
		for _, statData in ipairs(statList) do
			local before = list and #list or 0
			orig(self, { statData }, actor, actorName)
			if list then
				for i = before + 1, #list do
					local row = list[i]
					if type(row) == "table" then
						row.__stat = statData.stat or statData.labelStat
						row.__actor = actorName
					end
				end
			end
		end
	end
	wrapped = true
end

-- ---------------------------------------------------------------------------
-- Methods
-- ---------------------------------------------------------------------------

local M = {}

function M.ping() return { pong = true } end

function M.version()
	return {
		pobVersion = launch.versionNumber,
		pobBranch = launch.versionBranch,
		pobPlatform = launch.versionPlatform,
		bridge = BRIDGE_VERSION,
		caps = capabilities(),
		game = GAME,
		headless = PobToolsHeadless and PobToolsHeadless() or false,
		gate = gate,
		-- Where POB keeps builds for this install (userPath .. "Builds/"); a
		-- load_build_file path must start with it.
		buildPath = launch.main and launch.main.buildPath or nil,
		-- POB reopens the last build by itself at startup (Settings.xml); the
		-- UI must not assume it starts empty.
		buildLoaded = (build() and build().calcsTab and build().calcsTab.mainOutput) and true or false,
	}
end

function M.self_check()
	return run_probes()
end

-- load_build_file{path=...}: an absolute path under main.buildPath (POB
-- derives the sub-folder from it). Runs the mode switch and the first
-- calculation before answering.
function M.load_build_file(p)
	local path = p and p.path
	if type(path) ~= "string" or path == "" then error("params.path required", 0) end
	local m = main()
	local f = io.open(path, "r")
	if not f then error("cannot open " .. path, 0) end
	f:close()
	local name = path:match("([^/\\]+)%.xml$") or path:match("([^/\\]+)$")
	m:SetMode("BUILD", path, name)
	wrap_add_display_stat_list()
	frame() -- mode switch + Build:Init + first BuildOutput
	frame() -- anything the first pass flagged again
	local b = ensure_build()
	return {
		buildName = b.buildName,
		dbFileName = b.dbFileName,
		outputRevision = b.outputRevision,
		className = b.spec and b.spec.curClassName,
		ascendClassName = b.spec and b.spec.curAscendClassName,
		level = b.characterLevel,
	}
end

-- Every scalar in calcsTab.mainOutput, plus one level down for the table
-- entries (output.MainHand.Accuracy -> "MainHandAccuracy"): that concatenation
-- is how Build.lua names a childStat in <PlayerStat stat=...>, so the keys
-- here are exactly the saved build's.
local function scalar_out(v)
	-- JSON cannot carry inf/nan; POB itself saves them as tostring ("inf"),
	-- so hand those over the same way.
	if type(v) == "number" and (v ~= v or v == math.huge or v == -math.huge) then
		return tostring(v)
	end
	return v
end

function M.get_stats()
	local b = ensure_build()
	local out = {}
	local n = 0
	for k, v in pairs(b.calcsTab.mainOutput) do
		if type(k) == "string" then
			if is_scalar(v) then
				out[k] = scalar_out(v)
				n = n + 1
			elseif type(v) == "table" then
				for ck, cv in pairs(v) do
					if type(ck) == "string" and is_scalar(cv) then
						out[k .. ck] = scalar_out(cv)
						n = n + 1
					end
				end
			end
		end
	end
	return { stats = out, count = n, outputRevision = b.outputRevision }
end

-- The sidebar exactly as POB filled it: one entry per row, text translated,
-- raw kept. Spacer rows have no text.
function M.get_sidebar()
	local b = ensure_build()
	local list = b.controls and b.controls.statBox and b.controls.statBox.list
	if type(list) ~= "table" then error("statBox list missing", 0) end
	local canBreakdown = type(b.GetSidebarBreakdown) == "function"
	local rows = {}
	for i, row in ipairs(list) do
		local lhs, rhs = row[1], row[2]
		rows[i] = {
			h = row.height,
			lhs = lhs and tr(lhs) or nil,
			rhs = rhs and tr(rhs) or nil,
			lhsRaw = lhs,
			rhsRaw = rhs,
			stat = row.__stat,
			actor = row.__actor,
			align = row.align,
			-- Either key is enough for GetSidebarBreakdown to say something;
			-- without that function (master) there is nothing to open.
			hasBreakdown = (canBreakdown and (row.breakdown ~= nil or row.modNames ~= nil)) and true or false,
		}
	end
	local warnings = {}
	if b.controls.warnings and type(b.controls.warnings.lines) == "table" then
		for i, w in ipairs(b.controls.warnings.lines) do
			warnings[i] = { text = tr(w), raw = w }
		end
	end
	return { rows = rows, warnings = warnings, rev = b.outputRevision, outputRevision = b.outputRevision }
end

-- The breakdown POB shows when a sidebar row is hovered: same three calls the
-- classic window makes (Build.lua ShowDisplayStat -> GetSidebarBreakdown ->
-- CalcBreakdownControl:SetBreakdownData), then read the control's section
-- list and clear it again. rowIndex is 1-based into get_sidebar's rows.
local function cell_text(v)
	if type(v) == "number" then
		if v == math.floor(v) then return tostring(math.floor(v)) end
		return string.format("%.4g", v)
	end
	if v == nil then return "" end
	return tostring(v)
end

function M.sidebar_breakdown(p)
	local b = ensure_build()
	local idx = p and tonumber(p.rowIndex)
	local list = b.controls.statBox.list
	local line = idx and list[idx]
	if not line then error("no sidebar row " .. tostring(idx), 0) end
	if (not line.breakdown and not line.modNames) or type(b.GetSidebarBreakdown) ~= "function" then
		return { sections = {}, rev = b.outputRevision }
	end
	local ctl = b.controls.breakdown
	local data = b:GetSidebarBreakdown(line.breakdown, line.modNames, line.ignoredSections, line.actorName)
	local sections = {}
	local ok, err = pcall(function()
		ctl:SetBreakdownData(data, false, line.actorName)
		for _, s in ipairs(ctl.sectionList or {}) do
			if s.type == "TEXT" then
				local lines = {}
				for i, l in ipairs(s.lines) do lines[i] = tr(l) end
				sections[#sections + 1] = { type = "text", size = s.textSize or 16, lines = lines }
			elseif s.type == "TABLE" then
				local cols, rows = {}, {}
				for i, c in ipairs(s.colList or {}) do
					cols[i] = { label = tr(c.label or ""), key = tostring(c.key), right = c.right and true or false }
				end
				for i, r in ipairs(s.rowList or {}) do
					local row = {}
					for _, c in ipairs(s.colList or {}) do
						row[tostring(c.key)] = tr(cell_text(r[c.key]))
					end
					rows[i] = setmetatable(row, { __object = true })
				end
				sections[#sections + 1] = {
					type = "table",
					label = s.label and tr(s.label) or nil,
					footer = s.footer and tr(s.footer) or nil,
					cols = cols,
					rows = rows,
				}
			elseif s.type == "RADIUS" then
				sections[#sections + 1] = { type = "radius", radius = s.radius }
			end
		end
	end)
	ctl:SetBreakdownData() -- always clear, the classic window does too (ClearDisplayStat)
	if not ok then error(err, 0) end
	return { sections = sections, rev = b.outputRevision }
end

-- Builds under main.buildPath, scanned/filtered/sorted by POB's own helpers
-- (Modules/BuildListHelpers: the same index the classic build list shows).
-- list_builds{subPath, filter?, sortMode?}: the build list screen. `filter`
-- is its search box (main.filterBuildList: PoE1 matches words and class:x
-- across the subfolders, PoE2 globs file names in this folder), `sortMode`
-- its sort drop-down (main.buildSortMode, which POB keeps in Settings.xml).
function M.list_builds(p)
	local subPath = p and p.subPath or ""
	local helpers = require("Modules.BuildListHelpers")
	local m = main()
	local sortModes = {}
	for i, s in ipairs(helpers.buildSortDropList or {}) do
		sortModes[i] = { sortMode = s.sortMode, label = s.label }
		if p and p.sortMode == s.sortMode then m.buildSortMode = s.sortMode end
	end
	local filter = ""
	if p and type(p.filter) == "string" then
		filter = p.filter
		m.filterBuildList = filter
	end
	-- PoE1: ScanFolder indexes, FilterList picks the folder's entries. PoE2's
	-- helpers predate that split: ScanFolder(subPath, filter) returns the list.
	local list
	if helpers.FilterList then
		list = helpers.FilterList(helpers.ScanFolder(subPath), subPath, filter)
	else
		list = helpers.ScanFolder(subPath, filter)
	end
	helpers.SortList(list, m.buildSortMode or "NAME")
	local entries = {}
	for i, e in ipairs(list) do
		entries[i] = {
			isFolder = e.folderName ~= nil,
			folderName = e.folderName,
			fileName = e.fileName,
			fullFileName = e.fullFileName,
			subPath = e.subPath,
			buildName = e.buildName,
			level = e.level,
			className = e.className,
			ascendClassName = e.ascendClassName,
			modified = e.modified,
		}
	end
	return { buildPath = m.buildPath, subPath = subPath, entries = entries, filter = filter, sortMode = m.buildSortMode or "NAME", sortModes = sortModes }
end

-- The header the UI shows: name, class, level, points. Points come from
-- CountAllocNodes and the limits from the string POB itself formats for the
-- point display (Build.lua EstimatePlayerProgress) -- the arithmetic behind
-- usedMax lives there and nowhere else.
local function strip_escapes(s)
	if type(s) ~= "string" then return s end
	return (s:gsub("%^x%x%x%x%x%x%x", ""):gsub("%^%d", ""))
end

function M.get_build_info()
	local b = ensure_build()
	local spec = b.spec
	local used, ascUsed, secondaryAscUsed, sockets = 0, 0, 0, 0
	if spec and spec.CountAllocNodes then
		used, ascUsed, secondaryAscUsed, sockets = spec:CountAllocNodes()
	end
	local pd = b.controls.pointDisplay
	local usedMax, ascMax
	if pd and type(pd.str) == "string" then
		local a, bm, c, d = strip_escapes(pd.str):match("(%d+)%s*/%s*(%d+)%s+(%d+)%s*/%s*(%d+)")
		usedMax, ascMax = tonumber(bm), tonumber(d)
	end
	return {
		buildName = b.buildName,
		dbFileName = b.dbFileName or nil,
		unsaved = b.unsaved and true or false,
		level = b.characterLevel,
		classId = spec and spec.curClassId,
		className = spec and spec.curClassName,
		classNameZh = spec and tr(spec.curClassName),
		ascendClassId = spec and spec.curAscendClassId,
		ascendClassName = spec and spec.curAscendClassName,
		ascendClassNameZh = spec and tr(spec.curAscendClassName),
		treeVersion = spec and spec.treeVersion,
		points = {
			used = used, ascUsed = ascUsed, secondaryAscUsed = secondaryAscUsed, sockets = sockets,
			usedMax = usedMax, ascMax = ascMax,
			display = pd and pd.str, req = pd and pd.req and tr(pd.req),
		},
		rev = b.outputRevision,
	}
end

-- The build serialised the way POB saves it. The <PlayerStat> elements in it
-- are the golden oracle: they are POB's own tostring of mainOutput.
function M.save_xml()
	local b = ensure_build()
	local xml = b:SaveDB(b.dbFileName or "unsaved.xml")
	if type(xml) ~= "string" then error("SaveDB returned nothing", 0) end
	return { xml = xml, dbFileName = b.dbFileName }
end

-- Synchronous update check, the way Launch.lua's first-run path does it:
-- LoadModule("UpdateCheck") in this thread. Needs the CWD to be the POB
-- folder (the engine's PCall sets it) and network access. Returns POB's own
-- answer: "none" | "normal" | "basic", or the error text.
function M.check_update_sync()
	local mode, err = LoadModule("UpdateCheck")
	return { mode = mode, error = err }
end

function M.get_update_status()
	return {
		available = launch.updateAvailable,
		checking = launch.updateCheckRunning and true or false,
		progress = launch.updateProgress,
		error = launch.updateErrMsg,
	}
end

-- Kick POB's own background check (LaunchSubScript thread); poll
-- get_update_status for the answer.
function M.check_update_async()
	launch:CheckForUpdate(false)
	return { started = true }
end

function M.apply_update(p)
	local mode = p and p.mode or launch.updateAvailable
	if mode ~= "normal" and mode ~= "basic" then error("no update to apply (mode=" .. tostring(mode) .. ")", 0) end
	-- "basic" ends in Exit() inside this very call, so the reply never leaves;
	-- the page learns what is happening from this event instead.
	emit("update_applying", { mode = mode })
	launch:ApplyUpdate(mode)
	return { applied = mode }
end

-- ---------------------------------------------------------------------------
-- Passive tree
-- ---------------------------------------------------------------------------

local function as_object(t)
	return setmetatable(t, { __object = true })
end

local function file_exists(path)
	local f = io.open(path, "rb")
	if f then f:close() return true end
	return false
end

local function current_tree_version(p)
	local v = p and p.version
	if type(v) ~= "string" or v == "" then
		local b = build()
		v = b and b.spec and b.spec.treeVersion or latestTreeVersion
	end
	return v
end

-- The tree as POB has already laid it out: PassiveTree:PassiveTree() computed
-- every node's x/y from group + orbit (ProcessNode), built the connector list
-- (BuildConnector: straight line, or an arc around the group centre) and
-- decided which frame art each node type uses (nodeOverlay). All of that is
-- read here and handed over; nothing about the tree's geometry is recomputed
-- on the page. Frame names follow PassiveTreeView.lua's rule per state.
local FRAME_STATES = { "alloc", "path", "unalloc" }
local CLASS_ART = { -- PassiveTreeView.lua: the class illustration, fixed per class id
	[1] = { name = "BackgroundStr", x = -2750, y = 1600 },
	[2] = { name = "BackgroundDex", x = 2550, y = 1600 },
	[3] = { name = "BackgroundInt", x = -250, y = -2200 },
	[4] = { name = "BackgroundStrDex", x = -150, y = 2350 },
	[5] = { name = "BackgroundStrInt", x = -2100, y = -1500 },
	[6] = { name = "BackgroundDexInt", x = 2350, y = -1950 },
}

local function node_frames(tree, node)
	local ov = node.overlay
	if not ov then return nil end
	local frames = {}
	if GAME == "poe2" then
		-- PoE2 (PassiveTree:ProcessNode): node.overlay already is the node's own
		-- set (ascendancy frames included), keyed by state.
		for _, state in ipairs(FRAME_STATES) do frames[state] = ov[state] end
		return as_object(frames)
	end
	local prefix = ""
	if node.ascendancyName then
		prefix = node.bloodlineOverlayPrefix or (tree.bloodlineSpritePrefixes and tree.bloodlineSpritePrefixes[node.ascendancyName]) or ""
	end
	for _, state in ipairs(FRAME_STATES) do
		local name
		if node.type == "Socket" then
			name = ov[state .. (node.expansionJewel and "Alt" or "")]
			if name and node.dn == "Charm Socket" then name = "Azmeri" .. name end
		else
			name = ov[state .. (node.ascendancyName and "Ascend" or "") .. (node.isBlighted and "Blighted" or "")]
			if name and prefix ~= "" then name = prefix .. name end
		end
		frames[state] = name
	end
	return as_object(frames)
end

local treeCache = {}
function M.tree_data(p)
	local v = current_tree_version(p)
	if treeCache[v] then return treeCache[v] end
	local tree = main():LoadTree(v)
	if not tree then error("no tree version " .. tostring(v), 0) end

	-- groups: id, centre, which orbits are in use (ring art), ascendancy plate
	local groups, groupIndex = {}, {}
	for gid, g in pairs(tree.groups) do
		groupIndex[g] = gid
		local oo = {}
		for o in pairs(g.oo or {}) do oo[#oo + 1] = o end
		table.sort(oo)
		groups[#groups + 1] = {
			id = gid, x = g.x, y = g.y, oo = oo,
			isProxy = g.isProxy and true or false,
			asc = g.ascendancyName, ascStart = g.isAscendancyStart and true or false,
			-- PoE2's renderGroup: only groups that name a background get one
			bg = type(g.background) == "table" and {
				image = g.background.image, half = g.background.isHalfImage and true or false,
				offsetX = g.background.offsetX, offsetY = g.background.offsetY,
			} or nil,
		}
	end

	local nodes, count = {}, 0
	for id, node in pairs(tree.nodes) do
		if type(id) == "number" and type(node.x) == "number" then
			count = count + 1
			local stats, statsZh = {}, {}
			for i, s in ipairs(node.sd or {}) do stats[i] = s; statsZh[i] = tr(s) end
			local linked = {}
			for i, other in ipairs(node.linkedId or {}) do linked[i] = other end
			local effects
			if node.masteryEffects then
				effects = {}
				for i, e in ipairs(node.masteryEffects) do
					local es, esZh = {}, {}
					for j, s in ipairs(e.sd or e.stats or {}) do es[j] = s; esZh[j] = tr(s) end
					effects[i] = { effect = e.effect, stats = es, statsZh = esZh }
				end
			end
			nodes[tostring(id)] = {
				id = id, name = node.dn, nameZh = tr(node.dn), type = node.type,
				stats = stats, statsZh = statsZh,
				x = node.x, y = node.y, size = node.size or 0,
				group = node.group and groupIndex[node.group] or nil,
				orbit = node.o,
				asc = node.ascendancyName,
				classStartIndex = node.classStartIndex,
				startArt = node.startArt,
				icon = node.icon, activeIcon = node.activeIcon, inactiveIcon = node.inactiveIcon,
				effectImage = node.activeEffectImage,
				frames = node_frames(tree, node),
				blighted = node.isBlighted and true or false,
				expansion = node.expansionJewel and true or false,
				expansionParent = (node.expansionJewel and node.expansionJewel.parent) and true or false,
				proxy = node.isProxy and true or false,
				linked = linked,
				masteryEffects = effects,
				flavour = node.flavourText,
				-- PoE2 draws every node at PassiveTree:GetNodeTargetSize's half
				-- extents (DrawAsset doubles them) instead of sheet size x 1.33.
				draw = node.targetSize and {
					w = node.targetSize.width, h = node.targetSize.height,
					ow = node.targetSize.overlay and node.targetSize.overlay.width,
					oh = node.targetSize.overlay and node.targetSize.overlay.height,
					ew = node.targetSize.effect and node.targetSize.effect.width,
					eh = node.targetSize.effect and node.targetSize.effect.height,
				} or nil,
				attribute = node.isAttribute and true or nil,
			}
		end
	end

	-- connectors: POB built one entry per edge (two for arcs it mirrored);
	-- the page only needs which pair, and whether it curves around a group.
	local connectors, seen = {}, {}
	for _, c in ipairs(tree.connectors or {}) do
		local a, b = c.nodeId1, c.nodeId2
		local key = (a < b) and (a .. ":" .. b) or (b .. ":" .. a)
		if not seen[key] then
			seen[key] = true
			local orbit = c.type and tonumber(c.type:match("^Orbit(%d+)$"))
			local e = { a = a, b = b, orbit = orbit, asc = c.ascendancyName }
			-- PassiveTree:BuildArc puts the arc's centre in the quad's first
			-- vertex. PoE1 arcs always centre on the group; PoE2 connections can
			-- curve around a point of their own, so hand over POB's.
			local v = orbit and c.vert and c.vert.Normal
			if v and type(v[1]) == "number" and type(v[2]) == "number" then
				e.cx, e.cy = v[1], v[2]
			end
			connectors[#connectors + 1] = e
		end
	end

	local classes = {}
	for cid, class in pairs(tree.classes) do
		if type(cid) == "number" and type(class) == "table" then
			local ascs = {}
			for i, a in ipairs(class.ascendancies or {}) do
				ascs[#ascs + 1] = { id = i, key = a.id, name = a.name, nameZh = tr(a.name) }
			end
			local bg
			if GAME == "poe2" and type(class.background) == "table" then
				-- PoE2 PassiveTreeView:Draw: the class plate, BGTreeActive turned
				-- towards the start node, BGTree over it; each ascendancy its own
				-- plate (full colour for the current one, 50% grey otherwise).
				local cb = class.background
				bg = { image = cb.image, x = cb.x, y = cb.y, w = cb.width, h = cb.height,
				       active = cb.active and { w = cb.active.width, h = cb.active.height } or nil,
				       center = cb.bg and { w = cb.bg.width, h = cb.bg.height } or nil, ascendancies = {} }
				for ascId, a in pairs(class.classes or {}) do
					if type(ascId) == "number" and ascId > 0 and type(a.background) == "table" then
						local ab = a.background
						bg.ascendancies[#bg.ascendancies + 1] = { id = ascId, key = a.id, image = ab.image,
							x = ab.x, y = ab.y, w = ab.width, h = ab.height, replace = a.replace, replaceBy = a.replaceBy }
						if ab.image and ascs[ascId] then ascs[ascId].bg = ab.image end
					end
				end
			end
			classes[#classes + 1] = {
				id = cid, name = class.name, nameZh = tr(class.name),
				startNodeId = class.startNodeId, ascendancies = ascs,
				art = GAME ~= "poe2" and CLASS_ART[cid] or nil,
				background = bg,
			}
		end
	end
	table.sort(classes, function(x, y) return x.id < y.id end)
	local alternate = {}
	for ascId, a in pairs(tree.alternate_ascendancies or {}) do
		if type(a) == "table" then alternate[#alternate + 1] = { id = ascId, key = a.id, name = a.name, nameZh = tr(a.name) } end
	end
	table.sort(alternate, function(x, y) return tostring(x.id) < tostring(y.id) end)

	local out = {
		game = GAME,
		artScale = tree.scaleImage,
		treeVersion = v,
		size = tree.size,
		bounds = { minX = tree.min_x, minY = tree.min_y, maxX = tree.max_x, maxY = tree.max_y },
		orbitRadii = tree.orbitRadii,
		nodes = as_object(nodes),
		nodeCount = count,
		groups = groups,
		connectors = connectors,
		classes = classes,
		alternateAscendancies = alternate,
	}
	treeCache[v] = out
	return out
end

-- Every named sprite and standalone image PassiveTree uses, as pixel rects
-- into the files under TreeData (served to the page by the host). Sheet
-- rects come from sprites.lua's own coords, not from the spriteMap's UVs:
-- headless ImageSize() cannot read .webp, so those UVs would be wrong.
local function sheet_file(v, filename)
	local base = filename:gsub("%?%x+$", ""):gsub(".*/", "")
	if file_exists("TreeData/" .. v .. "/" .. base) then return "TreeData/" .. v .. "/" .. base end
	if file_exists("TreeData/" .. base) then return "TreeData/" .. base end
	return nil
end

function M.tree_assets(p)
	local v = current_tree_version(p)
	local tree = main():LoadTree(v)
	if not tree then error("no tree version " .. tostring(v), 0) end
	local assets, disabled, sheets = {}, {}, {}
	local missing = {}
	for spriteType, data in pairs(tree.skillSprites or {}) do
		local file = data.filename and sheet_file(v, data.filename)
		if not file then
			missing[#missing + 1] = tostring(spriteType)
		else
			sheets[file] = { w = data.w, h = data.h }
			local bucket = (spriteType:match("Inactive$") or spriteType:match("Disabled$")) and disabled or assets
			for name, c in pairs(data.coords or {}) do
				bucket[name] = { file = file, x = c.x, y = c.y, w = c.w, h = c.h, ow = c.w, oh = c.h }
			end
		end
	end
	-- Standalone PNGs (frames, orbits, class plates...). Only the ones that are
	-- real files: a few names PassiveTree fills from a sheet already have a rect.
	for name, data in pairs(tree.assets or {}) do
		if not assets[name] and type(data) == "table" and not data[1] then
			local file
			if file_exists("TreeData/" .. name .. ".png") then file = "TreeData/" .. name .. ".png"
			elseif file_exists("TreeData/" .. v .. "/" .. name .. ".png") then file = "TreeData/" .. v .. "/" .. name .. ".png" end
			if file then
				local w, h = data.width or 0, data.height or 0
				assets[name] = { file = file, x = 0, y = 0, w = w, h = h, ow = w, oh = h }
				sheets[file] = { w = w, h = h }
			end
		end
	end
	-- PoE2: art lives in DDS texture arrays (tree.ddsCoords: file -> sprite name
	-- -> layer). The engine decodes each array once into PNG pages under
	-- PobTools\cache (PobToolsTextureAtlas); a sprite is its layer's cell there.
	-- "cache:" tells the page to load from the cache host, not the install.
	local ddsFailed = {}
	if type(tree.ddsCoords) == "table" then
		for fileName, names in pairs(tree.ddsCoords) do
			local stem = ("%s_%s_%s"):format(GAME, v, fileName:gsub("%.dds%.zst$", ""):gsub("%.dds$", "")):gsub("[^%w_%.%-]", "_")
			local layout, why
			if PobToolsTextureAtlas then
				layout, why = PobToolsTextureAtlas("TreeData/" .. v .. "/" .. fileName, stem)
			else
				why = "engine without PobToolsTextureAtlas"
			end
			if not layout then
				ddsFailed[#ddsFailed + 1] = fileName .. ": " .. tostring(why)
			else
				for _, page in ipairs(layout.pages) do
					sheets["cache:" .. page.file] = { w = page.w, h = page.h }
				end
				local bucket = fileName:match("^skills%-disabled") and disabled or assets
				for name, layer in pairs(names) do
					local i = (tonumber(layer) or 1) - 1
					local page = layout.pages[math.floor(i / layout.perPage) + 1]
					if page then
						local j = i % layout.perPage
						local rect = { file = "cache:" .. page.file,
							x = (j % layout.perRow) * layout.layerW, y = math.floor(j / layout.perRow) * layout.layerH,
							w = layout.layerW, h = layout.layerH, ow = layout.srcW or layout.layerW, oh = layout.srcH or layout.layerH }
						-- the same icon name in more than one sheet size: keep the largest
						local prev = bucket[name]
						if not prev or (prev.ow or 0) < rect.ow then bucket[name] = rect end
					end
				end
			end
		end
		-- PoE2's standalone PNGs (connector and orbit art) are named by file.
		for name, data in pairs(tree.assets or {}) do
			if not assets[name] and type(data) == "table" and type(data[1]) == "string" then
				local file = "TreeData/" .. v .. "/" .. data[1]
				if file_exists(file) then
					local w, h = data.width or 0, data.height or 0
					assets[name] = { file = file, x = 0, y = 0, w = w, h = h, ow = w, oh = h }
					sheets[file] = { w = w, h = h }
				end
			end
		end
	end
	for _, f in ipairs(ddsFailed) do missing[#missing + 1] = f end

	-- Images PassiveTreeView loads by file path rather than through the tree's
	-- asset table (its constructor, :28-70): the jewel radius rings. Sizes are
	-- not needed -- the viewer draws them at the radius, not at sheet size.
	local images = {}
	for key, file in pairs(RING_IMAGES) do
		if file_exists(file) then images[key] = file end
	end
	return { version = v, assets = as_object(assets), disabled = as_object(disabled),
	         sheets = as_object(sheets), missingSheets = missing, images = as_object(images) }
end

local function node_type_name(node)
	return node and node.type or nil
end

-- PassiveTreeView:PassiveTreeView() image handles, by the field name it uses.
RING_IMAGES = {
	ring = "Assets/ring.png",
	jewelShadedOuterRing = "Assets/ShadedOuterRing.png",
	jewelShadedOuterRingFlipped = "Assets/ShadedOuterRingFlipped.png",
	jewelShadedInnerRing = "Assets/ShadedInnerRing.png",
	jewelShadedInnerRingFlipped = "Assets/ShadedInnerRingFlipped.png",
	eternal1 = "TreeData/PassiveSkillScreenEternalEmpireJewelCircle1.png",
	eternal2 = "TreeData/PassiveSkillScreenEternalEmpireJewelCircle2.png",
	karui1 = "TreeData/PassiveSkillScreenKaruiJewelCircle1.png",
	karui2 = "TreeData/PassiveSkillScreenKaruiJewelCircle2.png",
	maraketh1 = "TreeData/PassiveSkillScreenMarakethJewelCircle1.png",
	maraketh2 = "TreeData/PassiveSkillScreenMarakethJewelCircle2.png",
	templar1 = "TreeData/PassiveSkillScreenTemplarJewelCircle1.png",
	templar2 = "TreeData/PassiveSkillScreenTemplarJewelCircle2.png",
	vaal1 = "TreeData/PassiveSkillScreenVaalJewelCircle1.png",
	vaal2 = "TreeData/PassiveSkillScreenVaalJewelCircle2.png",
	kalguur1 = "TreeData/PassiveSkillScreenKalguuranJewelCircle1.png",
	kalguur2 = "TreeData/PassiveSkillScreenKalguuranJewelCircle2.png",
}

-- drawJewelRadius (PassiveTreeView:Draw) picks the timeless ring pair by the
-- jewel's title; anything else gets the shaded rings.
local TIMELESS_RINGS = {
	{ "^Brutal Restraint", "maraketh" }, { "^Elegant Hubris", "eternal" }, { "^Glorious Vanity", "vaal" },
	{ "^Lethal Pride", "karui" }, { "^Militant Faith", "templar" }, { "^Heroic Tragedy", "kalguur" },
}
local function ring_key(title)
	for _, e in ipairs(TIMELESS_RINGS) do
		if type(title) == "string" and title:match(e[1]) then return e[2] end
	end
	return nil
end
local function abyss_conquered(x)
	local c = x and x.conqueredBy and x.conqueredBy.conqueror
	return c and c.type and type(c.type) == "string" and c.type:match("^abyss_") and true or false
end

-- What the build did to the tree: allocations, per-node overrides (mastery
-- choice, tattoos, timeless jewels), cluster subgraphs, points.
local function node_modes(spec)
	local out = {}
	for id, node in pairs(spec.nodes) do
		if node.alloc and (node.allocMode or 0) > 0 then out[tostring(id)] = node.allocMode end
	end
	return out
end

function M.get_tree_state()
	local b = ensure_build()
	local spec = b.spec
	if not spec then error("no spec", 0) end
	local tree = spec.tree
	local alloc = {}
	for id, node in pairs(spec.nodes) do
		if node.alloc then alloc[#alloc + 1] = id end
	end
	table.sort(alloc)
	local overrides = {}
	for id, node in pairs(spec.nodes) do
		local tnode = tree.nodes[id]
		local why = nil
		if node.alloc and node.type == "Mastery" and spec.masterySelections and spec.masterySelections[id] then
			why = "mastery"
		elseif node.conqueredBy then
			why = "conquered"
		elseif spec.hashOverrides and spec.hashOverrides[id] then
			why = (GAME == "poe2" and node.isAttribute) and "attribute" or "tattoo"
		elseif tnode and node.dn ~= tnode.dn then
			why = "renamed"
		end
		if why then
			local stats = {}
			for i, s in ipairs(node.sd or {}) do stats[i] = s end
			local statsZh = {}
			for i, s in ipairs(stats) do statsZh[i] = tr(s) end
			overrides[tostring(id)] = {
				why = why,
				name = node.dn,
				nameZh = tr(node.dn),
				icon = (node.type == "Mastery" and node.activeIcon) or node.icon,
				effect = node.activeEffectImage,
				stats = stats,
				statsZh = statsZh,
			}
		end
	end
	-- Cluster jewel subgraphs: POB generates and positions these itself
	-- (PassiveSpec:BuildSubgraph -> PassiveTree:ProcessNode). subGraph.nodes is
	-- an array, so the id is the node's own; connectors are POB's BuildConnector
	-- output (arcs included), serialised the way tree_data does the static ones.
	local dynamicNodes, dynamicGroups, dynamicConnectors = {}, {}, {}
	local seenConn = {}
	for sgId, sg in pairs(spec.subGraphs or {}) do
		if sg.group then
			local orbits = {}
			for o in pairs(sg.group.oo or {}) do orbits[#orbits + 1] = o end
			table.sort(orbits)
			dynamicGroups[#dynamicGroups + 1] = {
				id = sgId, x = sg.group.x, y = sg.group.y, orbits = orbits,
				parentSocket = sg.parentSocket and sg.parentSocket.id or nil,
			}
		end
		for _, node in ipairs(sg.nodes or {}) do
			if type(node.id) == "number" and type(node.x) == "number" then
				local links = {}
				for _, other in ipairs(node.linked or {}) do links[#links + 1] = other.id end
				local stats, statsZh = {}, {}
				for i, st in ipairs(node.sd or {}) do stats[i] = st; statsZh[i] = tr(st) end
				dynamicNodes[#dynamicNodes + 1] = {
					id = node.id, name = node.dn, nameZh = tr(node.dn), type = node.type,
					stats = stats, statsZh = statsZh, x = node.x, y = node.y, size = node.size or 0,
					orbit = node.o, icon = node.icon, links = links, group = sgId,
					frames = node_frames(tree, node),
					expansion = node.expansionJewel ~= nil,
					expansionSkill = node.expansionSkill and true or false,
					allocated = node.alloc and true or false,
				}
			end
		end
		for _, c in ipairs(sg.connectors or {}) do
			local a, b = c.nodeId1, c.nodeId2
			if type(a) == "number" and type(b) == "number" then
				local key = (a < b) and (a .. ":" .. b) or (b .. ":" .. a)
				if not seenConn[key] then
					seenConn[key] = true
					local orbit = c.type and tonumber(c.type:match("^Orbit(%d+)$"))
					dynamicConnectors[#dynamicConnectors + 1] = { a = a, b = b, orbit = orbit, group = sgId }
				end
			end
		end
	end
	-- Every jewel socket the spec has (tree sockets + cluster inner sockets),
	-- with what PassiveTreeView:Draw needs for it: the jewel, the overlay art
	-- GetJewelSocketOverlay picks, and the radius drawJewelRadius would ring.
	local sockets = {}
	local viewer = b.treeTab and b.treeTab.viewer
	local itemsTab = b.itemsTab
	for id, node in pairs(spec.nodes) do
		if node.type == "Socket" then
			local e = {
				nodeId = id,
				expansion = node.expansionJewel ~= nil,
				expansionSize = node.expansionJewel and node.expansionJewel.size or nil,
				charm = (node.name == "Charm Socket" or node.dn == "Charm Socket") and true or false,
			}
			if itemsTab and itemsTab.sockets and itemsTab.sockets[id] and itemsTab.GetSocketAndJewelForNodeID then
				local ok, socket, jewel = pcall(itemsTab.GetSocketAndJewelForNodeID, itemsTab, id)
				if ok and jewel then
					e.itemId = socket and socket.selItemId
					e.name = jewel.name
					e.title = jewel.title
					e.baseName = jewel.baseName
					e.nameZh = jewel.title and (tr(jewel.title) .. ", " .. tr(jewel.baseName or "")) or tr(jewel.name or "")
					e.rarity = jewel.rarity
					if viewer and viewer.GetJewelSocketOverlay then
						local ok2, ov = pcall(viewer.GetJewelSocketOverlay, viewer, jewel, node.expansionJewel)
						if ok2 and type(ov) == "string" then e.overlay = ov end
					elseif GAME == "poe2" then
						-- PoE2 PassiveTreeView:Draw names the art inline: a unique's own
						-- title when the tree has art for it, else the base type.
						local hasArt = jewel.rarity == "UNIQUE" and jewel.title and (tree.ddsMap and tree.ddsMap[jewel.title] or tree.assets[jewel.title] or tree.spriteMap and tree.spriteMap[jewel.title])
						e.overlay = hasArt and jewel.title or jewel.baseName
					end
					if jewel.jewelRadiusIndex and not abyss_conquered(jewel.jewelData) then
						e.radiusIndex = jewel.jewelRadiusIndex
					end
					e.radiusLabel = jewel.jewelRadiusLabel
					e.ringKey = ring_key(jewel.title)
				end
			end
			sockets[#sockets + 1] = e
		end
	end
	table.sort(sockets, function(x, y) return x.nodeId < y.nodeId end)
	local radii = {}
	for i, r in ipairs((data and data.jewelRadius) or {}) do
		radii[i] = { inner = r.inner, outer = r.outer, col = r.col, label = r.label }
	end
	local info = M.get_build_info()
	return {
		treeVersion = spec.treeVersion,
		classId = spec.curClassId, className = spec.curClassName,
		ascendClassId = spec.curAscendClassId, ascendClassName = spec.curAscendClassName,
		allocatedNodes = alloc,
		allocCount = #alloc,
		-- PoE2 weapon-set passives: the mode new points go to (0 = main tree,
		-- 1/2 = weapon set) and every allocated node that belongs to a set.
		allocMode = GAME == "poe2" and (spec.allocMode or 0) or nil,
		nodeModes = GAME == "poe2" and as_object(node_modes(spec)) or nil,
		overrides = as_object(overrides),
		dynamicNodes = dynamicNodes,
		dynamicGroups = dynamicGroups,
		dynamicConnectors = dynamicConnectors,
		sockets = sockets,
		jewelRadius = radii,
		points = info.points,
		rev = b.outputRevision,
	}
end

-- What allocating (or removing) this node would touch: POB's own path and
-- dependency lists, recomputed by BuildAllDependsAndPaths after every change.
function M.node_hover(p)
	local b = ensure_build()
	local id = p and tonumber(p.id)
	local node = id and b.spec.nodes[id]
	if not node then error("no node " .. tostring(id), 0) end
	local path, depends = {}, {}
	if node.alloc then
		for _, n in ipairs(node.depends or {}) do depends[#depends + 1] = n.id end
	elseif node.path then
		for _, n in ipairs(node.path) do path[#path + 1] = n.id end
	end
	local cost = node.alloc and 0 or (node.pathDist or 1000)
	if cost >= 1000 then cost = nil end
	return { id = id, allocated = node.alloc and true or false, path = path, depends = depends, cost = cost }
end

-- The tooltip POB draws for a node, as lines. Wraps the viewer's own
-- AddNodeTooltip (returnEarly: no stat-difference pass, which costs two full
-- calculations per hover). AddNodeTooltip sets maxWidth=800 and the tooltip
-- then wraps lines through main:WrapString, which needs real font metrics we
-- do not have headless -- swap it for identity during the call and let the
-- page wrap.
function M.node_info(p)
	local b = ensure_build()
	local id = p and tonumber(p.id)
	local node = id and b.spec.nodes[id]
	if not node then error("no node " .. tostring(id), 0) end
	local viewer = b.treeTab and b.treeTab.viewer
	if not viewer or not viewer.AddNodeTooltip then error("tree viewer not available", 0) end
	local tt = make("Tooltip")
	local savedWrap, savedDiff = main().WrapString, viewer.showStatDifferences
	main().WrapString = function(_, s) return { s } end
	viewer.showStatDifferences = false
	local ok, err = pcall(viewer.AddNodeTooltip, viewer, tt, node, b, true)
	main().WrapString = savedWrap
	viewer.showStatDifferences = savedDiff
	if not ok then error(err, 0) end
	local lines = {}
	for i, l in ipairs(tt.lines or {}) do
		if l.text ~= nil then
			lines[i] = { size = l.size, text = tr(l.text), raw = l.text, center = l.center and true or false, font = l.font }
		else
			lines[i] = { sep = l.size or 0 }
		end
	end
	local effects = nil
	if node.type == "Mastery" and node.masteryEffects then
		effects = {}
		for i, e in ipairs(node.masteryEffects) do
			local stats = {}
			for j, s in ipairs(e.sd or {}) do stats[j] = tr(s) end
			effects[i] = { effect = e.effect, stats = stats }
		end
	end
	local stats, statsZh = {}, {}
	for i, s in ipairs(node.sd or {}) do stats[i] = s; statsZh[i] = tr(s) end
	return {
		id = id, name = node.dn, nameZh = tr(node.dn), type = node_type_name(node),
		allocated = node.alloc and true or false,
		header = tt.tooltipHeader or nil,
		lines = lines,
		stats = stats, statsZh = statsZh,
		masteryEffects = effects,
		masterySelected = b.spec.masterySelections and b.spec.masterySelections[id] or nil,
		pathDist = node.pathDist,
	}
end

-- POB takes the undo base state in PassiveSpec:Load, before PostLoad builds
-- the cluster-jewel subgraphs, so undoing back to the start of the session
-- drops every cluster node (and the sockets inside them) in classic POB too.
-- While nothing has been changed yet the stack is that single stale entry;
-- retaking it from the now-complete spec costs nothing and fixes the base.
local function ensure_undo_base(spec)
	if spec.undo and #spec.undo <= 1 and (not spec.redo or #spec.redo == 0) then
		spec:ResetUndo()
	end
end

-- After any change to the spec: the same three steps PassiveTreeView does
-- after a click, then one POB frame so the calculation and sidebar catch up,
-- then the state the page redraws from.
local function committed(b, spec)
	spec:AddUndoState()
	if spec.SetWindowTitleWithBuildClass then spec:SetWindowTitleWithBuildClass() end
	b.buildFlag = true
	frame()
	return M.get_tree_state()
end

-- The mastery effects POB would list for this node, with the ones another
-- mastery of the same kind already took marked (OpenMasteryPopup's rule).
local function mastery_choices(b, node)
	local list = {}
	for _, e in ipairs(node.masteryEffects or {}) do
		local takenBy = nil
		for nodeId, effectId in pairs(b.spec.masterySelections or {}) do
			if effectId == e.effect and nodeId ~= node.id then takenBy = nodeId end
		end
		local stats, statsZh = {}, {}
		for i, s in ipairs(e.sd or e.stats or {}) do stats[i] = s; statsZh[i] = tr(s) end
		list[#list + 1] = { effect = e.effect, stats = stats, statsZh = statsZh, takenBy = takenBy }
	end
	return list
end

-- A left click on a node, exactly as PassiveTreeView.lua:391-518 handles it.
-- Allocated -> DeallocNode. Ascendancy nodes may switch ascendancy or class
-- (a cross-class switch that would reset the tree comes back as
-- needsConfirm so the page can ask; `confirm` = "reset" | "connect" answers
-- it). A mastery with effects comes back as needsMastery unless `effect` is
-- given. Everything else -> AllocNode along POB's own path.
function M.tree_click(p)
	local b = ensure_build()
	local spec = b.spec
	ensure_undo_base(spec)
	local id = p and tonumber(p.id)
	local node = id and spec.nodes[id]
	if not node then error("no node " .. tostring(id), 0) end

	if GAME == "poe2" then
		-- PassiveTreeView:Draw's click rules for keystones/sockets in
		-- weapon-set mode (its local shouldBlockGlobalNode* helpers): they stay
		-- on the main tree.
		local global = node.type == "Keystone" or node.type == "Socket" or node.containJewelSocket
		local viewer = b.treeTab and b.treeTab.viewer
		if global and node.alloc and (node.allocMode or 0) == 0 and (spec.allocMode or 0) > 0 then
			return { blocked = "weapon_set_global", id = id }
		end
		if global and not node.alloc and node.path and ((spec.allocMode or 0) > 0
			or (viewer and viewer.IsConnectedToWeaponSetNodes and viewer:IsConnectedToWeaponSetNodes(node))) then
			return { blocked = "weapon_set_global", id = id }
		end
		if node.alloc and node.isAttribute then
			-- a plain click resets the chosen attribute and deallocates
			spec.hashOverrides[id] = nil
			spec:DeallocNode(node)
			return committed(b, spec)
		end
	end

	if node.alloc then
		spec:DeallocNode(node)
		return committed(b, spec)
	end

	if node.ascendancyName then
		local tree = spec.tree
		if node.isBloodline and tree.alternate_ascendancies then
			local different = not spec.curSecondaryAscendClass or node.ascendancyName ~= spec.curSecondaryAscendClass.id
			if different then
				for bloodlineId, data in pairs(tree.alternate_ascendancies) do
					if data.id == node.ascendancyName then
						spec:SelectSecondaryAscendClass(bloodlineId)
						break
					end
				end
			end
		else
			local differentAsc = false
			if spec.curAscendClassId == 0 or node.ascendancyName ~= spec.curAscendClassBaseName then
				if not (spec.curSecondaryAscendClass and node.ascendancyName == spec.curSecondaryAscendClass.id) then
					differentAsc = true
				end
			end
			if differentAsc then
				local targetAscId
				-- (PoE2: an ascendancy that replaces another counts as that one)
				if spec.curAscendClass and spec.curAscendClass.replace and node.ascendancyName == spec.curAscendClass.replace then
					targetAscId = spec.curAscendClassId
				end
				for ascId, asc in pairs(targetAscId and {} or spec.curClass.classes) do
					if asc.id == node.ascendancyName then targetAscId = ascId break end
				end
				if targetAscId then
					spec:SelectAscendClass(targetAscId)
				else
					local targetClassId, targetClass
					for classId, classData in pairs(tree.classes) do
						for ascId, asc in pairs(classData.classes or {}) do
							if asc.id == node.ascendancyName then
								targetClassId, targetClass, targetAscId = classId, classData, ascId
								break
							end
						end
						if targetClassId then break end
					end
					if targetClassId then
						local used = spec:CountAllocNodes()
						local confirm = p.confirm
						if used == 0 or spec:IsClassConnected(targetClassId) or confirm == "reset" then
							spec:SelectClass(targetClassId)
							spec:SelectAscendClass(targetAscId)
						elseif confirm == "connect" then
							if spec:ConnectToClass(targetClassId) then
								spec:SelectClass(targetClassId)
								spec:SelectAscendClass(targetAscId)
							else
								return { needsConfirm = "class_change", id = id, className = targetClass.name,
								         classNameZh = tr(targetClass.name), connectFailed = true }
							end
						else
							return { needsConfirm = "class_change", id = id, className = targetClass.name,
							         classNameZh = tr(targetClass.name), ascendClassName = node.ascendancyName }
						end
					end
				end
			end
		end
		-- fall through: allocate the clicked node in its (now current) ascendancy
	end

	node = spec.nodes[id]
	if node and node.path and not node.alloc then
		if node.type == "Mastery" and node.masteryEffects then
			if p.effect then
				return M.select_mastery({ id = id, effect = p.effect })
			end
			return { needsMastery = true, id = id, name = node.dn, nameZh = tr(node.dn),
			         effects = mastery_choices(b, node), selected = spec.masterySelections and spec.masterySelections[id] or nil }
		end
		if GAME == "poe2" and node.isAttribute then
			-- TreeTab:ModifyAttributePopup: pick Strength/Dexterity/Intelligence,
			-- then SwitchAttributeNode + AllocNode.
			local idx = p.attribute and tonumber(p.attribute)
			if not idx then
				local options = {}
				for i, name in ipairs({ "Strength", "Dexterity", "Intelligence" }) do
					options[i] = { index = i, name = name, nameZh = tr(name) }
				end
				return { needsAttribute = true, id = id, options = options, last = spec.attributeIndex }
			end
			spec:SwitchAttributeNode(id, idx)
			spec.attributeIndex = idx
			node = spec.nodes[id]
		end
		spec:AllocNode(node)
	end
	return committed(b, spec)
end

-- tree_attribute{id, attribute=1..3}: PoE2's right-click / hotkey switch of an
-- allocated attribute node (PassiveTreeView:Draw, processAttributeHotkeys):
-- SwitchAttributeNode, then the paths and dependencies rebuilt.
function M.tree_attribute(p)
	if GAME ~= "poe2" then error("attribute nodes are PoE2 only", 0) end
	local b = ensure_build()
	local spec = b.spec
	ensure_undo_base(spec)
	local id = p and tonumber(p.id)
	local idx = p and tonumber(p.attribute)
	local node = id and spec.nodes[id]
	if not node or not node.isAttribute then error("not an attribute node: " .. tostring(id), 0) end
	if not idx or idx < 1 or idx > 3 then error("attribute must be 1..3", 0) end
	spec.attributeIndex = idx
	spec:SwitchAttributeNode(id, idx)
	if spec.nodes[id].alloc then
		spec:BuildAllDependsAndPaths()
	else
		spec:AllocNode(spec.nodes[id])
	end
	return committed(b, spec)
end

-- set_alloc_mode{mode=0|1|2}: PoE2's weapon-set allocation mode (the tree
-- tab's weapon set toggle; Build.lua cycles spec.allocMode).
function M.set_alloc_mode(p)
	if GAME ~= "poe2" then error("weapon-set allocation is PoE2 only", 0) end
	local b = ensure_build()
	local mode = p and tonumber(p.mode)
	if not mode or mode < 0 or mode > 2 then error("mode must be 0, 1 or 2", 0) end
	b.spec.allocMode = math.floor(mode)
	b.spec:BuildAllDependsAndPaths()
	frame()
	return M.get_tree_state()
end

-- Choosing a mastery effect: TreeTab:SaveMasteryPopup does the whole
-- sequence (stats swap, ProcessStats, masterySelections, AllocNode,
-- AddUndoState); it only wants a list control with a selection and closes a
-- popup we never opened, so hand it a stand-in and mute ClosePopup.
function M.select_mastery(p)
	local b = ensure_build()
	local spec = b.spec
	ensure_undo_base(spec)
	local id = p and tonumber(p.id)
	local effect = p and tonumber(p.effect)
	local node = id and spec.nodes[id]
	if not node then error("no node " .. tostring(id), 0) end
	if not effect or not spec.tree.masteryEffects[effect] then error("no mastery effect " .. tostring(effect), 0) end
	local m = main()
	local savedClose = m.ClosePopup
	m.ClosePopup = function() end
	local ok, err = pcall(b.treeTab.SaveMasteryPopup, b.treeTab, node, { selValue = { id = effect } })
	m.ClosePopup = savedClose
	if not ok then error(err, 0) end
	b.buildFlag = true
	frame()
	return M.get_tree_state()
end

-- Undo/redo restore through ImportFromNodeList, which parks cluster-jewel
-- node ids whose subgraph is currently gone (the dealloc that disconnected
-- the socket removed it) in allocSubgraphNodes and leaves them there until
-- the next BuildClusterJewelGraphs. Classic POB shows the cluster nodes as
-- lost until a jewel is touched; we finish the restore right away.
local function after_undo(b)
	local spec = b.spec
	if spec.allocSubgraphNodes and #spec.allocSubgraphNodes > 0 then
		spec:BuildClusterJewelGraphs()
	end
	b.buildFlag = true
	frame()
	return M.get_tree_state()
end

function M.tree_undo()
	local b = ensure_build()
	ensure_undo_base(b.spec)
	b.spec:Undo()
	return after_undo(b)
end

function M.tree_redo()
	local b = ensure_build()
	ensure_undo_base(b.spec)
	b.spec:Redo()
	return after_undo(b)
end

probe("main.LoadTree", function() return type(launch.main.LoadTree) == "function" end)
probe("classes.TreeTab.SaveMasteryPopup/OpenMasteryPopup", function()
	local c = class_of("TreeTab")
	return type(c) == "table" and type(c.SaveMasteryPopup) == "function" and type(c.OpenMasteryPopup) == "function"
end)
probe("classes.PassiveSpec class switching", function()
	local c = class_of("PassiveSpec")
	return type(c) == "table" and type(c.SelectClass) == "function" and type(c.SelectAscendClass) == "function"
		and type(c.IsClassConnected) == "function" and type(c.ConnectToClass) == "function"
end)
probe("classes.PassiveTree.ProcessNode", function() local c = class_of("PassiveTree"); return type(c) == "table" and type(c.ProcessNode) == "function" end)
-- beta adds returnEarly; master (tooltip, node, build) ignores it and skips
-- its stat-difference pass on showStatDifferences=false, which node_info sets.
-- PoE2's tree surfaces (attribute nodes, weapon-set allocation). Required on
-- PoE2 -- tree_click/tree_attribute/set_alloc_mode call them -- and not
-- applicable (true) on PoE1, which has neither.
probe("classes.PassiveSpec.SwitchAttributeNode/BuildAllDependsAndPaths + PassiveTreeView.IsConnectedToWeaponSetNodes (PoE2)", function()
	if GAME ~= "poe2" then return true end
	local s, v = class_of("PassiveSpec"), class_of("PassiveTreeView")
	return type(s) == "table" and type(s.SwitchAttributeNode) == "function" and type(s.BuildAllDependsAndPaths) == "function"
		and type(v) == "table" and type(v.IsConnectedToWeaponSetNodes) == "function"
end)
probe("classes.PassiveTreeView.AddNodeTooltip(tooltip, node, build[, returnEarly])", function()
	local c = class_of("PassiveTreeView")
	local n = type(c) == "table" and type(c.AddNodeTooltip) == "function" and nparams(c.AddNodeTooltip)
	return n == 4 or n == 5
end)
probe("classes.UndoHandler.AddUndoState/Undo/Redo", function()
	local c = class_of("UndoHandler")
	return type(c) == "table" and type(c.AddUndoState) == "function" and type(c.Undo) == "function" and type(c.Redo) == "function"
end)
probe("classes.Tooltip.AddLine", function() local c = class_of("Tooltip"); return type(c) == "table" and type(c.AddLine) == "function" end)
probe("latestTreeVersion", function() return type(latestTreeVersion) == "string" end)
probe("classes.PassiveTreeView.GetJewelSocketOverlay", function()
	local c = class_of("PassiveTreeView")
	return type(c) == "table" and type(c.GetJewelSocketOverlay) == "function"
end)
probe("classes.ItemsTab.GetSocketAndJewelForNodeID", function()
	local c = class_of("ItemsTab")
	return type(c) == "table" and type(c.GetSocketAndJewelForNodeID) == "function"
end)
probe("data.jewelRadius {inner,outer,col,label}", function()
	local r = type(data) == "table" and data.jewelRadius
	return type(r) == "table" and type(r[1]) == "table" and type(r[1].outer) == "number" and type(r[1].col) == "string"
end)
probe("classes.PassiveSpec.BuildClusterJewelGraphs + subGraphs shape", function()
	local c = class_of("PassiveSpec")
	return type(c) == "table" and type(c.BuildClusterJewelGraphs) == "function" and type(c.BuildSubgraph) == "function"
end)

-- ---------------------------------------------------------------------------
-- Class / ascendancy switching (Build.lua's classDrop / ascendDrop /
-- secondaryAscendDrop callbacks, :259-300)
-- ---------------------------------------------------------------------------

-- The lists those dropdowns are built from (UpdateClassDropdowns :1553 and
-- UpdateSecondaryAscendancyDropdown :1113), ids being what Select* takes:
-- the class id, the index into class.classes (0 = None), and the key of
-- tree.alternate_ascendancies (0 = None).
local function class_lists(b)
	local spec = b.spec
	local tree = spec.tree
	local classes = {}
	for cid, class in pairs(tree.classes) do
		if type(cid) == "number" and type(class) == "table" then
			local ascs = {}
			for i = 0, #(class.classes or {}) do
				local a = class.classes[i]
				if a then ascs[#ascs + 1] = { id = i, name = a.name, nameZh = tr(a.name) } end
			end
			classes[#classes + 1] = { id = cid, name = class.name, nameZh = tr(class.name), ascendancies = ascs }
		end
	end
	table.sort(classes, function(x, y) return x.name < y.name end)
	local legacy = { Warden = true, Warlock = true, Primalist = true }
	local sel = spec.curSecondaryAscendClassId or 0
	local secondary = { { id = 0, name = "None", nameZh = tr("None") } }
	local sortable = {}
	for ascId, a in pairs(tree.alternate_ascendancies or {}) do
		if type(a) == "table" and a.id and (not legacy[a.id] or ascId == sel) then
			sortable[#sortable + 1] = { id = ascId, name = a.name, nameZh = tr(a.name) }
		end
	end
	table.sort(sortable, function(x, y) return x.name < y.name end)
	for _, e in ipairs(sortable) do secondary[#secondary + 1] = e end
	return classes, secondary
end

local function class_current(spec)
	return {
		classId = spec.curClassId, className = spec.curClassName, classNameZh = tr(spec.curClassName),
		ascendClassId = spec.curAscendClassId, ascendClassName = spec.curAscendClassName,
		secondaryAscendClassId = spec.curSecondaryAscendClassId or 0,
	}
end

function M.list_classes()
	local b = ensure_build()
	local classes, secondary = class_lists(b)
	return { classes = classes, secondary = secondary, current = class_current(b.spec) }
end

-- set_class{classId, confirm?="reset"|"connect"}: classDrop's callback. When
-- the tree has points and the new class is not connected, POB asks
-- (Continue = reset the tree, Connect Path = ConnectToClass); the page asks
-- instead and calls again with confirm.
function M.set_class(p)
	local b = ensure_build()
	local spec = b.spec
	local classId = p and tonumber(p.classId)
	local class = classId and spec.tree.classes[classId]
	if not class then error("bad classId " .. tostring(p and p.classId), 0) end
	if classId == spec.curClassId then return M.get_tree_state() end
	local confirm = p.confirm
	ensure_undo_base(spec)
	if spec:CountAllocNodes() == 0 or spec:IsClassConnected(classId) or confirm == "reset" then
		spec:SelectClass(classId)
	elseif confirm == "connect" then
		if not spec:ConnectToClass(classId) then
			return { needsConfirm = "class_change", classId = classId, className = class.name, classNameZh = tr(class.name), connectFailed = true }
		end
		spec:SelectClass(classId)
	else
		return { needsConfirm = "class_change", classId = classId, className = class.name, classNameZh = tr(class.name) }
	end
	return committed(b, spec)
end

-- set_ascendancy{ascendClassId}: ascendDrop's callback (index into class.classes, 0 = None).
function M.set_ascendancy(p)
	local b = ensure_build()
	local spec = b.spec
	local id = p and tonumber(p.ascendClassId)
	local class = spec.tree.classes[spec.curClassId]
	if not id or id < 0 or not class or (id > 0 and not class.classes[id]) then error("bad ascendClassId " .. tostring(p and p.ascendClassId), 0) end
	if id == spec.curAscendClassId then return M.get_tree_state() end
	ensure_undo_base(spec)
	spec:SelectAscendClass(id)
	return committed(b, spec)
end

-- set_secondary_ascendancy{ascendClassId}: secondaryAscendDrop's callback (key of tree.alternate_ascendancies, 0 = None).
function M.set_secondary_ascendancy(p)
	local b = ensure_build()
	local spec = b.spec
	if type(spec.SelectSecondaryAscendClass) ~= "function" then error("this POB has no secondary ascendancies", 0) end
	local id = p and tonumber(p.ascendClassId)
	if not id or id < 0 or (id > 0 and not (spec.tree.alternate_ascendancies or {})[id]) then error("bad ascendClassId " .. tostring(p and p.ascendClassId), 0) end
	if id == (spec.curSecondaryAscendClassId or 0) then return M.get_tree_state() end
	ensure_undo_base(spec)
	spec:SelectSecondaryAscendClass(id)
	return committed(b, spec)
end

probe("classes.PassiveSpec.SelectSecondaryAscendClass (optional)", function()
	local c = class_of("PassiveSpec")
	return type(c) == "table" and (c.SelectSecondaryAscendClass == nil or type(c.SelectSecondaryAscendClass) == "function")
end)
probe("tree.classes[id].classes[0..n] + alternate_ascendancies", function()
	local t = launch.main.tree and launch.main.tree[latestTreeVersion]
	if not t then t = launch.main:LoadTree(latestTreeVersion) end
	-- PoE1 numbers classes from 0, PoE2 by the tree's integerId; either way
	-- every class carries its ascendancies with "None" at index 0.
	local _, c = next(t and t.classes or {})
	return type(c) == "table" and type(c.classes) == "table" and type(c.classes[0]) == "table" and type(c.classes[0].name) == "string"
end)

-- ---------------------------------------------------------------------------
-- Build header (top bar), save, import / export
-- ---------------------------------------------------------------------------

-- Every mutation on the build ends the way POB's own control callbacks do:
-- modFlag + buildFlag, then one frame so mainOutput and the sidebar catch up.
local function commit(b)
	b.modFlag = true
	b.buildFlag = true
	frame()
	return { rev = b.outputRevision, unsaved = b.unsaved and true or false }
end

-- A DropDownControl's list as {val,label,labelZh}; entries may be plain
-- strings (mainSkillMinionSkill) or tables with val/label/minionId/itemSetId.
local function dd_entries(ctl)
	local out = {}
	for i, e in ipairs(ctl.list or {}) do
		if type(e) == "table" then
			out[i] = { val = e.val, label = e.label, labelZh = tr(e.label), minionId = e.minionId, itemSetId = e.itemSetId }
		else
			out[i] = { val = i, label = e, labelZh = tr(e) }
		end
	end
	return out
end

local function dd_shown(ctl)
	if not ctl then return false end
	local s = ctl.shown
	if type(s) == "function" then return s(ctl) and true or false end
	return s ~= false
end

-- The top bar as POB lays it out: level (+ auto), the main-skill selectors
-- POB itself fills through RefreshSkillSelectControls, and the save state.
function M.get_build_header()
	local b = ensure_build()
	local c = b.controls
	b:RefreshSkillSelectControls(c, b.mainSocketGroup, "")
	local classes, secondary = class_lists(b)
	local h = {
		buildName = b.buildName,
		dbFileName = b.dbFileName or nil,
		classes = classes, secondaryAscendancies = secondary, classPick = class_current(b.spec),
		unsaved = b.unsaved and true or false,
		level = b.characterLevel,
		levelAuto = b.characterLevelAutoMode and true or false,
		mainSocketGroup = { index = c.mainSocketGroup.selIndex or 1, list = dd_entries(c.mainSocketGroup) },
		rev = b.outputRevision,
	}
	if dd_shown(c.mainSkill) then
		h.mainSkill = { index = c.mainSkill.selIndex or 1, list = dd_entries(c.mainSkill), enabled = c.mainSkill.enabled ~= false }
	end
	if dd_shown(c.mainSkillPart) then
		h.mainSkillPart = { index = c.mainSkillPart.selIndex or 1, list = dd_entries(c.mainSkillPart) }
	end
	if dd_shown(c.mainSkillStageCount) then h.mainSkillStageCount = tonumber(c.mainSkillStageCount.buf) end
	if dd_shown(c.mainSkillMineCount) then h.mainSkillMineCount = tonumber(c.mainSkillMineCount.buf) or 0 end
	if dd_shown(c.mainSkillMinion) then
		h.mainSkillMinion = { index = c.mainSkillMinion.selIndex or 1, list = dd_entries(c.mainSkillMinion), enabled = c.mainSkillMinion.enabled ~= false }
	end
	if dd_shown(c.mainSkillMinionSkill) then
		h.mainSkillMinionSkill = { index = c.mainSkillMinionSkill.selIndex or 1, list = dd_entries(c.mainSkillMinionSkill) }
	end
	return h
end

-- The active skill instance the main-skill selectors edit (Build.lua's
-- callbacks all start from these three lines).
local function main_src_instance(b)
	local g = b.skillsTab.socketGroupList[b.mainSocketGroup]
	local as = g and g.displaySkillList and g.displaySkillList[g.mainActiveSkill or 1]
	return as and as.activeEffect and as.activeEffect.srcInstance, g
end

-- set_build_field{field, value}: one top-bar control's callback, verbatim.
function M.set_build_field(p)
	local b = ensure_build()
	local c = b.controls
	local field = p and p.field
	local v = p and p.value
	if field == "level" then
		local lv = math.min(math.max(tonumber(v) or 1, 1), 100)
		b.characterLevel = lv
		b.configTab:BuildModList()
		b.characterLevelAutoMode = false
		c.levelScalingButton.label = "Manual"
		c.characterLevel:SetText(tostring(lv))
	elseif field == "levelAuto" then
		b.characterLevelAutoMode = v and true or false
		c.levelScalingButton.label = b.characterLevelAutoMode and "Auto" or "Manual"
		b.configTab:BuildModList()
	elseif field == "mainSocketGroup" then
		local idx = tonumber(v)
		if not idx or not b.skillsTab.socketGroupList[idx] then error("no socket group " .. tostring(v), 0) end
		b.mainSocketGroup = idx
	elseif field == "mainSkill" then
		local g = b.skillsTab.socketGroupList[b.mainSocketGroup]
		if not g then error("no main socket group", 0) end
		g.mainActiveSkill = tonumber(v) or 1
	elseif field == "mainSkillPart" then
		local src = main_src_instance(b)
		if not src then error("no active skill", 0) end
		src.skillPart = tonumber(v) or 1
	elseif field == "mainSkillStageCount" then
		local src = main_src_instance(b)
		if not src then error("no active skill", 0) end
		src.skillStageCount = tonumber(v)
	elseif field == "mainSkillMineCount" then
		local src = main_src_instance(b)
		if not src then error("no active skill", 0) end
		src.skillMineCount = tonumber(v)
	elseif field == "mainSkillMinion" then
		local src = main_src_instance(b)
		if not src then error("no active skill", 0) end
		b:RefreshSkillSelectControls(c, b.mainSocketGroup, "")
		local e = c.mainSkillMinion.list[tonumber(v) or 0]
		if type(e) ~= "table" then error("no minion entry " .. tostring(v), 0) end
		if e.itemSetId then src.skillMinionItemSet = e.itemSetId else src.skillMinion = e.minionId end
	elseif field == "mainSkillMinionSkill" then
		local src = main_src_instance(b)
		if not src then error("no active skill", 0) end
		src.skillMinionSkill = tonumber(v) or 1
	else
		error("unknown field " .. tostring(field), 0)
	end
	return commit(b)
end

-- --- save -------------------------------------------------------------------

local function norm_path(s)
	return (tostring(s or ""):gsub("\\", "/"):lower())
end

local function under_build_path(path)
	local root = norm_path(main().buildPath)
	if root == "" then return false end
	if root:sub(-1) ~= "/" then root = root .. "/" end
	return norm_path(path):sub(1, #root) == root
end

-- Writes the build where it came from. SaveDBFile answers true on failure
-- (it opens a popup nobody sees here), nil on success.
function M.save_build()
	local b = ensure_build()
	if not b.dbFileName then error("build has no file yet; use save_build_as", 0) end
	local failed = b:SaveDBFile()
	if failed then error("save failed: " .. tostring(b.dbFileName), 0) end
	frame()
	return { dbFileName = b.dbFileName, buildName = b.buildName, unsaved = b.unsaved and true or false, rev = b.outputRevision }
end

-- save_build_as{path}: the same bookkeeping OpenSaveAsPopup does before it
-- calls SaveDBFile (name, sub-folder), restricted to POB's build folder.
function M.save_build_as(p)
	local b = ensure_build()
	local path = p and p.path
	if type(path) ~= "string" or path == "" then error("params.path required", 0) end
	path = path:gsub("/", "\\")
	if path:sub(-4):lower() ~= ".xml" then path = path .. ".xml" end
	if not under_build_path(path) then
		error("path must be under the build folder: " .. tostring(main().buildPath), 0)
	end
	local name = path:match("([^\\]+)%.xml$")
	if not name or name:find("[\\/:%*%?\"<>|%c]") then error("bad build name", 0) end
	local m = main()
	local dir = path:match("^(.*)\\[^\\]+$")
	if dir then MakeDir(dir) end
	b.dbFileName = path
	b.buildName = name
	b.dbFileSubPath = path:sub(#m.buildPath + 1, -#name - 5)
	local failed = b:SaveDBFile()
	if failed then error("save failed: " .. path, 0) end
	if b.spec and b.spec.SetWindowTitleWithBuildClass then b.spec:SetWindowTitleWithBuildClass() end
	frame()
	return { dbFileName = b.dbFileName, buildName = b.buildName, subPath = b.dbFileSubPath, unsaved = b.unsaved and true or false, rev = b.outputRevision }
end

-- Reloads the file on disk, throwing away unsaved changes.
function M.revert_build()
	local b = ensure_build()
	if not b.dbFileName then error("build has no file to revert to", 0) end
	return M.load_build_file({ path = b.dbFileName })
end

-- --- share codes ------------------------------------------------------------

-- ImportTab's "Generate" button, one line.
function M.export_code()
	local b = ensure_build()
	local xml = b:SaveDB("code")
	if not xml then error("SaveDB failed", 0) end
	local code = common.base64.encode(Deflate(xml)):gsub("+", "-"):gsub("/", "_")
	return { code = code, bytes = #code }
end

local MAX_CODE = 4 * 1024 * 1024

local function decode_share_code(code)
	if type(code) ~= "string" then error("params.code required", 0) end
	code = code:gsub("^[%s?]+", ""):gsub("[%s?]+$", "")
	if #code == 0 then error("empty code", 0) end
	if #code > MAX_CODE then error("code too large", 0) end
	local ok, xml = pcall(function()
		return Inflate(common.base64.decode(code:gsub("-", "+"):gsub("_", "/")))
	end)
	if not ok or type(xml) ~= "string" or #xml == 0 then error("not a Path of Building code", 0) end
	local doc, err = common.xml.ParseXML(xml)
	if not doc then error("code did not decode to a build: " .. tostring(err), 0) end
	local root = doc[1]
	-- Build.lua LoadDB's root: PathOfBuilding (PoE1) / PathOfBuilding2 (PoE2)
	local rootElem = GAME == "poe2" and "PathOfBuilding2" or "PathOfBuilding"
	if type(root) ~= "table" or root.elem ~= rootElem then error("code is not a build file for this game", 0) end
	return xml, root
end

-- decode_code{code}: what the page shows before the user commits to importing.
function M.decode_code(p)
	local xml, root = decode_share_code(p and p.code)
	local info = { sections = {} }
	for _, node in ipairs(root) do
		if type(node) == "table" and node.elem then
			info.sections[#info.sections + 1] = node.elem
			if node.elem == "Build" then
				local a = node.attrib or {}
				info.className = a.className
				info.classNameZh = tr(a.className)
				info.ascendClassName = a.ascendClassName
				info.ascendClassNameZh = tr(a.ascendClassName)
				info.level = tonumber(a.level)
				info.targetVersion = a.targetVersion
			elseif node.elem == "Items" then
				local n = 0
				for _, c in ipairs(node) do if type(c) == "table" and c.elem == "Item" then n = n + 1 end end
				info.itemCount = n
			elseif node.elem == "Skills" then
				local n = 0
				for _, c in ipairs(node) do
					if type(c) == "table" and (c.elem == "Skill" or c.elem == "SkillSet") then n = n + 1 end
				end
				info.skillCount = n
			elseif node.elem == "Tree" then
				info.hasTree = true
			end
		end
	end
	info.xmlBytes = #xml
	return info
end

-- After Build:Init rebuilt everything, bring the page's world back like a
-- fresh load: the sidebar wrapper, two frames, the same shape load_build_file
-- returns.
local function after_reinit()
	wrap_add_display_stat_list()
	frame()
	frame()
	local b = ensure_build()
	return {
		buildName = b.buildName,
		dbFileName = b.dbFileName or nil,
		outputRevision = b.outputRevision,
		className = b.spec and b.spec.curClassName,
		ascendClassName = b.spec and b.spec.curAscendClassName,
		level = b.characterLevel,
	}
end

-- import_code{code, mode="replace"|"new"}: ImportTab's importSelectedBuild
-- without its confirm popup (the page asks). "replace" keeps this build's
-- file and name; "new" is an unsaved "Imported build".
function M.import_code(p)
	local b = ensure_build()
	local xml = decode_share_code(p and p.code)
	local mode = p and p.mode or "new"
	b:Shutdown()
	if mode == "replace" then
		b:Init(b.dbFileName, b.buildName, xml, false)
	else
		b:Init(false, "Imported build", xml, false)
	end
	b.viewMode = "TREE"
	return after_reinit()
end

-- import_character{items=<get-items JSON text>, passives=<get-passive-skills
-- JSON text>, importTree, importItems, deleteJewels, clearItems, clearSkills,
-- ignoreWeaponSwap}: the two site-import buttons, fed pasted responses
-- instead of a download. charData is assembled the way ImportTab's callbacks
-- assemble it (character block + passives + jewels / equipment + guardian).
function M.import_character(p)
	local b = ensure_build()
	local dkjson = require("dkjson")
	local itemsJson, passivesJson
	if p.items and p.items ~= "" then
		local v, _, err = dkjson.decode(p.items)
		if not v then error("items JSON: " .. tostring(err), 0) end
		itemsJson = v
	end
	if p.passives and p.passives ~= "" then
		local v, _, err = dkjson.decode(p.passives)
		if not v then error("passives JSON: " .. tostring(err), 0) end
		passivesJson = v
	end
	local charData = copyTable((itemsJson and itemsJson.character) or (passivesJson and passivesJson.character) or {})
	charData.class = charData.class or (b.spec and b.spec.curClassName)
	charData.level = charData.level or b.characterLevel
	charData.name = charData.name or b.buildName
	charData.league = charData.league or ""
	local did = {}
	if p.importTree ~= false and passivesJson then
		if not passivesJson.hashes then error("passives JSON has no hashes", 0) end
		charData.passives = passivesJson
		charData.jewels = passivesJson.items
		b.importTab:ImportPassiveTreeAndJewels(charData, p.deleteJewels and true or false)
		did[#did + 1] = "tree"
	end
	if p.importItems ~= false and itemsJson then
		if not itemsJson.items then error("items JSON has no items", 0) end
		charData.equipment = itemsJson.items
		charData.guardian = itemsJson.guardian
		b.importTab:ImportItemsAndSkills(charData, p.clearItems and true or false, p.clearSkills and true or false, p.ignoreWeaponSwap and true or false)
		did[#did + 1] = "items"
	end
	if #did == 0 then error("nothing to import: give items and/or passives JSON", 0) end
	local r = commit(b)
	r.imported = did
	return r
end

-- ---------------------------------------------------------------------------
-- Account import (ImportTab.lua:24-471): the OAuth flow (PoEAPI + the
-- LaunchServer.lua loopback subscript) and the account-name flow
-- (character-window/get-*). Both download through POB's subscripts, whose
-- callbacks land on later engine frames, so every starter returns at once
-- and the page polls import_status for the outcome.
-- ---------------------------------------------------------------------------

-- Headless there is no window to bring forward, and the URL the OAuth
-- subscript opens is worth showing on the page (OnSubCall resolves these
-- globals by name at call time, so wrapping them here is enough).
if PobToolsHeadless and PobToolsHeadless() then
	SetForeground = function() end
	local origOpenURL = OpenURL
	OpenURL = function(url)
		M._lastOpenedUrl = url
		if type(origOpenURL) == "function" then return origOpenURL(url) end
	end
end

local function import_tab(b)
	local tab = b.importTab
	if not tab or not tab.controls then error("import tab not available", 0) end
	return tab
end

-- Every ImportPassiveTreeAndJewels / ImportItemsAndSkills that POB's own
-- download callbacks run is counted on the instance; import_status reports
-- the count and recalculates once it has moved.
local function hook_import_counters(tab)
	if tab._pobtoolsHooked then return end
	tab._pobtoolsHooked = true
	tab._pobtoolsImported = 0
	tab._pobtoolsLastImport = nil
	local origTree, origItems = tab.ImportPassiveTreeAndJewels, tab.ImportItemsAndSkills
	tab.ImportPassiveTreeAndJewels = function(self, ...)
		local r = origTree(self, ...)
		self._pobtoolsImported = self._pobtoolsImported + 1
		self._pobtoolsLastImport = "tree"
		return r
	end
	tab.ImportItemsAndSkills = function(self, ...)
		local r = origItems(self, ...)
		self._pobtoolsImported = self._pobtoolsImported + 1
		self._pobtoolsLastImport = "items"
		return r
	end
end

-- realmList is file-local in ImportTab.lua; the realm dropdown holds it.
local function realm_entries(tab)
	local dd = tab.controls.accountRealm or tab.controls.siteAccountRealm
	return (dd and dd.list) or {}
end
local function realm_by_id(tab, id)
	for _, r in ipairs(realm_entries(tab)) do
		if r.id == id or r.realmCode == id then return r end
	end
	error("unknown realm " .. tostring(id), 0)
end

-- The character list entries as the page shows them (BuildCharacterList's fields).
local function char_summaries(list, realmCode)
	local out = {}
	for _, c in ipairs(list or {}) do
		if type(c) == "table" and (not realmCode or c.realm == nil or c.realm == realmCode) then
			out[#out + 1] = {
				name = c.name, league = c.league, class = c.class, classZh = c.class and tr(c.class) or nil,
				level = c.level, realm = c.realm,
			}
		end
	end
	return out
end

function M.import_status()
	local b = ensure_build()
	local tab = import_tab(b)
	hook_import_counters(tab)
	local m = main()
	local api = m.api
	local realms = {}
	for _, r in ipairs(realm_entries(tab)) do
		realms[#realms + 1] = { id = r.id, label = r.label, realmCode = r.realmCode }
	end
	local characters = {}
	for code, list in pairs(tab.characterList or {}) do characters[code] = char_summaries(list, code) end
	local history = {}
	for name in pairs(m.gameAccounts or {}) do history[#history + 1] = name end
	table.sort(history, function(x, y) return x:lower() < y:lower() end)
	local site = tab.controls.siteAccountName
	local imported = tab._pobtoolsImported or 0
	local recalculated = false
	if imported ~= (tab._pobtoolsReported or 0) then
		tab._pobtoolsReported = imported
		commit(b)
		recalculated = true
	end
	return {
		authorized = (api and api.authToken ~= nil) and true or false,
		oauth = {
			loading = tab.oauthLoading and true or false,
			errCode = tab.oauthErrCode,
			timer = tab.oauthTimer,
			rateLimitEnd = tab.rateLimitEndTime,
			now = os.time(),
			url = M._lastOpenedUrl,
		},
		site = {
			mode = tab.charImportMode,
			status = tab.charImportStatus,
			statusZh = tab.charImportStatus and tr(tab.charImportStatus) or nil,
			accountName = site and site.buf or nil,
			characters = char_summaries(tab.lastCharList, nil),
		},
		realms = realms,
		lastRealm = m.lastRealm,
		lastLeague = m.lastLeague,
		characters = as_object(characters),
		lastAccountName = m.lastAccountName,
		accountHistory = history,
		hasPoints = b.spec and b.spec:CountAllocNodes() > 0 or false,
		imported = imported,
		lastImport = tab._pobtoolsLastImport,
		recalculated = recalculated,
		rev = b.outputRevision,
	}
end

-- The "Authorize with Path of Exile" button (:164-183): PoEAPI:FetchAuthToken
-- starts the loopback server subscript, which opens the browser and copies
-- the URL. The 60 s timer field is what the status label counts down.
function M.oauth_start()
	local b = ensure_build()
	local tab = import_tab(b)
	local api = main().api
	if not api then error("PoE API not available", 0) end
	if api.authToken then return { authorized = true } end
	M._lastOpenedUrl = nil
	tab.oauthErrCode = nil
	api:FetchAuthToken(function(errCode)
		if errCode then
			tab.oauthErrCode = errCode
		else
			tab.oauthErrCode = nil
		end
		tab.oauthTimer = nil
	end)
	tab.oauthTimer = os.time()
	return { started = true, url = M._lastOpenedUrl }
end

-- "Logout from Path of Exile API" (:84-89).
function M.oauth_logout()
	local b = ensure_build()
	import_tab(b)
	local api = main().api
	if api then api:ResetDetails() end
	main():SaveSettings()
	return { authorized = false }
end

-- fetch_characters{source="oauth"|"site", realm="PC"|"XBOX"|"SONY", accountName?}:
-- the "Fetch Characters" button (:138-161) or the account-name "Start"
-- button (:363-366, DownloadSiteCharacterList).
function M.fetch_characters(p)
	local b = ensure_build()
	local tab = import_tab(b)
	hook_import_counters(tab)
	local realm = realm_by_id(tab, (p and p.realm) or main().lastRealm or "PC")
	if p and p.source == "site" then
		if capMissing.siteImport then error("this Path of Building has no account-name import", 0) end
		local name = p.accountName
		if type(name) ~= "string" or not name:match("%S[#%-]%d%d%d%d$") then
			error("account name needs its discriminator, e.g. Name#1234", 0)
		end
		local ctl = tab.controls.siteAccountName
		if ctl.pasteFilter then name = ctl.pasteFilter(name) end
		ctl:SetText(name)
		tab.controls.siteAccountRealm:SelByValue(realm.id, "id")
		tab:DownloadSiteCharacterList(realm)
		return { started = true, mode = tab.charImportMode }
	end
	local api = main().api
	if not api or not api.authToken then error("not authorized", 0) end
	tab.controls.accountRealm:SelByValue(realm.id, "id")
	tab.oauthLoading = true
	api:DownloadCharacterList(realm.realmCode, function(body, err, timeNext)
		if not err then
			tab.characterList[realm.realmCode] = body and body.characters or {}
			tab.oauthErrCode = nil
		elseif err == "Response code: 429" then
			tab.rateLimitEndTime = timeNext
		elseif err:match("401") then
			tab.oauthErrCode = "Auth token is invalid. Please login again."
			api:ResetDetails()
		else
			tab.oauthErrCode = err
		end
		tab.oauthLoading = false
	end)
	return { started = true }
end

-- import_account_character{source, realm, name, league?, what="tree"|"items",
-- deleteJewels, clearItems, clearSkills, ignoreWeaponSwap}: the "Passive
-- Tree and Jewels" / "Items and Skills" buttons (:249-321 OAuth, :429-467
-- site). One download per call; the page runs tree then items.
function M.import_account_character(p)
	local b = ensure_build()
	local tab = import_tab(b)
	hook_import_counters(tab)
	if type(p) ~= "table" or type(p.name) ~= "string" or p.name == "" then error("params.name required", 0) end
	local what = p.what == "items" and "items" or "tree"
	local realm = realm_by_id(tab, p.realm or main().lastRealm or "PC")
	local c = tab.controls
	if p.source == "site" then
		if tab.charImportMode ~= "SELECTCHAR" then error("fetch the character list first (mode " .. tostring(tab.charImportMode) .. ")", 0) end
		tab:BuildCharacterList(realm.realmCode, nil, tab.lastCharList, c.siteCharSelect)
		local sel
		for i, e in ipairs(c.siteCharSelect.list) do
			if e.char and e.char.name == p.name then sel = i break end
		end
		if not sel then error("character not in the fetched list: " .. p.name, 0) end
		c.siteCharSelect.selIndex = sel
		c.siteCharImportTreeClearJewels.state = p.deleteJewels and true or false
		c.siteCharImportItemsClearItems.state = p.clearItems and true or false
		c.siteCharImportItemsClearSkills.state = p.clearSkills and true or false
		c.siteCharImportItemsIgnoreWeaponSwap.state = p.ignoreWeaponSwap and true or false
		if what == "tree" then tab:DownloadPassiveTree(realm) else tab:DownloadItems(realm) end
		if tab.SetPredefinedBuildName then tab:SetPredefinedBuildName() end
		return { started = true, what = what }
	end
	local api = main().api
	if not api or not api.authToken then error("not authorized", 0) end
	local m = main()
	m.lastRealm = realm.id
	tab.lastRealm = realm.id
	if p.league then m.lastLeague = p.league; tab.lastLeague = p.league end
	m.lastCharacterHash = common.sha1(p.name)
	tab.lastCharacterHash = m.lastCharacterHash
	local deleteJewels = p.deleteJewels and true or false
	local clearItems, clearSkills, ignoreSwap = p.clearItems and true or false, p.clearSkills and true or false, p.ignoreWeaponSwap and true or false
	tab.oauthLoading = true
	api:DownloadCharacter(realm.realmCode, p.name, function(data, errMsg)
		if data and data.character then
			tab.oauthErrCode = nil
			if what == "tree" then
				tab:ImportPassiveTreeAndJewels(data.character, deleteJewels)
			else
				tab:ImportItemsAndSkills(data.character, clearItems, clearSkills, ignoreSwap)
			end
		else
			tab.oauthErrCode = errMsg and ("Could not import: " .. errMsg) or "Could not import character"
		end
		tab.oauthLoading = false
	end)
	return { started = true, what = what }
end

-- The account-name section's "Close" button (:468-471).
function M.import_site_reset()
	local b = ensure_build()
	local tab = import_tab(b)
	tab.charImportMode = "GETACCOUNTNAME"
	tab.charImportStatus = "Idle"
	return { mode = tab.charImportMode }
end

probe("classes.PoEAPI.FetchAuthToken/ResetDetails/DownloadCharacterList/DownloadCharacter", function()
	local c = class_of("PoEAPI")
	return type(c) == "table" and type(c.FetchAuthToken) == "function" and type(c.ResetDetails) == "function"
		and type(c.DownloadCharacterList) == "function" and type(c.DownloadCharacter) == "function"
end)
probe("classes.ImportTab.DownloadPassiveTree/DownloadItems/BuildCharacterList", function()
	local c = class_of("ImportTab")
	return type(c) == "table" and type(c.DownloadPassiveTree) == "function"
		and type(c.DownloadItems) == "function" and type(c.BuildCharacterList) == "function"
end)
probe("classes.ImportTab.DownloadSiteCharacterList/SetPredefinedBuildName (account-name import)", function()
	local c = class_of("ImportTab")
	return type(c) == "table" and type(c.DownloadSiteCharacterList) == "function" and type(c.SetPredefinedBuildName) == "function"
end, "siteImport")
probe("LaunchServer.lua present (OAuth loopback)", function()
	local f = io.open("LaunchServer.lua", "r")
	if f then f:close() return true end
	return false
end)

-- ---------------------------------------------------------------------------
-- Build list management (Modules/BuildList.lua buttons, BuildListControl's
-- NewFolder / RenameBuild / DeleteBuild). File operations stay under
-- main.buildPath; the popups' inputs arrive as params.
-- ---------------------------------------------------------------------------

local function build_root()
	local r = tostring(main().buildPath or "")
	if r ~= "" and r:sub(-1) ~= "/" and r:sub(-1) ~= "\\" then r = r .. "/" end
	return r
end

-- RenameBuild's EditControl filter: no path separators, wildcards, quotes or controls.
local function safe_build_name(name)
	if type(name) ~= "string" or not name:match("%S") or name:find('[\\/:%*%?"<>|%c]') then
		error("bad name: " .. tostring(name), 0)
	end
	return (name:gsub("^%s+", ""):gsub("%s+$", ""))
end

local function path_exists(path)
	local f = io.open(path, "r")
	if f then f:close() return true end
	return false
end

-- new_build{name?, subPath?}: the list's "New" button (BuildList.lua:39-41).
-- Main's mode switch shuts the open build down the way "<< Back" does.
function M.new_build(p)
	local m = main()
	local name = (p and type(p.name) == "string" and p.name:match("%S")) and p.name or "Unnamed build"
	if p and type(p.subPath) == "string" and m.modes.LIST then m.modes.LIST.subPath = p.subPath end
	m:SetMode("BUILD", false, name)
	return after_reinit()
end

-- new_folder{subPath?, name}: OpenNewFolderPopup's save (MakeDir under the list's folder).
function M.new_folder(p)
	local sub = (p and type(p.subPath) == "string") and p.subPath or ""
	local name = safe_build_name(p and p.name)
	local dir = build_root() .. sub .. name
	if not under_build_path(dir) then error("path must be under the build folder", 0) end
	if path_exists(dir) then error("already exists: " .. name, 0) end
	local ok, err = MakeDir(dir)
	if ok == false then error("cannot create folder: " .. tostring(err), 0) end
	return { path = dir, subPath = sub .. name .. "/" }
end

-- rename_build{path, subPath, isFolder, newName, copy?}: RenameBuild's save
-- button (BuildListControl.lua:122-148); copy=true is the "Copy" button.
function M.rename_build(p)
	if type(p) ~= "table" or type(p.path) ~= "string" then error("params.path required", 0) end
	if not under_build_path(p.path) then error("path must be under the build folder", 0) end
	local sub = type(p.subPath) == "string" and p.subPath or ""
	local newName = safe_build_name(p.newName)
	local dest = build_root() .. sub .. newName .. (p.isFolder and "" or ".xml")
	if not under_build_path(dest) then error("path must be under the build folder", 0) end
	if norm_path(dest) == norm_path(p.path) then return { path = dest, unchanged = true } end
	if path_exists(dest) then error("already exists: " .. newName, 0) end
	if p.copy then
		if p.isFolder then
			main():CopyFolder(p.path, dest)
		else
			local res, msg = copyFile(p.path, dest)
			if not res then error("copy failed: " .. tostring(msg), 0) end
		end
	else
		local res, msg = os.rename(p.path, dest)
		if not res then error("rename failed: " .. tostring(msg), 0) end
		-- the open build lives in that file: follow it, as SaveDBFile would
		local b = build()
		if b and b.dbFileName and norm_path(b.dbFileName) == norm_path(p.path) then
			b.dbFileName = dest
			b.buildName = newName
		end
	end
	return { path = dest }
end

-- delete_build{path, isFolder, recursive?}: DeleteBuild (BuildListControl.lua:149-175).
-- A non-empty folder needs recursive=true (POB asks first; the page does too).
function M.delete_build(p)
	if type(p) ~= "table" or type(p.path) ~= "string" then error("params.path required", 0) end
	if not under_build_path(p.path) then error("path must be under the build folder", 0) end
	if norm_path(p.path) == norm_path(build_root()) or norm_path(p.path) .. "/" == norm_path(build_root()) then
		error("refusing to delete the build folder itself", 0)
	end
	if p.isFolder then
		local nonEmpty = NewFileSearch(p.path .. "/*") or NewFileSearch(p.path .. "/*", true)
		if nonEmpty and not p.recursive then error("folder not empty", 0) end
		local res, msg = RemoveDir(p.path, nonEmpty and true or false)
		if res == false then error("cannot delete folder: " .. tostring(msg), 0) end
	else
		local b = build()
		if b and b.dbFileName and norm_path(b.dbFileName) == norm_path(p.path) then
			error("that build is open; close or save it elsewhere first", 0)
		end
		local res, msg = os.remove(p.path)
		if not res then error("cannot delete: " .. tostring(msg), 0) end
	end
	return { deleted = p.path }
end

-- move_build{path, subPath, isFolder, name, targetSubPath, copy?}: the build
-- list's cut/copy + paste into another folder and dragging onto a folder
-- (BuildList.lua OnFrame Ctrl+V, BuildListControl ReceiveDrag). A file keeps
-- its name with listMode:GetDestName's "[2]" suffix when taken; a folder
-- cannot go inside itself (BuildListHelpers.CanMoveToSubPath).
function M.move_build(p)
	if type(p) ~= "table" or type(p.path) ~= "string" or type(p.name) ~= "string" then error("params.path and params.name required", 0) end
	local sub = type(p.subPath) == "string" and p.subPath or ""
	local target = type(p.targetSubPath) == "string" and p.targetSubPath or ""
	local root = build_root()
	if not under_build_path(p.path) or not under_build_path(root .. target) then error("path must be under the build folder", 0) end
	local entry = { subPath = sub, folderName = p.isFolder and p.name or nil, fileName = (not p.isFolder) and p.name or nil }
	local helpers = require("Modules.BuildListHelpers")
	local can
	if helpers.CanMoveToSubPath then
		can = helpers.CanMoveToSubPath(entry, target)
	else
		-- PoE2's helpers have no CanMoveToSubPath (its paste does not check);
		-- the same two conditions as PoE1's, so a folder never recurses into itself
		can = sub ~= target and not (p.isFolder and target:sub(1, #(sub .. p.name .. "/")) == sub .. p.name .. "/")
	end
	if not can then error(p.isFolder and "a folder cannot be moved or copied into itself" or "already in that folder", 0) end
	local m = main()
	local dest
	if p.isFolder then
		dest = root .. target .. p.name
		if path_exists(dest) then error("already exists: " .. p.name, 0) end
		if p.copy then
			m:CopyFolder(p.path, dest)
		else
			m:MoveFolder(p.name, root .. sub, root .. target)
		end
	else
		dest = m.modes.LIST:GetDestName(target, p.name)
		local res, msg
		if p.copy then res, msg = copyFile(p.path, dest) else res, msg = os.rename(p.path, dest) end
		if not res then error((p.copy and "copy failed: " or "move failed: ") .. tostring(msg), 0) end
		local b = build()
		if not p.copy and b and b.dbFileName and norm_path(b.dbFileName) == norm_path(p.path) then b.dbFileName = dest end
	end
	return { path = dest }
end

probe("build list move/paste (main.MoveFolder, listMode.GetDestName, buildSortDropList)", function()
	local m = launch.main
	local ok, helpers = pcall(require, "Modules.BuildListHelpers")
	return type(m.MoveFolder) == "function" and type(m.modes) == "table" and type(m.modes.LIST) == "table"
		and type(m.modes.LIST.GetDestName) == "function" and ok and type(helpers.buildSortDropList) == "table"
end)

probe("build list file ops (copyFile/CopyFolder/RemoveDir/NewFileSearch/os.rename)", function()
	return type(copyFile) == "function" and type(launch.main.CopyFolder) == "function" and type(RemoveDir) == "function"
		and type(NewFileSearch) == "function" and type(os.rename) == "function" and type(os.remove) == "function"
end)

probe("classes.Build main-skill selectors", function()
	local b = build()
	return type(b) == "table" and type(b.RefreshSkillSelectControls) == "function" and type(b.SaveDB) == "function"
		and type(b.SaveDBFile) == "function" and type(b.ResetModFlags) == "function" and type(b.Init) == "function" and type(b.Shutdown) == "function"
end)
probe("share code codec (Deflate/Inflate/base64/xml)", function()
	return type(Deflate) == "function" and type(Inflate) == "function" and type(common) == "table" and type(common.base64) == "table"
		and type(common.base64.encode) == "function" and type(common.xml) == "table" and type(common.xml.ParseXML) == "function"
end)
probe("classes.ImportTab.ImportPassiveTreeAndJewels/ImportItemsAndSkills", function()
	local c = class_of("ImportTab")
	return type(c) == "table" and type(c.ImportPassiveTreeAndJewels) == "function" and type(c.ImportItemsAndSkills) == "function"
end)
probe("main.buildPath + MakeDir", function() return type(launch.main.buildPath) == "string" and type(MakeDir) == "function" end)

-- ---------------------------------------------------------------------------
-- Items
-- ---------------------------------------------------------------------------

-- A Tooltip's lines, the shape node_info hands out.
local function tooltip_lines(tt)
	local lines = {}
	for i, l in ipairs(tt.lines or {}) do
		if l.text ~= nil then
			lines[i] = { size = l.size, text = tr(l.text), raw = l.text, center = l.center and true or false, font = l.font }
		else
			lines[i] = { sep = l.size or 0 }
		end
	end
	return lines
end

-- Runs fn with POB's line wrapping disabled (the page wraps itself).
local function without_wrap(fn)
	local m = main()
	local savedWrap = m.WrapString
	m.WrapString = function(_, s) return { s } end
	local ok, err = pcall(fn)
	m.WrapString = savedWrap
	if not ok then error(err, 0) end
end

local influence_keys = { "shaper", "elder", "adjudicator", "basilisk", "crusader", "eyrie", "cleansing", "tangle" }

local function item_summary(it)
	local inf = {}
	for _, k in ipairs(influence_keys) do
		if it[k] then inf[#inf + 1] = k end
	end
	local sockets = {}
	for i, s in ipairs(it.sockets or {}) do sockets[i] = { color = s.color, group = s.group } end
	-- POB's display name is "Title, Base" for uniques; the dictionaries know
	-- the two halves, not the join.
	local nameZh
	if it.title and it.baseName then
		nameZh = tr(it.title) .. ", " .. tr(it.baseName)
	else
		nameZh = tr(it.name)
	end
	return {
		id = it.id,
		name = it.name,
		nameZh = nameZh,
		title = it.title,
		titleZh = it.title and tr(it.title) or nil,
		baseName = it.baseName,
		baseNameZh = it.baseName and tr(it.baseName) or nil,
		rarity = it.rarity,
		type = it.type,
		typeZh = it.type and tr(it.type) or nil,
		primarySlot = it.base and it:GetPrimarySlot() or nil,
		unsupported = (not it.base) and true or false,
		corrupted = it.corrupted and true or false,
		quality = it.quality,
		itemLevel = it.itemLevel,
		sockets = sockets,
		influences = inf,
		clusterJewel = it.clusterJewel and true or false,
		league = it.league,
		source = it.source,
	}
end

-- Where an item is used, the way ItemListControl:GetRowValue decides it.
local function item_used_in(tab, listCtl, it)
	local abyss = listCtl.FindEquippedAbyssJewel and listCtl:FindEquippedAbyssJewel(it.id, true)
	if abyss then return { kind = "abyss", setTitle = abyss, otherSet = true } end
	local tree = listCtl:FindSocketedJewel(it.id, true)
	if tree then return { kind = "jewel", specTitle = tree, otherSet = true } end
	local slot, set = tab:GetEquippedSlotForItem(it)
	if not slot then return nil end
	return { kind = "slot", slot = slot.slotName, label = slot.label, labelZh = tr(slot.label), setId = set and set.id or nil, setTitle = set and (set.title or "Default") or nil, otherSet = set ~= nil }
end

-- list_items: every item POB holds, the slot grid, item sets.
function M.list_items()
	local b = ensure_build()
	local tab = b.itemsTab
	-- Classic POB refreshes socket labels/activity from its Draw; do it here.
	if tab.UpdateSockets then tab:UpdateSockets() end
	local listCtl = tab.controls.itemList
	local items = {}
	for i, id in ipairs(tab.itemOrderList) do
		local it = tab.items[id]
		if it then
			local s = item_summary(it)
			-- "(Unused)" / "Used in '<set>'" the way ItemListControl:GetRowValue decides it
			if listCtl and it.base then s.usedIn = item_used_in(tab, listCtl, it) end
			items[#items + 1] = s
		end
	end
	local slots = {}
	for i, slot in ipairs(tab.orderedSlots) do
		-- Which of the build's items POB would let into this slot (its own rule).
		local valid = {}
		for _, id in ipairs(tab.itemOrderList) do
			local it = tab.items[id]
			if it and it.base and tab:IsItemValidForSlot(it, slot.slotName) then valid[#valid + 1] = id end
		end
		slots[i] = as_object({
			valid = valid,
			name = slot.slotName,
			label = slot.label,
			labelZh = tr(slot.label),
			selItemId = slot.selItemId or 0,
			weaponSet = slot.weaponSet,
			nodeId = slot.nodeId,
			socketIndex = slot.nodeId and tonumber(tostring(slot.label):match("#(%d+)")) or nil,
			shown = slot:IsShown() and true or false,
			inactive = slot.inactive and true or false,
			parent = slot.parentSlot and slot.parentSlot.slotName or nil,
			isFlask = slot.slotName:match("Flask") and true or false,
			active = slot.active and true or false,
		})
	end
	local sets = {}
	for i, id in ipairs(tab.itemSetOrderList) do
		local set = tab.itemSets[id]
		sets[i] = { id = id, title = set and set.title or nil }
	end
	return {
		items = items,
		slots = slots,
		itemSets = sets,
		activeItemSetId = tab.activeItemSetId,
		useSecondWeaponSet = tab.activeItemSet and tab.activeItemSet.useSecondWeaponSet and true or false,
		rev = b.outputRevision,
	}
end

-- Item text copied from the Chinese game client -> English, the way the
-- engine's Paste() hook treats clipboard text in the classic window.
-- PobToolsReverseText is that same reverse translator exposed to Lua; an
-- older engine DLL without it passes the text through untouched.
local function normalize_item_text(raw)
	if type(raw) ~= "string" or not raw:find("[\128-\255]") then return raw, false end
	if type(PobToolsReverseText) ~= "function" then return raw, false end
	local out = PobToolsReverseText(raw)
	if type(out) ~= "string" or out == "" then return raw, false end
	return out, out ~= raw
end

-- parse_item_text{raw}: what a paste turns into before anything is added --
-- the English text the reverse translator produced, and how POB's own parser
-- (new("Item")) read it: base, rarity, names, and every mod line with the part
-- modLib.parseMod could not use (modLine.extra). Lines still holding CJK after
-- the reverse translation are listed so the page can point at them.
function M.parse_item_text(p)
	ensure_build()
	local src = p and p.raw
	if type(src) ~= "string" or src == "" then error("params.raw required", 0) end
	local raw, reversed = normalize_item_text(src)
	local untranslated = {}
	for l in (raw .. "\n"):gmatch("([^\r\n]*)\r?\n") do
		if l:find("[\128-\255]") then untranslated[#untranslated + 1] = l end
	end
	local ok, it = pcall(make, "Item", raw, p.rarity, true)
	if not ok or type(it) ~= "table" then
		return { reversed = reversed, text = raw, untranslated = untranslated, parsed = false, error = tostring(it) }
	end
	local lines = {}
	local function take(kind, list)
		for _, ml in ipairs(list or {}) do
			if type(ml) == "table" and type(ml.line) == "string" then
				lines[#lines + 1] = { kind = kind, line = ml.line, lineZh = tr(ml.line),
				                      extra = ml.extra, unsupported = ml.extra ~= nil and true or false }
			end
		end
	end
	take("rune", it.runeModLines)
	take("enchant", it.enchantModLines)
	take("classRequirement", it.classRequirementModLines)
	take("implicit", it.implicitModLines)
	take("explicit", it.explicitModLines)
	take("crucible", it.crucibleModLines)
	return {
		reversed = reversed, text = raw, untranslated = untranslated, parsed = it.base ~= nil,
		rarity = it.rarity, name = it.name, title = it.title, baseName = it.baseName, type = it.type,
		titleZh = it.title and tr(it.title) or nil, baseNameZh = it.baseName and tr(it.baseName) or nil,
		itemLevel = it.itemLevel, quality = it.quality, corrupted = it.corrupted and true or false,
		lines = lines,
	}
end

-- item_tooltip{id | raw, slotName?, dbMode?}: POB's own item tooltip. With a
-- slot the stat-difference block ("equipping this changes DPS by...") is
-- included, exactly as hovering the item over that slot in the classic UI.
function M.item_tooltip(p)
	local b = ensure_build()
	local tab = b.itemsTab
	local it, dbMode
	if p and p.raw then
		local raw, reversed = normalize_item_text(p.raw)
		it = make("Item", raw, p.rarity, true)
		if reversed then p.reversed = true end
		if it.base then it:BuildModList() end
		dbMode = true
	else
		it = tab.items[tonumber(p and p.id or 0)]
		if not it then error("no item " .. tostring(p and p.id), 0) end
		dbMode = p.dbMode and true or false
	end
	-- compare (default on) = the stat-difference block ("equipping this in
	-- <slot> will give you: ..."); with no slot named POB's own choice
	-- (GetComparisonSlotNameForItem) is used, as its list tooltips do.
	local compare = not (p and p.compare == false)
	local slot = p and p.slotName and tab.slots[p.slotName] or nil
	if compare and not slot and it.base then
		local name = tab:GetComparisonSlotNameForItem(it)
		slot = name and tab.slots[name] or nil
	end
	local tt = make("Tooltip")
	local m = main()
	local savedDiff, savedSlotOnly = tab.showStatDifferences, m.slotOnlyTooltips
	tab.showStatDifferences = compare
	if p and p.slotOnly ~= nil then m.slotOnlyTooltips = p.slotOnly and true or false end
	local ok, err = pcall(without_wrap, function() tab:AddItemTooltip(tt, it, slot, dbMode) end)
	tab.showStatDifferences, m.slotOnlyTooltips = savedDiff, savedSlotOnly
	if not ok then error(err, 0) end
	return {
		compareSlot = slot and slot.slotName or nil,
		id = it.id,
		header = tt.tooltipHeader,
		color = tt.color,
		lines = tooltip_lines(tt),
		summary = item_summary(it),
		reversed = (p and p.reversed) and true or false,
	}
end

-- item_raw{id}: the item as text (what copying it in POB gives).
function M.item_raw(p)
	local b = ensure_build()
	local it = b.itemsTab.items[tonumber(p and p.id or 0)]
	if not it then error("no item " .. tostring(p and p.id), 0) end
	return { raw = it:BuildRaw() }
end

local function items_committed(b)
	b.itemsTab:PopulateSlots()
	b.itemsTab:AddUndoState()
	return commit(b)
end

-- add_item{raw, equip=true|false, slotName?}: new("Item") from pasted text,
-- then ItemsTab:AddItem (auto-equips into the first free valid slot unless
-- equip=false); slotName forces that slot.
function M.add_item(p)
	local b = ensure_build()
	local tab = b.itemsTab
	local raw = p and p.raw
	if type(raw) ~= "string" or #raw == 0 then error("params.raw required", 0) end
	if #raw > 64 * 1024 then error("item text too large", 0) end
	local reversed
	raw, reversed = normalize_item_text(raw)
	local it = make("Item", raw)
	if not it.base then error("item base not recognised: " .. tostring(it.baseName or it.name), 0) end
	local wantSlot = p.slotName and tab.slots[p.slotName] or nil
	tab:AddItem(it, p.equip == false or wantSlot ~= nil)
	if wantSlot then
		if not tab:IsItemValidForSlot(it, wantSlot.slotName) then
			tab:DeleteItem(it)
			error("item does not fit slot " .. wantSlot.slotName, 0)
		end
		wantSlot:SetSelItemId(it.id)
	end
	local r = items_committed(b)
	r.item = item_summary(it)
	r.reversed = reversed
	return r
end

function M.delete_item(p)
	local b = ensure_build()
	local tab = b.itemsTab
	local it = tab.items[tonumber(p and p.id or 0)]
	if not it then error("no item " .. tostring(p and p.id), 0) end
	tab:DeleteItem(it)
	return items_committed(b)
end

function M.equip_item(p)
	local b = ensure_build()
	local tab = b.itemsTab
	local it = tab.items[tonumber(p and p.id or 0)]
	local slot = p and p.slotName and tab.slots[p.slotName]
	if not it then error("no item " .. tostring(p and p.id), 0) end
	if not slot then error("no slot " .. tostring(p and p.slotName), 0) end
	if not tab:IsItemValidForSlot(it, slot.slotName) then error("item does not fit slot " .. slot.slotName, 0) end
	slot:SetSelItemId(it.id)
	return items_committed(b)
end

function M.unequip_slot(p)
	local b = ensure_build()
	local slot = p and p.slotName and b.itemsTab.slots[p.slotName]
	if not slot then error("no slot " .. tostring(p and p.slotName), 0) end
	slot:SetSelItemId(0)
	return items_committed(b)
end

-- Flask slots have an "active" tick.
function M.set_slot_active(p)
	local b = ensure_build()
	local tab = b.itemsTab
	local slot = p and p.slotName and tab.slots[p.slotName]
	if not slot or not slot.controls.activate then error("no flask slot " .. tostring(p and p.slotName), 0) end
	slot.active = p.active and true or false
	tab.activeItemSet[slot.slotName].active = slot.active
	slot.controls.activate.state = slot.active
	return items_committed(b)
end

function M.set_item_set(p)
	local b = ensure_build()
	local tab = b.itemsTab
	local id = tonumber(p and p.id)
	if not id or not tab.itemSets[id] then error("no item set " .. tostring(p and p.id), 0) end
	tab:SetActiveItemSet(id)
	tab:AddUndoState()
	return commit(b)
end

-- new_item_set{title, copyCurrent?}: the manage popup's "New" (+ "Copy").
function M.new_item_set(p)
	local b = ensure_build()
	local tab = b.itemsTab
	local set = tab:NewItemSet()
	set.title = p and p.title or nil
	if p and p.copyCurrent and tab.activeItemSet then
		for slotName, slot in pairs(tab.slots) do
			if not slot.nodeId and tab.activeItemSet[slotName] then
				set[slotName].selItemId = tab.activeItemSet[slotName].selItemId
				set[slotName].active = tab.activeItemSet[slotName].active
			end
		end
		set.useSecondWeaponSet = tab.activeItemSet.useSecondWeaponSet
	end
	table.insert(tab.itemSetOrderList, set.id)
	tab:SetActiveItemSet(set.id)
	tab:AddUndoState()
	local r = commit(b)
	r.id = set.id
	return r
end

function M.rename_item_set(p)
	local b = ensure_build()
	local set = b.itemsTab.itemSets[tonumber(p and p.id or 0)]
	if not set then error("no item set " .. tostring(p and p.id), 0) end
	set.title = p.title
	return commit(b)
end

function M.delete_item_set(p)
	local b = ensure_build()
	local tab = b.itemsTab
	local id = tonumber(p and p.id)
	if not id or not tab.itemSets[id] then error("no item set " .. tostring(p and p.id), 0) end
	if #tab.itemSetOrderList <= 1 then error("cannot delete the last item set", 0) end
	for i, v in ipairs(tab.itemSetOrderList) do
		if v == id then table.remove(tab.itemSetOrderList, i) break end
	end
	if tab.activeItemSetId == id then tab:SetActiveItemSet(tab.itemSetOrderList[1]) end
	tab.itemSets[id] = nil
	tab:AddUndoState()
	return commit(b)
end

function M.set_weapon_swap(p)
	local b = ensure_build()
	local tab = b.itemsTab
	if not tab.activeItemSet then error("no item set", 0) end
	tab.activeItemSet.useSecondWeaponSet = p and p.on and true or false
	tab:AddUndoState()
	return commit(b)
end

-- item_db{kind="unique"|"rare", query?, type?, page?, size?}: POB's unique
-- and rare databases (main.uniqueDB / rareDB), matched on English and
-- translated names, paged.
probe("PobToolsReverseText (optional: engine DLL with the paste reverse translator)", function()
	return PobToolsReverseText == nil or type(PobToolsReverseText) == "function"
end)
probe("classes.ItemsTab.AddItem/DeleteItem/AddItemTooltip/IsItemValidForSlot/PopulateSlots", function()
	local c = class_of("ItemsTab")
	return type(c) == "table" and type(c.AddItem) == "function" and type(c.DeleteItem) == "function" and type(c.AddItemTooltip) == "function"
		and type(c.IsItemValidForSlot) == "function" and type(c.PopulateSlots) == "function"
end)
probe("classes.ItemsTab item sets (NewItemSet/SetActiveItemSet)", function()
	local c = class_of("ItemsTab")
	return type(c) == "table" and type(c.NewItemSet) == "function" and type(c.SetActiveItemSet) == "function"
end)
probe("classes.ItemSlotControl.SetSelItemId", function() local c = class_of("ItemSlotControl"); return type(c) == "table" and type(c.SetSelItemId) == "function" end)
probe("classes.Item(raw)/BuildRaw/GetPrimarySlot", function()
	local c = class_of("Item")
	return type(c) == "table" and type(c.BuildRaw) == "function" and type(c.GetPrimarySlot) == "function" and type(c.BuildModList) == "function"
end)
probe("main.uniqueDB/rareDB", function() return type(launch.main.uniqueDB) == "table" and type(launch.main.rareDB) == "table" end)

-- ---------------------------------------------------------------------------
-- Items: the editing session (POB's own ItemsTab.displayItem and controls)
-- ---------------------------------------------------------------------------
-- Classic POB edits one item at a time in `itemsTab.displayItem`; every
-- control on that panel (sockets, quality, influence, affix dropdowns, range
-- sliders, custom-mod Remove buttons) is a real control object that exists
-- headless too, so the page drives those controls' own callbacks instead of
-- re-implementing what they do. Nothing recalculates until AddDisplayItem.

local function edit_item()
	local b = ensure_build()
	local tab = b.itemsTab
	if not tab.displayItem then error("no item is being edited; call item_edit_begin first", 0) end
	return b, tab, tab.displayItem
end

local function ctl_shown(ctl)
	if not ctl then return false end
	local ok, v = pcall(ctl.IsShown, ctl)
	return ok and v and true or false
end

-- A dropdown entry's label: entries are strings or tables with `label`.
local function dd_label(e)
	if type(e) == "table" then
		local l = e.label
		if type(l) == "function" then l = l() end
		return tostring(l or "")
	end
	return tostring(e)
end

local function dd_options(ctl)
	local out = {}
	for i, e in ipairs(ctl and ctl.list or {}) do
		local l = dd_label(e)
		out[i] = { label = l, labelZh = tr(l) }
	end
	return out
end

-- The ItemsTab dialogs (Craft / Edit text / Enchant / Anoint / Corrupt / Add
-- modifier / Crucible) keep their logic in closures over a `controls` table
-- handed to main:OpenPopup. This runs `fn` with OpenPopup swapped for one
-- that keeps that table and installs a stand-in popup (the corrupt dialog
-- writes main.popups[1].height), with ClosePopup muted. With `keep` the
-- stand-in stays in main.popups until popup_discard; otherwise it is removed
-- again before returning.
local function capture_popup(fn, keep)
	local m = main()
	local savedOpen, savedClose = m.OpenPopup, m.ClosePopup
	local captured
	m.OpenPopup = function(self, width, height, title, controls)
		captured = { controls = controls or {}, title = title, width = width, height = height }
		table.insert(self.popups, 1, captured)
		return captured
	end
	m.ClosePopup = function() end
	local ok, err = pcall(fn)
	m.OpenPopup, m.ClosePopup = savedOpen, savedClose
	if captured and not keep then
		for i, p in ipairs(m.popups) do
			if p == captured then table.remove(m.popups, i) break end
		end
	end
	if not ok then error(err, 0) end
	return captured
end

local function popup_discard(cap)
	if not cap then return end
	local m = main()
	for i, p in ipairs(m.popups) do
		if p == cap then table.remove(m.popups, i) break end
	end
end

-- Runs a popup control's callback with ClosePopup muted (the dialogs' Save
-- buttons close the popup themselves) and any dialog it opens captured.
local function with_popup(fn)
	return capture_popup(fn, false)
end

-- Runs a ListControl's coroutine ListBuilder to completion (classic POB
-- resumes it once per frame from Draw).
local function run_list_builder(ctl)
	local co = coroutine.create(ctl.ListBuilder)
	while coroutine.status(co) ~= "dead" do
		local ok, err = coroutine.resume(co, ctl)
		if not ok then error(err, 0) end
	end
end

-- What POB puts in the tooltip of a notable in the Anoint dialog:
-- NotableDBControl:AddValueTooltip -- the node's name, its own lines, the
-- reminder text and ItemsTab:AppendAnointTooltip's "anointing this gives you"
-- comparison. One calculation per node, so it is built on demand.
local function notable_tooltip(ctl, index, node)
	local tt = make("Tooltip")
	local ok, err = pcall(without_wrap, function() ctl:AddValueTooltip(tt, index, node) end)
	if not ok then error(err, 0) end
	return tooltip_lines(tt)
end

local edit_popup -- { kind, cap }

local function close_edit_popup()
	if edit_popup then
		popup_discard(edit_popup.cap)
		edit_popup = nil
	end
end

local function mod_line_text(modLine)
	local ok, s = pcall(itemLib.formatModLine, modLine)
	if ok and type(s) == "string" then return s end
	return tostring(modLine.line or "")
end

-- Everything the editing panel shows, read from POB's controls after they
-- were refreshed by SetDisplayItem / the last callback.
local function edit_state(p)
	local b, tab, it = edit_item()
	local c = tab.controls
	local tt = make("Tooltip")
	local savedDiff = tab.showStatDifferences
	tab.showStatDifferences = not (p and p.compare == false)
	local ok, err = pcall(without_wrap, function() tab:AddItemTooltip(tt, it) end)
	tab.showStatDifferences = savedDiff
	if not ok then error(err, 0) end

	local sockets = {}
	for i, s in ipairs(it.sockets or {}) do sockets[i] = { color = s.color, group = s.group } end
	local socketColors = {}
	for _, e in ipairs(c.displayItemSocket1 and c.displayItemSocket1.list or {}) do socketColors[#socketColors + 1] = e.color end
	local links = {}
	for i = 1, 5 do links[i] = { shown = ctl_shown(c["displayItemLink" .. i]), on = c["displayItemLink" .. i] and c["displayItemLink" .. i].state and true or false } end
	local socketShown = {}
	for i = 1, 6 do socketShown[i] = ctl_shown(c["displayItemSocket" .. i]) end

	-- influence: the two dropdowns share one list ("Influence" = none, then itemLib.influenceInfo.all)
	-- (PoE2 items have no influences: no controls, nothing to show)
	local infl = { shown = false, options = {}, sel = {}, keys = {}, current = {} }
	if c.displayItemInfluence and itemLib.influenceInfo and itemLib.influenceInfo.all then
		infl = { shown = ctl_shown(c.displayItemInfluence), options = dd_options(c.displayItemInfluence), sel = { c.displayItemInfluence.selIndex, c.displayItemInfluence2.selIndex }, keys = {} }
		for i, info in ipairs(itemLib.influenceInfo.all) do
			infl.keys[i] = info.key
		end
		local current = {}
		for _, info in ipairs(itemLib.influenceInfo.all) do
			if it[info.key] then current[#current + 1] = info.key end
		end
		infl.current = current
	end

	-- variants: either the group controls or the legacy variant/variantAlt dropdowns
	local variants = {}
	local variantCtls = { "displayItemVariant", "displayItemAltVariant", "displayItemAltVariant2", "displayItemAltVariant3", "displayItemAltVariant4", "displayItemAltVariant5" }
	for _, name in ipairs(variantCtls) do
		local ctl = c[name]
		if ctl and ctl_shown(ctl) then
			local en = ctl.enabled
			if type(en) == "function" then en = en(ctl) end
			variants[#variants + 1] = { control = name, options = dd_options(ctl), sel = ctl.selIndex or 1, enabled = en ~= false }
		end
	end
	local versions
	if it.usesVariantGroups and c.displayItemVersion and (it.versionList and #it.versionList > 1) then
		versions = { options = dd_options(c.displayItemVersion), sel = c.displayItemVersion.selIndex or 1 }
	end

	-- affixes (crafted items): POB's dropdowns after UpdateAffixControls
	local affixes = {}
	if it.crafted then
		for i = 1, (it.affixLimit or 0) do
			local drop = c["displayItemAffix" .. i]
			if drop and ctl_shown(drop) then
				local lbl = c["displayItemAffixLabel" .. i]
				local kind = lbl and dd_label(lbl) or ""
				local opts = {}
				for k, e in ipairs(drop.list or {}) do
					local l = dd_label(e)
					opts[k] = { label = l, labelZh = tr(l), tiers = type(e) == "table" and e.modList and #e.modList or nil, haveRange = type(e) == "table" and e.haveRange and true or false }
				end
				local slider = drop.slider
				affixes[#affixes + 1] = {
					index = i, table = drop.outputTable, slot = drop.outputIndex, kind = kind, kindZh = tr(kind),
					options = opts, sel = drop.selIndex or 1,
					roll = slider and slider.val or nil, rollShown = ctl_shown(slider), tiers = slider and slider.divCount or nil,
				}
			end
		end
	end

	-- range lines (uniques/rares with rolled values, foulborn mutations)
	local ranges = {}
	if c.displayItemRangeLine then
		for i, e in ipairs(c.displayItemRangeLine.list or {}) do
			local ml = e.modLine
			ranges[i] = {
				index = i, label = e.label, labelZh = tr(e.label),
				range = ml and ml.range or nil, showSlider = ml and ml.showSlider and true or false,
				mutable = ml and ml.modId and ml.newModId and true or false, mutated = ml and ml.mutated and true or false,
			}
		end
	end

	-- explicit + crucible lines with their Remove buttons (UpdateCustomControls numbers them)
	local modLines = {}
	local removeIndex = 0
	local removable = it.rareLikeUnique or it.rarity == "MAGIC" or it.rarity == "RARE" or (it.crucibleModLines and #it.crucibleModLines > 0)
	local all = {}
	for _, ml in ipairs(it.explicitModLines or {}) do all[#all + 1] = ml end
	for _, ml in ipairs(it.crucibleModLines or {}) do all[#all + 1] = ml end
	for i, ml in ipairs(all) do
		local entry = { index = i, text = mod_line_text(ml), disabled = ml.disabled and true or false,
			kind = ml.crafted and "crafted" or ml.custom and "custom" or ml.crucible and "crucible" or nil }
		entry.textZh = tr(entry.text)
		local okF, formatted = pcall(itemLib.formatModLine, ml)
		if removable and (ml.custom or ml.crafted or ml.crucible) and okF and formatted then
			removeIndex = removeIndex + 1
			entry.remove = removeIndex
		end
		modLines[i] = entry
	end

	local cluster
	if ctl_shown(c.displayItemClusterJewelSkill) then
		local cj = it.clusterJewel
		cluster = {
			options = dd_options(c.displayItemClusterJewelSkill), sel = c.displayItemClusterJewelSkill.selIndex or 1,
			nodeCount = it.clusterJewelNodeCount, minNodes = cj and cj.minNodes, maxNodes = cj and cj.maxNodes,
		}
	end

	return {
		id = it.id,
		isNew = tab.items[it.id] == nil,
		raw = it:BuildRaw(),
		summary = item_summary(it),
		tooltip = { header = tt.tooltipHeader, color = tt.color, lines = tooltip_lines(tt) },
		sockets = sockets, socketShown = socketShown, socketColors = socketColors, links = links,
		canAddSocket = ctl_shown(c.displayItemAddSocket),
		quality = { shown = ctl_shown(c.displayItemQualityEdit), value = it.quality },
		catalyst = { shown = ctl_shown(c.displayItemCatalyst), options = dd_options(c.displayItemCatalyst), sel = (it.catalyst or 0) + 1,
			qualityShown = ctl_shown(c.displayItemCatalystQualityEdit), quality = it.catalystQuality },
		influence = infl,
		variants = variants, versions = versions,
		affixes = affixes, crafted = it.crafted and true or false,
		ranges = ranges,
		modLines = modLines,
		cluster = cluster,
		actions = {
			enchant = ctl_shown(c.displayItemEnchant), enchant2 = ctl_shown(c.displayItemEnchant2),
			anoint = ctl_shown(c.displayItemAnoint), anoint2 = ctl_shown(c.displayItemAnoint2), anoint3 = ctl_shown(c.displayItemAnoint3), anoint4 = ctl_shown(c.displayItemAnoint4),
			corrupt = ctl_shown(c.displayItemCorrupt), addImplicit = ctl_shown(c.displayItemAddImplicit),
			custom = ctl_shown(c.displayItemAddCustom), crucible = ctl_shown(c.displayItemAddCrucible),
		},
		popup = edit_popup and edit_popup.kind or nil,
	}
end

function M.item_edit_state(p)
	return edit_state(p)
end

-- craft_item_options: the Craft Item dialog's rarity list plus POB's base
-- type / base lists (build.data.itemBaseTypeList / itemBaseLists).
function M.craft_item_options()
	local b = ensure_build()
	local tab = b.itemsTab
	local cap = capture_popup(function() tab:CraftItem() end, false)
	local rarities = {}
	for i, e in ipairs(cap.controls.rarity.list) do
		rarities[i] = { label = dd_label(e), labelZh = tr(strip_escapes(dd_label(e))), rarity = e.rarity }
	end
	local types = {}
	for i, t in ipairs(b.data.itemBaseTypeList) do
		local bases = {}
		for k, e in ipairs(b.data.itemBaseLists[t] or {}) do bases[k] = { label = e.label, labelZh = tr(e.label), name = e.name } end
		types[i] = { type = t, typeZh = tr(t), bases = bases }
	end
	return { rarities = rarities, types = types, defaults = { rarity = cap.controls.rarity.selIndex, type = cap.controls.type.selIndex, base = cap.controls.base.selIndex } }
end

-- item_edit_begin{id | raw | craft={rarity,type,base,title?}}: start editing
-- a copy of a build item (the list's double-click), pasted text
-- (CreateDisplayItemFromRaw), or a new item from the Craft Item dialog.
function M.item_edit_begin(p)
	local b = ensure_build()
	local tab = b.itemsTab
	close_edit_popup()
	if p and p.id then
		local src = tab.items[tonumber(p.id)]
		if not src then error("no item " .. tostring(p.id), 0) end
		local newItem = make("Item", src:BuildRaw())
		newItem.id = src.id
		tab:SetDisplayItem(newItem)
	elseif p and p.raw then
		local raw = normalize_item_text(p.raw)
		tab:SetDisplayItem(nil)
		tab:CreateDisplayItemFromRaw(raw, true)
		if not tab.displayItem then error("item base not recognised", 0) end
	elseif p and p.craft then
		local cr = p.craft
		local cap = capture_popup(function() tab:CraftItem() end, true)
		local ctl = cap.controls
		if cr.rarity then ctl.rarity:SelByValue(cr.rarity, "rarity") end
		if cr.type then
			for i, t in ipairs(ctl.type.list) do
				if t == cr.type then ctl.type:SetSel(i) break end
			end
		end
		if cr.base then
			for i, e in ipairs(ctl.base.list) do
				if e.name == cr.base or e.label == cr.base then ctl.base.selIndex = i break end
			end
		end
		if cr.title and ctl.title then ctl.title:SetText(cr.title) end
		-- Create: SetDisplayItem, then (for non-crafted rarities) the text editor, which we drop
		local ok, err = pcall(with_popup, function() ctl.save.onClick() end)
		popup_discard(cap)
		if not ok then error(err, 0) end
		if not tab.displayItem then error("craft did not produce an item", 0) end
	else
		error("params.id, params.raw or params.craft required", 0)
	end
	return edit_state(p)
end

function M.item_edit_cancel()
	local b = ensure_build()
	close_edit_popup()
	b.itemsTab:SetDisplayItem(nil)
	return { ok = true }
end

-- item_edit_commit{equip=true|false}: AddDisplayItem (POB's "Add to build" /
-- "Save"): AddItem or replace, PopulateSlots, undo state, recalc.
function M.item_edit_commit(p)
	local b, tab, it = edit_item()
	close_edit_popup()
	local wasNew = tab.items[it.id] == nil
	tab:AddDisplayItem(p and p.equip == false)
	local r = commit(b)
	r.id = it.id
	r.added = wasNew
	r.item = item_summary(it)
	return r
end

-- item_edit_set{...}: one field per call, through the panel control's own
-- callback. Returns the refreshed state.
function M.item_edit_set(p)
	local b, tab, it = edit_item()
	local c = tab.controls
	p = p or {}
	if p.quality ~= nil then
		c.displayItemQualityEdit:SetText(tostring(tonumber(p.quality) or 0), true)
	elseif p.catalyst ~= nil then
		c.displayItemCatalyst:SetSel(tonumber(p.catalyst) or 1)
	elseif p.catalystQuality ~= nil then
		c.displayItemCatalystQualityEdit:SetText(tostring(tonumber(p.catalystQuality) or 0), true)
	elseif p.influence ~= nil then
		-- two dropdown indices into the shared list (1 = none)
		local a, bIdx = tonumber(p.influence[1]) or 1, tonumber(p.influence[2]) or 1
		c.displayItemInfluence.selIndex = a
		c.displayItemInfluence2.selIndex = bIdx
		c.displayItemInfluence.selFunc(a, c.displayItemInfluence.list[a])
	elseif p.variant then
		local ctl = c[p.variant.control]
		if not ctl or not ctl.selFunc then error("no variant control " .. tostring(p.variant.control), 0) end
		ctl:SetSel(tonumber(p.variant.sel) or 1)
	elseif p.version then
		c.displayItemVersion:SetSel(tonumber(p.version) or 1)
	elseif p.socket then
		local i = tonumber(p.socket.index)
		local drop = c["displayItemSocket" .. tostring(i)]
		if not drop then error("no socket " .. tostring(i), 0) end
		drop:SelByValue(p.socket.color, "color")
		drop.selFunc(drop.selIndex, drop.list[drop.selIndex])
	elseif p.link then
		local i = tonumber(p.link.index)
		local box = c["displayItemLink" .. tostring(i)]
		if not box then error("no link " .. tostring(i), 0) end
		box.state = p.link.on and true or false
		box.changeFunc(box.state)
	elseif p.addSocket then
		if not ctl_shown(c.displayItemAddSocket) then error("no more sockets can be added", 0) end
		c.displayItemAddSocket.onClick()
	elseif p.range then
		local i = tonumber(p.range.index)
		if not c.displayItemRangeLine.list[i] then error("no range line " .. tostring(i), 0) end
		c.displayItemRangeLine.selIndex = i
		if p.range.mutate ~= nil then
			c.displayItemMutatedCheckbox.changeFunc(p.range.mutate and true or false)
		else
			local v = math.max(0, math.min(1, tonumber(p.range.value) or 0))
			c.displayItemRangeSlider.val = v
			c.displayItemRangeSlider.changeFunc(v)
		end
	elseif p.modLine then
		local i = tonumber(p.modLine.index)
		local n = #(it.explicitModLines or {})
		local ml = i <= n and it.explicitModLines[i] or it.crucibleModLines and it.crucibleModLines[i - n]
		if not ml then error("no mod line " .. tostring(i), 0) end
		if p.modLine.enabled ~= nil and (not ml.disabled) ~= (p.modLine.enabled and true or false) then
			if not tab.ToggleDisplayItemModLine then error("this Path of Building cannot switch mod lines off", 0) end
			tab:ToggleDisplayItemModLine(ml)
		end
	elseif p.removeModLine then
		local k = tonumber(p.removeModLine)
		tab:UpdateCustomControls()
		local btn = c["displayItemCustomModifierRemove" .. tostring(k)]
		if not btn or btn.shown == false then error("no removable line " .. tostring(k), 0) end
		btn.onClick()
	elseif p.cluster then
		if p.cluster.sel then
			c.displayItemClusterJewelSkill:SetSel(tonumber(p.cluster.sel) or 1)
		elseif p.cluster.nodeCount then
			local cj = it.clusterJewel
			local v = (tonumber(p.cluster.nodeCount) - cj.minNodes) / math.max(1, cj.maxNodes - cj.minNodes)
			v = math.max(0, math.min(1, v))
			c.displayItemClusterJewelNodeCount.val = v
			c.displayItemClusterJewelNodeCount.changeFunc(v)
		end
	elseif p.raw then
		local id = it.id
		tab:CreateDisplayItemFromRaw(p.raw, false)
		if tab.displayItem then tab.displayItem.id = id else error("item text not recognised", 0) end
	else
		error("nothing to set", 0)
	end
	return edit_state(p)
end

-- item_edit_affix{index, sel?, roll?}: the crafted-item affix dropdown /
-- tier slider (the dropdown's selFunc writes prefixes/suffixes and Crafts).
function M.item_edit_affix(p)
	local b, tab, it = edit_item()
	local c = tab.controls
	local drop = c["displayItemAffix" .. tostring(tonumber(p and p.index))]
	if not drop or not it.crafted then error("no affix slot " .. tostring(p and p.index), 0) end
	if p.roll ~= nil then drop.slider.val = math.max(0, math.min(1, tonumber(p.roll) or 0)) end
	if p.sel ~= nil then
		local sel = math.max(1, math.min(#drop.list, tonumber(p.sel) or 1))
		drop.selIndex = sel
		drop.selFunc(sel, drop.list[sel])
	elseif p.roll ~= nil then
		drop.slider.changeFunc(drop.slider.val)
	end
	return edit_state(p)
end

-- The captured dialog as data: every control with a list / text / state.
local function popup_state()
	if not edit_popup then return { popup = nil } end
	local ctls = {}
	local names = {}
	for name, ctl in pairs(edit_popup.cap.controls) do
		if type(name) == "string" and type(ctl) == "table" then names[#names + 1] = name end
	end
	table.sort(names)
	for _, name in ipairs(names) do
		local ctl = edit_popup.cap.controls[name]
		local sh = ctl.shown
		if type(sh) == "function" then sh = sh(ctl) end
		if sh ~= false then
			local en = ctl.enabled
			if type(en) == "function" then en = en(ctl) end
			local entry = { name = name, enabled = en ~= false }
			if name == "notableDB" then
				if ctl.listBuildFlag ~= false or not ctl.list then run_list_builder(ctl) ctl.listBuildFlag = false end
				entry.kind = "nodes"
				local opts = {}
				for i, node in ipairs(ctl.list or {}) do
					opts[i] = { id = node.id, label = node.dn, labelZh = tr(node.dn) }
					if ctl.selValue == node then entry.sel = i end
				end
				entry.options = opts
				entry.search = ctl.controls.search and ctl.controls.search.buf or ""
				if entry.sel and ctl.list[entry.sel] then
					entry.tooltip = notable_tooltip(ctl, entry.sel, ctl.list[entry.sel])
				end
			elseif ctl.DropIndexToListIndex and ctl.list then
				entry.kind = "dropdown"
				entry.options = dd_options(ctl)
				entry.sel = ctl.selIndex
			elseif ctl.SetText and ctl.buf ~= nil then
				entry.kind = "edit"
				entry.text = ctl.buf
			elseif ctl.GetDivVal then
				entry.kind = "slider"
				entry.value = ctl.val
			elseif ctl.state ~= nil and ctl.changeFunc then
				entry.kind = "check"
				entry.state = ctl.state and true or false
				entry.label = type(ctl.label) == "string" and ctl.label or nil
			elseif ctl.onClick then
				entry.kind = "button"
				local l = ctl.label
				if type(l) == "function" then l = l() end
				entry.label = tostring(l or "")
				entry.labelZh = tr(entry.label)
			else
				entry = nil
			end
			if entry then ctls[#ctls + 1] = entry end
		end
	end
	return { popup = edit_popup.kind, title = edit_popup.cap.title, controls = ctls }
end

local popup_openers = {
	enchant = function(tab, p)
		if not tab.EnchantDisplayItem then error("this Path of Building has no enchant dialog", 0) end
		tab:EnchantDisplayItem(tonumber(p.slot) or 1)
	end,
	anoint = function(tab, p) tab:AnointDisplayItem(tonumber(p.slot) or 1) end,
	-- through the panel's own button: beta opens one dialog with a source
	-- dropdown, master 2.67.2 passes the mod type ("Corrupted") itself
	corrupt = function(tab) tab.controls.displayItemCorrupt.onClick() end,
	custom = function(tab) tab:AddCustomModifierToDisplayItem() end,
	crucible = function(tab)
		if not tab.AddCrucibleModifierToDisplayItem then error("this Path of Building has no crucible dialog", 0) end
		tab:AddCrucibleModifierToDisplayItem()
	end,
	text = function(tab) tab:EditDisplayItemText() end,
	implicit = function(tab) tab.controls.displayItemAddImplicit.onClick() end,
}

-- item_edit_popup{kind, action="open"|"pick"|"apply"|"cancel", name?, sel?, text?, state?, value?, button?}
function M.item_edit_popup(p)
	local b, tab, it = edit_item()
	local action = p and p.action or "open"
	if action == "open" then
		local opener = popup_openers[p.kind or ""]
		if not opener then error("unknown popup " .. tostring(p.kind), 0) end
		close_edit_popup()
		local cap = capture_popup(function() opener(tab, p) end, true)
		if not cap then error("the dialog did not open", 0) end
		edit_popup = { kind = p.kind, cap = cap }
		return popup_state()
	end
	if not edit_popup then error("no dialog is open", 0) end
	local ctls = edit_popup.cap.controls
	if action == "pick" then
		local ctl = ctls[p.name or ""]
		if not ctl then error("no control " .. tostring(p.name), 0) end
		with_popup(function()
			if p.name == "notableDB" then
				local want = tonumber(p.value)
				for i, node in ipairs(ctl.list or {}) do
					if node.id == want then ctl:SelectIndex(i) ctl.selIndex = i break end
				end
			elseif p.sel ~= nil then
				ctl:SetSel(tonumber(p.sel) or 1)
			elseif p.text ~= nil then
				ctl:SetText(tostring(p.text), true)
			elseif p.state ~= nil then
				ctl.state = p.state and true or false
				if ctl.changeFunc then ctl.changeFunc(ctl.state) end
			end
		end)
		return popup_state()
	end
	if action == "apply" then
		local btn = ctls[p.button or "save"]
		if not btn or not btn.onClick then error("no button " .. tostring(p.button or "save"), 0) end
		local en = btn.enabled
		if type(en) == "function" then en = en(btn) end
		if en == false then error("button is disabled", 0) end
		local ok, err = pcall(with_popup, function() btn.onClick() end)
		close_edit_popup()
		if not ok then error(err, 0) end
		if not tab.displayItem then error("the dialog closed the item", 0) end
		return edit_state(p)
	end
	if action == "tip" then
		-- the hovered notable's effect, without changing the selection
		local ctl = ctls[p.name or ""]
		if not ctl or not ctl.list then error("no control " .. tostring(p.name), 0) end
		local want = tonumber(p.value)
		for i, node in ipairs(ctl.list) do
			if node.id == want then return { id = node.id, tooltip = notable_tooltip(ctl, i, node) } end
		end
		error("no node " .. tostring(p.value), 0)
	end
	if action == "cancel" then
		close_edit_popup()
		return edit_state(p)
	end
	error("unknown action " .. tostring(action), 0)
end

-- ---------------------------------------------------------------------------
-- Items: list management (ItemListControl's own buttons) and the databases
-- ---------------------------------------------------------------------------

local function item_list_control(tab)
	local ctl = tab.controls.itemList
	if not ctl then error("ItemsTab has no item list control", 0) end
	return ctl
end

function M.sort_items()
	local b = ensure_build()
	item_list_control(b.itemsTab).controls.sort.onClick()
	return commit(b)
end

function M.delete_unused_items()
	local b = ensure_build()
	local tab = b.itemsTab
	local before = #tab.itemOrderList
	item_list_control(tab).controls.deleteUnused.onClick()
	local r = commit(b)
	r.deleted = before - #tab.itemOrderList
	return r
end

-- Del All asks through main:OpenConfirmPopup; the page already asked.
function M.delete_all_items()
	local b = ensure_build()
	local tab = b.itemsTab
	local m = main()
	local saved = m.OpenConfirmPopup
	m.OpenConfirmPopup = function(self, title, msg, confirmLabel, onConfirm) onConfirm() end
	local ok, err = pcall(function() item_list_control(tab).controls.deleteAll.onClick() end)
	m.OpenConfirmPopup = saved
	if not ok then error(err, 0) end
	return commit(b)
end

-- equip_primary{id, alt=bool}: the list's Ctrl+Click, through
-- ItemListControl:OnSelClick itself. It reads the modifier keys with the
-- engine's IsKeyDown (a headless stub), so that global answers CTRL (and
-- SHIFT for `alt`) for the duration of the call.
function M.equip_primary(p)
	local b = ensure_build()
	local tab = b.itemsTab
	local it = tab.items[tonumber(p and p.id or 0)]
	if not it then error("no item " .. tostring(p and p.id), 0) end
	local slotName = it:GetPrimarySlot()
	if not (slotName and tab.slots[slotName]) then error("item has no primary slot", 0) end
	local listCtl = item_list_control(tab)
	local savedKey = IsKeyDown
	IsKeyDown = function(key) return key == "CTRL" or (key == "SHIFT" and p.alt and true or false) end
	local ok, err = pcall(listCtl.OnSelClick, listCtl, nil, it.id, false)
	IsKeyDown = savedKey
	if not ok then error(err, 0) end
	local r = commit(b)
	local equippedIn
	for _, slot in ipairs(tab.orderedSlots) do
		if slot.selItemId == it.id and not slot.inactive then equippedIn = slot.slotName break end
	end
	r.slotName = equippedIn
	r.equipped = equippedIn ~= nil
	return r
end


local function wait_db(kind)
	local m = main()
	local db = kind == "rare" and m.rareDB or m.uniqueDB
	if not db then error("item database missing", 0) end
	local pumps = 0
	while db.loading and pumps < 2000 do
		frame()
		pumps = pumps + 1
	end
	if db.loading then error("item database still loading", 0) end
	return db
end

local function db_control(tab, kind)
	local ctl = kind == "rare" and tab.controls.rareDB or tab.controls.uniqueDB
	if not ctl then error("ItemsTab has no " .. tostring(kind) .. " database control", 0) end
	if not ctl.leaguesAndTypesLoaded then ctl:LoadLeaguesAndTypes() end
	return ctl
end

-- item_db_options{kind}: the ItemDBControl's filter dropdowns.
function M.item_db_options(p)
	local b = ensure_build()
	local kind = p and p.kind == "rare" and "rare" or "unique"
	wait_db(kind)
	local ctl = db_control(b.itemsTab, kind)
	local c = ctl.controls
	local out = { kind = kind, slot = dd_options(c.slot), type = dd_options(c.type), searchMode = dd_options(c.searchMode) }
	if kind == "unique" then
		out.league = dd_options(c.league)
		out.requirement = dd_options(c.requirement)
		out.obtainable = dd_options(c.obtainable)
		local sorts = {}
		for i, e in ipairs(ctl.sortDropList) do sorts[i] = { label = e.label, labelZh = tr(e.label), sortMode = e.sortMode, stat = e.stat } end
		out.sort = sorts
	end
	return out
end

local DB_STAT_SORT_MAX = 300

-- item_db{kind, slot, type, league, requirement, obtainable, searchMode,
-- query, sortMode, page, size}: ItemDBControl's own filters (indices into
-- item_db_options) and sort order; a stat sort runs GetMiscCalculator per
-- item and slot, so it is refused above DB_STAT_SORT_MAX matches.
function M.item_db(p)
	local b = ensure_build()
	p = p or {}
	local kind = p.kind == "rare" and "rare" or "unique"
	local db = wait_db(kind)
	local ctl = db_control(b.itemsTab, kind)
	local c = ctl.controls
	local function setSel(dd, v) if dd then dd.selIndex = math.max(1, math.min(#dd.list, tonumber(v) or 1)) end end
	setSel(c.slot, p.slot)
	setSel(c.type, p.type)
	setSel(c.league, p.league)
	setSel(c.requirement, p.requirement)
	setSel(c.obtainable, p.obtainable)
	setSel(c.searchMode, p.searchMode)
	c.search.buf = type(p.query) == "string" and p.query or ""
	if c.sort then
		local mode = "NAME"
		for _, e in ipairs(ctl.sortDropList) do
			if e.sortMode == p.sortMode then mode = e.sortMode end
		end
		ctl:SetSortMode(mode)
	end
	-- count first: the stat sort is the expensive part
	local hits = 0
	for _, it in pairs(db.list) do
		if ctl:DoesItemMatchFilters(it) then hits = hits + 1 end
	end
	local statSort = ctl.sortDetail and ctl.sortDetail.stat and true or false
	if statSort and hits > DB_STAT_SORT_MAX then
		return { kind = kind, total = hits, tooMany = true, max = DB_STAT_SORT_MAX, page = 1, size = 0, items = {} }
	end
	run_list_builder(ctl)
	local list = ctl.list or {}
	local page = math.max(1, tonumber(p.page) or 1)
	local size = math.min(200, math.max(1, tonumber(p.size) or 50))
	local out = {}
	local first = (page - 1) * size
	for i = first + 1, math.min(#list, first + size) do
		local it = list[i]
		local s = item_summary(it)
		s.raw = it.raw
		s.id = nil
		if statSort then s.measuredPower = it.measuredPower end
		out[#out + 1] = s
	end
	return { kind = kind, total = #list, page = page, size = size, items = out, statSort = statSort }
end

probe("classes.ItemsTab display item editing (SetDisplayItem/AddDisplayItem/CreateDisplayItemFromRaw/UpdateAffixControls/UpdateCustomControls)", function()
	local c = class_of("ItemsTab")
	return type(c) == "table" and type(c.SetDisplayItem) == "function" and type(c.AddDisplayItem) == "function" and type(c.CreateDisplayItemFromRaw) == "function"
		and type(c.UpdateAffixControls) == "function" and type(c.UpdateCustomControls) == "function"
end)
probe("classes.ItemsTab.ToggleDisplayItemModLine", function()
	local c = class_of("ItemsTab")
	return type(c) == "table" and type(c.ToggleDisplayItemModLine) == "function"
end, "itemModLineToggle")
probe("classes.ItemsTab dialogs (CraftItem/EditDisplayItemText/AnointDisplayItem/CorruptDisplayItem/AddCustomModifierToDisplayItem)", function()
	local c = class_of("ItemsTab")
	return type(c) == "table" and type(c.CraftItem) == "function" and type(c.EditDisplayItemText) == "function"
		and type(c.AnointDisplayItem) == "function" and type(c.CorruptDisplayItem) == "function" and type(c.AddCustomModifierToDisplayItem) == "function"
end)
probe("classes.ItemsTab.EnchantDisplayItem", function()
	local c = class_of("ItemsTab")
	return type(c) == "table" and type(c.EnchantDisplayItem) == "function"
end, "itemEnchant")
probe("classes.ItemsTab.AddCrucibleModifierToDisplayItem", function()
	local c = class_of("ItemsTab")
	return type(c) == "table" and type(c.AddCrucibleModifierToDisplayItem) == "function"
end, "itemCrucible")
-- The stat-difference block itself is AddItemTooltip's business (a separate
-- AddItemStatDifferences on beta, inline on master); the bridge only toggles
-- showStatDifferences and names the slot.
probe("classes.ItemsTab comparison (GetComparisonSlotNameForItem/GetEquippedSlotForItem/SortItemList)", function()
	local c = class_of("ItemsTab")
	return type(c) == "table" and type(c.GetComparisonSlotNameForItem) == "function" and type(c.GetEquippedSlotForItem) == "function"
		and type(c.SortItemList) == "function"
end)
probe("classes.ItemListControl.FindEquippedAbyssJewel", function()
	local l = class_of("ItemListControl")
	return type(l) == "table" and type(l.FindEquippedAbyssJewel) == "function"
end, "abyssJewels")
probe("classes.ItemListControl (FindSocketedJewel/OnSelClick) + ItemDBControl (DoesItemMatchFilters/LoadLeaguesAndTypes/SetSortMode/ListBuilder)", function()
	local l, d = class_of("ItemListControl"), class_of("ItemDBControl")
	return type(l) == "table" and type(l.FindSocketedJewel) == "function" and type(l.OnSelClick) == "function"
		and type(d) == "table" and type(d.DoesItemMatchFilters) == "function" and type(d.LoadLeaguesAndTypes) == "function"
		and type(d.SetSortMode) == "function" and type(d.ListBuilder) == "function"
end)
probe("classes.Item editing (Craft/BuildAndParseRaw/NormaliseQuality)", function()
	local c = class_of("Item")
	return type(c) == "table" and type(c.Craft) == "function" and type(c.BuildAndParseRaw) == "function" and type(c.NormaliseQuality) == "function"
end)
probe("main.OpenPopup/ClosePopup/OpenConfirmPopup + data.itemBaseTypeList/itemBaseLists/powerStatList", function()
	local m = launch.main
	return type(m.OpenPopup) == "function" and type(m.ClosePopup) == "function" and type(m.OpenConfirmPopup) == "function"
		and type(data.itemBaseTypeList) == "table" and type(data.itemBaseLists) == "table" and type(data.powerStatList) == "table"
end)
probe("itemLib.influenceInfo.all (item influences)", function()
	return type(itemLib) == "table" and type(itemLib.influenceInfo) == "table" and type(itemLib.influenceInfo.all) == "table"
end, "itemInfluence")
probe("classes.NotableDBControl.AddValueTooltip + ItemsTab.AppendAnointTooltip/anointItem/getAnoint", function()
	local n, i = class_of("NotableDBControl"), class_of("ItemsTab")
	return type(n) == "table" and type(n.AddValueTooltip) == "function"
		and type(i) == "table" and type(i.AppendAnointTooltip) == "function" and type(i.anointItem) == "function" and type(i.getAnoint) == "function"
end)
probe("classes.DropDownControl.SetSel/SelByValue + EditControl.SetText + ListControl.SelectIndex", function()
	local d, e, l = class_of("DropDownControl"), class_of("EditControl"), class_of("ListControl")
	return type(d) == "table" and type(d.SetSel) == "function" and type(d.SelByValue) == "function"
		and type(e) == "table" and type(e.SetText) == "function" and type(l) == "table" and type(l.SelectIndex) == "function"
end)

-- ---------------------------------------------------------------------------
-- POB's own Options dialog and update dialog
-- ---------------------------------------------------------------------------
-- main:OpenOptionsPopup builds every option as a control whose callback writes
-- the setting on `main`; its Save button applies the proxy/build path, writes
-- the manifest branch (the weekly-beta opt-in) and SaveSettings(). The page
-- reads those controls and hands back only the values the user changed; they
-- go through the same callbacks and the same Save. Nothing here knows what an
-- option means.

-- POB's DPI override reopens the dialog from its callback and scales the
-- classic window; the new window has its own zoom, so it is not offered.
local OPTION_SKIP = { dpiScaleOverride = true }

local function option_kind(ctl)
	if type(ctl) ~= "table" then return nil end
	if ctl.DropIndexToListIndex and ctl.list then return "dropdown" end
	if ctl.SetText and ctl.buf ~= nil then return "edit" end
	if ctl.GetDivVal then return "slider" end
	-- a CheckBoxControl's state stays nil until POB assigns it (an option never
	-- saved in Settings.xml), so it is recognised by its callback alone
	if ctl.changeFunc then return "check" end
	return nil
end

local function open_options()
	-- the dialog sizes itself from main.screenW/H, which the first OnFrame sets
	if not main().screenH then frame() end
	return capture_popup(function() main():OpenOptionsPopup() end, true)
end

local function read_options(cap)
	local controls = cap.controls
	local hdrApp, hdrBuild = controls["section-app-label"], controls["section-build-label"]
	local function num(v) return type(v) == "number" and v or 0 end
	-- POB lays the two sections out side by side when the screen is wide
	-- enough and one above the other otherwise; the build header's position
	-- says which, and so which section a control is in.
	local twoCol = hdrBuild and num(hdrBuild.x) >= 600
	-- A row's caption is the LabelControl POB anchors to the control's left
	-- ({"RIGHT", control, "LEFT"}); a checkbox carries its caption itself.
	local captionOf = {}
	for _, c in pairs(controls) do
		if type(c) == "table" and c.label ~= nil and not option_kind(c) and c.anchor and type(c.anchor.other) == "table" and c.anchor.point == "RIGHT" then
			captionOf[c.anchor.other] = c
		end
	end
	local items = {}
	for name, ctl in pairs(controls) do
		local kind = type(name) == "string" and not OPTION_SKIP[name] and name ~= "save" and name ~= "cancel" and option_kind(ctl)
		if kind then
			-- a control anchored to another one (the proxy URL box) sits on that row
			local row = ctl
			if ctl.anchor and type(ctl.anchor.other) == "table" and ctl.anchor.other ~= controls.sectionAnchor then row = ctl.anchor.other end
			local x, y = num(row.x), num(row.y)
			local build = hdrBuild and (twoCol and x >= 600 or (not twoCol and y > num(hdrBuild.y))) or false
			local lblCtl = captionOf[ctl]
			local label = lblCtl and lblCtl.label or ctl.label
			if type(label) == "function" then label = label() end
			label = type(label) == "string" and label or nil
			local tip = type(ctl.tooltipText) == "string" and ctl.tooltipText or nil
			local e = { name = name, kind = kind, section = build and "build" or "app", y = y, x = x, anchoredTo = row ~= ctl and row == controls.proxyType and "proxyType" or nil,
				label = label, labelZh = label and tr(label) or nil, tooltip = tip, tooltipZh = tip and tr(tip) or nil }
			if kind == "dropdown" then
				e.options = dd_options(ctl)
				e.sel = ctl.selIndex or 1
			elseif kind == "edit" then
				e.text = ctl.buf
			elseif kind == "check" then
				e.state = ctl.state and true or false
			elseif kind == "slider" then
				e.value = ctl.val
			end
			items[#items + 1] = e
		end
	end
	table.sort(items, function(a, b)
		if a.section ~= b.section then return a.section == "app" end
		if a.y ~= b.y then return a.y < b.y end
		return a.x < b.x
	end)
	local function title(ctl)
		local l = ctl and ctl.label
		if type(l) == "function" then l = l() end
		return type(l) == "string" and l or nil
	end
	return {
		sections = {
			{ id = "app", title = title(hdrApp), titleZh = title(hdrApp) and tr(title(hdrApp)) or nil },
			{ id = "build", title = title(hdrBuild), titleZh = title(hdrBuild) and tr(title(hdrBuild)) or nil },
		},
		options = items,
		branch = launch.versionBranch,
		version = launch.versionNumber,
	}
end

-- pob_options: the Options dialog's controls, grouped as POB groups them.
function M.pob_options()
	local cap = open_options()
	local ok, res = pcall(read_options, cap)
	popup_discard(cap)
	if not ok then error(res, 0) end
	return res
end

-- set_pob_options{values={name=value}}: each value through its control's own
-- callback, then the dialog's Save. A failure puts the old values back
-- through the dialog's Cancel.
function M.set_pob_options(p)
	local values = p and p.values
	if type(values) ~= "table" then error("params.values required", 0) end
	local cap = open_options()
	local controls = cap.controls
	local ok, err = pcall(function()
		for name, v in pairs(values) do
			local ctl = controls[name]
			local kind = type(name) == "string" and not OPTION_SKIP[name] and option_kind(ctl)
			if not kind then error("no option " .. tostring(name), 0) end
			if kind == "dropdown" then
				ctl:SetSel(tonumber(v) or 1)
			elseif kind == "edit" then
				ctl:SetText(tostring(v), true)
			elseif kind == "check" then
				ctl.state = v and true or false
				if ctl.changeFunc then ctl.changeFunc(ctl.state) end
			elseif kind == "slider" then
				ctl.val = math.max(0, math.min(1, tonumber(v) or 0))
				if ctl.changeFunc then ctl.changeFunc(ctl.val) end
			end
		end
		controls.save.onClick()
	end)
	if not ok then
		pcall(function() controls.cancel.onClick() end)
		popup_discard(cap)
		error(err, 0)
	end
	popup_discard(cap)
	-- report the saved state from a fresh dialog, as reopening Options would show it
	local cap2 = open_options()
	local ok2, r = pcall(read_options, cap2)
	popup_discard(cap2)
	if not ok2 then error(r, 0) end
	r.saved = true
	return r
end

-- pob_update_info: what POB's "Update Ready" dialog shows -- the changelog
-- entries newer than this version (main:OpenUpdatePopup reads changelog.txt,
-- which the update check has just downloaded).
function M.pob_update_info()
	local cap = capture_popup(function() main():OpenUpdatePopup() end, false)
	local lines = {}
	local list = cap and cap.controls.changeLog and cap.controls.changeLog.list or {}
	-- On the beta branch the version never equals a changelog heading, so POB
	-- lists the whole file; the newest entries are what matters.
	local MAX = 400
	for i, e in ipairs(list) do
		if i > MAX then break end
		lines[i] = { text = e[1], height = e.height }
	end
	return {
		available = launch.updateAvailable,
		version = launch.versionNumber,
		branch = launch.versionBranch,
		lines = lines,
		truncated = #list > MAX,
	}
end

probe("main.OpenOptionsPopup/SaveSettings/SetManifestBranch/OpenUpdatePopup + launch.CheckForUpdate/ApplyUpdate", function()
	local m = launch.main
	return type(m.OpenOptionsPopup) == "function" and type(m.SaveSettings) == "function" and type(m.SetManifestBranch) == "function"
		and type(m.OpenUpdatePopup) == "function" and type(launch.CheckForUpdate) == "function" and type(launch.ApplyUpdate) == "function"
end)

-- ---------------------------------------------------------------------------
-- Skills (socket groups, gems, skill sets)
-- ---------------------------------------------------------------------------

-- The socket-group slot dropdown POB offers (SkillsTab's groupSlotDropList is
-- file-local; these are the item slots a group can be tied to).
local group_slot_names = { "Weapon 1", "Weapon 2", "Weapon 1 Swap", "Weapon 2 Swap", "Helmet", "Body Armour", "Gloves", "Boots", "Amulet", "Ring 1", "Ring 2", "Ring 3", "Belt" }

local function gem_summary(g, i)
	local gd = g.gemData
	local ge = g.grantedEffect or (gd and gd.grantedEffect)
	local name = (ge and ge.name) or (gd and gd.name) or g.nameSpec
	return {
		index = i,
		nameSpec = g.nameSpec,
		name = name,
		nameZh = tr(name),
		gemId = g.gemId or (gd and gd.id) or nil,
		skillId = g.skillId,
		level = g.level,
		quality = g.quality,
		enabled = g.enabled and true or false,
		enableGlobal1 = g.enableGlobal1 and true or false,
		enableGlobal2 = g.enableGlobal2 and true or false,
		count = g.count,
		errMsg = g.errMsg,
		color = g.color,
		support = ge and ge.support and true or false,
		hasGlobalEffect = ge and ge.hasGlobalEffect and true or false,
		naturalMaxLevel = gd and gd.naturalMaxLevel or nil,
		reqLevel = g.reqLevel,
		matchesSocket = g.matchesSocket and true or false,
		fromItem = g.fromItem and true or false,
		fromNode = g.fromNode and true or false,
	}
end

local function group_summary(g, i, b)
	local gems = {}
	for j, gem in ipairs(g.gemList or {}) do gems[j] = gem_summary(gem, j) end
	local skills = {}
	for j, as in ipairs(g.displaySkillList or {}) do
		local ge = as.activeEffect and as.activeEffect.grantedEffect
		skills[j] = { index = j, name = ge and ge.name, nameZh = ge and tr(ge.name) }
	end
	return {
		index = i,
		label = g.label or "",
		displayLabel = g.displayLabel,
		-- a user-typed label is shown verbatim; only POB's generated one is translated
		displayLabelZh = (g.label and g.label ~= "") and g.displayLabel or tr(g.displayLabel),
		enabled = g.enabled and true or false,
		includeInFullDPS = g.includeInFullDPS and true or false,
		slot = g.slot,
		slotEnabled = g.slotEnabled ~= false,
		source = g.source and true or false,
		sourceName = g.sourceItem and g.sourceItem.name or (g.sourceNode and g.sourceNode.dn) or nil,
		mainActiveSkill = g.mainActiveSkill or 1,
		isMain = (b.mainSocketGroup == i),
		gems = gems,
		skills = skills,
	}
end

function M.list_skills()
	local b = ensure_build()
	local tab = b.skillsTab
	local groups = {}
	for i, g in ipairs(tab.socketGroupList) do groups[i] = group_summary(g, i, b) end
	local sets = {}
	for i, id in ipairs(tab.skillSetOrderList or {}) do
		local set = tab.skillSets[id]
		sets[i] = { id = id, title = set and set.title or nil }
	end
	local slots = {}
	for i, n in ipairs(group_slot_names) do slots[i] = { name = n, label = n, labelZh = tr(n) } end
	return {
		groups = groups,
		skillSets = sets,
		activeSkillSetId = tab.activeSkillSetId,
		mainSocketGroup = b.mainSocketGroup,
		slotOptions = slots,
		defaultGemLevel = tab.defaultGemLevel,
		defaultGemQuality = tab.defaultGemQuality,
		rev = b.outputRevision,
	}
end

local function group_at(tab, i)
	local g = tab.socketGroupList[tonumber(i or 0)]
	if not g then error("no socket group " .. tostring(i), 0) end
	return g
end

-- After any edit to a group: what the gem-slot callbacks do before the frame.
local function skills_committed(b, g)
	local tab = b.skillsTab
	if g then tab:ProcessSocketGroup(g) end
	tab:AddUndoState()
	local r = commit(b)
	-- PoE1 re-runs its socket bookkeeping here; PoE2's SkillsTab has none.
	if tab.UpdateSocketGroups then tab:UpdateSocketGroups() end
	return r
end

-- add_group{label?, slot?, gems?=[{nameSpec,level?,quality?,enabled?,count?}]}:
-- PasteSocketGroup's shape without the clipboard.
function M.add_group(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local g = { label = p and p.label or "", enabled = true, gemList = {}, includeInFullDPS = false }
	if p and p.slot and p.slot ~= "" then g.slot = p.slot end
	for _, spec in ipairs(p and p.gems or {}) do
		g.gemList[#g.gemList + 1] = {
			nameSpec = spec.nameSpec or "", level = tonumber(spec.level) or 20, quality = tonumber(spec.quality) or 0,
			enabled = spec.enabled ~= false, count = tonumber(spec.count) or 1, enableGlobal1 = true, enableGlobal2 = true, new = true,
		}
	end
	table.insert(tab.socketGroupList, g)
	if b.mainSocketGroup == nil or b.mainSocketGroup == 0 then b.mainSocketGroup = #tab.socketGroupList end
	local r = skills_committed(b, g)
	r.index = #tab.socketGroupList
	return r
end

function M.delete_group(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local i = tonumber(p and p.index)
	group_at(tab, i)
	table.remove(tab.socketGroupList, i)
	if b.mainSocketGroup and b.mainSocketGroup > #tab.socketGroupList then b.mainSocketGroup = math.max(1, #tab.socketGroupList) end
	if tab.displayGroup and tab.SetDisplayGroup then pcall(tab.SetDisplayGroup, tab, tab.socketGroupList[1]) end
	return skills_committed(b, nil)
end

-- set_group{index, label?, enabled?, includeInFullDPS?, slot? ("" = none), mainActiveSkill?}
function M.set_group(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local g = group_at(tab, p and p.index)
	if p.label ~= nil then g.label = p.label end
	if p.enabled ~= nil then g.enabled = p.enabled and true or false end
	if p.includeInFullDPS ~= nil then g.includeInFullDPS = p.includeInFullDPS and true or false end
	if p.slot ~= nil then g.slot = (p.slot ~= "" and p.slot) or nil end
	if p.mainActiveSkill ~= nil then g.mainActiveSkill = tonumber(p.mainActiveSkill) or 1 end
	return skills_committed(b, g)
end

function M.move_group(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local from, to = tonumber(p and p.from), tonumber(p and p.to)
	group_at(tab, from)
	if not to or to < 1 or to > #tab.socketGroupList then error("bad target index", 0) end
	local g = table.remove(tab.socketGroupList, from)
	table.insert(tab.socketGroupList, to, g)
	if b.mainSocketGroup == from then b.mainSocketGroup = to
	elseif from < b.mainSocketGroup and to >= b.mainSocketGroup then b.mainSocketGroup = b.mainSocketGroup - 1
	elseif from > b.mainSocketGroup and to <= b.mainSocketGroup then b.mainSocketGroup = b.mainSocketGroup + 1 end
	return skills_committed(b, nil)
end

-- add_gem{group, nameSpec|gemId, level?, quality?, enabled?, count?}
function M.add_gem(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local g = group_at(tab, p and p.group)
	local gem = {
		nameSpec = p.nameSpec or "", level = tonumber(p.level) or 20, quality = tonumber(p.quality) or tab.defaultGemQuality or 0,
		enabled = p.enabled ~= false, count = tonumber(p.count) or 1, enableGlobal1 = true, enableGlobal2 = true, new = true,
	}
	if p.gemId and b.data.gems[p.gemId] then
		gem.gemId = p.gemId
		gem.nameSpec = b.data.gems[p.gemId].name
	end
	if gem.nameSpec == "" and not gem.gemId then error("nameSpec or gemId required", 0) end
	local at = tonumber(p.index)
	if at and at >= 1 and at <= #g.gemList + 1 then table.insert(g.gemList, at, gem) else table.insert(g.gemList, gem) end
	-- POB picks the natural max level for a fresh gem (CreateGemSlot's flow).
	tab:ProcessSocketGroup(g)
	if gem.gemData and not p.level then
		gem.level = tab:ProcessGemLevel(gem.gemData)
		tab:ProcessSocketGroup(g)
	end
	local r = skills_committed(b, g)
	r.gem = gem_summary(gem, #g.gemList)
	return r
end

-- set_gem{group, index, nameSpec?|gemId?, level?, quality?, enabled?, enableGlobal1?, enableGlobal2?, count?}
function M.set_gem(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local g = group_at(tab, p and p.group)
	local gem = g.gemList[tonumber(p.index or 0)]
	if not gem then error("no gem " .. tostring(p.index), 0) end
	if p.gemId ~= nil then
		if p.gemId == "" or p.gemId == false then
			gem.gemId, gem.skillId, gem.gemData = nil, nil, nil
		else
			if not b.data.gems[p.gemId] then error("unknown gemId " .. tostring(p.gemId), 0) end
			gem.gemId = p.gemId
			gem.skillId = nil
			gem.nameSpec = b.data.gems[p.gemId].name
		end
	elseif p.nameSpec ~= nil then
		gem.gemId, gem.skillId, gem.gemData = nil, nil, nil
		gem.nameSpec = p.nameSpec
	end
	if p.level ~= nil then gem.level = tonumber(p.level) or gem.level end
	if p.quality ~= nil then gem.quality = tonumber(p.quality) or gem.quality end
	if p.enabled ~= nil then gem.enabled = p.enabled and true or false end
	if p.enableGlobal1 ~= nil then gem.enableGlobal1 = p.enableGlobal1 and true or false end
	if p.enableGlobal2 ~= nil then gem.enableGlobal2 = p.enableGlobal2 and true or false end
	if p.count ~= nil then gem.count = tonumber(p.count) or gem.count end
	local r = skills_committed(b, g)
	r.gem = gem_summary(gem, tonumber(p.index))
	return r
end

function M.delete_gem(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local g = group_at(tab, p and p.group)
	local i = tonumber(p.index or 0)
	if not g.gemList[i] then error("no gem " .. tostring(p.index), 0) end
	table.remove(g.gemList, i)
	return skills_committed(b, g)
end

function M.move_gem(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local g = group_at(tab, p and p.group)
	local from, to = tonumber(p.from), tonumber(p.to)
	if not g.gemList[from or 0] or not to or to < 1 or to > #g.gemList then error("bad gem index", 0) end
	local gem = table.remove(g.gemList, from)
	table.insert(g.gemList, to, gem)
	return skills_committed(b, g)
end

-- gem_tooltip{group, index}: POB's own gem tooltip (Classes/GemTooltip).
function M.gem_tooltip(p)
	local b = ensure_build()
	local g = group_at(b.skillsTab, p and p.group)
	local gem = g.gemList[tonumber(p.index or 0)]
	if not gem then error("no gem " .. tostring(p.index), 0) end
	local gt = require("Classes.GemTooltip")
	local tt = make("Tooltip")
	without_wrap(function() gt.AddGemTooltip(tt, b, gem) end)
	return { lines = tooltip_lines(tt), header = tt.tooltipHeader }
end

function M.group_tooltip(p)
	local b = ensure_build()
	local g = group_at(b.skillsTab, p and p.index)
	local tt = make("Tooltip")
	without_wrap(function() b.skillsTab:AddSocketGroupTooltip(tt, g) end)
	return { lines = tooltip_lines(tt) }
end

-- gem_search{query, limit?, supportOnly?, activeOnly?}: the gems POB's own
-- dropdown would list (same filters as GemSelectControl:PopulateGemList with
-- "ALL" support types), matched on English and translated names.
local gem_index
local function gem_list(b)
	if gem_index and gem_index.data == b.data then return gem_index.list end
	local list = {}
	for gemId, gd in pairs(b.data.gems) do
		local ge = gd.grantedEffect
		if ge and not ge.hideFromGemList then
			local zh = tr(gd.name)
			local tags = {}
			for tg, on in pairs(gd.tags or {}) do if on then tags[#tags + 1] = tg end end
			table.sort(tags)
			list[#list + 1] = {
				gemId = gemId, name = gd.name, nameZh = zh, key = gd.name:lower(), keyZh = (zh or ""):lower(),
				support = ge.support and true or false, legacy = ge.legacy and true or false,
				exceptional = (gd.tagString or ""):match("Exceptional") and true or false,
				color = ge.color, tags = tags, naturalMaxLevel = gd.naturalMaxLevel,
			}
		end
	end
	table.sort(list, function(a, c) return a.key < c.key end)
	gem_index = { data = b.data, list = list }
	return list
end

-- ---- Gem Options section and the per-group extras -------------------------
-- Everything below drives SkillsTab's own controls (their list/selFunc/
-- changeFunc/onClick), the way a click would.

local function dropdown_options(c, key)
	local out = {}
	for i, v in ipairs(c and c.list or {}) do
		out[i] = { value = v[key], label = v.label, labelZh = tr(v.label), description = v.description }
	end
	return out
end

local function select_dropdown(c, key, value)
	for i, v in ipairs(c.list or {}) do
		if v[key] == value then
			c.selIndex = i
			if c.selFunc then c.selFunc(i, v) end
			return
		end
	end
	error("unknown option " .. tostring(value), 0)
end

-- get_gem_options{}: SkillsTab's "Gem Options" (sort gems by DPS and by what,
-- default gem level / quality, which supports to show, legacy gems).
function M.get_gem_options()
	local tab = ensure_build().skillsTab
	local c = tab.controls
	return {
		sortGemsByDPS = tab.sortGemsByDPS and true or false,
		sortGemsByDPSField = tab.sortGemsByDPSField,
		sortFields = dropdown_options(c.sortGemsByDPSFieldControl, "type"),
		defaultGemLevel = tab.defaultGemLevel,
		defaultGemLevels = dropdown_options(c.defaultLevel, "gemLevel"),
		defaultGemQuality = tab.defaultGemQuality,
		showSupportGemTypes = tab.showSupportGemTypes,
		supportGemTypes = dropdown_options(c.showSupportGemTypes, "show"),
		showLegacyGems = tab.showLegacyGems and true or false,
	}
end

-- set_gem_options{sortGemsByDPS?, sortGemsByDPSField?, defaultGemLevel?, defaultGemQuality?, showSupportGemTypes?, showLegacyGems?}
function M.set_gem_options(p)
	local tab = ensure_build().skillsTab
	local c = tab.controls
	p = p or {}
	if p.sortGemsByDPS ~= nil then
		c.sortGemsByDPS.state = p.sortGemsByDPS and true or false
		c.sortGemsByDPS.changeFunc(c.sortGemsByDPS.state)
	end
	if p.sortGemsByDPSField ~= nil then select_dropdown(c.sortGemsByDPSFieldControl, "type", p.sortGemsByDPSField) end
	if p.defaultGemLevel ~= nil then select_dropdown(c.defaultLevel, "gemLevel", p.defaultGemLevel) end
	if p.defaultGemQuality ~= nil then c.defaultQuality:SetText(tostring(math.floor(tonumber(p.defaultGemQuality) or 0)), true) end
	if p.showSupportGemTypes ~= nil then select_dropdown(c.showSupportGemTypes, "show", p.showSupportGemTypes) end
	if p.showLegacyGems ~= nil then
		c.showLegacyGems.state = p.showLegacyGems and true or false
		c.showLegacyGems.changeFunc(c.showLegacyGems.state)
	end
	return M.get_gem_options()
end

probe("SkillsTab Gem Options controls (sortGemsByDPS/sortGemsByDPSFieldControl/defaultLevel/defaultQuality/showSupportGemTypes/showLegacyGems)", function()
	local b = build()
	if not (b and b.skillsTab) then return true end
	local c = b.skillsTab.controls
	return type(c.sortGemsByDPS) == "table" and type(c.sortGemsByDPS.changeFunc) == "function"
		and type(c.sortGemsByDPSFieldControl) == "table" and type(c.sortGemsByDPSFieldControl.selFunc) == "function"
		and type(c.defaultLevel) == "table" and type(c.defaultLevel.selFunc) == "function"
		and type(c.defaultQuality) == "table" and type(c.defaultQuality.SetText) == "function"
		and type(c.showSupportGemTypes) == "table" and type(c.showLegacyGems) == "table" and type(c.showLegacyGems.changeFunc) == "function"
end)

-- The group's extras need it to be SkillsTab's display group: the controls'
-- closures read self.displayGroup.
-- a control's shown/enabled: a function, a flag, or absent (= true)
local function ctrl_flag(ctrl, field)
	local v = ctrl and ctrl[field]
	if type(v) == "function" then return v() and true or false end
	if v == nil then return true end
	return v and true or false
end
local function on_enabled(ctrl) return ctrl_flag(ctrl, "enabled") end

local function display_group(tab, g)
	if tab.displayGroup ~= g then tab:SetDisplayGroup(g) end
end

-- set_group_count{index, count}: "Count:" of a group granted by an item or node.
function M.set_group_count(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local g = group_at(tab, p and p.index)
	if not g.source then error("only a group from an item or passive has a count", 0) end
	display_group(tab, g)
	tab.controls.groupCount:SetText(tostring(tonumber(p.count) or 1), true)
	return skills_committed(b, g)
end

-- group_extras{index}: what the group detail shows beyond set_group's fields
-- (count, imbued support, Optimise Sockets) and whether each applies.
function M.group_extras(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local g = group_at(tab, p and p.index)
	display_group(tab, g)
	local c = tab.controls
	local on = ctrl_flag
	local imbued = c.imbuedSupport and {
		shown = on(c.imbuedSupportLabel, "shown"),
		enabled = on(c.imbuedSupport, "enabled"),
		name = g.imbuedSupport,
		nameZh = g.imbuedSupport and tr(g.imbuedSupport) or nil,
	} or nil
	return {
		index = tonumber(p.index),
		count = g.groupCount or 1,
		countShown = g.source ~= nil,
		imbued = imbued,
		optimiseSockets = c.optimiseSockets and { shown = on(c.optimiseSockets, "shown"), enabled = on(c.optimiseSockets, "enabled") } or nil,
	}
end

-- set_imbued_support{index, gemId|nil}: the Imbued Support gem picker and its
-- "x" (PoE1). The picker's own change function does the work.
function M.set_imbued_support(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local c = tab.controls
	if not c.imbuedSupport then error("this POB has no imbued supports", 0) end
	local g = group_at(tab, p and p.index)
	display_group(tab, g)
	if not on_enabled(c.imbuedSupport) then error("imbued support is not available for this group", 0) end
	if p.gemId and p.gemId ~= "" then
		local gem = b.data.gems[p.gemId]
		if not gem or not (gem.grantedEffect and gem.grantedEffect.support) then error("not a support gem: " .. tostring(p.gemId), 0) end
		c.imbuedSupport.gemChangeFunc(p.gemId, nil, nil, true, g.slot)
	else
		c.imbuedSupportClear.onClick()
	end
	return skills_committed(b, g)
end

-- optimise_sockets{index}: "Optimise Sockets" -- rebuild the item's sockets
-- to match the groups assigned to it (PoE1).
function M.optimise_sockets(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local c = tab.controls
	if not c.optimiseSockets then error("this POB has no Optimise Sockets", 0) end
	local g = group_at(tab, p and p.index)
	display_group(tab, g)
	if not on_enabled(c.optimiseSockets) then error("nothing to optimise for this group", 0) end
	c.optimiseSockets.onClick()
	b.itemsTab:PopulateSlots()
	return skills_committed(b, g)
end

-- copy_group{index} -> {text}: CopySocketGroup's clipboard text (Copy is
-- caught so the page puts it on the clipboard itself).
function M.copy_group(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local g = group_at(tab, p and p.index)
	local text
	local saved = Copy
	Copy = function(s) text = s end
	local ok, err = pcall(tab.CopySocketGroup, tab, g)
	Copy = saved
	if not ok then error(err, 0) end
	return { text = text or "" }
end

-- paste_group{text}: PasteSocketGroup on that text (it adds the group, selects
-- it and flags the build; nothing is added when no gem line parses).
function M.paste_group(p)
	local b = ensure_build()
	local tab = b.skillsTab
	if type(p) ~= "table" or type(p.text) ~= "string" then error("params.text required", 0) end
	local before = #tab.socketGroupList
	local saved = Paste
	Paste = function() return nil end
	local ok, err = pcall(tab.PasteSocketGroup, tab, p.text)
	Paste = saved
	if not ok then error(err, 0) end
	if #tab.socketGroupList == before then error("no gems found in the pasted text", 0) end
	local r = skills_committed(b, tab.socketGroupList[#tab.socketGroupList])
	r.index = #tab.socketGroupList
	return r
end

probe("SkillsTab.SetDisplayGroup/CopySocketGroup/PasteSocketGroup + groupCount control", function()
	local c = class_of("SkillsTab")
	local b = build()
	return type(c) == "table" and type(c.SetDisplayGroup) == "function" and type(c.CopySocketGroup) == "function"
		and type(c.PasteSocketGroup) == "function" and (not (b and b.skillsTab) or type(b.skillsTab.controls.groupCount) == "table")
end)
probe("SkillsTab imbued support + Optimise Sockets controls (PoE1)", function()
	local b = build()
	if GAME == "poe2" then return false end
	if not (b and b.skillsTab) then return true end
	local c = b.skillsTab.controls
	return type(c.imbuedSupport) == "table" and type(c.imbuedSupport.gemChangeFunc) == "function"
		and type(c.imbuedSupportClear) == "table" and type(c.imbuedSupportClear.onClick) == "function"
		and type(c.optimiseSockets) == "table" and type(c.optimiseSockets.onClick) == "function"
end, "skillImbued")

function M.gem_search(p)
	local b = ensure_build()
	local q = p and type(p.query) == "string" and p.query:lower() or ""
	local limit = math.min(100, tonumber(p and p.limit) or 30)
	local showLegacy = b.skillsTab.showLegacyGems
	local out = {}
	for _, e in ipairs(gem_list(b)) do
		if (showLegacy or not e.legacy)
			and (not p or not p.supportOnly or e.support) and (not p or not p.activeOnly or not e.support)
			and (q == "" or e.key:find(q, 1, true) or e.keyZh:find(q, 1, true)) then
			out[#out + 1] = { gemId = e.gemId, name = e.name, nameZh = e.nameZh, support = e.support, color = e.color, tags = e.tags, naturalMaxLevel = e.naturalMaxLevel, exceptional = e.exceptional }
			if #out >= limit then break end
		end
	end
	return { gems = out }
end

function M.set_skill_set(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local id = tonumber(p and p.id)
	if not id or not tab.skillSets[id] then error("no skill set " .. tostring(p and p.id), 0) end
	tab:SetActiveSkillSet(id)
	tab:AddUndoState()
	return commit(b)
end

function M.new_skill_set(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local set = tab:NewSkillSet()
	set.title = p and p.title or nil
	if p and p.copyCurrent then
		set.socketGroupList = copyTable(tab.socketGroupList)
	end
	table.insert(tab.skillSetOrderList, set.id)
	tab:SetActiveSkillSet(set.id)
	tab:AddUndoState()
	local r = commit(b)
	r.id = set.id
	return r
end

function M.rename_skill_set(p)
	local b = ensure_build()
	local set = b.skillsTab.skillSets[tonumber(p and p.id or 0)]
	if not set then error("no skill set " .. tostring(p and p.id), 0) end
	set.title = p.title
	return commit(b)
end

function M.delete_skill_set(p)
	local b = ensure_build()
	local tab = b.skillsTab
	local id = tonumber(p and p.id)
	if not id or not tab.skillSets[id] then error("no skill set " .. tostring(p and p.id), 0) end
	if #tab.skillSetOrderList <= 1 then error("cannot delete the last skill set", 0) end
	for i, v in ipairs(tab.skillSetOrderList) do
		if v == id then table.remove(tab.skillSetOrderList, i) break end
	end
	if tab.activeSkillSetId == id then tab:SetActiveSkillSet(tab.skillSetOrderList[1]) end
	tab.skillSets[id] = nil
	tab:AddUndoState()
	return commit(b)
end

probe("classes.SkillsTab.ProcessSocketGroup/FindSkillGem/ProcessGemLevel/AddSocketGroupTooltip", function()
	local c = class_of("SkillsTab")
	return type(c) == "table" and type(c.ProcessSocketGroup) == "function"
		and type(c.FindSkillGem) == "function" and type(c.ProcessGemLevel) == "function" and type(c.AddSocketGroupTooltip) == "function"
end)
probe("classes.SkillsTab skill sets (NewSkillSet/SetActiveSkillSet)", function()
	local c = class_of("SkillsTab")
	return type(c) == "table" and type(c.NewSkillSet) == "function" and type(c.SetActiveSkillSet) == "function"
end)
probe("Classes.GemTooltip.AddGemTooltip", function()
	local ok, gt = pcall(require, "Classes.GemTooltip")
	return ok and type(gt) == "table" and type(gt.AddGemTooltip) == "function"
end)
probe("data.gems / gemForSkill", function()
	return type(data) == "table" and type(data.gems) == "table" and type(data.gemForSkill) == "table"
end)

-- ---------------------------------------------------------------------------
-- Configuration
-- ---------------------------------------------------------------------------

local function config_var_list()
	local ok, list = pcall(require, "Modules.ConfigOptions")
	if not ok or type(list) ~= "table" then error("Modules/ConfigOptions not available", 0) end
	return list
end

local function config_set(tab)
	local set = tab.configSets and tab.configSets[tab.activeConfigSetId]
	if not set then error("no active config set", 0) end
	return set
end

local function control_prop(ctl, key)
	if not ctl then return nil end
	local v = ctl[key]
	if type(v) == "function" then
		local ok, r = pcall(v, ctl)
		return ok and r or nil
	end
	return v
end

-- list_config: POB's option list (Modules/ConfigOptions) in its sections,
-- each option's current value/placeholder, and whether POB would show it
-- right now (the control's own `shown` closure: ifCond / ifOption / ...).
function M.list_config()
	local b = ensure_build()
	local tab = b.configTab
	local set = config_set(tab)
	local sections, cur = {}, nil
	for _, vd in ipairs(config_var_list()) do
		if vd.section then
			cur = { name = vd.section, nameZh = tr(vd.section), col = vd.col or 1, items = {} }
			sections[#sections + 1] = cur
		elseif vd.var and cur then
			local ctl = tab.varControls and tab.varControls[vd.var]
			local visible = true
			if ctl and ctl.IsShown then
				local ok, shown = pcall(ctl.IsShown, ctl)
				visible = ok and shown and true or false
			end
			local item = {
				var = vd.var, type = vd.type,
				label = vd.label, labelZh = tr(vd.label),
				value = set.input[vd.var], placeholder = set.placeholder[vd.var],
				visible = visible,
				tooltip = control_prop(ctl, "tooltipText"),
			}
			if item.tooltip then item.tooltipZh = tr(item.tooltip) end
			if vd.type == "list" and vd.list then
				local opts = {}
				for i, o in ipairs(vd.list) do
					opts[i] = { val = o.val, label = o.label, labelZh = tr(o.label) }
				end
				item.list = opts
			end
			cur.items[#cur.items + 1] = as_object(item)
		end
	end
	local custom = {}
	for i, blk in ipairs(set.customModsList or {}) do
		custom[i] = { title = blk.title, text = blk.text or "", enabled = blk.enabled ~= false }
	end
	local sets = {}
	for i, id in ipairs(tab.configSetOrderList or {}) do
		local s = tab.configSets[id]
		sets[i] = { id = id, title = s and s.title or nil }
	end
	return { sections = sections, customMods = custom, configSets = sets, activeConfigSetId = tab.activeConfigSetId, rev = b.outputRevision }
end

local function config_committed(b)
	local tab = b.configTab
	tab:AddUndoState()
	tab:BuildModList()
	pcall(tab.UpdateControls, tab)
	return commit(b)
end

local function find_var(var)
	for _, vd in ipairs(config_var_list()) do
		if vd.var == var then return vd end
	end
	error("unknown config var " .. tostring(var), 0)
end

-- set_config{var, value}: the control callback for that option's type.
function M.set_config(p)
	local b = ensure_build()
	local tab = b.configTab
	local set = config_set(tab)
	local vd = find_var(p and p.var)
	local v = p.value
	if vd.type == "check" then
		v = v and true or false
	elseif vd.type == "count" or vd.type == "integer" or vd.type == "countAllowZero" or vd.type == "float" then
		v = (v ~= nil and v ~= "") and tonumber(v) or nil
	elseif vd.type == "list" then
		local okv = false
		for _, o in ipairs(vd.list or {}) do
			if o.val == v or tostring(o.val) == tostring(v) then v = o.val; okv = true; break end
		end
		if not okv then error("value not in list for " .. vd.var, 0) end
	elseif vd.type == "text" then
		v = v ~= nil and tostring(v) or nil
	end
	set.input[vd.var] = v
	return config_committed(b)
end

-- set_config_placeholder{var, value}: the grey "what the build implies" number.
function M.set_config_placeholder(p)
	local b = ensure_build()
	local set = config_set(b.configTab)
	local vd = find_var(p and p.var)
	set.placeholder[vd.var] = (p.value ~= nil and p.value ~= "") and tonumber(p.value) or nil
	return config_committed(b)
end

-- reset_config{var}: back to what NewConfigSet would have put there.
function M.reset_config(p)
	local b = ensure_build()
	local set = config_set(b.configTab)
	local vd = find_var(p and p.var)
	local v = vd.defaultState
	if vd.defaultIndex and vd.list then v = vd.list[vd.defaultIndex].val end
	set.input[vd.var] = v
	return config_committed(b)
end

-- set_custom_mods{list=[{title,text,enabled}]}: the whole custom-modifier
-- block list at once (BuildModList parses it, the same as the classic editor).
function M.set_custom_mods(p)
	local b = ensure_build()
	local tab = b.configTab
	local set = config_set(tab)
	local list = {}
	for i, blk in ipairs(p and p.list or {}) do
		list[i] = { title = blk.title or ("Group " .. i), text = blk.text or "", enabled = blk.enabled ~= false }
	end
	if #list == 0 then list[1] = { title = "Default", enabled = true, text = "" } end
	set.customModsList = list
	pcall(tab.UpdateCustomModsControls, tab)
	return config_committed(b)
end

function M.set_config_set(p)
	local b = ensure_build()
	local tab = b.configTab
	local id = tonumber(p and p.id)
	if not id or not tab.configSets[id] then error("no config set " .. tostring(p and p.id), 0) end
	tab:SetActiveConfigSet(id)
	tab:AddUndoState()
	return commit(b)
end

function M.new_config_set(p)
	local b = ensure_build()
	local tab = b.configTab
	local set = tab:NewConfigSet(nil, p and p.title or nil)
	if p and p.copyCurrent then
		local cur = config_set(tab)
		set.input = copyTable(cur.input)
		set.placeholder = copyTable(cur.placeholder)
		set.customModsList = copyTable(cur.customModsList or {})
	end
	table.insert(tab.configSetOrderList, set.id)
	tab:SetActiveConfigSet(set.id)
	tab:AddUndoState()
	local r = commit(b)
	r.id = set.id
	return r
end

function M.rename_config_set(p)
	local b = ensure_build()
	local set = b.configTab.configSets[tonumber(p and p.id or 0)]
	if not set then error("no config set " .. tostring(p and p.id), 0) end
	set.title = p.title
	return commit(b)
end

function M.delete_config_set(p)
	local b = ensure_build()
	local tab = b.configTab
	local id = tonumber(p and p.id)
	if not id or not tab.configSets[id] then error("no config set " .. tostring(p and p.id), 0) end
	if #tab.configSetOrderList <= 1 then error("cannot delete the last config set", 0) end
	for i, v in ipairs(tab.configSetOrderList) do
		if v == id then table.remove(tab.configSetOrderList, i) break end
	end
	if tab.activeConfigSetId == id then tab:SetActiveConfigSet(tab.configSetOrderList[1]) end
	tab.configSets[id] = nil
	tab:AddUndoState()
	return commit(b)
end

-- ---------------------------------------------------------------------------
-- Calcs
-- ---------------------------------------------------------------------------

-- The breakdown control's sections as data (shared by the sidebar and the
-- calcs page): SetBreakdownData fills ctl.sectionList, we read it, then clear.
local function breakdown_sections(ctl, fill)
	local sections = {}
	local ok, err = pcall(function()
		fill()
		for _, s in ipairs(ctl.sectionList or {}) do
			if s.type == "TEXT" then
				local lines = {}
				for i, l in ipairs(s.lines) do lines[i] = tr(l) end
				sections[#sections + 1] = { type = "text", size = s.textSize or 16, lines = lines }
			elseif s.type == "TABLE" then
				local cols, rows = {}, {}
				for i, c in ipairs(s.colList or {}) do
					cols[i] = { label = tr(c.label or ""), key = tostring(c.key), right = c.right and true or false }
				end
				for i, r in ipairs(s.rowList or {}) do
					local row = {}
					for _, c in ipairs(s.colList or {}) do
						row[tostring(c.key)] = tr(cell_text(r[c.key]))
					end
					rows[i] = setmetatable(row, { __object = true })
				end
				sections[#sections + 1] = { type = "table", label = s.label and tr(s.label) or nil, footer = s.footer and tr(s.footer) or nil, cols = cols, rows = rows }
			elseif s.type == "RADIUS" then
				sections[#sections + 1] = { type = "radius", radius = s.radius }
			end
		end
	end)
	ctl:SetBreakdownData()
	if not ok then error(err, 0) end
	return sections
end

local function calcs_actor(tab)
	if not tab.calcsEnv then error("calcs output not built yet", 0) end
	return tab.input.showMinion and tab.calcsEnv.minion or tab.calcsEnv.player
end

-- get_calcs: every section/subsection/row/cell of POB's Calcs tab, formatted
-- by POB's own formatCalcStr against the CALCS environment, with the flags
-- POB uses to hide rows (CheckFlag). Cells are addressable for breakdowns.
function M.get_calcs()
	local b = ensure_build()
	local tab = b.calcsTab
	local actor = calcs_actor(tab)
	local out = {}
	for si, sec in ipairs(tab.sectionList) do
		local enabled = tab:CheckFlag(sec) and true or false
		local anyRow = false
		local subs = {}
		for ui, sub in ipairs(sec.subSection or {}) do
			local rows = {}
			for ri, row in ipairs(sub.data or {}) do
				if enabled and tab:CheckFlag(row) then
					local cells = {}
					for ci, col in ipairs(row) do
						if col.format and tab:CheckFlag(col) then
							local ok, text = pcall(formatCalcStr, col.format, actor, col)
							text = ok and tostring(text) or "?"
							cells[#cells + 1] = as_object({
								ci = ci, text = tr(text), raw = text,
								hasBreakdown = #col > 0,
								control = col.controlName,
							})
						elseif col.controlName then
							cells[#cells + 1] = as_object({ ci = ci, control = col.controlName })
						end
					end
					rows[#rows + 1] = as_object({ ri = ri, label = row.label, labelZh = row.label and tr(row.label) or nil, color = row.color, textSize = row.textSize, cells = cells })
					anyRow = true
				end
			end
			local extra
			if sub.data and sub.data.extra then
				local ok, t = pcall(formatCalcStr, sub.data.extra, actor)
				extra = ok and tr(tostring(t)) or nil
			end
			subs[ui] = as_object({ ui = ui, label = sub.label, labelZh = tr(sub.label), collapsed = sub.collapsed and true or false, extra = extra, rows = rows })
		end
		-- CalcSectionControl:UpdateSize: a section whose rows are all filtered out is not drawn
		enabled = enabled and anyRow
		out[si] = as_object({ si = si, id = sec.id, group = sec.group, colour = sec.colour, widthCols = sec.widthCols, enabled = enabled, subsections = subs })
	end
	-- the skill/mode selectors POB draws in its first section
	local sel = tab.sectionList[1]
	local selectors = {}
	if sel and sel.controls and sel.controls.mainSocketGroup then
		b:RefreshSkillSelectControls(sel.controls, tab.input.skill_number, "Calcs")
		selectors.mainSocketGroup = { index = sel.controls.mainSocketGroup.selIndex or 1, list = dd_entries(sel.controls.mainSocketGroup) }
		if dd_shown(sel.controls.mainSkill) then selectors.mainSkill = { index = sel.controls.mainSkill.selIndex or 1, list = dd_entries(sel.controls.mainSkill) } end
		if dd_shown(sel.controls.mainSkillPart) then selectors.mainSkillPart = { index = sel.controls.mainSkillPart.selIndex or 1, list = dd_entries(sel.controls.mainSkillPart) } end
	end
	return {
		sections = out,
		input = { skill_number = tab.input.skill_number, misc_buffMode = tab.input.misc_buffMode, showMinion = tab.input.showMinion and true or false },
		selectors = selectors,
		hasMinion = tab.calcsEnv and tab.calcsEnv.minion ~= nil,
		rev = b.outputRevision,
	}
end

-- set_calcs_input{var, value}: skill_number / misc_buffMode / showMinion,
-- plus the "Calcs" twins of the main-skill selectors.
function M.set_calcs_input(p)
	local b = ensure_build()
	local tab = b.calcsTab
	local var, v = p and p.var, p and p.value
	if var == "skill_number" then
		tab.input.skill_number = tonumber(v) or 1
	elseif var == "misc_buffMode" then
		if v ~= "EFFECTIVE" and v ~= "COMBAT" and v ~= "BUFFED" and v ~= "UNBUFFED" then error("bad buff mode", 0) end
		tab.input.misc_buffMode = v
	elseif var == "showMinion" then
		tab.input.showMinion = v and true or false
	elseif var == "mainActiveSkill" then
		local g = b.skillsTab.socketGroupList[tab.input.skill_number]
		if not g then error("no socket group", 0) end
		g.mainActiveSkillCalcs = tonumber(v) or 1
	elseif var == "skillPart" then
		local g = b.skillsTab.socketGroupList[tab.input.skill_number]
		local as = g and g.displaySkillListCalcs and g.displaySkillListCalcs[g.mainActiveSkillCalcs or 1]
		local src = as and as.activeEffect and as.activeEffect.srcInstance
		if not src then error("no active skill", 0) end
		src.skillPartCalcs = tonumber(v) or 1
	else
		error("unknown calcs input " .. tostring(var), 0)
	end
	tab:AddUndoState()
	return commit(b)
end

-- calcs_breakdown{si, ui, ri, ci}: the breakdown POB shows when that cell is
-- hovered (CalcBreakdownControl:SetBreakdownData(colData)).
function M.calcs_breakdown(p)
	local b = ensure_build()
	local tab = b.calcsTab
	local sec = tab.sectionList[tonumber(p and p.si or 0)]
	local sub = sec and sec.subSection[tonumber(p.ui or 0)]
	local row = sub and sub.data[tonumber(p.ri or 0)]
	local col = row and row[tonumber(p.ci or 0)]
	if not col then error("no such cell", 0) end
	local ctl = tab.controls.breakdown
	local sections = breakdown_sections(ctl, function() ctl:SetBreakdownData(col, false) end)
	return { sections = sections, rev = b.outputRevision }
end

probe("classes.ConfigTab.BuildModList/UpdateLevel/SetActiveConfigSet/NewConfigSet", function()
	local c = class_of("ConfigTab")
	return type(c) == "table" and type(c.BuildModList) == "function" and type(c.UpdateLevel) == "function"
		and type(c.SetActiveConfigSet) == "function" and type(c.NewConfigSet) == "function"
end)
probe("Modules.ConfigOptions is a list with sections and vars", function()
	local ok, list = pcall(require, "Modules.ConfigOptions")
	if not ok or type(list) ~= "table" then return false end
	local sections, vars = 0, 0
	for _, vd in ipairs(list) do
		if vd.section then sections = sections + 1 elseif vd.var then vars = vars + 1 end
	end
	return sections >= 3 and vars >= 20
end)
probe("classes.CalcsTab.CheckFlag + sectionList + formatCalcStr", function()
	local c = class_of("CalcsTab")
	return type(c) == "table" and type(c.CheckFlag) == "function" and type(formatCalcStr) == "function"
end)

-- ---------------------------------------------------------------------------
-- Notes, Party
-- ---------------------------------------------------------------------------

-- NotesTab keeps the text only in its edit control and works out modFlag in
-- Draw (never called headless), so both are handled here explicitly.
function M.get_notes()
	local b = ensure_build()
	local tab = b.notesTab
	tab:SetShowColorCodes(false)
	-- the colour buttons above POB's editor, in its order: each label is the
	-- colour code followed by the name (NotesTab.lua "colorCodes.X.."X"")
	local colours = {}
	for _, n in ipairs({ "normal", "magic", "rare", "unique", "fire", "cold", "lightning", "chaos", "strength", "dexterity", "intelligence", "default" }) do
		local c = tab.controls[n]
		local label = c and c.label
		if type(label) == "function" then label = label() end
		if type(label) == "string" then
			local code, name = label:match("^(%^x%x%x%x%x%x%x)(.*)$")
			if not code then code, name = label:match("^(%^%d)(.*)$") end
			if code then colours[#colours + 1] = { code = code, name = name } end
		end
	end
	return { text = tab.controls.edit.buf or "", unsaved = tab.modFlag and true or false, rev = b.outputRevision, colours = colours }
end

function M.set_notes(p)
	local b = ensure_build()
	local tab = b.notesTab
	local text = p and p.text
	if type(text) ~= "string" then error("params.text required", 0) end
	if #text > 1024 * 1024 then error("notes too large", 0) end
	tab:SetShowColorCodes(false)
	tab.controls.edit:SetText(text)
	tab.modFlag = (tab.lastContent ~= tab.controls.edit.buf)
	b.buildFlag = true
	frame()
	return { unsaved = b.unsaved and true or false, rev = b.outputRevision }
end

-- PartyTab: seven text areas, each parsed by ParseBuffs into its list the way
-- Load does; the export toggle lives on the tab as enableExportBuffs.
local party_fields = {
	{ key = "partyMemberStats", control = "editPartyMemberStats", buffType = "PartyMemberStats", last = "PartyMemberStats" },
	{ key = "aura",             control = "editAuras",            buffType = "Aura",             last = "Aura",       simple = "simpleAuras" },
	{ key = "curse",            control = "editCurses",           buffType = "Curse",            last = "Curse",      simple = "simpleCurses" },
	{ key = "warcry",           control = "editWarcries",         buffType = "Warcry",           last = "Warcry",     simple = "simpleWarcries" },
	{ key = "link",             control = "editLinks",            buffType = "Link",             last = "Link",       simple = "simpleLinks" },
	{ key = "enemyCond",        control = "enemyCond",            buffType = "EnemyConditions",  last = "EnemyCond" },
	{ key = "enemyMods",        control = "enemyMods",            buffType = "EnemyMods",        last = "EnemyMods",  simple = "simpleEnemyMods" },
}

local function party_parse(tab, f, text)
	if f.buffType == "PartyMemberStats" then
		tab:ParseBuffs(tab.actor.modDB, text, "PartyMemberStats", tab.actor.output)
	elseif f.buffType == "EnemyConditions" then
		tab:ParseBuffs(tab.enemyModList, text, "EnemyConditions")
	elseif f.buffType == "EnemyMods" then
		tab:ParseBuffs(tab.enemyModList, text, "EnemyMods", tab.controls[f.simple])
	else
		tab:ParseBuffs(tab.actor[f.buffType], text, f.buffType, tab.controls[f.simple])
	end
end

function M.get_party()
	local b = ensure_build()
	local tab = b.partyTab
	local fields = {}
	for _, f in ipairs(party_fields) do
		local ctl = tab.controls[f.control]
		fields[f.key] = ctl and ctl.buf or ""
	end
	local exports = {}
	if tab.enableExportBuffs then
		for _, bt in ipairs({ "Aura", "Curse", "Warcry", "Link", "EnemyMods" }) do
			local ok, txt = pcall(tab.exportBuffs, tab, bt)
			exports[bt] = ok and txt or ""
		end
	end
	return {
		fields = as_object(fields),
		enableExportBuffs = tab.enableExportBuffs and true or false,
		exports = as_object(exports),
		unsaved = tab.modFlag and true or false,
		rev = b.outputRevision,
	}
end

-- set_party{field, text} for the seven areas, or {field="enableExportBuffs", value=bool}.
function M.set_party(p)
	local b = ensure_build()
	local tab = b.partyTab
	local key = p and p.field
	if key == "enableExportBuffs" then
		tab.enableExportBuffs = p.value and true or false
		tab.modFlag = true
		b.buildFlag = true
		frame()
		return { unsaved = b.unsaved and true or false, rev = b.outputRevision }
	end
	local f
	for _, e in ipairs(party_fields) do if e.key == key then f = e end end
	if not f then error("unknown party field " .. tostring(key), 0) end
	local text = p.text
	if type(text) ~= "string" then error("params.text required", 0) end
	if #text > 1024 * 1024 then error("text too large", 0) end
	local ctl = tab.controls[f.control]
	if not ctl then error("party control missing: " .. f.control, 0) end
	ctl:SetText(text)
	-- Re-parse everything the way Load does: the lists are rebuilt from scratch
	-- per field, and the enemy list is shared by two fields.
	if f.buffType == "EnemyConditions" or f.buffType == "EnemyMods" then
		tab.enemyModList = make("ModList")
		party_parse(tab, party_fields[6], tab.controls.enemyCond.buf or "")
		party_parse(tab, party_fields[7], tab.controls.enemyMods.buf or "")
	elseif f.buffType == "PartyMemberStats" then
		tab.actor.modDB = make("ModDB")
		tab.actor.modDB.actor = tab.actor
		tab.actor.output = {}
		party_parse(tab, f, text)
	else
		tab.actor[f.buffType] = {}
		party_parse(tab, f, text)
	end
	tab.modFlag = (tab.lastContent[f.last] ~= text) or tab.modFlag
	b.buildFlag = true
	frame()
	return { unsaved = b.unsaved and true or false, rev = b.outputRevision }
end

probe("classes.NotesTab.SetShowColorCodes + edit control", function()
	local c = class_of("NotesTab")
	return type(c) == "table" and type(c.SetShowColorCodes) == "function"
end)
probe("classes.PartyTab.ParseBuffs/exportBuffs", function()
	local c = class_of("PartyTab")
	return type(c) == "table" and type(c.ParseBuffs) == "function" and type(c.exportBuffs) == "function"
end)

-- ---------------------------------------------------------------------------
-- Wire-up
-- ---------------------------------------------------------------------------

setDispatcher(function(method, params)
	local fn = M[method]
	if type(fn) ~= "function" then error("unknown method: " .. tostring(method), 0) end
	return fn(params)
end)

run_probes()
if not gate.ok then
	log_error("gate failed: " .. table.concat(gate.failed, "; "))
end
-- Install the sidebar wrapper now, not on the first load_build_file: POB may
-- already have reopened the last build during OnInit, and its rows were laid
-- down before we existed -- refresh them once so they carry stat/actor too.
do
	local ok, err = pcall(function()
		wrap_add_display_stat_list()
		local b = build()
		if b and b.calcsTab and b.calcsTab.mainOutput and b.RefreshStatList then
			b:RefreshStatList()
		end
	end)
	if not ok then log_error("sidebar wrapper: " .. tostring(err)) end
end
-- The host remembers the verdict per POB version (PobTools\bridge_gate.json),
-- so the version rides along with it.
gate.pobVersion = type(launch) == "table" and launch.versionNumber or nil
gate.pobBranch = type(launch) == "table" and launch.versionBranch or nil
emit("gate_result", gate)
emit("hello", {
	protocol = 1,
	bridge = BRIDGE_VERSION,
	pobVersion = type(launch) == "table" and launch.versionNumber or nil,
	pobBranch = type(launch) == "table" and launch.versionBranch or nil,
})
