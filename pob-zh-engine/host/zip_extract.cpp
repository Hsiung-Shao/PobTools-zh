#include "zip_extract.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <miniz.h>

#pragma comment(lib, "shell32.lib")

static std::wstring widen(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

// Normalized name ('/' separators) -> safe? Rejects absolute paths, drive
// letters and any "."/".." path component.
static bool entry_name_safe(const std::string& name)
{
	if (name.empty() || name[0] == '/') return false;
	if (name.find(':') != std::string::npos) return false;
	size_t start = 0;
	while (start <= name.size()) {
		size_t slash = name.find('/', start);
		size_t len = (slash == std::string::npos ? name.size() : slash) - start;
		if (len > 0) {
			const char* c = name.c_str() + start;
			if ((len == 1 && c[0] == '.') || (len == 2 && c[0] == '.' && c[1] == '.'))
				return false;
		}
		if (slash == std::string::npos) break;
		start = slash + 1;
	}
	return true;
}

static bool write_file_bytes(const std::wstring& path, const void* data, size_t size)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	bool ok = size == 0 ||
		(WriteFile(h, data, (DWORD)size, &written, nullptr) && written == (DWORD)size);
	CloseHandle(h);
	return ok;
}

bool ExtractZipToDir(const void* data, size_t size, const std::wstring& destDir,
                     std::string* err, int* filesOut)
{
	if (filesOut) *filesOut = 0;
	std::wstring base = destDir;
	if (!base.empty() && base.back() != L'\\') base += L'\\';
	if (SHCreateDirectoryExW(nullptr, base.c_str(), nullptr) != ERROR_SUCCESS &&
	    GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) {
		if (err) *err = u8"無法建立解壓目錄";
		return false;
	}

	mz_zip_archive za{};
	if (!mz_zip_reader_init_mem(&za, data, size, 0)) {
		if (err) *err = u8"zip 檔格式無效";
		return false;
	}

	bool ok = true;
	int files = 0;
	mz_uint count = mz_zip_reader_get_num_files(&za);
	for (mz_uint i = 0; i < count && ok; i++) {
		mz_zip_archive_file_stat st{};
		if (!mz_zip_reader_file_stat(&za, i, &st)) {
			if (err) *err = u8"zip 條目資訊讀取失敗";
			ok = false;
			break;
		}
		// Tolerate PowerShell Compress-Archive output: '\' as separator.
		std::string name = st.m_filename;
		for (char& c : name) if (c == '\\') c = '/';
		if (name.empty()) continue;
		if (!entry_name_safe(name)) {
			if (err) *err = u8"zip 條目路徑非法: " + name;
			ok = false;
			break;
		}

		std::wstring rel = widen(name);
		for (wchar_t& c : rel) if (c == L'/') c = L'\\';

		bool isDir = name.back() == '/' || mz_zip_reader_is_file_a_directory(&za, i);
		if (isDir) {
			SHCreateDirectoryExW(nullptr, (base + rel).c_str(), nullptr);
			continue;
		}

		size_t bs = rel.find_last_of(L'\\');
		if (bs != std::wstring::npos)
			SHCreateDirectoryExW(nullptr, (base + rel.substr(0, bs)).c_str(), nullptr);

		// extract_to_heap + CreateFileW: miniz's own extract_to_file goes
		// through fopen(char*) and breaks on non-ASCII install paths.
		size_t outSize = 0;
		void* p = mz_zip_reader_extract_to_heap(&za, i, &outSize, 0);
		if (!p) {
			if (err) *err = u8"zip 條目解壓失敗: " + name;
			ok = false;
			break;
		}
		if (!write_file_bytes(base + rel, p, outSize)) {
			if (err) *err = u8"檔案寫入失敗: " + name;
			ok = false;
		}
		mz_free(p);
		files++;
	}

	mz_zip_reader_end(&za);
	if (ok && filesOut) *filesOut = files;
	return ok;
}
