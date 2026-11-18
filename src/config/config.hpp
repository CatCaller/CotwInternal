#pragma once

#include <Windows.h>

namespace cotw::config
{
    struct Config
    {
        bool noSensesEnabled;
        bool noSmellEnabled;
        bool speedEnabled;
        float speedMultiplier;
        float highlightDistance;
        int keyToggleSenses;
        int keyToggleSpeed;
    };

    void loadConfig(Config& config, HMODULE module) noexcept;
}
