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

// The light sandbox for --mcp: over a full tool run POB wrote only files at
// its top level, Update\ and Builds\, so every other top-level folder is
// junctioned to the user's install and only the top-level files and Builds\
// are copied (about 1 MB instead of a 70 MB copy). One known exception goes
// through the Data junction: the timeless-jewel .bin cache, which the MCP
// host turns off (mcp_tools.lua M.mcp_setup). The writes left in POB's code
// are dev-only or UI-only (tree download, mod cache, trade query).
void LinkTree(const std::wstring& src, const std::wstring& dst, CopyStats& st);

} // namespace PobSandbox
