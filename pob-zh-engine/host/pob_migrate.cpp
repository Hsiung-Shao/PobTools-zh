#include "pob_migrate.h"

#include <windows.h>
#include <shlobj.h>

#include "hash_sha256.h"
#include "http_client.h"
#include "launcher_config.h" // DetectInstalls
#include "win_util.h"
#include "zip_extract.h"

#include <json.hpp> // nlohmann::ordered_json (deps/nlohmann)

#include <vector>

using nlohmann::ordered_json;

namespace {

std::wstring with_slash(std::wstring d)
{
	if (!d.empty() && d.back() != L'\\' && d.back() != L'/') d += L'\\';
	return d;
}

std::wstring no_slash(std::wstring d)
{
	for (wchar_t& c : d) if (c == L'/') c = L'\\';
	while (d.size() > 3 && d.back() == L'\\') d.pop_back(); // keep "C:\"
	return d;
}

bool path_exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

struct Entry { std::wstring name; bool dir; bool link; };

std::vector<Entry> list_dir(const std::wstring& dir)
{
	std::vector<Entry> out;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return out;
	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
		out.push_back({ fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
		                // only real links; OneDrive placeholders are reparse points too and must still copy
		                (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
		                    (fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT || fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK) });
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return out;
}

void rmtree(const std::wstring& dir)
{
	for (const Entry& e : list_dir(dir)) {
		const std::wstring p = dir + L"\\" + e.name;
		if (e.dir && e.link) RemoveDirectoryW(p.c_str()); // a junction: drop the link, never its target
		else if (e.dir) rmtree(p);
		else {
			SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
			DeleteFileW(p.c_str());
		}
	}
	RemoveDirectoryW(dir.c_str());
}

// 0 = different, 1 = identical, -1 = could not read
int same_bytes(const std::wstring& a, const std::wstring& b)
{
	WIN32_FILE_ATTRIBUTE_DATA fa{}, fb{};
	if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa) ||
	    !GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb)) return -1;
	if (fa.nFileSizeHigh != fb.nFileSizeHigh || fa.nFileSizeLow != fb.nFileSizeLow) return 0;
	std::string ca, cb;
	if (!read_file_utf8(a, ca) || !read_file_utf8(b, cb)) return -1;
	return ca == cb ? 1 : 0;
}

bool is_same_or_inside(const std::wstring& inner, const std::wstring& outer)
{
	const std::wstring i = no_slash(inner), o = no_slash(outer);
	if (i.size() < o.size() || _wcsnicmp(i.c_str(), o.c_str(), o.size()) != 0) return false;
	return i.size() == o.size() || o.back() == L'\\' /* drive root */ || i[o.size()] == L'\\';
}

void copy_tree(const std::wstring& src, const std::wstring& dst, BuildCopyStats& st)
{
	SHCreateDirectoryExW(nullptr, dst.c_str(), nullptr);
	if (!dir_exists(dst)) { st.failed++; return; }
	for (const Entry& e : list_dir(src)) {
		const std::wstring s = src + L"\\" + e.name, d = dst + L"\\" + e.name;
		if (e.link) { st.failed++; continue; } // junctions/symlinks can loop or point anywhere
		if (e.dir) {
			if (file_exists(d)) st.failed++; // a file sits where the folder should go
			else copy_tree(s, d, st);
			continue;
		}
		const size_t dot = e.name.find_last_of(L'.');
		const std::wstring stem = dot == std::wstring::npos || dot == 0 ? e.name : e.name.substr(0, dot);
		const std::wstring ext = e.name.substr(stem.size());
		bool done = false;
		for (int n = 0; n < 1000 && !done; n++) {
			const std::wstring cand = n == 0 ? d
				: dst + L"\\" + stem + (n == 1 ? std::wstring(L" (匯入)") : L" (匯入 " + std::to_wstring(n) + L")") + ext;
			if (!path_exists(cand)) {
				if (CopyFileW(s.c_str(), cand.c_str(), TRUE)) (n == 0 ? st.copied : st.renamed)++;
				else st.failed++;
				done = true;
			} else {
				const int same = dir_exists(cand) ? 0 : same_bytes(s, cand);
				if (same == 1) { st.skipped++; done = true; }
				else if (same < 0) { st.failed++; done = true; }
			}
		}
		if (!done) st.failed++;
	}
}

std::string lower_ascii(std::string s)
{
	for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
	return s;
}

std::string xml_unescape(std::string s)
{
	static const char* const kEnt[][2] = { { "&quot;", "\"" }, { "&apos;", "'" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&amp;", "&" } };
	for (const auto& e : kEnt)
		for (size_t p = 0; (p = s.find(e[0], p)) != std::string::npos; p++) s.replace(p, strlen(e[0]), e[1]);
	return s;
}

} // namespace

std::wstring PobBuildDir(const std::wstring& pobDir)
{
	const std::wstring base = no_slash(pobDir);
	std::string xml;
	if (read_file_utf8(base + L"\\Settings.xml", xml)) {
		const size_t tag = xml.find("<Misc");
		const size_t end = tag == std::string::npos ? tag : xml.find('>', tag);
		if (end != std::string::npos) {
			const std::string misc = xml.substr(tag, end - tag);
			const char* key = " buildPath=\"";
			const size_t a = misc.find(key);
			const size_t b = a == std::string::npos ? a : misc.find('"', a + strlen(key));
			if (b != std::string::npos && b > a + strlen(key))
				return no_slash(widen(xml_unescape(misc.substr(a + strlen(key), b - a - strlen(key)))));
		}
	}
	return base + L"\\Builds";
}

BuildCopyStats ImportBuildTree(const std::wstring& srcDir, const std::wstring& dstDir)
{
	BuildCopyStats st;
	const std::wstring s = no_slash(srcDir), d = no_slash(dstDir);
	// Importing a tree into itself would never end.
	if (!dir_exists(s) || is_same_or_inside(d, s) || is_same_or_inside(s, d)) return st;
	copy_tree(s, d, st);
	return st;
}

BuildImportResult ImportBuildsFromTool(const std::wstring& srcToolDir, const std::wstring& ourExeDir)
{
	BuildImportResult r;
	const InstallInfo src = DetectInstalls(with_slash(srcToolDir));
	const InstallInfo ours = DetectInstalls(with_slash(ourExeDir));
	r.sourceFound = !src.poe1Dir.empty() || !src.poe2Dir.empty();
	auto one = [](const std::wstring& from, const std::wstring& to, bool& need, BuildCopyStats& st) {
		if (from.empty()) return;
		const std::wstring builds = PobBuildDir(from);
		if (!dir_exists(builds)) return;
		if (to.empty()) need = true;
		else st = ImportBuildTree(builds, PobBuildDir(to));
	};
	one(src.poe1Dir, ours.poe1Dir, r.needPoe1, r.poe1);
	one(src.poe2Dir, ours.poe2Dir, r.needPoe2, r.poe2);
	return r;
}

const wchar_t* PobPortableDirName(bool poe2)
{
	return poe2 ? L"PathOfBuildingCommunity-PoE2-Portable" : L"PathOfBuildingCommunity-Portable";
}

bool ParsePobRelease(const std::string& json, bool poe2, PobRelease& out, std::string* err)
{
	const std::string want = narrow(PobPortableDirName(poe2)) + ".zip";
	const ordered_json j = ordered_json::parse(json, nullptr, false);
	if (j.is_object() && j.contains("assets") && j["assets"].is_array()) {
		for (const auto& a : j["assets"]) {
			if (!a.is_object() || a.value("name", std::string()) != want) continue;
			const std::string digest = a.contains("digest") && a["digest"].is_string() ? a["digest"].get<std::string>() : "";
			PobRelease r;
			r.tag = j.value("tag_name", std::string());
			r.url = a.value("browser_download_url", std::string());
			r.size = a.contains("size") && a["size"].is_number_unsigned() ? a["size"].get<unsigned long long>() : 0;
			if (digest.compare(0, 7, "sha256:") == 0) r.sha256 = lower_ascii(digest.substr(7));
			if (r.url.empty() || r.sha256.size() != 64) break;
			out = r;
			return true;
		}
	}
	if (err) *err = u8"官方發布資料裡找不到可用的 " + want;
	return false;
}

bool InstallPobZip(const void* zip, size_t size, const std::string& sha256Hex,
                   const std::wstring& exeDir, bool poe2, std::string* err)
{
	auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
	const std::wstring dest = with_slash(exeDir) + PobPortableDirName(poe2);
	if (path_exists(dest)) return fail(u8"資料夾已存在：" + narrow(dest));
	std::string hex;
	if (sha256Hex.empty() || !Sha256Hex(zip, size, &hex) || hex != lower_ascii(sha256Hex))
		return fail(u8"下載的檔案與官方雜湊不符，已放棄安裝");

	// Not beside the exe: DetectInstalls would take a half-extracted folder there for an install.
	const std::wstring staging = with_slash(exeDir) + L"PobTools\\" + PobPortableDirName(poe2) + L".part";
	rmtree(staging); // a leftover of ours from an interrupted run
	std::string e;
	if (!ExtractZipToDir(zip, size, staging, &e)) {
		rmtree(staging);
		return fail(e);
	}
	std::wstring root = staging;
	if (!file_exists(root + L"\\Launch.lua")) {
		const std::vector<Entry> top = list_dir(staging);
		if (top.size() == 1 && top[0].dir) root = staging + L"\\" + top[0].name;
	}
	if (!file_exists(root + L"\\Launch.lua")) {
		rmtree(staging);
		return fail(u8"壓縮檔內容不是 Path of Building");
	}
	// A virus scanner may still hold the fresh exe/dll files open for a moment.
	bool ok = false;
	for (int i = 0; i < 10 && !(ok = MoveFileExW(root.c_str(), dest.c_str(), 0) != 0); i++) Sleep(500);
	rmtree(staging);
	return ok ? true : fail(u8"無法寫入安裝資料夾：" + narrow(dest));
}

bool DownloadAndInstallPob(const std::wstring& exeDir, bool poe2, const std::atomic<bool>* cancel,
                           const std::function<void(unsigned long long, unsigned long long)>& progress,
                           std::string* err)
{
	auto fail = [&](const std::string& m) { if (err) *err = m; return false; };
	if (path_exists(with_slash(exeDir) + PobPortableDirName(poe2))) return fail(u8"安裝資料夾已存在");

	std::string body;
	{
		HttpsClient api(L"api.github.com");
		const wchar_t* path = poe2 ? L"/repos/PathOfBuildingCommunity/PathOfBuilding-PoE2/releases/latest"
		                           : L"/repos/PathOfBuildingCommunity/PathOfBuilding/releases/latest";
		if (!api.GetString(path, body, err, cancel)) return false;
	}
	PobRelease rel;
	if (!ParsePobRelease(body, poe2, rel, err)) return false;

	// zip + extracted copy, with headroom
	ULARGE_INTEGER freeBytes{};
	if (rel.size && GetDiskFreeSpaceExW(with_slash(exeDir).c_str(), &freeBytes, nullptr, nullptr) &&
	    freeBytes.QuadPart < rel.size * 3)
		return fail(u8"磁碟空間不足，需要約 " + std::to_string(rel.size * 3 / (1024 * 1024)) + " MB");

	const size_t slash = rel.url.find('/', 8);
	if (rel.url.compare(0, 8, "https://") != 0 || slash == std::string::npos) return fail(u8"下載網址格式不正確");
	// shortcut: the whole zip (~630 MB) is held in memory because ExtractZipToDir
	// reads from memory; move to a temp file + file mapping if 32-bit builds or low-RAM reports appear.
	std::vector<unsigned char> buf;
	{
		HttpsClient c(widen(rel.url.substr(8, slash - 8)));
		// no Content-Length: fall back to the size the release lists
		const std::function<void(unsigned long long, unsigned long long)> prog =
			[&](unsigned long long got, unsigned long long total) { if (progress) progress(got, total ? total : rel.size); };
		if (!c.Get(widen(rel.url.substr(slash)), buf, err, cancel, prog)) return false;
	}
	if (cancel && cancel->load()) return fail(u8"已取消");
	return InstallPobZip(buf.data(), buf.size(), rel.sha256, exeDir, poe2, err);
}
