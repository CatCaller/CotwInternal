#pragma once

#include <cstdint>

namespace cotw::game
{
    bool initialize() noexcept;
    bool isAvailable() noexcept;
    std::uintptr_t base() noexcept;

    bool readViewProjection(float (&matrix)[16]) noexcept;
    bool readCameraPosition(float (&position)[3]) noexcept;

    std::uintptr_t findPlayer() noexcept;
    bool validatePlayer(std::uintptr_t player) noexcept;
    std::uintptr_t playerPawn(std::uintptr_t player) noexcept;

    bool readFloat(std::uintptr_t address, float& value) noexcept;
    bool readPointer(std::uintptr_t address, std::uintptr_t& value) noexcept;
    bool readInteger(std::uintptr_t address, std::int32_t& value) noexcept;
}
