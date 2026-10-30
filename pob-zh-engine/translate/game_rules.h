/*
** game_rules.h - the translation rules that differ between PoE1 and PoE2
**
** 使用者裁定 2026-10-29/30: PoE1 and PoE2 run their own rules. One DLL serves
** both for now (方案 C); the game is picked once per load (translation_game()).
** A rule only one game needs goes here as a field, never as an `if game` inside
** the shared matcher -- that is how a PoE1 rule once ate PoE2's "[...]" rows.
** rules_poe1.cpp keeps today's PoE1 behaviour field for field.
*/
#ifndef GAME_RULES_H
#define GAME_RULES_H

struct GameRules {
    const char *game; /* "poe1" / "poe2" */
    /* Stat lookups keep the input line's square brackets as they are. POB2's
    ** drawn strings never carry GGPK markup, so stripping would eat POB's own
    ** "[...]" around a crafting row's lines. PoE1 strips (GGPK "[a|b]" markup). */
    bool stat_keep_input_brackets;
};

extern const GameRules kGameRulesPoe1;
extern const GameRules kGameRulesPoe2;

#endif
