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
-- draws in Traditional Chinese (English and Chinese shown side by side).

return function(M, ctx)
	local tr, gem_tr, ensure_build, main = ctx.tr, ctx.gem_tr, ctx.ensure_build, ctx.main

	local T = {}

	-- ---- helpers ----------------------------------------------------------

	-- argerr's messages carry an invisible mark so mcp_call can tell "the model
	-- passed something wrong" from a PobTools failure; it is stripped there.
	local ARG_MARK = "\2"
	local function argerr(fmt, ...) error(ARG_MARK .. string.format(fmt, ...), 0) end

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
		"CombinedDPS", "TotalDPS", "FullDPS", "TotalDot", "TotalDotDPS", "IgniteDPS", "BleedDPS", "PoisonDPS", "ImpaleDPS", "CullingDPS",
		"AverageHit", "Speed", "CritChance", "CritMultiplier", "HitChance",
		"FireResist", "ColdResist", "LightningResist", "ChaosResist",
		"Armour", "Evasion", "BlockChance", "SpellBlockChance", "SpellSuppressionChance",
		"PhysicalDamageReduction", "MeleeEvadeChance", "ProjectileEvadeChance",
		"LifeRegenRecovery", "EnergyShieldRegenRecovery", "ManaRegenRecovery",
		"LifeUnreserved", "ManaUnreserved", "ManaCost", "LifeCost",
		"Str", "Dex", "Int", "MovementSpeedMod",
		"StunAvoidChance", "FreezeAvoidChance", "ShockAvoidChance", "IgniteAvoidChance", "ChillAvoidChance",
		"BleedAvoidChance", "PoisonAvoidChance",
	}
	-- The minion's share of the same (mainOutput.Minion; keys as in POB's
	-- BuildDisplayStats minionDisplayStats).
	local MINION_STATS = {
		"CombinedDPS", "TotalDPS", "TotalDot", "IgniteDPS", "BleedDPS", "PoisonDPS", "AverageDamage", "Speed",
		"Life", "EnergyShield", "LifeRegenRecovery", "FireResist", "ColdResist", "LightningResist", "ChaosResist",
	}

	local function pick(out, keys)
		local s = {}
		for _, k in ipairs(keys) do
			local v = out[k]
			if type(v) == "number" and v == v and v ~= math.huge and v ~= -math.huge then s[k] = v end
		end
		return obj(s)
	end

	local function minion_stats(b)
		local m = b.calcsTab.mainOutput.Minion
		if type(m) ~= "table" then return nil end
		return pick(m, MINION_STATS)
	end

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
		local list, seen = {}, {}
		for _, n in pairs(b.spec.allocNodes) do
			if n.type == "Keystone" then
				local e = node_entry(n)
				e.stats = n.sd or {}
				list[#list + 1] = e
				seen[n.dn] = true
			end
		end
		-- Keystones granted by items / jewels (Cloak of Defiance, Skin of the
		-- Lords...): CalcPerform merges them into env.keystonesAdded.
		local env = b.calcsTab.mainEnv
		local map = b.spec.tree.keystoneMap or {}
		for name in pairs(env and env.keystonesAdded or {}) do
			local n = map[name]
			if n and not seen[name] then
				local e = node_entry(n)
				e.stats = n.sd or {}
				e.fromItem = true
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
			minionStats = minion_stats(b),
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
		-- reusePower (build_report only): POB's one power pass scores both metrics.
		if not (a and a.reusePower) then M.node_power({}) end
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

	-- Whether a socket group's gems count, by CalcSetup's own rule: the main
	-- group always does; others when enabled and their slot belongs to the
	-- weapon set in use (group.slotEnabled, set on every calculation).
	local function group_active(b, index)
		local g = b.skillsTab.socketGroupList[index]
		if not g then return false, "no such group" end
		if index == b.mainSocketGroup then return true end
		if not g.enabled then return false, "the group is disabled" end
		if g.slotEnabled == false then
			return false, "socketed in " .. tostring(g.slot) .. ", which belongs to the weapon set not in use"
		end
		return true
	end

	function T.list_skills()
		local b = ensure_build()
		local r = M.list_skills()
		local groups = {}
		for i, g in ipairs(r.groups) do
			local gems = {}
			for j, gem in ipairs(g.gems) do
				gems[j] = { name = gem.name, nameZh = gem.nameZh, level = gem.level, quality = gem.quality, enabled = gem.enabled, support = gem.support, error = gem.errMsg }
			end
			local active, why = group_active(b, g.index)
			groups[i] = { index = g.index, label = g.label ~= "" and g.label or nil, slot = g.slot, enabled = g.enabled, isMain = g.isMain,
				active = active, inactiveReason = why, fromItem = g.source or nil, gems = gems }
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
		local r = { ok = true, index = index, summary = summary() }
		local active, why = group_active(b, index)
		if not active then
			r.warning = "These gems do not affect the build: " .. why .. ". Omit index to add a new group, or set slot to an active one."
		end
		return r
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
		-- POB fills the unique DB over many frames after boot (Main.lua
		-- loadItemDBs, pairsYield): reading it early returns only part of it.
		local db = ctx.wait_db and ctx.wait_db("unique") or main().uniqueDB
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
					local v = it.value
					if it.type == "check" then v = v and true or false end -- unset is unchecked, not unknown
					local e = { key = it.var, section = sec.name, label = it.label, labelZh = it.labelZh, type = it.type, value = v }
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
		local r = M.set_config({ var = key, value = a.value })
		local after = type(r) == "table" and r.after or nil
		-- applied: what POB stored is what was asked for (a list takes its own value type)
		local isCheck = type(r) == "table" and r.type == "check"
		local cleared = a.value == nil or a.value == "" -- back to the default / the build's own value
		local before = type(r) == "table" and r.before or nil
		if isCheck then -- a checkbox: unset is unchecked (and a nil field would vanish from the JSON)
			before, after = before and true or false, after and true or false
		end
		local applied = after == a.value or tostring(after) == tostring(a.value) or (cleared and after == nil) or
		                (isCheck and after == (a.value == true))
		return { ok = true, key = key, before = before, after = after, applied = applied, summary = summary() }
	end

	function T.rank_supports(a)
		local b = ensure_build()
		local index = tonumber(a and a.index) or b.mainSocketGroup
		if not b.skillsTab.socketGroupList[index or 0] then argerr("no socket group %s (see list_skills)", tostring(a and a.index)) end
		local top = math.max(1, math.min(30, tonumber(a and a.top) or 10))
		-- POB's estimate puts each candidate into the group, calculates, then takes
		-- it out, with no pcall: an error mid-way would leave a gem in the build.
		local snap = M.export_code().code
		local okSearch, r = pcall(M.gem_search, { byDps = true, group = index, supportableOnly = true, limit = 100 })
		if not okSearch then
			local okRestore, restoreErr = pcall(M.import_code, { code = snap, mode = "replace" })
			if not okRestore then
				error(tostring(r) .. "; RESTORING THE BUILD FAILED (" .. tostring(restoreErr) .. "): load it again", 0)
			end
			error(tostring(r) .. " -- the build was restored", 0)
		end
		if not r.byDps then error("this Path of Building cannot sort gems by DPS", 0) end
		local out = {}
		for _, g in ipairs(r.gems or {}) do
			if #out >= top then break end
			if g.support and g.canSupport then
				out[#out + 1] = { name = g.name, nameZh = g.nameZh, gemId = g.gemId, dps = g.dps, delta = g.delta, pct = g.pct }
			end
		end
		return { group = index, dpsField = r.dpsField, baseDps = r.baseDps, supports = out,
		         basis = "each gem ADDED to the group as an extra gem; in a full socket group it must replace a support, so measure that swap with what_if" }
	end

	function T.rank_anoints(a)
		local b = ensure_build()
		local slot = b.itemsTab.slots and b.itemsTab.slots["Amulet"]
		local id = slot and slot.selItemId
		if not id or id == 0 or not b.itemsTab.items[id] then
			argerr("no amulet is equipped; equip one first (equip_item with slot Amulet)")
		end
		local ok, r = pcall(function()
			M.item_edit_begin({ id = id })
			M.item_edit_popup({ action = "open", kind = "anoint", slot = 1 })
			return M.item_edit_popup({ action = "rank", metric = (a and a.metric) or "CombinedDPS", top = a and a.top })
		end)
		M.item_edit_cancel()
		if not ok then error(r, 0) end
		r.basis = "power is measured against the same amulet with NO anoint, so the anoint already on it is ranked too"
		return r
	end

	-- ---- what if ---------------------------------------------------------

	-- after - before for every key that moved (a key gone from after counts as 0).
	local function stat_diff(before, after)
		local diff = obj({})
		for k, v in pairs(after) do
			local d = v - (before[k] or 0)
			if d ~= 0 then diff[k] = d end
		end
		for k, v in pairs(before) do
			if after[k] == nil then diff[k] = -v end
		end
		return diff
	end

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
		return { before = before, after = after, diff = stat_diff(before, after), reverted = true }
	end

	-- ---- scenarios / inflation audit -------------------------------------

	-- Run fn on the live build, then always put it back from the share code
	-- (as what_if does): any step may throw half way through.
	local function measured(fn)
		local snap = M.export_code().code
		local viewMode = ensure_build().viewMode
		local ok, r = pcall(fn)
		local okRestore, restoreErr = pcall(M.import_code, { code = snap, mode = "replace" })
		if not okRestore then
			error((ok and "" or (tostring(r) .. "; ")) .. "RESTORING THE BUILD FAILED (" .. tostring(restoreErr) ..
				"): load the build again before trusting any number", 0)
		end
		-- POB's import opens the build on the tree tab; the tab is saved in the build.
		ensure_build().viewMode = viewMode
		if not ok then error(tostring(r) .. " -- the build was restored", 0) end
		return r
	end

	-- list_config's options by var (first entry wins), checking the boss
	-- option up front: guessing another key would measure the wrong enemy.
	local function config_items()
		local r = M.list_config()
		local byVar, order = {}, {}
		for _, sec in ipairs(r.sections) do
			for _, it in ipairs(sec.items) do
				if not byVar[it.var] then byVar[it.var] = it; order[#order + 1] = it end
			end
		end
		local choices = {}
		for _, o in ipairs(byVar.enemyIsBoss and byVar.enemyIsBoss.list or {}) do choices[o.val] = true end
		if not (choices.Pinnacle and choices.Uber) then
			error("this Path of Building has no enemyIsBoss setting with Pinnacle and Uber choices", 0)
		end
		return byVar, order, r.customMods or {}
	end

	-- Each M.set_config is a full recalculation: skip the ones that change nothing.
	local function set_boss(cur, want)
		if cur ~= want then M.set_config({ var = "enemyIsBoss", value = want }) end
		return want
	end

	-- Slots with an "active" tick (the bridge's flask slots), ticked and holding an item.
	local function active_flasks(b)
		local tab, list = b.itemsTab, {}
		for _, slot in ipairs(tab.orderedSlots) do
			local it = slot.controls and slot.controls.activate and slot.active and tab.items[slot.selItemId or 0]
			if it then list[#list + 1] = { slot = slot.slotName, item = it } end
		end
		return list
	end

	local function flasks_active(list, on)
		for _, f in ipairs(list) do M.set_slot_active({ slotName = f.slot, active = on }) end
	end

	function T.scenarios()
		local b = ensure_build()
		return measured(function()
			local byVar = config_items()
			local s, minion = { current = key_stats(b) }, {}
			-- A minion skill's damage is not in the player's stats.
			local function snap_minion(k)
				local m = b.calcsTab.mainOutput.Minion
				if type(m) == "table" then minion[k] = m.CombinedDPS end
			end
			snap_minion("current")
			-- Flasks first, so the boss option never has to be put back by hand.
			local flasks = active_flasks(b)
			flasks_active(flasks, false)
			s.flasksOff = key_stats(b)
			snap_minion("flasksOff")
			flasks_active(flasks, true)
			local boss = set_boss(byVar.enemyIsBoss.value, "Pinnacle")
			s.pinnacle = key_stats(b)
			snap_minion("pinnacle")
			set_boss(boss, "Uber")
			s.uber = key_stats(b)
			snap_minion("uber")
			return { scenarios = obj(s), minionDps = next(minion) and minion or nil, reverted = true }
		end)
	end

	-- One POB recalculation per switch (twice per contributor, once more for the
	-- baseline): the slowest read-only tool, hence its longer host timeout.
	local CHARACTER_CHOICE = { bandit = true, pantheonMajorGod = true, pantheonMinorGod = true, resistancePenalty = true }

	function T.audit_inflation()
		local b = ensure_build()
		local tab = b.configTab
		if type(tab.GetDefaultState) ~= "function" then error("this Path of Building has no ConfigTab:GetDefaultState", 0) end
		return measured(function()
			local byVar, order, custom = config_items()
			local current = key_stats(b)
			local list = {}
			-- A minion skill's damage is the minion's: the player's own is 0.
			local function dps_now()
				local o = b.calcsTab.mainOutput
				return (type(o.Minion) == "table" and o.Minion.CombinedDPS) or o.CombinedDPS or 0
			end
			local dps0 = dps_now()
			local function measure(kind, key, label, labelZh)
				local e = { kind = kind, key = key, label = label, labelZh = labelZh,
					dps = dps0 - dps_now(), ehp = (current.TotalEHP or 0) - (b.calcsTab.mainOutput.TotalEHP or 0) }
				if e.dps ~= 0 or e.ehp ~= 0 then list[#list + 1] = e end
			end
			-- POB's own "changed" rule (the highlight in ConfigTab): set, and not
			-- its GetDefaultState (placeholder, else the option's default).
			-- Left alone: the character's own choices, and a list value this POB
			-- no longer offers (old builds), which set_config could not put back.
			local changed = {}
			for _, it in ipairs(order) do
				local cur = it.value
				local offered = not it.list
				for _, o in ipairs(it.list or {}) do offered = offered or o.val == cur end
				if cur ~= nil and offered and not CHARACTER_CHOICE[it.var] and cur ~= tab:GetDefaultState(it.var, type(cur)) then
					changed[#changed + 1] = it
				end
			end
			for _, it in ipairs(changed) do
				M.reset_config({ var = it.var })
				measure("config", it.var, it.label, it.labelZh)
				M.set_config({ var = it.var, value = it.value })
			end
			local flasks = active_flasks(b)
			for _, f in ipairs(flasks) do
				M.set_slot_active({ slotName = f.slot, active = false })
				local it = f.item
				measure("flask", f.slot, it.name, (it.title and it.baseName) and (tr(it.title) .. ", " .. tr(it.baseName)) or tr(it.name))
				M.set_slot_active({ slotName = f.slot, active = true })
			end
			local hasCustom = false
			for _, blk in ipairs(custom) do
				if blk.enabled and blk.text:match("%S") then hasCustom = true end
			end
			-- From here on nothing is put back by hand: the snapshot does it.
			if hasCustom then
				M.set_custom_mods({ list = {} })
				measure("customMods", "customMods", "Custom Modifiers", tr("Custom Modifiers"))
			end
			for _, it in ipairs(changed) do
				if it.var ~= "enemyIsBoss" then M.reset_config({ var = it.var }) end
			end
			flasks_active(flasks, false)
			set_boss(byVar.enemyIsBoss.value, "Pinnacle")
			local baseline = key_stats(b)
			table.sort(list, function(x, y)
				local dx, dy = math.abs(x.dps), math.abs(y.dps)
				if dx ~= dy then return dx > dy end
				local ex, ey = math.abs(x.ehp), math.abs(y.ehp)
				if ex ~= ey then return ex > ey end
				return x.key < y.key
			end)
			local top = {}
			for i = 1, math.min(15, #list) do top[i] = list[i] end
			return { current = current, baseline = baseline, diff = stat_diff(current, baseline),
				contributors = top, dpsNow = dps0, dpsBaseline = dps_now(), configChanged = #changed, partyAudited = false, reverted = true }
		end)
	end

	-- ---- one-shot report -------------------------------------------------

	-- Pasted into free AI chats, whose input boxes take about this much.
	local REPORT_LIMIT = 24000

	local function u8len(s) return #s - select(2, s:gsub("[\128-\191]", "")) end
	-- The first n characters, never splitting a multi-byte one.
	local function u8cut(s, n)
		local count = 0
		for i = 1, #s do
			local c = s:byte(i)
			if c < 128 or c >= 192 then
				count = count + 1
				if count > n then return s:sub(1, i - 1) end
			end
		end
		return s
	end

	-- Rounded half away from zero, as std::llround; "+ 0" turns -0 into 0.
	local function int(v)
		local r = v >= 0 and math.floor(v + 0.5) or -math.floor(-v + 0.5)
		return string.format("%.0f", r + 0)
	end
	local function num(v)
		if type(v) ~= "number" or v ~= v then return "n/a" end
		if v == math.huge then return "inf" elseif v == -math.huge then return "-inf" end
		if v == math.floor(v) or math.abs(v) >= 100 then return int(v) end
		return string.format("%.2f", v)
	end
	local function signed(v) return (type(v) == "number" and v > 0 and "+" or "") .. num(v) end
	local function pct(part, whole)
		if type(part) ~= "number" or type(whole) ~= "number" or whole == 0 then return "" end
		return string.format(" (%+.1f%%)", part / whole * 100)
	end
	local function nm(name, zh)
		if type(name) ~= "string" or name == "" then return "?" end
		-- POB colour codes (^xRRGGBB, ^7) are noise outside its own window.
		local function plain(s) return (s:gsub("%^x%x%x%x%x%x%x", ""):gsub("%^%d", "")) end
		name, zh = plain(name), type(zh) == "string" and plain(zh) or nil
		if type(zh) == "string" and zh ~= "" and zh ~= name then return name .. " / " .. zh end
		return name
	end

	-- The item's lines as POB uses them: other variants' lines, the internal id
	-- and the name lines already in the heading dropped, mod-range tags stripped.
	-- shortcut: "Selected Alt Variant" (two-variant uniques) is not resolved; add it if such items mislead.
	local function item_lines(it)
		local text = it.text or ""
		local sel = tonumber(text:match("Selected Variant: (%d+)"))
		local out, i = {}, 0
		for ln in text:gmatch("[^\r\n]+") do
			i = i + 1
			local keep = not (ln:match("^Rarity:") or ln:match("^Unique ID:") or ln:match("^Variant:") or
				ln:match("^Selected Variant:") or ln:match("^Selected Alt Variant:") or ln:match("^Has Alt Variant") or
				ln:match("^%a+BasePercentile:") or ln:match("^Item Level:") or ln:match("^LevelReq:"))
			local vars = ln:match("^{variant:([%d,]+)}")
			if keep and vars and sel then
				keep = false
				for v in vars:gmatch("%d+") do if tonumber(v) == sel then keep = true end end
			end
			if keep and i <= 3 and (it.name or ""):find(ln, 1, true) then keep = false end
			if keep then
				ln = ln:gsub("{variant:[^}]*}", ""):gsub("{range:[^}]*}", ""):gsub("{tags:[^}]*}", "")
				out[#out + 1] = "    " .. ln
			end
		end
		return out
	end

	function T.build_report()
		local b = ensure_build()
		local snap = M.export_code().code
		local viewMode = b.viewMode
		-- One part failing (no amulet to anoint, an old POB) costs one line, not the report.
		local function try(fn, ...)
			local ok, r = pcall(fn, ...)
			if ok then return r end
			-- Every later number would be measured on a build left changed.
			if tostring(r):find("RESTORING THE BUILD FAILED", 1, true) then error(r, 0) end
			return nil, (tostring(r):gsub(ARG_MARK, ""):gsub(" %-%- the build was restored$", ""))
		end
		local L = {}
		local function add(s) L[#L + 1] = s end

		local sm = summary()
		add("# Path of Building build report")
		add("")
		add("## 1. Instructions / 給 AI 的說明")
		add("- Every number in this report was calculated by Path of Building (PoE1) through PobTools; none is an estimate.")
		add("- Your knowledge of the current league may be out of date (skills, uniques and mechanics change every league). Where it disagrees with this report, trust the report.")
		add("- You cannot recalculate this build. Prefer the candidates in section 7, which were tested here with their measured effect; mark any other idea as untested.")
		add("- Section 3 shows how much each condition the player switched on adds. That is not proof it is wrong: ask whether it holds in their play.")
		add("- Answer in the user's language.")
		add("")
		add("## 2. Build summary / 流派摘要")
		if sm.buildName and sm.buildName ~= "" then add("- Name: " .. sm.buildName) end
		add("- Class: " .. nm(sm.class, sm.classZh) .. ((sm.ascendancy and sm.ascendancy ~= "None") and (", ascendancy " .. nm(sm.ascendancy, sm.ascendancyZh)) or ""))
		add("- Level: " .. num(sm.level))
		add(string.format("- Passive points: %s / %s, ascendancy %s / %s", num(sm.points.used), num(sm.points.max), num(sm.points.ascUsed), num(sm.points.ascMax)))
		if sm.mainSkill then add("- Main skill: " .. nm(sm.mainSkill.name, sm.mainSkill.nameZh)) end
		local ksn = {}
		for _, k in ipairs(sm.keystones) do ksn[#ksn + 1] = nm(k.name, k.nameZh) .. (k.fromItem and " (from an item)" or "") end
		add("- Keystones: " .. (#ksn > 0 and table.concat(ksn, ", ") or "none"))
		if #sm.immuneTo > 0 then add("- Immune to (Path of Building rates these hits as unable to kill): " .. table.concat(sm.immuneTo, ", ")) end
		add("Key numbers:")
		for _, k in ipairs(KEY_STATS) do
			if sm.stats[k] then add("- " .. k .. ": " .. num(sm.stats[k])) end
		end
		if sm.minionStats then
			for _, k in ipairs(MINION_STATS) do
				if sm.minionStats[k] then add("- Minion " .. k .. ": " .. num(sm.minionStats[k])) end
			end
		end
		add("")

		add("## 3. Inflation audit / 去水分")
		add("How much each setting the player switched on, each active flask and the custom modifiers add, measured by switching off only that one. This is the impact of each, not a verdict that it is inflated.")
		add("Party members' auras and buffs (Party tab) were not audited (partyAudited=false).")
		local au, auErr = try(T.audit_inflation)
		if not au then
			add("- Not available: " .. auErr)
		else
			add("Conservative baseline (Pinnacle boss, flasks off, custom modifiers cleared, every changed setting back to its default):")
			for _, k in ipairs({ "CombinedDPS", "FullDPS", "TotalEHP", "Life", "EnergyShield" }) do
				if au.current[k] or au.baseline[k] then
					add(string.format("- %s: now %s, baseline %s%s", k, num(au.current[k]), num(au.baseline[k]),
						pct((au.baseline[k] or 0) - (au.current[k] or 0), au.current[k])))
				end
			end
			add(string.format("- Main skill DPS (the minion's for a minion skill): now %s, baseline %s%s", num(au.dpsNow), num(au.dpsBaseline),
				pct(au.dpsBaseline - au.dpsNow, au.dpsNow)))
			if #au.contributors == 0 then
				add("- No setting, flask or custom modifier changes the main skill DPS or effective HP.")
			else
				add("Largest contributors (main skill DPS / TotalEHP each one adds):")
				for _, c in ipairs(au.contributors) do
					add(string.format("- [%s] %s (%s): DPS %s%s, EHP %s%s", c.kind, nm(c.label, c.labelZh), tostring(c.key),
						signed(c.dps), pct(c.dps, au.dpsNow), signed(c.ehp), pct(c.ehp, au.current.TotalEHP)))
				end
			end
		end
		add("")

		add("## 4. Scenarios / 多情境")
		local sc, scErr = try(T.scenarios)
		if not sc then
			add("- Not available: " .. scErr)
		else
			local cols = { "CombinedDPS", "FullDPS", "TotalEHP", "Life", "EnergyShield", "PhysicalMaximumHitTaken" }
			add("| Scenario | " .. table.concat(cols, " | ") .. " |")
			add("|---" .. string.rep("|---", #cols) .. "|")
			local names = { { "current", "As configured" }, { "pinnacle", "Pinnacle boss" }, { "uber", "Uber pinnacle boss" }, { "flasksOff", "All flasks off" } }
			for _, n in ipairs(names) do
				local s, row = sc.scenarios[n[1]] or {}, {}
				for i, k in ipairs(cols) do row[i] = num(s[k]) end
				add("| " .. n[2] .. " | " .. table.concat(row, " | ") .. " |" ..
					(sc.minionDps and (" minion CombinedDPS " .. num(sc.minionDps[n[1]])) or ""))
			end
		end
		add("")

		add("## 5. Issues and gaps / 問題與缺口")
		local ck, ckErr = try(T.check_build)
		if not ck then
			add("- Not available: " .. ckErr)
		elseif #ck.issues == 0 then
			add("- The standard checks found nothing.")
		else
			for _, it in ipairs(ck.issues) do add("- [" .. it.severity .. "] " .. it.message) end
		end
		add("")

		add("## 6. Build details / 天賦、技能、設定、裝備")
		local tree, tErr = try(T.tree_summary)
		add("### Passive tree")
		if not tree then add("- Not available: " .. tErr) else
			add("- Allocated nodes: " .. num(tree.allocatedCount))
			local nt = {}
			for _, n in ipairs(tree.notables) do nt[#nt + 1] = nm(n.name, n.nameZh) end
			add("- Notables: " .. (#nt > 0 and table.concat(nt, ", ") or "none"))
			for _, s in ipairs(tree.jewelSockets) do
				add("- Jewel socket " .. tostring(s.id) .. ": " .. (s.jewel and nm(s.jewel.name, s.jewel.nameZh) or "empty"))
			end
		end
		local sk, skErr = try(T.list_skills)
		add("### Skill groups")
		if not sk then add("- Not available: " .. skErr) else
			for _, g in ipairs(sk.groups) do
				local gems = {}
				for _, gem in ipairs(g.gems) do
					gems[#gems + 1] = nm(gem.name, gem.nameZh) .. " " .. num(gem.level) .. "/" .. num(gem.quality) ..
						(gem.enabled == false and " (disabled)" or "")
				end
				add(string.format("- Group %d%s (%s%s): %s%s", g.index, g.isMain and " [main]" or "", tostring(g.slot or "no slot"),
					g.label and (", " .. g.label) or "", table.concat(gems, ", "),
					g.active and "" or (" -- not counted: " .. tostring(g.inactiveReason))))
			end
		end
		local cf, cfErr = try(T.get_config)
		add("### Configuration (options that are set)")
		if not cf then add("- Not available: " .. cfErr) else
			local n = 0
			for _, o in ipairs(cf.options) do
				if o.value ~= nil and o.value ~= false and o.value ~= "" then
					n = n + 1
					add("- " .. nm(o.label, o.labelZh) .. " (" .. o.key .. "): " .. tostring(o.value))
				end
			end
			if n == 0 then add("- Nothing set (all defaults).") end
		end
		-- Items go last: when the report is too long their mod lines are what is cut.
		local items = {}
		local li, liErr = try(T.list_items)
		if li then
			for _, s in ipairs(li.slots) do
				if s.item then
					items[#items + 1] = { head = string.format("- %s: %s (%s, %s)", s.slot, nm(s.item.name, s.item.nameZh),
						tostring(s.item.base), tostring(s.item.rarity)), lines = item_lines(s.item) }
				end
			end
		end
		local before = table.concat(L, "\n") .. "\n### Items\n"
		if not li then before = before .. "- Not available: " .. liErr .. "\n" end
		L = {}

		add("")
		add("## 7. Tested candidates / 已試算候選")
		add("Each candidate below was applied by Path of Building one at a time and measured against the build as it is.")
		local tops = 10
		local sn, snErr = try(T.suggest_nodes, { metric = "offence", top = tops })
		add("### Passive nodes for damage (not allocated, reachable)")
		if not sn then add("- Not available: " .. snErr)
		elseif #sn.nodes == 0 then add("- No unallocated node raises the main skill DPS.") else
			for _, n in ipairs(sn.nodes) do
				add(string.format("- %s (%s, id %s, %s points away): DPS %+.1f%%", nm(n.name, n.nameZh), tostring(n.type), tostring(n.id),
					num(n.pathDist), (n.power or 0) * 100))
			end
		end
		local sd, sdErr = try(T.suggest_nodes, { metric = "defence", top = tops, reusePower = sn ~= nil })
		add("### Passive nodes for defence (Path of Building's combined defence score)")
		if not sd then add("- Not available: " .. sdErr)
		elseif #sd.nodes == 0 then add("- No unallocated node raises defence.") else
			for _, n in ipairs(sd.nodes) do
				add(string.format("- %s (%s, id %s, %s points away): defence score %.3f", nm(n.name, n.nameZh), tostring(n.type), tostring(n.id),
					num(n.pathDist), n.power or 0))
			end
		end
		local rs, rsErr = try(T.rank_supports, { top = tops })
		add("### Support gems for the main skill group")
		if not rs then add("- Not available: " .. rsErr) else
			add("Each gem added to group " .. tostring(rs.group) .. " as an extra gem (" .. tostring(rs.dpsField) ..
				", now " .. num(rs.baseDps) .. "); in a full group it has to replace a support.")
			if #rs.supports == 0 then add("- No support gem raises the DPS.") end
			for _, g in ipairs(rs.supports) do
				add("- " .. nm(g.name, g.nameZh) .. ": " .. signed(g.delta) .. (g.pct and string.format(" (%+.1f%%)", g.pct) or ""))
			end
		end
		local ra, raErr = try(T.rank_anoints, { metric = "CombinedDPS", top = tops })
		add("### Amulet anoints")
		if not ra then add("- Not available: " .. raErr) else
			add("CombinedDPS each anoint adds against the same amulet with no anoint (so the current anoint is ranked too).")
			if #ra.notables == 0 then add("- No anoint raises the DPS.") end
			for _, n in ipairs(ra.notables) do add("- " .. nm(n.name, n.nameZh) .. ": " .. signed(n.power)) end
		end
		local after = table.concat(L, "\n") .. "\n"

		-- Ranking leaves nothing behind by design; put the build back if anything did.
		if M.export_code().code ~= snap then
			local okRestore, restoreErr = pcall(M.import_code, { code = snap, mode = "replace" })
			if not okRestore then
				error("RESTORING THE BUILD FAILED (" .. tostring(restoreErr) .. "): load the build again before trusting any number", 0)
			end
			ensure_build().viewMode = viewMode
		end

		-- Over the limit: drop item mod lines from the last item backwards, then
		-- cut the end of section 6 (section 7 is what the reader is told to use).
		local function render(full, note)
			local p = {}
			for i, it in ipairs(items) do
				p[#p + 1] = it.head
				if i <= full then for _, ln in ipairs(it.lines) do p[#p + 1] = ln end end
			end
			if note then p[#p + 1] = note end
			return before .. table.concat(p, "\n") .. (#p > 0 and "\n" or "")
		end
		local room = REPORT_LIMIT - u8len(after)
		local mid, full = render(#items), #items
		while u8len(mid) > room and full > 0 do
			full = full - 1
			mid = render(full, string.format("(Mod lines of the last %d items were left out to keep this report under %d characters.)",
				#items - full, REPORT_LIMIT))
		end
		if u8len(mid) > room then
			local note = string.format("\n(Section 6 was cut to keep this report under %d characters.)\n", REPORT_LIMIT)
			mid = u8cut(mid, math.max(0, room - u8len(note))) .. note
		end
		return { text = u8cut(mid .. after, REPORT_LIMIT) }
	end

	-- ---- entry -----------------------------------------------------------

	-- Test only: the host lists it only under POB_ZH_MCP_DEBUG=1.
	function T.debug_lua_error(a)
		if a and a.kind == "plain" then
			error("this POB has no debug feature", 0) -- the bridge's own style: level 0
		end
		local missing = nil
		return missing.field -- a genuine runtime error, with file:line
	end

	-- Errors are of two kinds. A wrong argument goes back to the model to fix
	-- and is not a PobTools problem: argerr above, and the bridge's argument
	-- messages ("no node 123", "unknown gem X", "params.text required"...).
	-- Everything else -- a runtime error, "this POB has no ...", "the download
	-- did not finish", a build that could not be restored -- is a bug or a
	-- broken install: write it to the failure log with its traceback, and tell
	-- the model how the user can report it. The bridge raises everything with
	-- level 0, so the position alone cannot tell them apart.
	-- Must stay in step with mcp_server.cpp kReportHint.
	local REPORT_HINT = " (details were written to PobTools\\logs\\error-<date>.log; call report_problem to prepare a bug report for the user)"
	local ARG_PATTERNS = {
		"^no ", "^unknown ", "required", "^item does not fit", "^already exists", "^value not in list",
		"^path must be", "^bad ", "^character not in", "^fetch the character", "^cannot open ",
		"^item base not recognised", "^item text too large", "^not a build%-site link",
		"^empty code", "^not a Path of Building code", "^code is not a build file",
	}
	local function is_argument(msg)
		if msg:find("RESTORING THE BUILD FAILED", 1, true) or msg:find("%.lua:%d+:") then return false end
		if msg:find(ARG_MARK, 1, true) then return true end
		-- what_if reports "actions[2] (set_config): <the bridge's message>".
		local core = msg:gsub("^actions%[%d+%] %([%w_]+%): ", "")
		for _, pat in ipairs(ARG_PATTERNS) do
			if core:find(pat) then return true end
		end
		return false
	end

	function M.mcp_call(p)
		local name = p and p.name
		local fn = type(name) == "string" and T[name]
		if not fn then error("unknown tool " .. tostring(name), 0) end
		local trace
		local res = { xpcall(function() return fn(p.arguments or {}) end, function(e)
			trace = debug.traceback("", 2)
			return e
		end) }
		if res[1] then return res[2] end
		local raw = tostring(res[2])
		local msg = raw:gsub(ARG_MARK, "")
		if is_argument(raw) then error(msg, 0) end
		if PobToolsLogError then
			local where = (trace or ""):gsub("^%s*stack traceback:%s*", ""):sub(1, 600)
			PobToolsLogError("mcp", name .. ": " .. msg .. " | " .. where)
		end
		error(msg .. REPORT_HINT, 0)
	end

	-- Called once by the MCP host after boot, never by the new UI (which runs
	-- on the user's real install, where the cache is wanted).
	-- The sandbox's Data\ is a junction to the user's POB, and POB caches each
	-- decompressed timeless-jewel table as Data\TimelessJewelData\<name>.bin
	-- (DataJewelFileLoader, cacheUncompressed defaults to true): that would
	-- write up to 50 MB into the user's folder. Wrap the loader (call the
	-- original, bridge rule 3) so the sandbox reads without caching.
	function M.mcp_setup()
		local readLUT = data and data.readLUT
		local function upvalue(fn, want)
			if type(fn) ~= "function" then return nil end
			for i = 1, 64 do
				local n, v = debug.getupvalue(fn, i)
				if not n then return nil end
				if n == want then return i, v end
			end
		end
		local _, loadTimeless = upvalue(readLUT, "loadTimelessJewel")
		local idx, loadJewelFile = upvalue(loadTimeless, "loadJewelFile")
		if not idx or type(loadJewelFile) ~= "function" then
			return { jewelCacheOff = false }
		end
		debug.setupvalue(loadTimeless, idx, function(name) return loadJewelFile(name, false) end)
		return { jewelCacheOff = true }
	end
end
