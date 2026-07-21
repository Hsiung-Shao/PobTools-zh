// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// UI Sub Script Header
//

// ==========
// Interfaces
// ==========

// UI Sub Script Handler
class ui_ISubScript {
public:
	static ui_ISubScript* GetHandle(class ui_main_c*, dword id);
	static void FreeHandle(ui_ISubScript*);

	virtual bool	Start() = 0;
	virtual	void	SubScriptFrame() = 0;
	virtual bool	IsRunning() = 0;
	virtual size_t	GetScriptMemory() = 0;
};