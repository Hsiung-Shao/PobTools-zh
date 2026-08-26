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

out:write(string.format("%d checks, %d failed\nRESULT %s\n", checks, failures, failures > 0 and "FAIL" or "PASS"))
out:close()
Exit()
