// A sandbox copy of a POB install, for running the headless engine without
// touching the user's folder. POB writes Settings.xml, Update\ and Builds\
// next to itself in standalone mode, so everything is copied except the two
// big read-only data folders (TreeData, Data\TimelessJewelData), which are
// junctioned. Shared by --headless-selftest and --mcp.
#pragma once

#include <string>

namespace PobSandbox {

struct CopyStats { int files = 0; int dirs = 0; int junctions = 0; int errors = 0; };

bool IsDir(const std::wstring& p);
bool IsReparse(const std::wstring& p);
// Removes a tree. A junction is removed as a directory entry and NEVER
// descended into: descending would delete the user's TreeData through it.
void Rmtree(const std::wstring& dir);
bool MkdirP(const std::wstring& dir);
// NTFS junction (mount point): no privilege needed, unlike a symlink.
bool MakeJunction(const std::wstring& link, const std::wstring& target);

// Copies src\ into dst\ recursively. Settings.xml, imgui.ini and Update\ are
// always left out (POB regenerates them); Builds\ only when includeBuilds is
// false. Under Wine, which has no junctions, the big folders are copied.
void CopyTree(const std::wstring& src, const std::wstring& dst, CopyStats& st, bool includeBuilds = false);

} // namespace PobSandbox
