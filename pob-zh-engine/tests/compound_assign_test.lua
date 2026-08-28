-- LuaJIT compound-assignment smoke test: POB beta (2026-08) started using
-- "count += 1" (Modules/Main.lua), which vanilla LuaJIT before 2026-07 rejects
-- at COMPILE time -- Main.lua then never loads, common.classes never exists,
-- and the whole translation injection reports "整個沒有啟動".
--
-- Run: pob-zh.exe tests\compound_assign_test.lua
-- Writes compound_assign_test_out.txt next to this script (GUI subsystem has
-- no usable exit code); last line is "RESULT PASS" / "RESULT FAIL".
local out = io.open("compound_assign_test_out.txt", "w")

-- The operator is a syntax extension, so it must be COMPILED inside loadstring:
-- writing it inline would make this whole file unparseable on an old runtime,
-- and the failure would look like a broken test instead of a missing feature.
local checks = {
	{ "x += n",  "local x = 1 x += 2 return x",      3 },
	{ "x -= n",  "local x = 5 x -= 2 return x",      3 },
	{ "x *= n",  "local x = 3 x *= 2 return x",      6 },
	{ "x /= n",  "local x = 8 x /= 2 return x",      4 },
	{ "s ..= t", 'local s = "a" s ..= "b" return s', "ab" },
}
local failures = 0
for _, c in ipairs(checks) do
	local name, src, want = c[1], c[2], c[3]
	local f, err = loadstring(src)
	if not f then
		failures = failures + 1
		out:write("FAIL ", name, "  (does not compile: ", tostring(err), ")\n")
	else
		local ok, got = pcall(f)
		if ok and got == want then
			out:write("PASS ", name, "\n")
		else
			failures = failures + 1
			out:write("FAIL ", name, "  (got ", tostring(got), ", want ", tostring(want), ")\n")
		end
	end
end

out:write(string.format("%d checks, %d failed\nRESULT %s\n",
	#checks, failures, failures > 0 and "FAIL" or "PASS"))
out:close()
Exit()
