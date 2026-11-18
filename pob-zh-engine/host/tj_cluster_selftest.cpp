// --tj-cluster-selftest [pobDir]: 只依 tj_cluster.h 的介面寫成(C0-C7),不看實作。
// 夾具用 bridge 回傳格式手組;C7 是真實流程(啟動 headless POB 讀 3.29死靈力爆 流派)。
// 假設:PassiveTreeData 有 `float worldPerPx`(預設 2.66f),夾具會明確設定它。

#include "tj_cluster.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace {

constexpr float kWpp = 2.66f;          // 夾具的 world/px,選非 1 才能抓到「忘了乘」
constexpr float kPi = 3.14159265358979f;

bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

// 夾具樹:sheets 故意讓 skills-3.jpg 不在索引 0(抓寫死 0 的 bug);主樹插槽 2491 放在索引 1。
PassiveTreeData MakeTree()
{
	PassiveTreeData t;
	t.sheets = { { "frame-3.png", 400, 400 }, { "skills-3.jpg", 1000, 500 }, { "skills-disabled-3.jpg", 1000, 500 } };
	t.worldPerPx = kWpp;
	PtNode dummy; dummy.id = 100; dummy.kind = kPtNormal;
	PtNode sock; sock.id = 2491; sock.kind = kPtSocket; sock.x = 1000; sock.y = 2000;
	t.nodes = { dummy, sock };
	t.sockets = { 1 };
	return t;
}

json MakeState()
{
	return json::parse(u8R"({
	"dynamicNodes":[
	 {"id":65537,"name":"Test Notable","nameZh":"測試顯著","type":"Notable","stats":["+10 to Strength"],"statsZh":["+10 力量"],
	  "x":1082,"y":2000,"orbit":1,"icon":"IconA","links":[65538],"group":10,
	  "frames":{"unalloc":"NotableFrameUnallocated","alloc":"NotableFrameAllocated"},"expansion":0,"expansionSkill":false,"allocated":true},
	 {"id":65538,"name":"Test Small","nameZh":"測試小點","type":"Normal","stats":["+5 to Dexterity"],"statsZh":["+5 敏捷"],
	  "x":1000,"y":2082,"orbit":1,"icon":"IconB","links":[65537],"group":10,
	  "frames":{"unalloc":"NormalFrameUnallocated","alloc":"NormalFrameAllocated"},"expansion":0,"expansionSkill":false,"allocated":false},
	 {"id":65539,"name":"Inner Socket","nameZh":"內部插槽","type":"Socket","stats":[],"statsZh":[],
	  "x":1300,"y":2000,"orbit":0,"icon":"","links":[],"group":11,
	  "frames":{"unalloc":"JewelFrameUnallocated","alloc":"JewelFrameAllocated"},"expansion":0,"expansionSkill":false,"allocated":false},
	 {"id":65540,"name":"Test Mastery","nameZh":"測試精通","type":"Mastery","stats":[],"statsZh":[],
	  "x":1100,"y":2100,"orbit":0,"icon":"IconM","links":[],"group":10,"frames":{},"expansion":0,"expansionSkill":false,"allocated":false}
	],
	"dynamicConnectors":[
	 {"a":65537,"b":65538,"orbit":1,"group":10},
	 {"a":2491,"b":65537,"orbit":0,"group":10}
	],
	"dynamicGroups":[
	 {"id":10,"x":1000,"y":2000,"orbits":[1],"parentSocket":2491},
	 {"id":11,"x":1300,"y":2000,"orbits":[0],"parentSocket":2491}
	]})");
}

json MakeAssets()
{
	return json::parse(R"({
	"assets":{
	 "IconA":{"file":"TreeData/3_29/skills-3.jpg","x":100,"y":50,"w":40,"h":30},
	 "IconB":{"file":"TreeData/3_29/skills-3.jpg","x":300,"y":200,"w":20,"h":20},
	 "NotableFrameUnallocated":{"file":"TreeData/3_29/frame-3.png","x":80,"y":0,"w":80,"h":80},
	 "NotableFrameAllocated":{"file":"TreeData/3_29/frame-3.png","x":0,"y":0,"w":80,"h":80},
	 "NormalFrameUnallocated":{"file":"TreeData/3_29/frame-3.png","x":50,"y":100,"w":50,"h":50},
	 "NormalFrameAllocated":{"file":"TreeData/3_29/frame-3.png","x":0,"y":100,"w":50,"h":50},
	 "JewelFrameUnallocated":{"file":"TreeData/3_29/frame-3.png","x":60,"y":200,"w":60,"h":60},
	 "JewelFrameAllocated":{"file":"TreeData/3_29/frame-3.png","x":0,"y":200,"w":60,"h":60}
	},
	"disabled":{
	 "IconA":{"file":"TreeData/3_29/skills-disabled-3.jpg","x":200,"y":100,"w":40,"h":30},
	 "IconB":{"file":"TreeData/3_29/skills-disabled-3.jpg","x":300,"y":200,"w":20,"h":20}
	}})");
}

int FindById(const TJClusterGraph& g, int id)
{
	for (size_t i = 0; i < g.nodes.size(); i++)
		if (g.nodes[i].id == id) return (int)i;
	return -1;
}

bool UvNear(const PtSprite& s, float u0, float v0, float u1, float v1)
{
	return Near(s.uv[0], u0, 1e-4f) && Near(s.uv[1], v0, 1e-4f) && Near(s.uv[2], u1, 1e-4f) && Near(s.uv[3], v1, 1e-4f);
}

std::string UvStr(const PtSprite& s)
{
	char b[160];
	snprintf(b, sizeof b, "sheet=%d uv=(%.4f,%.4f,%.4f,%.4f) w=%.3f h=%.3f valid=%d",
	         s.sheet, s.uv[0], s.uv[1], s.uv[2], s.uv[3], s.w, s.h, s.valid ? 1 : 0);
	return b;
}

bool DirExists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool FileExists(const std::wstring& p)
{
	DWORD a = GetFileAttributesW(p.c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

int RunTJClusterSelfTest(const std::wstring& exeDir, const std::wstring& pobDirArg)
{
	AllocConsole();
	FILE* dummyOut; freopen_s(&dummyOut, "CONOUT$", "w", stdout);
	FILE* rep = nullptr;
	_wfopen_s(&rep, (exeDir + L"tj_cluster_selftest.txt").c_str(), L"w");

	int pass = 0, fail = 0;
	auto check = [&](bool ok, const char* name, const std::string& detail) {
		std::string line = std::string(ok ? "[PASS] " : "[FAIL] ") + name + (detail.empty() ? "" : "  " + detail) + "\n";
		fputs(line.c_str(), stdout);
		if (rep) fputs(line.c_str(), rep);
		ok ? pass++ : fail++;
	};

	const PassiveTreeData tree = MakeTree();

	// C0 沒有 dynamicNodes 陣列:回 false 並帶錯誤訊息,不可吐出假資料或崩潰
	{
		const json missing = json::parse(R"({"dynamicConnectors":[],"dynamicGroups":[]})");
		const json notArray = json::parse(R"({"dynamicNodes":5})");
		for (int k = 0; k < 2; k++) {
			TJClusterGraph g; std::string err;
			const bool ok = ParseClusterGraph(k == 0 ? missing : notArray, MakeAssets(), tree, g, &err);
			check(!ok && !err.empty() && g.nodes.empty(),
			      k == 0 ? "C0 treeState 缺 dynamicNodes 時回 false 並帶錯誤訊息"
			             : "C0 dynamicNodes 不是陣列時回 false 並帶錯誤訊息",
			      "ok=" + std::to_string(ok) + " err=[" + err + "] nodes=" + std::to_string(g.nodes.size()));
		}
	}

	// C1 解析星團節點:3 個節點(大點/小點/內部插槽),名稱、配點、kind 照夾具
	{
		TJClusterGraph g; std::string err;
		const bool ok = ParseClusterGraph(MakeState(), MakeAssets(), tree, g, &err);
		check(ok && g.nodes.size() == 3, "C1 解析夾具得到 3 個節點", "ok=" + std::to_string(ok) + " n=" + std::to_string(g.nodes.size()) + " err=" + err);
		const int i1 = FindById(g, 65537), i2 = FindById(g, 65538), i3 = FindById(g, 65539);
		check(i1 >= 0 && i2 >= 0 && i3 >= 0, "C1 三個夾具節點 id 都在輸出", "");
		if (i1 >= 0 && i2 >= 0 && i3 >= 0) {
			const TJClusterNode &n1 = g.nodes[(size_t)i1], &n2 = g.nodes[(size_t)i2], &n3 = g.nodes[(size_t)i3];
			check(n1.kind == kPtNotable && n2.kind == kPtNormal && n3.kind == kPtSocket,
			      "C1 Notable/Normal/Socket 對應 kPtNotable/kPtNormal/kPtSocket",
			      std::to_string(n1.kind) + "," + std::to_string(n2.kind) + "," + std::to_string(n3.kind));
			check(n1.nameZh == u8"測試顯著" && n2.nameZh == u8"測試小點" && n3.nameZh == u8"內部插槽" &&
			      n1.name == "Test Notable",
			      "C1 name / nameZh 照夾具", n1.nameZh);
			check(n1.alloc && !n2.alloc && !n3.alloc, "C1 alloc 照夾具 allocated(只有大點為 true)",
			      std::to_string(n1.alloc) + std::to_string(n2.alloc) + std::to_string(n3.alloc));
			check(Near(n1.x, 1082, 0.01f) && Near(n1.y, 2000, 0.01f), "C1 座標照夾具 x/y", std::to_string(n1.x) + "," + std::to_string(n1.y));
			check(n1.stats.size() == 1 && n1.stats[0] == "+10 to Strength" && n1.statsZh.size() == 1 && n1.statsZh[0] == u8"+10 力量",
			      "C1 stats / statsZh 照夾具", "");
		}
	}

	// C2 圖示對到我們的圖集:sheet 索引、uv、世界尺寸(px * worldPerPx);框 sprite 同規則
	{
		TJClusterGraph g; std::string err;
		ParseClusterGraph(MakeState(), MakeAssets(), tree, g, &err);
		const int i1 = FindById(g, 65537);
		if (i1 < 0) check(false, "C2 找不到大點節點", err);
		else {
			const TJClusterNode& n = g.nodes[(size_t)i1];
			check(n.on.valid && n.on.sheet == 1 && UvNear(n.on, 0.10f, 0.10f, 0.14f, 0.16f),
			      "C2 已配點圖示對到 skills-3.jpg 的索引與 uv(x/W,y/H,(x+w)/W,(y+h)/H)", UvStr(n.on));
			check(n.on.valid && Near(n.on.w, 40 * kWpp, 1e-3f) && Near(n.on.h, 30 * kWpp, 1e-3f),
			      "C2 已配點圖示世界尺寸 = px * worldPerPx", UvStr(n.on));
			check(n.off.valid && n.off.sheet == 2 && UvNear(n.off, 0.20f, 0.20f, 0.24f, 0.26f),
			      "C2 未配點圖示取 disabled 表並對到 skills-disabled-3.jpg", UvStr(n.off));
			check(n.frameOn.valid && n.frameOn.sheet == 0 && UvNear(n.frameOn, 0, 0, 0.2f, 0.2f) &&
			      n.frameOff.valid && n.frameOff.sheet == 0 && UvNear(n.frameOff, 0.2f, 0, 0.4f, 0.2f),
			      "C2 框 sprite 的 alloc/unalloc 分別對到 frame-3.png 的 on/off 矩形",
			      UvStr(n.frameOn) + " | " + UvStr(n.frameOff));
		}
	}

	// C3 icon 的 file 不在 sheets:節點仍保留,on/off 標為無效,不崩
	{
		json assets = MakeAssets();
		assets["assets"]["IconA"]["file"] = "TreeData/3_29/skills-unknown.jpg";
		assets["disabled"]["IconA"]["file"] = "TreeData/3_29/skills-unknown.jpg";
		TJClusterGraph g; std::string err;
		const bool ok = ParseClusterGraph(MakeState(), assets, tree, g, &err);
		const int i1 = FindById(g, 65537), i2 = FindById(g, 65538);
		check(ok && i1 >= 0 && !g.nodes[(size_t)i1].on.valid && !g.nodes[(size_t)i1].off.valid,
		      "C3 圖集對不到時節點仍在、on/off.valid=false", "ok=" + std::to_string(ok) + " i1=" + std::to_string(i1));
		check(i2 >= 0 && g.nodes[(size_t)i2].on.valid && g.nodes[(size_t)i2].off.valid,
		      "C3 只有對不到的節點受影響,其他節點圖示仍有效", "");
	}

	// C4 同組同環連線:畫弧,半徑=節點到組中心距離,弧度 0..π
	{
		TJClusterGraph g; std::string err;
		ParseClusterGraph(MakeState(), MakeAssets(), tree, g, &err);
		const int i1 = FindById(g, 65537), i2 = FindById(g, 65538);
		const TJClusterEdge* arc = nullptr;
		for (const TJClusterEdge& e : g.edges)
			if (!e.aMain && !e.bMain && ((e.a == i1 && e.b == i2) || (e.a == i2 && e.b == i1))) arc = &e;
		check(arc != nullptr, "C4 大點與小點之間有一條邊(索引指向 nodes)", "edges=" + std::to_string(g.edges.size()));
		if (arc) {
			check(arc->hasArc, "C4 同組同環的邊 hasArc=true", "");
			check(Near(arc->r, 82.0f, 1.0f), "C4 弧半徑 = 節點到組中心距離(82)", "r=" + std::to_string(arc->r));
			check(Near(arc->cx, 1000, 1.0f) && Near(arc->cy, 2000, 1.0f), "C4 弧中心 = 組中心", std::to_string(arc->cx) + "," + std::to_string(arc->cy));
			check(arc->sweep >= 0 && arc->sweep <= kPi + 1e-3f, "C4 sweep 落在 0..π(取短弧)", "sweep=" + std::to_string(arc->sweep));
		}
	}

	// C5 連到主樹插槽:該端標 Main,索引為 tree.IndexOfId(2491),另一端是星團節點
	{
		TJClusterGraph g; std::string err;
		ParseClusterGraph(MakeState(), MakeAssets(), tree, g, &err);
		const int i1 = FindById(g, 65537);
		const int mainIdx = tree.IndexOfId(2491);
		const TJClusterEdge* me = nullptr;
		for (const TJClusterEdge& e : g.edges) if (e.aMain || e.bMain) me = &e;
		check(me != nullptr && mainIdx == 1, "C5 有一條連到主樹插槽的邊", "edges=" + std::to_string(g.edges.size()));
		if (me) {
			const bool aSide = me->aMain;
			const int mainEnd = aSide ? me->a : me->b, otherEnd = aSide ? me->b : me->a;
			const bool otherMain = aSide ? me->bMain : me->aMain;
			check(mainEnd == mainIdx, "C5 Main 端索引 = tree.IndexOfId(2491)(不是 nodes 索引)", "got " + std::to_string(mainEnd) + " want " + std::to_string(mainIdx));
			check(!otherMain && otherEnd == i1, "C5 另一端是星團節點 65537 且不標 Main", "other=" + std::to_string(otherEnd) + " want " + std::to_string(i1));
		}
	}

	// C6 Mastery 略過
	{
		TJClusterGraph g; std::string err;
		ParseClusterGraph(MakeState(), MakeAssets(), tree, g, &err);
		check(FindById(g, 65540) < 0 && g.nodes.size() == 3, "C6 Mastery 節點不進輸出 nodes", "n=" + std::to_string(g.nodes.size()));
	}

	// C7 真實流派端到端:真的啟動 headless POB,找不到環境算失敗(不 skip)
	{
		std::wstring pobDir = pobDirArg.empty() ? L"D:\\codeproject\\PobTest\\PathOfBuildingCommunity" : pobDirArg;
		while (pobDir.size() > 3 && (pobDir.back() == L'\\' || pobDir.back() == L'/')) pobDir.pop_back();
		const std::wstring build = pobDir + L"\\Builds\\3.29\u6B7B\u9748\u529B\u7206.xml";
		if (!DirExists(pobDir)) {
			check(false, "C7 找不到 POB 資料夾(不可略過,請用參數指定或建立測試環境)", std::string("pobDir 不存在: ") + std::to_string(pobDir.size()) + " 字元路徑");
		} else if (!FileExists(build)) {
			check(false, "C7 找不到流派檔 Builds\\3.29死靈力爆.xml(不可略過)", "pobDir 存在但流派檔缺");
		} else {
			PassiveTreeData real; std::string lerr;
			if (!real.Load(exeDir, &lerr)) {
				check(false, "C7 PassiveTreeData::Load 失敗,無法繼續", lerr);
			} else {
				json ts, ta; std::string ferr;
				const bool fetched = TJClusterFetchSync(exeDir, pobDir, build, ts, ta, &ferr);
				check(fetched, "C7 TJClusterFetchSync 對真實流派回 true", ferr);
				TJClusterGraph g; std::string perr;
				const bool parsed = fetched && ParseClusterGraph(ts, ta, real, g, &perr);
				check(parsed && !g.nodes.empty(), "C7 解析結果 nodes>0(流派確有星團珠寶)", "parsed=" + std::to_string(parsed) + " n=" + std::to_string(g.nodes.size()) + " err=" + perr);
				int allocN = 0, missFrame = 0;
				for (const TJClusterNode& n : g.nodes) {
					if (n.alloc) allocN++;
					if (n.kind != kPtSocket && !n.frameOff.valid) missFrame++;
				}
				check(allocN >= 1, "C7 至少一個節點 alloc=true", "alloc=" + std::to_string(allocN));
				check(missFrame == 0, "C7 每個非插槽節點的 frameOff 有效", "missing=" + std::to_string(missFrame));
				bool idxOk = true;
				for (const TJClusterEdge& e : g.edges) {
					const int na = e.aMain ? (int)real.nodes.size() : (int)g.nodes.size();
					const int nb = e.bMain ? (int)real.nodes.size() : (int)g.nodes.size();
					if (e.a < 0 || e.a >= na || e.b < 0 || e.b >= nb) idxOk = false;
				}
				check(idxOk, "C7 所有邊的端點索引都在範圍內(Main 端對主樹、其餘對 nodes)", "edges=" + std::to_string(g.edges.size()));
				// C7b UI 拿到的 POB 路徑(FindPoe1Dir)結尾帶反斜線:沙盒照樣要能開機
				json ts2, ta2; std::string ferr2;
				check(TJClusterFetchSync(exeDir, pobDir + L"\\", build, ts2, ta2, &ferr2) && ts2.contains("dynamicNodes"),
				      "C7b POB 路徑結尾帶反斜線時照樣取得星團資料", ferr2);
			}
		}
	}

	const std::string tail = "RESULT " + std::string(fail == 0 ? "PASS" : "FAIL") + "  pass=" + std::to_string(pass) + " fail=" + std::to_string(fail) + "\n";
	fputs(tail.c_str(), stdout);
	if (rep) { fputs(tail.c_str(), rep); fclose(rep); }
	return fail;
}
