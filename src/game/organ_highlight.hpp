#pragma once

#include <cstdint>

namespace cotw::game
{
    [[nodiscard]] bool installOrganHighlight() noexcept;
    void updateOrganHighlightForAnimal(
        std::uintptr_t animalAddress,
        std::uintptr_t characterAddress,
        std::uintptr_t updateContextAddress,
        bool isAlive) noexcept;
}
