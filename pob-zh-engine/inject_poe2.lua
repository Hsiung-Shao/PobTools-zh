-- inject_poe2.lua
--
-- PoE2-only rules for poecharm_inject.lua (使用者裁定 2026-10-29/30: PoE1 and
-- PoE2 each run their own rules). Loaded by the entry script only when the
-- engine says the game is poe2 (PobToolsGame); PoE1 never runs this file.
--
-- Called with the entry's helpers and returns the rules table the entry asks:
--   headedRow(label)          -> parts of a "name   ^8[line] (word)" row, or nil
--   headedRowZh(parts, v, id) -> that row in Chinese, or nil
--   treeBackdrop              -> { zoomLevelMax, draw(self, tree, bg, size, vp) }
--   itemParsed(item)          -> after Item:ParseRaw; fixes up what it left out

local ctx = ...
local statIdsOf, zhByIdOnly, zhRaw, zhStatRow = ctx.statIdsOf, ctx.zhByIdOnly, ctx.zhRaw, ctx.zhStatRow

local rules = {}

-- PoE2 第 4 步第 4 輪(契約 v5/v5.1,使用者裁定 2026-10-29):PoE2 的「詞綴名   ^8[行]尾巴」列逐段以 id 翻。
-- "<head><spaces>[^N][<body>]( (<word>))*" -> parts, nil for any other shape.
-- head may be empty (the desecrated rows without an affix name).
function rules.headedRow(label)
	local head, gap, colour, body, tail = label:match("^([^%[]-)( +)(%^%d)%[(.*)%](.*)$")
	if not head then
		head, gap, body, tail = label:match("^([^%[]-)( +)%[(.*)%](.*)$")
		colour = ""
	end
	if not head or body == "" or head:find("%s$") then return nil end
	local words, rest = {}, tail
	while rest ~= "" do
		local w, r = rest:match("^ %(([^()]*)%)(.*)$")
		if not w then return nil end
		words[#words + 1] = w
		rest = r
	end
	return { head = head, gap = gap, colour = colour, body = body, words = words }
end

-- One headed row in Chinese, or nil when any piece has no answer (no half-English row).
function rules.headedRowZh(p, v, modId)
	local ids = modId and statIdsOf("mod", modId) or nil
	if not ids then return nil end
	local hz = ""
	if p.head ~= "" then
		if type(v.essence) == "table" or type(v.emotion) == "table" then
			hz = zhRaw(p.head)
		else
			hz = zhByIdOnly("affixes", modId, p.head)
		end
		if type(hz) ~= "string" or hz == "" or hz == p.head then return nil end
	end
	local bz = zhStatRow(p.body, "item", ids)
	if not bz then return nil end
	local out = { hz, p.gap, p.colour, "[", bz, "]" }
	for _, w in ipairs(p.words) do
		local wz = w
		if w ~= "" then
			wz = zhRaw(w)
			if type(wz) ~= "string" or wz == "" or wz == w then return nil end
		end
		out[#out + 1] = " (" .. wz .. ")"
	end
	return table.concat(out)
end

-- "Title\0Base" -> the "Radius:" label of that unique in data.uniques; false
-- when its text names two different radii. Built on first use.
local uniqueRadius
local function uniqueJewelRadiusLabel(title, base)
	if not uniqueRadius then
		uniqueRadius = {}
		for _, list in pairs(data and data.uniques or {}) do
			for _, raw in pairs(list) do
				if type(raw) == "string" then
					local t, b = raw:match("^%s*([^\r\n]-)%s*\r?\n%s*([^\r\n]-)%s*\r?\n")
					local label
					for l in (raw .. "\n"):gmatch("\nRadius: ([%a ]-)%s*\r?\n") do
						label = (label == nil or label == l) and l or false
					end
					if t and b and label ~= nil then uniqueRadius[t .. "\0" .. b] = label end
				end
			end
		end
	end
	return uniqueRadius[title .. "\0" .. base]
end

-- A unique jewel written without its "Radius:" line (poe.ninja's PoE2 export
-- drops it) keeps jewelRadiusIndex nil; socketing From Nothing then crashed
-- PassiveSpec:NodesInIntuitiveLeapLikeRadius (pairs on nodesInRadius[nil];
-- upstream master and dev). Take the radius from the unique's own data text
-- (main.uniqueDB fills in a coroutine, too late for a build reopened at
-- start-up), unless it is "Variable" there.
function rules.itemParsed(item)
	if item.type ~= "Jewel" or item.jewelRadiusIndex or item.rarity ~= "UNIQUE"
		or not item.title or not item.baseName then
		return
	end
	local label = uniqueJewelRadiusLabel(item.title, item.baseName)
	if not label or label == "Variable" then return end
	for index, info in ipairs(data.jewelRadius or {}) do
		if info.label == label then
			item.jewelRadiusIndex = index
			item.jewelRadiusLabel = label
			return
		end
	end
end

-- The tree's backdrop: POB2 draws tree:GetAssetByName("Background2") as a
-- static tile (PassiveTreeView.lua ~576) and clamps Zoom() at 20.
rules.treeBackdrop = {
	zoomLevelMax = 20,
	draw = function(self, tree, bg, size, vp)
		DrawImage(bg.handle, vp.x, vp.y, vp.width, vp.height, 0, 0, vp.width / 100, vp.height / 100)
	end,
}

return rules
