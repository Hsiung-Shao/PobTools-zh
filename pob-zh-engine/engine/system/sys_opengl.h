// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// System OpenGL Header
//

// =======
// Classes
// =======

// OpenGL settings structure
struct sys_glSet_s {
	int		bColor;			// Color bits
	int		bDepth;			// Depth bits
	int		bStencil;		// Stencil bits
	bool	vsync;			// Enable vsync?
};

// ==========
// Interfaces
// ==========

// System OpenGL
class sys_IOpenGL {
public:
	static sys_IOpenGL* GetHandle(class sys_IMain* sysHnd);
	static void FreeHandle(sys_IOpenGL* hnd);

	virtual	bool	Init(sys_glSet_s* set) = 0; // Initialise
	virtual	bool	Shutdown() = 0;	// Shutdown
	virtual	void	Swap() = 0; // Swap buffers

	virtual void*	GetProc(const char* name) = 0; // Get extension function address
};
