// "Open in PoB" links: the pob:// (PoE1) and pob2:// (PoE2) URL protocols that
// poe.ninja, pobb.in and the other build sites hand to the browser.
//
// Registration is opt-in (settings page) and per user: HKCU\Software\Classes
// only, so it needs no elevation and never touches HKLM. Unregistering removes
// only a scheme whose command points at THIS exe -- an official POB install (or
// another PobTools folder) that owns the scheme is left alone.
//
// A URI arrives from a web page through the browser and is therefore untrusted
// input. ParsePobUri is a strict whitelist; nothing else may reach the engine
// or a command line.
#pragma once

#include <string>

namespace PobProtocol {

// What HKCU says about one scheme (or, for QueryPobProtocol, both).
enum class State {
	None,  // not registered in HKCU
	Ours,  // registered, command runs this exe
	Other, // registered to some other program (official POB, another folder)
};

// Cheap prefix test (case-insensitive "pob:" / "pob2:") used to route argv[1]
// to the link handler; ParsePobUri decides whether it is acceptable.
bool LooksLikePobUri(const std::wstring& arg);

// Accepts only `(pob|pob2):/*<siteId>/<id>`, case-insensitive scheme, where
//   siteId = [A-Za-z0-9_-]+
//   id     = 1..8 path segments of [A-Za-z0-9_-]+ joined by '/', then either an
//            optional single trailing '/', or a query "?k=v(&k=v)*" with keys
//            [A-Za-z0-9_-]+ and values of [A-Za-z0-9_.~+-] or %XX escapes
// and a total length <= 512. (poe.ninja's real buttons are
// pob://poeninja/overview/code?account=..&name=..&overview=..&type=..; pobb.in
// sends pob://pobbin/<id>.) No quotes, spaces, backslashes, '#', dot segments
// or non-ASCII can pass. On success `game` is "poe1" (pob) or "poe2" (pob2) and
// `normalized` is "<scheme>://<siteid>/<id>" with the scheme and site id
// lowercased (POB matches `siteInfo.id:lower()` case-sensitively) and no
// trailing slash. Either out-pointer may be null.
bool ParsePobUri(const std::wstring& uri, std::wstring* game, std::wstring* normalized = nullptr);

// The raw command line (GetCommandLineW) of a link start must be exactly
// <program> "<arg1>" (or <program> <arg1>). CommandLineToArgvW silently drops
// quotes, so a link carrying '"' (pob://x/y?n=a"b) arrives as a harmless-looking
// argv[1] "pob://x/y?n=ab"; checking the raw text refuses it instead.
bool PobUriCommandLineIsClean(const std::wstring& rawCommandLine, const std::wstring& arg1);

// Ours only when BOTH pob and pob2 run `exePath`; Other when either belongs
// to another program; None otherwise (nothing, or only one of ours).
State QueryPobProtocol(const std::wstring& exePath);

// Writes both schemes: default "URL:Path of Building", "URL Protocol"="",
// DefaultIcon, shell\open\command = "<exe>" "%1". Then SHChangeNotify.
// `err` (optional) receives a reason on failure.
bool RegisterPobProtocol(const std::wstring& exePath, std::wstring* err = nullptr);

// Deletes each scheme whose command runs `exePath`; others are untouched.
// True when afterwards no scheme points at this exe.
bool UnregisterPobProtocol(const std::wstring& exePath, std::wstring* err = nullptr);

// ---- a POB of the same game is already open (phase 2) ----------------------

// Window props, shared with the engine (ui_main.cpp) and the new interface
// window (modern_ui_window.cpp).
inline constexpr wchar_t kPropExitPending[]   = L"PobTools.ExitPending";
inline constexpr wchar_t kPropExitCancelled[] = L"PobTools.ExitCancelled";
inline constexpr wchar_t kPropGame[]          = L"PobTools.Game"; // 1 = poe1, 2 = poe2
// COPYDATASTRUCT.dwData of a link handed to a running new-interface window.
inline constexpr unsigned long kLinkCopyDataMagic = 0x50424C4Bul; // 'PBLK'

// "poe1" / "poe2" from a classic POB window title ("<build> - Path of Building"
// / "... - Path of Building (PoE2)"), "" for anything else.
std::wstring GameFromPobTitle(const std::wstring& title);

// Validates a WM_COPYDATA link payload (dwData, UTF-16 bytes): the magic,
// an even size within the URI limit, and ParsePobUri. `uri` = normalized.
bool ParseLinkCopyData(unsigned long dwData, const void* data, unsigned long bytes, std::wstring* uri);

// The top-most (z-order) visible classic POB window of `game` run by `exePath`
// (GLFW class, same image path, title suffix), or null.
void* FindLastPobWindow(const std::wstring& game, const std::wstring& exePath);

// The top-most new-interface window (class PobToolsModernUi) of `game` run by
// `exePath` (prop PobTools.Game), or null.
void* FindModernUiWindow(const std::wstring& game, const std::wstring& exePath);

enum class CloseResult {
	Closed,    // the process ended (saved, not saved, or nothing to save)
	Cancelled, // the user cancelled POB's save question (or Save As)
	NoAnswer,  // neither closed nor asked within the grace period
};
// Brings `hwnd` (a classic POB) to the front, posts WM_CLOSE and waits: for
// the process to end, or for the engine's ExitPending -> ExitCancelled props.
// No time limit once ExitPending is up (the user may be typing a file name);
// `graceMs` without either answer gives NoAnswer.
CloseResult ClosePobWindowAndWait(void* hwnd, unsigned long graceMs = 5000);

// Hands a link to a running new-interface window (WM_COPYDATA with the magic).
bool SendLinkToModernUi(void* hwnd, const std::wstring& uri);

// --pob-protocol-selftest: URI whitelist cases plus a registry round-trip on a
// test-only scheme (pobtools-selftest), never the real pob / pob2. Report in
// <exeDir>PobTools\pob_protocol_selftest.txt; exit 0 = all pass.
int RunPobProtocolSelfTest(const std::wstring& exeDir);

} // namespace PobProtocol
