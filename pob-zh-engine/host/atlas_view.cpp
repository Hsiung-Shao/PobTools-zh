// must precede every imgui.h include (atlas_view.h pulls it in)
#define IMGUI_DEFINE_MATH_OPERATORS

#include "atlas_view.h"
#include "atlas_i18n.h"
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

void AtlasView::updateHover(AtlasTreeData& d, ImVec2 mouseWorld)
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
	hover_ = found;

	if (hover_ != previewFor_ || allocDirty_) {
		previewFor_ = hover_;
		allocDirty_ = false;
		hoverPath_.clear();
		hoverRemove_.clear();
		mark_.assign(d.nodes.size(), 0);
		if (hover_ != -1) {
			if (d.nodes[hover_].alloc) {
				hoverRemove_ = d.FindRemoveSet(hover_);
				for (int i : hoverRemove_) mark_[i] = 2;
			} else {
				hoverPath_ = d.FindPathTo(hover_);
				for (int i : hoverPath_) mark_[i] = 1;
			}
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
	ImGui::TextColored(nameCol, "%s", dispName.empty() ? (n.kind == kAtlasStart ? u8"圖譜起點" : "?") : dispName.c_str());
	// cost badge next to the name, poeplanner style (+N / -N nodes)
	if (n.kind != kAtlasStart) {
		if (n.alloc)
			// ASCII '-'（U+2212 − 不在 FZ_ZY 字形內,會畫成 '?'）
			{ ImGui::SameLine(0, 14.0f * uiScale); ImGui::TextColored(ImVec4(0.88f, 0.35f, 0.35f, 1.0f), u8"-%d 點", (int)hoverRemove_.size()); }
		else if (!hoverPath_.empty())
			{ ImGui::SameLine(0, 14.0f * uiScale); ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), u8"+%d 點", (int)hoverPath_.size()); }
	}

	// 明確 wrap 寬度:TextWrapped 會跟著「目前視窗寬」換行,而 auto-resize
	// tooltip 的寬度由最寬的不換行元件(短標題)決定 → 一行七八字的窄條。
	ImGui::PushTextWrapPos(380.0f * uiScale);
	for (const std::string& s : n.stats) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.62f, 0.68f, 0.90f, 1.0f));
		ImGui::TextUnformatted((zh ? zh->StatLine(s) : s).c_str());
		ImGui::PopStyleColor();
	}
	ImGui::PopTextWrapPos();

	ImGui::Separator();
	if (n.kind == kAtlasStart) {
		ImGui::TextDisabled(u8"起點節點");
	} else if (n.alloc) {
		ImGui::TextColored(ImVec4(0.88f, 0.35f, 0.35f, 1.0f), u8"點擊移除 %d 點", (int)hoverRemove_.size());
	} else if (hoverPath_.empty()) {
		ImGui::TextDisabled(u8"無法連接到已配置的節點");
	} else {
		int remain = d.TotalPoints() - d.UsedPoints();
		if ((int)hoverPath_.size() > remain)
			ImGui::TextColored(ImVec4(0.88f, 0.35f, 0.35f, 1.0f),
			                   u8"需要 %d 點（剩餘 %d 點，不足）", (int)hoverPath_.size(), remain);
		else
			ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), u8"點擊配置 %d 點", (int)hoverPath_.size());
	}
	ImGui::EndTooltip();
	ImGui::PopStyleVar();
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

bool AtlasView::Draw(AtlasTreeData& d, float uiScale, const AtlasI18n* zh)
{
	ImGuiIO& io = ImGui::GetIO();
	bool changed = false;

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
		updateHover(d, screenToWorld(io.MousePos));
	else
		hover_ = -1;

	// click-to-allocate / deallocate (release with negligible total movement)
	if (ImGui::IsItemDeactivated() &&
	    ImLengthSqr(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left)) < 16.0f * uiScale * uiScale &&
	    hover_ != -1) {
		const AtlasNode& n = d.nodes[hover_];
		if (n.kind == kAtlasStart) {
			// inert
		} else if (n.alloc) {
			std::vector<int> rs = d.FindRemoveSet(hover_);
			d.Remove(rs);
			status_ = u8"已移除 " + std::to_string((int)rs.size()) + u8" 點";
			changed = true;
		} else if (!hoverPath_.empty()) {
			int remain = d.TotalPoints() - d.UsedPoints();
			if ((int)hoverPath_.size() <= remain) {
				d.Alloc(hoverPath_);
				status_ = u8"已配置 " + std::to_string((int)hoverPath_.size()) + u8" 點";
				changed = true;
			} else {
				status_ = u8"點數不足：需要 " + std::to_string((int)hoverPath_.size()) +
				          u8" 點，剩餘 " + std::to_string(remain) + u8" 點";
			}
		}
		if (changed) {
			allocDirty_ = true;
			updateHover(d, screenToWorld(io.MousePos)); // refresh preview immediately
		}
	}

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
	dl->PopClipRect();

	if (hovered) drawTooltip(d, uiScale, zh);

	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();
	return changed;
}
