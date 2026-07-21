#include "item_library.h"

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

void ItemLibrary::Load(const std::wstring& exeDir, const FilterI18n& i18n)
{
	items_.clear();

	// Complete name set (bases + gems + uniques); fall back to the icon index.
	std::string content;
	if (!read_file_utf8(exeDir + L"Data\\filter_items_zh.json", content) &&
	    !read_file_utf8(exeDir + L"Data\\icon_paths.json", content))
		return;

	ordered_json doc;
	try { doc = ordered_json::parse(content); } catch (...) { return; }
	if (!doc.is_object()) return;

	items_.reserve(doc.size());
	for (auto& [name, _] : doc.items()) {
		LibItem it;
		it.en = name;
		it.zh = i18n.DisplayName(name);   // falls back to en
		it.enClass = i18n.ItemClass(name); // repoe class (complete; "" if unknown)
		it.tags = i18n.Tags(name);         // repoe tags (for sub-categorisation)
		items_.push_back(std::move(it));
	}

	std::sort(items_.begin(), items_.end(),
		[](const LibItem& a, const LibItem& b) { return a.en < b.en; });
}
