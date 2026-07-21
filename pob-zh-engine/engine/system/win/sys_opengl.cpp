// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: System OpenGL
// Platform: Windows
//

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
	glfwSwapBuffers((GLFWwindow*)sys->video->GetWindowHandle());
}

void* sys_openGL_c::GetProc(const char* name)
{
	return (void*)glfwGetProcAddress(name);
}
