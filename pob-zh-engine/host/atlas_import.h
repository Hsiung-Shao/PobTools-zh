// In-app importer for a new season's atlas tree: converts a GGG
// atlastree-export data.json (https://github.com/grindinggear/atlastree-export,
// cloned or "Download ZIP" + extracted) into Data/atlas_tree_poe1.json and
// copies the referenced sprite sheets into Data/atlas/.
//
// C++ port of tools/gen_atlas_tree.py — keep the two in sync when the GGG
// schema or the condensed schema changes.
#pragma once

#include <string>

// dataJsonPath points at the export's data.json; the sprite sheets are read
// from the sibling "assets" directory. The condensed tree JSON and its sprite
// sheets are written into destDir (trailing backslash, e.g. a season folder
// Data/atlas_versions/<tag>/); the folder + its atlas/ subdir are created as
// needed and only overwritten after the conversion fully validates. On success
// *summary gets a short UTF-8 stat line; on failure *err gets a UTF-8 reason.
bool ImportAtlasTreeData(const std::wstring& dataJsonPath, const std::wstring& destDir,
                         std::string* err, std::string* summary);

// "pob-zh.exe --atlas-import <data.json>": headless import into the active
// season folder. Prints the result to an attached console; 0 on success.
int RunAtlasImportCli(const std::wstring& dataJsonPath, const std::wstring& exeDir);
