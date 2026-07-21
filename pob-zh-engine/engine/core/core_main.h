// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Engine Core Header
//

// ==========
// Interfaces
// ==========

// Engine Core
class core_IMain {
public:
	static core_IMain* GetHandle(sys_IMain* sysHnd);
	static void FreeHandle(core_IMain* hnd);

	core_IConfig*	config = nullptr;
	core_IVideo*	video = nullptr;

	virtual void	Init(int argc, char** argv) = 0;
	virtual void	Frame() = 0;
	virtual void	Shutdown() = 0;
	virtual void	KeyEvent(int key, int type) = 0;
	virtual bool	CanExit() = 0;
};

