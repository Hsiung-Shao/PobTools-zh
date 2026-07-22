#include "atlas_i18n.h"
#include "atlas_version_index.h" // season resolution (versioned data layout)

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

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

bool AtlasI18n::Load(const std::wstring& exeDir)
{
	return LoadVersion(exeDir, std::string()); // active season
}

bool AtlasI18n::LoadVersion(const std::wstring& exeDir, const std::string& tag)
{
	nameById_.clear();
	statByEn_.clear();
	note_.clear();
	repoe_.clear();
	loaded_ = false;

	AtlasVersionIndex idx;
	idx.Load(exeDir);
	std::wstring dir = idx.ResolveDataDir(exeDir, tag);

	std::string content;
	if (!read_file_utf8(dir + L"atlas_tree_zh.json", content)) return false;

	try {
		ordered_json doc = ordered_json::parse(content);
		if (!doc.contains("nodes") || !doc["nodes"].is_object()) return false;

		for (const auto& [key, v] : doc["nodes"].items()) {
			if (!v.contains("zh")) continue; // English-only entry (repoe behind)
			int id = 0;
			try {
				id = std::stoi(key);
			} catch (...) {
				continue;
			}
			std::string zh = v.value("zh", std::string());
			if (!zh.empty()) nameById_.emplace(id, std::move(zh));

			// stat lines join by full English text; only aligned lists are safe
			if (v.contains("statsEn") && v.contains("statsZh") &&
				v["statsEn"].is_array() && v["statsZh"].is_array() &&
				v["statsEn"].size() == v["statsZh"].size()) {
				for (size_t i = 0; i < v["statsEn"].size(); i++) {
					if (v["statsEn"][i].is_string() && v["statsZh"][i].is_string())
						statByEn_.emplace(v["statsEn"][i].get<std::string>(),
						                  v["statsZh"][i].get<std::string>());
				}
			}
		}

		repoe_ = doc.value("repoe", std::string());
		note_ = doc.value("tag", std::string("?")) + " / repoe " + (repoe_.empty() ? "?" : repoe_);
		loaded_ = !nameById_.empty();
		return loaded_;
	} catch (...) {
		nameById_.clear();
		statByEn_.clear();
		return false;
	}
}

const std::string& AtlasI18n::NodeName(int id, const std::string& en) const
{
	auto it = nameById_.find(id);
	return it != nameById_.end() ? it->second : en;
}

const std::string& AtlasI18n::StatLine(const std::string& en) const
{
	auto it = statByEn_.find(en);
	return it != statByEn_.end() ? it->second : en;
}
