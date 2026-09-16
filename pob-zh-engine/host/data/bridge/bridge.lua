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

local BRIDGE_VERSION = "0.1.0"

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

local function log_error(msg)
	if PobToolsLogError then PobToolsLogError("bridge", msg) end
end

-- ---------------------------------------------------------------------------
-- Probes: every POB surface the bridge depends on, each checked by name.
-- ---------------------------------------------------------------------------

local probes = {}
local function probe(name, fn) probes[#probes + 1] = { name = name, fn = fn } end

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
probe("buildMode.AddDisplayStatList(3 params)", function() return nparams(launch.main.modes.BUILD.AddDisplayStatList) == 4 end)
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
probe("classes.CalcsTab.BuildOutput", function() local c = class_of("CalcsTab"); return type(c) == "table" and type(c.BuildOutput) == "function" end)
probe("buildMode.GetSidebarBreakdown", function() return type(launch.main.modes.BUILD.GetSidebarBreakdown) == "function" end)
probe("classes.CalcBreakdownControl.SetBreakdownData", function() local c = class_of("CalcBreakdownControl"); return type(c) == "table" and type(c.SetBreakdownData) == "function" end)
probe("Modules.BuildListHelpers.ScanFolder/FilterList/SortList", function()
	local ok, h = pcall(require, "Modules.BuildListHelpers")
	return ok and type(h) == "table" and type(h.ScanFolder) == "function" and type(h.FilterList) == "function" and type(h.SortList) == "function"
end)
probe("classes.PassiveSpec.CountAllocNodes", function() local c = class_of("PassiveSpec"); return type(c) == "table" and type(c.CountAllocNodes) == "function" end)
probe("classes.PassiveSpec.AllocNode", function() local c = class_of("PassiveSpec"); return type(c) == "table" and type(c.AllocNode) == "function" and type(c.DeallocNode) == "function" end)
probe("UpdateCheck.lua present", function() local f = io.open("UpdateCheck.lua", "r"); if f then f:close() return true end return false end)

local gate = { ok = false, failed = {}, checked = 0 }

local function run_probes()
	gate = { ok = true, failed = {}, checked = 0 }
	if type(launch) == "table" and launch.promptMsg then
		-- POB's own boot error (the popup nobody can see headless).
		gate.ok = false
		gate.failed[#gate.failed + 1] = "POB reported: " .. tostring(launch.promptMsg)
	end
	for _, p in ipairs(probes) do
		gate.checked = gate.checked + 1
		local ok, res = pcall(p.fn)
		if not ok or not res then
			gate.ok = false
			gate.failed[#gate.failed + 1] = p.name .. (ok and "" or (": " .. tostring(res)))
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
-- so splitting the list changes nothing it produces. (Technique from
-- pob-redux's bridge, MIT.)
local wrapped = false
local function wrap_add_display_stat_list()
	local B = build()
	if wrapped or not B or type(B.AddDisplayStatList) ~= "function" then return end
	local orig = B.AddDisplayStatList
	B.AddDisplayStatList = function(self, statList, actor, actorName)
		local list = self.controls and self.controls.statBox and self.controls.statBox.list
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
			-- Either key is enough for GetSidebarBreakdown to say something.
			hasBreakdown = (row.breakdown ~= nil or row.modNames ~= nil) and true or false,
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
	if not line.breakdown and not line.modNames then
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
function M.list_builds(p)
	local subPath = p and p.subPath or ""
	local helpers = require("Modules.BuildListHelpers")
	local index = helpers.ScanFolder(subPath)
	local list = helpers.FilterList(index, subPath, "")
	helpers.SortList(list, main().buildSortMode or "NAME")
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
	return { buildPath = main().buildPath, subPath = subPath, entries = entries }
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
	launch:ApplyUpdate(mode)
	return { applied = mode }
end

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
emit("gate_result", gate)
emit("hello", {
	protocol = 1,
	bridge = BRIDGE_VERSION,
	pobVersion = type(launch) == "table" and launch.versionNumber or nil,
	pobBranch = type(launch) == "table" and launch.versionBranch or nil,
})
