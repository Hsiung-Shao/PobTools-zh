#include "icon_manager.h"
#include "image_tex.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>

#include <json.hpp> // nlohmann::ordered_json
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

using nlohmann::ordered_json;

// ---- helpers ---------------------------------------------------------------

static std::wstring widen(const std::string& s)
{
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
	return w;
}

static bool read_file_bytes(const std::wstring& path, std::vector<unsigned char>& out)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size{};
	bool ok = false;
	if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (1ll << 28)) {
		out.resize((size_t)size.QuadPart);
		DWORD rd = 0;
		ok = ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr) && rd == out.size();
		if (!ok) out.clear();
	}
	CloseHandle(h);
	return ok;
}

static void write_file_bytes(const std::wstring& path, const std::vector<unsigned char>& data)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD wr = 0;
	if (!data.empty()) WriteFile(h, data.data(), (DWORD)data.size(), &wr, nullptr);
	CloseHandle(h);
}

// art path "Art/2DItems/Currency/CurrencyRerollRare" -> cache filename component.
static std::wstring sanitize(const std::string& path)
{
	std::wstring w = widen(path);
	for (wchar_t& c : w)
		if (c == L'/' || c == L'\\' || c == L':' || c == L'*' || c == L'?' || c == L'"' ||
		    c == L'<' || c == L'>' || c == L'|') c = L'_';
	return w;
}

// HTTPS GET web.poecdn.com<path> via an existing connection handle. 200 only.
static bool HttpGet(HINTERNET hConnect, const std::wstring& path, std::vector<unsigned char>& out)
{
	if (!hConnect) return false;
	HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr,
		WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
	if (!hReq) return false;

	bool ok = false;
	if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
		WinHttpReceiveResponse(hReq, nullptr)) {
		DWORD code = 0, len = sizeof(code);
		WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
		if (code == 200) {
			DWORD avail = 0;
			do {
				avail = 0;
				if (!WinHttpQueryDataAvailable(hReq, &avail)) break;
				if (avail == 0) break;
				size_t off = out.size();
				out.resize(off + avail);
				DWORD rd = 0;
				if (!WinHttpReadData(hReq, out.data() + off, avail, &rd)) { out.resize(off); break; }
				out.resize(off + rd);
			} while (avail > 0);
			ok = !out.empty();
		}
	}
	WinHttpCloseHandle(hReq);
	return ok;
}

// ---- IconManager -----------------------------------------------------------

void IconManager::Init(const std::wstring& exeDir)
{
	// Load the bundled name -> art-path index.
	std::vector<unsigned char> idx;
	if (read_file_bytes(exeDir + L"Data\\icon_paths.json", idx)) {
		try {
			ordered_json doc = ordered_json::parse(std::string((const char*)idx.data(), idx.size()));
			if (doc.is_object())
				for (auto& [k, v] : doc.items())
					if (v.is_string()) paths_.emplace(k, v.get<std::string>());
		} catch (...) {}
	}
	if (paths_.empty()) return; // no index -> icons disabled (graceful)

	cacheDir_ = exeDir + L"PobTools\\cache\\icons\\";
	SHCreateDirectoryExW(nullptr, cacheDir_.c_str(), nullptr); // best-effort

	worker_ = std::thread(&IconManager::workerLoop, this);
}

void IconManager::Shutdown()
{
	if (worker_.joinable()) {
		{ std::lock_guard<std::mutex> lk(reqMx_); stop_.store(true); }
		reqCv_.notify_all();
		worker_.join();
	}
	// Drain any leftover decoded buffers.
	{
		std::lock_guard<std::mutex> lk(doneMx_);
		for (Done& d : doneQ_) FreeDecoded(d.rgba);
		doneQ_.clear();
	}
	for (auto& [name, t] : tex_) DeleteTexture(t);
	tex_.clear();
}

void IconManager::Request(const std::string& name)
{
	if (name.empty() || requested_.count(name)) return;
	if (paths_.find(name) == paths_.end()) { requested_.insert(name); return; } // no icon for this name
	requested_.insert(name);
	{ std::lock_guard<std::mutex> lk(reqMx_); reqQ_.push_back(name); }
	reqCv_.notify_one();
}

unsigned IconManager::Texture(const std::string& name)
{
	auto it = tex_.find(name);
	return it != tex_.end() ? it->second : 0;
}

void IconManager::Pump()
{
	// Upload a bounded number of finished downloads per frame (GL thread).
	for (int n = 0; n < 16; n++) {
		Done d;
		{
			std::lock_guard<std::mutex> lk(doneMx_);
			if (doneQ_.empty()) break;
			d = doneQ_.front();
			doneQ_.pop_front();
		}
		unsigned t = CreateTextureRGBA(d.rgba, d.w, d.h);
		FreeDecoded(d.rgba);
		tex_[d.name] = t; // even 0 (failed) is recorded so we don't retry
	}
}

void IconManager::workerLoop()
{
	HINTERNET hSession = WinHttpOpen(L"PobTools/1.0 (POE filter editor)",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
	HINTERNET hConnect = hSession ? WinHttpConnect(hSession, L"web.poecdn.com",
		INTERNET_DEFAULT_HTTPS_PORT, 0) : nullptr;

	for (;;) {
		std::string name;
		{
			std::unique_lock<std::mutex> lk(reqMx_);
			reqCv_.wait(lk, [&] { return stop_.load() || !reqQ_.empty(); });
			if (stop_.load()) break;
			name = reqQ_.front();
			reqQ_.pop_front();
		}

		auto it = paths_.find(name);
		if (it == paths_.end()) continue;
		const std::string& artPath = it->second;

		std::wstring cacheFile = cacheDir_ + sanitize(artPath) + L".png";
		std::vector<unsigned char> bytes;
		if (!read_file_bytes(cacheFile, bytes)) {
			std::wstring urlPath = L"/image/" + widen(artPath) + L".png";
			if (HttpGet(hConnect, urlPath, bytes))
				write_file_bytes(cacheFile, bytes);
		}
		if (bytes.empty()) continue;

		int w = 0, h = 0;
		unsigned char* rgba = DecodeImageRGBA(bytes.data(), (int)bytes.size(), &w, &h);
		if (!rgba) continue;
		{
			std::lock_guard<std::mutex> lk(doneMx_);
			doneQ_.push_back(Done{ name, w, h, rgba });
		}
	}

	if (hConnect) WinHttpCloseHandle(hConnect);
	if (hSession) WinHttpCloseHandle(hSession);
}
