// In-app importer for a new league's character passive tree: converts a GGG
// skilltree-export data.json (https://github.com/grindinggear/skilltree-export)
// into Data/passive_tree_poe1.json and bundles the referenced sprite sheets
// (zoom 0.3835, the "-3" files) into Data/tree/.
//
// C++ port of tools/gen_passive_tree.py's --ggg mode — keep the two in sync
// when the GGG schema or the condensed schema changes. Traditional-Chinese
// names/stats are baked in via the same template dictionaries the Python
// generator uses (Data/poe1/zh-rTW/{stats,ui,passives}.json, numbers -> '#',
// "{N}" refill), so a league update keeps its translations without any Python.
//
// Sprite sheets are sourced from the user's PoB TreeData/<ver>/ when that
// league folder exists locally (no download), else from the "assets" directory
// next to data.json. Existing bundled data is only overwritten after the
// conversion fully validates (same contract as atlas_import).
#pragma once

#include <string>

// dataJsonPath points at the export's data.json; ver is the folder-style tree
// version to stamp (e.g. "3_29" — data.json itself carries no league number).
// On success *summary gets a short UTF-8 stat line; on failure *err a reason.
bool ImportPassiveTreeData(const std::wstring& dataJsonPath, const std::string& ver,
                           const std::wstring& exeDir, std::string* err, std::string* summary);

// "pob-zh.exe --pt-import <data.json> <ver>": headless import for automation
// and parity testing against the Python generator. 0 on success.
int RunPassiveImportCli(const std::wstring& dataJsonPath, const std::wstring& ver,
                        const std::wstring& exeDir);

// "pob-zh.exe --pt-import-selftest": synthetic checks of the hand-rolled zh
// template scanners (normalize / token findall / "{N}" refill) — the pieces
// ported from the Python regexes. Console + pt_import_selftest.txt; 0 on pass.
int RunPassiveImportSelfTest(const std::wstring& exeDir);
