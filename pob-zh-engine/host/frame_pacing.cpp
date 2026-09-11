#include "frame_pacing.h"

#include <imgui.h>

#include <cstring>

namespace FramePacing {
namespace {

// Word-at-a-time mixing: the launcher's draw data is a few hundred kilobytes
// of vertices per frame, and a byte-wise FNV over that would cost more than
// the presents it saves. Not cryptographic; it only has to notice a change.
inline uint64_t Mix(uint64_t h, uint64_t v)
{
	h ^= v;
	h *= 0x9E3779B97F4A7C15ull;
	return h ^ (h >> 32);
}

uint64_t HashBytes(uint64_t h, const void* p, size_t n)
{
	const unsigned char* b = (const unsigned char*)p;
	size_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t w;
		memcpy(&w, b + i, 8);
		h = Mix(h, w);
	}
	if (i < n) {
		uint64_t w = 0;
		memcpy(&w, b + i, n - i);
		h = Mix(h, w);
	}
	return Mix(h, (uint64_t)n);
}

} // namespace

uint64_t HashDrawData(const ImDrawData* dd)
{
	if (!dd || !dd->Valid) return 0;
	uint64_t h = 0x243F6A8885A308D3ull;
	h = HashBytes(h, &dd->DisplayPos, sizeof(dd->DisplayPos));
	h = HashBytes(h, &dd->DisplaySize, sizeof(dd->DisplaySize));
	h = HashBytes(h, &dd->FramebufferScale, sizeof(dd->FramebufferScale));
	h = Mix(h, (uint64_t)dd->CmdListsCount);
	for (int n = 0; n < dd->CmdListsCount; n++) {
		const ImDrawList* dl = dd->CmdLists[n];
		h = Mix(h, (uint64_t)dl->VtxBuffer.Size);
		h = HashBytes(h, dl->VtxBuffer.Data, (size_t)dl->VtxBuffer.Size * sizeof(ImDrawVert));
		h = Mix(h, (uint64_t)dl->IdxBuffer.Size);
		h = HashBytes(h, dl->IdxBuffer.Data, (size_t)dl->IdxBuffer.Size * sizeof(ImDrawIdx));
		h = Mix(h, (uint64_t)dl->CmdBuffer.Size);
		for (const ImDrawCmd& c : dl->CmdBuffer) {
			h = HashBytes(h, &c.ClipRect, sizeof(c.ClipRect));
			h = Mix(h, (uint64_t)(uintptr_t)c.TextureId);
			h = Mix(h, ((uint64_t)c.VtxOffset << 32) | c.IdxOffset);
			h = Mix(h, (uint64_t)c.ElemCount);
			h = Mix(h, (uint64_t)(uintptr_t)c.UserCallback);
		}
	}
	// Never collide with "nothing presented yet".
	return h ? h : 1;
}

bool ImGuiActivity()
{
	const ImGuiIO& io = ImGui::GetIO();
	if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) return true;
	if (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f) return true;
	if (ImGui::IsAnyMouseDown()) return true;
	for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
		if (ImGui::IsKeyDown((ImGuiKey)k)) return true;
	if (ImGui::IsAnyItemActive() || io.WantTextInput) return true;
	if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) return true;
	return false;
}

bool Pacer::ShouldRender(const Inputs& in, const ImDrawData* dd)
{
	if (in.activity) lastActivity_ = in.now;
	fast_ = in.busy || (in.now - lastActivity_ < kActiveTail);
	iconified_ = in.iconified;
	if (in.iconified) {
		// Nothing to present into. Remembered so the first frame after the
		// restore is presented even when its draw data happens to match.
		hiddenSince_ = true;
		return false;
	}
	const uint64_t h = HashDrawData(dd);
	const bool render = in.forceRender || !presentedOnce_ || hiddenSince_ || h != lastPresented_;
	if (render) {
		lastPresented_ = h;
		presentedOnce_ = true;
		hiddenSince_ = false;
	}
	return render;
}

double Pacer::WaitSeconds(double now) const
{
	if (iconified_ || !fast_) return kIdleWait;
	const double spent = now - frameStart_;
	return spent >= kActiveWait ? 0.0 : kActiveWait - spent;
}

} // namespace FramePacing
