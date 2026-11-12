#include "item_library.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "win_util.h"

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)
#include <algorithm>

using nlohmann::ordered_json;

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
