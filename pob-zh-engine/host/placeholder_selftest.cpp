// 多佔位符詞條的數值填位回歸。
//
// 這裡防守的不變量只有一句:一個 {N} 的值,不管中文把它排到句子的哪裡,都必須落在
// 標著 {N} 的那一格。舊做法把 {0}/{1}/{2} 全部正規化成 '#' 之後照出現順序填,所以
// 中英語序相反的詞條(光 poe1 就 2,373 條)數值必定填反:
//
//   EN  {0} to {1} Added Attack Lightning Damage per {2} Accuracy Rating
//   ZH  每 {2} 命中值附加 {0} 至 {1} 攻擊閃電傷害
//   舊  每 1 命中值附加 6 至 200 攻擊閃電傷害      <- 值照中文的出現順序塞
//   新  每 200 命中值附加 1 至 6 攻擊閃電傷害
//
// 三支進入點:
//   --placeholder-selftest              邊角案例表 + 出貨字典端到端 + 突變測試
//   --placeholder-dump <out.tsv> [game] 全量 A/B 快照(改動前後各跑一次逐行比對)
//   --placeholder-probe <english>       單筆正向查詢,排查用
//
// 案例表走 translation_debug_fill,也就是載入期與執行期共用的那份計畫產生器 —— 不是
// 在測試裡另寫一份對照邏輯。本專案吃過「驗證器與被驗證的程式共享同一個盲點」的虧,
// 這裡的取捨是反過來的:寧可讓測試直接驅動生產程式碼,也不要有第二份實作。
//
// 為什麼案例大多是合成的:編號不從 0 開始、目標用了來源沒有的編號、同一個 {0} 用
// 兩次、樣板裡寫死數字 —— 出貨字典裡不見得樣樣齊全,而為了測試往字典塞假資料等於
// 改變被測對象。合成樣板 + 真實字典端到端兩者都做,才同時有鑑別力與代表性。

#include "placeholder_selftest.h"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "editor_data.h"
#include "../translate/translation_manager.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* what)
{
	(ok ? g_pass : g_fail)++;
	printf("  %s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
}

std::wstring exe_dir()
{
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(nullptr, path, MAX_PATH);
	std::wstring s(path);
	size_t slash = s.find_last_of(L'\\');
	return slash == std::wstring::npos ? L"" : s.substr(0, slash + 1);
}

std::string narrow(const std::wstring& w)
{
	if (w.empty()) return {};
	int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

// pob-zh 是 WIN32 子系統的 exe,啟動時根本沒有 stdout,printf 直接掉進黑洞。既有的
// 自檢都自己 AttachConsole(ATTACH_PARENT_PROCESS),但那樣一來從 PowerShell 重導就
// 收不到 —— 輸出跑去主控台緩衝區,不走管線。所以這裡先把 stdout 導進報告檔,結束時
// 接回主控台再把檔案內容回放一次:螢幕上看得到,腳本也讀得到同一份。
struct TeeOut {
	FILE*        f = nullptr;
	std::wstring path;

	explicit TeeOut(const wchar_t* name)
	{
		path = exe_dir() + name;
		_wfreopen_s(&f, path.c_str(), L"w", stdout);
	}
	~TeeOut()
	{
		fflush(stdout);
		if (f) fclose(f);
		if (AttachConsole(ATTACH_PARENT_PROCESS)) {
			FILE* c = nullptr;
			freopen_s(&c, "CONOUT$", "w", stdout);
		}
		FILE* r = nullptr;
		_wfopen_s(&r, path.c_str(), L"rb");
		if (r) {
			char   buf[4096];
			size_t n;
			while ((n = fread(buf, 1, sizeof(buf), r)) > 0) fwrite(buf, 1, n, stdout);
			fclose(r);
		}
		fflush(stdout);
	}
};

std::string tsv_safe(const std::string& s)
{
	std::string r;
	r.reserve(s.size());
	for (char c : s) r += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
	return r;
}

// ---------------------------------------------------------------- 案例表

// 一列 = 來源樣板 / 目標樣板 / 一行實際輸入 / 應該吐出來的字。
// `order` 標記這一列的正確性是否取決於「依編號填」:突變測試只要求這些列在舊行為下
// 變紅。沒標的列是「舊行為本來就對」的,它們必須在兩種模式下都過 —— 過度修正同樣
// 是回歸,把碰巧正確的案例一併寫進來就是為了擋這個。
struct Case {
	const char* name;
	const char* src;
	const char* dst;
	const char* in;
	const char* want;
	bool        order;
};

const Case kCases[] = {
	// ---- 五種佔位符寫法 ----
	{ "{N} 重排(使用者回報 1)",
	  "{0} to {1} Added Attack Lightning Damage per {2} Accuracy Rating",
	  "每 {2} 命中值附加 {0} 至 {1} 攻擊閃電傷害",
	  "1 to 6 Added Attack Lightning Damage per 200 Accuracy Rating",
	  "每 200 命中值附加 1 至 6 攻擊閃電傷害", true },

	{ "{N:+d} 重排(使用者回報 2)",
	  "{0:+d} to maximum number of Summoned Golems if you have {1} Primordial Items Socketed",
	  "若你鑲嵌 {1} 個原始物品，召喚魔像的最大數量增加 {0:+d} 個",
	  "+1 to maximum number of Summoned Golems if you have 3 Primordial Items Socketed",
	  "若你鑲嵌 3 個原始物品，召喚魔像的最大數量增加 +1 個", true },

	{ "{N:d} 單格(語序相同,舊行為本來就對)",
	  "Split Arrow fires {0} additional arrows",
	  "裂化箭矢發射 {0:d} 個額外投射物",
	  "Split Arrow fires 3 additional arrows",
	  "裂化箭矢發射 3 個額外投射物", false },

	{ "{N:+} 與字面數字同句",
	  "Chain Hook has {0:+} metre to radius per 12 Rage",
	  "奪魂勾索每 12 層盛怒 {0:+} 米範圍",
	  "Chain Hook has +2 metre to radius per 12 Rage",
	  "奪魂勾索每 12 層盛怒 +2 米範圍", true },

	// ---- 邊角 1:字面數字寫死的變體 ----
	// 同一條詞綴常有兩個變體並存(一個 {2}、一個寫死 200)。寫死的那一格在中文樣板裡
	// 沒有對應的 {N},依編號索引時必須正確跳過。
	{ "字面 200 變體:值仍照編號落位",
	  "{0} to {1} Added Attack Lightning Damage per 200 Accuracy Rating",
	  "每 200 命中值附加 {0} 至 {1} 攻擊閃電傷害",
	  "1 to 6 Added Attack Lightning Damage per 200 Accuracy Rating",
	  "每 200 命中值附加 1 至 6 攻擊閃電傷害", true },

	// 雜湊會把 200 與 250 摺成同一個鍵,所以「per 250」的實物會命中「per 200」那條
	// 樣板。字面格取的是輸入的值而不是樣板的值,這種撞鍵才不會印出錯的常數。
	{ "字面格跟著輸入走,不是照抄樣板",
	  "{0} to {1} Added Attack Lightning Damage per 200 Accuracy Rating",
	  "每 200 命中值附加 {0} 至 {1} 攻擊閃電傷害",
	  "1 to 6 Added Attack Lightning Damage per 250 Accuracy Rating",
	  "每 250 命中值附加 1 至 6 攻擊閃電傷害", true },

	// 中文自己多寫了一個來源沒有的常數:沒有來源可取,照抄樣板,而不是吃掉一個真值。
	{ "目標多出的字面數字照抄,不吃真值",
	  "{0} to maximum number of Golems",
	  "最多可同時擁有額外 {0} 個魔像，上限 3 個",
	  "+2 to maximum number of Golems",
	  "最多可同時擁有額外 +2 個魔像，上限 3 個", true },

	// ---- 邊角 2:GGG markup 不可被當成佔位符 ----
	{ "[Key|Display] 標記",
	  "{0} to maximum number of Summoned Golems if you have {1} Primordial Items Socketed or [Equipped]",
	  "若你鑲嵌或[Equipped|已裝備] {1} 個原始物品，召喚魔像的最大數量增加 {0} 個",
	  "+1 to maximum number of Summoned Golems if you have 3 Primordial Items Socketed or Equipped",
	  "若你鑲嵌或已裝備 3 個原始物品，召喚魔像的最大數量增加 +1 個", true },

	{ "<colour>{{...}} 標記 + 重排",
	  "<default>{{<white>{{Awakening Bonus:}} Complete this map with at least tier {0} and Awakening level {1}}}",
	  "<default>{{<white>{{覺醒加成:}} 在至少覺醒等級 {1} 和地圖階級 {0} 下完成此地圖}}",
	  "<default>{{<white>{{Awakening Bonus:}} Complete this map with at least tier 14 and Awakening level 8}}",
	  "<default>{{<white>{{覺醒加成:}} 在至少覺醒等級 8 和地圖階級 14 下完成此地圖}}", true },

	// "<grey>{(" 裡的 '{' 緊接著 '('，不是 {N};而括號裡的 (20-30) 是一個範圍,
	// 會被摺成一格 —— 它必須被當成字面數字,不能吃掉 {0} 的值。
	{ "<grey>{(範圍)} 不是佔位符",
	  "{0}% increased Damage <grey>{(20-30)}",
	  "<grey>{(20-30)} 增加 {0}% 傷害",
	  "35% increased Damage <grey>{(20-30)}",
	  "<grey>{(20-30)} 增加 35% 傷害", true },

	// ---- 邊角 3:編號不從 0 開始 / 不連續 / 重複 / 越界 ----
	{ "編號不從 0 開始({1} 單獨出現)",
	  "Bathed in the blood of {1} sacrificed in the name of Xibaqua",
	  "浸泡在以賽巴昆之名獻祭的 {1} 條生命中",
	  "Bathed in the blood of 88 sacrificed in the name of Xibaqua",
	  "浸泡在以賽巴昆之名獻祭的 88 條生命中", false },

	{ "編號不連續({0} 與 {2},中間沒有 {1})",
	  "{0} Life and {2} Mana",
	  "{2} 魔力與 {0} 生命",
	  "50 Life and 30 Mana",
	  "30 魔力與 50 生命", true },

	{ "同一個編號在目標出現兩次",
	  "{0}% increased Effect",
	  "增加 {0}% 效果(共 {0}%)",
	  "25% increased Effect",
	  "增加 25% 效果(共 25%)", true },

	// 越界:目標引用了來源沒有的 {1}。不得崩潰,也不得安靜地填一個錯的數字進去 ——
	// 留 '#' 是「這裡沒有值」的既有寫法,看得見。
	{ "目標引用來源沒有的編號:留 '#' 不亂填",
	  "{0} Damage",
	  "{0} 至 {1} 傷害",
	  "5 Damage",
	  "5 至 # 傷害", false },

	// ---- 邊角 4:抽值規則(正負號吸收 / 範圍 / 進階複製 / 千分位)不得改變 ----
	{ "進階複製 7(6-11) 併成一格",
	  "{0}% increased Burning Damage",
	  "增加 {0}% 燃燒傷害",
	  "19(16-20)% increased Burning Damage",
	  "增加 19(16-20)% 燃燒傷害", false },

	{ "負值範圍 -72(-80--70) 四個減號",
	  "{0}% to all Elemental Resistances",
	  "{0}% 所有元素抗性",
	  "-72(-80--70)% to all Elemental Resistances",
	  "-72(-80--70)% 所有元素抗性", false },

	{ "5-10 是分隔不是負號",
	  "Adds {0} to {1} Fire Damage",
	  "附加 {0} 至 {1} 火焰傷害",
	  "Adds 5 to 10 Fire Damage",
	  "附加 5 至 10 火焰傷害", false },

	{ "千分位留在數值裡",
	  "{0} to maximum Life",
	  "{0} 最大生命",
	  "7,881 to maximum Life",
	  "7,881 最大生命", false },

	{ "小數點留在數值裡",
	  "Regenerate {0} Life per second",
	  "每秒回復 {0} 生命",
	  "Regenerate 12.5 Life per second",
	  "每秒回復 12.5 生命", false },

	// POB 工藝介面的詞綴列本來就寫著 '#',那一格沒有值可填,必須原樣留著。
	{ "輸入自帶 '#'(工藝介面詞綴列)",
	  "{0}% increased Armour during Effect",
	  "效果期間增加 {0}% 護甲",
	  "#% increased Armour during Effect",
	  "效果期間增加 #% 護甲", false },

	// 同一行同時有「輸入自帶的 '#'」與一個真的字面數字:兩格都要對到正確的來源,
	// 否則 10 會被塞進 '#' 那一格。
	{ "自帶 '#' 與字面數字並存",
	  "{0}% increased Armour per 10 Strength",
	  "增加 {0}% 護甲，每 10 點力量",
	  "#% increased Armour per 10 Strength",
	  "增加 #% 護甲，每 10 點力量", true },
};

// 把 {N} / {N:spec} 換成每個編號各不相同的哨兵值。全部填同一個值(舊的 census 就是
// 這樣)在這裡等於沒測 —— 順序錯了也看不出來,這正是這個 bug 活這麼久的原因之一。
std::string fill_sentinels(const std::string& s)
{
	std::string out;
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] == '{' && i + 2 < s.size()) {
			size_t j = i + 1;
			while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
			if (j > i + 1 && j < s.size()) {
				size_t close = std::string::npos;
				bool   plus  = false;
				if (s[j] == '}') {
					close = j;
				} else if (s[j] == ':') {
					size_t k = s.find('}', j);
					if (k != std::string::npos && k - j <= 8) {
						close = k;
						plus = s.find('+', j) < k;
					}
				}
				if (close != std::string::npos) {
					int n = atoi(s.c_str() + i + 1);
					if (n < 0) n = 0;
					if (n > 20) n = 20;
					char buf[16];
					// 9001, 9002, ... 每個編號一個值,而且不像 1/2/3 那樣容易與
					// 樣板自己寫死的常數撞在一起。
					snprintf(buf, sizeof(buf), "%s%d", plus ? "+" : "", 9001 + n);
					out += buf;
					i = close;
					continue;
				}
			}
		}
		out += s[i];
	}
	return out;
}

bool has_placeholder_or_digit(const std::string& s)
{
	for (size_t i = 0; i < s.size(); i++) {
		if (s[i] >= '0' && s[i] <= '9') return true;
		if (s[i] == '#') return true;
		if (s[i] == '{' && i + 2 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') return true;
	}
	return false;
}

// 反向探針的外殼。裸一行會被段落文法當成物品名那一段而走別條規則,詞條永遠碰不到
// 樣式比對,量出來的「零差異」會是假的。這個外殼沒有物品等級行,所以文法對探針那行
// 不下判斷(Unknown),正好是舊行為的比較基準。
const char* kShell = "\xe7\xa8\x80\xe6\x9c\x89\xe5\xba\xa6: \xe7\xa8\x80\xe6\x9c\x89\n"  // 稀有度: 稀有
                     "New Item\nCrimson Jewel\n--------\n";

std::string reverse_probe(const std::string& probe)
{
	std::string text = std::string(kShell) + probe + "\n";
	char* out = translation_reverse_text(text.c_str());
	if (!out) return std::string("<null>");
	std::string s(out);
	translation_free(out);
	return s;
}

int run_cases(const char* title, bool wantOrderRowsToPass, int* orderFailures)
{
	printf("\n%s\n", title);
	int failed = 0;
	if (orderFailures) *orderFailures = 0;
	for (const Case& c : kCases) {
		const char* got = translation_debug_fill(c.src, c.dst, c.in);
		bool ok = got && std::string(got) == c.want;
		if (!wantOrderRowsToPass && c.order) {
			// 突變模式:這一列的正確性取決於依編號填,所以現在應該是錯的。
			if (!ok) {
				if (orderFailures) (*orderFailures)++;
				printf("  [紅]    %s\n           got:  %s\n", c.name, got ? got : "(NULL)");
			} else {
				printf("  [沒紅]  %s  <- 突變後仍通過,這一列擋不住回歸\n", c.name);
				failed++;
			}
			continue;
		}
		if (!ok) {
			failed++;
			printf("  [FAIL]  %s\n           got:    %s\n           expect: %s\n",
			       c.name, got ? got : "(NULL: 輸入雜湊後與來源樣板對不起來)", c.want);
		} else {
			printf("  [PASS]  %s\n", c.name);
		}
	}
	return failed;
}

} // namespace

int RunPlaceholderSelftest()
{
	TeeOut tee(L"placeholder_selftest.txt");
	SetEnvironmentVariableA("POB_LOCALE", "zh-rTW");
	SetEnvironmentVariableA("POB_TR_LOG", "0");   // 十萬筆探針不要寫 miss log
	translation_init();
	printf("locale=%s entries=%d\n", translation_get_locale(), translation_get_count());

	// ---- 1. 邊角案例表(修好之後應該全綠) ----
	int failed = run_cases("== 1. 填位規則(合成樣板,走引擎自己的計畫產生器) ==", true, nullptr);
	g_pass += (int)(sizeof(kCases) / sizeof(kCases[0])) - failed;
	g_fail += failed;

	// ---- 2. 出貨字典端到端 ----
	printf("\n== 2. 出貨字典端到端(translation_lookup,POB 畫面走的那支) ==\n");
	{
		// 使用者回報的那一條。字典裡「per {2}」與「per 200」兩個變體雜湊成同一個鍵,
		// 誰勝出由載入序決定 —— 所以這裡不釘死中文用字,只釘死不變量:200 必須落在
		// 「命中值」前面那一格,1 與 6 必須落在傷害那兩格。
		const char* in = "1 to 6 Added Attack Lightning Damage per 200 Accuracy Rating";
		const char* got = translation_lookup(in);
		printf("  in:  %s\n  out: %s\n", in, got ? got : "(NOT FOUND)");
		std::string s = got ? got : "";
		// 字典裡至少三條詞綴雜湊成這個鍵(「每 {2}…」「每有 {2}…」「per 200」),
		// 誰勝出由載入序決定,所以斷言不能夾帶任何一種措辭 —— 只釘「200 緊接著
		// 命中值」這個位置事實,它在每一種措辭下都必須成立。
		check(s.find("200 \xe5\x91\xbd\xe4\xb8\xad\xe5\x80\xbc") != std::string::npos,
		      "200 落在「命中值」前面那一格");
		check(s.find("1 \xe8\x87\xb3 6") != std::string::npos, "1 與 6 落在傷害那兩格");
	}
	{
		const char* in = "+1 to maximum number of Summoned Golems if you have 3 Primordial Items Socketed or Equipped";
		const char* got = translation_lookup(in);
		printf("  in:  %s\n  out: %s\n", in, got ? got : "(NOT FOUND)");
		std::string s = got ? got : "";
		// 3 是「幾個原始物品」,+1 是「魔像數量」。不管勝出的是哪個中文變體,
		// 「3 個」必定出現、而「+1 個」不可以接在物品那一格。
		check(s.find("3 \xe5\x80\x8b") != std::string::npos, "3 落在物品數量那一格");
		check(s.find("+1") != std::string::npos, "+1 仍在輸出裡(沒有被吃掉)");
		check(s.find("+1 \xe5\x80\x8b\xe5\x8e\x9f\xe5\xa7\x8b") == std::string::npos &&
		      s.find("+1 \xe5\x80\x8b\xe5\x85\x88\xe7\xa5\x96") == std::string::npos,
		      "+1 沒有落在物品數量那一格");
	}

	{
		// 反向路徑算查表鍵時「沒有」做 normalize_placeholders,正向有。抽值的切法
		// 必須各自跟自己那條路徑一致,否則 GGG 的 "<colour:...>{{{0}}}" 這種把數字
		// 包在大括號裡的 UI 字串會多切一刀,值整個變成空的。全量 A/B 掃出 23 列才
		// 抓到這一刀,所以釘一條在這裡。
		const char* zh = "- <colour:rgb(255,255,255)>{{4321}} \xe9\x80\xb2\xe5\x85\xa5 -";
		std::string got = reverse_probe(zh);
		printf("  rev: %s\n   ->  %s\n", zh, got.c_str());
		check(got.find("4321") != std::string::npos,
		      "大括號裡的數字沒有被當成佔位符吃掉(反向)");
		check(got.find("255,255,255") != std::string::npos,
		      "顏色標記裡的數字原樣保留(反向)");
	}

	// ---- 3. 兩支掃描器不得漂開 ----
	// 填值用的數值格與 extract_numbers 是同一套規則的兩個出口。全字典逐條比對,
	// 並且斷言真的檢查過東西(數 bad == 0 而沒有 checked > 0 是空洞判準)。
	printf("\n== 3. 數值格 vs extract_numbers(全字典) ==\n");
	{
		EditorModel model = LoadModel(exe_dir() + L"Data\\poe1\\", "zh-rTW");
		int checked = 0, bad = 0, withSlots = 0;
		for (const EditorEntry& e : model.entries) {
			if (e.key.empty() || e.value.empty()) continue;
			checked += 2;
			if (has_placeholder_or_digit(e.key) || has_placeholder_or_digit(e.value)) withSlots++;
			bad += translation_debug_slot_mismatch(e.key.c_str());
			bad += translation_debug_slot_mismatch(e.value.c_str());
		}
		printf("  checked=%d strings, %d entries carry a value slot\n", checked, withSlots);
		check(checked > 1000, "真的掃到字典(不是空迴圈)");
		check(withSlots > 1000, "掃到的字典裡真的有帶數值格的條目");
		check(bad == 0, "digits_to_hash 的數值格與 extract_numbers 逐一相符");
		if (bad) printf("           %d slot(s) disagree\n", bad);
	}

	// ---- 4. 突變測試 ----
	// 把填值改回「照 '#' 出現順序填」,依編號才會對的那些列必須全部變紅。
	// 只驗合成表還不夠:計畫是載入時算進字典的,所以連字典一起重載一次,證明真正的
	// 執行期路徑也退回了舊行為。
	printf("\n== 4. 突變測試(退回「照出現順序填」) ==\n");
	{
		translation_debug_set_legacy_fill(1);
		int orderRows = 0;
		for (const Case& c : kCases) if (c.order) orderRows++;
		int reds = 0;
		int mutFailed = run_cases("-- 突變後的案例表 --", false, &reds);
		printf("  依編號才對的列:%d,突變後變紅:%d\n", orderRows, reds);
		check(mutFailed == 0 && reds == orderRows,
		      "突變後,每一條依編號才對的案例都會 FAIL");

		translation_shutdown();
		translation_init();
		const char* in = "1 to 6 Added Attack Lightning Damage per 200 Accuracy Rating";
		const char* got = translation_lookup(in);
		printf("  舊行為下的字典輸出: %s\n", got ? got : "(NOT FOUND)");
		std::string s = got ? got : "";
		check(s.find("200 \xe5\x91\xbd\xe4\xb8\xad\xe5\x80\xbc") == std::string::npos,
		      "舊行為下,同一條字典查詢確實是錯的(證明修的是這條路徑)");

		translation_debug_set_legacy_fill(0);
		translation_shutdown();
		translation_init();
		const char* back = translation_lookup(in);
		check(back && std::string(back).find("200 \xe5\x91\xbd\xe4\xb8\xad\xe5\x80\xbc") != std::string::npos,
		      "關掉突變後回到正確輸出");
	}

	printf("\n%d passed, %d failed\n", g_pass, g_fail);
	return g_fail == 0 ? 0 : 1;
}

int RunPlaceholderDump(const std::wstring& outPath, const std::string& game, bool legacy)
{
	TeeOut tee(L"placeholder_dump.log");
	SetEnvironmentVariableA("POB_LOCALE", "zh-rTW");
	SetEnvironmentVariableA("POB_TR_LOG", "0");
	SetEnvironmentVariableA("POB_GAME", game.c_str());
	// 計畫是載入時算進字典的,所以突變開關必須在 translation_init 之前設。
	if (legacy) translation_debug_set_legacy_fill(1);
	printf("fill mode = %s\n", legacy ? "LEGACY (照出現順序)" : "BY-INDEX (照編號)");
	translation_init();

	std::wstring root = exe_dir() + L"Data\\" + std::wstring(game.begin(), game.end()) + L"\\";
	EditorModel model = LoadModel(root, "zh-rTW");
	if (!model.localeExists) {
		printf("no %s/zh-rTW dictionary next to the exe\n", game.c_str());
		return 2;
	}
	printf("dump over %d entries in %d files (%s)\n",
	       (int)model.entries.size(), (int)model.files.size(), game.c_str());

	FILE* f = nullptr;
	_wfopen_s(&f, outPath.c_str(), L"wb");
	if (!f) {
		printf("cannot write %ls\n", outPath.c_str());
		return 2;
	}
	// 樣板兩欄跟著每一列走,而不是留給分析端用 id 反查字典。第一版就是那樣做的,
	// 而 LoadModel 的條目順序與 meta.json 的 load_order 並不相同 —— 於是 diff 全部
	// 掛到別條詞綴上,分類表整個是錯的。稽核工具的定位假設要嘛驗證要嘛消滅,這裡選
	// 消滅:每一列自己說明它是誰。
	fputs("id\tfile\tdir\ttmpl_en\ttmpl_zh\tin\tout\n", f);

	int id = -1, probes = 0, skipped = 0, fwdHit = 0, revChanged = 0;
	for (const EditorEntry& e : model.entries) {
		id++;
		if (e.key.empty() || e.value.empty()) continue;
		// 沒有任何數值格的條目,填值改動碰不到它們(它們根本不進樣式表)。
		if (!has_placeholder_or_digit(e.key) && !has_placeholder_or_digit(e.value)) continue;
		// GGG 自帶換行的條目在遊戲裡本來就是兩行,沒有單一貼上行與之對應。
		if (e.key.find('\n') != std::string::npos || e.value.find('\n') != std::string::npos) {
			skipped++;
			continue;
		}
		const char* srcFile = (e.fileIdx >= 0 && e.fileIdx < (int)model.files.size())
		                          ? model.files[e.fileIdx].name.c_str() : "?";
		std::string tmpl = tsv_safe(e.key) + "\t" + tsv_safe(e.value);

		// 正向:英文樣板實體化成一行真的文字,問 POB 畫面會拿到什麼。
		std::string en = fill_sentinels(translation_strip_ggg_markup(e.key.c_str()));
		const char* zh = translation_lookup(en.c_str());
		if (zh) fwdHit++;
		fprintf(f, "%d\t%s\tfwd\t%s\t%s\t%s\n", id, srcFile, tmpl.c_str(),
		        tsv_safe(en).c_str(), zh ? tsv_safe(zh).c_str() : "(none)");
		probes++;

		// 反向:中文樣板實體化,問貼上路徑會還原成什麼。
		std::string cn = fill_sentinels(translation_strip_ggg_markup(e.value.c_str()));
		std::string rev = reverse_probe(cn);
		if (rev.find(cn) == std::string::npos) revChanged++;
		fprintf(f, "%d\t%s\trev\t%s\t%s\t%s\n", id, srcFile, tmpl.c_str(),
		        tsv_safe(cn).c_str(), tsv_safe(rev).c_str());
		probes++;

		if (probes % 40000 == 0) printf("  %d probes...\n", probes);
	}
	fclose(f);
	printf("probes=%d  fwd hits=%d  rev changed=%d  skipped(multiline)=%d\n",
	       probes, fwdHit, revChanged, skipped);
	printf("wrote %ls\n", outPath.c_str());
	return 0;
}

int RunPlaceholderProbe(const std::wstring& text)
{
	TeeOut tee(L"placeholder_probe.txt");
	SetEnvironmentVariableA("POB_LOCALE", "zh-rTW");
	SetEnvironmentVariableA("POB_TR_LOG", "0");
	translation_init();
	std::string in = narrow(text);
	const char* got = translation_lookup(in.c_str());
	printf("in : %s\nout: %s\n", in.c_str(), got ? got : "(NOT FOUND)");
	return got ? 0 : 1;
}
