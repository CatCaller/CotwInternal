#pragma once

#include <cstdint>

namespace cotw::game
{
    [[nodiscard]] bool installAnimalOutlines() noexcept;
    [[nodiscard]] bool resolveAnimalFromCharacter(
        std::uintptr_t characterAddress,
        std::uintptr_t& animalAddress) noexcept;
    [[nodiscard]] bool isAnimalAlive(std::uintptr_t animalAddress) noexcept;
    [[nodiscard]] bool readCharacterWorldCenter(
        std::uintptr_t characterAddress,
        float& worldX,
        float& worldY,
        float& worldZ) noexcept;
    void updateCullingSlotIgnoreSize(
        std::uintptr_t renderableAddress,
        bool enableIgnoreSize) noexcept;
}
