-- PobTools MCP tools: the Lua half of `pob-zh.exe --mcp` (host/mcp_server.cpp).
--
-- Loaded by bridge.lua, which hands over its method table M and a few of its
-- helpers. Each tool is a small, AI-sized view over bridge methods that
-- already exist: every number is POB's own (bridge rule 1), the shapes are cut
-- down so a model's context is not flooded with the UI's full payloads.
-- Tool names and argument schemas live in mcp_tools.json (the host lists them
-- before the engine is up); keep the two in step.
--
-- Names: `name` is POB's English, `nameZh` the same words the classic window
-- draws in Traditional Chinese (使用者核准 2026-11-05: 英文＋繁中並列).

return function(M, ctx)
	local tr, gem_tr, ensure_build, main = ctx.tr, ctx.gem_tr, ctx.ensure_build, ctx.main

	local T = {}

	-- ---- helpers ----------------------------------------------------------

	local function argerr(fmt, ...) error(string.format(fmt, ...), 0) end

	local function lower(s) return type(s) == "string" and s:lower() or "" end

	-- POB text with translation switched off for the span of fn: breakdowns
	-- and labels go to the model in English (it reasons better on them).
	local function english(fn)
		if not (PobToolsSetTranslate and PobToolsGetTranslate) then return fn() end
		local was = PobToolsGetTranslate()
		PobToolsSetTranslate(false)
		local ok, r = pcall(fn)
		PobToolsSetTranslate(was)
		if not ok then error(r, 0) end
		return r
	end

	-- An empty Lua table goes out as [] (engine/headless_bridge.cpp IsArray);
	-- these are JSON objects by contract.
	local function obj(t) return setmetatable(t, { __object = true }) end

	local function has_build()
		return pcall(ensure_build)
	end

	local KEY_STATS = {
		"Life", "EnergyShield", "Mana", "Ward", "TotalEHP", "PhysicalMaximumHitTaken",
		"FireMaximumHitTaken", "ColdMaximumHitTaken", "LightningMaximumHitTaken", "ChaosMaximumHitTaken",
		"CombinedDPS", "TotalDPS", "FullDPS", "TotalDot", "AverageHit", "Speed", "CritChance", "CritMultiplier", "HitChance",
		"FireResist", "ColdResist", "LightningResist", "ChaosResist",
		"Armour", "Evasion", "BlockChance", "SpellBlockChance", "SpellSuppressionChance",
		"PhysicalDamageReduction", "MeleeEvadeChance", "ProjectileEvadeChance",
		"LifeRegenRecovery", "EnergyShieldRegenRecovery", "ManaRegenRecovery",
		"LifeUnreserved", "ManaUnreserved", "ManaCost", "LifeCost",
		"Str", "Dex", "Int", "MovementSpeedMod",
	}

	local function key_stats(b)
		local out = b.calcsTab.mainOutput
		local s = {}
		for _, k in ipairs(KEY_STATS) do
			local v = out[k]
			if type(v) == "number" and v == v and v ~= math.huge and v ~= -math.huge then s[k] = v end
		end
		return obj(s)
	end

	local function main_skill(b)
		local g = b.skillsTab.socketGroupList[b.mainSocketGroup or 0]
		if not g then return nil end
		local as = g.displaySkillList and g.displaySkillList[g.mainActiveSkill or 1]
		local ge = as and as.activeEffect and as.activeEffect.grantedEffect
		if not ge then return { group = b.mainSocketGroup, name = g.displayLabel } end
		return { group = b.mainSocketGroup, name = ge.name, nameZh = gem_tr(ge.id, ge.name) }
	end

	local function node_entry(n)
		return { id = n.id, name = n.dn, nameZh = tr(n.dn), type = n.type }
	end

	-- Damage types POB itself rates as unhittable (its <Type>MaximumHitTaken is
	-- inf: Chaos Inoculation, Divine Flesh-style immunities...). key_stats drops
	-- inf, so without this the model never learns the build is immune.
	local DAMAGE_TYPES = { "Physical", "Fire", "Cold", "Lightning", "Chaos" }
	local function immune_to(b)
		local out, list = b.calcsTab.mainOutput, {}
		for _, t in ipairs(DAMAGE_TYPES) do
			if out[t .. "MaximumHitTaken"] == math.huge then list[#list + 1] = t end
		end
		return list
	end

	-- Allocated keystones with POB's own stat lines: the build's intent (Life 1
	-- under Chaos Inoculation is the plan, not a flaw).
	local function keystones(b)
		local list = {}
		for _, n in pairs(b.spec.allocNodes) do
			if n.type == "Keystone" then
				local e = node_entry(n)
				e.stats = n.sd or {}
				list[#list + 1] = e
			end
		end
		table.sort(list, function(x, y) return (x.name or "") < (y.name or "") end)
		return list
	end

	local function summary()
		local b = ensure_build()
		local info = M.get_build_info()
		return {
			keystones = keystones(b),
			immuneTo = immune_to(b),
			buildName = info.buildName,
			class = info.className, classZh = info.classNameZh,
			ascendancy = info.ascendClassName, ascendancyZh = info.ascendClassNameZh,
			level = info.level,
			points = { used = info.points.used, max = info.points.usedMax, ascUsed = info.points.ascUsed, ascMax = info.points.ascMax },
			mainSkill = main_skill(b),
			stats = key_stats(b),
		}
	end

	local function find_slot_of(tab, itemId)
		for _, slot in ipairs(tab.orderedSlots) do
			if slot.selItemId == itemId then return slot.slotName end
		end
		return nil
	end

	-- ---- read in / out ---------------------------------------------------

	function T.load_build(a)
		local src = a and a.source
		if type(src) ~= "string" or not src:match("%S") then argerr("source required: a share code, a build URL (pobb.in, poe.ninja...) or a file name from list_my_builds") end
		src = src:gsub("^%s+", ""):gsub("%s+$", "")
		if not has_build() then M.new_build({ name = "MCP" }) end
		if src:match("^https?://") then
			local r = M.import_from_url({ url = src })
			if type(r) ~= "table" or type(r.code) ~= "string" then argerr("could not download a build from %s", src) end
			M.import_code({ code = r.code, mode = "new" })
		elseif src:lower():match("%.xml$") then
			local base = main().buildPath or ""
			local rel = src:gsub("/", "\\")
			if rel:find("%.%.") then argerr("source must be a file inside the Builds folder") end
			local path = base .. rel
			local f = io.open(path, "r")
			if not f then argerr("no build file %s (use list_my_builds for the names)", src) end
			f:close()
			M.load_build_file({ path = path })
		else
			local ok, err = pcall(M.import_code, { code = src, mode = "new" })
			if not ok then argerr("source is not a build file name, a URL or a valid share code: %s", tostring(err)) end
		end
		return { ok = true, summary = summary() }
	end

	function T.list_my_builds()
		local out = {}
		local function walk(subPath, depth)
			local r = M.list_builds({ subPath = subPath, filter = "" })
			for _, e in ipairs(r.entries or {}) do
				if e.isFolder then
					if depth < 4 then walk((e.subPath or subPath) .. e.folderName .. "/", depth + 1) end
				elseif e.fileName then
					out[#out + 1] = {
						file = (e.subPath or subPath):gsub("/", "\\") .. e.fileName,
						name = e.buildName, class = e.className, ascendancy = e.ascendClassName, level = e.level,
					}
				end
			end
		end
		walk("", 0)
		return { builds = out }
	end

	function T.new_build(a)
		local want = lower(a and a.class)
		if want == "" then argerr("class required (e.g. \"Witch\")") end
		-- A wrong class / ascendancy name must not cost the user the loaded build.
		local snap = has_build() and M.export_code().code or nil
		-- The share code carries no file or build name; keep them to put back.
		local old = snap and ensure_build()
		local oldFile, oldName = old and old.dbFileName, old and old.buildName
		local ok, err = pcall(function()
			M.new_build({ name = "MCP build" })
			local cl = M.list_classes()
			local class
			for _, c in ipairs(cl.classes) do
				if lower(c.name) == want or lower(c.nameZh) == want then class = c end
			end
			if not class then
				local names = {}
				for _, c in ipairs(cl.classes) do names[#names + 1] = c.name end
				argerr("unknown class %q; one of: %s", a.class, table.concat(names, ", "))
			end
			M.set_class({ classId = class.id, confirm = "reset" })
			local asc = lower(a.ascendancy)
			if asc ~= "" then
				local found
				for _, x in ipairs(class.ascendancies) do
					if x.id > 0 and (lower(x.name) == asc or lower(x.nameZh) == asc) then found = x end
				end
				if not found then
					local names = {}
					for _, x in ipairs(class.ascendancies) do if x.id > 0 then names[#names + 1] = x.name end end
					argerr("unknown ascendancy %q for %s; one of: %s", a.ascendancy, class.name, table.concat(names, ", "))
				end
				M.set_ascendancy({ ascendClassId = found.id })
			end
		end)
		if not ok then
			if snap then
				local okRestore, restoreErr = pcall(M.import_code, { code = snap, mode = "replace" })
				if not okRestore then
					error(tostring(err) .. "; RESTORING THE BUILD FAILED (" .. tostring(restoreErr) .. "): load it again", 0)
				end
				local b = ensure_build()
				b.dbFileName, b.buildName = oldFile, oldName
				error(tostring(err) .. " -- the previous build was kept", 0)
			end
			error(err, 0)
		end
		return { ok = true, summary = summary() }
	end

	function T.export_code()
		return { code = M.export_code().code }
	end

	-- ---- overview --------------------------------------------------------

	function T.build_summary()
		return summary()
	end

	function T.stat_breakdown(a)
		local b = ensure_build()
		local stat = a and a.stat
		if type(stat) ~= "string" or stat == "" then argerr("stat required (a key from build_summary.stats, e.g. \"Life\")") end
		if type(b.GetSidebarBreakdown) ~= "function" then
			-- A POB without Build:GetSidebarBreakdown: give the number and say
			-- why there are no lines.
			return { stat = stat, value = b.calcsTab.mainOutput[stat], lines = {},
				note = "This Path of Building version has no sidebar breakdown." }
		end
		local side = M.get_sidebar()
		local idx
		for i, row in ipairs(side.rows) do
			if row.stat == stat and row.hasBreakdown and (row.actor == nil or row.actor == "player") then idx = i; break end
		end
		if not idx then
			local have = {}
			for _, row in ipairs(side.rows) do
				if row.stat and row.hasBreakdown then have[#have + 1] = row.stat end
			end
			argerr("no breakdown for %s; stats with one: %s", stat, table.concat(have, ", "))
		end
		local bd = english(function() return M.sidebar_breakdown({ rowIndex = idx }) end)
		local lines = {}
		for _, s in ipairs(bd.sections) do
			if s.type == "text" then
				for _, l in ipairs(s.lines) do lines[#lines + 1] = l end
			elseif s.type == "table" then
				if s.label then lines[#lines + 1] = "[" .. s.label .. "]" end
				for _, r in ipairs(s.rows) do
					local parts = {}
					for _, c in ipairs(s.cols) do
						local v = r[c.key]
						if v and v ~= "" then parts[#parts + 1] = (c.label ~= "" and (c.label .. ": ") or "") .. v end
					end
					lines[#lines + 1] = table.concat(parts, " | ")
				end
			end
			if #lines >= 120 then break end
		end
		for i, l in ipairs(lines) do lines[i] = (l:gsub("%^x%x%x%x%x%x%x", ""):gsub("%^%d", "")) end
		return { stat = stat, value = b.calcsTab.mainOutput[stat], lines = lines }
	end

	-- ---- check -----------------------------------------------------------

	local DEFAULT_RULES = {
		resistMin = 75, chaosResistMin = 0, poolError = 3000, poolWarn = 4000,
		hitChanceMin = 90, defenceLayersMin = 2,
		layers = { BlockChance = 40, SpellBlockChance = 30, SpellSuppressionChance = 70, MeleeEvadeChance = 50, PhysicalDamageReduction = 30 },
	}

	local function load_rules()
		local rules = DEFAULT_RULES
		if ctx.dir then
			local f = io.open(ctx.dir .. "mcp_check_rules.json", "r")
			if f then
				local text = f:read("*a")
				f:close()
				local ok, dkjson = pcall(require, "dkjson")
				local parsed = ok and dkjson.decode(text) or nil
				if type(parsed) == "table" then
					for k, v in pairs(parsed) do rules[k] = v end
				end
			end
		end
		return rules
	end

	local function group_has_tag(b, tag)
		for _, g in ipairs(b.skillsTab.socketGroupList) do
			if g.enabled ~= false then
				for _, gem in ipairs(g.gemList or {}) do
					local gd = gem.gemData
					if gem.enabled ~= false and gd and gd.tags and gd.tags[tag] then return true end
				end
			end
		end
		return false
	end

	function T.check_build()
		local b = ensure_build()
		local o = b.calcsTab.mainOutput
		local R = load_rules()
		local issues = {}
		local function add(rule, sev, msg, msgZh, value, threshold)
			issues[#issues + 1] = { rule = rule, severity = sev, message = msg, messageZh = msgZh, value = value, threshold = threshold }
		end
		local function num(k) local v = o[k]; return type(v) == "number" and v or nil end

		local low = {}
		for _, k in ipairs({ "FireResist", "ColdResist", "LightningResist" }) do
			local v = num(k)
			if v and v < R.resistMin then low[#low + 1] = string.format("%s %d%%", k, v) end
		end
		if #low > 0 then
			add("elemental_resist", "error", "Elemental resistances below " .. R.resistMin .. "%: " .. table.concat(low, ", "),
				"元素抗性未達 " .. R.resistMin .. "%:" .. table.concat(low, "、"), nil, R.resistMin)
		end
		local chaos = num("ChaosResist")
		local chaosImmune = o.ChaosMaximumHitTaken == math.huge
		if chaos and chaos < R.chaosResistMin and not chaosImmune then
			add("chaos_resist", "warning", string.format("Chaos resistance %d%% is below %d%%", chaos, R.chaosResistMin),
				string.format("混沌抗性 %d%% 低於 %d%%", chaos, R.chaosResistMin), chaos, R.chaosResistMin)
		end
		local pool = (num("Life") or 0) + (num("EnergyShield") or 0)
		if pool < R.poolError then
			add("life_pool", "error", string.format("Life + energy shield is only %d (below %d)", pool, R.poolError),
				string.format("生命＋能量護盾只有 %d(低於 %d)", pool, R.poolError), pool, R.poolError)
		elseif pool < R.poolWarn then
			add("life_pool", "warning", string.format("Life + energy shield is %d (below %d)", pool, R.poolWarn),
				string.format("生命＋能量護盾 %d(低於 %d)", pool, R.poolWarn), pool, R.poolWarn)
		end
		local env = b.calcsTab.mainEnv
		local ms = env and env.player and env.player.mainSkill
		local isAttack = ms and ms.skillFlags and ms.skillFlags.attack
		local hit = num("HitChance")
		if isAttack and hit and hit < R.hitChanceMin then
			add("hit_chance", "warning", string.format("Main skill hits %d%% of the time (below %d%%)", hit, R.hitChanceMin),
				string.format("主技能命中率 %d%%(低於 %d%%)", hit, R.hitChanceMin), hit, R.hitChanceMin)
		end
		local manaCost, manaLeft = num("ManaCost"), num("ManaUnreserved")
		if manaCost and manaCost > 0 and manaLeft and manaLeft < manaCost then
			add("mana_sustain", "error", string.format("Unreserved mana %d cannot pay one cast (%d)", manaLeft, manaCost),
				string.format("未保留魔力 %d 不夠施放一次(消耗 %d)", manaLeft, manaCost), manaLeft, manaCost)
		end
		local layers, met = R.layers or {}, {}
		for k, t in pairs(layers) do
			local v = num(k)
			if v and v >= t then met[#met + 1] = k end
		end
		table.sort(met)
		if #met < R.defenceLayersMin then
			add("defence_layers", "warning",
				"Fewer than " .. R.defenceLayersMin .. " defence layers at their thresholds (met: " .. (#met > 0 and table.concat(met, ", ") or "none") .. ")",
				"達標的減傷層少於 " .. R.defenceLayersMin .. " 層(已達標:" .. (#met > 0 and table.concat(met, "、") or "無") .. ")", #met, R.defenceLayersMin)
		end
		local sup = num("SpellSuppressionChance")
		if sup and sup > 0 and sup < 100 then
			add("spell_suppression_partial", "info", string.format("Spell suppression %d%% is invested but not capped", sup),
				string.format("法術壓制 %d%% 有投入但未滿", sup), sup, 100)
		end
		if not group_has_tag(b, "movement") then
			add("movement_skill", "info", "No enabled movement skill", "沒有啟用的移動技能")
		end
		if not group_has_tag(b, "guard") then
			add("guard_skill", "info", "No enabled guard skill (e.g. Steelskin, Molten Shell)", "沒有啟用的防護技能(例如鋼鐵之膚、熔岩護盾)")
		end
		return { issues = issues }
	end

	-- ---- tree ------------------------------------------------------------

	function T.tree_summary()
		local b = ensure_build()
		local ks, nt, js = {}, {}, {}
		local count = 0
		for _, n in pairs(b.spec.allocNodes) do
			if n.type ~= "ClassStart" and n.type ~= "AscendClassStart" then count = count + 1 end
			if n.type == "Keystone" then ks[#ks + 1] = node_entry(n)
			elseif n.type == "Notable" then nt[#nt + 1] = node_entry(n)
			elseif n.type == "Socket" then
				local e = node_entry(n)
				local slot = b.itemsTab.sockets and b.itemsTab.sockets[n.id]
				local it = slot and b.itemsTab.items[slot.selItemId]
				if it then e.jewel = { name = it.name, nameZh = tr(it.name) } end
				js[#js + 1] = e
			end
		end
		local function byName(x, y) return (x.name or "") < (y.name or "") end
		table.sort(ks, byName); table.sort(nt, byName); table.sort(js, byName)
		return { allocatedCount = count, keystones = ks, notables = nt, jewelSockets = js }
	end

	function T.suggest_nodes(a)
		local b = ensure_build()
		local metric = (a and a.metric) or "offence"
		if metric ~= "offence" and metric ~= "defence" then argerr("metric must be \"offence\" or \"defence\"") end
		local top = math.max(1, math.min(50, tonumber(a and a.top) or 10))
		local r = M.node_power({})
		local list = {}
		for id, n in pairs(b.spec.nodes) do
			local pw = (not n.alloc) and n.power and n.power[metric]
			-- Only nodes this tree can reach (PassiveSpec:BuildAllDependsAndPaths sets
			-- pathDist; 1000 = unreachable, e.g. another class's ascendancy).
			local reachable = type(n.pathDist) == "number" and n.pathDist < 1000
			if type(pw) == "number" and pw > 0 and n.type ~= "Mastery" and reachable then
				local e = node_entry(n)
				e.id = tonumber(id) or n.id
				e.power = pw
				-- PassiveSpec:BuildPathFromNode keeps each node's distance from the allocated tree
				e.pathDist = n.pathDist
				list[#list + 1] = e
			end
		end
		table.sort(list, function(x, y) if x.power ~= y.power then return x.power > y.power end return x.id < y.id end)
		local out = {}
		for i = 1, math.min(top, #list) do out[i] = list[i] end
		return { metric = metric, nodes = out }
	end

	function T.search_nodes(a)
		local b = ensure_build()
		local q = lower(a and a.query)
		if q == "" then argerr("query required") end
		local limit = math.max(1, math.min(50, tonumber(a and a.limit) or 20))
		local out = {}
		for id, n in pairs(b.spec.nodes) do
			if n.dn and n.type ~= "ClassStart" and n.type ~= "AscendClassStart" then
				local hit = lower(n.dn):find(q, 1, true) or lower(tr(n.dn)):find(q, 1, true)
				if not hit then
					for _, l in ipairs(n.sd or {}) do
						if lower(l):find(q, 1, true) then hit = true; break end
					end
				end
				if hit then
					local e = node_entry(n)
					e.id = tonumber(id) or n.id
					e.allocated = n.alloc and true or false
					e.stats = n.sd
					out[#out + 1] = e
				end
			end
		end
		local rank = { Keystone = 1, Notable = 2, Socket = 3, Mastery = 4 }
		table.sort(out, function(x, y)
			local rx, ry = rank[x.type] or 9, rank[y.type] or 9
			if rx ~= ry then return rx < ry end
			return x.id < y.id
		end)
		local cut = {}
		for i = 1, math.min(limit, #out) do cut[i] = out[i] end
		return { nodes = cut, total = #out }
	end

	local function click_nodes(a, wantAlloc)
		local b = ensure_build()
		local ids = a and a.ids
		if type(ids) ~= "table" or #ids == 0 then argerr("ids required: a list of node ids (from search_nodes / suggest_nodes)") end
		local effects = {}
		for _, m in ipairs((a and a.masteries) or {}) do effects[tonumber(m.id)] = tonumber(m.effect) end
		-- All or nothing: POB applies a click before the bridge can tell it was
		-- unwanted (an ascendancy switch), and a later id may fail after earlier
		-- ones landed -- so any error puts the whole build back first.
		local snap = M.export_code().code
		local ascStart = b.spec.curAscendClassId
		local ok, err = pcall(function()
			for _, raw in ipairs(ids) do
				local id = tonumber(raw)
				local n = id and b.spec.nodes[id]
				if not n then argerr("no passive node with id %s", tostring(raw)) end
				if (n.alloc and true or false) ~= wantAlloc then
					local p = { id = id }
					if wantAlloc and n.type == "Mastery" then
						p.effect = effects[id]
						if not p.effect then argerr("node %d is a mastery: pass masteries=[{id=%d, effect=<effect id>}]", id, id) end
					end
					local ascBefore = b.spec.curAscendClassId
					local r = M.tree_click(p)
					if type(r) == "table" and r.blocked then argerr("node %d cannot be changed: %s", id, tostring(r.blocked)) end
					if type(r) == "table" and r.needsConfirm then
						argerr("node %d belongs to another class's ascendancy; use new_build to change class", id)
					end
					-- From None, POB picks the node's ascendancy (allowed, reported);
					-- from one ascendancy to another it would wipe the first one's points.
					if ascBefore ~= 0 and b.spec.curAscendClassId ~= ascBefore then
						argerr("node %d is on a different ascendancy than the build's; it was not allocated", id)
					end
					if (b.spec.nodes[id].alloc and true or false) ~= wantAlloc then
						argerr("node %d could not be %s (Path of Building found no path or refused it)", id, wantAlloc and "allocated" or "deallocated")
					end
				end
			end
		end)
		if not ok then
			local okRestore, restoreErr = pcall(M.import_code, { code = snap, mode = "replace" })
			if not okRestore then
				error(tostring(err) .. "; RESTORING THE BUILD FAILED (" .. tostring(restoreErr) .. "): load it again", 0)
			end
			error(tostring(err) .. " -- no node was changed", 0)
		end
		local count = 0
		for _, n in pairs(b.spec.allocNodes) do
			if n.type ~= "ClassStart" and n.type ~= "AscendClassStart" then count = count + 1 end
		end
		local r = { ok = true, allocatedCount = count, summary = summary() }
		if b.spec.curAscendClassId ~= ascStart then r.ascendancyChangedTo = b.spec.curAscendClassName end
		return r
	end

	function T.allocate_nodes(a) return click_nodes(a, true) end
	function T.deallocate_nodes(a) return click_nodes(a, false) end

	-- ---- skills ----------------------------------------------------------

	function T.list_skills()
		local r = M.list_skills()
		local groups = {}
		for i, g in ipairs(r.groups) do
			local gems = {}
			for j, gem in ipairs(g.gems) do
				gems[j] = { name = gem.name, nameZh = gem.nameZh, level = gem.level, quality = gem.quality, enabled = gem.enabled, support = gem.support, error = gem.errMsg }
			end
			groups[i] = { index = g.index, label = g.label ~= "" and g.label or nil, slot = g.slot, enabled = g.enabled, isMain = g.isMain, fromItem = g.source or nil, gems = gems }
		end
		return { groups = groups, mainGroup = r.mainSocketGroup }
	end

	function T.search_gems(a)
		local q = a and a.query
		if type(q) ~= "string" or not q:match("%S") then argerr("query required (part of a gem name, English or Chinese)") end
		ensure_build()
		local r = M.gem_search({ query = q, limit = tonumber(a.limit) or 20, supportOnly = a.support == true, activeOnly = a.support == false })
		local out = {}
		for i, g in ipairs(r.gems) do
			local tags = {}
			for k in pairs(g.tags or {}) do if type(k) == "string" then tags[#tags + 1] = k end end
			table.sort(tags)
			out[i] = { id = g.gemId, name = g.name, nameZh = g.nameZh, support = g.support and true or false, tags = tags }
		end
		return { gems = out }
	end

	local function resolve_gem(name)
		local r = M.gem_search({ query = name, limit = 100 })
		local want = lower(name)
		for _, g in ipairs(r.gems) do
			if lower(g.name) == want or lower(g.nameZh) == want then return g.name end
		end
		local near = {}
		for i = 1, math.min(5, #r.gems) do near[#near + 1] = r.gems[i].name end
		argerr("unknown gem %q%s", name, #near > 0 and (" (did you mean: " .. table.concat(near, ", ") .. ")") or "")
	end

	function T.set_skill_group(a)
		local b = ensure_build()
		local gems = a and a.gems
		if type(gems) ~= "table" or #gems == 0 then argerr("gems required: [{name, level?, quality?}]") end
		local list = {}
		for i, g in ipairs(gems) do
			if type(g) ~= "table" or type(g.name) ~= "string" then argerr("gems[%d].name required", i) end
			list[i] = { nameSpec = resolve_gem(g.name), level = tonumber(g.level) or 20, quality = tonumber(g.quality) or 0 }
		end
		local tab = b.skillsTab
		local index = tonumber(a.index)
		if index then
			local g = tab.socketGroupList[index]
			if not g then argerr("no skill group %d (list_skills shows %d)", index, #tab.socketGroupList) end
			if g.source then argerr("skill group %d comes from an item and cannot be edited", index) end
			g.gemList = {}
			for _, s in ipairs(list) do
				g.gemList[#g.gemList + 1] = {
					nameSpec = s.nameSpec, level = s.level, quality = s.quality, enabled = true, count = 1,
					enableGlobal1 = true, enableGlobal2 = true, new = true,
				}
			end
			M.set_group({ index = index, slot = a.slot })
		else
			index = M.add_group({ slot = a.slot, gems = list }).index
		end
		if a.main then
			b.mainSocketGroup = index
			M.set_group({ index = index })
		end
		return { ok = true, index = index, summary = summary() }
	end

	-- ---- items -----------------------------------------------------------

	local function item_text(it)
		local ok, raw = pcall(function() return it.raw or it:BuildRaw() end)
		return ok and raw or nil
	end

	function T.list_items()
		local b = ensure_build()
		local tab = b.itemsTab
		if tab.UpdateSockets then tab:UpdateSockets() end
		local slots = {}
		for _, slot in ipairs(tab.orderedSlots) do
			if slot:IsShown() and not slot.nodeId then
				local e = { slot = slot.slotName }
				local it = tab.items[slot.selItemId or 0]
				if it then
					e.item = { name = it.name, nameZh = (it.title and it.baseName) and (tr(it.title) .. ", " .. tr(it.baseName)) or tr(it.name),
						base = it.baseName, rarity = it.rarity, text = item_text(it) }
				end
				slots[#slots + 1] = e
			end
		end
		return { slots = slots }
	end

	function T.search_uniques(a)
		local q = lower(a and a.query)
		if q == "" then argerr("query required (part of a unique's name, English or Chinese)") end
		local limit = math.max(1, math.min(30, tonumber(a and a.limit) or 10))
		local wantType = lower(a and a.slot)
		local db = main().uniqueDB
		local out = {}
		for _, it in pairs(db and db.list or {}) do
			if type(it) == "table" and it.name then
				local zh = (it.title and it.baseName) and (tr(it.title) .. ", " .. tr(it.baseName)) or tr(it.name)
				local typeOk = wantType == "" or lower(it.type):find(wantType, 1, true)
				if typeOk and (lower(it.name):find(q, 1, true) or lower(zh):find(q, 1, true)) then
					out[#out + 1] = { name = it.name, nameZh = zh, base = it.baseName, type = it.type, text = item_text(it) }
				end
			end
		end
		table.sort(out, function(x, y) return x.name < y.name end)
		local cut = {}
		for i = 1, math.min(limit, #out) do cut[i] = out[i] end
		return { items = cut, total = #out }
	end

	function T.list_bases(a)
		local q = lower(a and a.query)
		local wantType = lower(a and a.type)
		local limit = math.max(1, math.min(50, tonumber(a and a.limit) or 20))
		local out = {}
		for name, base in pairs(data.itemBases or {}) do
			if type(base) == "table" and not base.hidden
				and (q == "" or lower(name):find(q, 1, true) or lower(tr(name)):find(q, 1, true))
				and (wantType == "" or lower(base.type):find(wantType, 1, true)) then
				out[#out + 1] = { name = name, nameZh = tr(name), type = base.type, subType = base.subType, implicit = base.implicit, level = base.req and base.req.level }
			end
		end
		table.sort(out, function(x, y) return x.name < y.name end)
		local cut = {}
		for i = 1, math.min(limit, #out) do cut[i] = out[i] end
		return { bases = cut, total = #out }
	end

	function T.equip_item(a)
		local b = ensure_build()
		local text = a and a.text
		if type(text) ~= "string" or not text:match("%S") then argerr("text required: the item in POB / game copy format (Rarity line, name, base, mods)") end
		local slot = a.slot
		if slot ~= nil and slot ~= "" and not b.itemsTab.slots[slot] then
			local names = {}
			for _, s in ipairs(b.itemsTab.orderedSlots) do if not s.nodeId then names[#names + 1] = s.slotName end end
			argerr("unknown slot %q; one of: %s", slot, table.concat(names, ", "))
		end
		local r = M.add_item({ raw = text, slotName = (slot ~= "" and slot) or nil })
		local tab = b.itemsTab
		local it = tab.items[r.item.id]
		local where = it and find_slot_of(tab, it.id)
		if it and not where then
			-- POB's auto-equip only fills an EMPTY slot (ItemsTab:AddItem); the
			-- tool promises "into the first slot it fits", so replace what is
			-- there, preferring the item's own primary slot.
			local want = r.item.primarySlot
			if not (want and tab.slots[want] and tab:IsItemValidForSlot(it, want)) then
				want = nil
				for _, s in ipairs(tab.orderedSlots) do
					if not s.nodeId and s:IsShown() and tab:IsItemValidForSlot(it, s.slotName) then want = s.slotName; break end
				end
			end
			if not want then
				tab:DeleteItem(it)
				argerr("no slot can hold %s (base %s)", tostring(r.item.name), tostring(r.item.baseName))
			end
			M.equip_item({ id = it.id, slotName = want })
			where = want
		end
		return { ok = true, slot = where, item = { name = r.item.name, nameZh = r.item.nameZh }, summary = summary() }
	end

	function T.unequip_item(a)
		ensure_build()
		M.unequip_slot({ slotName = a and a.slot })
		return { ok = true, summary = summary() }
	end

	-- ---- config ----------------------------------------------------------

	function T.get_config()
		local r = M.list_config()
		local out = {}
		for _, sec in ipairs(r.sections) do
			for _, it in ipairs(sec.items) do
				if it.visible then
					local e = { key = it.var, section = sec.name, label = it.label, labelZh = it.labelZh, type = it.type, value = it.value }
					if it.list then
						local ch = {}
						for i, o in ipairs(it.list) do ch[i] = { value = o.val, label = o.label } end
						e.choices = ch
					end
					out[#out + 1] = e
				end
			end
		end
		return { options = out }
	end

	function T.set_config(a)
		local key = a and a.key
		if type(key) ~= "string" or key == "" then argerr("key required (from get_config)") end
		M.set_config({ var = key, value = a.value })
		return { ok = true, summary = summary() }
	end

	-- ---- what if ---------------------------------------------------------

	local MUTATING = { allocate_nodes = true, deallocate_nodes = true, set_skill_group = true, equip_item = true, unequip_item = true, set_config = true }

	function T.what_if(a)
		local b = ensure_build()
		local actions = a and a.actions
		if type(actions) ~= "table" or #actions == 0 then argerr("actions required: [{tool, arguments}]") end
		for i, act in ipairs(actions) do
			if type(act) ~= "table" or not MUTATING[act.tool] then
				argerr("actions[%d].tool must be one of allocate_nodes, deallocate_nodes, set_skill_group, equip_item, unequip_item, set_config", i)
			end
		end
		local snap = M.export_code().code
		local before = key_stats(b)
		local failed
		for i, act in ipairs(actions) do
			local ok, err = pcall(T[act.tool], act.arguments or {})
			if not ok then failed = string.format("actions[%d] (%s): %s", i, act.tool, tostring(err)); break end
		end
		local after
		if not failed then
			local ok, s = pcall(function() return key_stats(ensure_build()) end)
			if ok then after = s else failed = "reading the result: " .. tostring(s) end
		end
		local okRestore, restoreErr = pcall(M.import_code, { code = snap, mode = "replace" })
		if not okRestore then
			error((failed and (failed .. "; ") or "") .. "RESTORING THE BUILD FAILED (" .. tostring(restoreErr) ..
				"): load the build again before trusting any number", 0)
		end
		if failed then error(failed .. " -- the build was restored", 0) end
		local diff = obj({})
		for k, v in pairs(after) do
			local d = v - (before[k] or 0)
			if d ~= 0 then diff[k] = d end
		end
		for k, v in pairs(before) do
			if after[k] == nil then diff[k] = -v end
		end
		return { before = before, after = after, diff = diff, reverted = true }
	end

	-- ---- entry -----------------------------------------------------------

	function M.mcp_call(p)
		local name = p and p.name
		local fn = type(name) == "string" and T[name]
		if not fn then error("unknown tool " .. tostring(name), 0) end
		return fn(p.arguments or {})
	end

	function M.mcp_tool_names()
		local names = {}
		for k in pairs(T) do names[#names + 1] = k end
		table.sort(names)
		return { names = names }
	end
end
