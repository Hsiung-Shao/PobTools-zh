// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: System OpenGL
// Platform: Windows
//

#include <glad/gles2.h>

#include "sys_local.h"

#include <GLFW/glfw3.h>

// =====================
// sys_IOpenGL Interface
// =====================

class sys_openGL_c: public sys_IOpenGL {
public:
	// Interface
	bool	Init(sys_glSet_s* set);
	bool	Shutdown();
	void	Swap();

	void*	GetProc(const char* name);

	// Encapsulated
	sys_openGL_c(sys_IMain* sysHnd);

	sys_main_c* sys;
};

sys_IOpenGL* sys_IOpenGL::GetHandle(sys_IMain* sysHnd)
{
	return new sys_openGL_c(sysHnd);
}

void sys_IOpenGL::FreeHandle(sys_IOpenGL* hnd)
{
	delete (sys_openGL_c*)hnd;
}

sys_openGL_c::sys_openGL_c(sys_IMain* sysHnd)
	: sys((sys_main_c*)sysHnd)
{
}

// ===================
// System OpenGL Class
// ===================

bool sys_openGL_c::Init(sys_glSet_s* set)
{
	// Set swap interval
	glfwSwapInterval(set->vsync ? 1 : 0);

	return false;
}

bool sys_openGL_c::Shutdown()
{
	return false;
}

void sys_openGL_c::Swap()
{
	// Opacity diagnostics: what alpha actually sits in the default framebuffer
	// right before it is presented (see sys_opacity_trace). Once a second, and
	// only with POB_ZH_OPACITY_TRACE set: glReadPixels stalls the pipeline.
	static unsigned frame = 0;
	if (sys_opacity_trace_enabled() && (frame++ % 60) == 0) {
		static bool reported = false;
		if (!reported) {
			reported = true;
			GLint a = -1;
			glGetIntegerv(GL_ALPHA_BITS, &a);
			sys_opacity_trace("default framebuffer GL_ALPHA_BITS=%d", (int)a);
		}
		const int w = sys->video->vid.fbSize[0], h = sys->video->vid.fbSize[1];
		if (w > 64 && h > 64) {
			unsigned char side[4] = {}, centre[4] = {};
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glReadPixels(30, h / 6, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, side);          // sidebar, lower-left
			glReadPixels(w / 2, h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, centre);     // tree canvas
			sys_opacity_trace("pct=%d fb=%dx%d sidebar rgba=%d,%d,%d,%d centre rgba=%d,%d,%d,%d",
			                  sys->video->windowOpacityPct, w, h,
			                  side[0], side[1], side[2], side[3], centre[0], centre[1], centre[2], centre[3]);
		}
	}
	glfwSwapBuffers((GLFWwindow*)sys->video->GetWindowHandle());
}

void* sys_openGL_c::GetProc(const char* name)
{
	return (void*)glfwGetProcAddress(name);
}
