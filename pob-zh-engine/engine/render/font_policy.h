#pragma once
#include <string_view>

namespace FontPolicy {
// Match the bundled face, including paths used by the FreeType fallback chain.
inline bool IsPico(std::string_view file)
{
    const auto slash = file.find_last_of("/\\");
    if (slash != std::string_view::npos) file.remove_prefix(slash + 1);
    constexpr std::string_view name = "pico-regular.ttf";
    if (file.size() != name.size()) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = file[i] >= 'A' && file[i] <= 'Z' ? file[i] + ('a' - 'A') : file[i];
        if (c != name[i]) return false;
    }
    return true;
}

inline bool UseSelectedAscii(std::string_view loadedFile, std::string_view bitmapFace, bool applyAll)
{
    // Pico preserves fixed columns itself, even when the legacy ASCII switch is off.
    return IsPico(loadedFile) || (applyAll && bitmapFace != "Bitstream Vera Sans Mono");
}
} // namespace FontPolicy
