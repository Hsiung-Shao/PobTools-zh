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
#include <vector>

struct RegexEntryDef {
	std::string id;                  // GGPK modifier id or English base name
	int  group = 0;                  // index into RegexPageDef::groups
	bool t17 = false;                // only rolls on tier-17 maps
	std::string affixZh;             // the affix names that print this line
	std::vector<std::string> zh;     // printed lines, '#' where a number goes
	std::vector<std::string> en;
};

struct RegexPageDef {
	std::string id;
	std::string title;
	// "poe1" / "poe2". A page belongs to the game whose file it came from, and
	// the launcher's current game only decides the ORDER: both catalogues stay
	// visible, because someone with PoE2 selected may still want to look up a
	// PoE1 map modifier, as long as the label says which game it is.
	std::string game;
	std::string note;
	int limit = 250;                 // the client's search field, in characters
	std::vector<std::string> groups;
	std::vector<RegexEntryDef> entries;
};

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

private:
	bool LoadOne(const std::wstring& exeDir, const std::wstring& game, std::string* err);

	std::vector<RegexPageDef> pages_;
	std::string source_;
};
