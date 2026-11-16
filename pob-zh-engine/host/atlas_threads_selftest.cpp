// Self-test for atlas_threads (3.30 Threads of Creation rules + persistence).
// Folded into --atlas-selftest.

#include "atlas_threads.h"

#include <string>
#include <vector>

namespace {

struct ThreadReport {
	std::string& text;
	int failures = 0;

	void check(bool ok, const char* what, const std::string& detail = std::string())
	{
		text += ok ? "PASS  " : "FAIL  ";
		text += what;
		if (!detail.empty()) { text += "  ("; text += detail; text += ")"; }
		text += "\n";
		if (!ok) failures++;
	}
};

const char* kFixture = u8R"({
  "source": "selftest",
  "threads": [
    {"id":"Test/ThreadA","en":"Thread A","zh":"絲線甲","art":"Art/2DItems/A","mechanic":"Abyss","descEn":["a1"],"descZh":["甲1"]},
    {"id":"Test/ThreadB","en":"Thread B","zh":"絲線乙","art":"Art/2DItems/B","mechanic":"Blight","descEn":["b1","b2"],"descZh":["乙1"]},
    {"id":"Test/ThreadC","en":"Thread C","zh":"絲線丙","art":"Art/2DItems/C","mechanic":"Delirium","descEn":[],"descZh":[]},
    {"id":"Test/ThreadD","en":"Thread D","zh":"絲線丁","art":"Art/2DItems/D","mechanic":"Ritual","descEn":["d1"],"descZh":["丁1"]}
  ]
})";

const char* A = "Test/ThreadA";
const char* B = "Test/ThreadB";
const char* C = "Test/ThreadC";
const char* D = "Test/ThreadD";

std::string Describe(const std::vector<ThreadSlot>& v)
{
	std::string s = "[";
	for (const ThreadSlot& t : v) s += t.id + "x" + std::to_string(t.count) + " ";
	return s + "]";
}

} // namespace

int RunThreadSelfTest(const std::wstring& exeDir, std::string& out)
{
	ThreadReport rep{ out };
	out += "\n-- threads --\n";

	ThreadDb db;
	std::string err;
	bool loaded = db.LoadFromJson(kFixture, &err);
	rep.check(loaded && db.available() && db.All().size() == 4,
	          "記憶體 JSON 載入 4 種假絲線且 available 為真", err);

	const ThreadRules normal;
	ThreadRules keystone;
	keystone.singleTypeKeystone = true;

	// CanPlace
	rep.check(db.CanPlace({}, A, normal) == ThreadPlace::kOk, "T1 0 槽放已知絲線 A 回 kOk");
	rep.check(db.CanPlace({}, "Test/Nope", normal) == ThreadPlace::kUnknown, "T2 放不存在的 id 回 kUnknown");
	rep.check(db.CanPlace({ {A, 1}, {B, 1}, {C, 1} }, D, normal) == ThreadPlace::kFull,
	          "T3 已有 3 槽再放新種類 D 回 kFull");
	rep.check(db.CanPlace({ {A, 1} }, A, normal) == ThreadPlace::kDuplicate,
	          "T4 已有 A 再放 A 回 kDuplicate");
	rep.check(db.CanPlace({ {A, 1} }, B, keystone) == ThreadPlace::kKeystoneMixed,
	          "T5 關鍵天賦下已有 A 放 B 回 kKeystoneMixed");

	// max / clamp
	rep.check(ThreadMaxCount(normal) == 8 && ThreadMaxCount(keystone) == 10,
	          "T6a 單槽上限一般 8、關鍵天賦 10",
	          std::to_string(ThreadMaxCount(normal)) + "/" + std::to_string(ThreadMaxCount(keystone)));
	rep.check(ThreadClampCount(12, normal) == 8 && ThreadClampCount(12, keystone) == 10,
	          "T6b ThreadClampCount(12) 一般夾到 8、關鍵天賦夾到 10");
	rep.check(ThreadClampCount(0, normal) == 1 && ThreadClampCount(-3, normal) == 1 &&
	          ThreadClampCount(0, keystone) == 1 && ThreadClampCount(-3, keystone) == 1,
	          "T7 count 為 0 或 -3 時夾到 1");

	// Sanitize
	{
		// unknown X, duplicate B, over-cap A, 6 entries -> truncated to 3
		std::vector<ThreadSlot> in = { {B, 2}, {"Test/Nope", 1}, {A, 20}, {B, 5}, {C, 1}, {D, 1} };
		std::string note;
		std::vector<ThreadSlot> r = db.Sanitize(in, normal, &note);
		std::vector<ThreadSlot> want = { {B, 2}, {A, 8}, {C, 1} };
		rep.check(r == want,
		          "T8a 未知 id、重複種類、超量、超槽的輸入只剩合法項且保留原順序、重複留第一個、count 夾到 8",
		          Describe(r));
		rep.check(r.size() <= (size_t)kMaxThreadSlots, "T8b 結果不超過 3 槽");
		rep.check(!note.empty(), "T8c 有變動時 note 非空");
	}
	{
		std::vector<ThreadSlot> in = { {A, 3}, {B, 8} };
		std::string note = "stale";
		std::vector<ThreadSlot> r = db.Sanitize(in, normal, &note);
		rep.check(r == in && note.empty(), "T9 合法 2 槽 Sanitize 原樣回傳且 note 為空",
		          Describe(r) + " note=" + note);
	}
	{
		std::vector<ThreadSlot> in = { {A, 9}, {B, 1}, {C, 1} };
		std::vector<ThreadSlot> r = db.Sanitize(in, keystone, nullptr);
		rep.check(r.size() == 1 && r[0].id == A && r[0].count == 9,
		          "T10 關鍵天賦下 3 種只剩第一種 A 且 count 9 不被改", Describe(r));
	}
	{
		ThreadDb empty;
		std::vector<ThreadSlot> in = { {"Test/Nope", 20}, {A, 99}, {A, 1}, {B, 0}, {C, 1}, {D, 1} };
		std::string note;
		std::vector<ThreadSlot> r = empty.Sanitize(in, normal, &note);
		rep.check(!empty.available() && r == in, "T11 未載入資料時 Sanitize 原樣回傳輸入", Describe(r));
	}

	// persistence
	{
		AtlasBuildFile touched;
		touched.ParseDoc(u8"{\"builds\":[{\"name\":\"a\",\"alloc\":[7]}]}");
		AtlasBuildFile pristine = touched;
		touched.builds[0].threads = { {A, 3} };
		touched.builds[0].threads.clear();
		std::string withThreads;
		{
			AtlasBuildFile f = pristine;
			f.builds[0].threads = { {A, 3} };
			withThreads = f.SerializeDoc();
		}
		std::string docTouched = touched.SerializeDoc();
		std::string docPristine = pristine.SerializeDoc();
		rep.check(withThreads.find("\"threads\"") != std::string::npos,
		          "T12a 有絲線時序列化含 \"threads\"", withThreads);
		rep.check(docTouched.find("\"threads\"") == std::string::npos &&
		          docTouched == docPristine,
		          "T12b threads 為空時序列化不含 \"threads\" 且與從未設定者逐字相同", docTouched);
	}
	{
		AtlasBuildFile f;
		f.ParseDoc(u8"{\"builds\":[{\"name\":\"a\",\"alloc\":[7]}]}");
		f.builds[0].threads = { {A, 6}, {B, 1} };
		std::string doc = f.SerializeDoc();
		AtlasBuildFile g;
		bool ok = g.ParseDoc(doc);
		rep.check(ok && g.builds.size() == 1 && g.builds[0].threads == f.builds[0].threads,
		          "T13 2 槽 (A×6,B×1) 序列化再解析後 id、count、順序完全相同",
		          ok ? Describe(g.builds[0].threads) : "ParseDoc failed");
	}
	{
		AtlasBuildEntry e;
		e.name = "t";
		e.alloc = { 1, 2, 3 };
		e.threads = { {A, 6}, {C, 2} };
		std::string code = AtlasBuildShareCode(e, "3.30.0");
		AtlasBuildEntry back;
		std::string perr;
		bool ok = AtlasParseShareCode(code, &back, &perr);
		rep.check(code.rfind("PTAT1|", 0) == 0 && ok && back.threads == e.threads,
		          "T14 分享碼以 PTAT1| 開頭且解回的 threads 與原本相同",
		          ok ? Describe(back.threads) : perr);
	}
	{
		AtlasBuildFile f;
		bool ok = f.ParseDoc(u8"{\"version\":\"x\",\"alloc\":[1,2,3]}");
		rep.check(ok && f.builds.size() == 1 && f.builds[0].threads.empty() &&
		          f.builds[0].alloc == std::vector<int>({ 1, 2, 3 }),
		          "T15 舊格式檔解析後 threads 為空且 alloc 仍為 1,2,3");
	}

	// ---- 3.30 祭祀綢帶 / 血祭容器(U 系列) ----

	// U1 出貨資料檔
	{
		ThreadDb shipped;
		std::string lerr;
		bool ok = shipped.Load(exeDir, &lerr);
		const ThreadDef* ritual = shipped.ById("Metadata/Items/Threads/ThreadRitual");
		rep.check(ok && shipped.available() && shipped.All().size() == 10,
		          "U1a 出貨資料檔載入成功且共 10 種綢帶", lerr + " n=" + std::to_string(shipped.All().size()));
		rep.check(ritual && ritual->en == "Ritual Thread" && ritual->zh == u8"祭祀綢帶" && ritual->mechanic == "Ritual",
		          "U1b 祭祀綢帶 en/zh/mechanic 正確");
		bool namesOk = !shipped.All().empty();
		for (const ThreadDef& t : shipped.All()) if (t.en.empty() || t.zh.empty()) namesOk = false;
		rep.check(namesOk, "U1c 10 種綢帶的 en、zh 都非空");
	}

	// U2 關鍵天賦詞條(樹檔節點 65225 的 statsEn 原文)
	{
		const std::vector<std::string> keystoneStats = {
			"You can only apply one type of [Thread] to your Maps",
			"Your Maps with [Thread|Threads] applied gain 2 additional Threads,\nwhich can bypass the 8 Thread maximum",
			"75% chance for [Thread|Threads] found in your Maps to be converted to Gold",
		};
		rep.check(ThreadSingleTypeFromStats(keystoneStats), "U2a 配點詞條含關鍵天賦那行時回 true");
		rep.check(!ThreadSingleTypeFromStats({ "2% increased effect of Explicit Modifiers on your Maps",
		                                       "Your Maps contain 10% increased number of Runic Monster Markers" }),
		          "U2b 只有無關詞條時回 false");
		rep.check(!ThreadSingleTypeFromStats({}), "U2c 空詞條清單回 false");
	}

	// U3 Pickable
	{
		std::vector<const ThreadDef*> p1 = db.Pickable({ {A, 1} }, normal);
		bool noA = true;
		for (const ThreadDef* t : p1) if (t->id == A) noA = false;
		rep.check(p1.size() == 3 && noA, "U3a 一般狀態已放 A 時可選 3 種且不含 A", std::to_string(p1.size()));
		rep.check(db.Pickable({}, normal).size() == 4, "U3b 空槽時可選 4 種");
		rep.check(db.Pickable({ {A, 1}, {B, 1}, {C, 1} }, normal).empty(), "U3c 已滿 3 槽時可選 0 種");
		rep.check(db.Pickable({ {A, 1} }, keystone).empty(), "U3d 關鍵天賦且已放一種時可選 0 種");
		rep.check(db.Pickable({}, keystone).size() == 4, "U3e 關鍵天賦且空槽時可選 4 種");
	}

	// U4 RitualVesselSlots
	{
		auto vs = [&](const std::vector<ThreadSlot>& s) { return RitualVesselSlots(db, s); };
		rep.check(vs({}) == 0 && vs({ {A, 3} }) == 0, "U4a 沒有祭祀綢帶時容器槽為 0");
		rep.check(vs({ {D, 1} }) == 1, "U4b 祭祀綢帶 ×1 時容器槽為 1");
		rep.check(vs({ {D, 3} }) == 1, "U4c 祭祀綢帶 ×3 時容器槽仍為 1");
		rep.check(vs({ {D, 4} }) == 2, "U4d 祭祀綢帶 ×4 時容器槽為 2");
		rep.check(vs({ {D, 7} }) == 2, "U4e 祭祀綢帶 ×7 時容器槽仍為 2");
		rep.check(vs({ {D, 8} }) == 3, "U4f 祭祀綢帶 ×8 時容器槽為 3");
		rep.check(vs({ {A, 8} }) == 0, "U4g 只有非祭祀綢帶 ×8 時容器槽為 0");
	}

	// U5 ClampRitualVessels
	{
		const std::vector<ThreadSlot> d10 = { {D, 10} };
		rep.check(ClampRitualVessels(5, db, d10) == 3, "U5a D×10 時 5 夾到 3");
		rep.check(ClampRitualVessels(-1, db, d10) == 0, "U5b D×10 時 -1 夾到 0");
		rep.check(ClampRitualVessels(2, db, d10) == 2, "U5c D×10 時 2 維持 2");
		rep.check(ClampRitualVessels(2, db, {}) == 0, "U5d 沒有祭祀綢帶時 2 夾到 0");
		rep.check(ClampRitualVessels(3, db, { {D, 6} }) == 2, "U5e D×6 時 3 夾到 2");
	}

	// U6 存檔往返 ritualVessels
	{
		AtlasBuildFile f;
		f.ParseDoc(u8"{\"builds\":[{\"name\":\"a\",\"alloc\":[7]}]}");
		f.builds[0].ritualVessels = 2;
		std::string doc = f.SerializeDoc();
		AtlasBuildFile g;
		bool ok = g.ParseDoc(doc);
		rep.check(doc.find("\"vessels\"") != std::string::npos && ok && g.builds.size() == 1 &&
		          g.builds[0].ritualVessels == 2,
		          "U6a ritualVessels=2 序列化含 \"vessels\" 且讀回為 2", doc);

		f.builds[0].ritualVessels = 0;
		std::string doc0 = f.SerializeDoc();
		rep.check(doc0.find("vessels") == std::string::npos, "U6b ritualVessels=0 時序列化不含 vessels", doc0);

		AtlasBuildFile h;
		bool threw = false;
		try { h.ParseDoc(u8"{\"builds\":[{\"name\":\"a\",\"alloc\":[7],\"vessels\":\"x\"}]}"); }
		catch (...) { threw = true; }
		rep.check(!threw && h.builds.size() == 1 && h.builds[0].ritualVessels == 0,
		          "U6c \"vessels\" 為字串時讀回 0 且不丟例外");
	}

	return rep.failures;
}
