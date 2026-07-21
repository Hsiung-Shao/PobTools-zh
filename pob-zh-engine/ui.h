// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// UI Global Header
//

// ==========
// Interfaces
// ==========

// UI Main: ui_main.cpp
class ui_IMain {
public:
	static ui_IMain* GetHandle(sys_IMain* sysHnd, core_IMain* coreHnd);
	static void FreeHandle(ui_IMain* hnd);

	virtual void	Init(int argc, char** argv) = 0;
	virtual void	Frame() = 0;
	virtual void	Shutdown() = 0;
	virtual void	KeyEvent(int key, int type) = 0;
	virtual bool	CanExit() = 0;
};
