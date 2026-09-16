// The new interface's compatibility gate, remembered between runs.
//
// bridge.lua probes POB's classes and functions on every boot and reports
// `gate_result`. When it fails the new-interface window falls back to the
// classic POB window and writes PobTools\bridge_gate.json, so the launcher
// can grey its "new interface" button and say why. The verdict is bound to
// the POB version it was taken against: once POB updates, the button comes
// back and the next open takes a fresh verdict.
#pragma once
#include <string>
#include <vector>

namespace BridgeGate {

struct Verdict {
	bool ok = true;
	std::vector<std::string> failed;   // probe names, from bridge.lua
	std::string pobVersion;            // launch.versionNumber at the time
	std::string pobBranch;
	std::wstring pobDir;               // which install
	std::string checkedAtUtc;          // ISO 8601, informational
	bool present = false;              // a file was read
};

std::wstring GatePath(const std::wstring& exeDir);
bool Write(const std::wstring& exeDir, const Verdict& v);
// Missing or unreadable file = a Verdict with present=false (and ok=true).
Verdict Read(const std::wstring& exeDir);
// True when the remembered verdict says "not compatible" for this very POB
// (same install, same version); anything else lets the button through.
bool BlocksModernUi(const Verdict& v, const std::wstring& pobDir, const std::string& pobVersion);
// Pure parser for the self-test (and Read): JSON text -> Verdict.
Verdict Parse(const std::string& jsonText);

} // namespace BridgeGate
