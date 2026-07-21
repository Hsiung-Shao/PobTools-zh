#include "economy_service.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)
#include <algorithm>

using nlohmann::ordered_json;

static bool read_file_utf8(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 30)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

void EconomyService::Init(const std::wstring& exeDir, const std::string& league, bool enabled)
{
	enabled_ = enabled;
	league_ = league.empty() ? std::string("Mirage") : league;
	prices_.clear();
	thresholds_.clear();

	// Static prices: { "Divine Orb": 200, ... } (chaos value).
	std::string content;
	if (read_file_utf8(exeDir + L"Data\\economy_poe1.json", content)) {
		try {
			ordered_json doc = ordered_json::parse(content);
			if (doc.is_object())
				for (auto& [name, val] : doc.items())
					if (val.is_number()) prices_.emplace(name, val.get<double>());
		} catch (...) {}
	}

	// Thresholds: { "thresholds": [200, 50, 10, 2, 0.3] } (descending chaos cut-offs).
	if (read_file_utf8(exeDir + L"Data\\economy_tiers_poe1.json", content)) {
		try {
			ordered_json doc = ordered_json::parse(content);
			if (doc.contains("thresholds") && doc["thresholds"].is_array())
				for (auto& v : doc["thresholds"])
					if (v.is_number()) thresholds_.push_back(v.get<double>());
		} catch (...) {}
	}
	std::sort(thresholds_.begin(), thresholds_.end(), std::greater<double>());
}

PriceInfo EconomyService::PriceOf(const std::string& en) const
{
	PriceInfo p;
	auto it = prices_.find(en);
	if (it != prices_.end()) { p.chaos = it->second; p.known = true; }
	return p;
}

int EconomyService::SuggestTier(const std::string& en, const TierCategory& cat, const TierListIndex& index) const
{
	if (!available() || thresholds_.empty()) return -1;
	PriceInfo p = PriceOf(en);
	if (!p.known) return -1;

	// Draggable tiers of this category, in file order (assumed high-value first).
	std::vector<int> blocks; // FilterFile::blocks indices
	for (int ti : cat.tierEntryIdx)
		if (index.tiers[ti].draggable()) blocks.push_back(index.tiers[ti].blockIndex);
	if (blocks.empty()) return -1;

	// Bucket = how many descending thresholds the value clears (0 = most valuable).
	int bucket = 0;
	for (double t : thresholds_) { if (p.chaos >= t) break; bucket++; }
	if (bucket >= (int)blocks.size()) bucket = (int)blocks.size() - 1;
	return blocks[bucket];
}
