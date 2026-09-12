// 倉庫收益統計 — turning an item into the key a price is filed under.
//
// The same key format is produced from both sides -- a stash item here, a
// poe.ninja line in warehouse_ninja -- so pricing is a plain map lookup. The
// bucketing (gem level/quality brackets, map tiers, six-links) is ported from
// awakened-poe-trade's getDetailsId logic: prices only exist at the granularity
// poe.ninja publishes, so the key must round to it.
//
//   currency|<name>            stackables the exchange API covers (also fragments,
//                              scarabs, oils, essences, div cards by their own tag)
//   card|<name>                divination cards
//   gem|<name>|<lvl>|<q>|c?    level bucket 1/20/21, quality bucket 0/20/23
//   unique|<name>|<base>[|6L]
//   map|<name>|T<n>
//
// Pure functions; the self-test drives the whole matrix.
#pragma once

#include <string>

struct StashItemRaw;

// False = this item kind cannot be priced from overview data (rare gear, random
// magic items...). `dispEn` receives the English display name the UI should
// show for the aggregated line.
bool BuildPriceKey(const StashItemRaw& it, std::string* key, std::string* dispEn);

// The ninja side of the same contract, used by warehouse_ninja's parsers.
std::string NinjaExchangeKey(const std::string& name);
std::string NinjaCardKey(const std::string& name);
std::string NinjaGemKey(const std::string& name, int level, int quality, bool corrupted);
std::string NinjaUniqueKey(const std::string& name, const std::string& baseType, int links);
std::string NinjaMapKey(const std::string& name, int tier);
