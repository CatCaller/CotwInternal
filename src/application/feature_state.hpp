#pragma once

namespace cotw::application
{
    struct FeatureState
    {
        bool noSensesEnabled;
        bool noSmellEnabled;
        bool speedEnabled;
        float speedMultiplier;
        float highlightDistance;
    };

    inline FeatureState features{
        true,
        true,
        true,
        2.0f,
        600.0f,
    };
}
