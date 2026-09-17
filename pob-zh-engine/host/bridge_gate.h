// The new interface's compatibility gate, remembered between runs.
//
// bridge.lua probes POB's classes and functions on every boot and reports
// `gate_result`. When it fails the new-interface window falls back to the
// classic POB window and writes PobTools\bridge_gate.json, so the launcher
// can grey its "new interface" button and say why. The verdict is bound to
// the POB version it was taken against and to the bridge.lua that took it:
// once POB updates or a new bridge arrives (app or data line), the button
// comes back and the next open takes a fresh verdict.
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
	std::string bridgeHash;            // BridgeFingerprint() of the bridge that judged
	bool present = false;              // a file was read
};

std::wstring GatePath(const std::wstring& exeDir);
bool Write(const std::wstring& exeDir, const Verdict& v);
// Missing or unreadable file = a Verdict with present=false (and ok=true).
Verdict Read(const std::wstring& exeDir);
// Content fingerprint of <exeDir>Data\bridge\bridge.lua (FNV-1a 64, hex);
// empty when the file cannot be read.
std::string BridgeFingerprint(const std::wstring& exeDir);
// True when the remembered verdict says "not compatible" for this very POB
// with this very bridge (same install, same version, same bridge.lua);
// anything else -- including a verdict that names no bridge -- lets the
// button through.
bool BlocksModernUi(const Verdict& v, const std::wstring& pobDir, const std::string& pobVersion, const std::string& bridgeHash);
// Pure parser for the self-test (and Read): JSON text -> Verdict.
Verdict Parse(const std::string& jsonText);

} // namespace BridgeGate
