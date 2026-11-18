#pragma once

#include <cstddef>
#include <cstdint>

namespace cotw::game
{
    inline constexpr float kOutlineRed = 1.0f;
    inline constexpr float kOutlineGreen = 0.28f;
    inline constexpr float kOutlineBlue = 0.0f;
    inline constexpr float kOutlineAlpha = 0.20f;

    [[nodiscard]] bool isAlignedUserAddress(
        std::uintptr_t address,
        std::size_t alignment) noexcept;
    [[nodiscard]] bool isPointInHighlightVolume(
        float worldX,
        float worldY,
        float worldZ,
        float extraMargin) noexcept;
    void enforceRenderableOutlineBytes(std::uintptr_t renderableAddress) noexcept;
    void registerHighlightedRenderable(std::uintptr_t renderableAddress) noexcept;
    void unregisterHighlightedRenderable(std::uintptr_t renderableAddress) noexcept;
    [[nodiscard]] bool installOutlineCullingHook() noexcept;
}
