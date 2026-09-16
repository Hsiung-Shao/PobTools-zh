#include "bridge_gate.h"

#include <json.hpp>
#include <windows.h>
#include <ctime>

using json = nlohmann::json;

namespace BridgeGate {

static std::string narrow_utf8(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

static std::wstring widen_utf8(const std::string& s)
{
	if (s.empty()) return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
	return w;
}

static std::wstring lower(std::wstring s)
{
	for (auto& c : s) c = (wchar_t)towlower(c);
	while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
	for (auto& c : s) if (c == L'/') c = L'\\';
	return s;
}

std::wstring GatePath(const std::wstring& exeDir)
{
	return exeDir + L"PobTools\\bridge_gate.json";
}

Verdict Parse(const std::string& text)
{
	Verdict v;
	json j;
	try {
		j = json::parse(text);
	} catch (...) {
		return v;
	}
	if (!j.is_object()) return v;
	v.present = true;
	v.ok = j.value("ok", true);
	if (j.contains("failed") && j["failed"].is_array())
		for (auto& f : j["failed"]) if (f.is_string()) v.failed.push_back(f.get<std::string>());
	v.pobVersion = j.value("pobVersion", "");
	v.pobBranch = j.value("pobBranch", "");
	v.pobDir = widen_utf8(j.value("pobDir", ""));
	v.checkedAtUtc = j.value("checkedAtUtc", "");
	return v;
}

bool Write(const std::wstring& exeDir, const Verdict& v)
{
	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	std::string stamp = v.checkedAtUtc;
	if (stamp.empty()) {
		time_t now = time(nullptr);
		tm t{};
		gmtime_s(&t, &now);
		char buf[32];
		strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
		stamp = buf;
	}
	json j{
		{"ok", v.ok},
		{"failed", v.failed},
		{"pobVersion", v.pobVersion},
		{"pobBranch", v.pobBranch},
		{"pobDir", narrow_utf8(v.pobDir)},
		{"checkedAtUtc", stamp},
	};
	std::string text = j.dump(2);
	const std::wstring path = GatePath(exeDir);
	const std::wstring tmp = path + L".new";
	HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD written = 0;
	BOOL ok = WriteFile(h, text.data(), (DWORD)text.size(), &written, nullptr);
	CloseHandle(h);
	if (!ok || written != text.size()) { DeleteFileW(tmp.c_str()); return false; }
	return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

Verdict Read(const std::wstring& exeDir)
{
	HANDLE h = CreateFileW(GatePath(exeDir).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return {};
	std::string text;
	char buf[4096];
	DWORD read = 0;
	while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) text.append(buf, read);
	CloseHandle(h);
	return Parse(text);
}

bool BlocksModernUi(const Verdict& v, const std::wstring& pobDir, const std::string& pobVersion)
{
	if (!v.present || v.ok) return false;
	if (v.pobVersion.empty() || pobVersion.empty() || v.pobVersion != pobVersion) return false;
	if (!v.pobDir.empty() && !pobDir.empty() && lower(v.pobDir) != lower(pobDir)) return false;
	return true;
}

} // namespace BridgeGate
