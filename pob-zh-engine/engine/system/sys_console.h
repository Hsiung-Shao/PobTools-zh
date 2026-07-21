// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// System Console Header
//

// ==========
// Interfaces
// ==========

// System Console
class sys_IConsole {
public:
	static sys_IConsole* GetHandle(class sys_IMain* sysHnd);
	static void FreeHandle(sys_IConsole* hnd);

	virtual	void	SetVisible(bool show) = 0;	// Set window state
	virtual	bool	IsVisible() = 0;				// Get window state
	virtual void	SetForeground() = 0; // Bring window to foreground if shown
	virtual void	SetTitle(const char* title) = 0; // Set window title
};
