// PobTools filter-editor card schema: a pure data table describing every
// condition / action keyword the editor can edit as a card — its Chinese title,
// card widget kind, allowed operators, enum values, integer range, the default
// line inserted when the user ticks it in the add-column, alias keywords folded
// into the same card, and mutually-exclusive groups (e.g. the alert-sound
// family). The UI renders FROM this table; output tokens stay English.
#pragma once

#include <string>
#include <vector>

enum class CardKind {
	Toggle,       // flag with no value (DisableDropSound, Continue)
	Bool,         // True / False
	IntOp,        // operator + integer
	IntRange,     // bare integer slider (SetFontSize)
	EnumOp,       // operator + one ordered enum value (Rarity)
	EnumMulti,    // several enum values (HasInfluence, GemQualityType)
	StringList,   // quoted string chips (BaseType, Class, HasEnchantment, ...)
	ModList,      // HasExplicitMod: op + count prefix + mod-name chips
	SocketSpec,   // Sockets / SocketGroup: op + "5GGG"-style token
	Color,        // R G B [A]
	SoundBuiltin, // PlayAlertSound id volume (alias Positional)
	SoundCustom,  // CustomAlertSound "path" [vol] (alias Optional)
	MinimapIcon,  // size colour shape
	PlayEffect,   // colour [Temp]
};

struct SchemaEnumValue {
	const char* token;  // English output token
	const char* zh;     // display label
};

struct CardSchema {
	const char* keyword;             // primary English keyword
	const char* zh;                  // card title (Traditional Chinese)
	bool isAction;
	CardKind kind;
	const char* group;               // add-column group heading
	std::vector<const char*> ops;    // allowed operators; "" renders as 等於
	std::vector<SchemaEnumValue> enums;
	int minInt;
	int maxInt;
	const char* defaultLine;         // full line (no indent) inserted on tick
	int exclusiveGroup;              // >0: adding this disables live lines of the
	                                 // same group's other keywords
	const char* alias;               // variant keyword folded into this card
	const char* tooltip;             // short syntax help (may be nullptr)
};

// Table in display order (conditions first, then actions); card order in the
// middle pane and the add-column both follow it.
const std::vector<CardSchema>& FilterSchemaAll();

// Find by primary keyword or alias; nullptr when the keyword has no card.
const CardSchema* FilterSchemaFind(const std::string& keyword);

// Display helpers: Chinese title for a keyword (falls back to the keyword
// itself) and Chinese label for an enum/bool value (falls back to the value).
std::string FilterSchemaKeywordZh(const std::string& keyword);
std::string FilterSchemaValueZh(const std::string& keyword, const std::string& value);

// Add-column group headings in display order.
const std::vector<const char*>& FilterSchemaGroups();
