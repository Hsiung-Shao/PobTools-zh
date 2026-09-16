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
-- so splitting the list changes nothing it produces.
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
	local h = {
		buildName = b.buildName,
		dbFileName = b.dbFileName or nil,
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
	if type(root) ~= "table" or root.elem ~= "PathOfBuilding" then error("code is not a build file", 0) end
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

-- list_items: every item POB holds, the slot grid, item sets.
function M.list_items()
	local b = ensure_build()
	local tab = b.itemsTab
	-- Classic POB refreshes socket labels/activity from its Draw; do it here.
	if tab.UpdateSockets then tab:UpdateSockets() end
	local items = {}
	for i, id in ipairs(tab.itemOrderList) do
		local it = tab.items[id]
		if it then items[#items + 1] = item_summary(it) end
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

-- item_tooltip{id | raw, slotName?, dbMode?}: POB's own item tooltip. With a
-- slot the stat-difference block ("equipping this changes DPS by...") is
-- included, exactly as hovering the item over that slot in the classic UI.
function M.item_tooltip(p)
	local b = ensure_build()
	local tab = b.itemsTab
	local it, dbMode
	if p and p.raw then
		it = new("Item"):Item(p.raw, p.rarity, true)
		if it.base then it:BuildModList() end
		dbMode = true
	else
		it = tab.items[tonumber(p and p.id or 0)]
		if not it then error("no item " .. tostring(p and p.id), 0) end
		dbMode = p.dbMode and true or false
	end
	local slot = p and p.slotName and tab.slots[p.slotName] or nil
	local tt = new("Tooltip"):Tooltip()
	local savedDiff = tab.showStatDifferences
	tab.showStatDifferences = slot ~= nil
	without_wrap(function() tab:AddItemTooltip(tt, it, slot, dbMode) end)
	tab.showStatDifferences = savedDiff
	return {
		id = it.id,
		header = tt.tooltipHeader,
		color = tt.color,
		lines = tooltip_lines(tt),
		summary = item_summary(it),
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
	local it = new("Item"):Item(raw)
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
local db_cache = {}
local function db_sorted(kind)
	local m = main()
	local db = kind == "rare" and m.rareDB or m.uniqueDB
	if not db then error("item database missing", 0) end
	-- POB parses the databases in a coroutine it resumes once per frame (Main.lua
	-- onFrameFuncs.LoadItems, ~20 ms of work each); pump frames until it is done.
	local pumps = 0
	while db.loading and pumps < 2000 do
		frame()
		pumps = pumps + 1
	end
	if db.loading then error("item database still loading", 0) end
	if db_cache[kind] and db_cache[kind].db == db then return db_cache[kind].list, db_cache[kind].types end
	local list, typeSet = {}, {}
	for name, it in pairs(db.list) do
		local nameZh = (it.title and it.baseName) and (tr(it.title) .. ", " .. tr(it.baseName)) or tr(it.name)
		list[#list + 1] = { it = it, key = (it.name or ""):lower(), keyZh = (nameZh or ""):lower(), nameZh = nameZh }
		if it.type then typeSet[it.type] = true end
	end
	table.sort(list, function(a, b) return a.key < b.key end)
	local types = {}
	for t in pairs(typeSet) do types[#types + 1] = t end
	table.sort(types)
	db_cache[kind] = { db = db, list = list, types = types }
	return list, types
end

function M.item_db(p)
	ensure_build()
	local kind = p and p.kind == "rare" and "rare" or "unique"
	local list, types = db_sorted(kind)
	local q = p and type(p.query) == "string" and p.query:lower() or ""
	local wantType = p and p.type or nil
	local page = math.max(1, tonumber(p and p.page) or 1)
	local size = math.min(200, math.max(1, tonumber(p and p.size) or 50))
	local hits = {}
	for _, e in ipairs(list) do
		local it = e.it
		if (wantType == nil or wantType == "" or it.type == wantType)
			and (q == "" or e.key:find(q, 1, true) or e.keyZh:find(q, 1, true)) then
			hits[#hits + 1] = e
		end
	end
	local out = {}
	local first = (page - 1) * size
	for i = first + 1, math.min(#hits, first + size) do
		local e = hits[i]
		local s = item_summary(e.it)
		s.raw = e.it.raw
		s.id = nil
		out[#out + 1] = s
	end
	local typeList = {}
	for i, t in ipairs(types) do typeList[i] = { type = t, typeZh = tr(t) } end
	return { kind = kind, total = #hits, page = page, size = size, items = out, types = typeList }
end

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
