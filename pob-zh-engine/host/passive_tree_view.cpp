// must precede every imgui.h include (passive_tree_view.h pulls it in)
#define IMGUI_DEFINE_MATH_OPERATORS

#include "passive_tree_view.h"
#include "editor_util.h"   // EdReadFile / EdWiden
#include "image_tex.h"

#include <imgui_internal.h> // ImLengthSqr

#include <algorithm>
#include <cmath>

// ---- palette ----------------------------------------------------------------

static const ImU32 kColEdge       = IM_COL32(88, 94, 108, 150);
static const ImU32 kColEdgeInRad  = IM_COL32(150, 130, 70, 200);
static const ImU32 kColHoverRing  = IM_COL32(255, 255, 255, 110);
static const ImU32 kColAffected   = IM_COL32(240, 60, 60, 255);    // matched: red
static const ImU32 kColReplaced   = IM_COL32(255, 130, 70, 255);   // replaced notable: orange-red
static const ImU32 kColSocket     = IM_COL32(150, 160, 180, 220);
static const ImU32 kColSocketSel  = IM_COL32(255, 205, 90, 255);
static const ImU32 kColRadiusFill = IM_COL32(255, 205, 90, 26);
static const ImU32 kColRadiusRing = IM_COL32(255, 205, 90, 150);

static const float kFocusMinZoom = 0.14f;
static const float kMaxZoom = 0.7f;

// ---- textures ---------------------------------------------------------------

bool PassiveTreeView::LoadTextures(const std::wstring& exeDir, const PassiveTreeData& d, std::string* err)
{
	DestroyTextures();
	std::wstring base = exeDir + L"PathOfBuildingCommunity\\TreeData\\" + EdWiden(d.TreeVersion()) + L"\\";
	tex_.assign(d.sheets.size(), 0);
	for (size_t i = 0; i < d.sheets.size(); i++) {
		std::wstring rel = EdWiden(d.sheets[i].file);
		for (wchar_t& c : rel) if (c == L'/') c = L'\\';
		std::vector<unsigned char> bytes = EdReadFile(base + rel);
		if (bytes.empty()) {
			if (err) *err = "找不到天賦樹圖集：TreeData/" + d.TreeVersion() + "/" + d.sheets[i].file +
			                "（請確認 POB 已下載天賦樹資料）";
			return false;
		}
		int w = 0, h = 0;
		unsigned char* rgba = DecodeImageRGBA(bytes.data(), (int)bytes.size(), &w, &h);
		if (!rgba) {
			if (err) *err = "無法解碼天賦樹圖集：" + d.sheets[i].file;
			return false;
		}
		tex_[i] = CreateTextureRGBA(rgba, w, h);
		FreeDecoded(rgba);
		if (!tex_[i]) {
			if (err) *err = "GL 上傳失敗：" + d.sheets[i].file;
			return false;
		}
	}
	return true;
}

void PassiveTreeView::DestroyTextures()
{
	for (unsigned t : tex_) if (t) DeleteTexture(t);
	tex_.clear();
}

// ---- camera -----------------------------------------------------------------

ImVec2 PassiveTreeView::worldToScreen(ImVec2 w) const
{
	return ImVec2(vpPos_.x + vpSize_.x * 0.5f + (w.x - center_.x) * zoom_,
	              vpPos_.y + vpSize_.y * 0.5f + (w.y - center_.y) * zoom_);
}

ImVec2 PassiveTreeView::screenToWorld(ImVec2 s) const
{
	return ImVec2(center_.x + (s.x - vpPos_.x - vpSize_.x * 0.5f) / zoom_,
	              center_.y + (s.y - vpPos_.y - vpSize_.y * 0.5f) / zoom_);
}

int PassiveTreeView::hitTest(const PassiveTreeData& d, ImVec2 mouseWorld, bool socketsOnly) const
{
	int found = -1;
	float bestSq = 0.0f;
	const float pad = 16.0f;
	auto consider = [&](int i) {
		const PtNode& n = d.nodes[i];
		float base = n.kind == kPtSocket ? 60.0f : std::max(n.off.w, n.off.h);
		float r = base * 0.5f + pad;
		float dx = n.x - mouseWorld.x, dy = n.y - mouseWorld.y;
		float dsq = dx * dx + dy * dy;
		if (dsq <= r * r && (found == -1 || dsq < bestSq)) { found = i; bestSq = dsq; }
	};
	if (socketsOnly) {
		for (int i : d.sockets) consider(i);
	} else {
		for (int i = 0; i < (int)d.nodes.size(); i++) consider(i);
	}
	return found;
}

void PassiveTreeView::CenterOn(const PassiveTreeData& d, int nodeIdx)
{
	if (nodeIdx < 0 || nodeIdx >= (int)d.nodes.size() || zoom_ <= 0.0f) return;
	focusTarget_ = ImVec2(d.nodes[nodeIdx].x, d.nodes[nodeIdx].y);
	focusZoomTarget_ = std::clamp(std::max(zoom_, kFocusMinZoom), minZoom_, kMaxZoom);
	focusAnim_ = true;
}

// ---- main entry -------------------------------------------------------------

PassiveTreeOutput PassiveTreeView::Draw(const PassiveTreeData& d, float uiScale, const PassiveTreeInput& in)
{
	PassiveTreeOutput out;
	ImGuiIO& io = ImGui::GetIO();

	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.027f, 0.031f, 0.043f, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
	ImGui::BeginChild("##ptcanvas", ImVec2(0, 0), false,
		ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

	vpPos_ = ImGui::GetCursorScreenPos();
	vpSize_ = ImGui::GetContentRegionAvail();
	if (vpSize_.x < 32.0f || vpSize_.y < 32.0f) {
		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();
		return out;
	}

	float treeW = std::max(1.0f, d.maxX - d.minX + 1600.0f);
	float treeH = std::max(1.0f, d.maxY - d.minY + 1600.0f);
	float fit = std::min(vpSize_.x / treeW, vpSize_.y / treeH);
	minZoom_ = fit * 0.9f;
	if (zoom_ <= 0.0f) {
		zoom_ = fit;
		center_ = ImVec2((d.minX + d.maxX) * 0.5f, (d.minY + d.maxY) * 0.5f);
	}

	ImGui::InvisibleButton("##pthit", vpSize_,
		ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	bool hovered = ImGui::IsItemHovered();

	if (hovered && io.MouseWheel != 0.0f) {
		focusAnim_ = false;
		ImVec2 anchor = screenToWorld(io.MousePos);
		zoom_ = std::clamp(zoom_ * powf(1.18f, io.MouseWheel), minZoom_, kMaxZoom);
		ImVec2 after = screenToWorld(io.MousePos);
		center_.x += anchor.x - after.x;
		center_.y += anchor.y - after.y;
	}
	if (ImGui::IsItemActive() &&
	    (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) ||
	     ImGui::IsMouseDragging(ImGuiMouseButton_Right, 2.0f))) {
		focusAnim_ = false;
		center_.x -= io.MouseDelta.x / zoom_;
		center_.y -= io.MouseDelta.y / zoom_;
	}
	if (focusAnim_) {
		float k = 1.0f - expf(-8.0f * io.DeltaTime);
		center_.x += (focusTarget_.x - center_.x) * k;
		center_.y += (focusTarget_.y - center_.y) * k;
		zoom_ += (focusZoomTarget_ - zoom_) * k;
		float dx = focusTarget_.x - center_.x, dy = focusTarget_.y - center_.y;
		if (dx * dx + dy * dy < 1.0f && fabsf(focusZoomTarget_ - zoom_) < 0.001f)
			focusAnim_ = false;
	}

	hover_ = hovered ? hitTest(d, screenToWorld(io.MousePos), false) : -1;

	// left click: a socket selects the jewel location; any other node is a
	// toggle target (caller decides). Only on a genuine click, not a drag.
	if (ImGui::IsItemDeactivated() &&
	    ImLengthSqr(ImGui::GetMouseDragDelta(ImGuiMouseButton_Left)) < 16.0f * uiScale * uiScale) {
		int s = hitTest(d, screenToWorld(io.MousePos), true);
		if (s != -1) out.clickedSocket = s;
		else {
			int n = hitTest(d, screenToWorld(io.MousePos), false);
			if (n != -1 && d.nodes[n].kind != kPtSocket) out.clickedNode = n;
		}
	}

	const std::vector<unsigned char>* hi = in.hi;
	auto hiOf = [&](int i) -> unsigned char {
		return (hi && i < (int)hi->size()) ? (*hi)[i] : (unsigned char)kPtHiNone;
	};

	ImVec2 wmin = screenToWorld(vpPos_);
	ImVec2 wmax = screenToWorld(vpPos_ + vpSize_);
	auto cull = [&](float x, float y, float m) {
		return x < wmin.x - m || x > wmax.x + m || y < wmin.y - m || y > wmax.y + m;
	};

	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->PushClipRect(vpPos_, vpPos_ + vpSize_, true);

	// --- group backgrounds -------------------------------------------------
	auto drawDecos = [&](const std::vector<PtDeco>& decos) {
		for (const PtDeco& deco : decos) {
			if (!deco.spr.valid || deco.spr.sheet >= (int)tex_.size()) continue;
			float hw = deco.spr.w * 0.5f, hh = deco.spr.h;
			if (cull(deco.x, deco.y, hh + hw)) continue;
			ImTextureID t = (ImTextureID)(intptr_t)tex_[deco.spr.sheet];
			const float* uv = deco.spr.uv;
			if (deco.half) {
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
	};
	drawDecos(d.groupbg);

	// --- edges -------------------------------------------------------------
	float thick = std::clamp(zoom_ * 14.0f, 1.0f, 5.0f);
	const float emargin = 900.0f;
	for (const PtEdge& e : d.edges) {
		const PtNode& na = d.nodes[e.a];
		const PtNode& nb = d.nodes[e.b];
		if ((na.x < wmin.x - emargin && nb.x < wmin.x - emargin) ||
		    (na.x > wmax.x + emargin && nb.x > wmax.x + emargin) ||
		    (na.y < wmin.y - emargin && nb.y < wmin.y - emargin) ||
		    (na.y > wmax.y + emargin && nb.y > wmax.y + emargin))
			continue;
		bool inRad = hiOf(e.a) && hiOf(e.b);
		ImU32 col = inRad ? kColEdgeInRad : kColEdge;
		float t = inRad ? thick * 1.2f : thick;
		if (e.hasArc) {
			ImVec2 c = worldToScreen(ImVec2(e.cx, e.cy));
			int segs = std::clamp((int)(e.sweep * 16.0f) + 4, 6, 48);
			dl->PathArcTo(c, e.r * zoom_, e.a0, e.a0 + e.sweep, segs);
			dl->PathStroke(col, 0, t);
		} else {
			dl->AddLine(worldToScreen(ImVec2(na.x, na.y)), worldToScreen(ImVec2(nb.x, nb.y)), col, t);
		}
	}

	drawDecos(d.masteries);

	// --- jewel radius ring (under nodes so highlights stay readable) -------
	if (in.selectedSocket >= 0 && in.selectedSocket < (int)d.nodes.size()) {
		const PtNode& s = d.nodes[in.selectedSocket];
		ImVec2 c = worldToScreen(ImVec2(s.x, s.y));
		float rr = in.radiusWorld * zoom_;
		dl->AddCircle(c, rr, kColRadiusRing, 96, 2.5f); // outline only, no fill
	}

	// --- nodes -------------------------------------------------------------
	for (int i = 0; i < (int)d.nodes.size(); i++) {
		const PtNode& n = d.nodes[i];
		if (cull(n.x, n.y, 260.0f)) continue;
		unsigned char h = hiOf(i);

		if (n.kind == kPtSocket) {
			ImVec2 c = worldToScreen(ImVec2(n.x, n.y));
			bool sel = (i == in.selectedSocket);
			float rad = (sel ? 26.0f : 20.0f) * zoom_ + 3.0f;
			dl->AddCircleFilled(c, rad, IM_COL32(20, 24, 34, 235), 20);
			dl->AddCircle(c, rad, sel ? kColSocketSel : kColSocket, 20, sel ? 3.5f : 2.0f);
			if (sel) dl->AddCircleFilled(c, rad * 0.45f, kColSocketSel, 16);
			continue;
		}

		ImVec2 c = worldToScreen(ImVec2(n.x, n.y));
		bool sel = in.selected && i < (int)in.selected->size() && (*in.selected)[i];
		// a SELECTED node lights up (allocated art); everything else stays dim
		const PtSprite& icon = (sel ? n.on : n.off);
		if (icon.valid && icon.sheet < (int)tex_.size()) {
			ImVec2 a = worldToScreen(ImVec2(n.x - icon.w * 0.5f, n.y - icon.h * 0.5f));
			ImVec2 b = worldToScreen(ImVec2(n.x + icon.w * 0.5f, n.y + icon.h * 0.5f));
			dl->AddImage((ImTextureID)(intptr_t)tex_[icon.sheet], a, b,
			             ImVec2(icon.uv[0], icon.uv[1]), ImVec2(icon.uv[2], icon.uv[3]));
		}
		if (n.kind >= 0 && n.kind <= 2) {
			const PtFrame& fr = d.frames[n.kind];
			const PtSprite& f = (sel ? fr.on : fr.off);
			if (f.valid && f.sheet < (int)tex_.size()) {
				ImVec2 a = worldToScreen(ImVec2(n.x - f.w * 0.5f, n.y - f.h * 0.5f));
				ImVec2 b = worldToScreen(ImVec2(n.x + f.w * 0.5f, n.y + f.h * 0.5f));
				dl->AddImage((ImTextureID)(intptr_t)tex_[f.sheet], a, b,
				             ImVec2(f.uv[0], f.uv[1]), ImVec2(f.uv[2], f.uv[3]));
			}
		}
		// a node whose rolled affix MATCHES the search: red ring, drawn a bit
		// larger than the node itself so it reads as a halo around it.
		// (a SELECTED node needs no ring — its lit-up icon already stands out.)
		if (h) {
			float ring = (std::max(n.off.w, n.off.h) * 0.5f + 22.0f) * zoom_;
			dl->AddCircle(c, std::max(ring, 8.0f), h == kPtHiReplaced ? kColReplaced : kColAffected, 0, 3.5f);
		}
		if (i == hover_)
			dl->AddCircle(c, std::max((std::max(n.off.w, n.off.h) * 0.5f + 16.0f) * zoom_, 5.0f),
			              kColHoverRing, 0, 2.0f);
	}

	// emphasized node picked from the side list: a bright pulsing ring
	if (in.emphasize >= 0 && in.emphasize < (int)d.nodes.size()) {
		const PtNode& n = d.nodes[in.emphasize];
		if (!cull(n.x, n.y, 260.0f)) {
			float pulse = 0.5f + 0.5f * sinf((float)ImGui::GetTime() * 4.0f);
			float base = (std::max(n.off.w, n.off.h) * 0.5f + 18.0f) * zoom_;
			dl->AddCircle(worldToScreen(ImVec2(n.x, n.y)), std::max(base + pulse * 6.0f, 8.0f),
			              IM_COL32(255, 235, 130, 200 + (int)(pulse * 55)), 0, 3.5f);
		}
	}

	dl->PopClipRect();

	ImGui::EndChild();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor();

	out.hoveredNode = hover_;
	return out;
}
