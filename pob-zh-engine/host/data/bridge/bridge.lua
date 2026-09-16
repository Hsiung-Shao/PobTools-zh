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
				proxy = node.isProxy and true or false,
				linked = linked,
				masteryEffects = effects,
				flavour = node.flavourText,
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
			connectors[#connectors + 1] = { a = a, b = b, orbit = orbit, asc = c.ascendancyName }
		end
	end

	local classes = {}
	for cid, class in pairs(tree.classes) do
		if type(cid) == "number" and type(class) == "table" then
			local ascs = {}
			for _, a in ipairs(class.ascendancies or {}) do
				ascs[#ascs + 1] = { id = a.id, name = a.name, nameZh = tr(a.name) }
			end
			classes[#classes + 1] = {
				id = cid, name = class.name, nameZh = tr(class.name),
				startNodeId = class.startNodeId, ascendancies = ascs,
				art = CLASS_ART[cid],
			}
		end
	end
	table.sort(classes, function(x, y) return x.id < y.id end)
	local alternate = {}
	for _, a in ipairs(tree.alternate_ascendancies or {}) do
		alternate[#alternate + 1] = { id = a.id, name = a.name, nameZh = tr(a.name) }
	end

	local out = {
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
	return { version = v, assets = as_object(assets), disabled = as_object(disabled),
	         sheets = as_object(sheets), missingSheets = missing }
end

local function node_type_name(node)
	return node and node.type or nil
end

-- What the build did to the tree: allocations, per-node overrides (mastery
-- choice, tattoos, timeless jewels), cluster subgraphs, points.
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
			why = "tattoo"
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
	-- Cluster jewel subgraphs: POB positions these itself (ProcessNode).
	local dynamicNodes, dynamicGroups = {}, {}
	for _, sg in pairs(spec.subGraphs or {}) do
		if sg.group then
			local orbits = {}
			for o in pairs(sg.group.oo or {}) do orbits[#orbits + 1] = o end
			table.sort(orbits)
			dynamicGroups[#dynamicGroups + 1] = { x = sg.group.x, y = sg.group.y, orbits = orbits }
		end
		for id, node in pairs(sg.nodes or {}) do
			if type(node.x) == "number" then
				local links = {}
				for _, other in ipairs(node.linked or {}) do links[#links + 1] = other.id end
				local stats = {}
				for i, s in ipairs(node.sd or {}) do stats[i] = s end
				dynamicNodes[#dynamicNodes + 1] = {
					id = id, name = node.dn, nameZh = tr(node.dn), type = node.type,
					stats = stats, x = node.x, y = node.y, icon = node.icon, links = links,
					expansion = node.expansionJewel ~= nil, allocated = node.alloc and true or false,
				}
			end
		end
	end
	local sockets = {}
	if b.itemsTab and b.itemsTab.GetSocketAndJewelForNodeID then
		for id, node in pairs(spec.nodes) do
			if node.type == "Socket" then
				local ok, socket, jewel = pcall(b.itemsTab.GetSocketAndJewelForNodeID, b.itemsTab, id)
				if ok and jewel then
					sockets[#sockets + 1] = {
						nodeId = id, itemId = socket and socket.selItemId, name = jewel.name,
						title = jewel.title, baseName = jewel.baseName,
					}
				end
			end
		end
	end
	local info = M.get_build_info()
	return {
		treeVersion = spec.treeVersion,
		classId = spec.curClassId, className = spec.curClassName,
		ascendClassId = spec.curAscendClassId, ascendClassName = spec.curAscendClassName,
		allocatedNodes = alloc,
		allocCount = #alloc,
		overrides = as_object(overrides),
		dynamicNodes = dynamicNodes,
		dynamicGroups = dynamicGroups,
		sockets = sockets,
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
	local tt = new("Tooltip"):Tooltip()
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
				for ascId, asc in pairs(spec.curClass.classes) do
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
		spec:AllocNode(node)
	end
	return committed(b, spec)
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
probe("classes.PassiveTreeView.AddNodeTooltip(4 params)", function()
	local c = class_of("PassiveTreeView")
	return type(c) == "table" and type(c.AddNodeTooltip) == "function" and nparams(c.AddNodeTooltip) == 5
end)
probe("classes.UndoHandler.AddUndoState/Undo/Redo", function()
	local c = class_of("UndoHandler")
	return type(c) == "table" and type(c.AddUndoState) == "function" and type(c.Undo) == "function" and type(c.Redo) == "function"
end)
probe("classes.Tooltip.AddLine", function() local c = class_of("Tooltip"); return type(c) == "table" and type(c.AddLine) == "function" end)
probe("latestTreeVersion", function() return type(latestTreeVersion) == "string" end)

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
