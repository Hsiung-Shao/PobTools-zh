// The regex tool's shipped catalogue: Data\regex_<game>.json.
//
// Everything the tool can search over is a "page" -- map modifiers, expedition
// logbook modifiers, item bases -- and a page is nothing but a list of entries
// with the text the game prints for each. So a new page is a data change, not a
// code change, and the panel below never learns what a map modifier is.
//
// The file is produced by tools/gen_regex_data.py out of the GGPK and the
// audited statdescription pairing; see that script for where each field comes
// from and what it deliberately leaves out.
#pragma once

#include <string>
#include <utility>
#include <vector>

struct RegexEntryDef {
	std::string id;                  // GGPK modifier id or English base name
	int  group = 0;                  // index into RegexPageDef::groups
	bool t17 = false;                // only rolls on tier-17 maps
	std::string affixZh;             // the affix names that print this line
	std::vector<std::string> zh;     // printed lines, '#' where a number goes
	std::vector<std::string> en;
	// Text the game's search also reads for this entry without printing it as
	// a line: the advanced description (affix name, tier, tags), the reminder
	// text, the name pieces. Only ever used to veto a token -- see regex_gen.h.
	// Optional in the file; an older catalogue simply has none.
	std::vector<std::string> hiddenZh;
	std::vector<std::string> hiddenEn;
};

// Page kind (schema 2 `kind`, exile-appraiser regex/src/data.ts:27): mods /
// names are corpus pages (the cover algorithm in regex_gen); numeric / sockets
// are algorithmic pages built in code, never read from the file. A schema-1
// file has no `kind` = mods; an unknown kind also reads as mods (data.ts:167),
// so a newer generator still yields a usable corpus page here.
enum class RegexPageKind { Mods, Names, Numeric, Sockets };

struct RegexPageDef {
	std::string id;
	RegexPageKind kind = RegexPageKind::Mods;
	std::string title;
	std::string titleEn;             // schema 2; empty in schema 1
	// "poe1" / "poe2". A page belongs to the game whose file it came from, and
	// the launcher's current game only decides the ORDER: both catalogues stay
	// visible, because someone with PoE2 selected may still want to look up a
	// PoE1 map modifier, as long as the label says which game it is.
	std::string game;
	std::string note;
	int limit = 250;                 // the client's search field, in characters
	std::vector<std::string> groups;
	std::vector<std::string> groupsEn;   // schema 2; empty in schema 1
	std::vector<RegexEntryDef> entries;
	// Text every item of this page carries (property labels, the base name, the
	// flavour paragraph, the random words rare names are made of). A token that
	// hits it hits everything, so the generator must never emit one. Optional
	// in the file, like the entry fields above.
	std::vector<std::string> ambientZh;
	std::vector<std::string> ambientEn;
	std::vector<std::string> namePrefixZh;   // rare-name words, left half
	std::vector<std::string> nameSuffixZh;   // right half, leading space kept
	std::vector<std::string> namePrefixEn;
	std::vector<std::string> nameSuffixEn;
};

// Schema 2 top-level `labels{zh,en}`: clientstrings key -> text, normalized by
// RegexNormalizeLabel. Kept in file order (it is small; a linear lookup is fine).
struct RegexLabels {
	std::vector<std::pair<std::string, std::string>> zh;
	std::vector<std::pair<std::string, std::string>> en;
	bool present = false;            // false = schema 1 (data.ts: labels = null)
	const std::string* Find(bool zhSide, const std::string& key) const;
};

// data.ts:175 normalizeLabel: "[Id|Text]" -> Text, "[Id]" -> Id, "{0}" -> "#".
std::string RegexNormalizeLabel(const std::string& s);

// data.ts:122 + :167 pageKind: "mods"/"names"/"numeric"/"sockets"; anything else = Mods.
RegexPageKind RegexPageKindFrom(const std::string& s);

class RegexDataset {
public:
	// Loads every Data\regex_<game>.json that exists, `preferred` first. False
	// with *err set when none of them do -- the panel says so rather than showing
	// an empty list, because "no map modifiers" and "the data file did not load"
	// look identical from the outside.
	bool Load(const std::wstring& exeDir, const std::wstring& preferred, std::string* err);

	const std::vector<RegexPageDef>& Pages() const { return pages_; }
	const std::string& Source() const { return source_; }

	// Is there a catalogue for this game specifically? The panel needs to tell
	// "PoE2 has no list yet" apart from "nothing loaded at all"; they read the
	// same to anyone who does not already know which files ship.
	bool HasGame(const std::string& game) const;

	// The `labels` of that game's file; nullptr when the file is not loaded.
	// A schema-1 file loads with present == false.
	const RegexLabels* Labels(const std::string& game) const;

private:
	bool LoadOne(const std::wstring& exeDir, const std::wstring& game, std::string* err);

	std::vector<RegexPageDef> pages_;
	std::vector<std::pair<std::string, RegexLabels>> labels_;   // game -> labels
	std::string source_;
};
