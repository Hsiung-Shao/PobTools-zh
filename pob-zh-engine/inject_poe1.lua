-- inject_poe1.lua
--
-- PoE1-only rules for poecharm_inject.lua; loaded only when the engine says the game is poe1
-- (PobToolsGame). Returns the rules table the entry asks (list in inject_poe2.lua).
-- PoE1 has no headed drop-down rows: its crafting rows are translated whole by stat id.

local rules = {}

-- POB1 keeps the backdrop in tree.assets and pans it with the tree
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
