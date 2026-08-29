// 倉庫收益統計 — the panel's persisted settings: PobTools\warehouse_ui.json.
//
// The session id is the first credential this application has ever stored, so
// it does NOT follow the plain-text convention the other tools' state files
// use: on disk it exists only as a DPAPI blob (CryptProtectData, current user),
// which a copy of the file -- a backup, a cloud sync, another machine -- cannot
// decrypt. A failed decrypt loads as "no session id saved", never as an error.
#pragma once

#include <string>
#include <vector>

struct WarehouseUiState {
	std::string accountName;
	std::string league;
	std::vector<std::string> selectedTabIds;
	int autoMinutes = 0;     // auto-snapshot interval; 0 = off, else clamped >= 5
	bool showDivine = true; // totals shown in divine by default (user request)

	// Memory only. Load fills it from the DPAPI blob when this machine+user can
	// decrypt it; Save writes the blob only when non-empty.
	std::string sessid;

	bool Load(const std::wstring& exeDir);
	bool Save(const std::wstring& exeDir) const;
};

// Exposed for the self-test: round-trips arbitrary bytes through the same DPAPI
// + base64 path Save/Load use. Encrypt returns "" on failure; Decrypt returns
// "" for anything this user cannot open.
std::string WarehouseProtectSecret(const std::string& plain);
std::string WarehouseUnprotectSecret(const std::string& blobB64);
