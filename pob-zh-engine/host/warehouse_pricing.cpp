#include "warehouse_pricing.h"

#include "warehouse_provider.h"

namespace {

// poe.ninja publishes gem prices at these brackets and nowhere between: 1/20/21
// for level, 0/20/23 for quality. Anything off-bracket is priced as the bracket
// below it -- the same rounding a human does on the site.
int GemLevelBucket(int level)
{
	if (level >= 21) return 21;
	if (level >= 20) return 20;
	return 1;
}

int GemQualityBucket(int q)
{
	if (q >= 23) return 23;
	if (q >= 20) return 20;
	return 0;
}

} // namespace

std::string NinjaExchangeKey(const std::string& name)
{
	return "currency|" + name;
}

std::string NinjaCardKey(const std::string& name)
{
	return "card|" + name;
}

std::string NinjaGemKey(const std::string& name, int level, int quality, bool corrupted)
{
	std::string k = "gem|" + name + "|" + std::to_string(GemLevelBucket(level)) + "|" +
	                std::to_string(GemQualityBucket(quality));
	if (corrupted) k += "|c";
	return k;
}

std::string NinjaUniqueKey(const std::string& name, const std::string& baseType, int links)
{
	std::string k = "unique|" + name + "|" + baseType;
	if (links >= 6) k += "|6L";
	return k;
}

std::string NinjaMapKey(const std::string& name, int tier)
{
	return "map|" + name + "|T" + std::to_string(tier);
}

bool BuildPriceKey(const StashItemRaw& it, std::string* key, std::string* dispEn)
{
	switch (it.frameType) {
	case 5: // currency-frame stackables: currency, fragments-with-frame, scarabs,
	        // oils, essences, fossils, tattoos, omens... exactly the exchange API's world
		*key = NinjaExchangeKey(it.typeLine);
		*dispEn = it.typeLine;
		return true;
	case 6: // divination card
		*key = NinjaCardKey(it.typeLine);
		*dispEn = it.typeLine;
		return true;
	case 4: // gem -- typeLine IS the gem name
		*key = NinjaGemKey(it.typeLine, it.gemLevel, it.gemQuality, it.corrupted);
		*dispEn = it.typeLine;
		return true;
	case 3: // unique; an unidentified one has no name and cannot be priced
		if (it.name.empty()) return false;
		if (it.mapTier > 0) {
			// Unique maps are their own ninja category but key fine on name+base.
			*key = NinjaUniqueKey(it.name, it.baseType, 0);
		} else {
			*key = NinjaUniqueKey(it.name, it.baseType, it.links);
		}
		*dispEn = it.name;
		return true;
	case 0:
		if (it.mapTier > 0) { // white map: baseType is the clean map name
			*key = NinjaMapKey(it.baseType, it.mapTier);
			*dispEn = it.baseType;
			return true;
		}
		// Frameless stackables (sacrifice fragments, breach splinters in old
		// payloads...) have no item level; white GEAR always has one. A miss here
		// just yields an unpriced line, never a wrong price.
		if (it.ilvl == 0 && it.gemLevel == 0) {
			*key = NinjaExchangeKey(it.typeLine);
			*dispEn = it.typeLine;
			return true;
		}
		return false;
	case 1:
	case 2: // magic/rare: only maps are overview-priceable, by their base
		if (it.mapTier > 0) {
			*key = NinjaMapKey(it.baseType, it.mapTier);
			*dispEn = it.baseType;
			return true;
		}
		return false;
	default:
		return false;
	}
}
