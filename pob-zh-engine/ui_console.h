// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// UI Console Header
//

// ==========
// Interfaces
// ==========

// Console Handler
class ui_IConsole {
public:
	static ui_IConsole* GetHandle(class ui_main_c* ui);
	static void FreeHandle(ui_IConsole* hnd);

	virtual void	Toggle() = 0;
	virtual void	Hide() = 0;
	virtual bool	KeyEvent(int key, int type) = 0;
	virtual void	Render() = 0;
};
