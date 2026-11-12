// In-app importer for a new season's atlas tree: converts a GGG
// atlastree-export data.json (https://github.com/grindinggear/atlastree-export,
// cloned or "Download ZIP" + extracted) into Data/atlas_tree_poe1.json and
// copies the referenced sprite sheets into Data/atlas/.
//
// C++ port of tools/gen_atlas_tree.py — keep the two in sync when the GGG
// schema or the condensed schema changes.
#pragma once

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)
#include <string>

// Which zoom level of a sprite category the import uses, given that category's
// {"<zoom>": {filename,w,h,coords}} object; nullptr when it has no usable one.
// Everything is zoom 0.5 except the full-tree background: it is stretched over
// the tree bounds, so its resolution is free, and 3.30 ships the 0.5 level at
// 5184px (107 MB as RGBA) -- that one takes the largest level within 2560px.
// The updater calls this too, so it downloads exactly the files the import reads.
const nlohmann::ordered_json* AtlasSpriteLevel(const std::string& cat, const nlohmann::ordered_json& levels);

// dataJsonPath points at the export's data.json; the sprite sheets are read
// from the sibling "assets" directory. The condensed tree JSON and its sprite
// sheets are written into destDir (trailing backslash, e.g. a season folder
// Data/atlas_versions/<tag>/); the folder + its atlas/ subdir are created as
// needed and only overwritten after the conversion fully validates. On success
// *summary gets a short UTF-8 stat line; on failure *err gets a UTF-8 reason.
bool ImportAtlasTreeData(const std::wstring& dataJsonPath, const std::wstring& destDir,
                         std::string* err, std::string* summary);

// The options of one GGG node object (json text), condensed to the tree file's
// `[{"e":<effect id>,"stats":[...]}]` (json text). An ordinary node yields "".
// Fails -- so the import aborts instead of shipping a node with its effects
// silently missing -- when the node is flagged isMultipleChoiceVariant but has
// no usable variantEffects, or an option lacks an integer effect / stats array.
bool AtlasImportVariants(const std::string& nodeJson, std::string* outJson, std::string* err);

// "pob-zh.exe --atlas-import <data.json>": headless import into the active
// season folder. Prints the result to an attached console; 0 on success.
int RunAtlasImportCli(const std::wstring& dataJsonPath, const std::wstring& exeDir);
