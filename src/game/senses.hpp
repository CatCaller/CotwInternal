#pragma once

#include "config/config.hpp"

namespace cotw::game
{
    bool installSenseSuppression(const config::Config& config) noexcept;
    void updateSenseSuppression() noexcept;
}
