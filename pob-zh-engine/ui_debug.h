// DyLua: SimpleGraphic
// (c) David Gowor, 2014
//
// UI Debug Header
//

// ==========
// Interfaces
// ==========

// UI Debug Handler
class ui_IDebug {
public:
	static ui_IDebug* GetHandle(class ui_main_c*);
	static void FreeHandle(ui_IDebug*);

	virtual void	SetProfiling(bool enable) = 0;
	virtual void	ToggleProfiling() = 0;
};