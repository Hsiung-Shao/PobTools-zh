// 3.30 Threads of Creation: catalogue + placement rules.
//
// Rules come from the 3.30 reveal, pending GGPK verification. The three
// assumptions (no type in two slots, a slot holds >= 1, keystone +2 only on the
// allowed type) are listed in atlas_threads.h; change them there and here.
#include "atlas_threads.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <algorithm>

using nlohmann::ordered_json;

// Same file helper as the sibling atlas modules; kept local so this stays a leaf.
static bool thread_read_file(const std::wstring& path, std::string& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (1ll << 26)) {
		out.resize((size_t)size.QuadPart);
		DWORD read = 0;
		ok = out.empty() || (ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size());
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

static std::vector<std::string> thread_lines(const ordered_json& obj, const char* key)
{
	std::vector<std::string> out;
	auto it = obj.find(key);
	if (it == obj.end() || !it->is_array()) return out;
	for (const auto& s : *it)
		if (s.is_string()) out.push_back(s.get<std::string>());
	return out;
}

static std::string thread_str(const ordered_json& obj, const char* key)
{
	auto it = obj.find(key);
	return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string();
}

int ThreadMaxCount(const ThreadRules& rules)
{
	return kMaxThreadsPerSlot + (rules.singleTypeKeystone ? kKeystoneThreadBonus : 0);
}

int ThreadClampCount(int n, const ThreadRules& rules)
{
	return std::clamp(n, 1, ThreadMaxCount(rules));
}

bool ThreadDb::Load(const std::wstring& exeDir, std::string* err)
{
	std::string body;
	if (!thread_read_file(exeDir + L"Data\\threads_poe1.json", body)) {
		defs_.clear();
		byId_.clear();
		source_.clear();
		if (err) *err = u8"找不到 Data\\threads_poe1.json";
		return false;
	}
	return LoadFromJson(body, err);
}

bool ThreadDb::LoadFromJson(const std::string& json, std::string* err)
{
	defs_.clear();
	byId_.clear();
	source_.clear();
	try {
		ordered_json doc = ordered_json::parse(json);
		auto arr = doc.find("threads");
		if (!doc.is_object() || arr == doc.end() || !arr->is_array()) {
			if (err) *err = u8"threads_poe1.json 缺少 threads 陣列";
			return false;
		}
		source_ = thread_str(doc, "source");
		for (const auto& t : *arr) {
			if (!t.is_object()) continue;
			ThreadDef d;
			d.id = thread_str(t, "id");
			if (d.id.empty() || byId_.count(d.id)) continue;
			d.en = thread_str(t, "en");
			d.zh = thread_str(t, "zh");
			d.art = thread_str(t, "art");
			d.mechanic = thread_str(t, "mechanic");
			d.descEn = thread_lines(t, "descEn");
			d.descZh = thread_lines(t, "descZh");
			byId_.emplace(d.id, (int)defs_.size());
			defs_.push_back(std::move(d));
		}
	} catch (const std::exception& e) {
		defs_.clear();
		byId_.clear();
		source_.clear();
		if (err) *err = std::string(u8"threads_poe1.json 解析失敗: ") + e.what();
		return false;
	}
	if (defs_.empty()) {
		if (err) *err = u8"threads_poe1.json 沒有任何項目";
		return false;
	}
	return true;
}

const ThreadDef* ThreadDb::ById(const std::string& id) const
{
	auto it = byId_.find(id);
	return it == byId_.end() ? nullptr : &defs_[it->second];
}

ThreadPlace ThreadDb::CanPlace(const std::vector<ThreadSlot>& cur, const std::string& id,
                               const ThreadRules& rules) const
{
	if (!ById(id)) return ThreadPlace::kUnknown;
	for (const ThreadSlot& s : cur)
		if (s.id == id) return ThreadPlace::kDuplicate;
	if (rules.singleTypeKeystone && !cur.empty()) return ThreadPlace::kKeystoneMixed;
	if ((int)cur.size() >= kMaxThreadSlots) return ThreadPlace::kFull;
	return ThreadPlace::kOk;
}

std::vector<ThreadSlot> ThreadDb::Sanitize(const std::vector<ThreadSlot>& slots, const ThreadRules& rules,
                                           std::string* note) const
{
	if (note) note->clear();
	// No catalogue -> no basis to judge; keep the saved slots (see ScarabDb::Sanitize).
	if (!available()) return slots;

	std::vector<ThreadSlot> out;
	int unknown = 0, dup = 0, mixed = 0, over = 0, clamped = 0;
	for (const ThreadSlot& s : slots) {
		switch (CanPlace(out, s.id, rules)) {
		case ThreadPlace::kOk: {
			const int n = ThreadClampCount(s.count, rules);
			if (n != s.count) clamped++;
			out.push_back({ s.id, n });
			break;
		}
		case ThreadPlace::kUnknown:       unknown++; break;
		case ThreadPlace::kDuplicate:     dup++;     break;
		case ThreadPlace::kKeystoneMixed: mixed++;   break;
		case ThreadPlace::kFull:          over++;    break;
		}
	}
	if (note) {
		if (unknown) *note += u8"，忽略 " + std::to_string(unknown) + u8" 個未知絲線";
		if (dup)     *note += u8"，忽略 " + std::to_string(dup) + u8" 個重複種類的絲線槽";
		if (mixed)   *note += u8"，關鍵天賦只允許一種絲線，捨棄其他 " + std::to_string(mixed) + u8" 種";
		if (over)    *note += u8"，超過 " + std::to_string(kMaxThreadSlots) + u8" 個絲線槽的 " +
		                      std::to_string(over) + u8" 個項目已捨棄";
		if (clamped) *note += u8"，" + std::to_string(clamped) + u8" 個絲線槽的數量已調整到 1～" +
		                      std::to_string(ThreadMaxCount(rules));
	}
	return out;
}
