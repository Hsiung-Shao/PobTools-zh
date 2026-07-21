// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Core Video Header
//

// ==========
// Interfaces
// ==========

// Video Manager
class core_IVideo {
public:
	static core_IVideo* GetHandle(sys_IMain* sysHnd);
	static void FreeHandle(core_IVideo* hnd);

	virtual void	Apply(bool shown = true) = 0;
	virtual void	Save() = 0;
};
