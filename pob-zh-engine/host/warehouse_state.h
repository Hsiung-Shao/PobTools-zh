// 倉庫收益統計 — the panel's persisted settings: PobTools\warehouse_ui.json.
//
// The session id is a credential, so on disk it exists only as a DPAPI blob (CryptProtectData,
// current user); a copied file cannot decrypt it. A failed decrypt loads as "no session id saved", never an error.
#pragma once

#include <string>
#include <vector>

// What the player picked for one game (per game: PoE1 league/tab ids mean nothing on PoE2).
struct WarehouseGameSel {
	std::string league;
	std::vector<std::string> tabIds;
};

struct WarehouseUiState {
	std::string accountName;
	std::string game;        // "poe1" / "poe2"; empty until chosen (the panel then
	                         // follows the launcher's game)
	WarehouseGameSel poe1, poe2;
	WarehouseGameSel& sel() { return game == "poe2" ? poe2 : poe1; }
	const WarehouseGameSel& sel() const { return game == "poe2" ? poe2 : poe1; }
	int autoMinutes = 0;     // auto-snapshot interval: 0 = off, else 60..360 (whole hours)
	bool showDivine = true;

	// Whether the session id may touch the disk at all. Default OFF (opt-in). A file written before
	// this setting existed carries a blob and no flag -- that reads as ON, so an upgrade keeps what was saved.
	bool rememberSessid = false;

	// Memory only. Load fills it from the DPAPI blob when this machine+user can decrypt it;
	// Save(writeSecret=true) writes the blob only when rememberSessid and non-empty, otherwise an
	// EMPTY field (erasing an earlier blob).
	std::string sessid;

	bool Load(const std::wstring& exeDir);

	// writeSecret: whether THIS save is about the credential. Default false is the safe one: several
	// panels may be open at once, each with its own stale copy, and a save rewrites the whole file, so
	// an ordinary save carries the file's OWN sessidDpapi/rememberSessid forward untouched. Only the
	// three places that mean it (typing in the field, the 記住 box, the 清除 button) pass true.
	bool Save(const std::wstring& exeDir, bool writeSecret = false) const;
};

// Overwrites a string's bytes, then clears it (wherever a session id is dropped). Best effort:
// earlier copies and reallocated buffers are beyond reach.
void WarehouseWipe(std::string& s);

// The PoE1 league the revenue panel is set to, read from the settings file without touching the
// DPAPI blob (cheap to poll). "" when nothing is saved yet.
std::string WarehouseSavedLeague(const std::wstring& exeDir);

// The auto-snapshot interval as it may be saved: off, or 1..6 whole hours. Anything else (including
// old sub-hour values) reads as off below an hour, else the nearest hour capped at 6, so an old file
// can never switch a burst-prone schedule back on.
int WarehouseNormalizeAutoMinutes(int minutes);

// request guard: PobTools\warehouse_guard.json
// GGG's rate limit is one pool per ACCOUNT (standalone tool, atlas planner embed, site, trade tools),
// so the politeness rules live in one shared file rather than in any panel:
//   * a snapshot starts at most once per kWarehouseSnapshotCooldownS, manual
//     or automatic, whichever panel starts it;
//   * a pause the server asked for (a 429's Retry-After, an exhausted bucket's
//     penalty) holds EVERY stash request back until it has passed.
constexpr long long kWarehouseSnapshotCooldownS = 600;

struct WarehouseGuard {
	long long lastSnapshotStartUtc = 0;
	long long blockedUntilUtc = 0;
};

// Pure: the earliest moment the next snapshot may start.
long long WarehouseNextSnapshotUtc(const WarehouseGuard& g);

// Absent or corrupt -> all zero.
WarehouseGuard WarehouseGuardLoad(const std::wstring& exeDir);

// Claims the right to start a snapshot now: re-reads the file under a
// cross-process mutex and, only when the cooldown and any pause have passed,
// records nowUtc as the last start. False -> *nextUtc says when it may.
bool WarehouseClaimSnapshot(const std::wstring& exeDir, long long nowUtc, long long* nextUtc);

// Records a server-requested pause; an existing one that ends later stays.
void WarehouseNoteBlocked(const std::wstring& exeDir, long long untilUtc);

// Pure: when the automatic snapshot is due -- one interval after the later of
// the moment it was armed (the panel opened, or auto was switched on) and the
// newest snapshot since (by hand, automatic, or another panel's; one older than
// the arming does not count), and never before the guard allows. 0 = off.
long long WarehouseAutoDueUtc(long long latestSnapUtc, long long armedUtc, int minutes,
                              const WarehouseGuard& g);

// Exposed for the self-test: round-trips arbitrary bytes through the same DPAPI
// + base64 path Save/Load use. Encrypt returns "" on failure; Decrypt returns
// "" for anything this user cannot open.
std::string WarehouseProtectSecret(const std::string& plain);
std::string WarehouseUnprotectSecret(const std::string& blobB64);
