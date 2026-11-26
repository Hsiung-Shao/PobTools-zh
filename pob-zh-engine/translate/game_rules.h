/*
** game_rules.h - the translation rules that differ between PoE1 and PoE2
**
** One DLL serves both games; the game is picked once per load (translation_game()).
** A rule only one game needs goes here as a field, never as an `if game` inside
** the shared matcher -- that is how a PoE1 rule once ate PoE2's "[...]" rows.
*/
#ifndef GAME_RULES_H
#define GAME_RULES_H

struct GameRules {
    const char *game; /* "poe1" / "poe2" */
    /* Stat lookups keep the input line's square brackets as they are. POB2's
    ** drawn strings never carry GGPK markup, so stripping would eat POB's own
    ** "[...]" around a crafting row's lines. PoE1 strips (GGPK "[a|b]" markup). */
    bool stat_keep_input_brackets;
    /* Reverse lookup: a bracket-stripped copy of a "[a|b]" row may replace an
    ** existing plain row. PoE2's markup rows are its own wording; in PoE1 files
    ** they are PoE2 strays and must not steal 增加 15% 暴擊率 from PoE1's row. */
    bool stripped_reverse_wins;
};

extern const GameRules kGameRulesPoe1;
extern const GameRules kGameRulesPoe2;

#endif
