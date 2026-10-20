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

#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

#include <json.hpp>

#include "../translate/translation_manager.h"

namespace {

int g_pass = 0;
int g_fail = 0;

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

} // namespace

int RunStatSelftest(const std::wstring& statsJsonArg)
{
	g_pass = g_fail = 0;
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

	printf("\n=== 無 source_lookup 旗標的語系 ===\n");
	{
		// E8:同一份資料、同樣列在 source_dicts,只差 meta 沒有 source_lookup。
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", noflagRoot.c_str());
		translation_reload();
		const Said noflag = ask(kE2, "item");
		const std::string built = translation_debug_sources();
		// E8b:真的 PoE2 出貨字典(無旗標)。
		SetEnvironmentVariableW(L"POB_ZH_DATADIR", nullptr);
		SetEnvironmentVariableW(L"POB_GAME", L"poe2");
		translation_reload();
		const Said poe2 = ask(kE2, "item");
		SetEnvironmentVariableW(L"POB_GAME", L"poe1");
		check(noflag.null && e2Hit, "E8 無 source_lookup 旗標:不走 stat 查詢(同資料有旗標時有命中)");
		if (!(noflag.null && e2Hit)) printf("           noflag='%s' guardE2=%d\n", shown(noflag), e2Hit);
		check(poe2.null, "E8b PoE2 出貨字典(無旗標)查同句回空");
		if (!poe2.null) printf("           poe2='%s'\n", shown(poe2));
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

	printf("\nstat selftest: %d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 3;
}
