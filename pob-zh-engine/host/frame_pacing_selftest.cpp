// --frame-pacing-selftest: the pacing rules as a table, checked without a
// window. What matters is what the launcher would DO with the answer: a frame
// that is not presented stays on screen from the previous present, so "no"
// must only ever be said for a picture that really is the same, and "yes"
// must come back the moment anything -- input, a restore, a refresh -- means
// the screen and the draw data may disagree.

#include "frame_pacing.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <imgui.h>

#include <cmath>
#include <string>

namespace {

std::string g_report;
int g_fail = 0;

void line(const std::string& s) { g_report += s; g_report += "\r\n"; }

void check(const std::string& what, bool ok, const std::string& detail = "")
{
	if (!ok) g_fail++;
	line(std::string(ok ? "PASS " : "FAIL ") + what + (detail.empty() ? "" : "  (" + detail + ")"));
}

// A draw list with one textured quad, built by hand: no ImGui context needed.
struct Frame {
	ImDrawList list{ nullptr };
	ImDrawData data;

	explicit Frame(float x = 10.0f, ImTextureID tex = (ImTextureID)(uintptr_t)7, unsigned elems = 6)
	{
		ImDrawVert v{};
		v.col = IM_COL32_WHITE;
		for (int i = 0; i < 4; i++) {
			v.pos = ImVec2(x + (float)(i & 1) * 20.0f, 10.0f + (float)(i >> 1) * 20.0f);
			v.uv = ImVec2((float)(i & 1), (float)(i >> 1));
			list.VtxBuffer.push_back(v);
		}
		const ImDrawIdx idx[6] = { 0, 1, 2, 1, 3, 2 };
		for (ImDrawIdx i : idx) list.IdxBuffer.push_back(i);
		ImDrawCmd cmd;
		cmd.ClipRect = ImVec4(0, 0, 800, 600);
		cmd.TextureId = tex;
		cmd.ElemCount = elems;
		list.CmdBuffer.push_back(cmd);
		data.Valid = true;
		data.CmdLists.push_back(&list);
		data.CmdListsCount = 1;
		data.TotalVtxCount = list.VtxBuffer.Size;
		data.TotalIdxCount = list.IdxBuffer.Size;
		data.DisplayPos = ImVec2(0, 0);
		data.DisplaySize = ImVec2(800, 600);
		data.FramebufferScale = ImVec2(1, 1);
	}
};

bool Near(double a, double b) { return std::fabs(a - b) < 1e-6; }

} // namespace

int RunFramePacingSelfTest(const std::wstring& exeDir)
{
	using namespace FramePacing;
	g_report.clear();
	g_fail = 0;
	line("PobTools frame pacing self-test");
	line("");

	// P1 -- the hash sees what the renderer sees, and nothing else.
	{
		Frame a, b;
		check("P1 identical draw data hashes equal", HashDrawData(&a.data) == HashDrawData(&b.data));
		Frame moved(11.0f);
		check("P1b one vertex moved changes the hash", HashDrawData(&a.data) != HashDrawData(&moved.data));
		Frame tex(10.0f, (ImTextureID)(uintptr_t)8);
		check("P1c a different texture changes the hash", HashDrawData(&a.data) != HashDrawData(&tex.data));
		Frame elems(10.0f, (ImTextureID)(uintptr_t)7, 3);
		check("P1d a different element count changes the hash", HashDrawData(&a.data) != HashDrawData(&elems.data));
		Frame size;
		size.data.DisplaySize = ImVec2(801, 600);
		check("P1e a different display size changes the hash", HashDrawData(&a.data) != HashDrawData(&size.data));
		check("P1f null and invalid data hash to zero", HashDrawData(nullptr) == 0 &&
		      [] { Frame f; f.data.Valid = false; return HashDrawData(&f.data) == 0; }());
		check("P1g real data never hashes to zero", HashDrawData(&a.data) != 0);
	}

	// P2 -- the first frame is always presented; an unchanged second one is not.
	{
		Pacer p;
		Frame f;
		Inputs in;
		in.now = 100.0;
		p.BeginFrame(in.now);
		check("P2 the first frame is presented", p.ShouldRender(in, &f.data));
		in.now = 100.1;
		p.BeginFrame(in.now);
		check("P2b the same frame again is not", !p.ShouldRender(in, &f.data));
		Frame g(12.0f);
		in.now = 100.2;
		check("P2c a changed frame is", p.ShouldRender(in, &g.data));
		in.now = 100.3;
		check("P2d and the changed frame repeated is not", !p.ShouldRender(in, &g.data));
		in.forceRender = true;
		check("P2e a refresh event presents an unchanged frame", p.ShouldRender(in, &g.data));
		in.forceRender = false;
		check("P2f but only once", !p.ShouldRender(in, &g.data));
	}

	// P3 -- minimised: never presented, idle wait; the restore presents once
	// even when the picture matches the last one that reached the screen.
	{
		Pacer p;
		Frame f;
		Inputs in;
		in.now = 1.0;
		p.BeginFrame(in.now);
		p.ShouldRender(in, &f.data);
		in.iconified = true;
		in.activity = true; // input while minimised must not turn the cadence up
		in.now = 1.1;
		p.BeginFrame(in.now);
		check("P3 minimised is never presented", !p.ShouldRender(in, &f.data));
		check("P3b minimised waits the idle interval", Near(p.WaitSeconds(1.105), kIdleWait));
		in.iconified = false;
		in.activity = false;
		in.now = 1.2;
		p.BeginFrame(in.now);
		check("P3c the first frame after a restore is presented", p.ShouldRender(in, &f.data));
		in.now = 1.3;
		check("P3d and then only on change again", !p.ShouldRender(in, &f.data));
	}

	// P4 -- cadence: fast for half a second after input, idle after, busy wins.
	{
		Pacer p;
		Frame f;
		Inputs in;
		in.now = 10.0;
		p.BeginFrame(in.now);
		p.ShouldRender(in, &f.data);
		check("P4 no input ever seen: idle cadence", !p.Fast() && Near(p.WaitSeconds(10.001), kIdleWait));
		in.activity = true;
		in.now = 10.1;
		p.BeginFrame(in.now);
		p.ShouldRender(in, &f.data);
		check("P4b input: fast cadence", p.Fast());
		check("P4c fast wait is the 60 Hz frame minus the time the frame took",
		      Near(p.WaitSeconds(10.1 + 0.005), kActiveWait - 0.005), std::to_string(p.WaitSeconds(10.105)));
		check("P4d a frame slower than 60 Hz waits nothing", Near(p.WaitSeconds(10.1 + 0.03), 0.0));
		in.activity = false;
		in.now = 10.1 + kActiveTail - 0.05;
		p.BeginFrame(in.now);
		p.ShouldRender(in, &f.data);
		check("P4e still fast inside the tail", p.Fast());
		in.now = 10.1 + kActiveTail + 0.05;
		p.BeginFrame(in.now);
		p.ShouldRender(in, &f.data);
		check("P4f idle once the tail has passed", !p.Fast() && Near(p.WaitSeconds(in.now), kIdleWait));
		in.busy = true;
		in.now = 20.0;
		p.BeginFrame(in.now);
		p.ShouldRender(in, &f.data);
		check("P4g busy keeps the fast cadence without input", p.Fast() && Near(p.WaitSeconds(20.0), kActiveWait));
		in.busy = false;
		in.iconified = true;
		in.now = 20.1;
		p.BeginFrame(in.now);
		p.ShouldRender(in, &f.data);
		check("P4h minimised overrides busy for the wait", Near(p.WaitSeconds(20.1), kIdleWait));
	}

	line("");
	line(g_fail ? "RESULT FAIL" : "RESULT PASS");

	CreateDirectoryW((exeDir + L"PobTools").c_str(), nullptr);
	HANDLE h = CreateFileW((exeDir + L"PobTools\\frame_pacing_selftest.txt").c_str(),
	                       GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w = 0;
		WriteFile(h, g_report.data(), (DWORD)g_report.size(), &w, nullptr);
		CloseHandle(h);
	}
	return g_fail ? 2 : 0;
}
