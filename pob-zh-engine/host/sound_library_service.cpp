#include "sound_library_service.h"
#include "sound_manager.h"   // Get/SetSoundFolder (pob-zh.ini)
#include "audio_player.h"    // StopAudio before renames (MCI file locks)
#include "filter_parser.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)
#include <algorithm>
#include <fstream>

using nlohmann::ordered_json;

namespace {

std::wstring widen(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

bool ieq(const std::wstring& a, const std::wstring& b)
{
	return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// Split "name.ext" -> {"name", "ext"} (ext without the dot, may be empty).
void split_ext(const std::wstring& file, std::wstring& stem, std::wstring& ext)
{
	size_t dot = file.find_last_of(L'.');
	if (dot == std::wstring::npos || dot == 0) { stem = file; ext.clear(); return; }
	stem = file.substr(0, dot);
	ext = file.substr(dot + 1);
}

// Case-insensitive substring (both mapped through towlower).
bool contains_ci(const std::wstring& hay, const std::wstring& needle)
{
	if (needle.empty()) return false;
	std::wstring h = hay, n = needle;
	for (wchar_t& c : h) c = towlower(c);
	for (wchar_t& c : n) c = towlower(c);
	return h.find(n) != std::wstring::npos;
}

} // namespace

std::string SoundRulesToJson(const std::vector<NamingRule>& rules)
{
	ordered_json arr = ordered_json::array();
	for (const NamingRule& r : rules) {
		arr.push_back({
			{ "name", r.name }, { "match", r.match },
			{ "rename", r.rename }, { "enabled", r.enabled },
		});
	}
	ordered_json root = { { "version", 1 }, { "rules", arr } };
	return root.dump(2);
}

bool SoundRulesFromJson(const std::string& text, std::vector<NamingRule>* out)
{
	out->clear();
	ordered_json root = ordered_json::parse(text, nullptr, false);
	if (root.is_discarded() || !root.is_object() || !root.contains("rules") || !root["rules"].is_array())
		return false;
	for (const auto& j : root["rules"]) {
		NamingRule r;
		r.name = j.value("name", std::string());
		r.match = j.value("match", std::string());
		r.rename = j.value("rename", std::string());
		r.enabled = j.value("enabled", true);
		if (!r.name.empty() || !r.rename.empty()) out->push_back(std::move(r));
	}
	return true;
}

std::vector<int> FindCustomSoundRefs(const FilterFile& f, const std::wstring& fileName)
{
	std::vector<int> out;
	for (int i = 0; i < (int)f.lines.size(); i++) {
		const FilterLine& ln = f.lines[i];
		if (ln.kind != FilterLineKind::Action) continue;
		if (ln.keyword != "CustomAlertSound" && ln.keyword != "CustomAlertSoundOptional") continue;
		if (ln.values.empty()) continue;
		std::wstring v = widen(ln.values[0].text);
		size_t slash = v.find_last_of(L"\\/");
		std::wstring base = (slash == std::wstring::npos) ? v : v.substr(slash + 1);
		if (ieq(base, fileName)) out.push_back(i);
	}
	return out;
}

int ReplaceSoundRefs(FilterDocumentEditor* doc, const std::wstring& oldName,
                     const std::wstring& newName)
{
	if (!doc || !doc->file() || newName.empty() || ieq(oldName, newName)) return 0;
	FilterFile* f = doc->file();
	std::vector<int> refs = FindCustomSoundRefs(*f, oldName);
	std::string newU8 = narrow(newName);
	int n = 0;
	for (int li : refs) {
		if (li < 0 || li >= (int)f->lines.size()) continue;
		FilterLine& ln = f->lines[li];
		if (ln.values.empty()) continue;
		std::string v = ln.values[0].text;
		size_t slash = v.find_last_of("\\/");
		std::string prefix = (slash == std::string::npos) ? std::string() : v.substr(0, slash + 1);
		FilterSetValueStr(ln, 0, prefix + newU8, true);
		f->dirty = true;
		n++;
	}
	return n;
}

void SoundLibraryService::Init(const std::wstring& exeDir)
{
	exeDir_ = exeDir;
	folder_ = GetSoundFolder();
	rules_.clear();
	std::ifstream in(exeDir_ + L"Data\\sound_rules.json", std::ios::binary);
	if (in) {
		std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		SoundRulesFromJson(text, &rules_);   // bad json -> empty rules, no crash
	}
	Rescan();
}

void SoundLibraryService::SetFolder(const std::wstring& folder)
{
	folder_ = folder;
	SetSoundFolder(folder);
	Rescan();
}

void SoundLibraryService::Rescan()
{
	files_.clear();
	if (folder_.empty()) return;
	static const wchar_t* exts[] = { L"*.wav", L"*.mp3", L"*.ogg", L"*.flac", L"*.m4a", L"*.aac" };
	for (const wchar_t* ext : exts) {
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW((folder_ + L"\\" + ext).c_str(), &fd);
		if (h == INVALID_HANDLE_VALUE) continue;
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
			SoundFileInfo fi;
			fi.name = fd.cFileName;
			fi.size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
			files_.push_back(std::move(fi));
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	std::sort(files_.begin(), files_.end(),
		[](const SoundFileInfo& a, const SoundFileInfo& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
}

bool SoundLibraryService::SaveRules(std::string* err)
{
	std::wstring dir = exeDir_ + L"Data";
	CreateDirectoryW(dir.c_str(), nullptr);
	std::ofstream o(dir + L"\\sound_rules.json", std::ios::binary);
	if (!o) { if (err) *err = "cannot write Data\\sound_rules.json"; return false; }
	std::string text = SoundRulesToJson(rules_);
	o.write(text.data(), (std::streamsize)text.size());
	return true;
}

std::vector<RenamePlanEntry> SoundLibraryService::BuildRenamePlan(const FilterFile* openFilter) const
{
	std::vector<RenamePlanEntry> plan;
	std::vector<std::wstring> taken;   // target names claimed by earlier entries
	for (const SoundFileInfo& fi : files_) {
		const NamingRule* hit = nullptr;
		for (const NamingRule& r : rules_) {
			if (!r.enabled || r.match.empty() || r.rename.empty()) continue;
			if (contains_ci(fi.name, widen(r.match))) { hit = &r; break; }
		}
		if (!hit) continue;

		std::wstring stem, ext;
		split_ext(fi.name, stem, ext);
		std::wstring tmpl = widen(hit->rename);

		auto expand = [&](int n) {
			std::wstring t = tmpl;
			size_t p;
			while ((p = t.find(L"{ext}")) != std::wstring::npos) t.replace(p, 5, ext);
			while ((p = t.find(L"{n}")) != std::wstring::npos) t.replace(p, 3, n > 0 ? std::to_wstring(n) : L"");
			return t;
		};

		bool hasN = tmpl.find(L"{n}") != std::wstring::npos;
		auto occupied = [&](const std::wstring& name) {
			if (ieq(name, fi.name)) return false;     // renaming onto itself is fine
			for (const SoundFileInfo& other : files_)
				if (ieq(other.name, name)) return true;
			for (const std::wstring& t : taken)
				if (ieq(t, name)) return true;
			return false;
		};

		RenamePlanEntry e;
		e.oldName = fi.name;
		if (hasN) {
			int n = 1;
			while (occupied(expand(n)) && n < 1000) n++;
			e.newName = expand(n);
		} else {
			e.newName = expand(0);
		}

		if (ieq(e.newName, e.oldName)) e.state = RenamePlanEntry::State::Unchanged;
		else if (occupied(e.newName)) e.state = RenamePlanEntry::State::Conflict;
		else e.state = RenamePlanEntry::State::Rename;

		if (e.state != RenamePlanEntry::State::Unchanged) taken.push_back(e.newName);
		if (openFilter) e.refLines = FindCustomSoundRefs(*openFilter, e.oldName);
		plan.push_back(std::move(e));
	}
	return plan;
}

RenamePlanEntry SoundLibraryService::BuildSingleRename(const std::wstring& oldName,
	const std::wstring& newName, const FilterFile* openFilter) const
{
	RenamePlanEntry e;
	e.oldName = oldName;
	e.newName = newName;
	if (ieq(newName, oldName) || newName.empty()) {
		e.state = RenamePlanEntry::State::Unchanged;
	} else {
		bool exists = false;
		for (const SoundFileInfo& fi : files_)
			if (ieq(fi.name, newName)) exists = true;
		e.state = exists ? RenamePlanEntry::State::Conflict : RenamePlanEntry::State::Rename;
	}
	if (openFilter) e.refLines = FindCustomSoundRefs(*openFilter, oldName);
	return e;
}

SoundLibraryService::ApplyResult SoundLibraryService::ApplyRenamePlan(
	std::vector<RenamePlanEntry>& plan, FilterDocumentEditor* syncDoc)
{
	ApplyResult res;

	// Every conflict must carry an explicit decision before anything runs.
	for (const RenamePlanEntry& e : plan)
		if (e.state == RenamePlanEntry::State::Conflict &&
		    e.resolution == RenamePlanEntry::Resolution::Unset) {
			res.err = u8"尚有衝突未選擇處理方式（跳過 / 加後綴 / 互換）";
			return res;
		}

	StopAudio();   // MCI keeps the previewed file locked

	auto full = [&](const std::wstring& name) { return folder_ + L"\\" + name; };
	auto existsOnDisk = [&](const std::wstring& name) {
		DWORD a = GetFileAttributesW(full(name).c_str());
		return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
	};

	auto syncRefs = [&](const RenamePlanEntry& e, const std::wstring& finalName) {
		if (!syncDoc || !syncDoc->file() || e.refLines.empty()) return;
		FilterFile* f = syncDoc->file();
		std::string newU8 = narrow(finalName);
		for (int li : e.refLines) {
			if (li < 0 || li >= (int)f->lines.size()) continue;
			FilterLine& ln = f->lines[li];
			if (ln.values.empty()) continue;
			std::string v = ln.values[0].text;
			size_t slash = v.find_last_of("\\/");
			std::string prefix = (slash == std::string::npos) ? std::string() : v.substr(0, slash + 1);
			FilterSetValueStr(ln, 0, prefix + newU8, true);
			f->dirty = true;
			res.refsUpdated++;
		}
	};

	for (RenamePlanEntry& e : plan) {
		if (e.state == RenamePlanEntry::State::Unchanged) continue;

		std::wstring target = e.newName;
		bool isSwap = false;
		if (e.state == RenamePlanEntry::State::Conflict) {
			switch (e.resolution) {
				case RenamePlanEntry::Resolution::Skip:
					res.skipped++;
					continue;
				case RenamePlanEntry::Resolution::Suffix: {
					std::wstring stem, ext;
					split_ext(e.newName, stem, ext);
					int n = 2;
					std::wstring cand;
					do {
						cand = stem + L" (" + std::to_wstring(n) + L")" + (ext.empty() ? L"" : L"." + ext);
						n++;
					} while (existsOnDisk(cand) && n < 1000);
					target = cand;
					break;
				}
				case RenamePlanEntry::Resolution::Swap:
					isSwap = true;
					break;
				default:
					res.skipped++;
					continue;
			}
		}

		if (isSwap) {
			// A <-> B via a temp name. References are left untouched: the point
			// of a swap is that each NAME keeps its role but gets the other sound.
			std::wstring tmp = e.oldName + L".pobswap.tmp";
			if (!MoveFileExW(full(e.oldName).c_str(), full(tmp).c_str(), 0) ||
			    !MoveFileExW(full(e.newName).c_str(), full(e.oldName).c_str(), 0) ||
			    !MoveFileExW(full(tmp).c_str(), full(e.newName).c_str(), 0)) {
				res.err += narrow(e.oldName) + u8": 互換失敗; ";
				res.skipped++;
				MoveFileExW(full(tmp).c_str(), full(e.oldName).c_str(), 0); // best-effort undo
				continue;
			}
			res.swapped++;
			continue;
		}

		if (!MoveFileExW(full(e.oldName).c_str(), full(target).c_str(), 0)) {
			res.err += narrow(e.oldName) + u8": 改名失敗; ";
			res.skipped++;
			continue;
		}
		res.renamed++;
		syncRefs(e, target);
	}

	Rescan();
	return res;
}
