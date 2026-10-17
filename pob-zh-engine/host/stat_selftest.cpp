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
	// C 層只回文字;每行補色碼目前在 ui_api.cpp 的 Lua 綁定(只補開頭一次),host
	// 測不到 —— 這裡只釘住 C 層輸出是真換行兩行,色碼見回報的建議。
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
