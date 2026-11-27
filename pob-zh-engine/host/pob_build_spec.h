// A POB build file's active passive tree, for the Abyss jewel mode of the
// timeless jewel calculator (Zorath's path and the "allocated only" filter need
// the character's allocation, which a standalone tool can only get from here).
#pragma once

#include <set>
#include <string>
#include <vector>

struct PobBuildSpec {
	std::string className;        // <Build className>, e.g. "Marauder"
	std::string ascendClassName;  // <Build ascendClassName>, e.g. "Juggernaut"; "None" / "" when unset
	std::string treeVersion;      // active <Spec treeVersion>, e.g. "3_30"
	int classId = -1;             // active <Spec classId>
	std::set<int> nodes;          // active <Spec nodes>
};

// Every *.xml under `buildDir`, recursively (POB keeps sub-folders), sorted.
std::vector<std::wstring> ListPobBuilds(const std::wstring& buildDir);

// Parse build XML text. The active spec is <Tree activeSpec="n"> (1-based,
// default 1). False with *err set when there is no usable <Spec>.
bool ParsePobBuildSpec(const std::string& xml, PobBuildSpec& out, std::string* err);
bool LoadPobBuildSpec(const std::wstring& path, PobBuildSpec& out, std::string* err);

// --pob-build-selftest
int RunPobBuildSelfTest(const std::wstring& exeDir);
