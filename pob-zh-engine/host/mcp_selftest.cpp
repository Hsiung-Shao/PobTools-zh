// --mcp-selftest: 啟動 `pob-zh.exe --mcp --pob <dir>`,用 stdio JSON-RPC 驗證
// docs/notes/mcp_contract.md 的契約(測項 M1-M11)。
//
// 只依契約寫成,不看 MCP 實作。測試不碰 --pob 資料夾:M9 在前後各取一次檔案清單比對。

#include "mcp_selftest.h"

#include "launcher_config.h"
#include "mcp_config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace {

// ---------------------------------------------------------------- 報告

struct Res { bool ok = false; std::string detail; };
std::map<int, Res> g_res;

const char* const kTitles[22] = {
	"",
	"握手回傳協定版本與工具清單",
	"stdout 只出現 JSON-RPC",
	"讀入範例建置後數值等於 POB 存檔值",
	"分享碼來回不失真",
	"what_if 回傳差異且自動還原",
	"suggest_nodes 結果合理",
	"check_build 抓到抗性不足",
	"名稱英中並列",
	"不寫回使用者資料夾",
	"子行程掛掉後自動重開",
	"參數錯誤回傳可讀的錯誤",
	"設定字串帶入正確路徑",
	"equip_item 會取代已有裝備的欄位",
	"suggest_nodes 帶出路徑點數",
	"點他昇華的節點會報錯且建置不變",
	"new_build 職業打錯時保留原建置與名稱",
	"build_summary 帶出已點基石",
	"免疫混沌時不警告混沌抗性",
	"build_guide 回傳判讀守則",
	"build_summary 帶出迴避機率",
	"寫進未啟用的技能組會警告",
};

void Record(int n, bool ok, const std::string& detail) { g_res[n] = Res{ok, detail}; }

// ---------------------------------------------------------------- 小工具

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s(n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

bool IsDir(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileExists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string ReadFileA(const std::wstring& path)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return {};
	std::string out;
	char buf[65536];
	DWORD got = 0;
	while (ReadFile(h, buf, sizeof(buf), &got, nullptr) && got > 0) out.append(buf, got);
	CloseHandle(h);
	return out;
}

bool WriteFileA(const std::wstring& path, const std::string& data)
{
	HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	WriteFile(h, data.data(), (DWORD)data.size(), &w, nullptr);
	CloseHandle(h);
	return w == data.size();
}

std::string Cut(const std::string& s, size_t n)
{
	return s.size() <= n ? s : s.substr(0, n) + "...";
}

std::string Num(double v)
{
	char buf[64];
	snprintf(buf, sizeof(buf), "%.10g", v);
	return buf;
}

// structuredContent.error 轉成字串(不是字串就回空)。
std::string ErrText(const json& sc)
{
	return sc.is_object() && sc.contains("error") && sc["error"].is_string() ? sc["error"].get<std::string>() : std::string();
}

std::string Lower(std::string s)
{
	for (auto& c : s) c = (char)tolower((unsigned char)c);
	return s;
}

bool NonEmptyStr(const json& j, const char* key)
{
	return j.is_object() && j.contains(key) && j[key].is_string() && !j[key].get<std::string>().empty();
}

// ---------------------------------------------------------------- MCP 子行程

struct ToolRes {
	bool transportOk = false;   // 有收到對應的 JSON-RPC 回應
	std::string transportErr;
	bool rpcError = false;      // 回應是 JSON-RPC error
	int rpcCode = 0;
	std::string rpcMsg;
	bool isError = false;       // result.isError
	json result;
	json sc = json::object();   // structuredContent(沒有就解 content[0].text)

	bool Ok() const { return transportOk && !rpcError && !isError; }
	std::string Why() const
	{
		if (!transportOk) return transportErr;
		if (rpcError) return "JSON-RPC error " + std::to_string(rpcCode) + " " + rpcMsg;
		if (sc.is_object() && sc.contains("error")) {
			return sc["error"].is_string() ? sc["error"].get<std::string>() : sc["error"].dump();
		}
		return Cut(sc.dump(), 200);
	}
};

class McpProc {
public:
	~McpProc() { Close(); }

	bool Start(const std::wstring& exeDir, const std::wstring& pobDir, std::string& err)
	{
		const std::wstring exe = exeDir + L"pob-zh.exe";
		if (!FileExists(exe)) { err = "找不到 " + narrow(exe); return false; }

		SECURITY_ATTRIBUTES sa{};
		sa.nLength = sizeof(sa);
		sa.bInheritHandle = TRUE;
		HANDLE inR = nullptr, inW = nullptr, outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
		if (!CreatePipe(&inR, &inW, &sa, 0) || !CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
			err = "建立管線失敗";
			return false;
		}
		// 父端的那頭不給子行程繼承。
		SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

		std::wstring cmd = L"\"" + exe + L"\" --mcp --pob \"" + pobDir + L"\"";
		std::vector<wchar_t> buf(cmd.begin(), cmd.end());
		buf.push_back(0);
		STARTUPINFOW si{};
		si.cb = sizeof(si);
		si.dwFlags = STARTF_USESTDHANDLES;
		si.hStdInput = inR;
		si.hStdOutput = outW;
		si.hStdError = errW;
		PROCESS_INFORMATION pi{};
		BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
		                         exeDir.c_str(), &si, &pi);
		DWORD le = GetLastError();
		CloseHandle(inR);
		CloseHandle(outW);
		CloseHandle(errW);
		if (!ok) {
			CloseHandle(inW);
			CloseHandle(outR);
			CloseHandle(errR);
			err = "CreateProcess 失敗 (" + std::to_string(le) + ")";
			return false;
		}
		CloseHandle(pi.hThread);
		hProc_ = pi.hProcess;
		hIn_ = inW;
		hOut_ = outR;
		hErr_ = errR;
		tOut_ = std::thread([this] { ReadOut(); });
		tErr_ = std::thread([this] { ReadErr(); });
		return true;
	}

	bool Alive() const { return hProc_ && WaitForSingleObject(hProc_, 0) == WAIT_TIMEOUT; }

	// 先關 stdin,等 5 秒,還沒結束就 TerminateProcess。
	void Close()
	{
		if (hIn_) { CloseHandle(hIn_); hIn_ = nullptr; }
		if (hProc_) {
			if (WaitForSingleObject(hProc_, 5000) != WAIT_OBJECT_0) {
				TerminateProcess(hProc_, 1);
				terminated_ = true;
				WaitForSingleObject(hProc_, 3000);
			}
			DWORD code = 0;
			if (GetExitCodeProcess(hProc_, &code)) exitCode_ = code;
			CloseHandle(hProc_);
			hProc_ = nullptr;
		}
		JoinReader(tOut_, outDone_);
		JoinReader(tErr_, errDone_);
		if (hOut_) { CloseHandle(hOut_); hOut_ = nullptr; }
		if (hErr_) { CloseHandle(hErr_); hErr_ = nullptr; }
	}

	bool Terminated() const { return terminated_; }
	unsigned long ExitCode() const { return exitCode_; }

	std::vector<std::string> AllStdoutLines()
	{
		std::lock_guard<std::mutex> lk(mu_);
		return all_;
	}

	std::string StderrTail()
	{
		std::lock_guard<std::mutex> lk(mu_);
		return errTail_;
	}

	bool SendRaw(const std::string& line)
	{
		if (!hIn_) return false;
		std::string data = line + "\n";
		DWORD w = 0;
		return WriteFile(hIn_, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
	}

	void Notify(const std::string& method)
	{
		json m = {{"jsonrpc", "2.0"}, {"method", method}};
		SendRaw(m.dump());
	}

	// 送一個請求,讀到 id 相符的回應為止。其餘行(通知、別的 id)略過。
	bool Request(const std::string& method, const json& params, unsigned timeoutMs, json& resp, std::string& err)
	{
		const int id = nextId_++;
		json m = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
		if (!SendRaw(m.dump(-1, ' ', false, json::error_handler_t::replace))) {
			err = "寫入 stdin 失敗(子行程可能已結束)";
			return false;
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		std::unique_lock<std::mutex> lk(mu_);
		for (;;) {
			while (!q_.empty()) {
				std::string ln = std::move(q_.front());
				q_.pop_front();
				json j = json::parse(ln, nullptr, false);
				if (j.is_discarded() || !j.is_object()) continue;
				if (j.contains("method")) continue;
				if (j.contains("id") && j["id"].is_number_integer() && j["id"].get<int>() == id) {
					resp = std::move(j);
					return true;
				}
			}
			if (eof_) {
				err = "子行程已結束,沒有收到回應 (exit " + std::to_string(Alive() ? 0ul : ExitCodeNow()) + ")";
				return false;
			}
			if (cv_.wait_until(lk, deadline) == std::cv_status::timeout && q_.empty()) {
				err = "逾時 " + std::to_string(timeoutMs / 1000) + " 秒 (" + method + ")";
				return false;
			}
		}
	}

	ToolRes Call(const std::string& name, const json& args, unsigned timeoutMs = 30000)
	{
		ToolRes r;
		json resp;
		if (!Request("tools/call", {{"name", name}, {"arguments", args}}, timeoutMs, resp, r.transportErr)) return r;
		r.transportOk = true;
		if (resp.contains("error") && resp["error"].is_object()) {
			r.rpcError = true;
			r.rpcCode = resp["error"].value("code", 0);
			r.rpcMsg = resp["error"].value("message", std::string());
			return r;
		}
		r.result = resp.value("result", json::object());
		r.isError = r.result.is_object() && r.result.value("isError", false);
		if (r.result.is_object() && r.result.contains("structuredContent") && r.result["structuredContent"].is_object()) {
			r.sc = r.result["structuredContent"];
		} else if (r.result.is_object() && r.result.contains("content") && r.result["content"].is_array() &&
		           !r.result["content"].empty() && r.result["content"][0].contains("text") &&
		           r.result["content"][0]["text"].is_string()) {
			json t = json::parse(r.result["content"][0]["text"].get<std::string>(), nullptr, false);
			if (t.is_object()) r.sc = std::move(t);
		}
		return r;
	}

private:
	unsigned long ExitCodeNow()
	{
		DWORD c = 0;
		if (hProc_ && GetExitCodeProcess(hProc_, &c)) return c;
		return exitCode_;
	}

	void ReadOut()
	{
		std::string cur;
		char buf[65536];
		DWORD got = 0;
		while (ReadFile(hOut_, buf, sizeof(buf), &got, nullptr) && got > 0) {
			for (DWORD i = 0; i < got; i++) {
				if (buf[i] == '\n') {
					if (!cur.empty() && cur.back() == '\r') cur.pop_back();
					{
						std::lock_guard<std::mutex> lk(mu_);
						all_.push_back(cur);
						q_.push_back(cur);
					}
					cv_.notify_all();
					cur.clear();
				} else {
					cur.push_back(buf[i]);
				}
			}
		}
		if (!cur.empty()) {
			std::lock_guard<std::mutex> lk(mu_);
			all_.push_back(cur);
			q_.push_back(cur);
		}
		{
			std::lock_guard<std::mutex> lk(mu_);
			eof_ = true;
		}
		cv_.notify_all();
		outDone_ = true;
	}

	void ReadErr()
	{
		char buf[4096];
		DWORD got = 0;
		while (ReadFile(hErr_, buf, sizeof(buf), &got, nullptr) && got > 0) {
			std::lock_guard<std::mutex> lk(mu_);
			errTail_.append(buf, got);
			if (errTail_.size() > 4096) errTail_.erase(0, errTail_.size() - 4096);
		}
		errDone_ = true;
	}

	// 子行程結束後讀取執行緒應該很快收到 EOF;若孫行程還抓著管線,就取消阻塞中的讀取。
	static void JoinReader(std::thread& t, std::atomic<bool>& done)
	{
		if (!t.joinable()) return;
		for (int i = 0; i < 60 && !done; i++) Sleep(50);
		if (!done) {
			CancelSynchronousIo(t.native_handle());
			for (int i = 0; i < 40 && !done; i++) Sleep(50);
		}
		t.join();
	}

	HANDLE hProc_ = nullptr, hIn_ = nullptr, hOut_ = nullptr, hErr_ = nullptr;
	std::thread tOut_, tErr_;
	std::atomic<bool> outDone_{false}, errDone_{false};
	std::mutex mu_;
	std::condition_variable cv_;
	std::deque<std::string> q_;
	std::vector<std::string> all_;
	std::string errTail_;
	bool eof_ = false;
	bool terminated_ = false;
	unsigned long exitCode_ = 0;
	int nextId_ = 1;
};

constexpr unsigned kSlowMs = 180000;

// ---------------------------------------------------------------- 契約資料

const char* const kContractTools[] = {
	"load_build", "list_my_builds", "new_build", "export_code", "build_summary", "stat_breakdown",
	"check_build", "tree_summary", "suggest_nodes", "search_nodes", "allocate_nodes", "deallocate_nodes",
	"list_skills", "search_gems", "set_skill_group", "list_items", "search_uniques", "list_bases",
	"equip_item", "unequip_item", "get_config", "set_config", "what_if", "build_guide",
};

// ---------------------------------------------------------------- 範例建置

std::vector<std::pair<std::string, std::string>> ParsePlayerStats(const std::string& xml)
{
	std::vector<std::pair<std::string, std::string>> out;
	size_t pos = 0;
	for (;;) {
		pos = xml.find("<PlayerStat ", pos);
		if (pos == std::string::npos) break;
		size_t end = xml.find('>', pos);
		if (end == std::string::npos) break;
		std::string tag = xml.substr(pos, end - pos);
		auto attr = [&](const char* name) -> std::string {
			std::string key = std::string(name) + "=\"";
			size_t a = tag.find(key);
			if (a == std::string::npos) return {};
			a += key.size();
			size_t b = tag.find('"', a);
			return b == std::string::npos ? std::string() : tag.substr(a, b - a);
		};
		std::string stat = attr("stat"), value = attr("value");
		if (!stat.empty()) out.emplace_back(stat, value);
		pos = end;
	}
	return out;
}

struct XmlCand { std::wstring rel; ULONGLONG mtime; };

void CollectXml(const std::wstring& dir, const std::wstring& rel, bool recurse, std::vector<XmlCand>& out)
{
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		const std::wstring n = fd.cFileName;
		if (n == L"." || n == L"..") continue;
		const std::wstring r = rel.empty() ? n : rel + L"\\" + n;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (recurse && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) CollectXml(dir + L"\\" + n, r, true, out);
		} else if (n.size() > 4 && _wcsicmp(n.c_str() + n.size() - 4, L".xml") == 0) {
			ULARGE_INTEGER u;
			u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
			u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
			out.push_back({r, u.QuadPart});
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

// 最新修改、且含 <PlayerStat 的 .xml。先找 Builds 頂層,沒有再往子資料夾找。回傳相對於 Builds 的檔名。
bool FindSampleBuild(const std::wstring& buildsDir, std::wstring& rel, std::string& xml)
{
	for (int pass = 0; pass < 2; pass++) {
		std::vector<XmlCand> c;
		CollectXml(buildsDir, L"", pass == 1, c);
		std::sort(c.begin(), c.end(), [](const XmlCand& a, const XmlCand& b) { return a.mtime > b.mtime; });
		for (const auto& x : c) {
			std::string data = ReadFileA(buildsDir + L"\\" + x.rel);
			if (data.find("<PlayerStat ") != std::string::npos) {
				rel = x.rel;
				xml = std::move(data);
				return true;
			}
		}
	}
	return false;
}

// ---------------------------------------------------------------- M9:資料夾快照

struct FileInfo { ULONGLONG size = 0; ULONGLONG mtime = 0; bool dir = false; };
using Snap = std::map<std::wstring, FileInfo>;

// 跳過 junction 與大型資料夾(TreeData、Data、TimelessJewelData)。
void Snapshot(const std::wstring& dir, const std::wstring& rel, Snap& out)
{
	WIN32_FIND_DATAW fd{};
	HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		const std::wstring n = fd.cFileName;
		if (n == L"." || n == L"..") continue;
		const std::wstring r = rel.empty() ? n : rel + L"\\" + n;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
			if (_wcsicmp(n.c_str(), L"TimelessJewelData") == 0) continue;
			if (rel.empty() && (_wcsicmp(n.c_str(), L"TreeData") == 0 || _wcsicmp(n.c_str(), L"Data") == 0)) continue;
			FileInfo fi;
			fi.dir = true;
			out[r] = fi;
			Snapshot(dir + L"\\" + n, r, out);
		} else {
			FileInfo fi;
			ULARGE_INTEGER s, t;
			s.LowPart = fd.nFileSizeLow;
			s.HighPart = fd.nFileSizeHigh;
			t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
			t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
			fi.size = s.QuadPart;
			fi.mtime = t.QuadPart;
			out[r] = fi;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

// ---------------------------------------------------------------- 共用查詢

bool GetStats(McpProc& s, json& stats, std::string& err)
{
	ToolRes r = s.Call("build_summary", json::object());
	if (!r.Ok()) { err = "build_summary 失敗: " + r.Why(); return false; }
	if (!r.sc.contains("stats") || !r.sc["stats"].is_object()) { err = "build_summary 沒有 stats 物件"; return false; }
	stats = r.sc["stats"];
	return true;
}

// 兩份 stats 不同的鍵(缺的視為 null)。
std::vector<std::string> StatDiffKeys(const json& a, const json& b)
{
	std::set<std::string> keys;
	for (auto it = a.begin(); it != a.end(); ++it) keys.insert(it.key());
	for (auto it = b.begin(); it != b.end(); ++it) keys.insert(it.key());
	std::vector<std::string> bad;
	for (const auto& k : keys) {
		json va = a.contains(k) ? a[k] : json(nullptr);
		json vb = b.contains(k) ? b[k] : json(nullptr);
		if (va != vb) bad.push_back(k + ":" + Cut(va.dump(), 24) + "->" + Cut(vb.dump(), 24));
	}
	return bad;
}

std::string JoinFirst(const std::vector<std::string>& v, size_t n)
{
	std::string out;
	for (size_t i = 0; i < v.size() && i < n; i++) out += (i ? ", " : "") + v[i];
	if (v.size() > n) out += ", ...";
	return out;
}

// ---------------------------------------------------------------- 測試本體

struct Ctx {
	McpProc* s = nullptr;
	std::string sample;     // 相對於 Builds 的檔名
	std::string sampleXml;  // 範例檔全文
	bool haveSuggest = false;
	ToolRes suggest;        // suggest_nodes offence top 5(M5、M6 共用)
};

bool LoadSample(Ctx& c, ToolRes& r)
{
	r = c.s->Call("load_build", {{"source", c.sample}}, kSlowMs);
	return r.Ok() && r.sc.value("ok", false);
}

void EnsureSuggest(Ctx& c)
{
	if (c.haveSuggest) return;
	c.suggest = c.s->Call("suggest_nodes", {{"metric", "offence"}, {"top", 5}}, kSlowMs);
	c.haveSuggest = true;
}

// M1 握手回傳協定版本與工具清單
void TestM1(Ctx& c)
{
	json resp;
	std::string err;
	json params = {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()},
	               {"clientInfo", {{"name", "pobtools-mcp-selftest"}, {"version", "1"}}}};
	if (!c.s->Request("initialize", params, 30000, resp, err)) { Record(1, false, "initialize: " + err); return; }
	if (!resp.contains("result") || !resp["result"].is_object()) { Record(1, false, "initialize 沒有 result: " + Cut(resp.dump(), 200)); return; }
	const std::string ver = resp["result"].value("protocolVersion", std::string());
	c.s->Notify("notifications/initialized");

	if (!c.s->Request("tools/list", json::object(), 30000, resp, err)) { Record(1, false, "tools/list: " + err); return; }
	if (!resp.contains("result") || !resp["result"].contains("tools") || !resp["result"]["tools"].is_array()) {
		Record(1, false, "tools/list 沒有 tools 陣列: " + Cut(resp.dump(), 200));
		return;
	}
	std::map<std::string, json> byName;
	for (const auto& t : resp["result"]["tools"]) {
		if (t.is_object() && t.contains("name") && t["name"].is_string()) byName[t["name"].get<std::string>()] = t;
	}
	std::vector<std::string> missing, noSchema;
	for (const char* name : kContractTools) {
		auto it = byName.find(name);
		if (it == byName.end()) { missing.push_back(name); continue; }
		if (!it->second.contains("inputSchema") || !it->second["inputSchema"].is_object()) noSchema.push_back(name);
	}
	const bool ok = !ver.empty() && missing.empty() && noSchema.empty();
	std::string d = "protocolVersion=" + ver + ", 工具 " + std::to_string(byName.size()) + " 個";
	if (!missing.empty()) d += ", 缺: " + JoinFirst(missing, 30);
	if (!noSchema.empty()) d += ", 沒有 inputSchema: " + JoinFirst(noSchema, 30);
	Record(1, ok, d);
}

// M3 讀入範例建置後數值等於 POB 存檔值
void TestM3(Ctx& c)
{
	ToolRes r;
	if (!LoadSample(c, r)) { Record(3, false, "load_build(" + c.sample + ") 失敗: " + r.Why()); return; }
	json stats;
	std::string err;
	if (!GetStats(*c.s, stats, err)) { Record(3, false, err); return; }
	const auto oracle = ParsePlayerStats(c.sampleXml);
	int compared = 0, bad = 0;
	std::vector<std::string> ex;
	for (const auto& [k, v] : oracle) {
		if (!stats.contains(k) || !stats[k].is_number()) continue;
		char* endp = nullptr;
		double x = strtod(v.c_str(), &endp);
		if (v.empty() || !endp || *endp != '\0') continue;
		double y = stats[k].get<double>();
		compared++;
		double diff = std::fabs(x - y);
		double scale = std::fmax(std::fabs(x), std::fabs(y));
		if (diff > 0.005 * scale && diff > 1e-9) {
			bad++;
			if (ex.size() < 5) ex.push_back(k + " 存檔=" + Num(x) + " 引擎=" + Num(y));
		}
	}
	const bool ok = compared > 0 && bad == 0;
	std::string d = "檔=" + c.sample + ", 比對 " + std::to_string(compared) + " 個鍵, 超過 0.5% " + std::to_string(bad) + " 個";
	if (!ex.empty()) d += " (" + JoinFirst(ex, 5) + ")";
	Record(3, ok, d);
}

// M4 分享碼來回不失真
void TestM4(Ctx& c)
{
	json before, after;
	std::string err;
	if (!GetStats(*c.s, before, err)) { Record(4, false, "前: " + err); return; }
	ToolRes ex = c.s->Call("export_code", json::object());
	if (!ex.Ok() || !NonEmptyStr(ex.sc, "code")) { Record(4, false, "export_code 失敗: " + ex.Why()); return; }
	const std::string code = ex.sc["code"].get<std::string>();
	ToolRes ld = c.s->Call("load_build", {{"source", code}}, kSlowMs);
	if (!ld.Ok() || !ld.sc.value("ok", false)) { Record(4, false, "load_build(code) 失敗: " + ld.Why()); return; }
	if (!GetStats(*c.s, after, err)) { Record(4, false, "後: " + err); return; }
	auto bad = StatDiffKeys(before, after);
	Record(4, bad.empty(), "code 長 " + std::to_string(code.size()) + ", stats " + std::to_string(before.size()) +
	                           " 個鍵, 不同 " + std::to_string(bad.size()) + " 個" + (bad.empty() ? "" : " (" + JoinFirst(bad, 5) + ")"));
}

// 找一個有裝備的欄位,優先用武器 / 胸甲 / 頭盔。
std::string PickEquippedSlot(Ctx& c, std::string& err)
{
	ToolRes li = c.s->Call("list_items", json::object());
	if (!li.Ok() || !li.sc.contains("slots") || !li.sc["slots"].is_array()) { err = "list_items 失敗: " + li.Why(); return {}; }
	std::vector<std::string> equipped;
	for (const auto& sl : li.sc["slots"]) {
		if (sl.is_object() && NonEmptyStr(sl, "slot") && sl.contains("item") && sl["item"].is_object() && !sl["item"].empty())
			equipped.push_back(sl["slot"].get<std::string>());
	}
	if (equipped.empty()) { err = "範例建置沒有任何裝備"; return {}; }
	for (const char* pref : {"weapon 1", "body armour", "helmet"}) {
		for (const auto& e : equipped) if (Lower(e) == pref) return e;
	}
	return equipped[0];
}

// M5 what_if 回傳差異且自動還原
void TestM5(Ctx& c)
{
	std::string err;
	const std::string slot = PickEquippedSlot(c, err);
	if (slot.empty()) { Record(5, false, err); return; }
	EnsureSuggest(c);
	if (!c.suggest.Ok() || !c.suggest.sc.contains("nodes") || !c.suggest.sc["nodes"].is_array() || c.suggest.sc["nodes"].empty()) {
		Record(5, false, "suggest_nodes 沒有可用推薦: " + c.suggest.Why());
		return;
	}
	const json& n0 = c.suggest.sc["nodes"][0];
	const json nodeId = n0.contains("id") ? n0["id"] : json(0);
	json before;
	if (!GetStats(*c.s, before, err)) { Record(5, false, "試算前: " + err); return; }
	json actions = json::array({
		{{"tool", "unequip_item"}, {"arguments", {{"slot", slot}}}},
		{{"tool", "allocate_nodes"}, {"arguments", {{"ids", json::array({nodeId})}}}},
	});
	ToolRes w = c.s->Call("what_if", {{"actions", actions}}, kSlowMs);
	if (!w.Ok()) { Record(5, false, "what_if 失敗: " + w.Why()); return; }
	int nonZero = 0;
	if (w.sc.contains("diff") && w.sc["diff"].is_object()) {
		for (auto it = w.sc["diff"].begin(); it != w.sc["diff"].end(); ++it)
			if (it.value().is_number() && it.value().get<double>() != 0.0) nonZero++;
	}
	const bool reverted = w.sc.value("reverted", false);
	json afterStats;
	if (!GetStats(*c.s, afterStats, err)) { Record(5, false, "試算後: " + err); return; }
	auto bad = StatDiffKeys(before, afterStats);
	const bool ok = nonZero > 0 && reverted && bad.empty();
	Record(5, ok, "卸下 " + slot + " + 點 " + nodeId.dump() + ", diff 非 0 共 " + std::to_string(nonZero) + " 項, reverted=" +
	                  (reverted ? "true" : "false") + ", 還原後 stats 不同 " + std::to_string(bad.size()) + " 個" +
	                  (bad.empty() ? "" : " (" + JoinFirst(bad, 5) + ")"));
}

// M6 suggest_nodes 結果合理
void TestM6(Ctx& c)
{
	EnsureSuggest(c);
	const ToolRes& r = c.suggest;
	if (!r.Ok() || !r.sc.contains("nodes") || !r.sc["nodes"].is_array()) { Record(6, false, "suggest_nodes 失敗: " + r.Why()); return; }
	const auto& nodes = r.sc["nodes"];
	std::vector<std::string> bad;
	if (nodes.size() != 5) bad.push_back("筆數=" + std::to_string(nodes.size()));
	double prev = 0;
	for (size_t i = 0; i < nodes.size(); i++) {
		const auto& n = nodes[i];
		if (!n.is_object()) { bad.push_back("第" + std::to_string(i) + "筆不是物件"); continue; }
		if (!n.contains("id") || !n["id"].is_number_integer()) bad.push_back("第" + std::to_string(i) + "筆 id 不是整數");
		if (!NonEmptyStr(n, "name")) bad.push_back("第" + std::to_string(i) + "筆缺 name");
		if (!NonEmptyStr(n, "nameZh")) bad.push_back("第" + std::to_string(i) + "筆缺 nameZh");
		if (!n.contains("power") || !n["power"].is_number()) { bad.push_back("第" + std::to_string(i) + "筆 power 不是數字"); continue; }
		double p = n["power"].get<double>();
		if (i > 0 && p > prev) bad.push_back("第" + std::to_string(i) + "筆 power 比前一筆大");
		prev = p;
	}
	Record(6, bad.empty(), "共 " + std::to_string(nodes.size()) + " 筆" + (bad.empty() ? ", 由大到小, 欄位齊全" : ": " + JoinFirst(bad, 5)));
}

// M7 check_build 抓到抗性不足
void TestM7(Ctx& c)
{
	ToolRes ld;
	if (!LoadSample(c, ld)) { Record(7, false, "重新讀入範例失敗: " + ld.Why()); return; }
	ToolRes li = c.s->Call("list_items", json::object());
	if (!li.Ok() || !li.sc.contains("slots") || !li.sc["slots"].is_array()) { Record(7, false, "list_items 失敗: " + li.Why()); return; }
	int removed = 0;
	std::vector<std::string> fails;
	for (const auto& sl : li.sc["slots"]) {
		if (!sl.is_object() || !NonEmptyStr(sl, "slot")) continue;
		if (!sl.contains("item") || !sl["item"].is_object() || sl["item"].empty()) continue;
		const std::string slot = sl["slot"].get<std::string>();
		const std::string low = Lower(slot);
		bool target = false;
		for (const char* pre : {"ring", "amulet", "helmet", "gloves", "boots", "belt", "body armour"})
			if (low.rfind(pre, 0) == 0) target = true;
		if (!target) continue;
		ToolRes u = c.s->Call("unequip_item", {{"slot", slot}});
		if (u.Ok()) removed++; else fails.push_back(slot + ": " + Cut(u.Why(), 60));
	}
	ToolRes cb = c.s->Call("check_build", json::object());
	if (!cb.Ok() || !cb.sc.contains("issues") || !cb.sc["issues"].is_array()) { Record(7, false, "check_build 失敗: " + cb.Why()); return; }
	bool hit = false;
	std::vector<std::string> rules;
	for (const auto& is : cb.sc["issues"]) {
		if (!is.is_object()) continue;
		const std::string rule = is.value("rule", std::string());
		const std::string sev = is.value("severity", std::string());
		rules.push_back(rule + "/" + sev);
		if (rule == "elemental_resist" && sev == "error") hit = true;
	}
	std::string d = "卸下 " + std::to_string(removed) + " 件, issues=[" + JoinFirst(rules, 8) + "]";
	if (!fails.empty()) d += ", 卸下失敗: " + JoinFirst(fails, 3);
	Record(7, hit, d);
}

// M8 名稱英中並列
void TestM8(Ctx& c)
{
	ToolRes r = c.s->Call("search_gems", {{"query", "Fireball"}});
	if (!r.Ok() || !r.sc.contains("gems") || !r.sc["gems"].is_array()) { Record(8, false, "search_gems 失敗: " + r.Why()); return; }
	for (const auto& g : r.sc["gems"]) {
		if (g.is_object() && g.value("name", std::string()) == "Fireball") {
			const std::string zh = g.value("nameZh", std::string());
			const bool ok = !zh.empty() && zh != "Fireball";
			Record(8, ok, "Fireball -> nameZh=\"" + zh + "\", 共 " + std::to_string(r.sc["gems"].size()) + " 筆");
			return;
		}
	}
	Record(8, false, "結果 " + std::to_string(r.sc["gems"].size()) + " 筆,沒有 name==\"Fireball\" 的項目");
}

// M11 參數錯誤回傳可讀的錯誤
void TestM11(Ctx& c)
{
	std::vector<std::string> bad;
	std::string d;

	ToolRes a = c.s->Call("search_gems", json::object());
	const bool aOk = a.transportOk && !a.rpcError && a.isError && NonEmptyStr(a.sc, "error");
	if (!aOk) bad.push_back("search_gems 缺 query: " + (a.transportOk && !a.rpcError && !a.isError ? std::string("isError=false") : a.Why()));
	d += "缺 query -> \"" + Cut(ErrText(a.sc), 60) + "\"; ";

	ToolRes b = c.s->Call("allocate_nodes", {{"ids", json::array({999999999})}});
	const bool bOk = b.transportOk && !b.rpcError && b.isError && NonEmptyStr(b.sc, "error");
	if (!bOk) bad.push_back("allocate_nodes 不存在的 id: " + (b.transportOk && !b.rpcError && !b.isError ? std::string("isError=false") : b.Why()));
	d += "id=999999999 -> \"" + Cut(ErrText(b.sc), 60) + "\"; ";

	json resp;
	std::string err;
	bool cOk = false;
	if (!c.s->Request("no/such_method", json::object(), 30000, resp, err)) {
		bad.push_back("未知方法: " + err);
	} else {
		const int code = resp.contains("error") && resp["error"].is_object() ? resp["error"].value("code", 0) : 0;
		cOk = code == -32601;
		if (!cOk) bad.push_back("未知方法 code=" + std::to_string(code) + " (應為 -32601)");
		d += "未知方法 -> " + std::to_string(code);
	}
	Record(11, aOk && bOk && cOk, bad.empty() ? d : JoinFirst(bad, 5));
}

// M10 子行程掛掉後自動重開(另起一個帶 POB_ZH_MCP_DEBUG=1 的 MCP 行程)
std::vector<std::string> g_debugLines;
void TestM10(const std::wstring& exeDir, const std::wstring& pobDir)
{
	SetEnvironmentVariableW(L"POB_ZH_MCP_DEBUG", L"1");
	McpProc s;
	std::string err;
	const bool started = s.Start(exeDir, pobDir, err);
	SetEnvironmentVariableW(L"POB_ZH_MCP_DEBUG", nullptr);
	if (!started) { Record(10, false, "啟動失敗: " + err); return; }

	std::string d;
	bool ok = false;
	do {
		json resp;
		json params = {{"protocolVersion", "2025-06-18"}, {"capabilities", json::object()},
		               {"clientInfo", {{"name", "pobtools-mcp-selftest"}, {"version", "1"}}}};
		if (!s.Request("initialize", params, 30000, resp, err)) { d = "initialize: " + err; break; }
		s.Notify("notifications/initialized");
		// 先讓子行程確實起來,kill 才有東西可殺;這一步的結果不計入判定。
		ToolRes warm = s.Call("build_summary", json::object(), kSlowMs);
		ToolRes kill = s.Call("debug_kill_engine", json::object(), kSlowMs);
		if (!kill.transportOk || kill.rpcError) { d = "debug_kill_engine 沒有回應: " + kill.Why(); break; }
		ToolRes after = s.Call("build_summary", json::object(), kSlowMs);
		if (!after.transportOk) { d = "kill 之後 build_summary: " + after.transportErr; break; }
		const bool restarted = after.sc.value("engineRestarted", false);
		ok = !after.isError && !after.rpcError && restarted;
		d = std::string("暖機 ") + (warm.Ok() ? "ok" : "失敗") + ", kill 後 isError=" + (after.isError ? "true" : "false") +
		    ", engineRestarted=" + (restarted ? "true" : "false");
		if (!ok) d += " (" + Cut(after.Why(), 160) + ")";
	} while (false);
	s.Close();
	g_debugLines = s.AllStdoutLines();
	Record(10, ok, d);
}

// M2 stdout 只出現 JSON-RPC(兩個行程的所有 stdout 行)
void TestM2(const std::vector<std::string>& lines)
{
	int bad = 0;
	std::vector<std::string> ex;
	for (const auto& ln : lines) {
		json j = json::parse(ln, nullptr, false);
		bool ok = !j.is_discarded() && j.is_object() && j.value("jsonrpc", std::string()) == "2.0";
		if (ok) {
			const bool req = j.contains("method") && j["method"].is_string();
			const bool resp = j.contains("id") && (j.contains("result") != j.contains("error"));
			ok = req || resp;
		}
		if (!ok) {
			bad++;
			if (ex.size() < 3) ex.push_back("\"" + Cut(ln, 60) + "\"");
		}
	}
	Record(2, !lines.empty() && bad == 0,
	       "共 " + std::to_string(lines.size()) + " 行, 非 JSON-RPC " + std::to_string(bad) + " 行" +
	           (ex.empty() ? "" : " (" + JoinFirst(ex, 3) + ")") + (lines.empty() ? ", 完全沒有輸出" : ""));
}

// M9 不寫回使用者資料夾
void TestM9(const Snap& before, const std::wstring& pobDir)
{
	Snap after;
	Snapshot(pobDir, L"", after);
	std::vector<std::string> added, removed, changed;
	for (const auto& [k, v] : after) {
		auto it = before.find(k);
		if (it == before.end()) added.push_back(narrow(k));
		else if (it->second.dir != v.dir || it->second.size != v.size || it->second.mtime != v.mtime) changed.push_back(narrow(k));
	}
	for (const auto& [k, v] : before) if (!after.count(k)) removed.push_back(narrow(k));
	const bool ok = !before.empty() && added.empty() && removed.empty() && changed.empty();
	std::string d = "共 " + std::to_string(before.size()) + " 項, 新增 " + std::to_string(added.size()) + ", 刪除 " +
	                std::to_string(removed.size()) + ", 變更 " + std::to_string(changed.size());
	if (!added.empty()) d += "; 新增: " + JoinFirst(added, 3);
	if (!removed.empty()) d += "; 刪除: " + JoinFirst(removed, 3);
	if (!changed.empty()) d += "; 變更: " + JoinFirst(changed, 3);
	if (before.empty()) d += " (快照為空)";
	Record(9, ok, d);
}

// M13 equip_item 會取代已有裝備的欄位
void TestM13(Ctx& c)
{
	ToolRes ld;
	if (!LoadSample(c, ld)) { Record(13, false, "讀入範例失敗: " + ld.Why()); return; }
	ToolRes li = c.s->Call("list_items", json::object());
	if (!li.Ok() || !li.sc.contains("slots") || !li.sc["slots"].is_array()) { Record(13, false, "list_items 失敗: " + li.Why()); return; }
	std::vector<std::string> rings;
	for (const auto& sl : li.sc["slots"]) {
		if (!sl.is_object() || !NonEmptyStr(sl, "slot")) continue;
		if (!sl.contains("item") || !sl["item"].is_object() || sl["item"].empty()) continue;
		const std::string slot = sl["slot"].get<std::string>();
		if (slot.rfind("Ring", 0) == 0) rings.push_back(slot);
	}
	if (rings.empty()) { Record(13, false, "範例建置沒有已裝備的 Ring 欄位"); return; }

	const std::string text =
		"Rarity: Rare\nTest Loop\nTwo-Stone Ring\n--------\n+40% to Fire Resistance\n+40% to Cold Resistance\n";
	ToolRes eq = c.s->Call("equip_item", {{"text", text}});
	std::string d;
	bool ok = false;
	if (!eq.Ok()) {
		d = "equip_item 失敗: " + eq.Why();
	} else if (!NonEmptyStr(eq.sc, "slot")) {
		d = "equip_item 回傳的 slot 不是非空字串";
	} else {
		const std::string slot = eq.sc["slot"].get<std::string>();
		ToolRes li2 = c.s->Call("list_items", json::object());
		std::string name = "(找不到欄位)";
		if (li2.Ok() && li2.sc.contains("slots") && li2.sc["slots"].is_array()) {
			for (const auto& sl : li2.sc["slots"]) {
				if (sl.is_object() && sl.value("slot", std::string()) == slot) {
					name = sl.contains("item") && sl["item"].is_object() ? sl["item"].value("name", std::string("(無名稱)")) : std::string("(空)");
				}
			}
		}
		const bool wasEquipped = std::find(rings.begin(), rings.end(), slot) != rings.end();
		ok = name.rfind("Test Loop", 0) == 0 && wasEquipped; // POB 稀有物品名為「名稱, 底材」
		d = "原有 Ring 欄位 [" + JoinFirst(rings, 4) + "], 回傳 slot=" + slot + (wasEquipped ? "" : "(不是原有裝備的欄位)") +
		    ", 之後該欄位 item.name=" + name;
	}
	ToolRes re;
	if (!LoadSample(c, re)) { ok = false; d += "; 還原範例失敗: " + re.Why(); }
	Record(13, ok, d);
}

// M14 suggest_nodes 帶出路徑點數
void TestM14(Ctx& c)
{
	EnsureSuggest(c);
	const ToolRes& r = c.suggest;
	if (!r.Ok() || !r.sc.contains("nodes") || !r.sc["nodes"].is_array() || r.sc["nodes"].empty()) {
		Record(14, false, "suggest_nodes 沒有可用結果: " + r.Why());
		return;
	}
	std::vector<std::string> bad;
	std::string vals;
	const auto& nodes = r.sc["nodes"];
	for (size_t i = 0; i < nodes.size(); i++) {
		const auto& n = nodes[i];
		const bool ok = n.is_object() && n.contains("pathDist") && n["pathDist"].is_number_integer() && n["pathDist"].get<long long>() >= 1;
		vals += (i ? "," : "") + (n.is_object() && n.contains("pathDist") ? n["pathDist"].dump() : std::string("缺"));
		if (!ok) bad.push_back("第" + std::to_string(i) + "筆");
	}
	Record(14, bad.empty(), "共 " + std::to_string(nodes.size()) + " 筆, pathDist=[" + vals + "]" +
	                            (bad.empty() ? "" : ", 不合格: " + JoinFirst(bad, 5)));
}

// M15 點他昇華的節點會報錯且建置不變
void TestM15(Ctx& c)
{
	ToolRes ld;
	if (!LoadSample(c, ld)) { Record(15, false, "讀入範例失敗: " + ld.Why()); return; }
	ToolRes b0 = c.s->Call("build_summary", json::object());
	if (!b0.Ok() || !b0.sc.contains("stats") || !b0.sc.contains("ascendancy")) { Record(15, false, "前: build_summary 失敗或缺欄位: " + b0.Why()); return; }
	ToolRes sn = c.s->Call("search_nodes", {{"query", "Liege of the Primordial"}});
	if (!sn.Ok() || !sn.sc.contains("nodes") || !sn.sc["nodes"].is_array() || sn.sc["nodes"].empty() ||
	    !sn.sc["nodes"][0].is_object() || !sn.sc["nodes"][0].contains("id")) {
		Record(15, false, "search_nodes 沒有結果: " + sn.Why());
		return;
	}
	const json id = sn.sc["nodes"][0]["id"];
	ToolRes al = c.s->Call("allocate_nodes", {{"ids", json::array({id})}});
	const bool errOk = al.transportOk && !al.rpcError && al.isError && NonEmptyStr(al.sc, "error");
	ToolRes b1 = c.s->Call("build_summary", json::object());
	if (!b1.Ok() || !b1.sc.contains("stats")) { Record(15, false, "後: build_summary 失敗: " + b1.Why()); return; }
	const bool ascSame = b0.sc["ascendancy"] == b1.sc.value("ascendancy", json(nullptr));
	auto bad = StatDiffKeys(b0.sc["stats"], b1.sc["stats"]);
	std::string d = "節點 " + id.dump() + ", allocate_nodes " +
	                (errOk ? "報錯: \"" + Cut(ErrText(al.sc), 60) + "\"" : "沒有報錯(" + (al.Ok() ? std::string("isError=false") : al.Why()) + ")") +
	                ", ascendancy " + b0.sc["ascendancy"].dump() + (ascSame ? " 不變" : "->" + b1.sc.value("ascendancy", json(nullptr)).dump()) +
	                ", stats 不同 " + std::to_string(bad.size()) + " 個" + (bad.empty() ? "" : " (" + JoinFirst(bad, 5) + ")");
	Record(15, errOk && ascSame && bad.empty(), d);
}

// M16 new_build 職業打錯時保留原建置與名稱
void TestM16(Ctx& c)
{
	ToolRes ld;
	if (!LoadSample(c, ld)) { Record(16, false, "讀入範例失敗: " + ld.Why()); return; }
	ToolRes b0 = c.s->Call("build_summary", json::object());
	if (!b0.Ok() || !b0.sc.contains("buildName") || !b0.sc.contains("ascendancy") || !b0.sc.contains("stats")) {
		Record(16, false, "前: build_summary 失敗或缺 buildName/ascendancy/stats: " + b0.Why());
		return;
	}
	ToolRes nb = c.s->Call("new_build", {{"class", "NoSuchClass"}}, kSlowMs);
	const bool errOk = nb.transportOk && !nb.rpcError && nb.isError && NonEmptyStr(nb.sc, "error");
	ToolRes b1 = c.s->Call("build_summary", json::object());
	if (!b1.Ok() || !b1.sc.contains("stats")) { Record(16, false, "後: build_summary 失敗: " + b1.Why()); return; }
	const json nul(nullptr);
	const bool nameSame = b0.sc["buildName"] == (b1.sc.contains("buildName") ? b1.sc["buildName"] : nul);
	const bool ascSame = b0.sc["ascendancy"] == (b1.sc.contains("ascendancy") ? b1.sc["ascendancy"] : nul);
	auto bad = StatDiffKeys(b0.sc["stats"], b1.sc["stats"]);
	std::string d = std::string("new_build ") +
	                (errOk ? "報錯: \"" + Cut(ErrText(nb.sc), 60) + "\"" : "沒有報錯(" + (nb.Ok() ? std::string("isError=false") : nb.Why()) + ")") +
	                ", buildName " + b0.sc["buildName"].dump() + (nameSame ? " 不變" : " 變了") +
	                ", ascendancy " + (ascSame ? "不變" : "變了") +
	                ", stats 不同 " + std::to_string(bad.size()) + " 個" + (bad.empty() ? "" : " (" + JoinFirst(bad, 5) + ")");
	Record(16, errOk && nameSame && ascSame && bad.empty(), d);
}

// M17 build_summary 帶出已點基石
void TestM17(Ctx& c)
{
	ToolRes ld;
	if (!LoadSample(c, ld)) { Record(17, false, "讀入範例失敗: " + ld.Why()); return; }
	ToolRes b = c.s->Call("build_summary", json::object());
	if (!b.Ok() || !b.sc.contains("keystones") || !b.sc["keystones"].is_array()) { Record(17, false, "build_summary 沒有 keystones 陣列: " + b.Why()); return; }
	const auto& ks = b.sc["keystones"];
	std::vector<std::string> bad;
	bool hasCI = false;
	if (ks.empty()) bad.push_back("keystones 是空的");
	for (size_t i = 0; i < ks.size(); i++) {
		const auto& k = ks[i];
		const std::string tag = "第" + std::to_string(i) + "筆";
		if (!k.is_object()) { bad.push_back(tag + "不是物件"); continue; }
		if (!k.contains("id") || !k["id"].is_number_integer()) bad.push_back(tag + " id 不是整數");
		if (!NonEmptyStr(k, "name")) bad.push_back(tag + "缺 name");
		if (!NonEmptyStr(k, "nameZh")) bad.push_back(tag + "缺 nameZh");
		bool statsOk = k.contains("stats") && k["stats"].is_array() && !k["stats"].empty();
		if (statsOk) for (const auto& st : k["stats"]) if (!st.is_string()) statsOk = false;
		if (!statsOk) bad.push_back(tag + " stats 不是非空字串陣列");
		if (k.value("name", std::string()) == "Chaos Inoculation") hasCI = true;
	}
	if (!hasCI) bad.push_back("沒有 Chaos Inoculation");
	Record(17, bad.empty(), "基石 " + std::to_string(ks.size()) + " 個" + (bad.empty() ? ", 欄位齊全, 含 Chaos Inoculation" : ": " + JoinFirst(bad, 5)));
}

// M18 免疫混沌時不警告混沌抗性
void TestM18(Ctx& c)
{
	ToolRes ld;
	if (!LoadSample(c, ld)) { Record(18, false, "讀入範例失敗: " + ld.Why()); return; }
	ToolRes b = c.s->Call("build_summary", json::object());
	if (!b.Ok() || !b.sc.contains("immuneTo") || !b.sc["immuneTo"].is_array()) { Record(18, false, "build_summary 沒有 immuneTo 陣列: " + b.Why()); return; }
	bool immune = false;
	std::vector<std::string> list;
	for (const auto& v : b.sc["immuneTo"]) {
		if (!v.is_string()) continue;
		list.push_back(v.get<std::string>());
		if (v.get<std::string>() == "Chaos") immune = true;
	}
	ToolRes cb = c.s->Call("check_build", json::object());
	if (!cb.Ok() || !cb.sc.contains("issues") || !cb.sc["issues"].is_array()) { Record(18, false, "check_build 失敗: " + cb.Why()); return; }
	bool warned = false;
	for (const auto& is : cb.sc["issues"]) if (is.is_object() && is.value("rule", std::string()) == "chaos_resist") warned = true;
	Record(18, immune && !warned, "immuneTo=[" + JoinFirst(list, 6) + "], issues " + std::to_string(cb.sc["issues"].size()) + " 筆, " +
	                                  (warned ? "仍有 chaos_resist 警告" : "沒有 chaos_resist 警告"));
}

// M19 build_guide 回傳判讀守則(M1 之後、其他工具呼叫之前)
void TestM19(Ctx& c)
{
	ToolRes g = c.s->Call("build_guide", json::object());
	if (!g.Ok()) { Record(19, false, "build_guide 失敗: " + g.Why()); return; }
	const std::string text = g.sc.contains("text") && g.sc["text"].is_string() ? g.sc["text"].get<std::string>() : std::string();
	const bool hasCI = text.find("Chaos Inoculation") != std::string::npos;
	Record(19, text.size() > 500 && hasCI, "text 長 " + std::to_string(text.size()) + " 位元組, " + (hasCI ? "含" : "不含") + " Chaos Inoculation");
}

// M20 build_summary 帶出迴避機率
void TestM20(Ctx& c)
{
	ToolRes ld;
	if (!LoadSample(c, ld)) { Record(20, false, "讀入範例失敗: " + ld.Why()); return; }
	ToolRes b = c.s->Call("build_summary", json::object());
	if (!b.Ok() || !b.sc.contains("stats") || !b.sc["stats"].is_object()) { Record(20, false, "build_summary 沒有 stats 物件: " + b.Why()); return; }
	const json& st = b.sc["stats"];
	std::vector<std::string> bad;
	std::string vals;
	for (const char* key : {"StunAvoidChance", "FreezeAvoidChance", "ShockAvoidChance"}) {
		if (!st.contains(key) || !st[key].is_number()) { bad.push_back(std::string(key) + " 缺少或不是數字"); continue; }
		const double v = st[key].get<double>();
		vals += std::string(vals.empty() ? "" : ", ") + key + "=" + Num(v);
		if (!(v >= 0.0 && v <= 100.0)) bad.push_back(std::string(key) + "=" + Num(v) + " 不在 0-100");
	}
	if (b.sc.contains("minionStats") && !b.sc["minionStats"].is_object()) bad.push_back("minionStats 存在但不是物件");
	Record(20, bad.empty(), bad.empty() ? vals + (b.sc.contains("minionStats") ? ", 有 minionStats 物件" : ", 無 minionStats") : JoinFirst(bad, 5));
}

// M21 寫進未啟用的技能組會警告
void TestM21(Ctx& c)
{
	std::vector<std::string> bad;
	std::string d;
	const json steel = {{"name", "Steelskin"}, {"level", 20}, {"quality", 20}};
	ToolRes ld;
	if (!LoadSample(c, ld)) { Record(21, false, "讀入範例失敗: " + ld.Why()); return; }

	// 1. list_skills:每組都有布林 active;Weapon 2 Swap 那組是 false 且有 inactiveReason
	ToolRes ls = c.s->Call("list_skills", json::object());
	if (!ls.Ok() || !ls.sc.contains("groups") || !ls.sc["groups"].is_array()) { Record(21, false, "list_skills 失敗: " + ls.Why()); return; }
	int swapIdx = -1, noActive = 0;
	for (const auto& g : ls.sc["groups"]) {
		if (!g.is_object() || !g.contains("active") || !g["active"].is_boolean()) { noActive++; continue; }
		if (g.value("slot", std::string()) == "Weapon 2 Swap" && swapIdx < 0 && g.contains("index") && g["index"].is_number_integer()) {
			swapIdx = g["index"].get<int>();
			if (g["active"].get<bool>()) bad.push_back("Weapon 2 Swap 組 active 不是 false");
			if (!NonEmptyStr(g, "inactiveReason")) bad.push_back("Weapon 2 Swap 組缺 inactiveReason");
		}
	}
	if (noActive > 0) bad.push_back(std::to_string(noActive) + " 組沒有布林 active");
	if (swapIdx < 0) bad.push_back("找不到 slot==\"Weapon 2 Swap\" 的技能組");
	d = "共 " + std::to_string(ls.sc["groups"].size()) + " 組, 交換武器組 index=" + std::to_string(swapIdx);

	// 2. 寫進未啟用的組:要有非空 warning
	if (swapIdx >= 0) {
		ToolRes w = c.s->Call("set_skill_group", {{"index", swapIdx}, {"gems", json::array({steel})}});
		if (!w.Ok()) bad.push_back("寫入未啟用組失敗: " + Cut(w.Why(), 80));
		else if (!NonEmptyStr(w.sc, "warning")) bad.push_back("寫入未啟用組沒有 warning");
		else d += ", warning=\"" + Cut(w.sc["warning"].get<std::string>(), 40) + "\"";
	}

	// 3. 還原後新增一組(不給 index):不可有 warning
	ToolRes re1;
	if (!LoadSample(c, re1)) bad.push_back("中途重新讀入範例失敗: " + Cut(re1.Why(), 80));
	ToolRes n = c.s->Call("set_skill_group", {{"gems", json::array({steel})}});
	if (!n.Ok()) bad.push_back("新增技能組失敗: " + Cut(n.Why(), 80));
	else if (n.sc.contains("warning")) bad.push_back("新增技能組不該有 warning");
	else d += ", 新增組無 warning";

	// 4. 還原
	ToolRes re2;
	if (!LoadSample(c, re2)) bad.push_back("還原範例失敗: " + Cut(re2.Why(), 80));
	Record(21, bad.empty(), bad.empty() ? d : JoinFirst(bad, 6) + " (" + d + ")");
}

// M12 設定字串帶入正確路徑(純函式,不啟動 MCP 行程)
void TestM12()
{
	const std::string exe = "C:\\Program Files\\PobTools 測試\\pob-zh.exe";
	std::vector<std::string> bad;

	// JSON:能解析,command 等於路徑
	{
		json j = json::parse(McpConfig::ClaudeDesktopJson(exe), nullptr, false);
		bool ok = false;
		if (!j.is_discarded() && j.is_object() && j.contains("mcpServers") && j["mcpServers"].is_object() &&
		    j["mcpServers"].contains("pobtools") && j["mcpServers"]["pobtools"].is_object()) {
			const json& e = j["mcpServers"]["pobtools"];
			ok = e.contains("command") && e["command"].is_string() && e["command"].get<std::string>() == exe &&
			     e.contains("args") && e["args"] == json::array({"--mcp"});
		}
		if (!ok) bad.push_back("JSON 無法解析或 command/args 不符");
	}

	// 指令:逐字相等
	{
		const std::string want = "claude mcp add pobtools -- \"" + exe + "\" --mcp";
		const std::string got = McpConfig::ClaudeCodeCommand(exe);
		if (got != want) bad.push_back("指令不符: " + Cut(got, 120));
	}

	// TOML:表頭一行、command 行還原跳脫後等於路徑、args 行逐字相等
	{
		const std::string toml = McpConfig::CodexToml(exe);
		bool header = false, command = false, args = false;
		size_t pos = 0;
		while (pos <= toml.size()) {
			size_t nl = toml.find('\n', pos);
			std::string ln = toml.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
			pos = nl == std::string::npos ? toml.size() + 1 : nl + 1;
			size_t a = ln.find_first_not_of(" \t\r");
			if (a == std::string::npos) continue;
			size_t b = ln.find_last_not_of(" \t\r");
			ln = ln.substr(a, b - a + 1);
			if (ln == "[mcp_servers.pobtools]") header = true;
			if (ln == "args = [\"--mcp\"]") args = true;
			const std::string pre = "command = \"";
			if (ln.size() > pre.size() && ln.compare(0, pre.size(), pre) == 0 && ln.back() == '"') {
				std::string raw = ln.substr(pre.size(), ln.size() - pre.size() - 1), un;
				for (size_t i = 0; i < raw.size(); i++) {
					if (raw[i] == '\\' && i + 1 < raw.size() && (raw[i + 1] == '\\' || raw[i + 1] == '"')) { un += raw[++i]; }
					else un += raw[i];
				}
				if (un == exe) command = true;
			}
		}
		if (!header) bad.push_back("TOML 缺 [mcp_servers.pobtools] 表頭");
		if (!command) bad.push_back("TOML command 行還原後不等於路徑");
		if (!args) bad.push_back("TOML args 行不是 args = [\"--mcp\"]");
	}

	Record(12, bad.empty(), bad.empty() ? "JSON、指令、TOML 都帶入 " + exe : JoinFirst(bad, 5));
}

void Guard(int n, const std::function<void()>& fn)
{
	try {
		fn();
	} catch (const std::exception& e) {
		Record(n, false, std::string("例外: ") + e.what());
	}
}

} // namespace

int RunMcpSelfTest(const std::wstring& exeDir, const std::wstring& pobDirOverride)
{
	g_res.clear();
	g_debugLines.clear();
	const std::wstring reportPath = exeDir + L"mcp_selftest.txt";
	DeleteFileW(reportPath.c_str()); // 不讓舊報告被當成這次的結果

	Guard(12, [] { TestM12(); }); // 純函式,找不到 POB 也照跑

	std::wstring pobDir = pobDirOverride;
	if (pobDir.empty()) {
		InstallInfo info = DetectInstalls(exeDir);
		pobDir = info.poe1Dir;
	}
	while (!pobDir.empty() && pobDir.back() == L'\\') pobDir.pop_back();

	auto failAll = [&](const std::string& why) {
		for (int n = 1; n <= 11; n++) Record(n, false, why); // M12 是純函式,不受影響
		for (int n = 13; n <= 21; n++) Record(n, false, why);
	};

	std::wstring sampleRel;
	std::string sampleXml;
	Snap snapBefore;

	if (pobDir.empty() || !IsDir(pobDir)) {
		failAll("找不到 PoE1 的 POB 資料夾: " + narrow(pobDir) + "(請把資料夾當第二個參數傳入)");
	} else if (!FileExists(exeDir + L"pob-zh.exe")) {
		failAll("找不到 " + narrow(exeDir) + "pob-zh.exe");
	} else {
		Snapshot(pobDir, L"", snapBefore);
		const bool haveSample = FindSampleBuild(pobDir + L"\\Builds", sampleRel, sampleXml);

		// 主行程不帶除錯變數,即使使用者環境裡已經設了。
		SetEnvironmentVariableW(L"POB_ZH_MCP_DEBUG", nullptr);

		std::vector<std::string> mainLines;
		{
			McpProc s;
			std::string err;
			if (!s.Start(exeDir, pobDir, err)) {
				for (int n : {1, 3, 4, 5, 6, 7, 8, 11, 13, 14, 15, 16, 17, 18, 19, 20, 21}) Record(n, false, "MCP 子行程啟動失敗: " + err);
			} else {
				Ctx c;
				c.s = &s;
				if (haveSample) {
					c.sample = narrow(sampleRel);
					c.sampleXml = sampleXml;
				}
				Guard(1, [&] { TestM1(c); });
				Guard(19, [&] { TestM19(c); }); // 要在其他工具呼叫之前
				if (!haveSample) {
					for (int n : {3, 4, 5, 6, 7, 13, 14, 15, 16, 17, 18, 20, 21})
						Record(n, false, "Builds 資料夾找不到含 <PlayerStat 的 .xml 範例建置");
				} else {
					Guard(3, [&] { TestM3(c); });
					Guard(4, [&] { TestM4(c); });
					Guard(5, [&] { TestM5(c); });
					Guard(6, [&] { TestM6(c); });
					Guard(7, [&] { TestM7(c); });
					Guard(13, [&] { TestM13(c); });
					Guard(14, [&] { TestM14(c); });
					Guard(15, [&] { TestM15(c); });
					Guard(16, [&] { TestM16(c); });
					Guard(17, [&] { TestM17(c); });
					Guard(18, [&] { TestM18(c); });
					Guard(20, [&] { TestM20(c); });
					Guard(21, [&] { TestM21(c); });
				}
				Guard(8, [&] { TestM8(c); });
				Guard(11, [&] { TestM11(c); });
				s.Close();
				mainLines = s.AllStdoutLines();
			}
		}

		Guard(10, [&] { TestM10(exeDir, pobDir); });

		std::vector<std::string> all = mainLines;
		all.insert(all.end(), g_debugLines.begin(), g_debugLines.end());
		if (!g_res.count(2)) Guard(2, [&] { TestM2(all); });
		Guard(9, [&] { TestM9(snapBefore, pobDir); });
	}

	// --- 報告:每項一行,最後一行總計 ------------------------------------
	std::string rep;
	int pass = 0, fail = 0;
	for (int n = 1; n <= 21; n++) {
		Res r = g_res.count(n) ? g_res[n] : Res{false, "沒有執行"};
		if (r.ok) pass++; else fail++;
		rep += std::string(r.ok ? "PASS" : "FAIL") + " M" + std::to_string(n) + " " + kTitles[n] + " — " + r.detail + "\r\n";
	}
	rep += "總計 PASS " + std::to_string(pass) + " FAIL " + std::to_string(fail) + (fail == 0 ? " ALL PASS" : " FAILURES") + "\r\n";

	if (AttachConsole(ATTACH_PARENT_PROCESS)) {
		FILE* f = nullptr;
		freopen_s(&f, "CONOUT$", "w", stdout);
	}
	printf("%s", rep.c_str());
	WriteFileA(reportPath, rep);
	return fail;
}
