// The new interface in the system browser (--modern-ui where WebView2 cannot
// run: Wine / CrossOver on macOS and Linux, or Windows without the Runtime).
//
// Same page, same headless POB child, same JSON Lines; only the transport
// differs. A loopback HTTP server (127.0.0.1, an ephemeral port, every URL
// under a random per-run token) serves the built page and the folders the
// WebView2 window maps as virtual hosts, streams the child's lines to the page
// as Server-Sent Events and takes the page's lines as POSTs. The URL is opened
// with ShellExecute, which Wine hands to the host system's browser
// (winebrowser -> xdg-open / open).
//
// The process lives until the page is gone: no event-stream connection for
// kGoneSeconds after one existed (or none at all within kFirstConnectSeconds),
// or the page asked to close.
#pragma once

#include <string>

struct LauncherConfig;

// True when the built page is there to serve (<exeDir>ui\index.html).
bool ModernUiBrowserAvailable(const std::wstring& exeDir);

// Blocks until the page is gone. Returns the process exit code (3 = the bridge
// gate refused this POB and the classic window was opened instead).
// `pobDirOverride` (self-test only) serves a sandbox install instead of the
// detected one.
int ShowModernUiInBrowser(const std::wstring& exeDir, const std::wstring& game,
                          const std::wstring& locale, const LauncherConfig& cfg,
                          const std::wstring& openBuild = std::wstring(),
                          const std::wstring& pobDirOverride = std::wstring());
