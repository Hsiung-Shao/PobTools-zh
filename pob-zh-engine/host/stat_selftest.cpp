// --stat-selftest：translation_lookup_stat 的契約(依 stats_by_id.json 翻單行 stat)。
//
// stats_by_id.json 的鍵是 GGG 的 stat id 組合 "stat_ids|cond|src_file|dup_ix",
// "templates" 放英文模板、"entries" 放繁中模板,佔位符是 {0} / {0:+d},多行是字面
// "\n"。查詢以正規化後的英文模板找候選鍵,依情境(item / passive)篩描述檔,篩後
// 中文一致才採用,再把原行的數值依 {N} 編號填回中文模板。
//
// 測試用真資料(GGPK 抽出的 stats_by_id.json),不植入假條目:字典複本建在
// %TEMP%\pobtools_stat_selftest\ 下,以 POB_ZH_DATADIR 指過去,出貨字典一個位元組
// 都不動;結束時刪掉複本、清掉環境變數。
//
// 負向測試(E6~E9、E12)都附「同環境下有命中」的前提,所以永遠回 nullptr 的實作
// 過不了 —— 只斷言「回空」的測試,空殼也會綠。
//
// 期望值的出處(tools/ggpk_zh/out_source_dicts/stats_by_id.json,2026-10-17 產出):
//   E1  base_resist_all_elements_%|#|stat_descriptions.txt|0       {0:+d}% 全部元素抗性
//   E1b accuracy_rating_+%|1|#|stat_descriptions.txt|0             增加 {0}% 全域命中值
//   E2  base_maximum_life|#|stat_descriptions.txt|0                {0:+d} 最大生命
//   E3  accuracy_rating_+%|#|-1|stat_descriptions.txt|0            減少 {0}% 全域命中值
//   E4  global_minimum_added_fire_damage global_maximum_added_fire_damage|# #|stat_descriptions.txt|0
//                                                                  附加 {0} 至 {1} 火焰傷害
//   E4b deaths_oath_debuff_on_kill_duration_ms deaths_oath_debuff_on_kill_base_chaos_damage_to_deal_per_minute|# #|stat_descriptions.txt|0
//       EN "You take {1} Chaos Damage per second for {0} seconds on Kill"
//       ZH "殺死敵人後 {0} 秒內持續承受 {1} 混沌傷害"(中英語序相反)
//   E5  accuracy_rating_+%|1|#|passive_skill_stat_descriptions.txt|0 增加 {0}% 命中率
//       local_accuracy_rating_+%|1|#|stat_descriptions.txt|0         增加 {0}% 命中值
//   E6  stats_ambiguous.tsv:"# to Level of Socketed Gems"(stat_descriptions.txt 兩譯)
//   E10 %_chance_to_gain_random_charge_on_trap_triggered_by_an_enemy|#|stat_descriptions.txt|0
//   E11 chance_%_to_gain_brine_charge_instead_of_endurance_charge|1|99|stat_descriptions.txt|0
//       EN "{0}% chance to gain a [BrineCharges|Brine Charge] instead of an Endurance Charge"

#include "stat_selftest.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <json.hpp>

#include "../translate/translation_manager.h"
#include "error_log.h"

namespace {

int g_pass = 0;
int g_fail = 0;
// 等資料進來才會生效的斷言(跨分支依賴):不算通過也不算失敗,但在結果行列出,不會被看漏
int g_wait = 0;

void wait_for_data(const char* what, const std::string& now, const std::string& want)
{
	g_wait++;
	printf("  [WAIT]  %s\n           (出貨資料尚未帶這筆;目前 '%s',資料進來後要是 '%s')\n", what, now.c_str(), want.c_str());
}

void check(bool ok, const char* what)
{
	(ok ? g_pass : g_fail)++;
	printf("  %s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
}

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring exe_dir()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring s(path);
	size_t slash = s.find_last_of(L'\\');
	return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}

bool read_bytes(const std::wstring& p, std::string& out)
{
	HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
	                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	out.clear();
	char tmp[65536];
	DWORD got = 0;
	while (ReadFile(h, tmp, sizeof(tmp), &got, nullptr) && got) out.append(tmp, got);
	CloseHandle(h);
	return true;
}

bool write_bytes(const std::wstring& p, const std::string& body)
{
	HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
	                       FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE) return false;
	DWORD w = 0;
	const BOOL ok = WriteFile(h, body.data(), (DWORD)body.size(), &w, nullptr);
	CloseHandle(h);
	return ok && w == (DWORD)body.size();
}

// 刪掉 dir 底下的檔案與一層子資料夾(本測試只建兩層:<root>\<variant>\zh-rTW\)。
void remove_tree(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			const std::wstring name = fd.cFileName;
			if (name == L"." || name == L"..") continue;
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(dir + name + L"\\");
			else DeleteFileW((dir + name).c_str());
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	RemoveDirectoryW(dir.c_str());
}

// 出貨字典 zh-rTW 的 *.json(meta.json 除外)整份複製到 dst,再放進 stats_by_id.json
// 與改過的 meta.json。flag=false 時 meta 不帶 source_lookup(PoE2 / zh-rCN 的狀態)。
bool build_dict_copy(const std::wstring& srcLocaleDir, const std::wstring& dst,
                     const std::wstring& statsJson, bool flag, std::string& why)
{
	CreateDirectoryW(dst.c_str(), nullptr);
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((srcLocaleDir + L"*.json").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) { why = "no *.json in " + narrow(srcLocaleDir); return false; }
	do {
		const std::wstring name = fd.cFileName;
		if (name == L"meta.json" || name == L"stats_by_id.json") continue;
		if (!CopyFileW((srcLocaleDir + name).c_str(), (dst + name).c_str(), FALSE)) {
			FindClose(h);
			why = "copy failed: " + narrow(name);
			return false;
		}
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	if (!CopyFileW(statsJson.c_str(), (dst + L"stats_by_id.json").c_str(), FALSE)) {
		why = "stats_by_id.json not found: " + narrow(statsJson);
		return false;
	}
	std::string body;
	if (!read_bytes(srcLocaleDir + L"meta.json", body)) { why = "meta.json unreadable"; return false; }
	try {
		auto meta = nlohmann::ordered_json::parse(body);
		auto& lo = meta["load_order"];
		for (auto it = lo.begin(); it != lo.end();) {
			if (it->is_string() && it->get<std::string>() == "stats_by_id.json") it = lo.erase(it);
			else ++it;
		}
		auto& sd = meta["source_dicts"];
		if (!sd.is_array()) sd = nlohmann::ordered_json::array();
		bool have = false;
		for (const auto& x : sd)
			if (x.is_string() && x.get<std::string>() == "stats_by_id.json") have = true;
		if (!have) sd.push_back("stats_by_id.json");
		if (flag) meta["source_lookup"] = true;
		else meta.erase("source_lookup");
		if (!write_bytes(dst + L"meta.json", meta.dump(2))) { why = "meta.json write failed"; return false; }
	} catch (...) {
		why = "meta.json parse failed";
		return false;
	}
	return true;
}

// nullptr 印成 (null),方便在失敗訊息裡和空字串分開。
struct Said { bool null; std::string s; };
Said ask(const char* line, const char* ctx)
{
	const char* r = translation_lookup_stat(line, ctx);
	return r ? Said{false, r} : Said{true, {}};
}
const char* shown(const Said& s) { return s.null ? "(null)" : s.s.c_str(); }

// 期望某譯文;失敗時印出實際值。
void expect(const char* line, const char* ctx, const std::string& want, const char* name)
{
	const Said got = ask(line, ctx);
	const bool ok = !got.null && got.s == want;
	check(ok, name);
	if (!ok) printf("           %s [%s]\n           got  '%s'\n           want '%s'\n",
	                line, ctx, shown(got), want.c_str());
}

bool in_array(const nlohmann::ordered_json& meta, const char* field, const char* f)
{
	if (!meta.contains(field) || !meta[field].is_array()) return false;
	for (const auto& x : meta[field])
		if (x.is_string() && x.get<std::string>() == f) return true;
	return false;
}

// 在字典複本的 ui.json 加條目(D 系列用):extra 個無關的虛構鍵(迫使雜湊表
// rehash、改變迭代順序)+ 指定的鍵值。
bool plant_ui(const std::wstring& localeDir, int extra,
              const std::vector<std::pair<std::string, std::string>>& kv)
{
	std::string body;
	if (!read_bytes(localeDir + L"ui.json", body)) return false;
	try {
		auto doc = nlohmann::ordered_json::parse(body);
		auto& e = doc["entries"];
		for (int i = 0; i < extra; i++) {
			char k[64], v[64];
			snprintf(k, sizeof(k), "Qqxfiller Unrelated Key %05d", i);
			snprintf(v, sizeof(v), "\xe5\xa1\xab\xe5\x85\x85 %05d", i); // 填充 NNNNN
			e[k] = v;
		}
		for (const auto& p : kv) e[p.first] = p.second;
		return write_bytes(localeDir + L"ui.json", doc.dump(2));
	} catch (...) {
		return false;
	}
}

// 在字典複本的任一扁平字典檔(entries 物件)加條目(X 系列用)。
bool plant_file(const std::wstring& path, const std::vector<std::pair<std::string, std::string>>& kv)
{
	std::string body;
	if (!read_bytes(path, body)) return false;
	try {
		auto doc = nlohmann::ordered_json::parse(body);
		auto& e = doc["entries"];
		if (!e.is_object()) return false;
		for (const auto& p : kv) e[p.first] = p.second;
		return write_bytes(path, doc.dump(2));
	} catch (...) {
		return false;
	}
}

// 改字典複本裡的 *_by_id.json(P0 用):templates 非空 → 寫入/合併 "templates";
// strip = true → 先移除原有 "templates"(templates 空 = 舊格式;非空 = 只留這幾筆,
// 不受 exe 旁 Data 是否已帶完整 templates 影響)。
bool patch_by_id(const std::wstring& path,
                 const std::vector<std::pair<std::string, std::string>>& templates, bool strip)
{
	std::string body;
	if (!read_bytes(path, body)) return false;
	try {
		auto doc = nlohmann::ordered_json::parse(body);
		if (strip) doc.erase("templates");
		if (!templates.empty()) {
			auto& t = doc["templates"];
			if (!t.is_object()) t = nlohmann::ordered_json::object();
			for (const auto& p : templates) t[p.first] = p.second;
		}
		return write_bytes(path, doc.dump(2));
	} catch (...) {
		return false;
	}
}

// 在字典複本放一份 id 檔(R15 用):寫入 file,並列進 meta.json 的 source_dicts。
bool add_source_dict(const std::wstring& localeDir, const char* file, const nlohmann::ordered_json& doc)
{
	const std::wstring wfile(file, file + strlen(file));
	if (!write_bytes(localeDir + wfile, doc.dump(2))) return false;
	std::string body;
	if (!read_bytes(localeDir + L"meta.json", body)) return false;
	try {
		auto meta = nlohmann::ordered_json::parse(body);
		auto& sd = meta["source_dicts"];
		if (!sd.is_array()) sd = nlohmann::ordered_json::array();
		bool have = false;
		for (const auto& x : sd)
			if (x.is_string() && x.get<std::string>() == file) have = true;
		if (!have) sd.push_back(file);
		return write_bytes(localeDir + L"meta.json", meta.dump(2));
	} catch (...) {
		return false;
	}
}

// 從字典複本拿掉幾個檔(R15 ID5 用):刪檔,並從 meta.json 的 source_dicts / load_order 移除,
// 造出「沒有這些 id 表」的字典,不管出貨字典有沒有它們。
bool drop_dict_files(const std::wstring& localeDir, const std::vector<std::string>& files)
{
	for (const std::string& f : files) {
		const std::wstring wf(f.begin(), f.end());
		DeleteFileW((localeDir + wf).c_str());
		if (GetFileAttributesW((localeDir + wf).c_str()) != INVALID_FILE_ATTRIBUTES) return false;
	}
	std::string body;
	if (!read_bytes(localeDir + L"meta.json", body)) return false;
	try {
		auto meta = nlohmann::ordered_json::parse(body);
		for (const char* field : {"source_dicts", "load_order"}) {
			if (!meta.contains(field) || !meta[field].is_array()) continue;
			auto& arr = meta[field];
			for (auto it = arr.begin(); it != arr.end();) {
				const bool drop = it->is_string() &&
				                  std::find(files.begin(), files.end(), it->get<std::string>()) != files.end();
				if (drop) it = arr.erase(it);
				else ++it;
			}
		}
		return write_bytes(localeDir + L"meta.json", meta.dump(2));
	} catch (...) {
		return false;
	}
}

// 改字典複本 meta.json 的 "source"(C1 用):poe1 / poe2。
bool set_meta_source(const std::wstring& localeDir, const char* source)
{
	std::string body;
	if (!read_bytes(localeDir + L"meta.json", body)) return false;
	try {
		auto meta = nlohmann::ordered_json::parse(body);
		meta["source"] = source;
		return write_bytes(localeDir + L"meta.json", meta.dump(2));
	} catch (...) {
		return false;
	}
}

// 刪掉 dir 底下所有 error-*.log、並把它們串成一個字串讀回(G 系列讀錯誤記錄用)。
void clear_error_logs(const std::wstring& dir)
{
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"error-*.log").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do { DeleteFileW((dir + fd.cFileName).c_str()); } while (FindNextFileW(h, &fd));
	FindClose(h);
}

std::string read_error_logs(const std::wstring& dir)
{
	std::string all;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW((dir + L"error-*.log").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return all;
	do {
		std::string body;
		if (read_bytes(dir + fd.cFileName, body)) all += body;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return all;
}

// 錯誤記錄裡有沒有「i18n 不一致」那一行:同一行要同時含 [i18n]、POB_GAME、source。
bool has_i18n_mismatch_line(const std::string& log)
{
	size_t pos = 0;
	while (pos <= log.size()) {
		size_t end = log.find('\n', pos);
		if (end == std::string::npos) end = log.size();
		const std::string line = log.substr(pos, end - pos);
		if (line.find("[i18n]") != std::string::npos && line.find("POB_GAME") != std::string::npos &&
		    line.find("source") != std::string::npos) return true;
		pos = end + 1;
	}
	return false;
}

} // namespace

int RunStatSelftest(const std::wstring& statsJsonArg)
{
	g_pass = g_fail = g_wait = 0;
	const std::wstring dir = exe_dir();
	const std::wstring shippedLocale = dir + L"Data\\poe1\\zh-rTW\\";
	const std::wstring statsJson = statsJsonArg.empty() ? shippedLocale + L"stats_by_id.json" : statsJsonArg;

	wchar_t tmp[MAX_PATH];
	GetTempPathW(MAX_PATH, tmp);
	const std::wstring root = std::wstring(tmp) + L"pobtools_stat_selftest\\";
	const std::wstring flagRoot = root + L"flag\\";      // POB_ZH_DATADIR(有 source_lookup)
	const std::wstring noflagRoot = root + L"noflag\\";  // POB_ZH_DATADIR(無 source_lookup)
	remove_tree(root);
	CreateDirectoryW(root.c_str(), nullptr);
	CreateDirectoryW(flagRoot.c_str(), nullptr);
	CreateDirectoryW(noflagRoot.c_str(), nullptr);

	printf("stat selftest - stats_by_id.json: %s\n", narrow(statsJson).c_str());
	printf("  dictionary copies under %s\n", narrow(root).c_str());
	std::string why;
	if (!build_dict_copy(shippedLocale, flagRoot + L"zh-rTW\\", statsJson, true, why) ||
	    !build_dict_copy(shippedLocale, noflagRoot + L"zh-rTW\\", statsJson, false, why)) {
		printf("  [SETUP FAILED] %s\n", why.c_str());
		remove_tree(root);
		return 2;
	}

	SetEnvironmentVariableW(L"POB_GAME", L"poe1");
	SetEnvironmentVariableW(L"POB_LOCALE", L"zh-rTW");
	SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
	translation_set_source(nullptr);
	translation_set_enabled(true);
	translation_reload();

	// 前提:引擎真的讀了複本(複本與出貨字典內容相同,所以只能確認有字典在作用)。
	{
		const char* chaos = translation_lookup("Chaos Orb");
		check(chaos != nullptr && std::string(chaos) != "Chaos Orb", "E0 測試字典複本已載入");
	}

	const char* kE2 = "+20 to maximum Life";
	const std::string kE2Want = u8"+20 最大生命";

	printf("\n=== 正向:數值填回 ===\n");
	// E1:題目指定的範例。⚠ 這句在 stat_descriptions.txt 另有 thread_of_hope 一譯
	// ({1:+d}%全元素抗性),依「篩後中文全同才採用」它是歧義 —— 見回報。
	expect("+(10-15)% to all Elemental Resistances", "item", u8"+(10-15)% 全部元素抗性",
	       "E1 物品行範圍值原樣填回(全部元素抗性)");
	expect("(10-15)% increased Global Accuracy Rating", "item", u8"增加 (10-15)% 全域命中值",
	       "E1b 物品行範圍值原樣填回(無歧義句)");
	expect(kE2, "item", kE2Want, "E2 {0:+d} 模板保留正號");

	// E3:同一 stat 的 1|#(increased)與 #|-1(reduced)兩句型各得各的中文。
	{
		const Said red = ask("10% reduced Global Accuracy Rating", "item");
		const Said inc = ask("10% increased Global Accuracy Rating", "item");
		const bool ok = !red.null && red.s == u8"減少 10% 全域命中值" &&
		                !inc.null && inc.s == u8"增加 10% 全域命中值";
		check(ok, "E3 negate 變體:reduced 行得 reduced 那句中文");
		if (!ok) printf("           reduced='%s' increased='%s'\n", shown(red), shown(inc));
	}

	expect("Adds 12 to 34 Fire Damage", "item", u8"附加 12 至 34 火焰傷害",
	       "E4 一行兩值依編號填入");
	// E4b:英文先出現 {1} 再 {0},中文反過來 —— 照出現順序填就會填反。
	expect("You take 450 Chaos Damage per second for 4 seconds on Kill", "item",
	       u8"殺死敵人後 4 秒內持續承受 450 混沌傷害", "E4b 中英語序相反時依編號對位");

	printf("\n=== 情境選描述檔 ===\n");
	{
		const Said p = ask("12% increased Accuracy Rating", "passive");
		const Said i = ask("12% increased Accuracy Rating", "item");
		const bool ok = !p.null && p.s == u8"增加 12% 命中率" && !i.null && i.s == u8"增加 12% 命中值";
		check(ok, "E5 passive 取天賦描述檔、item 取物品描述檔");
		if (!ok) printf("           passive='%s' item='%s'\n", shown(p), shown(i));
	}

	printf("\n=== 回空(皆附同環境有命中的前提) ===\n");
	const bool e2Hit = [&] { const Said s = ask(kE2, "item"); return !s.null && s.s == kE2Want; }();
	{
		const Said s = ask("+2 to Level of Socketed Gems", "item");
		check(s.null && e2Hit, "E6 篩後中文不一致(歧義)時回空");
		if (!s.null || !e2Hit) printf("           ambiguous='%s' guardE2=%d\n", shown(s), e2Hit);
	}
	{
		const Said a = ask("(Not supported in PoB yet)", "item");
		const Said b = ask("(Not supported in PoB yet)", "passive");
		const Said c = ask("Completely Made Up Stat Line 42", "item");
		check(a.null && b.null && c.null && e2Hit, "E7 查無 stat 鍵回空");
		if (!(a.null && b.null && c.null && e2Hit))
			printf("           item='%s' passive='%s' made-up='%s' guardE2=%d\n",
			       shown(a), shown(b), shown(c), e2Hit);
	}

	// E9:F2 關閉。
	{
		translation_set_enabled(false);
		const Said off = ask(kE2, "item");
		translation_set_enabled(true);
		const Said on = ask(kE2, "item");
		const bool ok = off.null && !on.null && on.s == kE2Want;
		check(ok, "E9 翻譯關閉(F2)時回空,開回後照常");
		if (!ok) printf("           off='%s' on='%s'\n", shown(off), shown(on));
	}

	printf("\n=== 多行與標記 ===\n");
	expect("15% chance to gain an Endurance, Frenzy or Power Charge when any\n"
	       "of your Traps are Triggered by an Enemy", "item",
	       u8"當你的陷阱被敵人觸發時，\n有 15% 機率獲得 1 顆耐力、狂怒或暴擊球",
	       "E10 兩行模板以真換行整段查得中文兩行");
	expect("25% chance to gain a Brine Charge instead of an Endurance Charge", "item",
	       u8"25% 機率在獲得1顆耐力球時改為獲得1顆海洋球",
	       "E11 [Tag|顯示] 標記行以顯示文字查得到");

	printf("\n=== 舊引擎相容 ===\n");
	{
		// E12a:出貨 meta.json(exe 旁的 Data)與測試複本都把 stats_by_id.json 只列在
		// source_dicts,不在 load_order —— 舊引擎只讀 load_order。
		std::string shipped, copy;
		bool ok = read_bytes(shippedLocale + L"meta.json", shipped) &&
		          read_bytes(flagRoot + L"zh-rTW\\meta.json", copy);
		bool notInOrder = false, listedShipped = false;
		try {
			const auto m = nlohmann::ordered_json::parse(shipped);
			const auto c = nlohmann::ordered_json::parse(copy);
			notInOrder = !in_array(m, "load_order", "stats_by_id.json") &&
			             !in_array(c, "load_order", "stats_by_id.json");
			listedShipped = in_array(m, "source_dicts", "stats_by_id.json");
		} catch (...) { ok = false; }
		check(ok && notInOrder, "E12a stats_by_id.json 不在 load_order");
		check(ok && listedShipped, "E12b 出貨 meta.json 的 source_dicts 列有 stats_by_id.json");

		// E12c:檔案已載入(E2 有命中)但 id 鍵不進全域表。
		const char* kKey = "base_maximum_life|#|stat_descriptions.txt|0";
		const char* r = translation_lookup(kKey);
		const bool kept = r == nullptr || std::string(r) == kKey;
		check(kept && e2Hit, "E12c 全域 exact 查 stat 鍵得不到中文(id 鍵不進全域表)");
		if (!(kept && e2Hit)) printf("           global='%s' guardE2=%d\n", r ? r : "(null)", e2Hit);
	}

	// ---- 稽核 r4(tools/ggpk_zh/audit_prod/report_r4)的重現 -----------------
	printf("\n=== 稽核 r4 重現 ===\n");
	// F1:天賦核心(keystone)的說明是 passive_skill_stat_descriptions.txt 多行模板的
	// 其中一行;同英文在 stat_descriptions.txt 另有別的 stat(base_cannot_evade 等)。
	// passive 情境應取 keystone 模板對應那一行的中文(misjoin_r4.tsv 第 1~7 列)。
	expect("Cannot Evade enemy Attacks", "passive", u8"不能閃避敵人攻擊",
	       "F1a keystone 行取天賦描述檔(不能閃避敵人攻擊,非 base_cannot_evade)");
	expect("Removes all mana", "passive", u8"移除全部魔力",
	       "F1b keystone 行取天賦描述檔(移除全部魔力,非 base_no_mana)");
	expect("20% less Effect of your Curses", "passive", u8"20% 更少你的詛咒效果",
	       "F1c keystone 行取天賦描述檔(咒術大師,非 curse_pillar)");
	expect("You take 65% of Damage from Blocked Hits", "passive", u8"你承受 65% 已格擋擊中的傷害",
	       "F1d keystone 行取天賦描述檔(側身之擊,非 base_block_%_damage_taken)");

	// F2:模板本身的連續空白照抄(regress_r4.tsv:mace_elemental_damage_+%、
	// display_trigger_arcane_wake_after_spending_200_mana_%_chance)。
	expect("12% increased Elemental Damage with Maces or Sceptres", "passive",
	       u8"錘和權杖增加  12% 元素傷害", "F2a 填值保留模板的兩個連續空白(錘和權杖)");
	expect("Trigger Level 20 Arcane Wake after Spending a total of 200 Mana", "item",
	       u8"消耗總計 200 魔力時觸發等級  20 的秘能喚醒", "F2b 填值保留模板的兩個連續空白(秘能喚醒)");

	// F3:引擎只收 stat_descriptions.txt 與 passive_skill_stat_descriptions.txt。
	// 只存在於 skill_stat_descriptions.txt 的句子查不到;兩檔同英文不同譯時用物品檔。
	{
		const Said a = ask("30% chance to gain a Frenzy Charge when a Mine is Detonated targeting an Enemy", "item");
		const Said b = ask("30% chance to gain a Frenzy Charge when a Mine is Detonated targeting an Enemy", "passive");
		check(a.null && b.null && e2Hit, "F3a 只在 skill_stat_descriptions 的句子不採用");
		if (!(a.null && b.null && e2Hit))
			printf("           item='%s' passive='%s' guardE2=%d\n", shown(a), shown(b), e2Hit);
	}
	expect("20% less Critical Strike Chance", "item", u8"更少 20% 暴擊率",
	       "F3b 同英文時取 stat_descriptions 譯法(非 skill_stat 的「20% 更少暴擊率」)");

	// F4:GGPK 模板的 "\n" 讓單行英文得到兩行中文(Guardian / Bastion of Hope)。
	// translation_lookup_stat 只回文字(F4a);逐行補色碼由 translation_lookup_stat_colored
	// 負責(F4b/F4c),ui_api.cpp 的 Lua 綁定 PobToolsTranslateStat 只是轉呼叫它。
	expect("If you've Attacked Recently, you and nearby Allies have +10% Chance to Block Attack Damage",
	       "passive", u8"若你近期有攻擊，你和\n附近友方 +10% 攻擊傷害格擋率",
	       "F4a 單行英文對到含 \\n 的模板時 C 層回真換行兩行");
	// F4b / F4c:translation_lookup_stat_colored(Lua 綁定改呼叫它)。
	{
		const char* r = translation_lookup_stat_colored(
		    "^x8888FFIf you've Attacked Recently, you and nearby Allies have +10% Chance to Block Attack Damage",
		    "passive");
		const std::string want = u8"^x8888FF若你近期有攻擊，你和\n^x8888FF附近友方 +10% 攻擊傷害格擋率";
		const bool ok = r && want == r;
		check(ok, "F4b 帶色碼的行命中多行模板時每行都補同一色碼");
		if (!ok) printf("           got  '%s'\n           want '%s'\n", r ? r : "(null)", want.c_str());
	}
	{
		// 無色碼:命中、多行命中、查不到三種都要與 translation_lookup_stat 相同。
		const char* lines[][2] = {
		    {"If you've Attacked Recently, you and nearby Allies have +10% Chance to Block Attack Damage", "passive"},
		    {"+20 to maximum Life", "item"},
		    {"(Not supported in PoB yet)", "item"}};
		bool ok = true;
		for (auto& l : lines) {
			const Said plain = ask(l[0], l[1]);
			const char* c = translation_lookup_stat_colored(l[0], l[1]);
			const bool same = plain.null ? c == nullptr : (c && plain.s == c);
			if (!same) {
				ok = false;
				printf("           %s: plain='%s' colored='%s'\n", l[0], shown(plain), c ? c : "(null)");
			}
		}
		check(ok && e2Hit, "F4c 無色碼輸入時與 translation_lookup_stat 結果相同");
	}

	// ---- R:code review 確認的實作問題 ----------------------------------------
	printf("\n=== R code review 重現 ===\n");
	// R1:colored 的回傳指標要和 translation_lookup_stat 一樣活到下次 reload。
	// 結果遠超過 SSO 長度,快取格若每次被新字串 move 覆寫,第一次的指標就懸空。
	{
		const char* kLine =
		    "^x8888FFIf you've Attacked Recently, you and nearby Allies have +10% Chance to Block Attack Damage";
		const char* p1 = translation_lookup_stat_colored(kLine, "passive");
		const std::string copy = p1 ? p1 : "";
		const char* p2 = translation_lookup_stat_colored(kLine, "passive");
		const bool samePtr = p1 && p1 == p2;
		// 照指示讀第一次的指標;若已懸空,這是讀已釋放的堆積(Windows 堆積上不會崩,
		// 但值不可靠),所以同時以 samePtr 判定。
		const bool sameText = p1 && copy.size() > 32 && strcmp(p1, copy.c_str()) == 0;
		check(samePtr && sameText, "R1 colored 連查兩次:指標相同且第一次指標內容不變");
		if (!(samePtr && sameText))
			printf("           p1=%p p2=%p len=%zu samePtr=%d sameText=%d\n", (const void*)p1,
			       (const void*)p2, copy.size(), samePtr, sameText);
	}
	// R2:平手時比 stat 數,行內數值只算對到英文 {N} 的值;模板寫死的數字(4 seconds)
	// 不算。候選(item):
	//   chance_to_gain_onslaught_on_kill_% onslaught_time_granted_on_kill_ms|1|# 0   2 stat
	//       "{0}% chance to gain Onslaught for 4 seconds on Kill" → 擊殺時 {0}% 機率獲得 4 秒猛攻
	//   chance_to_gain_onslaught_on_kill_% onslaught_time_granted_on_kill_ms|1|# 1|#  2 stat
	//       "{0}% chance ... for {1} seconds on Kill" → 擊殺時 {0}% 機率獲得 {1} 秒猛攻
	//   chance_to_gain_onslaught_on_kill_for_4_seconds_%|1|99                       1 stat
	//       → 擊殺時有 {0}% 機率獲得 4 秒猛攻效果
	// 只有 10 對到所有候選的 {N}(4 在兩個候選是寫死數字),值數 = 1 → 取 1-stat 那筆。
	expect("10% chance to gain Onslaught for 4 seconds on Kill", "item",
	       u8"擊殺時有 10% 機率獲得 4 秒猛攻效果", "R2 平手時寫死數字不算值(4 秒猛攻取 1-stat 譯法)");
	// R2b:同型的 10 秒版(chance_to_gain_onslaught_on_kill_for_10_seconds_%|1|99)。
	expect("13% chance to gain Onslaught for 10 seconds on Kill", "item",
	       u8"擊殺時有 13% 機率獲得猛攻 10 秒", "R2b 平手時寫死數字不算值(10 秒猛攻取 1-stat 譯法)");

	// ---- P0:依 id 查譯名時比對英文(軍團珠寶/刺青換名不換 id)-------------------
	// 英文取自 GGPK(ggpk_zh.sqlite cell passiveskills.Name / mods.Name,與 POB 3_29
	// tree.lua 的 name 相同):94 Evasion→閃避;33287 Juggernaut→戰神(天賦);
	// 17765 Juggernaut→勇士(昇華);AbyssAccuracyRatingJewel1 "of Calm"→平靜之。
	printf("\n=== P0 依 id 查譯名需英文相符 ===\n");
	{
		const std::wstring tRoot = root + L"p0t\\";   // by_id 檔帶 templates
		const std::wstring lRoot = root + L"p0l\\";   // 舊格式：無 templates
		CreateDirectoryW(tRoot.c_str(), nullptr);
		CreateDirectoryW(lRoot.c_str(), nullptr);
		std::string w;
		bool setup = build_dict_copy(shippedLocale, tRoot + L"zh-rTW\\", statsJson, true, w) &&
		             build_dict_copy(shippedLocale, lRoot + L"zh-rTW\\", statsJson, true, w);
		setup = setup &&
		        patch_by_id(tRoot + L"zh-rTW\\passives_by_id.json",
		                    {{"6", "Twin Terrors"}, {"94", "Evasion"},
		                     {"33287", "Juggernaut"}, {"17765", "Juggernaut"}}, true) &&
		        patch_by_id(tRoot + L"zh-rTW\\affixes_by_id.json",
		                    {{"AbyssAccuracyRatingJewel1", "of Calm"},
		                     {"AbyssAccuracyRatingJewel2", "of Steadiness"}}, true) &&
		        patch_by_id(lRoot + L"zh-rTW\\passives_by_id.json", {}, true) &&
		        patch_by_id(lRoot + L"zh-rTW\\affixes_by_id.json", {}, true);
		check(setup, "P0-0 測試用 by_id 複本(含/不含 templates)已建立");
		if (!setup) printf("           %s\n", w.c_str());

		auto en = [](const char* src, const char* id, const char* e) {
			const char* r = translation_lookup_by_id_en(src, id, e);
			return r ? std::string(r) : std::string("(null)");
		};
		auto byId = [](const char* src, const char* id) {
			const char* r = translation_lookup_by_id(src, id);
			return r ? std::string(r) : std::string("(null)");
		};
		auto expectEq = [](const std::string& got, const std::string& want, const char* what) {
			if (got != want) printf("           %s got '%s' want '%s'\n", what, got.c_str(), want.c_str());
			return got == want;
		};

		// --- 帶 templates 的複本(templates 只含上面幾筆;id 33 刻意沒有)---
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", tRoot.c_str());
		translation_set_source(nullptr);
		translation_reload();
		{
			const bool miss = expectEq(en("passives", "94", "Accuracy"), "(null)", "94/Accuracy");
			const bool guard = expectEq(en("passives", "94", "Evasion"), u8"閃避", "94/Evasion");
			check(setup && miss && guard, "P0-1 英文與 id 不符(換名節點)時不回原節點譯名");
		}
		{
			const bool a = expectEq(en("passives", "94", "Evasion"), u8"閃避", "94/Evasion");
			const bool b = expectEq(en("passives", "33287", "Juggernaut"), u8"戰神", "33287/Juggernaut");
			const bool c = expectEq(en("passives", "17765", "Juggernaut"), u8"勇士", "17765/Juggernaut");
			check(setup && a && b && c, "P0-2 英文相符時回 id 譯名，同名不同 id 各回各的");
		}
		{
			const bool hit = expectEq(en("affixes", "AbyssAccuracyRatingJewel1", "of Calm"), u8"平靜之",
			                          "Jewel1/of Calm");
			const bool miss = expectEq(en("affixes", "AbyssAccuracyRatingJewel1", "of Steadiness"), "(null)",
			                           "Jewel1/of Steadiness");
			check(setup && hit && miss, "P0-2b affixes 同理：英文相符才回譯名");
		}
		{
			// 契約：去頭尾空白、內部空白正規化、大小寫敏感;templates 沒有的 id 不回。
			const bool ws = expectEq(en("passives", "6", "  Twin   Terrors "), u8"雙重懼刃", "6/'  Twin   Terrors '");
			const bool caseMiss = expectEq(en("passives", "94", "evasion"), "(null)", "94/evasion");
			const bool noTmpl = expectEq(en("passives", "33", "Lord of Drought"), "(null)", "33(無 template)");
			check(setup && ws && caseMiss && noTmpl,
			      "P0-3 英文比對去空白、大小寫敏感;templates 無此 id 時回空");
		}
		{
			bool ok = setup;
			for (auto& q : std::vector<std::pair<const char*, const char*>>{
			         {"passives", "94"}, {"passives", "33287"}, {"passives", "33"},
			         {"passives", "99999999"}, {"affixes", "AbyssAccuracyRatingJewel1"}, {"affixes", "NoSuchModXyz"}}) {
				const std::string a = en(q.first, q.second, nullptr);
				const std::string b = byId(q.first, q.second);
				if (!expectEq(a, b, q.second)) ok = false;
			}
			ok = ok && byId("passives", "94") == u8"閃避";   // 前提:id 查詢本身有命中
			check(ok, "P0-5 en 為 nullptr 時等同 translation_lookup_by_id");
		}

		// --- 舊格式(無 templates)的複本 ---
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", lRoot.c_str());
		translation_reload();
		{
			const bool a = expectEq(en("passives", "94", "Accuracy"), u8"閃避", "legacy 94/Accuracy");
			const bool b = expectEq(en("affixes", "AbyssAccuracyRatingJewel1", "anything"), u8"平靜之",
			                        "legacy Jewel1/anything");
			const bool c = expectEq(en("passives", "94", nullptr), byId("passives", "94"), "legacy 94/nullptr");
			check(setup && a && b && c, "P0-4 舊格式無 templates 時沿用 id 譯名(相容舊資料包)");
		}

		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();
		remove_tree(tRoot);
		remove_tree(lRoot);
	}

	// ---- P1:寶石情境(ctx "gem")與寶石說明 id 字典(gems_desc)--------------------
	// POB 寶石提示用的描述檔(Data/Skills/*.lua 的 statDescriptionScope + 各檔 parent 鏈):
	//   skill / gem / active_skill_gem / minion_skill / minion_spell / minion_spell_damage /
	//   single_minion_spell / minion_attack / aura / banner_aura / buff / secondary_debuff /
	//   debuff / curse / brand / beam / offering / variable_duration _stat_descriptions,
	//   最終 parent 為 stat_descriptions。全部都在 stats_by_id.json 的 src_file 裡。
	printf("\n=== P1 寶石情境 ===\n");
	{
		// P1-3a:同英文 "Fires 1 Projectile" 在 gem 情境有三個候選(中文都不同):
		//   base_number_of_projectiles|1|active_skill_gem_stat_descriptions.txt|1 "Fires 1 Projectile" → 發射1個投射物
		//   base_number_of_projectiles|1|active_skill_gem_stat_descriptions.txt|0 "Fires {0:+d} Projectile"(gem_quality)→ 發射+1個投射物
		//   power_siphon_fire_at_all_targets ...|0 1 0 # ...|skill_stat_descriptions.txt|0 "Fires 1 Projectile" → 發射 1 投射物
		// 純文字分不出 —— 由 POB describeStats 的 lineMap 給的 stat id 縮小。
		{
			const char* r = translation_lookup_stat_ids("Fires 1 Projectile", "gem", "base_number_of_projectiles");
			const bool ok = r && std::string(r) == u8"發射1個投射物";
			check(ok, "P1-3a gem + stat id:Fires 1 Projectile 取 base_number_of_projectiles 的非品質句");
			if (!ok) printf("           got '%s' want '發射1個投射物'\n", r ? r : "(null)");
		}
		{
			// 不帶 ids:可回空或三者之一，但不得含佔位符。
			const Said g = ask("Fires 1 Projectile", "gem");
			const bool ok = g.null || g.s == u8"發射1個投射物" || g.s == u8"發射+1個投射物" || g.s == u8"發射 1 投射物";
			check(ok, "P1-3a' 不帶 stat id 時回空或三候選之一(不含佔位符)");
			if (!ok) printf("           got '%s'\n", shown(g));
		}
		// P1-3e:POB StatDescriber 對 base_number_of_projectiles 的四個變體(active_skill_gem 第 125 塊):
		//   [1] gem_quality limit 1|1  "Fires {0:+d} Projectile"   [2] gem_quality limit 2|#  "Fires {0:+d} Projectiles"
		//   [3]             limit 1|1  "Fires 1 Projectile"         [4]             limit 2|#  "Fires {0} Projectiles"
		// stats_by_id:...|2|#|active_skill_gem_stat_descriptions.txt|0 → 發射{0:+d}個投射物(品質句)
		//              ...|2|#|active_skill_gem_stat_descriptions.txt|1 → 發射{0}個投射物
		{
			const char* ids = "base_number_of_projectiles";
			const char* plain = translation_lookup_stat_ids("Fires 2 Projectiles", "gem", ids);
			const char* qual = translation_lookup_stat_ids("Fires +2 Projectiles", "gem", ids);
			const bool ok = plain && std::string(plain) == u8"發射2個投射物" &&
			                qual && std::string(qual) == u8"發射+2個投射物";
			check(ok, "P1-3e gem + stat id:複數句依正號選一般句 / 品質句(+d)");
			if (!ok) printf("           'Fires 2 Projectiles'='%s' 'Fires +2 Projectiles'='%s'\n",
			                plain ? plain : "(null)", qual ? qual : "(null)");
		}
		{
			// stat_ids 為空或 nullptr 時等同 translation_lookup_stat(用已綠的 E2 句當前提)。
			const char* a = translation_lookup_stat_ids("+20 to maximum Life", "item", nullptr);
			const char* b = translation_lookup_stat_ids("+20 to maximum Life", "item", "");
			const Said c = ask("+20 to maximum Life", "item");
			const bool ok = !c.null && a && b && c.s == a && c.s == b;
			check(ok, "P1-3f stat_ids 為空時等同 translation_lookup_stat");
			if (!ok) printf("           null='%s' empty='%s' plain='%s'\n", a ? a : "(null)", b ? b : "(null)", shown(c));
		}
		// P1-3b:critical_strike_multiplier_+_per_power_charge|#|skill_stat_descriptions.txt|0
		//   "每顆暴擊球 {0:+d}% 暴擊加成"(stat_descriptions 版是「每個暴擊球」;寶石檔優先)。
		expect("+20% to Critical Strike Multiplier per Power Charge", "gem", u8"每顆暴擊球 +20% 暴擊加成",
		       "P1-3b gem 情境：正號與數值正確，寶石描述檔優先於 stat_descriptions");
		// P1-3c:%_chance_to_gain_frenzy_charge_on_mine_detonated_targeting_an_enemy|#|gem_stat_descriptions.txt|0
		expect("20% chance to gain a Frenzy Charge when a Mine from Supported\nSkills is Detonated targeting an Enemy",
		       "gem", u8"當透過輔助技能製造的地雷對敵人引爆時，\n有 20% 機率獲得 1 顆狂怒球",
		       "P1-3c gem 情境：狂怒球地雷多行句得狂怒球版");
		// P1-3d:item / passive 的篩選不變由 E1~E11、F1~F3 持續涵蓋;這裡再釘一句
		//   只在 skill_stat 的句子,item 情境仍不採用(與 F3a 同義,索引擴大後最易破)。
		{
			const Said a = ask("30% chance to gain a Frenzy Charge when a Mine is Detonated targeting an Enemy", "item");
			const Said g = ask("30% chance to gain a Frenzy Charge when a Mine is Detonated targeting an Enemy", "gem");
			const bool ok = a.null && !g.null && g.s == u8"當地雷對敵人引爆時，有 30% 機率獲得 1 顆狂怒球";
			check(ok, "P1-3d 寶石檔只在 gem 情境採用,item 情境不受索引擴大影響");
			if (!ok) printf("           item='%s' gem='%s'\n", shown(a), shown(g));
		}
	}
	{
		// P1-1:gems_desc_by_id.json(by_id 格式含 templates),鍵 = POB grantedEffect id(Data/Skills/act_int.lua 的
		// skills["Fireball"] / skills["FrostBolt"]);值 = GGPK activeskills.Description(經
		// grantedeffects.ActiveSkill;ggpk_zh.sqlite cell version 1 的 fireball / frost_bolt)。
		const std::wstring gRoot = root + L"p1g\\";
		CreateDirectoryW(gRoot.c_str(), nullptr);
		std::string w;
		bool setup = build_dict_copy(shippedLocale, gRoot + L"zh-rTW\\", statsJson, true, w);
		const char* kFireEn = "Unleashes a ball of fire towards a target which explodes, damaging nearby foes.";
		const char* kFrostEn = "Fires a slow-moving projectile that pierces through enemies, dealing cold damage.";
		const std::string kFireZh = u8"釋放一顆球型火焰向前飛射，接觸到怪物時會爆炸並對周圍敵人造成傷害";
		const std::string kFrostZh = u8"發射出一個緩慢移動，會穿透敵人的投射物，造成冰冷傷害。";
		if (setup) {
			nlohmann::ordered_json doc = nlohmann::ordered_json::object();
			doc["source_files"] = nlohmann::ordered_json::array({"ggpk:activeskills.Description by grantedEffect id"});
			doc["is_base_items"] = false;
			doc["entries"] = {{"Fireball", kFireZh}, {"FrostBolt", kFrostZh}};
			doc["templates"] = {{"Fireball", kFireEn}, {"FrostBolt", kFrostEn}};
			setup = write_bytes(gRoot + L"zh-rTW\\gems_desc_by_id.json", doc.dump(2));
			std::string mb;
			try {
				setup = setup && read_bytes(gRoot + L"zh-rTW\\meta.json", mb);
				auto meta = nlohmann::ordered_json::parse(mb);
				if (!in_array(meta, "source_dicts", "gems_desc_by_id.json"))
					meta["source_dicts"].push_back("gems_desc_by_id.json");
				setup = setup && write_bytes(gRoot + L"zh-rTW\\meta.json", meta.dump(2));
			} catch (...) { setup = false; }
		}
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", gRoot.c_str());
		translation_set_source(nullptr);
		translation_reload();
		auto en = [](const char* id, const char* e) {
			const char* r = translation_lookup_by_id_en("gems_desc", id, e);
			return r ? std::string(r) : std::string("(null)");
		};
		const std::string a = en("Fireball", kFireEn);
		const std::string b = en("FrostBolt", kFrostEn);
		const std::string c = en("Fireball", kFrostEn);   // 英文不符
		const bool ok = setup && a == kFireZh && b == kFrostZh && c == "(null)";
		check(ok, "P1-1 gems_desc:id 加正確英文回繁中，英文不符回空");
		if (!ok) printf("           setup=%d Fireball='%s' FrostBolt='%s' mismatch='%s' %s\n", setup,
		                a.c_str(), b.c_str(), c.c_str(), w.c_str());
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();
		remove_tree(gRoot);
	}

	// ---- P4:星團附魔「Added Small Passive Skills grant: X」 -------------------------
	// GGPK 沒有含此前綴的 stat 模板(stats_by_id 只有 "...also grant: ..." 的
	// local_affliction_jewel_small_nodes_grant_* 與 "...grant Nothing");前綴來自
	// clientstrings StatDescripotionTreeExpansionJewelGrantedSmallStat:
	//   "Added Small Passive Skills grant: {0}" → "附加的小型天賦給予：{0}",
	// {0} 是小天賦的 stat 行(POB Data/ClusterJewels.lua 的 enchant 字面值)。
	// 期望 = 前綴譯文 + 內層 stat 行依 stats_by_id 的譯文。
	printf("\n=== P4 星團附魔前綴 ===\n");
	// P4-1(現況半形冒號「附加的小型天賦給予: 」,37 列):內層 area_damage_+%|1|#|stat_descriptions.txt|0
	expect("Added Small Passive Skills grant: 10% increased Area Damage", "item",
	       u8"附加的小型天賦給予：增加 10% 範圍傷害", "P4-1 星團附魔前綴用全形冒號(原半形冒號類)");
	// P4-2(現況缺「型」「附加的小天賦給予：」,8 列):內層 additional_dexterity / base_dexterity → {0:+d} 敏捷
	expect("Added Small Passive Skills grant: +10 to Dexterity", "item",
	       u8"附加的小型天賦給予：+10 敏捷", "P4-2 星團附魔前綴為「附加的小型天賦給予」(原缺「型」類)");
	// P4-3(現況英文,14 列):內層 burn_damage_+%|1|#|stat_descriptions.txt|0
	expect("Added Small Passive Skills grant: 12% increased Burning Damage", "item",
	       u8"附加的小型天賦給予：增加 12% 燃燒傷害", "P4-3 星團附魔(原英文類)以前綴 + 內層 stat 翻譯");

	// ---- P5:名稱槽(passive_hash / display_indexable_skill)-------------------------
	// 模板的 {N} 是查表型 handler,值為名稱：以 passives / gems 來源把名稱翻成中文。
	printf("\n=== P5 名稱槽 ===\n");
	// P5-2:unique_jewel_grants_notable_hash_part_1|#|stat_descriptions.txt|0(handler passive_hash)
	//   "Allocates {0} if you have the matching modifier on Forbidden Flesh" → "若禁忌血肉上有符合的詞綴，配置 {0}"
	//   Ancestral Fury = passiveskills AscendancyBerserker10 → 先祖之怒
	expect("Allocates Ancestral Fury if you have the matching modifier on Forbidden Flesh", "item",
	       u8"若禁忌血肉上有符合的詞綴，配置 先祖之怒", "P5-2 禁忌血肉：天賦名稱槽填入繁中名");
	// P5-3:random_skill_gem_level_+_level random_skill_gem_level_+_index|1|# #|stat_descriptions.txt|0
	//   (handler display_indexable_skill 於 {1})"+{0} to Level of all {1} Gems" → "全部 {1} 寶石等級 +{0}"
	//   Pact of Beidat = activeskills pact_of_beidat DisplayedName → 貝達特誓約
	expect("+3 to Level of all Pact of Beidat Gems", "item",
	       u8"全部 貝達特誓約 寶石等級 +3", "P5-3 寶石等級：技能名稱槽填入繁中名");

	// ---- FC:passive 情境含 passive_skill_aura_stat_descriptions.txt ----------------
	// coverage_r3 cat_08(天賦 40059 / 47630):光環型天賦行只在天賦光環描述檔。
	printf("\n=== FC passive 光環描述檔 ===\n");
	{
		// inquisitor_aura_elemental_damage_+%_final|#|-1|passive_skill_aura_stat_descriptions.txt|0
		//   "Nearby Enemies deal {0}% less Elemental Damage" → "附近敵人造成 {0}% 更少元素傷害"
		// hit_damage_+%_while_chilled|#|-1|passive_skill_aura_stat_descriptions.txt|0
		//   "Nearby Chilled Enemies deal {0}% reduced Damage with Hits" → "附近被冰緩的敵人減少 {0}% 擊中傷害"
		const Said a = ask("Nearby Enemies deal 8% less Elemental Damage", "passive");
		const Said b = ask("Nearby Chilled Enemies deal 10% reduced Damage with Hits", "passive");
		const bool ok = !a.null && a.s == u8"附近敵人造成 8% 更少元素傷害" &&
		                !b.null && b.s == u8"附近被冰緩的敵人減少 10% 擊中傷害";
		check(ok, "FC-1 passive 情境取天賦光環描述檔模板");
		if (!ok) printf("           a='%s' b='%s'\n", shown(a), shown(b));
	}

	// ---- FA:名稱槽寶石名先查 gem_names 來源 ------------------------------------
	// coverage_r3 cat_10 Pearl of Tsoatha:「…等級 20 的附加混沌傷害輔助輔助」—— 名稱槽
	// 填了 gems 來源的「…輔助」,模板本身又帶「輔助」。gem_names.json(扁平，鍵 = GGPK
	// 寶石顯示名英文，值 = 不含「輔助」的繁中)應先於 gems 來源 / 全域。
	// 模板:local_pearl_random_support_gem_1_slot_index …_index …_level|3 # #(Boots)/|2 # #(Gloves)
	//   "Skills Socketed in your Boots are Supported by level {2} {1}" → "鑲嵌於你的鞋子中的技能會受到等級 {2} 的{1}輔助"
	//   handler display_indexable_non_active_support 於 {1}
	// 名稱繁中:GGPK indexablenonactivesupportgems.Name「Added Chaos Damage」→附加混沌傷害、
	//   「Added Cold Damage」→附加冰冷傷害(baseitemtypes 是「…輔助」)。
	printf("\n=== FA 名稱槽寶石名 ===\n");
	{
		const std::wstring gnRoot = root + L"fag\\";
		CreateDirectoryW(gnRoot.c_str(), nullptr);
		std::string w;
		bool setup = build_dict_copy(shippedLocale, gnRoot + L"zh-rTW\\", statsJson, true, w);
		if (setup) {
			nlohmann::ordered_json d = nlohmann::ordered_json::object();
			d["source_files"] = nlohmann::ordered_json::array({"ggpk:indexable support gem names"});
			d["is_base_items"] = false;
			d["entries"] = {{"Added Chaos Damage", u8"附加混沌傷害"}, {"Added Cold Damage", u8"附加冰冷傷害"}};
			setup = write_bytes(gnRoot + L"zh-rTW\\gem_names.json", d.dump(2));
			std::string mb;
			try {
				setup = setup && read_bytes(gnRoot + L"zh-rTW\\meta.json", mb);
				auto meta = nlohmann::ordered_json::parse(mb);
				if (!in_array(meta, "source_dicts", "gem_names.json")) meta["source_dicts"].push_back("gem_names.json");
				setup = setup && write_bytes(gnRoot + L"zh-rTW\\meta.json", meta.dump(2));
			} catch (...) { setup = false; }
		}
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", gnRoot.c_str());
		translation_set_source(nullptr);
		translation_reload();
		const Said a = ask("Skills Socketed in your Boots are Supported by level 20 Added Chaos Damage", "item");
		const Said b = ask("Skills Socketed in your Gloves are Supported by level 20 Added Cold Damage", "item");
		const bool noDouble = a.s.find(u8"輔助輔助") == std::string::npos && b.s.find(u8"輔助輔助") == std::string::npos;
		const bool ok = setup && noDouble &&
		                !a.null && a.s == u8"鑲嵌於你的鞋子中的技能會受到等級 20 的附加混沌傷害輔助" &&
		                !b.null && b.s == u8"鑲嵌於你的手套中的技能會受到等級 20 的附加冰冷傷害輔助";
		check(ok, "FA-1 寶石名稱槽先查 gem_names:不出現「輔助輔助」");
		if (!ok) printf("           setup=%d boots='%s' gloves='%s' %s\n", setup, shown(a), shown(b), w.c_str());
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();
		remove_tree(gnRoot);
	}

	// ---- FR:audit r9(report_r9/regress_r9.tsv)--------------------------------
	// ---- RV-1:精確條目值裡的 '#' 是資料，不是未填格 ------------------------------
	// 出貨字典 "Charge on crit %:" → "暴擊時 #% 充能:"(poe1 ui.json L15320 / passives.json
	// L9769;poe2 passives.json L7396,load_order 最後,蓋過 ui.json 的「暴擊充能機率:」)。
	// 精確條目只檢查未填 {N}/{};'#' 規則只套在填值產出的結果(P2-5)。
	printf("\n=== RV-1 精確條目的 # ===\n");
	{
		translation_set_source(nullptr);
		const char* a = translation_lookup("Charge on crit %:");
		const bool ok = a && std::string(a) == u8"暴擊時 #% 充能:";
		check(ok, "RV-1a poe1 精確條目值含 # 照常輸出");
		if (!ok) printf("           got '%s'\n", a ? a : "(null)");
	}
	{
		// RV-1b:PoE2 出貨字典(無 source_lookup)。
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", nullptr);
		SetEnvironmentVariableW(L"POB_GAME", L"poe2");
		translation_set_source(nullptr);
		translation_reload();
		const char* b = translation_lookup("Charge on crit %:");
		const std::string bs = b ? b : "(null)";
		SetEnvironmentVariableW(L"POB_GAME", L"poe1");
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();
		check(bs == u8"暴擊時 #% 充能:", "RV-1b poe2 精確條目值含 # 照常輸出");
		if (bs != u8"暴擊時 #% 充能:") printf("           got '%s'\n", bs.c_str());
	}
	{
		// RV-1c:填值產出的殘留 '#' 仍擋(同 P2-5 那句)。
		translation_set_source(nullptr);
		const char* kEn = "1% Blood in the Blood Crucible while you are able to Shift into Nightmare";
		const char* r = translation_lookup(kEn);
		const bool ok = !r || std::string(r).find('#') == std::string::npos;
		check(ok, "RV-1c 填值結果殘留 # 仍視為未命中");
		if (!ok) printf("           got '%s'\n", r);
	}

	// ---- coverage_r4「一之一」可修(引擎)項 ----------------------------------------
	printf("\n=== coverage_r4 E2/E4/E5/N1 ===\n");
	// E2-1:兩個候選只差大小寫且中文不同 → 現行大小寫不敏感判成歧義:
	//   local_display_socketed_gems_get_chance_to_bleed_level "…supported by Level {0} Chance to Bleed" → 插槽中的寶石被等級 {0} 的機率流血輔助
	//   local_display_socketed_gems_supported_by_level_x_chance_to_bleed "…Supported by Level {0} Chance To Bleed" → 插槽中寶石被等級 {0} 的機率流血輔助
	// 規則：英文字面大小寫完全相同者優先。
	expect("Socketed Gems are Supported by Level 16 Chance To Bleed", "item",
	       u8"插槽中寶石被等級 16 的機率流血輔助", "E2-1 大小寫完全相同的候選優先(Chance To Bleed)");
	// E2-2:elemental_damage_taken_+%_while_stationary(小寫 taken)→ 站立時，減少 {0}% 承受元素傷害
	//      essence_display_elemental_damage_taken_while_not_moving_+%(大寫 Taken)→ 站立時受到的元素傷害減少 {0}%
	expect("5% reduced Elemental Damage Taken while stationary", "item",
	       u8"站立時受到的元素傷害減少 5%", "E2-2 大小寫完全相同的候選優先(Taken while stationary)");
	// E2-3:minion_maximum_life_%_to_convert_to_maximum_energy_shield_per_1%_chaos_resistance|1|#
	//   英文模板兩行(…Maximum Energy\nShield per 1%…),POB 給單行(兩行以空白相接)。
	expect("Minions Convert 2% of their Maximum Life to Maximum Energy Shield per 1% Chaos Resistance they have", "item",
	       u8"召喚物每 1% 混沌抗性，\n將他們 2% 的最大生命轉化為最大能量護盾", "E2-3 單行英文對到兩行模板(以空白相接)");
	// E4-1:光環檔小寫 rate 的 life_regeneration_rate_+%|#|-1|passive_skill_aura… → 附近敵人減少 {0}% 生命回復
	//   (天賦檔大寫 Rate 的 life_mastery_… 是另一個 stat;FR-2 釘住大寫那句)
	expect("Nearby Enemies have 100% reduced Life Regeneration rate", "passive",
	       u8"附近敵人減少 100% 生命回復", "E4-1 小寫 rate 取光環檔模板(大小寫完全相同優先)");
	expect("Nearby Enemies have 50% reduced Life Regeneration Rate", "passive",
	       u8"附近敵人減少 50% 生命恢復率", "E4-2 大寫 Rate 仍取天賦檔專精模板(同 FR-2)");
	// E5:寶石 stat 行在 gem tier 有 skill_stat 與 active_skill_gem / gem_stat 兩份不同中文 → 歧義回空。
	// POB scopeList:skill_stat → active_skill_gem → gem_stat → stat(越具體越先),主動技能取 skill_stat。
	{
		struct G { const char* en; const char* ids; const char* want; const char* name; };
		const G gs[] = {
		    {"1 maximum Beam Target", "static_strike_number_of_beam_targets", u8"最大光束目標數為1",
		     "E5-1 光束目標數取 skill_stat 模板(非 active_skill_gem)"},
		    {"During initial Duration, can be hit by your Projectiles up to 20 times", "tornado_maximum_number_of_hits",
		     u8"在初始持續時間內，最多可以被你的投射物擊中 20 次", "E5-2 龍捲風擊中次數取 skill_stat 模板"},
		    {"This Skill's Arrows deal 108% more Damage with Hits per Stage", "snipe_triggered_skill_hit_damage_+%_final_per_stage",
		     u8"此技能的箭矢，每層造成 108% 更多擊中傷害", "E5-3 狙擊每層傷害取 skill_stat 模板(非 gem_stat)"},
		};
		for (const G& g : gs) {
			const char* r = translation_lookup_stat_ids(g.en, "gem", g.ids);
			const bool ok = r && std::string(r) == g.want;
			check(ok, g.name);
			if (!ok) printf("           %s\n           got  '%s'\n           want '%s'\n", g.en, r ? r : "(null)", g.want);
		}
	}
	// E5-4:Kinetic Shell。GGPK kinetic_shell_hit_damage_absorbed_% …|100|# 1|#|skill_stat_descriptions.txt
	//   英文第 1 行無數字(條件 100 固定),中文帶 {0}% —— 產生器以 placeholder_mismatch 隔離，
	//   stats_by_id 沒有。依 N1 規則(條件單值 N 填 {0})+ 資料補入後應得兩行中文。
	{
		const char* en = "Damage from Hits is taken from Shell before target's Life or Energy Shield\n"
		                 "Shell can take 168 Damage before exploding";
		const char* r = translation_lookup_stat_ids(en, "gem",
		                                            "kinetic_shell_hit_damage_absorbed_% kinetic_shell_maximum_damage_absorbed_from_hits");
		const std::string want = u8"100%擊中傷害由壁壘先於目標的生命或能量護盾承受\n壁壘可以在爆炸前承受168傷害";
		const bool ok = r && want == r;
		check(ok, "E5-4 Kinetic Shell 兩行：條件固定值 100 填入 {0}(需資料補入)");
		if (!ok) printf("           got  '%s'\n           want '%s'\n", r ? r : "(null)", want.c_str());
	}
	// E5-5:ctx "gem:<scope>" 依 POB parent 鏈決定描述檔優先序。Gluttony 是輔助寶石
	// (scope gem_stat_descriptions,鏈 gem_stat → stat_descriptions):
	//   on_reaching_x_wildshard_stacks_gain_disgorge …|# # #|gem_stat_descriptions.txt
	//     "On reaching {0} Gluttony, lose all Gluttony and gain Disgorge for {1} seconds"
	//     → 在達到{0}層暴食後，失去所有的暴食並獲得持續{1}秒的噴吐
	//   同 stat|# # #|skill_stat_descriptions.txt "…for {2} seconds" → 在達到{0}層暴食時，失去所有暴食並獲得持續{2}秒的噴吐
	{
		const char* en = "On reaching 10 Gluttony, lose all Gluttony and gain Disgorge for 4 seconds";
		const char* g = translation_lookup_stat(en, "gem:gem_stat_descriptions");
		const char* s = translation_lookup_stat(en, "gem:skill_stat_descriptions");
		const std::string wantG = u8"在達到10層暴食後，失去所有的暴食並獲得持續4秒的噴吐";
		const std::string wantS = u8"在達到10層暴食時，失去所有暴食並獲得持續4秒的噴吐";
		const bool ok = g && wantG == g && s && wantS == s;
		check(ok, "E5-5 gem:<scope> 依 parent 鏈選檔(Gluttony:gem_stat 版 / skill_stat 版)");
		if (!ok) printf("           gem_stat   got '%s' want '%s'\n           skill_stat got '%s' want '%s'\n",
		                g ? g : "(null)", wantG.c_str(), s ? s : "(null)", wantS.c_str());
	}
	// N1-1(使用者裁定):map_num_extra_invasion_bosses|1|stat_descriptions.txt
	//   EN "Area is inhabited by an additional Invasion Boss" / ZH "地圖中含有 {0} 隻額外侵略聯盟的頭目"
	//   條件單值 1 → 填入 {0}。此條同樣被產生器隔離(placeholder_mismatch),需資料補入。
	expect("Area is inhabited by an additional Invasion Boss", "item",
	       u8"地圖中含有 1 隻額外侵略聯盟的頭目", "N1-1 英文無數字、條件單值 1 時以 1 填 {0}(需資料補入)");

	// ---- E6:名稱槽的來源名與「 Support」結尾 ----------------------------------------
	printf("\n=== E6 名稱槽來源 ===\n");
	{
		// E6-1:name_slots 指向 stats_by_id 以外的新來源(gem_names_nonactive)時，
		// 名稱從那個來源字典取。暫存複本:Pearl 頭盔兩把鍵(…|1 # #)的 name_slots
		// 改為 {"1":"gem_names_nonactive"},新來源只放 Bloodthirst→渴血,並列入 source_dicts。
		// gem_names.json 沒有 Bloodthirst,gems.json 是「渴血輔助」→ 退回 gems 就會「輔助輔助」。
		//   模板 local_pearl_random_support_gem_{1,2}_…|1 # #|stat_descriptions.txt|0
		//   "Skills Socketed in your Helmet are Supported by level {2} {1}" → 鑲嵌於你的頭盔中的技能會受到等級 {2} 的{1}輔助
		const std::wstring nRoot = root + L"e6n\\";
		CreateDirectoryW(nRoot.c_str(), nullptr);
		std::string w;
		bool setup = build_dict_copy(shippedLocale, nRoot + L"zh-rTW\\", statsJson, true, w);
		int patched = 0;
		if (setup) {
			std::string body;
			try {
				setup = read_bytes(nRoot + L"zh-rTW\\stats_by_id.json", body);
				auto doc = nlohmann::ordered_json::parse(body);
				auto& ns = doc["name_slots"];
				if (!ns.is_object()) ns = nlohmann::ordered_json::object();
				for (const char* k : {
				         "local_pearl_random_support_gem_1_slot_index local_pearl_random_support_gem_1_index "
				         "local_pearl_random_support_gem_1_level|1 # #|stat_descriptions.txt|0",
				         "local_pearl_random_support_gem_2_slot_index local_pearl_random_support_gem_2_index "
				         "local_pearl_random_support_gem_2_level|1 # #|stat_descriptions.txt|0"}) {
					if (doc["entries"].contains(k)) {
						ns[k] = {{"1", "gem_names_nonactive"}};
						patched++;
					}
				}
				setup = setup && patched == 2 && write_bytes(nRoot + L"zh-rTW\\stats_by_id.json", doc.dump(2));
				nlohmann::ordered_json g = nlohmann::ordered_json::object();
				g["source_files"] = nlohmann::ordered_json::array({"test"});
				g["is_base_items"] = false;
				g["entries"] = {{"Bloodthirst", u8"渴血"}};
				setup = setup && write_bytes(nRoot + L"zh-rTW\\gem_names_nonactive.json", g.dump(2));
				std::string mb;
				setup = setup && read_bytes(nRoot + L"zh-rTW\\meta.json", mb);
				auto meta = nlohmann::ordered_json::parse(mb);
				if (!in_array(meta, "source_dicts", "gem_names_nonactive.json"))
					meta["source_dicts"].push_back("gem_names_nonactive.json");
				setup = setup && write_bytes(nRoot + L"zh-rTW\\meta.json", meta.dump(2));
			} catch (...) { setup = false; }
		}
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", nRoot.c_str());
		translation_set_source(nullptr);
		translation_reload();
		const Said a = ask("Skills Socketed in your Helmet are Supported by level 20 Bloodthirst", "item");
		const std::string want = u8"鑲嵌於你的頭盔中的技能會受到等級 20 的渴血輔助";
		const bool ok = setup && !a.null && a.s == want && a.s.find(u8"輔助輔助") == std::string::npos;
		check(ok, "E6-1 name_slots 指向新來源 gem_names_nonactive 時從該來源取名");
		if (!ok) printf("           setup=%d patched=%d got '%s' want '%s' %s\n", setup, patched,
		                shown(a), want.c_str(), w.c_str());
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();
		remove_tree(nRoot);
	}
	// E6-2:Forbidden Shako「Socketed Gems are Supported by Level (1-10) Barrage Support」。
	// 實際 stat 是 local_random_support_gem_level local_random_support_gem_index(名稱槽 {1}:gems)
	//   "Socketed Gems are Supported by Level {0} {1}" → 插槽中的寶石被等級 {0} 的 {1} 輔助
	// 名稱 "Barrage Support" 去掉結尾 " Support" 後查 gem_names(Barrage→彈幕)。
	// (另有專屬 local_display_socketed_gems_supported_by_level_x_barrage →「插槽中寶石被等級 {0} 的彈幕輔助」,
	//  英文是 "…Level {0} Barrage" 不帶 Support,不是這件物品的 stat。)
	expect("Socketed Gems are Supported by Level 5 Barrage Support", "item",
	       u8"插槽中的寶石被等級 5 的 彈幕 輔助", "E6-2 名稱槽寶石名結尾「 Support」先去掉再查");

	// ---- S5 / V5(使用者裁定 2026-10-21)--------------------------------------------
	printf("\n=== S5 / V5 名稱槽 ===\n");
	// S5-1:Impossible Escape。local_unique_jewel_disconnected_passives_can_be_allocated_around_keystone_hash
	//   EN 三行 "Passive Skills in Radius of {} can be Allocated\nwithout being connected to your tree\nPassage"
	//   ZH 兩行 "天賦樹中在範圍{0}內未連結的天賦仍然可以配置\n通途";name_slots {"0":"passives"}
	// POB 給單行、小寫 radius/allocated;裁定：取中文第一行填名稱，不加「通途」。
	// 名稱(passives.json):Acrobatics→移形換影、Blood Magic→祭血術、Mind Over Matter→心靈昇華
	{
		struct K { const char* name; const char* zh; };
		const K ks[] = {{"Acrobatics", u8"移形換影"}, {"Blood Magic", u8"祭血術"}, {"Mind Over Matter", u8"心靈昇華"}};
		bool ok = true;
		for (const K& k : ks) {
			const std::string en = std::string("Passive Skills in radius of ") + k.name +
			                       " can be allocated without being connected to your tree";
			const std::string want = std::string(u8"天賦樹中在範圍") + k.zh + u8"內未連結的天賦仍然可以配置";
			const Said s = ask(en.c_str(), "item");
			const bool hit = !s.null && s.s == want && s.s.find(u8"通途") == std::string::npos &&
			                 s.s.find('{') == std::string::npos;
			if (!hit) {
				ok = false;
				printf("           %s\n           got  '%s'\n           want '%s'\n", en.c_str(), shown(s), want.c_str());
			}
		}
		check(ok, "S5-1 Impossible Escape 單行取中文第一行填天賦名(不加通途、無 {)");
	}
	// V5-1:名稱槽的 # 規則與精確條目一致 —— 名稱譯文裡的 '#' 是資料;'{0}' 仍擋。
	//   模板 unique_jewel_grants_notable_hash_part_1(name_slots {"0":"passives"})
	//   "Allocates {0} if you have the matching modifier on Forbidden Flesh" → 若禁忌血肉上有符合的詞綴，配置 {0}
	// 暫存複本的 passives.json 加兩個假名稱:Zzq Hash Notable→甲#乙、Zzq Brace Notable→甲{0}乙。
	{
		const std::wstring vRoot = root + L"v5\\";
		CreateDirectoryW(vRoot.c_str(), nullptr);
		std::string w;
		bool setup = build_dict_copy(shippedLocale, vRoot + L"zh-rTW\\", statsJson, true, w);
		if (setup) {
			std::string body;
			try {
				setup = read_bytes(vRoot + L"zh-rTW\\passives.json", body);
				auto doc = nlohmann::ordered_json::parse(body);
				doc["entries"]["Zzq Hash Notable"] = u8"甲#乙";
				doc["entries"]["Zzq Brace Notable"] = u8"甲{0}乙";
				setup = setup && write_bytes(vRoot + L"zh-rTW\\passives.json", doc.dump(2));
			} catch (...) { setup = false; }
		}
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", vRoot.c_str());
		translation_set_source(nullptr);
		translation_reload();
		const Said h = ask("Allocates Zzq Hash Notable if you have the matching modifier on Forbidden Flesh", "item");
		const Said b = ask("Allocates Zzq Brace Notable if you have the matching modifier on Forbidden Flesh", "item");
		const std::string wantH = u8"若禁忌血肉上有符合的詞綴，配置 甲#乙";
		const bool hashOk = setup && !h.null && h.s == wantH;
		const bool braceOk = setup && (b.null || b.s.find("{0}") == std::string::npos);
		check(hashOk, "V5-1a 名稱譯文含 # 時名稱槽照填(# 是資料)");
		if (!hashOk) printf("           setup=%d got '%s' want '%s' %s\n", setup, shown(h), wantH.c_str(), w.c_str());
		check(braceOk, "V5-1b 名稱譯文含 {0} 時仍被擋(不輸出未填佔位符)");
		if (!braceOk) printf("           got '%s'\n", shown(b));
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();
		remove_tree(vRoot);
	}

	// ---- E7:產生器把中文拆成與英文同行數(使用者核准)---------------------------
	// 期望值取自 dist stats_by_id.json(本輪新版)。行數約定：中文模板第二行為空
	// 字串時，整段結果保留結尾 "\n"(「第一行\n」,依 '\n' 切為兩段、第二段空字串),
	// 與英文行數一致 —— 注入層配對才能把第二行對上空字串而不是英文半句。
	printf("\n=== E7 中英同行數 ===\n");
	{
		// valueMoves:數值在英文第 1 行、中文第 2 行(語序互換)—— 依 P2-4「英繁每行 {N}
		// 集合相同才建逐行候選」,第 1 行單獨查不得套出中文第 1 行，只要求不洩漏。
		struct M { const char* en1; const char* en2; const char* ctx; const char* zh1; const char* zh2; const char* name; bool valueMoves; };
		const M ms[] = {
		    // gain_defiance_when_lose_life_to_hit_once_per_x_ms|1|#|stat_descriptions.txt|0
		    {"Gain Defiance for 10 seconds on losing Life to an Enemy Hit, no", "more than once every 0.3 seconds",
		     "passive", u8"敵人擊中而失去生命時獲得反抗 10 秒，", u8"每 0.3 秒不會超過一次", "E7-1 Defy Pain 兩行", false},
		    // poison_duration_+%_per_poison_applied_recently|1|#|stat_descriptions.txt|0
		    {"5% increased Poison Duration for each Poison you have inflicted Recently, up", "to a maximum of 100%",
		     "item", u8"近期每個你造成的中毒增加 5% 中毒持續時間，", u8"最多 100%", "E7-2 中毒持續時間兩行", false},
		    // flasks_adjacent_to_active_tinctures_have_effect_+%_if_hit_with_weapon_recently|1|#
		    {"Flasks adjacent to applied Tincture have 10% increased Effect when",
		     "used if you've Hit an enemy with a Weapon Recently", "item",
		     u8"若你近期有使用武器擊中敵人，", u8"當使用時相鄰套用萃取物的藥劑增加 10% 效果", "E7-3 萃取物藥劑兩行", true},
		    // life_mana_es_recovery_rate_+%_per_different_tribe_tattoos_allocated|1|#
		    {"5% increased Recovery Rate of Life, Mana and Energy Shield",
		     "per Tribe for which you have an allocated Tattoo", "passive",
		     u8"每個你配置紋身的部落，", u8"增加 5% 生命、魔力和能量護盾恢復率", "E7-4 部落紋身恢復率兩行", true},
		    // guardian_reserved_mana_%_given_to_you_and_nearby_allies_as_base_maximum_energy_shield|#(第二行空)
		    {"Grants maximum Energy Shield equal to 10% of your Reserved Mana to", "you and nearby Allies",
		     "item", u8"賦予你和附近友方等同你 10% 魔力保留的額外能量護盾", "", "E7-5 守護者保留魔力(第二行空)", false},
		    // gain_%_of_two_hand_weapon_physical_damage_as_added_spell_damage|#(runegraft_33,第二行空)
		    {"Spells have Added Spell Damage equal to 20% of Physical Damage of", "your Equipped Two handed Weapon",
		     "item", u8"法術擁有等同於你已裝備雙手武器20%物理傷害的附加法術傷害", "", "E7-6 雙手武器附加法術傷害(第二行空)", false},
		};
		for (const M& m : ms) {
			const std::string whole = std::string(m.en1) + "\n" + m.en2;
			const std::string want = std::string(m.zh1) + "\n" + m.zh2;
			const Said w = ask(whole.c_str(), m.ctx);
			const Said l1 = ask(m.en1, m.ctx);
			const bool okW = !w.null && w.s == want;
			const bool okL = m.valueMoves ? (l1.null || l1.s.find('{') == std::string::npos)
			                              : (!l1.null && l1.s == m.zh1);
			check(okW && okL, m.name);
			if (!okW) printf("           whole got '%s'\n           want      '%s'\n", shown(w), want.c_str());
			if (!okL) printf("           line1 got '%s' want '%s'\n", shown(l1), m.zh1);
		}
	}
	// E7-7:tornado_hinder|#|skill_stat_descriptions.txt|0(中文模板寫死數字;使用者裁定 2026-10-25 去掉負號,
	// 資料 32dab45 改成「減少 1%」)
	expect("Enemies in range are Hindered", "gem:skill_stat_descriptions",
	       u8"範圍內敵人被阻礙減少 1% 移動速度", "E7-7 龍捲風阻礙(gem:skill_stat_descriptions)");

	// ---- S8(使用者重新裁定，撤回依 id):legion 不載入 id 字典 -------------------
	// 查證:POB / poedb / GGPK Name 欄都是 Powerful Faith=暴擊信念、Frenzied Faith=
	// 狂怒信念，只有 GGPK 內部 Id 字串對調。裁定撤回「legion 依 id 取名」,改回依
	// 英文名查 legion 來源。即使 legion_by_id.json 列在 source_dicts,引擎也不得載入
	// (kIdDicts 沒有 legion),translation_lookup_by_id_en 也沒有 legion 例外。
	printf("\n=== S8 legion 不依 id ===\n");
	{
		const std::wstring lRoot = root + L"s8\\";
		CreateDirectoryW(lRoot.c_str(), nullptr);
		std::string w;
		bool setup = build_dict_copy(shippedLocale, lRoot + L"zh-rTW\\", statsJson, true, w);
		const std::wstring srcDir = statsJson.substr(0, statsJson.find_last_of(L"\\/") + 1);
		// dist 若已不出貨 legion_by_id.json,就在複本裡自己放一份(by_id 格式含 templates):
		// 要驗的是「即使列在 source_dicts 也不載入」。
		if (setup && !CopyFileW((srcDir + L"legion_by_id.json").c_str(),
		                        (lRoot + L"zh-rTW\\legion_by_id.json").c_str(), FALSE)) {
			nlohmann::ordered_json d = nlohmann::ordered_json::object();
			d["source_files"] = nlohmann::ordered_json::array({"test"});
			d["is_base_items"] = false;
			d["entries"] = {{"templar_notable_minimum_power_charge", u8"狂怒信念"},
			                {"vaal_small_attack_dodge", u8"避免元素異常狀態"}};
			d["templates"] = {{"templar_notable_minimum_power_charge", "Frenzied Faith"},
			                  {"vaal_small_attack_dodge", "Avoid Elemental Ailments"}};
			setup = write_bytes(lRoot + L"zh-rTW\\legion_by_id.json", d.dump(2));
		}
		if (setup) {
			std::string mb;
			try {
				setup = read_bytes(lRoot + L"zh-rTW\\meta.json", mb);
				auto meta = nlohmann::ordered_json::parse(mb);
				if (!in_array(meta, "source_dicts", "legion_by_id.json")) meta["source_dicts"].push_back("legion_by_id.json");
				setup = setup && write_bytes(lRoot + L"zh-rTW\\meta.json", meta.dump(2));
			} catch (...) { setup = false; }
		}
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", lRoot.c_str());
		translation_set_source(nullptr);
		translation_reload();
		const char* id = "templar_notable_minimum_power_charge";
		const char* a = translation_lookup_by_id("legion", id);
		const char* b = translation_lookup_by_id_en("legion", id, "Powerful Faith");
		const char* c = translation_lookup_by_id_en("legion", "vaal_small_attack_dodge", "Attack Dodge");
		const std::string built = translation_debug_sources();
		const size_t ip = built.find(";id=");
		const std::string ids = ip == std::string::npos ? built : built.substr(ip + 4);
		const bool noIdDict = ids.find("legion") == std::string::npos;
		// 前提:legion 的英文來源表照常建(依英文名查的路徑還在)
		const bool srcBuilt = built.find("exact=") != std::string::npos &&
		                      built.substr(0, ip).find("legion") != std::string::npos;
		check(setup && srcBuilt && noIdDict && a == nullptr,
		      "S8-1 legion_by_id 列在 source_dicts 也不載入:依 id 查 legion 回空");
		check(setup && b == nullptr && c == nullptr, "S8-2 translation_lookup_by_id_en 沒有 legion 例外(回空)");
		if (!(setup && srcBuilt && noIdDict && a == nullptr && b == nullptr && c == nullptr))
			printf("           setup=%d built='%s' a='%s' b='%s' c='%s' %s\n", setup, built.c_str(),
			       a ? a : "(null)", b ? b : "(null)", c ? c : "(null)", w.c_str());
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();
		remove_tree(lRoot);
	}

	printf("\n=== FR audit r9 ===\n");
	// FR-1:插槽輔助有專屬 stat(英文寫死寶石名),不得讓名稱槽通用模板
	//   local_random_support_gem_level local_random_support_gem_index "…Level {0} {1}" →「插槽中的寶石被等級 {0} 的 {1} 輔助」
	// 勝出。專屬模板(stat_descriptions.txt):
	//   …supported_by_level_x_intensify / …_x_spell_focus / …_x_intensify_level → 插槽中寶石被等級 {0} 的強化輔助
	//   …supported_by_x_controlled_destruction → 插槽中寶石被等級 {0} 精準破壞輔助
	//   …supported_by_level_x_greater_spell_echo → 插槽中寶石被等級 {0} 的高階施放迴響輔助
	expect("Socketed Gems are Supported by Level 10 Intensify", "item",
	       u8"插槽中寶石被等級 10 的強化輔助", "FR-1a 插槽輔助專屬模板優先於名稱槽(Intensify)");
	expect("Socketed Gems are Supported by Level 10 Controlled Destruction", "item",
	       u8"插槽中寶石被等級 10 精準破壞輔助", "FR-1b 插槽輔助專屬模板優先於名稱槽(Controlled Destruction)");
	expect("Socketed Gems are Supported by Level 1 Greater Spell Echo", "item",
	       u8"插槽中寶石被等級 1 的高階施放迴響輔助", "FR-1c 插槽輔助專屬模板優先於名稱槽(Greater Spell Echo)");
	// FR-2:passive 優先序 passive_skill → stat_descriptions → passive_skill_aura。
	//   life_mastery_nearby_enemy_life_regeneration_rate_+%|#|-1|passive_skill_stat_descriptions.txt|0
	//     → 附近敵人減少 {0}% 生命恢復率
	//   (光環檔 life_regeneration_rate_+%|#|-1|passive_skill_aura… 是「附近敵人減少 {0}% 生命回復」,不得採用)
	expect("Nearby Enemies have 50% reduced Life Regeneration Rate", "passive",
	       u8"附近敵人減少 50% 生命恢復率", "FR-2 專精行取天賦描述檔，光環檔排在最後");

	// ---- P3:anoint / legion / tattoo 來源字典 ------------------------------------
	// 扁平 {source_files,is_base_items,entries},只列 meta source_dicts,只在
	// source_lookup 下當來源表;絕不併入全域。三檔由測試在暫存複本放小樣本。
	// 全域現值(出貨字典):Juggernaut→勇士、Bleed Avoidance→避免流血、
	// Tattoo of the Kitava Makanga→奇塔弗馬坎加的紋身 —— 都和來源譯法不同。
	printf("\n=== P3 anoint / legion / tattoo 來源 ===\n");
	{
		const char* kOnlyAnoint = "Zzqanointonly Probe Key";
		auto plantP3 = [&](const std::wstring& r, bool flag, std::string& why) {
			CreateDirectoryW(r.c_str(), nullptr);
			if (!build_dict_copy(shippedLocale, r + L"zh-rTW\\", statsJson, flag, why)) return false;
			auto put = [&](const wchar_t* name, const char* src,
			               std::initializer_list<std::pair<const char*, std::string>> kv) {
				nlohmann::ordered_json d = nlohmann::ordered_json::object();
				d["source_files"] = nlohmann::ordered_json::array({src});
				d["is_base_items"] = false;
				d["entries"] = nlohmann::ordered_json::object();
				for (const auto& p : kv) d["entries"][p.first] = p.second;
				return write_bytes(r + L"zh-rTW\\" + name, d.dump(2));
			};
			bool ok = put(L"anoint.json", "test", {{"Juggernaut", u8"戰神"}, {kOnlyAnoint, u8"塗油專用"}}) &&
			          put(L"legion.json", "test", {{"Bleed Avoidance", u8"流血避免"}}) &&
			          put(L"tattoo.json", "test", {{"Tattoo of the Kitava Makanga", u8"奇塔弗鑄造的紋身"}});
			std::string mb;
			try {
				ok = ok && read_bytes(r + L"zh-rTW\\meta.json", mb);
				auto meta = nlohmann::ordered_json::parse(mb);
				for (const char* f : {"anoint.json", "legion.json", "tattoo.json"})
					if (!in_array(meta, "source_dicts", f)) meta["source_dicts"].push_back(f);
				ok = ok && write_bytes(r + L"zh-rTW\\meta.json", meta.dump(2));
			} catch (...) { ok = false; }
			return ok;
		};
		auto say = [](const char* k) {
			const char* r = translation_lookup(k);
			return r ? std::string(r) : std::string("(null)");
		};
		auto sourced = [&](const char* src, const char* k) {
			translation_set_source(src);
			const std::string r = say(k);
			translation_set_source(nullptr);
			return r;
		};

		// 基準：同一份出貨字典、沒有三個新檔時的全域值。
		translation_set_source(nullptr);
		const std::string bareJugg = say("Juggernaut");

		const std::wstring fRoot = root + L"p3f\\";
		const std::wstring nRoot = root + L"p3n\\";
		std::string w;
		const bool setup = plantP3(fRoot, true, w) && plantP3(nRoot, false, w);
		check(setup, "P3-0 三個來源檔已放入暫存複本並列入 source_dicts");
		if (!setup) printf("           %s\n", w.c_str());

		SetEnvironmentVariableW(L"POB_ZH_DATADIR", fRoot.c_str());
		translation_set_source(nullptr);
		translation_reload();
		{
			const std::string a = sourced("anoint", "Juggernaut");
			const std::string bare = say("Juggernaut");
			const bool ok = setup && a == u8"戰神" && bare == bareJugg && bare != a;
			check(ok, "P3-1 anoint 來源下 Juggernaut 得戰神，無來源仍為全域值");
			if (!ok) printf("           anoint='%s' bare='%s' baseline='%s'\n", a.c_str(), bare.c_str(), bareJugg.c_str());
		}
		{
			const std::string l = sourced("legion", "Bleed Avoidance");
			check(setup && l == u8"流血避免", "P3-2 legion 來源下 Bleed Avoidance 得流血避免");
			if (l != u8"流血避免") printf("           legion='%s'\n", l.c_str());
		}
		{
			const std::string t = sourced("tattoo", "Tattoo of the Kitava Makanga");
			check(setup && t == u8"奇塔弗鑄造的紋身", "P3-3 tattoo 來源下得奇塔弗鑄造的紋身");
			if (t != u8"奇塔弗鑄造的紋身") printf("           tattoo='%s'\n", t.c_str());
		}
		{
			const char* g = translation_lookup(kOnlyAnoint);
			const bool globalKept = g == nullptr || std::string(g) == kOnlyAnoint;
			const std::string viaSrc = sourced("anoint", kOnlyAnoint);
			const bool ok = setup && globalKept && viaSrc == u8"塗油專用";
			check(ok, "P3-4 只在 anoint 檔的鍵不進全域(來源下可查為前提)");
			if (!ok) printf("           global='%s' anoint='%s'\n", g ? g : "(null)", viaSrc.c_str());
		}
		{
			const std::string flagged = translation_debug_sources();
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", nRoot.c_str());
			translation_reload();
			const std::string noflag = translation_debug_sources();
			auto hasAll = [](const std::string& s) {
				return s.find("anoint") != std::string::npos && s.find("legion") != std::string::npos &&
				       s.find("tattoo") != std::string::npos;
			};
			auto hasAny = [](const std::string& s) {
				return s.find("anoint") != std::string::npos || s.find("legion") != std::string::npos ||
				       s.find("tattoo") != std::string::npos;
			};
			const bool ok = setup && hasAll(flagged) && !hasAny(noflag);
			check(ok, "P3-5 無 source_lookup 旗標時不建三個來源表(有旗標時三表齊全)");
			if (!ok) printf("           flagged='%s'\n           noflag='%s'\n", flagged.c_str(), noflag.c_str());
		}

		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_set_source(nullptr);
		translation_reload();
		remove_tree(fRoot);
		remove_tree(nRoot);
	}

	// ---- P2:不得輸出未填的佔位符 ------------------------------------------------
	// coverage_r2(tools/ggpk_zh/audit_prod/coverage_r2/out/cat_*.tsv 的 leak 欄)40 列:
	// POB 把多行 stat 拆成半句 / 逐行送來,半句被當成整條模板或 pattern 翻,模板裡
	// 找不到值的 {N} 原樣輸出(例「獲得 {0} 充能，每秒不超過 1 次」)。
	printf("\n=== P2 未填佔位符守門 ===\n");
	{
		// 含 "{數字" 或 "{}" 即算洩漏。
		auto leaks = [](const char* s) {
			if (!s) return false;
			for (const char* p = s; *p; ++p)
				if (*p == '{' && (p[1] == '}' || (p[1] >= '0' && p[1] <= '9'))) return true;
			return false;
		};
		// {英文(POB 實際送來的半句), stat ctx, 出處類別}
		struct Q { const char* en; const char* ctx; const char* cat; };
		const Q qs[] = {
		    {"Area is inhabited by an additional Invasion Boss", "item", "cat01 mod"},
		    {"Enemy with a Melee Weapon, no more than once every second", "passive", "cat08 pas 2275"},
		    {"used a Ruby Flask Recently", "passive", "cat08 pas 54928"},
		    {"each Unique item they have equipped", "passive", "cat08 pas 25944 ({})"},
		    {"a Unique Enemy is in your Presence", "passive", "cat09 Tukohama6"},
		    {"of your Traps are Triggered by an Enemy", "item", "cat10 Slavedriver's Hand"},
		    {"Fires 1 Projectile", "item", "cat13 FreezingPulse"},
		    {"Enemies in range are Hindered", "item", "cat13 Tornado"},
		    {"20% chance to gain a Frenzy Charge when a Mine from Supported", "item", "cat13 ChargedMines 第 1 行"},
		    {"Skills is Detonated targeting an Enemy", "item", "cat13 ChargedMines 第 2 行"},
		    {"20% chance to gain a Frenzy Charge when a Mine from Supported\nSkills is Detonated targeting an Enemy",
		     "item", "cat13 ChargedMines 整段(真換行)"},
		};
		bool ok = true;
		for (const Q& q : qs) {
			translation_set_source(nullptr);
			const char* g = translation_lookup(q.en);
			const bool gl = leaks(g);
			const std::string gs = g ? g : "(null)";
			const char* s = translation_lookup_stat(q.en, q.ctx);
			const bool sl = leaks(s);
			const std::string ss = s ? s : "(null)";
			if (gl || sl) {
				ok = false;
				printf("           [%s] %s\n             lookup='%s'%s\n             stat[%s]='%s'%s\n", q.cat, q.en,
				       gs.c_str(), gl ? "  <- 洩漏" : "", q.ctx, ss.c_str(), sl ? "  <- 洩漏" : "");
			}
		}
		check(ok, "P2-1 半句英文的查詢結果不含未填佔位符(可以是空或正確中文)");
	}
	{
		// P2-2:SupportChargedMines 的「狂怒球」多行模板只在 gem_stat_descriptions.txt
		//   %_chance_to_gain_frenzy_charge_on_mine_detonated_targeting_an_enemy|#|gem_stat_descriptions.txt|0
		//   EN "{0}% chance to gain a Frenzy Charge when a Mine from Supported\nSkills is Detonated targeting an Enemy"
		//   ZH "當透過輔助技能製造的地雷對敵人引爆時，\n有 {0}% 機率獲得 1 顆狂怒球"
		// 英繁兩行但語序互換(英文第 1 行的值在中文第 2 行),逐行對位不成立,所以以
		// 整段(真換行)查。新規則(b)把多行逐行候選擴大到所有已載入描述檔;此檔
		// 不屬 item 或 passive 情境,引擎目前也不載入 —— 這裡以 item 情境查整段,
		// 斷言:有答案時必須是狂怒球那句;不得是 coverage_r2 看到的「暴擊球」。
		const char* kWhole =
		    "20% chance to gain a Frenzy Charge when a Mine from Supported\nSkills is Detonated targeting an Enemy";
		const char* g = translation_lookup(kWhole);
		const char* s = translation_lookup_stat(kWhole, "item");
		const std::string want = u8"當透過輔助技能製造的地雷對敵人引爆時，\n有 20% 機率獲得 1 顆狂怒球";
		const std::string gs = g ? g : "(null)", ss = s ? s : "(null)";
		const bool noPower = gs.find(u8"暴擊球") == std::string::npos && ss.find(u8"暴擊球") == std::string::npos;
		const bool hit = (gs == want) || (ss == want);
		check(noPower && hit, "P2-2 狂怒球地雷多行句得狂怒球譯文(不得錯成暴擊球)");
		if (!(noPower && hit))
			printf("           lookup='%s'\n           stat[item]='%s'\n           want '%s'\n",
			       gs.c_str(), ss.c_str(), want.c_str());
	}
	{
		// P2-3:輸入本身就帶 {0} 的字典鍵(stats.json)不受守門影響。
		translation_set_source(nullptr);
		const char* a = translation_lookup("{0} (action speed modifier)");
		const char* b = translation_lookup("{0} - {1} (portion of damage from non-crits)");
		const bool ok = a && std::string(a) == u8"{0} (動作速度詞綴)" &&
		                b && std::string(b) == u8"{0} - {1} (來自非暴擊傷害百分比)";
		check(ok, "P2-3 輸入本身含 {0} 的鍵照常翻譯(守門只擋輸入沒有的佔位符)");
		if (!ok) printf("           a='%s' b='%s'\n", a ? a : "(null)", b ? b : "(null)");
	}

	{
		// P2-4:多行條目拆行衍生時，英繁該行的 {N} 集合不同就不得建(否則半句會被
		// 行內寫死數字當值套進錯的那半句)。來源(stats.json 字面 \n 鍵):
		//   "Minions Convert {0}% of their Maximum Life to Maximum Energy\nShield per 1% Chaos Resistance they have"
		//   "Attacks with this Weapon deal {0} to {1} added Chaos Damage against\nEnemies affected by at least 5 Poisons"
		//   "{0}% reduced Effect of Arcane Surge on you per\n200 Mana spent Recently"
		//   "Increases and Reductions to Armour also apply to Energy Shield\n..."(衍生「護甲值增減也以相當於其數值的 {0}%」)
		translation_set_source(nullptr);
		const char* bad[] = {"Shield per 1% Chaos Resistance they have",
		                     "Enemies affected by at least 5 Poisons",
		                     "Increases and Reductions to Armour also apply to Energy Shield",
		                     "200 Mana spent Recently"};
		bool ok = true;
		for (const char* e : bad) {
			const char* r = translation_lookup(e);
			if (r && std::string(r) != e) {
				ok = false;
				printf("           %s -> '%s'(應為空)\n", e, r);
			}
		}
		// 防過度收緊：英繁每行 {N} 集合相同的多行條目(stats.json 字面 \n 鍵,單行本身不是
		// 任何字典的鍵),拆行衍生的單行仍查得到:
		//   "{0:+d} to maximum number of Sacred Wisps\n{0:+d} to number of Sacred Wisps Summoned"
		//   "{0} to {1} added Fire Damage\nPlayers and their Minions have {0} to {1} added Fire Damage"
		const std::pair<const char*, std::string> goods[] = {
		    {"+2 to number of Sacred Wisps Summoned", u8"+2 召喚聖潔妖精數量"},
		    {"Players and their Minions have 5 to 10 added Fire Damage", u8"玩家和他們的召喚物附加 5 至 10 火焰傷害"}};
		bool goodOk = true;
		for (const auto& gd : goods) {
			const char* g = translation_lookup(gd.first);
			if (!(g && gd.second == g)) {
				goodOk = false;
				printf("           %s -> '%s' want '%s'\n", gd.first, g ? g : "(null)", gd.second.c_str());
			}
		}
		check(ok && goodOk, "P2-4 拆行衍生須英繁每行 {N} 相同：錯配半句回空，相同者單行仍可查");
	}
	{
		// P2-5:結果含 '#' 而英文沒有 —— 填值不完整的殘留，視為未命中。
		// 出處:p2_guard_placeholder_dump_changed.tsv(stats.json)。
		translation_set_source(nullptr);
		const char* kEn = "1% Blood in the Blood Crucible while you are able to Shift into Nightmare";
		const char* r = translation_lookup(kEn);
		const bool ok = !r || std::string(r).find('#') == std::string::npos;
		check(ok, "P2-5 英文無 # 時結果不得殘留 #");
		if (!ok) printf("           %s -> '%s'\n", kEn, r);
	}

	// ---- D:詞彙表(glossary)選擇規則(使用者裁定)----------------------------
	// 同一 term(小寫、去結尾冒號)只留一筆：優先原鍵不帶冒號者;都帶冒號取原鍵
	// 字典序最小;全形「：」與半形「:」結尾都去除;排序 = 長度降冪、再 term 字典序。
	// 例:ui.json "Breach:" → "裂痕：" 與 passives.json "Breach" → "裂痕" 去冒號後
	// 同為 "breach",舊做法只依長度 std::sort,平手先後隨雜湊排列翻轉。
	printf("\n=== D 詞彙表選擇規則 ===\n");
	{
		auto say = [](const char* s) {
			const char* r = translation_lookup(s);
			return r ? std::string(r) : std::string("(null)");
		};
		const std::vector<std::pair<std::string, std::string>> kZzq = {
		    {"Zzqtestterm:", u8"測試詞："}};   // 只有帶全形冒號的版本
		const char* kSentences[] = {
		    "Exclusive to: Breach", "Exclusive to: Harvest", "Exclusive to: Heist",
		    "Exclusive to: Legion", "Exclusive to: Delirium", "Exclusive to: Zzqtestterm",
		    "Evasion Rating: 120", "Source: Labyrinth"};
		// 每種虛構鍵數量各一份複本;0 = 只加 Zzqtestterm 的基準。
		const int kExtras[] = {0, 100, 500, 2000, 5000};
		std::vector<std::vector<std::string>> outs;
		bool setup = true;
		for (int n : kExtras) {
			const std::wstring vroot = root + L"g" + std::to_wstring(n) + L"\\";
			CreateDirectoryW(vroot.c_str(), nullptr);
			std::string w;
			if (!build_dict_copy(shippedLocale, vroot + L"zh-rTW\\", statsJson, true, w) ||
			    !plant_ui(vroot + L"zh-rTW\\", n, kZzq)) {
				setup = false;
				printf("           setup g%d failed %s\n", n, w.c_str());
				outs.emplace_back();
				continue;
			}
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", vroot.c_str());
			translation_set_source(nullptr);
			translation_reload();
			std::vector<std::string> o;
			for (const char* s : kSentences) o.push_back(say(s));
			outs.push_back(std::move(o));
			remove_tree(vroot);
		}
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();

		const std::vector<std::string>& base = outs[0];
		// D1/D2/D4 在每一種虛構鍵數量下都要成立(只看基準會被雜湊排列碰巧蓋過)。
		auto expectD = [&](const char* s, const std::string& want, const char* name) {
			size_t idx = 0;
			while (idx < std::size(kSentences) && strcmp(kSentences[idx], s)) idx++;
			bool ok = setup;
			for (size_t v = 0; v < outs.size(); v++) {
				const std::string got = idx < outs[v].size() ? outs[v][idx] : "(missing)";
				if (got != want) {
					ok = false;
					printf("           +%d keys: %s got '%s' want '%s'\n", kExtras[v], s,
					       got.c_str(), want.c_str());
				}
			}
			check(ok, name);
		};
		expectD("Exclusive to: Breach", u8"限定: 裂痕", "D1a Exclusive to: Breach → 限定: 裂痕(取不帶冒號鍵)");
		expectD("Exclusive to: Harvest", u8"限定: 豐收", "D1b Exclusive to: Harvest → 限定: 豐收(不取 Harvest: 的豐收忌劫)");
		expectD("Exclusive to: Zzqtestterm", u8"限定: 測試詞", "D2 只有帶冒號版本時去掉全形「：」");
		expectD("Evasion Rating: 120", u8"閃避值: 120", "D4 Evasion Rating: 120 → 閃避值: 120(採 GGPK)");

		// D3:每種數量的輸出都要與基準逐句相同。
		bool same = setup;
		for (size_t v = 1; v < outs.size(); v++) {
			for (size_t i = 0; i < std::size(kSentences); i++) {
				const std::string a = i < base.size() ? base[i] : "(missing)";
				const std::string b = i < outs[v].size() ? outs[v][i] : "(missing)";
				if (a != b) {
					same = false;
					printf("           +%d keys: %s: base='%s' now='%s'\n", kExtras[v],
					       kSentences[i], a.c_str(), b.c_str());
				}
			}
		}
		check(same, "D3 加入 100/500/2000/5000 筆無關鍵後詞彙表結果不變(與雜湊排列無關)");
		printf("           (基準輸出:");
		for (size_t i = 0; i < base.size(); i++) printf(" [%s]", base[i].c_str());
		printf(")\n");
	}

	// ---- G:keep_literals(使用者裁定 2026-10-25:岩漿之擊照 GGPK「…5 秒」)----------
	printf("\n=== G 照抄中文數字(keep_literals)只作用於標記鍵 ===\n");
	{
		const char* kKeepKey = "every_10_seconds_physical_damage_%_to_add_as_fire_for_3_seconds|#|stat_descriptions.txt|0";
		const char* kDexKey = "minion_damage_+%_per_5_dex|1|#|stat_descriptions.txt|0";
		const char* kMagma2 = "Every 10 seconds, gain 30% of Physical Damage\nas Extra Fire Damage for 4 seconds";
		const char* kMagma1 = "Every 10 seconds, gain 30% of Physical Damage as Extra Fire Damage for 4 seconds";
		const std::string kMagmaWant = u8"每 10 秒，獲得 30% 物理傷害的額外火焰傷害 5 秒";
		// G1:出貨資料的旗標(產生器 STATS_KEEP_LITERALS)。兩行原樣與單行合併兩種候選都照抄。
		expect(kMagma2, "item", kMagmaWant, "G1a 標記鍵(兩行原樣)照 GGPK 顯示 5 秒");
		expect(kMagma2, "passive", kMagmaWant, "G1b 標記鍵在天賦情境(塗油/天賦樹浮窗)也是 5 秒");
		expect(kMagma1, "item", kMagmaWant, "G1c 標記鍵的單行合併候選也帶旗標");
		// 受影響但未標記的鍵:使用者裁定維持英文數字(現行規則)
		const char* kUnflagged[] = {
		    "15% chance to Trigger Level 10 Summon Raging Spirit on Kill",
		    "Trigger Level 10 Summon Raging Spirit on Kill",
		    "Minions deal 12% increased Damage per 5 Dexterity",
		    "Minions deal 12% reduced Damage per 5 Dexterity",
		    "Gain Adrenaline when Stunned, for 2 seconds per 100ms of Stun Duration",
		};
		// 各行的 stats_by_id 鍵(G3a / G4 依鍵比,不靠位置:出貨資料把哪一鍵標成 keep_literals,那一鍵就不再是「未標記」)
		const char* kUnflaggedKeys[] = {
		    "local_display_summon_raging_spirit_on_kill_%|#|99|stat_descriptions.txt|0",
		    "local_display_summon_raging_spirit_on_kill_%|100|stat_descriptions.txt|0",
		    "minion_damage_+%_per_5_dex|1|#|stat_descriptions.txt|0",
		    "minion_damage_+%_per_5_dex|#|-1|stat_descriptions.txt|0",
		    "gain_adrenaline_for_x_ms_per_100_ms_stun_duration_on_you|1|#|stat_descriptions.txt|0",
		};
		static_assert(sizeof(kUnflagged) == sizeof(kUnflaggedKeys), "kUnflagged / kUnflaggedKeys 要一一對應");
		expect(kUnflagged[1], "item", u8"擊殺時觸發等級 10 的召喚憤怒狂靈", "G2a 未標記鍵(憤怒狂靈)維持英文的等級 10");
		expect(kUnflagged[2], "item", u8"每 5 點敏捷增加 12% 召喚物所造成的傷害", "G2b 未標記鍵(每 5 點敏捷)維持英文的 5");
		// G3:同一份資料拿掉全部旗標(= 舊行為),未標記鍵逐句不變、標記鍵才變。
		std::vector<std::string> withFlag, noFlag, planted;
		for (const char* s : kUnflagged) { const Said r = ask(s, "item"); withFlag.push_back(shown(r)); }
		const Said magWith = ask(kMagma2, "item");
		std::string body, why2;
		bool setup = read_bytes(flagRoot + L"zh-rTW\\stats_by_id.json", body);
		// 腎上腺素那條(kUnflagged[4])由使用者裁定 2026-10-26 改照 GGPK(資料端 keep_literals,另一分支):
		// 出貨資料帶上旗標後,它就不再是「未標記」,G3a 不拿它比。
		const char* kAdrKey = "gain_adrenaline_for_x_ms_per_100_ms_stun_duration_on_you|1|#|stat_descriptions.txt|0";
		std::vector<char> shippedFlag(std::size(kUnflaggedKeys), 0);
		try {
			const auto shippedDoc = nlohmann::json::parse(body);
			if (shippedDoc.contains("keep_literals") && shippedDoc["keep_literals"].is_object())
				for (size_t i = 0; i < std::size(kUnflaggedKeys); i++)
					shippedFlag[i] = shippedDoc["keep_literals"].contains(kUnflaggedKeys[i]) ? 1 : 0;
		} catch (...) {}
		bool adrFlagged = false;
		for (size_t i = 0; i < std::size(kUnflaggedKeys); i++)
			if (!strcmp(kUnflaggedKeys[i], kAdrKey)) adrFlagged = shippedFlag[i] != 0;
		auto variant = [&](const wchar_t* name, const char* plantKey, std::vector<std::string>& outs,
		                   Said& mag) {
			const std::wstring vroot = root + name + L"\\";
			CreateDirectoryW(vroot.c_str(), nullptr);
			bool ok = build_dict_copy(shippedLocale, vroot + L"zh-rTW\\", statsJson, true, why2);
			try {
				auto doc = nlohmann::ordered_json::parse(body);
				doc.erase("keep_literals");
				if (plantKey) doc["keep_literals"][plantKey] = true;
				ok = ok && write_bytes(vroot + L"zh-rTW\\stats_by_id.json", doc.dump(2));
			} catch (...) { ok = false; }
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", vroot.c_str());
			translation_reload();
			for (const char* s : kUnflagged) { const Said r = ask(s, "item"); outs.push_back(shown(r)); }
			mag = ask(kMagma2, "item");
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
			translation_reload();
			remove_tree(vroot);
			return ok;
		};
		Said magNo{true, {}}, magPlant{true, {}};
		setup = setup && variant(L"keep_none", nullptr, noFlag, magNo);
		setup = setup && variant(L"keep_dex", kDexKey, planted, magPlant);
		bool same3a = setup && withFlag.size() == noFlag.size();
		for (size_t i = 0; same3a && i < withFlag.size(); i++)
			if (!shippedFlag[i] && withFlag[i] != noFlag[i]) same3a = false;
		check(same3a, "G3a 拿掉全部旗標時,未標記的受影響鍵逐句不變");
		if (!same3a)
			for (size_t i = 0; i < withFlag.size() && i < noFlag.size(); i++)
				printf("           %s: with='%s' none='%s'\n", kUnflagged[i], withFlag[i].c_str(), noFlag[i].c_str());
		check(setup && !magNo.null && magNo.s != kMagmaWant && !magWith.null && magWith.s == kMagmaWant,
		      "G3b 旗標只在標記鍵上起作用(拿掉後同一行回到英文的 4 秒)");
		if (!setup || magNo.null || magNo.s == kMagmaWant)
			printf("           none='%s' with='%s' (%s)\n", shown(magNo), shown(magWith), why2.c_str());
		// G4:植入另一鍵的旗標(只改複本):那一鍵照抄中文的 10、其餘照舊,證明機制逐鍵。
		bool others4 = setup && planted.size() == noFlag.size();
		size_t dexIdx = 0;
		for (size_t i = 0; others4 && i < planted.size(); i++) {
			if (!strcmp(kUnflaggedKeys[i], kDexKey)) { dexIdx = i; continue; }
			if (planted[i] != noFlag[i]) others4 = false;
		}
		check(others4 && planted[dexIdx] == u8"每 10 點敏捷增加 12% 召喚物所造成的傷害",
		      "G4 植入旗標的鍵照抄中文數字,同一份資料的其他鍵不受影響");
		if (planted.size() == noFlag.size())
			for (size_t i = 0; i < planted.size(); i++)
				if (planted[i] != noFlag[i]) printf("           planted %s: '%s'(none '%s')\n", kUnflagged[i], planted[i].c_str(), noFlag[i].c_str());
		(void)kKeepKey;
		// G5:專精「Gain Adrenaline…per 100ms」英文寫死 100(毫秒)、GGPK 中文寫死 0.1(秒),字面數字不同。
		// 使用者裁定 2026-10-26 照 GGPK「每 0.1 秒」,修在資料端(keep_literals,wip/round9-data)。
		// G5a 先用測試字典植入旗標驗機制(現在就該綠);G5b 看出貨資料,資料帶上旗標才生效(之前列 WAIT)。
		const char* kAdr = kUnflagged[4];
		const std::string kAdrWant = u8"當暈眩時，每 0.1 秒暈眩持續時間，獲得腎上腺素 2 秒";
		{
			const std::wstring vroot = root + L"keep_adr\\";
			CreateDirectoryW(vroot.c_str(), nullptr);
			bool ok = build_dict_copy(shippedLocale, vroot + L"zh-rTW\\", statsJson, true, why2);
			try {
				auto doc = nlohmann::ordered_json::parse(body);
				doc["keep_literals"][kAdrKey] = true;
				ok = ok && write_bytes(vroot + L"zh-rTW\\stats_by_id.json", doc.dump(2));
			} catch (...) { ok = false; }
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", vroot.c_str());
			translation_reload();
			const Said pas = ask(kAdr, "passive");
			const Said itm = ask(kAdr, "item");
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
			translation_reload();
			remove_tree(vroot);
			check(ok && !pas.null && pas.s == kAdrWant, "G5a 植入旗標:專精行(passive)照 GGPK 顯示「每 0.1 秒」");
			if (!(ok && !pas.null && pas.s == kAdrWant)) printf("           passive='%s'\n", shown(pas));
			check(ok && !itm.null && itm.s == kAdrWant, "G5a' 植入旗標:同一行在物品情境也是「每 0.1 秒」");
			if (!(ok && !itm.null && itm.s == kAdrWant)) printf("           item='%s'\n", shown(itm));
		}
		{
			const Said now = ask(kAdr, "passive");
			if (adrFlagged) expect(kAdr, "passive", kAdrWant, "G5b 出貨資料:專精行(passive)照 GGPK 顯示「每 0.1 秒」");
			else wait_for_data("G5b 出貨資料:專精行(passive)照 GGPK 顯示「每 0.1 秒」", shown(now), kAdrWant);
		}
	}

	// ---- H:多行描述的整句重組(ggpk_groups.json) ------------------------------------
	printf("\n=== H 多行描述整句重組(translation_group_lines) ===\n");
	{
		struct G { int n; std::vector<std::string> out; };
		auto group = [](const std::vector<std::string>& lines, const char* ctx) {
			std::vector<const char*> in, out(lines.size(), nullptr);
			for (const auto& l : lines) in.push_back(l.c_str());
			G g;
			g.n = translation_group_lines(in.data(), (int)in.size(), ctx, out.data());
			for (const char* o : out) g.out.push_back(o ? std::string(o) : std::string("(null)"));
			return g;
		};
		auto expectG = [&](const std::vector<std::string>& lines, const char* ctx,
		                   const std::vector<std::string>& want, const char* name) {
			const G g = group(lines, ctx);
			const bool ok = g.out == want;
			check(ok, name);
			if (!ok) {
				printf("           [%s] groups=%d\n", ctx, g.n);
				for (size_t i = 0; i < lines.size(); i++)
					printf("           %s\n             got  '%s'\n             want '%s'\n", lines[i].c_str(),
					       i < g.out.size() ? g.out[i].c_str() : "-", i < want.size() ? want[i].c_str() : "-");
			}
		};
		const std::string N = "(null)";
		const std::string kNat1 = "Flasks adjacent to active Tinctures gain 2 charges when you Hit an";
		const std::string kNat2 = "Enemy with a Melee Weapon, no more than once every second";
		const std::vector<std::string> kNatZh = {u8"當你使用近戰武器擊中敵人時，與啟用萃取物相鄰的藥劑",
		                                         u8"獲得 2 充能，每秒不超過 1 次"};
		expectG({kNat1, kNat2}, "passive", kNatZh, "H1 兩行 stat、GGPK 中文兩行:各行照 GGPK 分行(自然調劑)");
		expectG({"Every 10 seconds, gain 30% of Physical Damage", "as Extra Fire Damage for 4 seconds"}, "passive",
		        {u8"每 10 秒，獲得 30% 物理傷害的額外火焰傷害 5 秒", ""},
		        "H2 GGPK 中文一句:整句在第 1 行、第 2 行留白,且照抄標記鍵的 5 秒(岩漿之擊)");
		expectG({"The Black Sekhema was a formidable fighter", "alongside her dekhara, but she was unstoppable",
		         "when she fought atop her trusted mount, Shiyo."}, "gem",
		        {u8"漆黑絲克瑪與她的部下並肩作戰時 已是一名令人敬畏的戰士，但當她騎乘 在忠誠的坐騎熙優背上戰鬥時，更是勢不可擋。", "", ""},
		        "H3 風味文字三行、GGPK 一段:整段在第 1 行、其餘留白(Companionship)");
		expectG({"My ancestral pact was sealed. Forevermore, I would gain sustenance", "only from the ravaged flesh of my enemies."},
		        "passive", {u8"在與先祖定下冥約之後...從此我將只能從蹂躪敵人的肉體中得到心靈的寄託，直到永遠。", ""},
		        "H3b 天賦風味文字兩行、GGPK 一段(Vaal Pact)");
		expectG({"10% increased Attack Speed", kNat1, kNat2, "+30 to Dexterity"}, "passive",
		        {N, kNatZh[0], kNatZh[1], N}, "H4 前後的單行 stat 不入組(維持逐行路徑)");
		expectG({kNat1, "+30 to Dexterity"}, "passive", {N, N}, "H5 只有第 1 行(後面不是續行)不成組");
		expectG({"Wield the gifts of Beidat, luminary of the pale unseeing,",
		         "whose honeyed words bore tunnels through pliant minds."}, "gem", {N, N},
		        "H6 風味文字英繁行數相同不入組(逐行字典負責)");
		expectG({"10% chance to gain Adrenaline for 2 Seconds when Leech is", "removed by Filling Unreserved Life",
		         "10% chance to gain Onslaught for 4 Seconds when Leech is", "removed by Filling Unreserved Life"},
		        "item", {u8"當偷取被填滿未保留生命移除時，有 10% 機率獲得腎上腺素 2 秒", "",
		                 u8"當偷取被填滿未保留生命移除時，有 10% 機率獲得猛攻 4 秒", ""},
		        "H7 兩組共用同一續行文字各自成組(Tanu Ahi)");
		expectG({"Spells have Added Spell Damage equal to 30% of Physical Damage of", "your Equipped Two handed Weapon"},
		        "passive", {u8"法術擁有等同於你已裝備雙手武器30%物理傷害的附加法術傷害", ""},
		        "H8 stat 譯文取 stats_by_id(含裁定:GGPK {0] 筆誤已修)(魔縛符文之結)");
		expectG({"Trigger level 1 Summon Spirit of Ikiaho Skill when you use a Travel",
		         "Skill while a Unique Enemy is in your Presence"}, "passive",
		        {u8"有傳奇敵人在你的存在範圍內且你使用快行技能時，", u8"觸發等級 1 的召喚伊基亞呼之靈技能"},
		        "H9 紋身兩行 stat 依 GGPK 中文分行(伊基亞呼)");
		{
			translation_set_enabled(false);
			const G off = group({kNat1, kNat2}, "passive");
			translation_set_enabled(true);
			const G on = group({kNat1, kNat2}, "passive");
			check(off.n == 0 && off.out == std::vector<std::string>({N, N}) && on.n == 1,
			      "H10 翻譯關閉(F2)時不成組,開回後照常");
		}
		// H11:製作下拉「/」串接列(ItemsTab table.concat(mod, "/"))整句一行。
		{
			const char* a = translation_lookup("Bleeding Enemies you Kill Explode, dealing #% of/their Maximum Life as Physical Damage");
			const char* b = translation_lookup("Bleeding Enemies you Kill Explode, dealing (2-3)% of/their Maximum Life as Physical Damage");
			const std::string wa = u8"擊殺流血的敵人時產生爆炸，造成等同其 #% 最大生命的物理傷害";
			const std::string wb = u8"擊殺流血的敵人時產生爆炸，造成等同其 (2-3)% 最大生命的物理傷害";
			check(a && wa == a, "H11a 「/」串接的兩行描述整句一行(# 值)");
			if (!(a && wa == a)) printf("           got '%s'\n", a ? a : "(null)");
			check(b && wb == b, "H11b 「/」串接的兩行描述整句一行(範圍值)");
			if (!(b && wb == b)) printf("           got '%s'\n", b ? b : "(null)");
			const char* c = translation_lookup("Life/Mana");
			const char* c2 = translation_lookup("Life");
			check(!(c && std::string(c).find(u8"生命") != std::string::npos && std::string(c).find('/') == std::string::npos) && c2,
			      "H11c 不是目錄描述的「/」字串不走整句重組");
			// H11d/e:專精彈窗列以 " / " 串接(POB 原樣):一句的整句一行;兩行的照 GGPK 分行、分隔照原字串
			const char* d = translation_lookup("Increases and Reductions to Armour also apply to Energy / Shield Recharge Rate at 20% of their value");
			const std::string wd = u8"增加和減少護甲同時以 20% 它們的值套用至能量護盾充能率";
			check(d && wd == d, "H11d 「 / 」串接、GGPK 一句:整句一行");
			if (!(d && wd == d)) printf("           got '%s'\n", d ? d : "(null)");
			const char* e = translation_lookup("Enemies you Kill that are affected by Elemental Ailments / grant 100% increased Flask Charges");
			const std::string we = u8"擊殺受元素異常狀態影響的敵人 / 增加 100% 藥劑充能";
			check(e && we == e, "H11e 「 / 」串接、GGPK 兩行:各行照 GGPK 分行,分隔維持原字串的「 / 」");
			if (!(e && we == e)) printf("           got '%s'\n", e ? e : "(null)");
		}
		// H12:同一段英文在目錄裡有兩個不同的 GGPK 繁中(SupportGemMinionPact 三行 / SupportGemHarrowingThrong
		// 一段)= 歧義,不整段取任一個:不成組,留給逐行字典。
		expectG({"Catarina envisioned a Wraeclast of disparate societies united.",
		         "That diversity would empower the Syndicate - and the increased",
		         "number of dispensible bodies was a convenient coincidence."}, "gem", {N, N, N},
		        "H12 同英文多個 GGPK 繁中的風味文字不成組(歧義)");
		// H13(C1,使用者裁定 2026-10-25):POB 製作列把模板寫死的 100 也寫成 #;群組 / 「/」查表時 # 視為吻合
		// 寫死的數字,並顯示 GGPK 寫死的 100。
		{
			const char* kP1 = "#% increased Poison Duration for each Poison you have inflicted Recently, up";
			const char* kP2 = "to a maximum of #%";
			const Said plain = ask((std::string(kP1) + "\n" + kP2).c_str(), "item");   // 先查:快取不得串到下面
			const std::string row = std::string(kP1) + "/" + kP2;
			const char* a = translation_lookup(row.c_str());
			const std::string wa = u8"近期每個你造成的中毒增加 #% 中毒持續時間，/最多 100%";
			check(a && wa == a, "H13a 「/」整句查表:# 吻合寫死的 100,顯示 GGPK 的「最多 100%」");
			if (!(a && wa == a)) printf("           got '%s'\n", a ? a : "(null)");
			expectG({kP1, kP2}, "item", {u8"近期每個你造成的中毒增加 #% 中毒持續時間，", u8"最多 100%"},
			        "H13b 群組查表同規則(兩行各自照 GGPK)");
			check(plain.null, "H13c 一般 stat 查表不套用(# 不吻合寫死的數字,與群組結果互不串快取)");
			if (!plain.null) printf("           plain='%s'\n", shown(plain));
			expectG({"3% increased Poison Duration for each Poison you have inflicted Recently, up", "to a maximum of 100%"},
			        "item", {u8"近期每個你造成的中毒增加 3% 中毒持續時間，", u8"最多 100%"},
			        "H13d 實際數值時照舊(寫死的數字要相同)");
			expectG({"3% increased Poison Duration for each Poison you have inflicted Recently, up", "to a maximum of 90%"},
			        "item", {N, N}, "H13e 寫死的數字不同仍不成組");
		}
		// H14(C2):一列含單行詞綴 + 一句兩行描述(製作下拉 3 段暴擊球):組外的段各自完整查表,全部有譯文才採用。
		{
			const char* a = translation_lookup("+1 to Maximum Power Charges/10% chance that if you would gain Power Charges, "
			                                   "you instead gain up to/your maximum number of Power Charges");
			const std::string wa = u8"+1 最大暴擊球數量/若你獲得暴擊球，有 10% 機率獲得最大上限暴擊球";
			check(a && wa == a, "H14a 一列多段:組外單行各自查表、組內照 GGPK,以原分隔接回");
			if (!(a && wa == a)) printf("           got '%s'\n", a ? a : "(null)");
			const char* b = translation_lookup("Zzqnope Untranslatable Words/10% chance that if you would gain Power Charges, "
			                                   "you instead gain up to/your maximum number of Power Charges");
			const bool bok = !b || std::string(b).find(u8"若你獲得暴擊球") == std::string::npos;
			check(bok, "H14b 組外有一段查不到:整列不採用(不出現半中半英)");
			if (!bok) printf("           got '%s'\n", b);
		}
		// H15(審查第 5 輪):「/」查表語境由呼叫端設定(專精彈窗 passive);快取依語境分開。
		// 這句兩行描述只在 passive_skill_stat_descriptions,item 語境查不到(--tr 實測 no match)。
		{
			const char* kRow = "20% less maximum Life / Increases and reductions to Maximum Life also apply to Maximum Rage at 20% Value";
			const std::string want = u8"20%更少最大生命 / 增加和減少生命同時以他們數值的20%套用至最大盛怒";
			auto look = [&]() { const char* r = translation_lookup(kRow); return r ? std::string(r) : std::string("(null)"); };
			const std::string item0 = look();
			const char* prev = translation_set_slash_context("passive");
			const std::string pas = look();
			const char* back = translation_set_slash_context(prev);
			const std::string item1 = look();
			check(pas == want, "H15a 語境設為 passive:天賦描述檔才有的兩行描述整句重組");
			check(item0 != want && item1 == item0, "H15b 預設(item)語境不採用,切回後結果與切換前相同(快取依語境)");
			check(prev == nullptr && back && std::string(back) == "passive", "H15c 設定函式回傳前一個語境(預設 nullptr)");
			if (pas != want || item0 == want || item1 != item0)
				printf("           item0='%s' passive='%s' item1='%s'\n", item0.c_str(), pas.c_str(), item1.c_str());
		}
	}

	// ---- X:顯示排除清單(使用者裁定 2026-10-25 方案 a)-------------------------------
	printf("\n=== X 顯示排除清單(display_exclude.json) ===\n");
	{
		const std::vector<std::pair<std::string, std::string>> kUi = {
		    {"Zzqexcl Word", u8"排除詞"}, {"Zzqctrl Word", u8"控制詞"},
		    {"Zzqexcl gain 5 things", u8"排除獲得 5 件"}, {"Zzqctrl gain 5 things", u8"控制獲得 5 件"},
		    {"Zzqexcl line one\nZzqexcl line two", u8"排除一\n排除二"},
		    {"Zzqctrl line one\nZzqctrl line two", u8"控制一\n控制二"},
		    // X12:被排除的單行鍵,同檔另有一筆沒列的多行鍵,第 1 行就是它(V10B「You can't deal Damage…」的形狀)
		    {"Zzqexcl dual one", u8"舊一"}, {"Zzqexcl dual one\nZzqexcl dual two", u8"舊一\n舊二"},
		    // X13:被排除的鍵,同檔另有一筆帶 GGG [X|Y] 標記、去標記後同句的鍵(V10B「Cursed Enemies…」的形狀)
		    {"Zzqexcl brk 5 things", u8"舊括號 5 件"}, {"[Zzqbrk|Zzqexcl] brk 5 things", u8"舊括號 5 件"}};
		const std::vector<std::pair<std::string, std::string>> kPas = {
		    {"Zzqexcl Source Word", u8"排除來源"}, {"Zzqctrl Source Word", u8"控制來源"},
		    {"Zzqexcl src 5 things", u8"排除來源 5 件"}, {"Zzqexcl src one\nZzqexcl src two", u8"來源一\n來源二"},
		    {"Zzqexcl dual one\nZzqexcl dual three", u8"新一\n新三"}};
		// 基底名:items.json(is_base_items)的鍵被排除,同一句的譯文改由 ggpk_full.json 提供(出貨資料的實際形狀)。
		const std::vector<std::pair<std::string, std::string>> kItems = {{"Zzqexcl Base", u8"排除基底"}};
		// ggpk_full.json(最後載入、不列排除)提供同句的 GGPK 值:被排除的鍵改回落到這裡。
		const std::vector<std::pair<std::string, std::string>> kFull = {
		    {"Zzqexcl Base", u8"排除基底"}, {"Zzqexcl Source Word", u8"全量來源"},
		    {"Zzqexcl src 5 things", u8"全量來源 5 件"}, {"Zzqexcl src one", u8"全量一"}};
		// 鍵逐檔:stats.json 列的 Zzqctrl Word 不在 stats.json,不影響 ui.json 的同名鍵。
		const std::string kExclude =
		    R"({"version":1,"entries":{"ui.json":["Zzqexcl Word","Zzqexcl gain 5 things","Zzqexcl dual one","Zzqexcl brk 5 things",)"
		    R"("Zzqexcl line one\nZzqexcl line two"],"passives.json":["Zzqexcl Source Word",)"
		    R"("Zzqexcl src 5 things","Zzqexcl src one\nZzqexcl src two"],"items.json":["Zzqexcl Base"],)"
		    R"("stats.json":["Zzqctrl Word"]}})";
		struct XR {
			std::string word, ctrl, pat, ctrlPat, line, ctrlLine, gl, ctrlGl, src, ctrlSrc, rWord, rPat, rLine;
			std::string srcMerged, srcPat, srcPatMerged, srcLine, srcLineMerged, title, dual, brk;
		};
		auto lk = [](const char* s) {
			const char* r = translation_lookup(s);
			return r ? std::string(r) : std::string("(null)");
		};
		auto rv = [](const char* s) {
			const char* r = translation_reverse_lookup(s);
			return r ? std::string(r) : std::string("(null)");
		};
		auto rvt = [](const char* s) {
			char* r = translation_reverse_text(s);
			std::string o = r ? std::string(r) : std::string("(null)");
			if (r) translation_free(r);
			return o;
		};
		auto run = [&](const wchar_t* name, bool withFile, XR& o) {
			const std::wstring vroot = root + name + L"\\";
			const std::wstring loc = vroot + L"zh-rTW\\";
			CreateDirectoryW(vroot.c_str(), nullptr);
			std::string w;
			bool ok = build_dict_copy(shippedLocale, loc, statsJson, true, w) &&
			          plant_file(loc + L"ui.json", kUi) && plant_file(loc + L"passives.json", kPas) &&
			          plant_file(loc + L"items.json", kItems) && plant_file(loc + L"ggpk_full.json", kFull);
			DeleteFileW((loc + L"display_exclude.json").c_str());   // 出貨資料若已帶,先拿掉
			if (withFile) ok = ok && write_bytes(loc + L"display_exclude.json", kExclude);
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", vroot.c_str());
			translation_set_source(nullptr);
			translation_reload();
			o.word = lk("Zzqexcl Word");
			o.ctrl = lk("Zzqctrl Word");
			o.pat = lk("Zzqexcl gain 7 things");
			o.ctrlPat = lk("Zzqctrl gain 7 things");
			o.line = lk("Zzqexcl line one");
			o.ctrlLine = lk("Zzqctrl line one");
			o.gl = lk("Exclusive to: Zzqexcl Word");
			o.ctrlGl = lk("Exclusive to: Zzqctrl Word");
			o.rWord = rv(u8"排除詞");
			o.rPat = rvt(u8"排除獲得 7 件");
			o.rLine = rv(u8"排除一");
			o.srcMerged = lk("Zzqexcl Source Word");
			o.srcPatMerged = lk("Zzqexcl src 8 things");
			o.srcLineMerged = lk("Zzqexcl src one");
			o.title = lk("Loath Joy, Zzqexcl Base");
			o.dual = lk("Zzqexcl dual one");
			o.brk = lk("Zzqexcl brk 7 things");
			translation_set_source("passives");
			o.src = lk("Zzqexcl Source Word");
			o.ctrlSrc = lk("Zzqctrl Source Word");
			o.srcPat = lk("Zzqexcl src 8 things");
			o.srcLine = lk("Zzqexcl src one");
			translation_set_source(nullptr);
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
			translation_reload();
			remove_tree(vroot);
			if (!ok) printf("           setup %ls failed %s\n", name, w.c_str());
			return ok;
		};
		XR off, on;
		const bool setup = run(L"x_nofile", false, off) && run(L"x_file", true, on);
		auto show = [](const char* tag, const XR& r) {
			printf("           [%s] word='%s' ctrl='%s' pat='%s' ctrlPat='%s' line='%s' ctrlLine='%s'\n"
			       "                  gl='%s' ctrlGl='%s' src='%s' ctrlSrc='%s'\n"
			       "                  rWord='%s' rPat='%s' rLine='%s'\n"
			       "                  srcMerged='%s' srcPat='%s' srcPatMerged='%s' srcLine='%s' srcLineMerged='%s'\n"
			       "                  title='%s' dual='%s' brk='%s'\n",
			       tag, r.word.c_str(), r.ctrl.c_str(), r.pat.c_str(), r.ctrlPat.c_str(), r.line.c_str(),
			       r.ctrlLine.c_str(), r.gl.c_str(), r.ctrlGl.c_str(), r.src.c_str(), r.ctrlSrc.c_str(),
			       r.rWord.c_str(), r.rPat.c_str(), r.rLine.c_str(), r.srcMerged.c_str(), r.srcPat.c_str(),
			       r.srcPatMerged.c_str(), r.srcLine.c_str(), r.srcLineMerged.c_str(), r.title.c_str(),
			       r.dual.c_str(), r.brk.c_str());
		};
		const std::string kTitle = u8"惡行 歡愉, 排除基底";
		const bool x1 = setup && off.word == u8"排除詞" && off.pat == u8"排除獲得 7 件" && off.line == u8"排除一" &&
		                off.gl == u8"限定: 排除詞" && off.src == u8"排除來源" && off.srcPat == u8"排除來源 8 件" &&
		                off.srcLine == u8"來源一" && off.srcMerged == u8"全量來源" && off.title == kTitle &&
		                off.dual == u8"舊一" && off.brk == u8"舊括號 7 件";
		check(x1, "X1 沒有 display_exclude.json 時行為不變(精確/樣板/逐行/詞彙表/來源都查得到)");
		const bool x2 = setup && on.word == "(null)" && on.ctrl == u8"控制詞";
		check(x2, "X2 排除鍵正向精確查不到,同檔未列的鍵照常");
		const bool x3 = setup && on.pat == "(null)" && on.ctrlPat == u8"控制獲得 7 件";
		check(x3, "X3 排除鍵不進正向樣板(換數字也查不到)");
		const bool x4 = setup && on.line == "(null)" && on.ctrlLine == u8"控制一";
		check(x4, "X4 排除鍵不進逐行衍生");
		const bool x5 = setup && on.gl.find(u8"排除詞") == std::string::npos && on.ctrlGl == u8"限定: 控制詞";
		check(x5, "X5 排除鍵不進詞彙表(句中替換不出現該詞)");
		// 使用者裁定 2026-10-25「全部改 GGPK」:標了來源的位置也不用被排除鍵的舊值,改回落到合併表(GGPK 值)。
		const bool x6 = setup && on.src == u8"全量來源" && on.ctrlSrc == u8"控制來源" && on.srcMerged == u8"全量來源";
		check(x6, "X6 標了來源也不回被排除鍵的舊值,改回落合併表的 GGPK 值");
		const bool x6b = setup && on.srcPat == u8"全量來源 8 件" && on.srcLine == u8"全量一" &&
		                 on.srcPatMerged == u8"全量來源 8 件" && on.srcLineMerged == u8"全量一";
		check(x6b, "X6b 來源字典的樣板與逐行衍生同樣排除,改回落合併表");
		// V10B 根因:被排除的鍵經由同檔其他條目回到正向表 —— 沒列的多行鍵的衍生行、帶 [X|Y] 標記的同句鍵。
		const bool x12 = setup && on.dual == u8"新一";
		check(x12, "X12 同檔沒列的多行鍵,衍生出被排除的那一行時也不進(改由後面的檔提供)");
		const bool x13 = setup && on.brk == "(null)";
		check(x13, "X13 同檔帶 [X|Y] 標記、去標記後就是被排除鍵的條目也不進(樣板不回舊值)");
		const bool x9 = setup && on.title == kTitle;
		check(x9, "X9 被排除的基底名仍認得是基底:「<稀有名>, <基底>」照常逐字翻前段");
		const bool x7 = setup && on.rWord == "Zzqexcl Word" && on.rPat == "Zzqexcl gain 7 things" &&
		                on.rLine == "Zzqexcl line one";
		check(x7, "X7 排除鍵仍進反向表(貼上中文→英文:精確/樣板/逐行)");
		const bool x8 = setup && on.ctrl == off.ctrl && on.ctrlPat == off.ctrlPat && on.ctrlLine == off.ctrlLine &&
		                on.ctrlGl == off.ctrlGl && on.ctrlSrc == off.ctrlSrc;
		check(x8, "X8 清單逐檔生效:別檔列的同名鍵與未列鍵結果與沒清單時相同");
		if (!(x1 && x2 && x3 && x4 && x5 && x6 && x6b && x7 && x8 && x9 && x12 && x13)) {
			show("無清單", off);
			show("有清單", on);
		}
		// X10/X11:出貨資料(含出貨的 display_exclude.json)。gems.json 被排除的 8 個寶石名在 gems 來源也回
		// ggpk_full.json 的值(使用者裁定:猛攻輔助→猛攻也接受);「<稀有名>, <被排除的基底>」照常翻。
		{
			std::string body;
			bool haveFile = GetFileAttributesW((flagRoot + L"zh-rTW\\display_exclude.json").c_str()) != INVALID_FILE_ATTRIBUTES;
			const char* kGems[] = {"Onslaught", "Bow", "Bone Armour", "Capacitor", "Fan the Flames", "Fire Attunement",
			                       "Ghost Dance", "Rend"};
			bool ok10 = haveFile && read_bytes(flagRoot + L"zh-rTW\\ggpk_full.json", body);
			try {
				const auto full = nlohmann::json::parse(body)["entries"];
				translation_set_source("gems");
				for (const char* g : kGems) {
					const std::string want = full.contains(g) && full[g].is_string() ? full[g].get<std::string>() : "";
					const char* r = translation_lookup(g);
					const std::string got = r ? r : "(null)";
					if (want.empty() || got != want) {
						ok10 = false;
						printf("           gems source %s: got '%s' want '%s'\n", g, got.c_str(), want.c_str());
					}
				}
				translation_set_source(nullptr);
			} catch (...) {
				translation_set_source(nullptr);
				ok10 = false;
			}
			check(ok10, "X10 出貨資料:被排除的 8 個寶石名在 gems 來源回 ggpk_full 的 GGPK 值(猛攻、弓…)");
			const char* t = translation_lookup("Loath Joy, Vaal Regalia");
			const std::string want11 = u8"惡行 歡愉, 瓦爾法衣";
			check(haveFile && t && want11 == t, "X11 出貨資料:「Loath Joy, Vaal Regalia」(基底在排除清單)照常翻成整串");
			if (!(haveFile && t && want11 == t)) printf("           file=%d got '%s'\n", haveFile ? 1 : 0, t ? t : "(null)");
		}
	}

	// ---- T:POB 尾端裝飾(使用者裁定 2026-10-26)-----------------------------------------------
	// 製作/自訂詞綴下拉的「 (Prefix)」「 ^8(Suffix)」與不支援詞綴的「 ^8(Not supported in PoB yet)」:剝掉後
	// 本文照常查表(GGPK 優先),再接回 —— 前/後綴用字典的「Prefix/Suffix」譯法,註記維持英文且原樣保留。
	// 剝除後的本文查表優先於整串舊鍵;本文查不到時才走原本的整串路徑(不會變英文)。
	printf("\n=== T 尾端裝飾(Prefix/Suffix/Not supported) ===\n");
	{
		auto lk = [](const char* s) {
			const char* r = translation_lookup(s);
			return r ? std::string(r) : std::string("(null)");
		};
		auto expectT = [&](const char* in, const std::string& want, const char* name) {
			const std::string got = lk(in);
			check(got == want, name);
			if (got != want) printf("           %s\n           got  '%s'\n           want '%s'\n", in, got.c_str(), want.c_str());
		};
		// 出貨資料:本文以 stat 查表(GGPK),合併表的舊樣板「增加 #%火焰傷害」不再勝出
		expectT("(7-12)% increased Fire Damage (Suffix)", u8"增加 (7-12)% 火焰傷害 (後綴)",
		        "T1 自訂詞綴下拉「 (Suffix)」:本文照 GGPK、接回 (後綴)");
		expectT("(7-12)% increased Fire Damage ^8(Prefix)", u8"增加 (7-12)% 火焰傷害 ^8(前綴)",
		        "T2 工藝詞綴下拉「 ^8(Prefix)」:色碼原樣保留、接回 (前綴)");
		// 註記:引擎不知道這行的語境(物品/寶石/天賦由 inject 依 stat id 先查),本文取「同一行不帶註記」的答案
		{
			const std::string bare = lk("^xF05050(7-12)% increased Fire Damage");
			const std::string got = lk("^xF05050(7-12)% increased Fire Damage ^8(Not supported in PoB yet)");
			const bool ok3 = bare != "(null)" && got == bare + " ^8(Not supported in PoB yet)";
			check(ok3, "T3 不支援註記:本文與不帶註記的同一行相同,註記英文原樣(含 ^8)接回");
			if (!ok3) printf("           bare='%s' got='%s'\n", bare.c_str(), got.c_str());
		}
		expectT("Bleeding Enemies you Kill Explode, dealing (2-3)% of/their Maximum Life as Physical Damage (Prefix)",
		        u8"擊殺流血的敵人時產生爆炸，造成等同其 (2-3)% 最大生命的物理傷害 (前綴)",
		        "T4 「/」串接的兩行描述帶 (Prefix):整句重組後接回 (前綴)");
		// 植入的複本:整串舊鍵與本文新鍵並存
		const std::wstring vroot = root + L"t_deco\\";
		const std::wstring loc = vroot + L"zh-rTW\\";
		CreateDirectoryW(vroot.c_str(), nullptr);
		std::string w;
		const std::vector<std::pair<std::string, std::string>> kUiT = {
		    {"Zzqdeco gain 5 power (Suffix)", u8"舊整串 5 威力 (後綴)"},
		    {"Zzqdeco gain 5 power (Not supported in PoB yet)", u8"舊註記整串 5 威力 (Not supported in PoB yet)"},
		    {"Zzqonly whole 5 (Suffix)", u8"整串才有 5 (後綴)"}};
		const std::vector<std::pair<std::string, std::string>> kFullT = {{"Zzqdeco gain 5 power", u8"新本文 5 威力"}};
		bool ok = build_dict_copy(shippedLocale, loc, statsJson, true, w) && plant_file(loc + L"ui.json", kUiT) &&
		          plant_file(loc + L"ggpk_full.json", kFullT);
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", vroot.c_str());
		translation_reload();
		const std::string t5 = lk("Zzqdeco gain 7 power (Suffix)");
		const std::string t6 = lk("^xF05050Zzqdeco gain 7 power ^8(Not supported in PoB yet)");
		const std::string t7 = lk("Zzqonly whole 9 (Suffix)");
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();
		remove_tree(vroot);
		if (!ok) printf("           setup t_deco failed %s\n", w.c_str());
		check(ok && t5 == u8"新本文 7 威力 (後綴)", "T5 剝除後的本文查表優先於整串舊鍵");
		if (t5 != u8"新本文 7 威力 (後綴)") printf("           got '%s'\n", t5.c_str());
		check(ok && t6 == u8"^xF05050新本文 7 威力 ^8(Not supported in PoB yet)", "T6 註記同規則:本文新鍵優先、註記原樣接回");
		if (t6 != u8"^xF05050新本文 7 威力 ^8(Not supported in PoB yet)") printf("           got '%s'\n", t6.c_str());
		check(ok && t7 == u8"整串才有 9 (後綴)", "T7 本文查不到時退回整串路徑(不會變英文)");
		if (t7 != u8"整串才有 9 (後綴)") printf("           got '%s'\n", t7.c_str());
		// T8:POB 裝飾只由 decorated_tail_lookup 處理;本文查不到時,「括號註記」步驟(3.5)不再拿本文裡更前面的
		// 「 (」亂切(舊行為:「生命 (5-6) gained ^8(Not supported in PoB yet)」半中半英)。
		expectT("Life (5-6) gained ^8(Not supported in PoB yet)", "(null)", "T8 註記行本文查不到:整行維持英文,不從本文中間切出半句中文");
		expectT("Mana (5-6) gained ^8(Suffix)", "(null)", "T8b 前/後綴行本文查不到:同上");
		// T9:不是 POB 裝飾的括號註記照舊由 3.5 處理
		expectT("Chaos Orb (Unused)", u8"混沌石 ^9(未使用)", "T9 一般括號註記(Unused)照舊");
		// T10:兩種疊在一起 —— POB 裝飾在最後、一般括號註記在本文裡:剝裝飾後本文交給 3.5,兩段都接回
		expectT("Chaos Orb (Unused) ^8(Suffix)", u8"混沌石 ^9(未使用) ^8(後綴)",
		        "T10 「X (Unused) ^8(Suffix)」:本文的 (Unused) 照 3.5 譯、尾端 (Suffix) 接回 (後綴)");
		expectT("Chaos Orb (Unused) ^8(Not supported in PoB yet)", u8"混沌石 ^9(未使用) ^8(Not supported in PoB yet)",
		        "T10b 「X (Unused) ^8(Not supported in PoB yet)」:註記維持英文接回");
	}

	// ---- EO:「只比整句」的鍵(exact_only.json,使用者裁定 2026-10-26)--------------------------------
	// 被列的鍵只進精確表(正向、來源副本、反向精確);不建正向/反向樣板、不做逐行衍生、不進詞彙表;排除清單優先。
	printf("\n=== EO 只比整句的鍵(exact_only.json) ===\n");
	{
		const std::vector<std::pair<std::string, std::string>> kFullE = {
		    {"Zzqeo gain 5 power", u8"GGPK 5 威力"}, {"Zzqeo Phrase", u8"整句詞"},
		    {"Zzqeo line one\nZzqeo line two", u8"整一\n整二"}, {"Zzqeo both 5", u8"雙列 5"},
		    {"Limited to 1 Runegraft of Bellows", u8"限制 1 個轟鳴符文之結"}};
		const std::vector<std::pair<std::string, std::string>> kUiE = {{"Zzqeo gain 3 power", u8"樣板 3 威力"}};
		const std::string kExactOnly =
		    R"({"version":1,"entries":{"ggpk_full.json":["Zzqeo gain 5 power","Zzqeo Phrase",)"
		    R"("Zzqeo line one\nZzqeo line two","Zzqeo both 5","Limited to 1 Runegraft of Bellows"]}})";
		struct ER { std::string exact, other, glossWord, gloss, rExact, rPat, line, both, rg, rgNote; };
		auto lk = [](const char* s) {
			const char* r = translation_lookup(s);
			return r ? std::string(r) : std::string("(null)");
		};
		auto run = [&](const wchar_t* name, bool withFile, ER& o) {
			const std::wstring vroot = root + name + L"\\";
			const std::wstring loc = vroot + L"zh-rTW\\";
			CreateDirectoryW(vroot.c_str(), nullptr);
			std::string w, ex;
			bool ok = build_dict_copy(shippedLocale, loc, statsJson, true, w) && plant_file(loc + L"ggpk_full.json", kFullE) &&
			          plant_file(loc + L"ui.json", kUiE);
			// 排除清單優先:把 Zzqeo both 5 加進出貨的 display_exclude.json(複本)
			try {
				nlohmann::ordered_json doc = nlohmann::ordered_json::object();
				if (read_bytes(loc + L"display_exclude.json", ex)) doc = nlohmann::ordered_json::parse(ex);
				else doc["version"] = 1;
				doc["entries"]["ggpk_full.json"].push_back("Zzqeo both 5");
				ok = ok && write_bytes(loc + L"display_exclude.json", doc.dump(1));
			} catch (...) { ok = false; }
			DeleteFileW((loc + L"exact_only.json").c_str());
			if (withFile) ok = ok && write_bytes(loc + L"exact_only.json", kExactOnly);
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", vroot.c_str());
			translation_set_source(nullptr);
			translation_reload();
			o.exact = lk("Zzqeo gain 5 power");
			o.other = lk("Zzqeo gain 7 power");
			o.glossWord = lk("Zzqeo Phrase");
			o.gloss = lk("Exclusive to: Zzqeo Phrase");
			{ const char* r = translation_reverse_lookup(u8"GGPK 5 威力"); o.rExact = r ? r : "(null)"; }
			{ char* r = translation_reverse_text(u8"GGPK 9 威力"); o.rPat = r ? r : "(null)"; if (r) translation_free(r); }
			o.line = lk("Zzqeo line one");
			o.both = lk("Zzqeo both 5");
			o.rg = lk("Limited to 1 Runegraft of Bellows");
			o.rgNote = lk("^xF05050Limited to 1 Runegraft of Bellows ^8(Not supported in PoB yet)");
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
			translation_reload();
			remove_tree(vroot);
			if (!ok) printf("           setup %ls failed %s\n", name, w.c_str());
			return ok;
		};
		ER off, on;
		const bool setup = run(L"eo_off", false, off) && run(L"eo_on", true, on);
		auto show = [](const char* tag, const ER& r) {
			printf("           [%s] exact='%s' other='%s' glossWord='%s' gloss='%s'\n"
			       "                  rExact='%s' rPat='%s' line='%s' both='%s'\n                  rg='%s' rgNote='%s'\n",
			       tag, r.exact.c_str(), r.other.c_str(), r.glossWord.c_str(), r.gloss.c_str(), r.rExact.c_str(),
			       r.rPat.c_str(), r.line.c_str(), r.both.c_str(), r.rg.c_str(), r.rgNote.c_str());
		};
		// 沒有清單 = 現行:ggpk_full 的帶數字鍵被做成樣板,外溢到別的數字(EO 對照組)
		const bool e0 = setup && off.other == u8"GGPK 7 威力" && off.gloss == u8"限定: 整句詞" && off.line == u8"整一" &&
		                off.rPat == "Zzqeo gain 9 power";
		check(e0, "EO0 沒有 exact_only.json 時行為不變(帶數字鍵照舊做成樣板、進詞彙表、有逐行衍生)");
		check(setup && on.exact == u8"GGPK 5 威力", "EO1 被列的鍵整句精確命中");
		check(setup && on.other == u8"樣板 7 威力", "EO2 被列的鍵不建樣板:同樣板別的數字仍走原本的樣板(不外溢)");
		check(setup && on.glossWord == u8"整句詞" && on.gloss.find(u8"整句詞") == std::string::npos,
		      "EO3 被列的鍵不進詞彙表(句中替換不出現)");
		check(setup && on.rExact == "Zzqeo gain 5 power" && on.rPat != "Zzqeo gain 9 power",
		      "EO4 反查只留精確,不建反向樣板");
		check(setup && on.line == "(null)", "EO5 被列的多行鍵不做逐行衍生");
		check(setup && on.both == "(null)", "EO6 同時在排除清單:排除優先(正向查不到)");
		check(setup && on.rg == u8"限制 1 個轟鳴符文之結" &&
		      on.rgNote == u8"^xF05050限制 1 個轟鳴符文之結 ^8(Not supported in PoB yet)",
		      "EO7 符文「Limited to 1 Runegraft of …」整句鍵:浮窗帶註記的那行也接得住");
		if (!(e0 && on.other == u8"樣板 7 威力" && on.line == "(null)")) {
			show("無清單", off);
			show("有清單", on);
		}
	}

	// ---- R12(使用者裁定 2026-10-27:舊字典既有錯譯現在修)------------------------------------------
	// GL 詞彙表詞條不從數字中間開始比;BR 整段方括號 "[...]" 把裡面整句查;CS 只差大小寫的兩個樣板
	// 輸入大小寫完全相同的那個優先;KE 登記為「維持英文」的整串顯示字不翻(傳奇風味)。
	printf("\n=== R12 詞彙表數字邊界 / 整段方括號 / 大小寫不同的樣板 / 維持英文 ===\n");
	{
		const std::vector<std::pair<std::string, std::string>> kUiR = {
		    {"Zzqgb Alpha", u8"甲"}, {"2% zzqgb power", u8"增加 2% 威力G"},
		    {"{0}% zzqbr power", u8"增加 {0}% 威力B"}, {"Zzqbr Essence", u8"精髓B"},
		    {"Zzqcs aura {0}% Effect on you for", u8"大寫 {0}%"}, {"Zzqcs aura {0}% effect on you for", u8"小寫 {0}%"},
		    {"Zzqke flavour line", u8"風味句"}, {"Zzqcol Name", u8"名稱C"}, {"{0}% zzqcol speed", u8"增加 {0}% 速度C"},
		    {"ZZQUP", u8"佔位Z"}, {"234 zzqgb gold", u8"二三四金"}, {"Zzqke piece", u8"片段句"}};
		auto lk = [](const char* s) {
			const char* r = translation_lookup(s);
			return r ? std::string(r) : std::string("(null)");
		};
		const std::wstring vroot = root + L"r12\\";
		const std::wstring loc = vroot + L"zh-rTW\\";
		CreateDirectoryW(vroot.c_str(), nullptr);
		std::string w;
		const bool setup = build_dict_copy(shippedLocale, loc, statsJson, true, w) && plant_file(loc + L"ui.json", kUiR);
		if (!setup) printf("           setup r12 failed %s\n", w.c_str());
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", vroot.c_str());
		translation_set_source(nullptr);
		translation_reload();
		const std::string g1 = lk("Zzqgb Alpha 32% zzqgb power"), g2 = lk("Zzqgb Alpha 2% zzqgb power");
		const std::string b1 = lk("[32% zzqbr power]"), b2 = lk("Zzqbr Essence   ^8[32% zzqbr power] (Prefix)");
		const std::string c1 = lk("Zzqcs aura 8% Effect on you for"), c2 = lk("Zzqcs aura 8% effect on you for"),
		                  c3 = lk("ZZQCS AURA 8% EFFECT ON YOU FOR");
		const std::string pad(48, ' ');
		const std::string t1 = lk(("Zzqcol Name" + pad + "15% zzqcol speed").c_str());
		const std::string t2 = lk(("Zzqcol Name" + pad + "Zzqcol unknown words").c_str());
		const std::string t3 = lk("Zzqcol Name   15% zzqcol speed");
		const std::string b3 = lk("[ZZQUP]"), b4 = lk("[UNUSED]"), b4bare = lk("UNUSED");
		const std::string g3 = lk("Zzqgb Alpha 1,234 zzqgb gold");
		const std::string k4 = lk("Zzqke piece");
		const char* kScope[] = {"Zzqke piece"};
		translation_set_keep_english_scope(kScope, 1);
		const std::string k5 = lk("Zzqke piece");
		translation_set_keep_english_scope(nullptr, 0);
		const std::string k6 = lk("Zzqke piece");
		const char* kKe = "^xAF6025Zzqke flavour line";
		const std::string k0 = lk(kKe);
		translation_keep_english(kKe);
		const std::string k1 = lk(kKe), k2 = lk("Zzqke flavour line");
		translation_reload();
		const std::string k3 = lk(kKe);
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();
		remove_tree(vroot);
		printf("           g1='%s' g2='%s'\n           b1='%s' b2='%s'\n           c1='%s' c2='%s' c3='%s'\n"
		       "           k0='%s' k1='%s' k2='%s' k3='%s'\n",
		       g1.c_str(), g2.c_str(), b1.c_str(), b2.c_str(), c1.c_str(), c2.c_str(), c3.c_str(), k0.c_str(), k1.c_str(),
		       k2.c_str(), k3.c_str());
		check(setup && g1.find(u8"3增加") == std::string::npos,
		      "GL1 詞彙表詞條不從數字中間開始比(「32%」不會被拆成「3」+「增加 2%」)");
		check(setup && g2 == u8"甲 增加 2% 威力G", "GL2 詞條前面是空白時照常替換(對照組)");
		check(setup && b1 == u8"[增加 32% 威力B]", "BR1 整段方括號 [..] 把裡面整句查表、括號接回");
		check(setup && b2 == u8"精髓B   ^8[增加 32% 威力B] (前綴)",
		      "BR2 精髓列「名稱   ^8[詞綴] (Prefix)」:方括號內整句翻、數字不拆");
		check(setup && c1 == u8"大寫 8%", "CS1 只差大小寫的兩個樣板:輸入大小寫完全相同的那個優先");
		check(setup && c2 == u8"小寫 8%", "CS2 另一個大小寫的輸入取另一個樣板");
		check(setup && c3 == u8"小寫 8%", "CS3 大小寫都對不上時照舊(後載入的勝)");
		check(setup && k0 == u8"^xAF6025風味句" && k1 == "(null)" && k2 == u8"風味句",
		      "KE1 登記維持英文的整串顯示字不翻;沒登記的(不帶色碼)照翻");
		check(setup && k3 == "(null)", "KE2 維持英文的登記不因重新載入字典而消失");
		// 使用者裁定 2026-10-23/24:下拉列名稱後 48 空白的英文詞綴是隱藏搜尋字,維持英文
		check(setup && t1 == u8"名稱C" + pad + "15% zzqcol speed",
		      "TL1 下拉列「名稱<48 空白>隱藏搜尋字」只翻名稱欄,48 空白後的英文原樣保留");
		check(setup && t2 == u8"名稱C" + pad + "Zzqcol unknown words", "TL2 隱藏搜尋字翻不翻得出來都不影響(只看名稱欄)");
		check(setup && t3 == "(null)", "TL3 不是下拉列(只有 3 空白)不逐欄翻");
		check(setup && b3 != u8"[佔位Z]", "BR3 方括號內是全大寫佔位字([UNUSED] 類)不走方括號整句翻");
		check(setup && (b4bare == "(null)" || b4 != "[" + b4bare + "]"),
		      "BR4 方括號內是詞彙表黑名單鍵(UNUSED)不走方括號整句翻(使用者裁定 2026-10-23 維持英文)");
		check(setup && g3.find(u8"1,二三四") == std::string::npos, "GL3 千分位逗號後的數字也算同一個數,不從中間比");
		check(setup && k4 == u8"片段句" && k5 == "(null)" && k6 == u8"片段句",
		      "KE3 範圍內(浮窗繪製期間)的字串不翻,範圍清掉後照翻");
		printf("           t1='%s'\n           t2='%s' t3='%s'\n           b3='%s' b4='%s' b4bare='%s' g3='%s'\n"
		       "           k4='%s' k5='%s' k6='%s'\n",
		       t1.c_str(), t2.c_str(), t3.c_str(), b3.c_str(), b4.c_str(), b4bare.c_str(), g3.c_str(), k4.c_str(),
		       k5.c_str(), k6.c_str());
	}

	// ---- R15(使用者裁定 2026-10-28「更改」:下拉列改用語言無關 id)-----------------------------------
	// ID 新的 id 表(詞綴→stat id、寶石名、昇華名、天賦→stat id)由 meta.json source_dicts 載入;
	// SR 一列詞綴以該詞綴的 stat id 走 stats_by_id(單行 / 「/」接的多行 / 尾綴 (Prefix)(Suffix));
	// DO 繪製期間的覆寫表(某控制項正在畫的字 → 依 id 翻好的中文),清掉後照舊。
	// 植入的 id 值抄自 GGPK(pos_affix/out/_cache 的 rel_mod_stat / passiveskills / ascendancy /
	// activeskills,2026-10-28 讀),不是虛構;出貨字典目前沒有這幾個檔(待 pob_export 補)。
	printf("\n=== R15 id 表 / 一列詞綴走 stat id / 繪製覆寫 ===\n");
	{
		auto lk = [](const char* s) {
			const char* r = translation_lookup(s);
			return r ? std::string(r) : std::string("(null)");
		};
		auto row = [](const char* s, const char* ids) {
			const char* r = translation_lookup_stat_row(s, "item", ids);
			return r ? std::string(r) : std::string("(null)");
		};
		auto byId = [](const char* src, const char* id, const char* en) {
			const char* r = translation_lookup_by_id_en(src, id, en);
			return r ? std::string(r) : std::string("(null)");
		};
		const std::wstring vroot = root + L"r15\\";
		const std::wstring loc = vroot + L"zh-rTW\\";
		CreateDirectoryW(vroot.c_str(), nullptr);
		std::string w;
		nlohmann::ordered_json modStats, passiveStats, gemNames, ascNames;
		modStats["source_files"] = {"ggpk:rel_mod_stat by Mods.Id"};
		modStats["entries"] = {{"DelveWeaponChanceToGainOnslaughtOnKill2h1", "chance_to_gain_onslaught_on_kill_%"},
		                       {"MaximumPowerChargeInfluenceMaven",
		                        "max_power_charges gain_maximum_power_charges_on_power_charge_gained_%_chance"}};
		passiveStats["source_files"] = {"ggpk:passiveskills.Stats by PassiveSkillGraphId"};
		passiveStats["entries"] = {{"42687", "base_dexterity_and_intelligence two_handed_melee_accuracy_rating_+% "
		                                     "two_handed_melee_attack_speed_+% two_handed_melee_critical_strike_chance_+%"}};
		gemNames["source_files"] = {"ggpk:activeskills.DisplayedName by GrantedEffects.Id"};
		gemNames["entries"] = {{"GemDetonateMines", u8"遙控地雷"}};
		gemNames["templates"] = {{"GemDetonateMines", "Detonate Mines"}};
		ascNames["source_files"] = {"ggpk:ascendancy.Name by Ascendancy.Id"};
		ascNames["entries"] = {{"Raider", u8"守林人"}};
		ascNames["templates"] = {{"Raider", "Warden"}};
		const bool setup = build_dict_copy(shippedLocale, loc, statsJson, true, w) &&
		                   plant_file(loc + L"ui.json", {{"Zzqdo row", u8"一般列"}}) &&
		                   add_source_dict(loc, "mod_stats_by_id.json", modStats) &&
		                   add_source_dict(loc, "passive_stats_by_id.json", passiveStats) &&
		                   add_source_dict(loc, "gem_names_by_id.json", gemNames) &&
		                   add_source_dict(loc, "ascendancy_by_id.json", ascNames);
		if (!setup) printf("           setup r15 failed %s\n", w.c_str());
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", vroot.c_str());
		translation_set_source(nullptr);
		translation_reload();
		const std::string i1 = byId("mod_stats", "DelveWeaponChanceToGainOnslaughtOnKill2h1", nullptr);
		const std::string i2 = byId("passive_stats", "42687", nullptr);
		const std::string i3 = byId("gem_names", "GemDetonateMines", "Detonate Mines");
		const std::string i3b = byId("gem_names", "GemDetonateMines", "Detonate Minez");
		const std::string i4 = byId("ascendancy", "Raider", "Warden");
		const char* kPow = "+1 to Maximum Power Charges";
		const char* kOns = "20% chance to gain Onslaught for 4 seconds on Kill (Suffix)";
		const char* kSlash = "+1 to Maximum Power Charges/10% chance that if you would gain Power Charges, "
		                     "you instead gain up to/your maximum number of Power Charges";
		const std::string f1 = lk(kPow), f2 = lk(kOns);
		const std::string s1 = row(kPow, "max_power_charges");
		const std::string s2 = row("+1 to Level of Socketed Gems", "local_socketed_gem_level_+");
		const std::string s3 = row("#% chance to gain Onslaught for # seconds on Kill", "chance_to_gain_onslaught_on_kill_%");
		const std::string s4 = row(kOns, "chance_to_gain_onslaught_on_kill_%");
		const std::string s5 = row(kSlash, "max_power_charges gain_maximum_power_charges_on_power_charge_gained_%_chance");
		const std::string s6 = row("Drops Scorched Ground while moving, lasting 4 seconds",
		                           "boots_implicit_ground_scorched_duration_ms");
		const std::string s7a = row(kPow, nullptr), s7b = row(kPow, "");
		const std::string s8 = row("+1 to Maximum Power Charges/Zzq no such line", "max_power_charges");
		const std::string s9 = row("^8+1 to Maximum Power Charges", "max_power_charges");
		const std::string s10 = row(kPow, "zzq_no_such_stat");
		// 多階合併列的 '#' 吃掉了負號(POB 把 "-9" 換成 "#");GGPK 模板 {0:+d} 照同值代入寫回 "+#"
		const std::string s11 = row("Nearby Enemies have #% to Fire Resistance",
		                            "base_fire_damage_resistance_% local_display_nearby_enemy_fire_damage_resistance_%");
		const std::string s11b = lk("Nearby Enemies have #% to Fire Resistance");
		const std::string d0 = lk("Zzqdo row");
		const char* oKeys[] = {"Zzqdo row"};
		const char* oVals[] = {u8"覆寫列"};
		translation_set_draw_override(oKeys, oVals, 1);
		const std::string d1 = lk("Zzqdo row"), d1b = lk(kPow);
		// 技能清單列「A, B」(兩個寶石名以逗號接):逐段查,每段都先看覆寫表
		const std::string d3 = lk("Zzqdo row, Chaos Orb");
		translation_set_draw_override(nullptr, nullptr, 0);
		const std::string d2 = lk("Zzqdo row");
		// ID5:測試自帶「沒有這 4 個 id 表」的字典(從出貨字典複製後刪檔、移出 meta),不看出貨字典
		// 有沒有它們 —— 出貨字典補上 id 表後(資料線 f00e05c)舊寫法就失準。
		const std::wstring nroot = root + L"r15none\\";
		const std::wstring nloc = nroot + L"zh-rTW\\";
		CreateDirectoryW(nroot.c_str(), nullptr);
		const std::vector<std::string> kIdFiles = {"mod_stats_by_id.json", "passive_stats_by_id.json",
		                                           "gem_names_by_id.json", "ascendancy_by_id.json"};
		std::string wn;
		const bool setupNone = build_dict_copy(shippedLocale, nloc, statsJson, true, wn) && drop_dict_files(nloc, kIdFiles);
		if (!setupNone) printf("           setup r15none failed %s\n", wn.c_str());
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", nroot.c_str());
		translation_reload();
		const std::string i5 = byId("mod_stats", "DelveWeaponChanceToGainOnslaughtOnKill2h1", nullptr);
		const std::string i5b = byId("passive_stats", "42687", nullptr);
		const std::string i5c = byId("gem_names", "GemDetonateMines", "Detonate Mines");
		const std::string i5d = byId("ascendancy", "Raider", "Warden");
		const std::string i5e = lk("Chaos Orb");   // 前提:字典本身照常載入
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
		translation_reload();
		remove_tree(vroot);
		remove_tree(nroot);
		printf("           i5b='%s' i5c='%s' i5d='%s' i5e='%s'\n", i5b.c_str(), i5c.c_str(), i5d.c_str(), i5e.c_str());
		printf("           i1='%s' i2='%s'\n           i3='%s' i3b='%s' i4='%s' i5='%s'\n", i1.c_str(), i2.c_str(),
		       i3.c_str(), i3b.c_str(), i4.c_str(), i5.c_str());
		printf("           f1='%s' f2='%s'\n           s1='%s' s2='%s'\n           s3='%s' s4='%s'\n"
		       "           s5='%s'\n           s6='%s' s7a='%s' s7b='%s' s8='%s' s9='%s' s10='%s'\n"
		       "           d0='%s' d1='%s' d1b='%s' d2='%s'\n",
		       f1.c_str(), f2.c_str(), s1.c_str(), s2.c_str(), s3.c_str(), s4.c_str(), s5.c_str(), s6.c_str(),
		       s7a.c_str(), s7b.c_str(), s8.c_str(), s9.c_str(), s10.c_str(), d0.c_str(), d1.c_str(), d1b.c_str(),
		       d2.c_str());
		check(setup && i1 == "chance_to_gain_onslaught_on_kill_%", "ID1 詞綴→stat id 表(mod_stats_by_id.json)由 source_dicts 載入");
		check(setup && i2.rfind("base_dexterity_and_intelligence ", 0) == 0, "ID2 天賦→stat id 表(passive_stats_by_id.json)載入");
		check(setup && i3 == u8"遙控地雷" && i3b == "(null)", "ID3 寶石名以 GrantedEffects id 查(英文對不上不給)");
		check(setup && i4 == u8"守林人", "ID4 昇華名以 Ascendancy id 查");
		check(setupNone && i5 == "(null)" && i5b == "(null)" && i5c == "(null)" && i5d == "(null)" &&
		          i5e != "(null)" && i5e != "Chaos Orb",
		      "ID5 字典沒有這 4 個 id 表時(測試自帶的字典)查不到、不出錯,其餘照常翻");
		check(setup && setupNone && i1 != "(null)" && i2 != "(null)" && i3 != "(null)" && i4 != "(null)",
		      "ID6 有 id 表時四種都查得到(同一批 id;對照 ID5 沒表時查不到)");
		check(setup && f1 == u8"+1 最大暴擊球數量" && s1 == u8"+1 最大暴擊球",
		      "SR1 一列詞綴以 stat id 走物品描述檔(對照:扁平查表是天賦描述的「數量」)");
		check(setup && s2 == u8"此物品插槽中寶石等級 +1", "SR2 插槽寶石等級以 stat id 得物品描述檔譯文");
		check(setup && s3 == u8"擊殺時 #% 機率獲得 # 秒猛攻", "SR3 多階合併列(數字皆 #)以 stat id 選對描述");
		check(setup && f2 == u8"擊殺時有 20% 機率獲得 4 秒猛攻效果 (後綴)" && s4 == u8"擊殺時 20% 機率獲得 4 秒猛攻 (後綴)",
		      "SR4 帶 (Suffix) 的列以 stat id 選對同英文的兩個 stat(對照:沒有 id 時選錯),尾綴照字典");
		check(setup && s5 == u8"+1 最大暴擊球/若你獲得暴擊球，有 10% 機率獲得最大上限暴擊球",
		      "SR5 「/」接的多行列:兩行一句整句翻、其餘行以 stat id 翻");
		check(setup && s6 == u8"移動時留下焦灼地面，持續 4 秒", "SR6 同英文兩個 stat(焦灼地面)以 stat id 選對");
		check(setup && s7a == "(null)" && s7b == "(null)", "SR7 沒有 stat id 不回答(呼叫端照舊)");
		check(setup && s8 == "(null)", "SR8 有一段翻不出來整列不回答(不出現半中半英)");
		check(setup && s9 == u8"^8+1 最大暴擊球", "SR9 開頭色碼保留");
		check(setup && s10 == "(null)", "SR10 stat id 對不上任何描述不回答");
		check(setup && s11 == u8"附近敵人 +#% 火焰抗性" && s11b == s11,
		      "SR11 合併列無正負號的 # 落在 {0:+d}:照 GGPK 模板寫 +#(與扁平查表、參考同)");
		if (!(s11 == u8"附近敵人 +#% 火焰抗性" && s11b == s11)) printf("           s11='%s' s11b='%s'\n", s11.c_str(), s11b.c_str());
		check(setup && d0 == u8"一般列" && d1 == u8"覆寫列" && d2 == u8"一般列",
		      "DO1 繪製覆寫表期間該字畫成覆寫中文,清掉後照舊(含已快取的字)");
		check(setup && d1b == f1, "DO2 覆寫表沒列的字不受影響");
		check(setup && d3 == u8"覆寫列, 混沌石", "DO3 逗號接起來的列逐段翻時,覆寫表對每一段都生效(技能清單「A, B」)");
		if (d3 != u8"覆寫列, 混沌石") printf("           d3='%s'\n", d3.c_str());
	}

	// ---- C1(使用者裁定 2026-10-29「PoE1 / PoE2 依遊戲走各自規則」)-------------------------------
	// 屬性查詢(translation_lookup_stat)對輸入行的方括號處理依字典 meta.json 的 "source":
	// poe2 的畫面字串不帶 GGPK 標記,不去方括號(POB 的 [整句] 外框要原樣當成查不到);
	// poe1(含沒寫)照舊去掉。樣板端不變。
	printf("\n=== C1 屬性查詢的方括號處理依 meta source ===\n");
	{
		const std::string bracketed = std::string("[") + kE2 + "]";
		auto run = [&](const wchar_t* sub, const char* source, Said& bare, Said& br) {
			const std::wstring vroot = root + sub + L"\\";
			const std::wstring loc = vroot + L"zh-rTW\\";
			CreateDirectoryW(vroot.c_str(), nullptr);
			std::string w;
			const bool ok = build_dict_copy(shippedLocale, loc, statsJson, true, w) && set_meta_source(loc, source);
			if (!ok) printf("           setup c1 %s failed %s\n", source, w.c_str());
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", vroot.c_str());
			SetEnvironmentVariableW(L"POB_GAME", nullptr);   // 沒設 POB_GAME:由 meta source 決定(C1 要測的就是這條)
			translation_set_source(nullptr);
			translation_reload();
			bare = ask(kE2, "item");
			br = ask(bracketed.c_str(), "item");
			SetEnvironmentVariableW(L"POB_GAME", L"poe1");
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
			translation_reload();
			remove_tree(vroot);
			return ok;
		};
		Said bare2{true, {}}, br2{true, {}}, bare1{true, {}}, br1{true, {}};
		const bool ok2 = run(L"c1poe2", "poe2", bare2, br2);
		const bool ok1 = run(L"c1poe1", "poe1", bare1, br1);
		printf("           poe2 bare='%s' bracketed='%s'\n           poe1 bare='%s' bracketed='%s'\n",
		       shown(bare2), shown(br2), shown(bare1), shown(br1));
		check(ok2 && !bare2.null && bare2.s == kE2Want, "C1a meta source=poe2:不帶方括號的已知單行 stat 照常翻");
		check(ok2 && br2.null && !bare2.null, "C1b meta source=poe2:帶方括號的同一句不去方括號、回空(對照 C1a 同環境有命中)");
		check(ok1 && !br1.null && br1.s.find(kE2Want) != std::string::npos,
		      "C1c meta source=poe1:帶方括號的同一句照舊去方括號、翻成中文(現行行為)");
	}

	// ---- G(引擎拆分第 1 期:遊戲由 POB_GAME 決定,沒設才看 meta source)----------------------------
	// translation_game() / translation_meta_source() 在最近一次載入時定下;方括號規則改看 game。
	// POB_GAME 有設、meta source 非空且兩者不同 → 寫一筆 i18n 錯誤記錄(含 POB_GAME 與 source)。
	printf("\n=== G 遊戲判斷:POB_GAME 優先,沒設才看 meta source ===\n");
	{
		const std::wstring logDir = root + L"gamelog\\";
		CreateDirectoryW(logDir.c_str(), nullptr);
		const std::string bracketed = std::string("[") + kE2 + "]";
		struct GameRun {
			bool setup = false;
			std::string game, metaSrc;
			Said bare{true, {}}, br{true, {}};
			bool mismatch = false;
			std::string log;
		};
		// envGame == nullptr:POB_GAME 不設。跑完把環境還原成進來時的狀態(POB_GAME=poe1、POB_ZH_DATADIR=flagRoot)。
		auto run = [&](const wchar_t* sub, const wchar_t* envGame, const char* metaSource) {
			GameRun r;
			const std::wstring vroot = root + sub + L"\\";
			const std::wstring loc = vroot + L"zh-rTW\\";
			CreateDirectoryW(vroot.c_str(), nullptr);
			std::string w;
			r.setup = build_dict_copy(shippedLocale, loc, statsJson, true, w) && set_meta_source(loc, metaSource);
			if (!r.setup) printf("           setup g %s failed %s\n", narrow(sub).c_str(), w.c_str());
			clear_error_logs(logDir);
			PobLog::SetDirForTest(logDir);
			PobLog::ResetCapsForTest();
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", vroot.c_str());
			SetEnvironmentVariableW(L"POB_GAME", envGame);
			translation_set_source(nullptr);
			translation_reload();
			const char* g = translation_game();
			const char* m = translation_meta_source();
			r.game = g ? g : "(null)";
			r.metaSrc = m ? m : "(null)";
			r.bare = ask(kE2, "item");
			r.br = ask(bracketed.c_str(), "item");
			r.log = read_error_logs(logDir);
			r.mismatch = has_i18n_mismatch_line(r.log);
			SetEnvironmentVariableW(L"POB_GAME", L"poe1");
			SetEnvironmentVariableW(L"POB_ZH_DATADIR", flagRoot.c_str());
			translation_reload();
			PobLog::SetDirForTest(L"");
			remove_tree(vroot);
			return r;
		};
		auto brHit = [&](const GameRun& r) { return !r.br.null && r.br.s.find(kE2Want) != std::string::npos; };
		auto brKept = [&](const GameRun& r) { return r.br.null && !r.bare.null && r.bare.s == kE2Want; };

		// T3:POB_GAME=poe2 壓過 meta source=poe1
		const GameRun t3 = run(L"g1", L"poe2", "poe1");
		printf("           G1 game='%s' meta='%s' mismatch=%d bare='%s' bracketed='%s'\n", t3.game.c_str(),
		       t3.metaSrc.c_str(), (int)t3.mismatch, shown(t3.bare), shown(t3.br));
		check(t3.setup && t3.game == "poe2" && t3.metaSrc == "poe1",
		      "G1a POB_GAME=poe2、meta source=poe1:translation_game 回 poe2、translation_meta_source 回 poe1");
		check(t3.setup && t3.mismatch,
		      "G1b POB_GAME 與 meta source 不一致:錯誤記錄有一行 [i18n] 同時提到 POB_GAME 與 source");
		if (!t3.mismatch) printf("           log='%s'\n", t3.log.c_str());
		check(t3.setup && brKept(t3),
		      "G1c POB_GAME=poe2 壓過 meta=poe1:帶 [整句] 外框的屬性行保留外框(查不到),對照不帶外框的同句有譯文");

		// T4:POB_GAME 沒設 → 照 meta;不報不一致
		const GameRun t4a = run(L"g2a", nullptr, "poe2");
		const GameRun t4b = run(L"g2b", nullptr, "poe1");
		printf("           G2 poe2: game='%s' meta='%s'  poe1: game='%s' meta='%s'\n", t4a.game.c_str(),
		       t4a.metaSrc.c_str(), t4b.game.c_str(), t4b.metaSrc.c_str());
		check(t4a.setup && t4a.game == "poe2" && t4a.metaSrc == "poe2" && brKept(t4a) && !t4a.mismatch,
		      "G2a 沒設 POB_GAME、meta source=poe2:game 為 poe2、保留方括號外框、不寫不一致記錄");
		check(t4b.setup && t4b.game == "poe1" && t4b.metaSrc == "poe1" && brHit(t4b) && !t4b.mismatch,
		      "G2b 沒設 POB_GAME、meta source=poe1:game 為 poe1、去掉方括號照常翻、不寫不一致記錄");

		// T4b:兩者一致 → 不誤報(附「不一致時確實會寫」的對照,空殼 logger 過不了)
		const GameRun t4c = run(L"g3", L"poe1", "poe1");
		check(t4c.setup && t4c.game == "poe1" && !t4c.mismatch && t3.mismatch && brHit(t4c),
		      "G3 POB_GAME=poe1、meta source=poe1 一致:不寫不一致記錄(對照 G1b 不一致時有寫)");
		if (t4c.mismatch) printf("           log='%s'\n", t4c.log.c_str());
		remove_tree(logDir);
	}

	printf("\n=== 無 source_lookup 旗標的語系 ===\n");
	{
		// E8:同一份資料、同樣列在 source_dicts,只差 meta 沒有 source_lookup。
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", noflagRoot.c_str());
		translation_reload();
		const Said noflag = ask(kE2, "item");
		const std::string built = translation_debug_sources();
		// E8b:真的 PoE2 出貨字典 —— 預期依它自己的 meta.json(有 source_lookup 且列有 stats_by_id.json 才翻)。
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", nullptr);
		SetEnvironmentVariableW(L"POB_GAME", L"poe2");
		translation_reload();
		const Said poe2 = ask(kE2, "item");
		SetEnvironmentVariableW(L"POB_GAME", L"poe1");
		bool poe2Flag = false, poe2Listed = false, poe2MetaOk = false;
		{
			std::string pm;
			if (read_bytes(dir + L"Data\\poe2\\zh-rTW\\meta.json", pm)) {
				try {
					const auto m = nlohmann::ordered_json::parse(pm);
					poe2Flag = m.contains("source_lookup") && m["source_lookup"].is_boolean() && m["source_lookup"].get<bool>();
					poe2Listed = in_array(m, "source_dicts", "stats_by_id.json");
					poe2MetaOk = true;
				} catch (...) {}
			}
		}
		const bool poe2Wants = poe2Flag && poe2Listed;   // 出貨 meta 要求依 stats_by_id 翻
		check(noflag.null && e2Hit, "E8 無 source_lookup 旗標:不走 stat 查詢(同資料有旗標時有命中)");
		if (!(noflag.null && e2Hit)) printf("           noflag='%s' guardE2=%d\n", shown(noflag), e2Hit);
		const bool poe2Ok = poe2MetaOk && (poe2Wants ? (!poe2.null && !poe2.s.empty()) : poe2.null);
		check(poe2Ok, "E8b PoE2 出貨字典:有 source_lookup 時依 stats_by_id 翻、沒有時回空(與出貨 meta 一致)");
		if (!poe2Ok) printf("           meta 讀到=%d source_lookup=%d 列有 stats_by_id.json=%d poe2='%s'\n",
		                    poe2MetaOk, poe2Flag, poe2Listed, shown(poe2));
		// 「不建索引」唯一看得到的觀察點:無旗標時來源表只有 gems。
		check(built == "exact=gems;pattern=;id=", "E8c 無旗標時不額外建來源/id 表");
		if (built != "exact=gems;pattern=;id=") printf("           %s\n", built.c_str());
	}

	// 收尾:環境變數回原狀、刪除複本。
	SetEnvironmentVariableW(L"POB_ZH_DATADIR", nullptr);
	SetEnvironmentVariableW(L"POB_GAME", L"poe1");
	translation_reload();
	remove_tree(root);
	const bool cleaned = GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES;
	check(cleaned, "ER 測試字典複本已刪除");

	// 結果碼:0 = 全過;3 = 有失敗;4 = 沒有失敗但有 WAIT(等另一分支的資料,斷言還沒生效,不能當全綠)
	if (g_wait) {
		printf("\nstat selftest: %d passed, %d failed, %d waiting for data\n", g_pass, g_fail, g_wait);
		printf("!!! WAITING FOR DATA: %d 筆斷言等資料進來才生效(結果碼 %d),不是全綠 !!!\n", g_wait, g_fail ? 3 : 4);
	} else {
		printf("\nstat selftest: %d passed, %d failed\n", g_pass, g_fail);
	}
	return g_fail ? 3 : (g_wait ? 4 : 0);
}
