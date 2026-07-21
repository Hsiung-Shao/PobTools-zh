-- Translation regression harness: run with `pob-zh.exe tests\translate_test.lua`.
-- Writes results next to this script (engine sets workdir to the script dir).

local samples = {
	-- POB-appended parenthetical note used to defeat the pattern match
	"Recover 8% of Life when you use a Mana Flask (Not Supported in PoB yet)",
	-- inner line alone (dictionary has "Recover {0}% of Life when you use a Mana Flask")
	"Recover 8% of Life when you use a Mana Flask",
	-- ascendancy flavour text: must stay fully English, not word-soup
	"Champion that which you love. He who fights for nothing, dies for nothing.",
	"No judge. No jury. Just the Executioner.",
	-- composite stat strings: glossary SHOULD translate these (all letters covered)
	"+5 Total Mana (+10.0%)",
	"-22,609.4 Total Wisp DPS (-1.2%)",
	-- requirements line from the item tooltip
	"Requires Level 69, 154 Int",
	-- simple sanity checks
	"Quality: +20%",
	"Maximum Life",
	-- colour codes glued onto words (config questions, gem rows)
	"Are you always on Full ^xE05030Life?",
	"^x7070FFKinetic Blast of Clustering ^720/20",
	"^7^x7070FFFrostblink of Wintry Blast ^720/0",
	-- comma-joined runtime lists (socket groups, buff summaries)
	"^74 Power Charges, 4 Endurance Charges, Onslaught",
	"4 Power Charges, 4 Endurance Charges, Onslaught",
	"Arcane Cloak, Automation, Sigil of Power B-B-B-R",
	-- full aura list (Spectral Tiger supplied via GUI.csv)
	"^7Arcane Cloak, Arcane Surge, Discipline, Herald of Purity, Precision, Sigil of Power, Spectral Tiger, Vaal Clarity",
	"Kinetic Blast of Clustering (Active) B-G-G-B-B-B",
	-- calc breakdown notes (comma-split + pattern entries from GUI.csv)
	"Buff, Parent Condition",
	"Lightning Min",
	"7% per Spectral Tiger Count, Buff",
	"25% per Spectral Tiger Count, Buff",
	"+141 per Sigil Of Power Stage, Buff",
	"+100% of Arcane Cloak Consumed Mana, Guard",
	"Multiplier: Sigil Of Power Max Stages",
	"Multiplier: Arcane Cloak Consumed Mana",
	"You can copy/paste socket groups using Ctrl+C and Ctrl+V.",
}

local out = io.open("translate_test_out.txt", "w")
out:write("locale test run\n================\n")
for _, s in ipairs(samples) do
	local zh = PobToolsTranslate and PobToolsTranslate(s) or nil
	out:write("IN : ", s, "\n")
	out:write("OUT: ", zh or "<nil (stays English)>", "\n\n")
end
out:close()
Exit()
