// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "video_core/oot_network.h"

using namespace VideoCommon::OotNetwork;

TEST_CASE("Unused AA dispatch requires confirmed output replacement and a skipped prepass",
          "[video_core][oot_network]") {
    CHECK_FALSE(ShouldSkipUnusedDispatch(UnusedDispatchProgramKey, false, false));
    CHECK_FALSE(ShouldSkipUnusedDispatch(UnusedDispatchProgramKey, false, true));
    CHECK_FALSE(ShouldSkipUnusedDispatch(UnusedDispatchProgramKey, true, false));
    CHECK(ShouldSkipUnusedDispatch(UnusedDispatchProgramKey, true, true));
    CHECK_FALSE(ShouldSkipUnusedDispatch(UnusedDispatchProgramKey ^ 1, true, true));
    CHECK_FALSE(ShouldSkipUnusedDispatch(0xd9ee412f00ce399aULL, true, true));
    CHECK_FALSE(ShouldSkipUnusedDispatch(PrepassProgramKey, true, true));
    CHECK_FALSE(ShouldSkipUnusedDispatch(NetworkProgramKey, true, true));
    CHECK_FALSE(ShouldSkipUnusedDispatch(OutputProgramKey, true, true));
    for (const auto& output : BaseOutputPrograms) {
        CHECK_FALSE(ShouldSkipUnusedDispatch(output.key, true, true));
    }
}

TEST_CASE("Ocarina of Time network bypass follows the setting and the game",
          "[video_core][oot_network]") {
    auto& setting = Settings::values.oot_anti_aliasing;
    const auto previous = setting.GetValue();

    setting.SetValue(Settings::OotAntiAliasing::Game);
    CHECK_FALSE(BypassEnabled(ProgramId));

    setting.SetValue(Settings::OotAntiAliasing::NoSharpening);
    CHECK(BypassEnabled(ProgramId));
    CHECK_FALSE(BypassEnabled(0x0400C3F00006E000ULL));

    setting.SetValue(previous);
}
