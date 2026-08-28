-- Display-translation regression harness, run INSIDE the engine so it goes
-- through the real tr_display() that DrawString uses:
--
--     pob-zh.exe tests\display_colour_test.lua
--
-- Writes display_colour_test_out.txt next to this script; the last line is
-- "RESULT PASS" or "RESULT FAIL" (the engine sets the workdir to the script
-- dir, and a GUI-subsystem exe cannot report an exit code, so read the file).
--
-- What it guards (2026-08-26): the calcs-page section values are strings like
-- "^xFF9922 0^7, ^x33FF77 0^7, ^x7070FF0". The lookup's comma-list path hands
-- "0, 0, 0" back unchanged (each "0" is its own trivial hit), and tr_display
-- used to treat that as a translation -- re-attaching only the LEADING colour
-- code, so the three charge counts lost their colours. An identity "hit"
-- must leave the original text, every colour code included.

local out = io.open("display_colour_test_out.txt", "w")
local checks, failures = 0, 0
local function check(name, ok, detail)
	checks = checks + 1
	if not ok then failures = failures + 1 end
	out:write((ok and "PASS " or "FAIL ") .. name .. (detail and ("  (" .. detail .. ")") or "") .. "\n")
end

local D = PobToolsTranslateDisplay
check("PobToolsTranslateDisplay exists", type(D) == "function")
if type(D) ~= "function" then
	out:write("RESULT FAIL\n")
	out:close()
	Exit()
	return
end

-- Same contract the Tooltip patch relies on: nil when nothing matched,
-- otherwise the translated text. Identity must therefore come back as nil.
local function unchanged(s)
	local r = D(s)
	return r == nil or r == s, tostring(r)
end

-- 1. The calcs-page value strings. Every colour code must survive.
local charges = "^7^xFF9922" .. "0^7, ^x33FF77" .. "0^7, ^x7070FF" .. "0"
check("charges '0, 0, 0' keeps all three colours", unchanged(charges))
local recoup = "^7^xFF5555" .. "0^7, ^x7070FF" .. "0^7, ^x88FFFF" .. "0"
check("recoup '0, 0, 0' keeps all three colours", unchanged(recoup))
check("'0 (0)' rage value untouched", unchanged("^7^xFF9922" .. "0 ^7(0)"))
check("bare '0, 0, 0' is not a translation", unchanged("0, 0, 0"))
check("single '0' is not a translation", unchanged("0"))

-- 2. Positive controls: real translations still happen, and a real
--    translation of coloured text still keeps its leading colour.
local r = D("Charges:")
check("'Charges:' translates", r ~= nil and r ~= "Charges:", tostring(r))
r = D("^7Charges:")
check("'^7Charges:' translates and keeps the leading colour",
      r ~= nil and r:sub(1, 2) == "^7" and r ~= "^7Charges:", tostring(r))
r = D("^x7070FFKinetic Blast ^720/20")
check("gem row: coloured, translated, still starts with its colour",
      r == nil or r:sub(1, 8) == "^x7070FF", tostring(r))

-- 3. Mid-string colour codes must survive translation (2026-08-28).
--    Every case below used to come back with EVERY escape gone: lookup step 3.4
--    stripped the codes before translating, so the only escape tr_display could
--    re-attach was the leading one -- and a line that starts with text (the
--    whole skill list) kept none at all. Field report from a zh-rCN user: "the
--    translated text takes the colour of whatever came before it, and setting a
--    colour has no effect".
local function codeCount(s)
	if type(s) ~= "string" then return -1 end
	-- "^xRRGGBB" and "^N" are counted separately; "^x..." can never match "^%d".
	return select(2, s:gsub("%^x%x%x%x%x%x%x", "")) + select(2, s:gsub("%^%d", ""))
end

-- Asserts the result carries at least as many escapes as the input, and that
-- each `mustContain` substring is present. Untranslated (nil) counts as keeping
-- the original, since DrawString then renders the input verbatim.
local function keepsCodes(name, input, mustContain)
	local res = D(input)
	local got = res or input
	local ok = codeCount(got) >= codeCount(input)
	if ok and mustContain then
		for _, needle in ipairs(mustContain) do
			if not got:find(needle, 1, true) then ok = false break end
		end
	end
	check(name, ok, tostring(res))
	return got
end

-- Side panel value, Build.lua:1759 -> "<colour>111,337^x808080 (Guard)". The
-- grey suffix used to inherit the value's colour, which is why the fire and
-- lightning rows went entirely orange/yellow in the field screenshots.
keepsCodes("sidebar suffix keeps its grey", "111,337^x808080 (Guard)", { "^x808080" })

-- Skill list row, SkillListControl.lua:77-110. Five escapes, none of them
-- leading, so the old path kept exactly zero and the row rendered all white.
keepsCodes("skill list row keeps all five escapes",
	"Storm Burst of Repulsion^xE0B0FF (Active) ^7^x8888FFB^7-^x8888FFB",
	{ "^xE0B0FF", "^x8888FF" })

keepsCodes("disabled group keeps its grey",
	"^x7F7F7FSummon Shaper Memory (Disabled)", { "^x7F7F7F" })

-- FullDPS is POB's own feature name and is whitelisted to stay English; the
-- segment must still be covered, or the whole line falls back to the colourless
-- path.
keepsCodes("FullDPS tag keeps its colour and stays English",
	"Hatred^xFFFF66 (FullDPS)", { "^xFFFF66", "(FullDPS)" })

keepsCodes("comma-joined list keeps its colour",
	"^x8888FFArcanist Brand, Punishment, Enfeeble", { "^x8888FF" })

-- Glossary path: the escape is the only thing in front of the term. Both hex
-- spellings are checked because the glossary's coverage rule counts ASCII
-- letters, and "B97452" contains some while "808080" does not.
keepsCodes("glossary term keeps its colour (numeric hex)", "^x808080Total Mana", { "^x808080" })
keepsCodes("glossary term keeps its colour (alpha hex)", "^xB97452Total Mana", { "^xB97452" })

-- Gem letters are whitelisted and the separators carry no letters, so nothing
-- translates and the line must come back untouched rather than half-eaten.
check("gem colour string is left alone", unchanged("^7^x8888FFB^7-^xFF6666R"))
check("overcap value is left alone", unchanged("75^x808080 (+12%)"))

-- 3b. EditControl's unfocused view: ConfigTab.lua:85 builds it per line as
--     colour..line.."\n", so the segment core carries a trailing newline that
--     no dictionary key has. The custom-mods box translated only while focused
--     (zh-rCN field report, 2026-08-28) until newlines counted as trim space.
r = D("^x8888FFWhile a Unique Enemy is in your Presence, 20% increased Attack Speed\n")
check("unfocused custom-mod line translates, keeps colour and newline",
      r ~= nil and r:find("^x8888FF", 1, true) == 1 and r:sub(-1) == "\n"
      and r:find("While a Unique Enemy", 1, true) == nil, tostring(r))

-- Mod Browser lines: composed "<presence prefix>, <stat>", translated via the
-- comma path now that the prefix is a dictionary key.
r = D("While a Pinnacle Atlas Boss is in your Presence, 27% increased Lightning Damage")
check("Mod Browser presence line translates",
      r ~= nil and r:find("Pinnacle", 1, true) == nil, tostring(r))

-- 3c. A literal caret that is NOT a colour escape must not recurse. The Notes
--     tab's description contains "(^)"; the segment path used to re-enter
--     lookup with the identical string and blow the stack (0xC00000FD the
--     moment the tab rendered, field-reported 2026-08-28). Reaching the next
--     check at all is the real assertion here.
check("bare caret string does not crash",
      (function() local _ = D("Using the caret symbol (^) followed by a Hex code") return true end)())
check("coloured line with a literal caret does not crash",
      (function() local _ = D("^7Using the caret symbol (^) followed by a number (0-9) will set the color.") return true end)())

-- 3d. Whole multi-line label blocks translate line by line. The skills tab's
--     "Usage Tips:" block is one [[...]] literal drawn in a single DrawString;
--     the dictionary keys are per line (zh-rCN's without the "- " bullet).
local tips = "\n^7Usage Tips:\n"
	.. "- You can copy/paste socket groups using Ctrl+C and Ctrl+V.\n"
	.. "- Ctrl + Click to enable/disable socket groups.\n"
	.. "- Ctrl + Right click to include/exclude in FullDPS calculations.\n"
	.. "- Right click to set as the Main skill group.\n"
r = D(tips)
check("usage tips block translates line by line, keeps ^7 and line count",
      r ~= nil and r:find("^7", 1, true) ~= nil
      and select(2, r:gsub("\n", "")) == select(2, tips:gsub("\n", ""))
      and r:find("Usage Tips", 1, true) == nil, tostring(r))

-- 3e. Multi-line blocks translate PER LINE, independently. The custom-mods
--     box mixes recognised mods with the player's own notes; the whole-block
--     all-or-nothing rule made one untranslatable note ("grafts--") drag every
--     recognised mod back to English while focused (drawn per line) showed
--     them in Chinese. Field report 2026-08-28: "未聚焦顯示英文,聚焦顯示中文".
local mixed = "^xF05050grafts--\n"
	.. "^x8888FFMaximum Life\n"
	.. "^xF05050far shot/fledging average (far shot 15%, fledgling 25%)\n"
	.. "^x8888FFQuality: +20%\n"
r = D(mixed)
check("mixed custom-mods block: recognised lines translate",
      r ~= nil and r:find("Maximum Life", 1, true) == nil
      and r:find("^x8888FF", 1, true) ~= nil, tostring(r))
check("mixed custom-mods block: the player's own notes stay verbatim",
      r ~= nil and r:find("grafts--", 1, true) ~= nil
      and r:find("far shot/fledging average (far shot 15%, fledgling 25%)", 1, true) ~= nil
      and r:find("^xF05050", 1, true) ~= nil, tostring(r))
check("mixed custom-mods block: line count preserved",
      r ~= nil and select(2, r:gsub("\n", "")) == select(2, mixed:gsub("\n", "")), tostring(r))

-- 4. "(Unused)" is 未使用, never GGG's "[UNUSED]" placeholder affix.
--    The bracketed key exists in the dictionary, but step 3.5 used to strip the
--    brackets and hand the bare "Unused" to the glossary, which carries
--    "[UNUSED]" -> 反鍊金的 (case-insensitively). Item rows came out as
--    "無盡之衣  (反鍊金的)".
local UNUSED_ZH = "\230\156\170\228\189\191\231\148\168"        -- 未使用
local PLACEHOLDER_ZH = "\229\143\141\233\141\138\233\135\145\231\154\132" -- 反鍊金的

r = D("Tabula Rasa  ^9(Unused)")
check("item '(Unused)' translates and keeps its grey",
      r ~= nil and r:find("^9(", 1, true) ~= nil and r:find(UNUSED_ZH, 1, true) ~= nil,
      tostring(r))
check("item '(Unused)' is not the [UNUSED] placeholder affix",
      r == nil or r:find(PLACEHOLDER_ZH, 1, true) == nil, tostring(r))

r = D("Unused Items")
check("'Unused Items' is not the [UNUSED] placeholder affix",
      r == nil or r:find(PLACEHOLDER_ZH, 1, true) == nil, tostring(r))

out:write(string.format("%d checks, %d failed\nRESULT %s\n", checks, failures, failures > 0 and "FAIL" or "PASS"))
out:close()
Exit()
