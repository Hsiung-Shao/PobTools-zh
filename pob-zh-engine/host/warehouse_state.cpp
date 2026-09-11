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
		autoMinutes = doc.value("autoMinutes", 0);
		if (autoMinutes != 0 && autoMinutes < 5) autoMinutes = 5;
		// The toggle is gone from the UI: values auto-convert (>= 1 divine shows
		// in d, the rest in c), so this stays permanently on.
		showDivine = true;
		sessid = WarehouseUnprotectSecret(doc.value("sessidDpapi", std::string()));
	} catch (const std::exception&) {
		*this = WarehouseUiState{};
		return false;
	}
	return true;
}

bool WarehouseUiState::Save(const std::wstring& exeDir) const
{
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);

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
	// The plain session id is deliberately not representable in this file.
	doc["sessidDpapi"] = sessid.empty() ? std::string() : WarehouseProtectSecret(sessid);
	return WriteAtomic(StatePath(exeDir), doc.dump(1, '\t'));
}
