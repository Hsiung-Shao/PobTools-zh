#include "warehouse_state.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>

#include <json.hpp>

#pragma comment(lib, "crypt32.lib")

using nlohmann::ordered_json;

namespace {

std::wstring StatePath(const std::wstring& exeDir)
{
	return exeDir + L"PobTools\\warehouse_ui.json";
}

bool ReadAll(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 20)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() ||
		     (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

bool WriteAtomic(const std::wstring& dst, const std::string& body)
{
	const std::wstring tmp = dst + L".tmp";
	HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD wrote = 0;
	const bool ok = WriteFile(f, body.data(), (DWORD)body.size(), &wrote, nullptr) &&
	                wrote == body.size();
	CloseHandle(f);
	if (!ok) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	if (!MoveFileExW(tmp.c_str(), dst.c_str(),
	                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		DeleteFileW(tmp.c_str());
		return false;
	}
	return true;
}

// Small local base64: the blob is a few hundred bytes, and keeping the module
// self-contained beats adapting the curl-derived C helpers in engine/common.
const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string B64Encode(const unsigned char* data, size_t len)
{
	std::string out;
	out.reserve((len + 2) / 3 * 4);
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = data[i] << 16;
		if (i + 1 < len) v |= data[i + 1] << 8;
		if (i + 2 < len) v |= data[i + 2];
		out += kB64[(v >> 18) & 63];
		out += kB64[(v >> 12) & 63];
		out += i + 1 < len ? kB64[(v >> 6) & 63] : '=';
		out += i + 2 < len ? kB64[v & 63] : '=';
	}
	return out;
}

std::vector<unsigned char> B64Decode(const std::string& s)
{
	auto val = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+') return 62;
		if (c == '/') return 63;
		return -1;
	};
	std::vector<unsigned char> out;
	int acc = 0, bits = 0;
	for (char c : s) {
		if (c == '=') break;
		int v = val(c);
		if (v < 0) return {};
		acc = (acc << 6) | v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back((unsigned char)((acc >> bits) & 0xFF));
		}
	}
	return out;
}

// Fixed entropy: not a secret (it is in the binary), just a namespace so a blob
// written by another program cannot be handed to us and vice versa.
const wchar_t kEntropy[] = L"PobTools.warehouse";

} // namespace

std::string WarehouseProtectSecret(const std::string& plain)
{
	if (plain.empty()) return std::string();
	DATA_BLOB in{ (DWORD)plain.size(), (BYTE*)plain.data() };
	DATA_BLOB entropy{ sizeof(kEntropy), (BYTE*)kEntropy };
	DATA_BLOB out{};
	if (!CryptProtectData(&in, L"PobTools warehouse session", &entropy, nullptr,
	                      nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
		return std::string();
	std::string b64 = B64Encode(out.pbData, out.cbData);
	LocalFree(out.pbData);
	return b64;
}

std::string WarehouseUnprotectSecret(const std::string& blobB64)
{
	std::vector<unsigned char> raw = B64Decode(blobB64);
	if (raw.empty()) return std::string();
	DATA_BLOB in{ (DWORD)raw.size(), raw.data() };
	DATA_BLOB entropy{ sizeof(kEntropy), (BYTE*)kEntropy };
	DATA_BLOB out{};
	if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
	                        CRYPTPROTECT_UI_FORBIDDEN, &out))
		return std::string(); // another user/machine wrote it: load as "not saved"
	std::string plain((const char*)out.pbData, out.cbData);
	LocalFree(out.pbData);
	return plain;
}

void WarehouseWipe(std::string& s)
{
	if (!s.empty()) {
		// volatile so the writes survive an optimiser that sees a dead store into
		// a buffer about to be freed.
		volatile char* p = &s[0];
		for (size_t i = 0; i < s.size(); i++) p[i] = 0;
	}
	s.clear();
}

bool WarehouseUiState::Load(const std::wstring& exeDir)
{
	std::string body;
	if (!ReadAll(StatePath(exeDir), body)) return false;
	try {
		ordered_json doc = ordered_json::parse(body);
		accountName = doc.value("accountName", std::string());
		game = doc.value("game", std::string());
		if (game != "poe1" && game != "poe2") game.clear();
		auto readSel = [](const ordered_json& o, WarehouseGameSel& s) {
			s.league = o.value("league", std::string());
			s.tabIds.clear();
			auto jt = o.find("selectedTabIds");
			if (jt != o.end() && jt->is_array())
				for (const auto& t : *jt)
					if (t.is_string()) s.tabIds.push_back(t.get<std::string>());
		};
		// Files written before PoE2 support kept PoE1's picks at the top level.
		auto j1 = doc.find("poe1");
		readSel(j1 != doc.end() && j1->is_object() ? *j1 : doc, poe1);
		auto j2 = doc.find("poe2");
		if (j2 != doc.end() && j2->is_object()) readSel(*j2, poe2);
		else poe2 = WarehouseGameSel{};
		autoMinutes = WarehouseNormalizeAutoMinutes(doc.value("autoMinutes", 0));
		// The toggle is gone from the UI: values auto-convert (>= 1 divine shows
		// in d, the rest in c), so this stays permanently on.
		showDivine = true;
		const std::string blob = doc.value("sessidDpapi", std::string());
		// No flag + a blob = written before the setting existed: the player did
		// save one, so keep honouring that. No flag and no blob = off.
		auto jr = doc.find("rememberSessid");
		rememberSessid = jr != doc.end() && jr->is_boolean() ? jr->get<bool>() : !blob.empty();
		// A blob this user cannot decrypt (another machine, another account) loads
		// as "nothing saved" while the flag stays on: the next paste is kept.
		sessid = WarehouseUnprotectSecret(blob);
	} catch (const std::exception&) {
		*this = WarehouseUiState{};
		return false;
	}
	return true;
}

bool WarehouseUiState::Save(const std::wstring& exeDir, bool writeSecret) const
{
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);

	// What the credential fields will say. Ours only when this save is about
	// them; otherwise whatever the file already holds, so another panel's clear
	// survives our write (see the header).
	bool remember = rememberSessid;
	std::string blob =
	    (!rememberSessid || sessid.empty()) ? std::string() : WarehouseProtectSecret(sessid);
	if (!writeSecret) {
		std::string prevBody;
		if (ReadAll(StatePath(exeDir), prevBody)) {
			try {
				ordered_json prev = ordered_json::parse(prevBody);
				auto jb = prev.find("sessidDpapi");
				blob = jb != prev.end() && jb->is_string() ? jb->get<std::string>()
				                                          : std::string();
				auto jr = prev.find("rememberSessid");
				remember = jr != prev.end() && jr->is_boolean() ? jr->get<bool>()
				                                                : !blob.empty();
			} catch (const std::exception&) {
				// Unreadable file: this save is what rebuilds it, ours stands.
			}
		}
	}

	ordered_json doc;
	doc["schema"] = 1;
	doc["accountName"] = accountName;
	doc["game"] = game;
	auto writeSel = [](const WarehouseGameSel& s) {
		ordered_json o;
		o["league"] = s.league;
		o["selectedTabIds"] = s.tabIds;
		return o;
	};
	doc["poe1"] = writeSel(poe1);
	doc["poe2"] = writeSel(poe2);
	doc["autoMinutes"] = autoMinutes;
	doc["showDivine"] = showDivine;
	doc["rememberSessid"] = remember;
	// The plain session id is deliberately not representable in this file. An
	// empty field is also how "forget it" is carried out: a credential save with
	// the box unticked (or after the 清除 button) overwrites a blob written
	// earlier.
	doc["sessidDpapi"] = blob;
	return WriteAtomic(StatePath(exeDir), doc.dump(1, '\t'));
}

int WarehouseNormalizeAutoMinutes(int minutes)
{
	if (minutes < 60) return 0;
	if (minutes >= 360) return 360;
	return (minutes + 30) / 60 * 60;
}

namespace {

std::wstring GuardPath(const std::wstring& exeDir)
{
	return exeDir + L"PobTools\\warehouse_guard.json";
}

bool SaveGuard(const std::wstring& exeDir, const WarehouseGuard& g)
{
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	ordered_json doc;
	doc["schema"] = 1;
	doc["lastSnapshotStartUtc"] = g.lastSnapshotStartUtc;
	doc["blockedUntilUtc"] = g.blockedUntilUtc;
	return WriteAtomic(GuardPath(exeDir), doc.dump());
}

// Serializes read-check-write across every PobTools process of this logon
// session. An abandoned mutex (its holder crashed) counts as acquired: the file
// is rewritten whole, so there is no half-state to protect. A timeout proceeds
// unlocked -- the guard is courtesy towards the server, not a correctness lock.
class GuardLock {
public:
	GuardLock()
	{
		h_ = CreateMutexW(nullptr, FALSE, L"Local\\PobTools.warehouse.guard");
		if (h_) {
			const DWORD w = WaitForSingleObject(h_, 2000);
			held_ = w == WAIT_OBJECT_0 || w == WAIT_ABANDONED;
		}
	}
	~GuardLock()
	{
		if (held_) ReleaseMutex(h_);
		if (h_) CloseHandle(h_);
	}
	GuardLock(const GuardLock&) = delete;
	GuardLock& operator=(const GuardLock&) = delete;

private:
	HANDLE h_ = nullptr;
	bool held_ = false;
};

} // namespace

long long WarehouseNextSnapshotUtc(const WarehouseGuard& g)
{
	const long long cool =
	    g.lastSnapshotStartUtc > 0 ? g.lastSnapshotStartUtc + kWarehouseSnapshotCooldownS : 0;
	return cool > g.blockedUntilUtc ? cool : g.blockedUntilUtc;
}

WarehouseGuard WarehouseGuardLoad(const std::wstring& exeDir)
{
	WarehouseGuard g;
	std::string body;
	if (!ReadAll(GuardPath(exeDir), body)) return g;
	try {
		ordered_json doc = ordered_json::parse(body);
		if (!doc.is_object()) return g;
		auto whole = [&doc](const char* k) {
			auto it = doc.find(k);
			return it != doc.end() && it->is_number_integer() ? it->get<long long>() : 0ll;
		};
		g.lastSnapshotStartUtc = whole("lastSnapshotStartUtc");
		g.blockedUntilUtc = whole("blockedUntilUtc");
	} catch (const std::exception&) {
		g = WarehouseGuard{};
	}
	return g;
}

bool WarehouseClaimSnapshot(const std::wstring& exeDir, long long nowUtc, long long* nextUtc)
{
	GuardLock lock;
	WarehouseGuard g = WarehouseGuardLoad(exeDir);
	// A start stamped in the future is a clock that was set back: measure the
	// cooldown from now, not from a moment that has not happened yet.
	if (g.lastSnapshotStartUtc > nowUtc) g.lastSnapshotStartUtc = nowUtc;
	const long long next = WarehouseNextSnapshotUtc(g);
	if (nowUtc < next) {
		if (nextUtc) *nextUtc = next;
		return false;
	}
	g.lastSnapshotStartUtc = nowUtc;
	SaveGuard(exeDir, g); // best effort: a failed write must not cost the snapshot
	if (nextUtc) *nextUtc = nowUtc + kWarehouseSnapshotCooldownS;
	return true;
}

void WarehouseNoteBlocked(const std::wstring& exeDir, long long untilUtc)
{
	GuardLock lock;
	WarehouseGuard g = WarehouseGuardLoad(exeDir);
	if (untilUtc <= g.blockedUntilUtc) return;
	g.blockedUntilUtc = untilUtc;
	SaveGuard(exeDir, g);
}

long long WarehouseAutoDueUtc(long long latestSnapUtc, long long armedUtc, int minutes,
                              const WarehouseGuard& g)
{
	if (minutes <= 0) return 0;
	const long long base = latestSnapUtc > armedUtc ? latestSnapUtc : armedUtc;
	const long long due = base + (long long)minutes * 60;
	const long long allowed = WarehouseNextSnapshotUtc(g);
	return due > allowed ? due : allowed;
}

std::string WarehouseSavedLeague(const std::wstring& exeDir)
{
	std::string body;
	if (!ReadAll(StatePath(exeDir), body)) return std::string();
	try {
		ordered_json doc = ordered_json::parse(body);
		if (!doc.is_object()) return std::string();
		// Files written before PoE2 support kept PoE1's picks at the top level.
		auto j1 = doc.find("poe1");
		const ordered_json& o = (j1 != doc.end() && j1->is_object()) ? *j1 : doc;
		auto jl = o.find("league");
		return jl != o.end() && jl->is_string() ? jl->get<std::string>() : std::string();
	} catch (const std::exception&) {
		return std::string();
	}
}
