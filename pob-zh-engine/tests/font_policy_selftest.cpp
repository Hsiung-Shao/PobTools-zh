// Standalone regression check: no renderer or external dependencies required.
#include "../engine/render/font_policy.h"
#include <cstdio>
#include <initializer_list>

int main()
{
    int checks = 0, failures = 0;
    const auto check = [&](const char* name, bool ok) {
        ++checks;
        if (!ok) ++failures;
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    };
    check("Pico filename", FontPolicy::IsPico("Pico-Regular.ttf"));
    check("case-insensitive Windows path", FontPolicy::IsPico("C:\\Fonts\\PICO-REGULAR.TTF"));
    check("Unix path", FontPolicy::IsPico("/opt/Fonts/Pico-Regular.ttf"));
    check("unrelated filename", !FontPolicy::IsPico("NotPico-Regular.ttf"));
    check("unrelated directory", !FontPolicy::IsPico("Pico-Regular.ttf/NotoSansTC-Regular.ttf"));
    for (bool applyAll : {false, true}) {
        for (const char* face : {"Bitstream Vera Sans", "Bitstream Vera Sans Bold", "Bitstream Vera Sans Mono"}) {
            check("Pico covers every bitmap face", FontPolicy::UseSelectedAscii("Fonts/Pico-Regular.ttf", face, applyAll));
        }
    }
    check("other fonts preserve monospace", !FontPolicy::UseSelectedAscii("NotoSansTC-Regular.ttf", "Bitstream Vera Sans Mono", true));
    check("other fonts honor ASCII switch off", !FontPolicy::UseSelectedAscii("NotoSansTC-Regular.ttf", "Bitstream Vera Sans", false));
    check("other fonts honor ASCII switch on", FontPolicy::UseSelectedAscii("NotoSansTC-Regular.ttf", "Bitstream Vera Sans", true));
    check("missing Pico falls back normally", !FontPolicy::UseSelectedAscii("NotoSansTC-Regular.ttf", "Bitstream Vera Sans Mono", true));
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
