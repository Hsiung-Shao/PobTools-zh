// 倉庫收益統計 — how money is written, shared by every view of it (the revenue
// panel and the atlas planner's cost card). One definition, so a value can never
// read "3.2 d" in one window and "640 c" in the other.
#pragma once

#include <cstdio>
#include <string>

namespace WhFmt {

inline std::string FormatChaos(double v)
{
	char buf[48];
	if (v >= 1000 || v <= -1000) snprintf(buf, sizeof(buf), "%.0f", v);
	else snprintf(buf, sizeof(buf), "%.1f", v);
	return buf;
}

// One value, in the display currency. Divine mode converts only what is worth
// at least one divine -- a 0.8c essence shown as "0.00 d" reads as zero, so
// sub-divine values stay in chaos (the Wealthy-Exile convention). divineRate 0
// forces chaos for everything.
inline std::string FormatValue(double chaos, bool divine, double divineRate)
{
	const double mag = chaos < 0 ? -chaos : chaos;
	if (divine && divineRate > 0 && mag >= divineRate) {
		char buf[48];
		const double d = chaos / divineRate;
		if (mag >= 100 * divineRate) snprintf(buf, sizeof(buf), "%.0f d", d);
		else snprintf(buf, sizeof(buf), "%.1f d", d);
		return buf;
	}
	return FormatChaos(chaos) + " c";
}

// "14d 64c": whole divines plus the chaos left over -- the Wealthy-Exile
// snapshot-card format. No known rate: chaos only ("326c").
inline std::string FormatDivChaos(double chaos, double divineRate, bool showPlus)
{
	const bool neg = chaos < 0;
	const double mag = neg ? -chaos : chaos;
	const char* sign = neg ? "-" : (showPlus ? "+" : "");
	char buf[64];
	if (divineRate > 0) {
		long long d = (long long)(mag / divineRate);
		long long c = (long long)(mag - (double)d * divineRate + 0.5);
		if ((double)c >= divineRate) { // the rounding carried a whole divine
			d++;
			c = 0;
		}
		snprintf(buf, sizeof(buf), "%s%lldd %lldc", sign, d, c);
	} else {
		snprintf(buf, sizeof(buf), "%s%.0fc", sign, mag);
	}
	return buf;
}

} // namespace WhFmt
