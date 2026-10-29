-- inject_poe1.lua
--
-- PoE1-only rules for poecharm_inject.lua (使用者裁定 2026-10-29/30: PoE1 and
-- PoE2 each run their own rules). Loaded by the entry script only when the
-- engine says the game is poe1 (PobToolsGame); PoE2 never runs this file.
--
-- Called with the entry's helpers and returns the rules table the entry asks
-- (see inject_poe2.lua for the full list). PoE1 has no headed drop-down rows:
-- its crafting rows are translated whole by stat id.

local rules = {}

-- The tree's backdrop: POB1 keeps it in tree.assets and pans it with the tree
-- (PassiveTreeView.lua:547-555); Zoom() clamps at 12.
rules.treeBackdrop = {
	zoomLevelMax = 12,
	draw = function(self, tree, bg, size, vp)
		local scale = math.min(vp.width, vp.height) / tree.size * self.zoom
		local bgSize = size.w * scale * 1.33 * 2.5
		DrawImage(bg.handle, vp.x, vp.y, vp.width, vp.height,
			(self.zoomX + vp.width / 2) / -bgSize, (self.zoomY + vp.height / 2) / -bgSize,
			(vp.width / 2 - self.zoomX) / bgSize, (vp.height / 2 - self.zoomY) / bgSize)
	end,
}

return rules
