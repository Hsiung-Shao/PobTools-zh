// Launcher UI strings as editable translation data.
//
// The 49 strings in launcher_strings.h are compiled into the exe, so a translator
// could not touch them at all -- let alone see the result. They now also ship as
// Data\launcher\<locale>\launcher.json, laid out EXACTLY like Data\poe1\zh-rTW\
// (a meta.json with a load_order plus one dictionary file). That shape is the
// whole design: the external-data-path override, the "update the app but not the
// translation data" split and the translation editor all handle it with no
// special-case code, because to each of them it is just another dictionary.
//
// The JSON is an OVERLAY, never the source of truth. Whatever it does not define
// keeps the compiled string, so a stale, partial or corrupt file can never blank
// a button -- and text added by a NEW build still renders correctly for someone
// who has turned translation-data updates off.
#pragma once

#include <deque>
#include <string>

#include "launcher_strings.h"

struct LauncherStringStore {
	LauncherStrings s{};             // fields point into the compiled table or into `owned`
	// deque, not vector: adding an element must never move the ones already
	// there, because `s` holds raw pointers into them. A vector reallocation
	// would leave every previously-overridden field dangling.
	std::deque<std::string> owned;
	int overridden = 0;              // how many fields the JSON replaced
	bool fileFound = false;

	// Copying would duplicate `owned` while `s` kept pointing at the ORIGINAL
	// strings -- fine until the original dies, then every overridden field is a
	// dangling pointer. Moving is safe (deque move preserves element addresses),
	// so the type stays returnable and storable; only the silent-copy trap is gone.
	LauncherStringStore() = default;
	LauncherStringStore(const LauncherStringStore&) = delete;
	LauncherStringStore& operator=(const LauncherStringStore&) = delete;
	LauncherStringStore(LauncherStringStore&&) = default;
	LauncherStringStore& operator=(LauncherStringStore&&) = default;
};

// Compiled defaults with <slotRoot><locale>\launcher.json layered on top.
// slotRoot ends with a backslash (<exeDir>Data\launcher\ or the folder configured
// for DictSlot::Launcher). Keys are the ENGLISH strings, like the POB dictionaries.
LauncherStringStore LoadLauncherStrings(const std::wstring& slotRoot, const std::wstring& locale);

// Maintainer CLI (--launcher-strings-export): regenerate
// <exeDir>Data\launcher\<locale>\{meta.json,launcher.json} from the compiled
// tables. Run before packaging so the shipped JSON cannot drift from the binary.
// Returns 0 on success.
int RunLauncherStringsExport(const std::wstring& exeDir);

// Headless checks (--launcher-strings-selftest): English keys pairwise unique,
// overlay/fallback semantics, pointer lifetime across many overrides, export
// round-trip. Report at <exeDir>PobTools\launcher_strings_selftest.txt; 0 = pass.
int RunLauncherStringsSelfTest(const std::wstring& exeDir);
