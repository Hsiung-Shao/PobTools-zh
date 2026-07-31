// must precede every imgui.h include (atlas_view.h pulls it in)
#define IMGUI_DEFINE_MATH_OPERATORS

#include "atlas_view.h"
#include "atlas_i18n.h"
#include "atlas_stat_agg.h" // StripStatMarkup for tooltip stat lines
#include "editor_util.h"   // EdReadFile / EdWiden
#include "image_tex.h"

#include <imgui_internal.h> // ImLengthSqr

#include <algorithm>
#include <cmath>
#include <cstdio>

// ---- palette ----------------------------------------------------------------

static const ImU32 kColEdgeOff = IM_COL32(90, 95, 110, 140);
static const ImU32 kColEdgeOn = IM_COL32(116, 202, 244, 240);   // allocated: light blue (poeplanner style)
static const ImU32 kColEdgePath = IM_COL32(120, 200, 150, 220); // preview: green
static const ImU32 kColEdgeLose = IM_COL32(224, 80, 80, 220);   // removal: red
static const ImU32 kColHoverRing = IM_COL32(255, 255, 255, 90);
static const ImU32 kColTargetRing = IM_COL32(255, 205, 110, 150); // deliberately picked
static const ImU32 kColWantRing = IM_COL32(94, 234, 140, 255);    // planning mode: "want"
static const ImU32 kColAvoidRing = IM_COL32(248, 113, 113, 245);  // ruled out by the user
static const ImU32 kColMechRing = IM_COL32(255, 226, 110, 235);   // mechanic highlight

// How long the cursor must rest on a node before its minimum-point plan is
// solved. Without this, sweeping the tree would run one solve per node passed.
static const float kSolveDelay = 0.12f;
// Target rings turn into visual noise on a zoomed-out map, so they only appear
// once nodes are big enough to read.
static const float kTargetRingMinZoom = 0.10f;

static const float kRejectFlash = 2.5f;    // "not enough points" banner lifetime

// side-panel click-to-focus
static const float kFocusDuration = 1.6f;  // highlight ring fade time (seconds)
static const float kFocusMinZoom = 0.16f;  // readable zoom floor when jumping in

// ---- texture management -------------------------------------------------------

bool AtlasView::LoadTextures(const std::wstring& exeDir, const AtlasTreeData& d, std::string* err)
{
	DestroyTextures();
	focusNode_ = -1;                 // node indices may change on hot-reload
	focusAnim_ = false;
	focusTimer_ = 0.0f;
	// Sprite sheets live beside the tree data (versioned: Data/atlas_versions/
	// <tag>/atlas/; legacy flat: Data/atlas/). DataDir() is the resolved folder.
	std::wstring base = d.DataDir().empty() ? (exeDir + L"Data\\") : d.DataDir();
	tex_.assign(d.sheets.size(), 0);
	for (size_t i = 0; i < d.sheets.size(); i++) {
		std::wstring rel = EdWiden(d.sheets[i].file);
		for (wchar_t& c : rel)
			if (c == L'/') c = L'\\';
		std::vector<unsigned char> bytes = EdReadFile(base + rel);
		if (bytes.empty()) {
			if (err) *err = "missing sprite sheet: Data/" + d.sheets[i].file;
			return false;
		}
		int w = 0, h = 0;
		unsigned char* rgba = DecodeImageRGBA(bytes.data(), (int)bytes.size(), &w, &h);
		if (!rgba) {
			if (err) *err = "cannot decode sprite sheet: " + d.sheets[i].file;
			return false;
		}
		tex_[i] = CreateTextureRGBA(rgba, w, h);
		FreeDecoded(rgba);
		if (!tex_[i]) {
			if (err) *err = "GL upload failed for sprite sheet: " + d.sheets[i].file;
			return false;
		}
	}
	return true;
}

void AtlasView::DestroyTextures()
{
	for (unsigned t : tex_)
		if (t) DeleteTexture(t);
	tex_.clear();
}

// ---- camera -------------------------------------------------------------------

ImVec2 AtlasView::worldToScreen(ImVec2 w) const
{
	return ImVec2(vpPos_.x + vpSize_.x * 0.5f + (w.x - center_.x) * zoom_,
	              vpPos_.y + vpSize_.y * 0.5f + (w.y - center_.y) * zoom_);
}

ImVec2 AtlasView::screenToWorld(ImVec2 s) const
{
	return ImVec2(center_.x + (s.x - vpPos_.x - vpSize_.x * 0.5f) / zoom_,
	              center_.y + (s.y - vpPos_.y - vpSize_.y * 0.5f) / zoom_);
}

// ---- hover / preview ------------------------------------------------------------

void AtlasView::solvePreview(AtlasTreeData& d)
{
	planReady_ = false;
	planTargets_.clear();
	planNodes_.clear();
	hoverAdd_.clear();
	hoverDrop_.clear();
	planPoints_ = 0;
	planExact_ = true;
	planNoop_ = false;
	mark_.assign(d.nodes.size(), 0);
	if (hover_ < 0 || d.nodes[hover_].kind == kAtlasStart) return;

	// Normal mode: connect the shortest path from whatever is already allocated
	// and never move a node the user has already bought. Re-deriving the whole
	// tree from the target set (what planning mode does) is minimal but it
	// reroutes paths the user deliberately walked -- clicking a second node
	// visibly scrambles the first one's route, and it compounds from there.
	// Minimality is available on demand instead, from the toolbar button.
	if (!planning_) {
		if (d.nodes[hover_].alloc) {
			hoverDrop_ = d.FindRemoveSet(hover_);
			for (int v : hoverDrop_) mark_[v] = 2;
			planPoints_ = d.UsedPoints() - (int)hoverDrop_.size();
		} else {
			hoverAdd_ = d.FindPathTo(hover_);
			for (int v : hoverAdd_) mark_[v] = 1;
			planPoints_ = d.UsedPoints() + (int)hoverAdd_.size();
		}
		planReady_ = true;
		return;
	}

	planTargets_ = AtlasTargetsAfterClick(d, hover_);
	AtlasPlan plan = AtlasPlanMinimal(d, planTargets_, d.BlockedIdx(), d.AllocIdx());
	planNodes_ = plan.nodes;
	planPoints_ = plan.points;
	planExact_ = plan.exact;

	std::vector<char> next(d.nodes.size(), 0);
	for (int v : planNodes_) next[v] = 1;
	for (int i = 0; i < (int)d.nodes.size(); i++) {
		if (d.nodes[i].kind == kAtlasStart) continue;
		if (next[i] && !d.nodes[i].alloc) { hoverAdd_.push_back(i); mark_[i] = 1; }
		else if (!next[i] && d.nodes[i].alloc) { hoverDrop_.push_back(i); mark_[i] = 2; }
	}
	planNoop_ = hoverAdd_.empty() && hoverDrop_.empty() && planTargets_ == d.TargetIdx();
	planReady_ = true;
}

void AtlasView::updateHover(AtlasTreeData& d, ImVec2 mouseWorld, float dt)
{
	int found = -1;
	float bestSq = 0.0f;
	// generous margin so small nodes stay clickable when zoomed out
	const float pad = 14.0f;
	ImVec2 wmin = screenToWorld(vpPos_);
	ImVec2 wmax = screenToWorld(vpPos_ + vpSize_);
	for (int i = 0; i < (int)d.nodes.size(); i++) {
		const AtlasNode& n = d.nodes[i];
		if (n.x < wmin.x - 200 || n.x > wmax.x + 200 || n.y < wmin.y - 200 || n.y > wmax.y + 200)
			continue;
		float r = std::max(n.on.w, n.on.h) * 0.5f + pad;
		float dx = n.x - mouseWorld.x, dy = n.y - mouseWorld.y;
		float dsq = dx * dx + dy * dy;
		if (dsq <= r * r && (found == -1 || dsq < bestSq)) {
			found = i;
			bestSq = dsq;
		}
	}
	// Mastery icons sit at cluster centres, where there is no node, so they are
	// only considered when nothing else claimed the cursor. That also keeps the
	// mechanic click from ever colliding with an allocation click.
	hoverMastery_ = -1;
	if (found == -1) {
		float bestM = 0.0f;
		for (int i = 0; i < (int)d.masteries.size(); i++) {
			const AtlasDeco& m = d.masteries[i];
			float r = std::max(m.spr.w, m.spr.h) * 0.5f;
			float dx = m.x - mouseWorld.x, dy = m.y - mouseWorld.y;
			float dsq = dx * dx + dy * dy;
			if (dsq <= r * r && (hoverMastery_ == -1 || dsq < bestM)) {
				hoverMastery_ = i;
				bestM = dsq;
			}
		}
	}

	if (found != hover_ || allocDirty_) {
		hover_ = found;
		previewFor_ = -1;
		planReady_ = false;
		dwell_ = 0.0f;
		mark_.assign(d.nodes.size(), 0);
		allocDirty_ = false;
	}

	// A full minimum-point solve is far heavier than the old BFS, so in planning
	// mode it waits for the cursor to settle -- sweeping across the tree must
	// not solve once per node passed over. The normal-mode BFS is cheap enough
	// to run on every hover change, so its preview stays instant.
	if (hover_ != -1 && hover_ != previewFor_) {
		dwell_ += dt;
		if (!planning_ || dwell_ >= kSolveDelay) {
			previewFor_ = hover_;
			solvePreview(d);
		}
	}
}

// ---- drawing --------------------------------------------------------------------

void AtlasView::drawDecos(const AtlasTreeData& d, ImDrawList* dl, const std::vector<AtlasDeco>& decos)
{
	ImVec2 wmin = screenToWorld(vpPos_);
	ImVec2 wmax = screenToWorld(vpPos_ + vpSize_);
	for (const AtlasDeco& deco : decos) {
		float hw = deco.spr.w * 0.5f, hh = deco.spr.h;
		if (deco.x + hw < wmin.x || deco.x - hw > wmax.x || deco.y + hh < wmin.y || deco.y - hh > wmax.y)
			continue;
		ImTextureID t = (ImTextureID)(intptr_t)tex_[deco.spr.sheet];
		const float* uv = deco.spr.uv;
		if (deco.half) {
			// stored as the top half; mirror it below the group center
			ImVec2 a = worldToScreen(ImVec2(deco.x - hw, deco.y - deco.spr.h));
			ImVec2 b = worldToScreen(ImVec2(deco.x + hw, deco.y));
			dl->AddImage(t, a, b, ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]));
			ImVec2 a2 = worldToScreen(ImVec2(deco.x - hw, deco.y));
			ImVec2 b2 = worldToScreen(ImVec2(deco.x + hw, deco.y + deco.spr.h));
			dl->AddImage(t, a2, b2, ImVec2(uv[0], uv[3]), ImVec2(uv[2], uv[1]));
		} else {
			ImVec2 a = worldToScreen(ImVec2(deco.x - hw, deco.y - deco.spr.h * 0.5f));
			ImVec2 b = worldToScreen(ImVec2(deco.x + hw, deco.y + deco.spr.h * 0.5f));
			dl->AddImage(t, a, b, ImVec2(uv[0], uv[1]), ImVec2(uv[2], uv[3]));
		}
	}
}

void AtlasView::drawEdges(const AtlasTreeData& d, ImDrawList* dl)
{
	ImVec2 wmin = screenToWorld(vpPos_);
	ImVec2 wmax = screenToWorld(vpPos_ + vpSize_);
	const float margin = 900.0f; // arcs can bow outside their endpoints' box
	float thick = std::clamp(zoom_ * 14.0f, 1.0f, 6.0f);

	for (const AtlasEdge& e : d.edges) {
		if (e.wormhole) continue; // paired gateways: no visual line
		const AtlasNode& na = d.nodes[e.a];
		const AtlasNode& nb = d.nodes[e.b];
		if ((na.x < wmin.x - margin && nb.x < wmin.x - margin) ||
		    (na.x > wmax.x + margin && nb.x > wmax.x + margin) ||
		    (na.y < wmin.y - margin && nb.y < wmin.y - margin) ||
		    (na.y > wmax.y + margin && nb.y > wmax.y + margin))
			continue;

		ImU32 col = kColEdgeOff;
		float t = thick;
		bool aIn = na.alloc || (e.a < (int)mark_.size() && mark_[e.a] == 1);
		bool bIn = nb.alloc || (e.b < (int)mark_.size() && mark_[e.b] == 1);
		bool aLose = e.a < (int)mark_.size() && mark_[e.a] == 2;
		bool bLose = e.b < (int)mark_.size() && mark_[e.b] == 2;
		if (aLose || bLose) {
			col = kColEdgeLose;
		} else if (na.alloc && nb.alloc) {
			col = kColEdgeOn;
			t = thick * 1.35f;
		} else if (aIn && bIn) {
			col = kColEdgePath;
			t = thick * 1.2f;
		}

		if (e.hasArc) {
			ImVec2 c = worldToScreen(ImVec2(e.cx, e.cy));
			int segs = std::clamp((int)(e.sweep * 16.0f) + 4, 6, 48);
			dl->PathArcTo(c, e.r * zoom_, e.a0, e.a0 + e.sweep, segs);
			dl->PathStroke(col, 0, t);
		} else {
			dl->AddLine(worldToScreen(ImVec2(na.x, na.y)), worldToScreen(ImVec2(nb.x, nb.y)), col, t);
		}
	}
}

void AtlasView::drawNodes(const AtlasTreeData& d, ImDrawList* dl)
{
	ImVec2 wmin = screenToWorld(vpPos_);
	ImVec2 wmax = screenToWorld(vpPos_ + vpSize_);

	for (int i = 0; i < (int)d.nodes.size(); i++) {
		const AtlasNode& n = d.nodes[i];
		if (n.x < wmin.x - 200 || n.x > wmax.x + 200 || n.y < wmin.y - 200 || n.y > wmax.y + 200)
			continue;

		int state = n.alloc ? 2 : 0; // 0 off, 1 path preview, 2 on
		if (!n.alloc && i < (int)mark_.size() && mark_[i] == 1) state = 1;

		// icon: allocated art only when actually allocated
		const AtlasSpriteRef& icon = (state == 2) ? n.on : n.off;
		{
			ImVec2 a = worldToScreen(ImVec2(n.x - icon.w * 0.5f, n.y - icon.h * 0.5f));
			ImVec2 b = worldToScreen(ImVec2(n.x + icon.w * 0.5f, n.y + icon.h * 0.5f));
			dl->AddImage((ImTextureID)(intptr_t)tex_[icon.sheet], a, b,
			             ImVec2(icon.uv[0], icon.uv[1]), ImVec2(icon.uv[2], icon.uv[3]));
		}

		// frame overlay (start node has none; its sprite is self-contained)
		if (n.kind >= 0 && n.kind <= 4) {
			const AtlasFrame& fr = d.frames[n.kind];
			const AtlasSpriteRef& f = (state == 2) ? fr.on : (state == 1) ? fr.path : fr.off;
			if (f.w > 0) {
				ImVec2 a = worldToScreen(ImVec2(n.x - f.w * 0.5f, n.y - f.h * 0.5f));
				ImVec2 b = worldToScreen(ImVec2(n.x + f.w * 0.5f, n.y + f.h * 0.5f));
				dl->AddImage((ImTextureID)(intptr_t)tex_[f.sheet], a, b,
				             ImVec2(f.uv[0], f.uv[1]), ImVec2(f.uv[2], f.uv[3]));
			}
		}

		// version-compare overlay: a bold ring on added / modified nodes
		if (!diffRing_.empty()) {
			auto it = diffRing_.find(i);
			if (it != diffRing_.end())
				dl->AddCircle(worldToScreen(ImVec2(n.x, n.y)),
				              (std::max(n.on.w, n.on.h) * 0.5f + 16.0f) * zoom_, it->second, 0, 3.5f);
		}

		// mechanic overlay: the "this is the same mechanic" ring, drawn under the
		// interaction rings so it never hides what a click would do
		// The floors keep both rings readable when zoomed all the way out; that is
		// exactly the view you use to answer "where else is this" / "what did I
		// mark", so shrinking them into dots would defeat the purpose.
		if (!mechNodes_.empty() && mechNodes_.count(i))
			dl->AddCircle(worldToScreen(ImVec2(n.x, n.y)),
			              std::max((std::max(n.on.w, n.on.h) * 0.5f + 14.0f) * zoom_, 7.0f),
			              kColMechRing, 0, 3.0f);

		// Deliberately picked nodes, so wiring is distinguishable from what the
		// user actually asked for. In planning mode that distinction IS the mode
		// -- everything not ringed was put there by the solver -- so the ring is
		// bold and drawn at every zoom. In normal mode nothing ever re-routes,
		// so the marker stays a quiet hint.
		if (n.target) {
			if (planning_)
				dl->AddCircle(worldToScreen(ImVec2(n.x, n.y)),
				              std::max((std::max(n.on.w, n.on.h) * 0.5f + 9.0f) * zoom_, 5.0f),
				              kColWantRing, 0, 3.5f);
			else if (zoom_ >= kTargetRingMinZoom)
				dl->AddCircle(worldToScreen(ImVec2(n.x, n.y)),
				              (std::max(n.on.w, n.on.h) * 0.5f + 8.0f) * zoom_, kColTargetRing, 0, 2.0f);
		}

		// ruled out: ring plus a cross, because a ring alone reads as "selected"
		// and this is the opposite. Drawn at every zoom -- an avoided node the
		// user cannot see is exactly the one they would wonder about.
		if (n.blocked) {
			ImVec2 c = worldToScreen(ImVec2(n.x, n.y));
			float r = (std::max(n.on.w, n.on.h) * 0.5f + 10.0f) * zoom_;
			dl->AddCircle(c, r, kColAvoidRing, 0, 2.5f);
			float x = r * 0.62f;
			dl->AddLine(ImVec2(c.x - x, c.y - x), ImVec2(c.x + x, c.y + x), kColAvoidRing, 2.5f);
			dl->AddLine(ImVec2(c.x - x, c.y + x), ImVec2(c.x + x, c.y - x), kColAvoidRing, 2.5f);
		}

		// removal preview: red ring over every node that would be lost
		if (i < (int)mark_.size() && mark_[i] == 2)
			dl->AddCircle(worldToScreen(ImVec2(n.x, n.y)),
			              (std::max(n.on.w, n.on.h) * 0.5f + 12.0f) * zoom_, kColEdgeLose, 0, 2.5f);

		if (i == hover_)
			dl->AddCircle(worldToScreen(ImVec2(n.x, n.y)),
			              (std::max(n.on.w, n.on.h) * 0.5f + 18.0f) * zoom_, kColHoverRing, 0, 2.0f);

		// fading gold ring after a side-panel "locate this node" click
		if (i == focusNode_ && focusTimer_ > 0.0f) {
			float a = focusTimer_ / kFocusDuration;
			dl->AddCircle(worldToScreen(ImVec2(n.x, n.y)),
			              (std::max(n.on.w, n.on.h) * 0.5f + 24.0f) * zoom_,
			              IM_COL32(255, 220, 120, (int)(255 * a)), 0, 3.0f);
		}
	}
}

void AtlasView::drawTooltip(const AtlasTreeData& d, float uiScale, const AtlasI18n* zh)
{
	if (hover_ == -1 && hoverMastery_ != -1) {
		const AtlasDeco& m = d.masteries[hoverMastery_];
		const std::string& lbl = (hoverMastery_ < (int)masteryLabel_.size() &&
		                          !masteryLabel_[hoverMastery_].empty())
			? masteryLabel_[hoverMastery_] : m.name;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * uiScale, 12.0f * uiScale));
		ImGui::BeginTooltip();
		ImGui::TextColored(ImVec4(1.0f, 0.89f, 0.43f, 1.0f), "%s", lbl.empty() ? "?" : lbl.c_str());
		ImGui::TextDisabled(mechMasteries_.count(hoverMastery_)
			? u8"點擊取消標示" : u8"點擊標示這個機制的所有位置");
		ImGui::EndTooltip();
		ImGui::PopStyleVar();
		return;
	}
	if (hover_ == -1) return;
	const AtlasNode& n = d.nodes[hover_];
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * uiScale, 12.0f * uiScale));
	ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(460.0f * uiScale, FLT_MAX));
	ImGui::BeginTooltip();

	ImVec4 nameCol =
		n.kind == kAtlasKeystone ? ImVec4(0.85f, 0.45f, 0.85f, 1.0f) :
		n.kind == kAtlasNotable ? ImVec4(0.95f, 0.80f, 0.40f, 1.0f) :
		n.kind == kAtlasWormhole ? ImVec4(0.55f, 0.80f, 0.95f, 1.0f) :
		ImVec4(0.90f, 0.90f, 0.90f, 1.0f);
	const std::string& dispName = zh ? zh->NodeName(n.id, n.name) : n.name;
	ImGui::TextColored(nameCol, "%s", dispName.empty() ? (n.kind == kAtlasStart ? u8"輿圖起點" : "?") : dispName.c_str());
	// cost badge next to the name, poeplanner style (+N / -N nodes)
	// ASCII '-'（U+2212 − 不在 FZ_ZY 字形內,會畫成 '?'）
	if (n.kind != kAtlasStart && planReady_) {
		int net = planPoints_ - d.UsedPoints();
		ImGui::SameLine(0, 14.0f * uiScale);
		if (net > 0)
			ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), u8"+%d 點", net);
		else if (net < 0)
			ImGui::TextColored(ImVec4(0.88f, 0.35f, 0.35f, 1.0f), u8"-%d 點", -net);
		else
			ImGui::TextDisabled(u8"±0 點");
	}
	if (n.target) {
		ImGui::SameLine(0, 10.0f * uiScale);
		ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.43f, 1.0f), u8"[目標]");
	}

	// 明確 wrap 寬度:TextWrapped 會跟著「目前視窗寬」換行,而 auto-resize
	// tooltip 的寬度由最寬的不換行元件(短標題)決定 → 一行七八字的窄條。
	ImGui::PushTextWrapPos(380.0f * uiScale);
	for (const std::string& s : n.stats) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.68f, 0.90f, 1.0f));
		ImGui::TextUnformatted(StripStatMarkup(zh ? zh->StatLine(s) : s).c_str());
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();

	ImGui::Separator();
	if (n.kind == kAtlasStart) {
		ImGui::TextDisabled(u8"起點節點");
	} else if (!planning_) {
		// Normal mode: shortest path from the current allocation, nothing moves.
		if (!planReady_) {
			ImGui::TextDisabled(u8"計算路徑…");
		} else if (n.alloc) {
			int lose = (int)hoverDrop_.size();
			ImGui::TextColored(ImVec4(0.88f, 0.35f, 0.35f, 1.0f),
			                   lose > 1 ? u8"點擊移除，連帶失去 %d 點" : u8"點擊移除（%d 點）", lose);
		} else if (hoverAdd_.empty()) {
			ImGui::TextDisabled(u8"無法從已配置的節點連到這裡");
		} else if (planPoints_ > d.TotalPoints()) {
			ImGui::TextColored(ImVec4(0.88f, 0.35f, 0.35f, 1.0f),
			                   u8"要 %d 點，超過上限 %d 點", planPoints_, d.TotalPoints());
		} else {
			ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f),
			                   u8"點擊配置 %d 點，共 %d 點", (int)hoverAdd_.size(), planPoints_);
		}
	} else if (!planReady_) {
		ImGui::TextDisabled(u8"計算最少點路徑…");
	} else if (planNodes_.empty()) {
		ImGui::TextDisabled(u8"無法連接到起點");
	} else if (planNoop_) {
		ImGui::TextDisabled(u8"連接用節點，沒有目標依賴它（點擊無效）");
	} else if (planPoints_ > d.TotalPoints()) {
		ImGui::TextColored(ImVec4(0.88f, 0.35f, 0.35f, 1.0f),
		                   u8"要 %d 點，超過上限 %d 點", planPoints_, d.TotalPoints());
	} else if (n.alloc && n.target) {
		ImGui::TextColored(ImVec4(0.88f, 0.35f, 0.35f, 1.0f),
		                   u8"點擊取消這個目標（剩 %d 點）", planPoints_);
	} else if (n.alloc) {
		ImGui::TextColored(ImVec4(0.88f, 0.35f, 0.35f, 1.0f),
		                   u8"連接用節點，點擊會放棄它後面的目標（剩 %d 點）", planPoints_);
	} else {
		ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f),
		                   u8"點擊設為目標，共 %d 點", planPoints_);
		if (!hoverDrop_.empty())
			ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
			                   u8"其中 %d 個連接用節點會改道", (int)hoverDrop_.size());
		if (!planExact_)
			ImGui::TextDisabled(u8"目標超過 %d 個，這是近似解", AtlasOptExactCap());
	}
	ImGui::EndTooltip();
	ImGui::PopStyleVar();
}

void AtlasView::SetMechanicHighlight(const std::vector<int>& nodeIdx, const std::vector<int>& masteryIdx)
{
	mechNodes_.clear();
	mechMasteries_.clear();
	mechNodes_.insert(nodeIdx.begin(), nodeIdx.end());
	mechMasteries_.insert(masteryIdx.begin(), masteryIdx.end());
}

void AtlasView::CenterOn(const AtlasTreeData& d, int nodeIdx)
{
	if (nodeIdx < 0 || nodeIdx >= (int)d.nodes.size()) return;
	if (zoom_ <= 0.0f) return; // first Draw hasn't sized the view yet
	const AtlasNode& n = d.nodes[nodeIdx];
	focusTarget_ = ImVec2(n.x, n.y);
	focusZoomTarget_ = std::clamp(std::max(zoom_, kFocusMinZoom), minZoom_, 0.6f);
	focusAnim_ = true;
	focusNode_ = nodeIdx;
	focusTimer_ = kFocusDuration;
}

// ---- main entry -------------------------------------------------------------------

bool AtlasView::Draw(AtlasTreeData& d, float uiScale, const AtlasI18n* zh, bool planning)
{
	ImGuiIO& io = ImGui::GetIO();
	bool changed = false;
	if (planning != planning_) {     // switching modes invalidates the cached preview
		planning_ = planning;
		planReady_ = false;
		previewFor_ = -1;
		allocDirty_ = true;
	}
	clickedMastery_ = -1;

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.031f, 0.035f, 0.047f, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::BeginChild("##atlascanvas", ImVec2(0, 0), false,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	vpPos_ = ImGui::GetCursorScreenPos();
	vpSize_ = ImGui::GetContentRegionAvail();
	if (vpSize_.x < 32.0f || vpSize_.y < 32.0f) {
		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
		return false;
	}

	// first frame: fit the whole tree, then clamp future zooming to sane bounds
	float treeW = std::max(1.0f, d.maxX - d.minX + 1600.0f);
	float treeH = std::max(1.0f, d.maxY - d.minY + 1600.0f);
	float fit = std::min(vpSize_.x / treeW, vpSize_.y / treeH);
	minZoom_ = fit * 0.85f;
	if (zoom_ <= 0.0f) {
		zoom_ = fit;
		center_ = ImVec2((d.minX + d.maxX) * 0.5f, (d.minY + d.maxY) * 0.5f);
	}

	ImGui::InvisibleButton("##atlashit", vpSize_,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	bool hovered = ImGui::IsItemHovered();

	// wheel zoom anchored at the mouse cursor
	if (hovered && io.MouseWheel != 0.0f) {
		focusAnim_ = false;          // user input takes the camera back
		ImVec2 anchor = screenToWorld(io.MousePos);
		zoom_ = std::clamp(zoom_ * powf(1.18f, io.MouseWheel), minZoom_, 0.6f);
		ImVec2 after = screenToWorld(io.MousePos);
		center_.x += anchor.x - after.x;
		center_.y += anchor.y - after.y;
	}

	// drag = pan (either button); a short left press-release = click
	if (ImGui::IsItemActive() &&
	    (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) ||
	     ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f))) {
		focusAnim_ = false;
		center_.x -= io.MouseDelta.x / zoom_;
		center_.y -= io.MouseDelta.y / zoom_;
	}

	// side-panel focus glide (exponential ease towards the clicked node)
	if (focusAnim_) {
		float k = 1.0f - expf(-8.0f * io.DeltaTime);
		center_.x += (focusTarget_.x - center_.x) * k;
		center_.y += (focusTarget_.y - center_.y) * k;
		zoom_ += (focusZoomTarget_ - zoom_) * k;
		float dx = focusTarget_.x - center_.x, dy = focusTarget_.y - center_.y;
		if (dx * dx + dy * dy < 1.0f && fabsf(focusZoomTarget_ - zoom_) < 0.001f) {
			center_ = focusTarget_;
			zoom_ = focusZoomTarget_;
			focusAnim_ = false;
		}
	}
	if (focusTimer_ > 0.0f) focusTimer_ -= io.DeltaTime;

	if (hovered)
		updateHover(d, screenToWorld(io.MousePos), io.DeltaTime);
	else
		hover_ = -1;

	// A mastery icon is the mechanic marker for its cluster, so clicking one asks
	// "where else is this mechanic" rather than touching the allocation. It can
	// only fire when no node is under the cursor (see updateHover), which is why
	// it sits outside the two click models below instead of racing them.
	if (hover_ == -1 && hoverMastery_ != -1 && ImGui::IsItemDeactivated() &&
	    ImLengthSqr(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left)) < 16.0f * uiScale * uiScale)
		clickedMastery_ = hoverMastery_;

	// Planning mode: mark first, solve after. Left click cycles the node through
	// want -> avoid -> clear so a handful of core nodes can be marked quickly;
	// right click clears in one step. Nothing here writes to disk -- the caller
	// keeps the sandbox and decides at the end whether to keep the result.
	if (planning) {
		bool markChanged = false;
		const bool shortLeft = ImGui::IsItemDeactivated() &&
			ImLengthSqr(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left)) < 16.0f * uiScale * uiScale;
		const bool shortRight = hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right) &&
			ImLengthSqr(ImGui::GetMouseDragDelta(ImGuiMouseButton_Right)) < 16.0f * uiScale * uiScale;
		if (hover_ != -1 && d.nodes[hover_].kind != kAtlasStart) {
			AtlasNode& n = d.nodes[hover_];
			if (shortRight && (n.target || n.blocked)) {
				n.target = n.blocked = false;
				status_ = u8"已清除標記";
				markChanged = true;
			} else if (shortLeft) {
				if (!n.target && !n.blocked)      { n.target = true;  status_ = u8"標記為想要"; }
				else if (n.target)                { n.target = false; n.blocked = true; status_ = u8"標記為不要"; }
				else                              { n.blocked = false; status_ = u8"已清除標記"; }
				markChanged = true;
			}
		}
		if (markChanged) {
			AtlasPlan p = AtlasPlanMinimal(d, d.TargetIdx(), d.BlockedIdx(), d.AllocIdx());
			d.SetAllocSet(p.nodes);
			planExact_ = p.exact;
			// A want that the avoids have walled off is reported, never left
			// allocated-but-disconnected (the defect this model exists to avoid).
			if (!p.ok()) {
				status_ += u8" · " + std::to_string(p.unreachable.size()) + u8" 個想要的點被不要的點擋住，連不上";
				rejectFlash_ = kRejectFlash;
			} else {
				status_ += u8" · 共 " + std::to_string(d.UsedPoints()) + u8" 點";
				if (!p.exact) status_ += u8"（近似解）";
			}
			changed = true;
			allocDirty_ = true;
			planReady_ = false;
		}
	} else
	// Normal mode: click allocates the shortest path to the node, or removes it
	// along with whatever it was holding on to. Nothing already allocated ever
	// moves -- see solvePreview for why the minimum-point model was pulled back
	// out of the click path.
	if (ImGui::IsItemDeactivated() &&
	    ImLengthSqr(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left)) < 16.0f * uiScale * uiScale &&
	    hover_ != -1 && d.nodes[hover_].kind != kAtlasStart) {
		// A click before the hover settled has no cached preview yet; build it
		// now rather than ignoring the click.
		if (!planReady_) { previewFor_ = hover_; solvePreview(d); }
		if (planReady_ && d.nodes[hover_].alloc) {
			// Targets are bookkeeping for the on-demand "compress" button; a
			// node that is gone must not stay pinned.
			for (int v : hoverDrop_)
				if (v >= 0 && v < (int)d.nodes.size()) d.nodes[v].target = false;
			d.Remove(hoverDrop_);
			status_ = u8"已移除 " + std::to_string((int)hoverDrop_.size()) + u8" 點";
			changed = true;
		} else if (planReady_ && hoverAdd_.empty()) {
			status_ = u8"無法從已配置的節點連到這裡";
			rejectFlash_ = kRejectFlash;
		} else if (planReady_) {
			int remain = d.TotalPoints() - d.UsedPoints();
			if ((int)hoverAdd_.size() > remain) {
				status_ = u8"點數不足：需要 " + std::to_string((int)hoverAdd_.size()) +
				          u8" 點，剩餘 " + std::to_string(remain) + u8" 點";
				rejectFlash_ = kRejectFlash;
			} else {
				d.Alloc(hoverAdd_);
				// Only the node actually clicked is a deliberate pick; the rest
				// of the path is wiring.
				d.nodes[hover_].target = true;
				status_ = u8"已配置 " + std::to_string((int)hoverAdd_.size()) +
				          u8" 點 · 共 " + std::to_string(d.UsedPoints()) + u8" 點";
				changed = true;
			}
		}
		if (changed) {
			allocDirty_ = true;
			updateHover(d, screenToWorld(io.MousePos), 0.0f);
			previewFor_ = hover_;                 // refresh the preview immediately
			solvePreview(d);
		}
	}
	if (rejectFlash_ > 0.0f) rejectFlash_ -= io.DeltaTime;

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(vpPos_, vpPos_ + vpSize_, true);
	if (d.hasBg && d.bg.sheet < (int)tex_.size() && tex_[d.bg.sheet]) {
		// tree background art stretched over the bounds, dimmed so nodes pop
		ImVec2 a = worldToScreen(ImVec2(d.minX, d.minY));
		ImVec2 b = worldToScreen(ImVec2(d.maxX, d.maxY));
		dl->AddImage((ImTextureID)(intptr_t)tex_[d.bg.sheet], a, b,
		             ImVec2(d.bg.uv[0], d.bg.uv[1]), ImVec2(d.bg.uv[2], d.bg.uv[3]),
		             IM_COL32(255, 255, 255, 165));
	}
	drawDecos(d, dl, d.groupBg);
	drawEdges(d, dl);
	drawDecos(d, dl, d.masteries);
	// Mechanic markers: the highlighted clusters, plus a hover ring so the icon
	// reads as clickable at all (it used to be pure decoration).
	for (int i = 0; i < (int)d.masteries.size(); i++) {
		if (i != hoverMastery_ && !mechMasteries_.count(i)) continue;
		const AtlasDeco& m = d.masteries[i];
		float r = std::max(std::max(m.spr.w, m.spr.h) * 0.5f * zoom_, 10.0f);
		if (mechMasteries_.count(i)) dl->AddCircle(worldToScreen(ImVec2(m.x, m.y)), r, kColMechRing, 0, 4.0f);
		if (i == hoverMastery_) dl->AddCircle(worldToScreen(ImVec2(m.x, m.y)), r + 5.0f, kColHoverRing, 0, 2.5f);
	}
	drawNodes(d, dl);

	// allocated-count chip pinned to the canvas corner (poeplanner style)
	{
		char buf[48];
		snprintf(buf, sizeof(buf), "%d / %d", d.UsedPoints(), d.TotalPoints());
		ImVec2 ts = ImGui::CalcTextSize(buf);
		ImVec2 pad(10.0f * uiScale, 5.0f * uiScale);
		ImVec2 p0 = vpPos_ + ImVec2(12.0f * uiScale, 12.0f * uiScale);
		dl->AddRectFilled(p0, p0 + ts + pad + pad, IM_COL32(16, 18, 24, 215), 6.0f * uiScale);
		dl->AddText(p0 + pad, IM_COL32(240, 244, 250, 255), buf);
	}

	// Refused click: a single line in the toolbar reads as "nothing happened",
	// so say it on the canvas, where the user is actually looking.
	if (rejectFlash_ > 0.0f && !status_.empty()) {
		float fade = std::min(1.0f, rejectFlash_ / 0.6f);
		ImVec2 ts = ImGui::CalcTextSize(status_.c_str());
		ImVec2 pad(14.0f * uiScale, 9.0f * uiScale);
		ImVec2 box = ts + pad + pad;
		ImVec2 p0(vpPos_.x + (vpSize_.x - box.x) * 0.5f, vpPos_.y + 26.0f * uiScale);
		dl->AddRectFilled(p0, p0 + box, IM_COL32(60, 18, 22, (int)(235 * fade)), 6.0f * uiScale);
		dl->AddRect(p0, p0 + box, IM_COL32(224, 80, 80, (int)(230 * fade)), 6.0f * uiScale, 0, 2.0f);
		dl->AddText(p0 + pad, IM_COL32(255, 200, 200, (int)(255 * fade)), status_.c_str());
	}
	dl->PopClipRect();

	if (hovered) drawTooltip(d, uiScale, zh);

	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
	return changed;
}
