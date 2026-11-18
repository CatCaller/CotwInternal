#pragma once

#include "config/config.hpp"

namespace cotw::game
{
    bool initializeSpeed(const config::Config& config) noexcept;
    void updateSpeed() noexcept;
}
