// --migrate-selftest: 只依 pob_migrate.h 的合約檢查匯入流派與安裝 POB zip 的規則。
// 全部在 %TEMP% 沙盒內操作本機檔案;不測 DownloadAndInstallPob(需要網路)。
// 報告寫到 <exeDir>PobTools\migrate_selftest.txt(UTF-8),回傳失敗數。
#include "pob_migrate.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <miniz.h>

#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "hash_sha256.h"
#include "launcher_config.h"

namespace {

// "匯入" 的 wide 字面值;用 \u 跳脫避免來源檔編碼影響檔名比對
const wchar_t* kTag = L"匯入";

const wchar_t* kP1 = L"PathOfBuildingCommunity-Portable";
const wchar_t* kP2 = L"PathOfBuildingCommunity-PoE2-Portable";

std::wstring J(const std::wstring& a, const std::wstring& b) { return a + L"\\" + b; }

std::string Narrow(const std::wstring& w)
{
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
	return s;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// 遞迴建立資料夾(含中間層)
bool MkDirs(const std::wstring& p)
{
	for (size_t i = 3; i <= p.size(); i++) {
		if (i == p.size() || p[i] == L'\\') {
			std::wstring part = p.substr(0, i);
			if (!CreateDirectoryW(part.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
		}
	}
	return true;
}

bool WriteFileBytes(const std::wstring& p, const std::string& content)
{
	size_t slash = p.find_last_of(L'\\');
	if (slash != std::wstring::npos) MkDirs(p.substr(0, slash));
	HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	bool ok = content.empty() || WriteFile(h, content.data(), (DWORD)content.size(), &w, nullptr);
	CloseHandle(h);
	return ok && w == content.size();
}

bool ReadFileBytes(const std::wstring& p, std::string& out)
{
	out.clear();
	HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER sz{};
	bool ok = GetFileSizeEx(h, &sz) != 0;
	if (ok && sz.QuadPart > 0) {
		out.resize((size_t)sz.QuadPart);
		DWORD got = 0;
		ok = ReadFile(h, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size();
	}
	CloseHandle(h);
	return ok;
}

std::string Content(const std::wstring& p)
{
	std::string s;
	return ReadFileBytes(p, s) ? s : std::string("<missing>");
}

void RmTree(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
			std::wstring p = dir + L"\\" + fd.cFileName;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RmTree(p);
			else DeleteFileW(p.c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

// 目錄快照:相對路徑 -> 內容(files)與相對資料夾路徑(dirs)
struct Snap {
	std::map<std::wstring, std::string> files;
	std::set<std::wstring> dirs;
	bool operator==(const Snap& o) const { return files == o.files && dirs == o.dirs; }
};

void SnapInto(const std::wstring& root, const std::wstring& rel, Snap& s)
{
	WIN32_FIND_DATAW fd{};
	std::wstring dir = rel.empty() ? root : J(root, rel);
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
		std::wstring r = rel.empty() ? std::wstring(fd.cFileName) : J(rel, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			s.dirs.insert(r);
			SnapInto(root, r, s);
		} else {
			std::string c;
			ReadFileBytes(J(root, r), c);
			s.files[r] = c;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

Snap TakeSnap(const std::wstring& root)
{
	Snap s;
	SnapInto(root, L"", s);
	return s;
}

// ---- 記憶體 zip ----
using Entries = std::vector<std::pair<std::string, std::string>>;

std::string MakeZip(const Entries& entries)
{
	mz_zip_archive zip;
	memset(&zip, 0, sizeof(zip));
	std::string out;
	if (!mz_zip_writer_init_heap(&zip, 0, 0)) return out;
	for (const auto& e : entries)
		mz_zip_writer_add_mem(&zip, e.first.c_str(), e.second.data(), e.second.size(), MZ_DEFAULT_LEVEL);
	void* buf = nullptr;
	size_t sz = 0;
	if (mz_zip_writer_finalize_heap_archive(&zip, &buf, &sz) && buf) out.assign((const char*)buf, sz);
	mz_zip_writer_end(&zip);
	return out;
}

// 標準 POB zip:Launch.lua + 一個子資料夾檔;prefix 為頂層資料夾("" 或 "Name/")
Entries PobEntries(const std::string& prefix)
{
	return { { prefix + "Launch.lua", "-- launch" },
	         { prefix + "Data/x.lua", "return 1" },
	         { prefix + "Builds/readme.txt", "hi" } };
}

std::string Sha(const std::string& bytes)
{
	std::string h;
	Sha256Hex(bytes.data(), bytes.size(), &h);
	return h;
}

// ---- GitHub releases JSON 樣本 ----
std::string Asset(const std::string& name, const std::string& url, unsigned long long size, const std::string& digest)
{
	std::string s = "{\"name\":\"" + name + "\"";
	if (!url.empty()) s += ",\"browser_download_url\":\"" + url + "\"";
	s += ",\"size\":" + std::to_string(size);
	if (!digest.empty()) s += ",\"digest\":\"" + digest + "\"";
	return s + "}";
}

std::string Release(const std::string& assetsJoined)
{
	return "{\"tag_name\":\"v2.67.2\",\"name\":\"Release 2.67.2\",\"draft\":false,\"assets\":[" + assetsJoined + "]}";
}

std::string SettingsXml(const std::wstring& buildPath)
{
	return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<PathOfBuilding>\n"
	       "\t<Foo bar=\"1\"/>\n"
	       "\t<Misc worksheetSearchDirection=\"1\" buildPath=\"" + Narrow(buildPath) + "\" showThousandsSeparators=\"true\"/>\n"
	       "</PathOfBuilding>\n";
}

std::string Stats(const BuildCopyStats& s)
{
	return "c=" + std::to_string(s.copied) + " r=" + std::to_string(s.renamed) +
	       " s=" + std::to_string(s.skipped) + " f=" + std::to_string(s.failed);
}

} // namespace

int RunMigrateSelfTest(const std::wstring& exeDir)
{
	wchar_t tmp[MAX_PATH];
	DWORD n = GetTempPathW(MAX_PATH, tmp);
	if (n == 0 || n >= MAX_PATH) return 1;
	const std::wstring root = std::wstring(tmp) + L"pobtools_migrate_st";
	RmTree(root);
	if (!MkDirs(root)) return 1;

	std::string report;
	int failures = 0;
	auto check = [&](const char* name, bool ok, const std::string& detail) {
		report += std::string(ok ? "PASS " : "FAIL ") + name + (detail.empty() ? "" : "  (" + detail + ")") + "\n";
		if (!ok) failures++;
	};
	auto Case = [&](const wchar_t* name) {
		std::wstring p = J(root, name);
		MkDirs(p);
		return p;
	};
	const std::wstring suffix1 = std::wstring(L" (") + kTag + L")";
	const std::wstring suffix2 = std::wstring(L" (") + kTag + L" 2)";
	const std::string binary("A\0B\r\n\xff\x01 end", 11);

	// ===== I1 多層資料夾照原樣複製 =====
	{
		std::wstring c = Case(L"i1");
		std::wstring src = J(c, L"src"), dst = J(c, L"dst\\new");
		MkDirs(J(c, L"dst")); // 父層存在、目的地本身不存在
		WriteFileBytes(J(src, L"top.xml"), "<top/>");
		WriteFileBytes(J(src, L"A\\y.xml"), "<y/>");
		WriteFileBytes(J(src, L"A\\B\\x.xml"), "<x/>");
		WriteFileBytes(J(src, L"A\\B\\C\\deep.xml"), "<deep/>");
		WriteFileBytes(J(src, L"A\\B\\C\\我的 流派 (1).xml"), "<zh/>");
		Snap before = TakeSnap(src);
		BuildCopyStats st = ImportBuildTree(src, dst);
		Snap after = TakeSnap(dst);
		check(u8"I1a 目的地不存在時,多層資料夾與內容逐檔相同", after.files == before.files,
		      "src=" + std::to_string(before.files.size()) + " dst=" + std::to_string(after.files.size()));
		check(u8"I1b copied 等於檔數且其餘為 0",
		      st.copied == (int)before.files.size() && st.renamed == 0 && st.skipped == 0 && st.failed == 0, Stats(st));

		std::wstring dst2 = J(c, L"dst2");
		MkDirs(dst2);
		BuildCopyStats st2 = ImportBuildTree(src, dst2);
		check(u8"I1c 目的地為既有空資料夾時同樣逐檔相同", TakeSnap(dst2).files == before.files && st2.copied == (int)before.files.size(),
		      Stats(st2));

		// 假設:dstDir 的上層也不存在時一併建立(合約只寫「created when missing」)
		std::wstring dst3 = J(c, L"no\\such\\parent\\dst");
		BuildCopyStats st3 = ImportBuildTree(src, dst3);
		check(u8"I1d 目的地連上層都不存在時一併建立並逐檔相同", TakeSnap(dst3).files == before.files && st3.copied == (int)before.files.size(),
		      Stats(st3));
	}

	// ===== I2 非 xml 檔也複製 =====
	{
		std::wstring c = Case(L"i2");
		std::wstring src = J(c, L"src"), dst = J(c, L"dst");
		WriteFileBytes(J(src, L"note.txt"), "text\r\nline2");
		WriteFileBytes(J(src, L"cfg.json"), "{\"a\":1}");
		WriteFileBytes(J(src, L"noext"), "no extension");
		WriteFileBytes(J(src, L"sub\\blob.bin"), binary);
		WriteFileBytes(J(src, L"b.xml"), "<b/>");
		Snap before = TakeSnap(src);
		BuildCopyStats st = ImportBuildTree(src, dst);
		Snap after = TakeSnap(dst);
		check(u8"I2a txt/json/無副檔名/二進位檔全部複製且位元組相同", after.files == before.files && st.copied == 5, Stats(st));
		check(u8"I2b 二進位內容含 \\0 與 CRLF 未被改寫", Content(J(dst, L"sub\\blob.bin")) == binary, "");
	}

	// ===== I3 同名同內容略過 =====
	{
		std::wstring c = Case(L"i3");
		std::wstring src = J(c, L"src"), dst = J(c, L"dst");
		WriteFileBytes(J(src, L"a.xml"), "SAME");
		WriteFileBytes(J(dst, L"a.xml"), "SAME");
		Snap before = TakeSnap(dst);
		BuildCopyStats st = ImportBuildTree(src, dst);
		check(u8"I3a 同名同內容不新增任何檔", TakeSnap(dst) == before, "");
		check(u8"I3b 同名同內容計為 skipped=1 且 copied/renamed 為 0",
		      st.skipped == 1 && st.copied == 0 && st.renamed == 0 && st.failed == 0, Stats(st));
	}

	// ===== I4 同名不同內容加後綴 =====
	std::wstring i4src;
	Snap i4srcBefore;
	{
		std::wstring c = Case(L"i4");
		std::wstring src = J(c, L"src"), dst = J(c, L"dst");
		i4src = src;
		WriteFileBytes(J(src, L"a.xml"), "NEW");
		WriteFileBytes(J(dst, L"a.xml"), "OLD");
		i4srcBefore = TakeSnap(src);
		BuildCopyStats st = ImportBuildTree(src, dst);
		Snap after = TakeSnap(dst);
		check(u8"I4a 目的地原檔內容不變", Content(J(dst, L"a.xml")) == "OLD", "");
		check(u8"I4b 來源內容寫成 a (匯入).xml 且只多一個檔",
		      Content(J(dst, L"a" + suffix1 + L".xml")) == "NEW" && after.files.size() == 2, "n=" + std::to_string(after.files.size()));
		check(u8"I4c 計為 renamed=1", st.renamed == 1 && st.copied == 0 && st.skipped == 0 && st.failed == 0, Stats(st));

		// 無副檔名的檔名衝突:後綴接在檔名尾端
		std::wstring c2 = Case(L"i4b");
		WriteFileBytes(J(c2, L"src\\noext"), "NEW2");
		WriteFileBytes(J(c2, L"dst\\noext"), "OLD2");
		ImportBuildTree(J(c2, L"src"), J(c2, L"dst"));
		check(u8"I4d 無副檔名衝突時寫成 noext (匯入)", Content(J(c2, L"dst\\noext" + suffix1)) == "NEW2" &&
		      Content(J(c2, L"dst\\noext")) == "OLD2", "");

		// 子資料夾內衝突:後綴留在同一資料夾
		std::wstring c3 = Case(L"i4c");
		WriteFileBytes(J(c3, L"src\\L\\a.xml"), "NEW3");
		WriteFileBytes(J(c3, L"dst\\L\\a.xml"), "OLD3");
		ImportBuildTree(J(c3, L"src"), J(c3, L"dst"));
		check(u8"I4e 子資料夾內衝突的別名檔留在同一子資料夾", Content(J(c3, L"dst\\L\\a" + suffix1 + L".xml")) == "NEW3", "");
	}

	// ===== I5 後綴再撞名往下編號 =====
	{
		std::wstring c = Case(L"i5");
		std::wstring src = J(c, L"src"), dst = J(c, L"dst");
		WriteFileBytes(J(src, L"a.xml"), "NEW");
		WriteFileBytes(J(dst, L"a.xml"), "O1");
		WriteFileBytes(J(dst, L"a" + suffix1 + L".xml"), "O2");
		BuildCopyStats st = ImportBuildTree(src, dst);
		check(u8"I5a 產生 a (匯入 2).xml 內容為來源", Content(J(dst, L"a" + suffix2 + L".xml")) == "NEW", Stats(st));
		check(u8"I5b 原本兩個檔內容不變且 renamed=1",
		      Content(J(dst, L"a.xml")) == "O1" && Content(J(dst, L"a" + suffix1 + L".xml")) == "O2" &&
		      st.renamed == 1 && TakeSnap(dst).files.size() == 3, "");

		// 再往下:(匯入) 與 (匯入 2) 都被占用 -> (匯入 3)
		std::wstring c2 = Case(L"i5b");
		WriteFileBytes(J(c2, L"src\\a.xml"), "NEW");
		WriteFileBytes(J(c2, L"dst\\a.xml"), "O1");
		WriteFileBytes(J(c2, L"dst\\a" + suffix1 + L".xml"), "O2");
		WriteFileBytes(J(c2, L"dst\\a" + suffix2 + L".xml"), "O3");
		ImportBuildTree(J(c2, L"src"), J(c2, L"dst"));
		check(u8"I5c 兩個後綴都被占用時編號到 3", Content(J(c2, L"dst\\a (" + std::wstring(kTag) + L" 3).xml")) == "NEW", "");

		// 別名位置已有相同內容:不再新增
		std::wstring c3 = Case(L"i5c");
		WriteFileBytes(J(c3, L"src\\a.xml"), "NEW");
		WriteFileBytes(J(c3, L"dst\\a.xml"), "OLD");
		WriteFileBytes(J(c3, L"dst\\a" + suffix1 + L".xml"), "X");
		WriteFileBytes(J(c3, L"dst\\a" + suffix2 + L".xml"), "NEW");
		Snap before = TakeSnap(J(c3, L"dst"));
		BuildCopyStats st3 = ImportBuildTree(J(c3, L"src"), J(c3, L"dst"));
		check(u8"I5d 後面編號已有相同內容時略過不新增", TakeSnap(J(c3, L"dst")) == before && st3.skipped == 1 && st3.renamed == 0,
		      Stats(st3));
	}

	// ===== I6 同名資料夾合併 =====
	{
		std::wstring c = Case(L"i6");
		std::wstring src = J(c, L"src"), dst = J(c, L"dst");
		WriteFileBytes(J(src, L"League\\s.xml"), "S");
		WriteFileBytes(J(dst, L"League\\d.xml"), "D");
		BuildCopyStats st = ImportBuildTree(src, dst);
		Snap after = TakeSnap(dst);
		std::map<std::wstring, std::string> want = { { L"League\\d.xml", "D" }, { L"League\\s.xml", "S" } };
		check(u8"I6a 兩邊的檔都在同一個 League 資料夾", after.files == want, "n=" + std::to_string(after.files.size()));
		check(u8"I6b 沒有多出別名資料夾", after.dirs == std::set<std::wstring>{ L"League" } && st.copied == 1 && st.renamed == 0,
		      Stats(st));
	}

	// ===== I7 重複匯入不增加檔案 =====
	{
		std::wstring c = Case(L"i7");
		std::wstring src = J(c, L"src"), dst = J(c, L"dst");
		WriteFileBytes(J(src, L"a.xml"), "NEW");
		WriteFileBytes(J(src, L"sub\\b.xml"), "B");
		WriteFileBytes(J(dst, L"a.xml"), "OLD");
		BuildCopyStats first = ImportBuildTree(src, dst);
		Snap mid = TakeSnap(dst);
		BuildCopyStats second = ImportBuildTree(src, dst);
		Snap last = TakeSnap(dst);
		check(u8"I7a 第一次匯入 copied=1 renamed=1", first.copied == 1 && first.renamed == 1, Stats(first));
		check(u8"I7b 第二次匯入 copied=0 renamed=0 skipped=2 failed=0",
		      second.copied == 0 && second.renamed == 0 && second.skipped == 2 && second.failed == 0, Stats(second));
		check(u8"I7c 第二次匯入後目的地完全不變", mid == last && last.files.size() == 3, "n=" + std::to_string(last.files.size()));
	}

	// ===== I8 兩代分開匯入 =====
	{
		std::wstring c = Case(L"i8");
		std::wstring ours = J(c, L"ours"), old = J(c, L"old");
		WriteFileBytes(J(ours, std::wstring(kP1) + L"\\Launch.lua"), "x");
		WriteFileBytes(J(ours, std::wstring(kP2) + L"\\Launch.lua"), "x");
		WriteFileBytes(J(old, L"PathOfBuildingCommunity\\Launch.lua"), "x");
		WriteFileBytes(J(old, L"PathOfBuildingCommunity\\Builds\\p1.xml"), "P1");
		WriteFileBytes(J(old, L"PathOfBuildingCommunity\\Builds\\sub\\p1b.xml"), "P1B");
		WriteFileBytes(J(old, std::wstring(kP2) + L"\\Launch.lua"), "x");
		WriteFileBytes(J(old, std::wstring(kP2) + L"\\Builds\\p2.xml"), "P2");
		Snap oldBefore = TakeSnap(old);
		BuildImportResult r = ImportBuildsFromTool(old, ours + L"\\");
		std::wstring b1 = J(ours, std::wstring(kP1) + L"\\Builds"), b2 = J(ours, std::wstring(kP2) + L"\\Builds");
		check(u8"I8a PoE1 流派進到我們的 PoE1 Builds 且保留子資料夾",
		      Content(J(b1, L"p1.xml")) == "P1" && Content(J(b1, L"sub\\p1b.xml")) == "P1B", "");
		check(u8"I8b PoE2 流派進到我們的 PoE2 Builds", Content(J(b2, L"p2.xml")) == "P2", "");
		check(u8"I8c 兩代不混放", !Exists(J(b1, L"p2.xml")) && !Exists(J(b2, L"p1.xml")) && !Exists(J(b2, L"sub")), "");
		check(u8"I8d sourceFound=true 無 needPoe 且統計正確",
		      r.sourceFound && !r.needPoe1 && !r.needPoe2 && r.poe1.copied == 2 && r.poe2.copied == 1 &&
		      r.poe1.failed == 0 && r.poe2.failed == 0,
		      "p1:" + Stats(r.poe1) + " p2:" + Stats(r.poe2));
		check(u8"I8e 來源工具資料夾不被更動", TakeSnap(old) == oldBefore, "");
	}

	// ===== I9 缺那一代 POB 時不匯入 =====
	{
		std::wstring c = Case(L"i9");
		std::wstring ours = J(c, L"ours"), old = J(c, L"old");
		WriteFileBytes(J(ours, std::wstring(kP1) + L"\\Launch.lua"), "x");
		WriteFileBytes(J(old, L"PathOfBuildingCommunity\\Launch.lua"), "x");
		WriteFileBytes(J(old, L"PathOfBuildingCommunity\\Builds\\p1.xml"), "P1");
		WriteFileBytes(J(old, std::wstring(kP2) + L"\\Launch.lua"), "x");
		WriteFileBytes(J(old, std::wstring(kP2) + L"\\Builds\\p2.xml"), "P2");
		Snap before = TakeSnap(ours);
		BuildImportResult r = ImportBuildsFromTool(old, ours);
		Snap after = TakeSnap(ours);
		auto want = before.files;
		want[std::wstring(kP1) + L"\\Builds\\p1.xml"] = "P1";
		check(u8"I9a PoE2 流派不出現在我們目錄任何位置", after.files == want && !Exists(J(ours, kP2 + std::wstring(L"\\Builds"))),
		      "files=" + std::to_string(after.files.size()));
		check(u8"I9b needPoe2=true needPoe1=false 且 PoE1 照常匯入",
		      r.sourceFound && r.needPoe2 && !r.needPoe1 && r.poe1.copied == 1 && r.poe2.copied == 0 && r.poe2.renamed == 0,
		      "p1:" + Stats(r.poe1) + " p2:" + Stats(r.poe2));
	}

	// ===== I10 來源沒有 POB 時回報 =====
	{
		std::wstring c = Case(L"i10");
		std::wstring ours = J(c, L"ours"), plain = J(c, L"plain");
		WriteFileBytes(J(ours, std::wstring(kP1) + L"\\Launch.lua"), "x");
		WriteFileBytes(J(ours, std::wstring(kP1) + L"\\Builds\\mine.xml"), "MINE");
		WriteFileBytes(J(plain, L"Builds\\x.xml"), "X");
		WriteFileBytes(J(plain, L"readme.txt"), "no pob here");
		Snap before = TakeSnap(ours);
		BuildImportResult r = ImportBuildsFromTool(plain, ours);
		check(u8"I10a 無 Launch.lua 的資料夾回報 sourceFound=false", !r.sourceFound && !r.needPoe1 && !r.needPoe2, "");
		check(u8"I10b 我們的目錄完全不變且統計為 0",
		      TakeSnap(ours) == before && r.poe1.copied == 0 && r.poe2.copied == 0 && r.poe1.renamed == 0, "");
	}

	// ===== I11 自訂流派路徑 =====
	{
		std::wstring c = Case(L"i11");
		std::wstring pob = J(c, L"pob");
		MkDirs(pob);
		check(u8"I11a 沒有 Settings.xml 時回 <pobDir>\\Builds", PobBuildDir(pob) == pob + L"\\Builds", Narrow(PobBuildDir(pob)));

		std::wstring custom = J(c, L"custom_builds");
		WriteFileBytes(J(pob, L"Settings.xml"), SettingsXml(custom));
		check(u8"I11b Settings.xml 的 Misc buildPath 生效(無尾端分隔)", PobBuildDir(pob) == custom, Narrow(PobBuildDir(pob)));

		std::wstring pob2 = J(c, L"pob2");
		WriteFileBytes(J(pob2, L"Settings.xml"), SettingsXml(custom + L"\\"));
		check(u8"I11c buildPath 帶尾端反斜線時回傳去除尾端分隔", PobBuildDir(pob2) == custom, Narrow(PobBuildDir(pob2)));

		std::wstring pob3 = J(c, L"pob3");
		WriteFileBytes(J(pob3, L"Settings.xml"), SettingsXml(L""));
		check(u8"I11d buildPath 為空字串時回 <pobDir>\\Builds", PobBuildDir(pob3) == pob3 + L"\\Builds", Narrow(PobBuildDir(pob3)));

		// 匯入時寫進自訂路徑,不是 <POB>\Builds
		std::wstring ours = J(c, L"ours"), old = J(c, L"old");
		std::wstring ourPob = J(ours, kP1);
		WriteFileBytes(J(ourPob, L"Launch.lua"), "x");
		WriteFileBytes(J(ourPob, L"Settings.xml"), SettingsXml(J(c, L"ours_custom")));
		WriteFileBytes(J(old, L"PathOfBuildingCommunity\\Launch.lua"), "x");
		WriteFileBytes(J(old, L"PathOfBuildingCommunity\\Builds\\x.xml"), "XB");
		BuildImportResult r = ImportBuildsFromTool(old, ours);
		check(u8"I11e 匯入的檔寫在自訂路徑", Content(J(c, L"ours_custom\\x.xml")) == "XB" && r.poe1.copied == 1,
		      "p1:" + Stats(r.poe1));
		check(u8"I11f 不寫進 <POB>\\Builds", !Exists(J(ourPob, L"Builds\\x.xml")), "");

		// 來源端自訂路徑:要從它的 buildPath 讀,不是 <src POB>\Builds
		std::wstring c2 = Case(L"i11b");
		std::wstring ours2 = J(c2, L"ours"), old2 = J(c2, L"old");
		WriteFileBytes(J(ours2, std::wstring(kP1) + L"\\Launch.lua"), "x");
		WriteFileBytes(J(old2, L"PathOfBuildingCommunity\\Launch.lua"), "x");
		WriteFileBytes(J(old2, L"PathOfBuildingCommunity\\Settings.xml"), SettingsXml(J(c2, L"old_custom")));
		WriteFileBytes(J(old2, L"PathOfBuildingCommunity\\Builds\\decoy.xml"), "DECOY");
		WriteFileBytes(J(c2, L"old_custom\\real.xml"), "REAL");
		ImportBuildsFromTool(old2, ours2);
		check(u8"I11g 來源的自訂路徑被讀取而非預設 Builds",
		      Content(J(ours2, std::wstring(kP1) + L"\\Builds\\real.xml")) == "REAL" &&
		      !Exists(J(ours2, std::wstring(kP1) + L"\\Builds\\decoy.xml")), "");
	}

	// ===== I12 來源不被更動 =====
	{
		// 另開沙盒重做 I4 情境,並一併檢查 I4 的來源(匯入前快照於 I4 內取得)
		std::wstring c = Case(L"i12");
		std::wstring src = J(c, L"src"), dst = J(c, L"dst");
		WriteFileBytes(J(src, L"a.xml"), "NEW");
		WriteFileBytes(J(src, L"sub\\z.xml"), "Z");
		WriteFileBytes(J(dst, L"a.xml"), "OLD");
		Snap s0 = TakeSnap(src);
		ImportBuildTree(src, dst);
		Snap s1 = TakeSnap(src);
		check(u8"I12a 匯入前後來源的相對路徑與內容逐檔相同(含衝突情境)", s0 == s1 && s0.files.size() == 2, "n=" + std::to_string(s1.files.size()));
		check(u8"I12b 沿用 I4 來源也未被更動", TakeSnap(i4src) == i4srcBefore && i4srcBefore.files.size() == 1, "");
	}

	// ===== D0 / D1 ParsePobRelease =====
	const std::string hA(64, 'a');
	const std::string hB = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	const std::string kZip1 = "PathOfBuildingCommunity-Portable.zip";
	const std::string kZip2 = "PathOfBuildingCommunity-PoE2-Portable.zip";
	const std::string kSetup = "PathOfBuildingCommunity-Setup.exe";
	auto parse = [&](const std::string& json, bool poe2, PobRelease& out, std::string& err) {
		out = PobRelease();
		err.clear();
		return ParsePobRelease(json, poe2, out, &err);
	};
	{
		check(u8"D0 PobPortableDirName 回傳兩代的固定資料夾名",
		      wcscmp(PobPortableDirName(false), kP1) == 0 && wcscmp(PobPortableDirName(true), kP2) == 0, "");

		std::string json = Release(Asset(kSetup, "https://example.invalid/Setup.exe", 111, "sha256:" + hA) + "," +
		                           Asset(kZip1, "https://example.invalid/dl/" + kZip1, 12345678, "sha256:" + hB));
		PobRelease r;
		std::string err;
		bool ok = parse(json, false, r, err);
		check(u8"D1a PoE1 解析只取確切檔名的 zip(tag/url/size/sha256)",
		      ok && r.tag == "v2.67.2" && r.url == "https://example.invalid/dl/" + kZip1 && r.size == 12345678ull && r.sha256 == hB,
		      "tag=" + r.tag + " url=" + r.url + " sha=" + r.sha256);
		check(u8"D1b sha256 不含 sha256: 前綴", ok && r.sha256.size() == 64 && r.sha256.find(':') == std::string::npos, r.sha256);

		bool ok2 = parse(json, true, r, err);
		check(u8"D1c 同一份 PoE1 JSON 以 poe2=true 解析失敗且有 err", !ok2 && !err.empty(), err);

		bool ok3 = parse(Release(Asset(kSetup, "https://example.invalid/Setup.exe", 111, "sha256:" + hA)), false, r, err);
		check(u8"D1d 只有 Setup.exe 時失敗且有 err", !ok3 && !err.empty(), err);

		bool ok4 = parse(Release(Asset(kZip1, "https://example.invalid/z.zip", 5, "")), false, r, err);
		check(u8"D1e 缺 digest 時失敗且有 err", !ok4 && !err.empty(), err);

		bool ok5 = parse(Release(Asset(kZip1, "", 5, "sha256:" + hA)), false, r, err);
		check(u8"D1f 缺下載網址時失敗且有 err", !ok5 && !err.empty(), err);

		bool ok6 = parse("{not json", false, r, err);
		bool ok7 = parse("", false, r, err);
		check(u8"D1g 壞 JSON 與空字串都失敗且有 err", !ok6 && !ok7 && !err.empty(), err);

		std::string json2 = Release(Asset(kZip1, "https://example.invalid/p1.zip", 1, "sha256:" + hA) + "," +
		                            Asset(kZip2, "https://example.invalid/p2.zip", 2, "sha256:" + hB));
		bool ok8 = parse(json2, true, r, err);
		check(u8"D1h PoE2 樣本以 poe2=true 取到 PoE2 zip", ok8 && r.url == "https://example.invalid/p2.zip" && r.size == 2 && r.sha256 == hB,
		      "url=" + r.url);
		bool ok9 = parse(json2, false, r, err);
		check(u8"D1i 同份兩代樣本以 poe2=false 取到 PoE1 zip", ok9 && r.url == "https://example.invalid/p1.zip" && r.sha256 == hA, "url=" + r.url);

		bool ok10 = parse(Release(Asset(kZip2, "https://example.invalid/p2.zip", 2, "sha256:" + hB)), false, r, err);
		check(u8"D1j 只有 PoE2 zip 時 poe2=false 失敗(不可用子字串比對)", !ok10 && !err.empty(), err);

		bool ok11 = parse(Release(Asset("Not" + kZip1, "https://example.invalid/n.zip", 2, "sha256:" + hB)), false, r, err);
		bool ok12 = parse(Release(Asset(kZip1 + ".sig", "https://example.invalid/n.sig", 2, "sha256:" + hB)), false, r, err);
		check(u8"D1k 檔名前後多字(NotXxx.zip、Xxx.zip.sig)都不接受", !ok11 && !ok12, "");

		bool ok13 = parse(Release(Asset(kZip1, "https://example.invalid/z.zip", 5, "sha1:" + std::string(40, 'b'))), false, r, err);
		check(u8"D1l digest 不是 sha256 演算法時失敗", !ok13 && !err.empty(), err);
	}

	// ===== D2 雜湊不符就放棄 =====
	{
		std::string zip = MakeZip(PobEntries(""));
		check(u8"D2a 測試用 zip 建立成功", !zip.empty(), "size=" + std::to_string(zip.size()));
		std::wstring c = Case(L"d2");
		std::string err;
		bool ok1 = InstallPobZip(zip.data(), zip.size(), std::string(64, '0'), c, false, &err);
		std::string e1 = err;
		err.clear();
		bool ok2 = InstallPobZip(zip.data(), zip.size(), "", c, false, &err);
		std::string e2 = err;
		Snap s = TakeSnap(c);
		check(u8"D2b 錯的 sha256 回 false 且有 err", !ok1 && !e1.empty(), e1);
		check(u8"D2c 空字串 sha256 回 false 且有 err", !ok2 && !e2.empty(), e2);
		check(u8"D2d 失敗後 exeDir 沒有安裝資料夾也沒有暫存", s.files.empty() && s.dirs.empty(),
		      "dirs=" + std::to_string(s.dirs.size()) + " files=" + std::to_string(s.files.size()));

		// 對照:正確雜湊(大寫也可)才會裝起來,證明前面失敗是雜湊造成
		std::string up = Sha(zip);
		for (char& ch : up) ch = (char)toupper((unsigned char)ch);
		bool okU = InstallPobZip(zip.data(), zip.size(), up, c, false, &err);
		check(u8"D2e 正確雜湊(大寫)可安裝", okU && Exists(J(c, std::wstring(kP1) + L"\\Launch.lua")), err);
	}

	// ===== D3 安裝後偵測得到 =====
	for (int g = 0; g < 2; g++) {
		bool poe2 = g == 1;
		const wchar_t* name = poe2 ? kP2 : kP1;
		std::wstring c = Case(poe2 ? L"d3_poe2" : L"d3_poe1");
		std::string zip = MakeZip(PobEntries(""));
		std::string err;
		// poe1 傳尾端反斜線、poe2 不傳,兩種寫法都要可用
		std::wstring arg = poe2 ? c : c + L"\\";
		bool ok = InstallPobZip(zip.data(), zip.size(), Sha(zip), arg, poe2, &err);
		InstallInfo info = DetectInstalls(c + L"\\");
		const std::wstring& lua = poe2 ? info.poe2Lua : info.poe1Lua;
		const std::wstring& other = poe2 ? info.poe1Lua : info.poe2Lua;
		std::wstring want = c + L"\\" + name + L"\\Launch.lua";
		const char* t1 = poe2 ? u8"D3b PoE2 安裝後 DetectInstalls 取得位於安裝資料夾的 poe2Lua" : u8"D3a PoE1 安裝後 DetectInstalls 取得位於安裝資料夾的 poe1Lua";
		check(t1, ok && !lua.empty() && _wcsicmp(lua.c_str(), want.c_str()) == 0, err + " lua=" + Narrow(lua));
		check(poe2 ? u8"D3c PoE2 安裝不會被誤判成 PoE1" : u8"D3d PoE1 安裝不會被誤判成 PoE2", other.empty(), Narrow(other));
		check(poe2 ? u8"D3e PoE2 zip 內子資料夾的檔一併解出" : u8"D3f PoE1 zip 內子資料夾的檔一併解出",
		      Content(J(c, std::wstring(name) + L"\\Data\\x.lua")) == "return 1" &&
		      Content(J(c, std::wstring(name) + L"\\Launch.lua")) == "-- launch", "");
	}

	// ===== D4 zip 自帶頂層資料夾也可 =====
	{
		std::wstring c = Case(L"d4a");
		std::string zip = MakeZip(PobEntries("PathOfBuildingCommunity-Portable/"));
		std::string err;
		bool ok = InstallPobZip(zip.data(), zip.size(), Sha(zip), c, false, &err);
		std::wstring inst = J(c, kP1);
		check(u8"D4a 頂層同名資料夾:Launch.lua 直接在安裝資料夾第一層", ok && Content(J(inst, L"Launch.lua")) == "-- launch", err);
		check(u8"D4b 頂層同名資料夾:沒有巢狀兩層", !Exists(J(inst, kP1)) && Content(J(inst, L"Data\\x.lua")) == "return 1", "");

		std::wstring c2 = Case(L"d4b");
		std::string zip2 = MakeZip(PobEntries("PoE2-Release-2.0/"));
		bool ok2 = InstallPobZip(zip2.data(), zip2.size(), Sha(zip2), c2, true, &err);
		std::wstring inst2 = J(c2, kP2);
		check(u8"D4c 頂層為任意名稱的資料夾(PoE2)也拆開到第一層",
		      ok2 && Content(J(inst2, L"Launch.lua")) == "-- launch" && !Exists(J(inst2, L"PoE2-Release-2.0")) &&
		      !Exists(J(c2, L"PoE2-Release-2.0")), err);
	}

	// ===== D5 失敗不留半套 =====
	{
		// exeDir 先放一些無關檔案,「完全相同」才有意義
		auto prep = [&](const wchar_t* nm) {
			std::wstring c = Case(nm);
			WriteFileBytes(J(c, L"pob-zh.exe"), "EXE");
			WriteFileBytes(J(c, L"PobTools\\x.ini"), "ini");
			return c;
		};
		std::string err;

		std::wstring c1 = prep(L"d5a");
		std::string zipNoLua = MakeZip({ { "readme.txt", "no launch" }, { "Data/x.lua", "return 1" } });
		Snap b1 = TakeSnap(c1);
		bool ok1 = InstallPobZip(zipNoLua.data(), zipNoLua.size(), Sha(zipNoLua), c1, false, &err);
		check(u8"D5a zip 內沒有 Launch.lua 時回 false 且有 err", !ok1 && !err.empty(), err);
		check(u8"D5b 無 Launch.lua 失敗後 exeDir 內容與呼叫前完全相同(無安裝資料夾與暫存)", TakeSnap(c1) == b1, "");

		std::wstring c2 = prep(L"d5b");
		std::string junk = "this is definitely not a zip archive";
		Snap b2 = TakeSnap(c2);
		err.clear();
		bool ok2 = InstallPobZip(junk.data(), junk.size(), Sha(junk), c2, true, &err);
		check(u8"D5c 位元組不是 zip(sha 正確)時回 false 且有 err", !ok2 && !err.empty(), err);
		check(u8"D5d 非 zip 失敗後 exeDir 內容與呼叫前完全相同", TakeSnap(c2) == b2, "");

		// 對照:同樣的 exeDir 放入有 Launch.lua 的 zip 會成功,證明 D5a 失敗是缺 Launch.lua 造成
		std::string good = MakeZip(PobEntries(""));
		err.clear();
		bool ok3 = InstallPobZip(good.data(), good.size(), Sha(good), c1, false, &err);
		check(u8"D5e 對照:補上 Launch.lua 的 zip 可安裝到同一個 exeDir", ok3 && Exists(J(c1, std::wstring(kP1) + L"\\Launch.lua")), err);
	}

	// ===== D6 已有安裝時不覆蓋 =====
	{
		std::wstring c = Case(L"d6");
		std::wstring inst = J(c, kP1);
		WriteFileBytes(J(inst, L"marker.txt"), "KEEP");
		std::string zip = MakeZip(PobEntries(""));
		Snap before = TakeSnap(c);
		std::string err;
		bool ok = InstallPobZip(zip.data(), zip.size(), Sha(zip), c, false, &err);
		Snap after = TakeSnap(c);
		check(u8"D6a 目的資料夾已存在時回 false 且有 err", !ok && !err.empty(), err);
		check(u8"D6b 標記檔內容不變", Content(J(inst, L"marker.txt")) == "KEEP", "");
		check(u8"D6c 安裝資料夾沒有多檔且 exeDir 沒有暫存", after == before && !Exists(J(inst, L"Launch.lua")),
		      "files=" + std::to_string(after.files.size()));
	}

	report += failures ? "RESULT FAIL\n" : "RESULT PASS\n";
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\migrate_selftest.txt").c_str(), GENERIC_WRITE, 0, nullptr,
	                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, report.data(), (DWORD)report.size(), &w, nullptr);
		CloseHandle(h);
	}
	RmTree(root);
	return failures;
}
