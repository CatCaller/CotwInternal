#include "game/game_memory.hpp"

#include <Windows.h>

#include "game/game_offsets.hpp"
#include "platform/memory.hpp"

namespace cotw::game
{
    namespace
    {
        std::uintptr_t moduleBase = 0;
        std::uintptr_t cachedPlayer = 0;

        bool isFinite(const float value) noexcept
        {
            std::uint32_t bits = 0;
            platform::copyBytes(&bits, &value, sizeof(bits));
            return (bits & 0x7F800000u) != 0x7F800000u;
        }

        bool validatePawn(const std::uintptr_t pawn, const std::uintptr_t player) noexcept
        {
            if (pawn == 0 || !platform::isReadable(reinterpret_cast<const void*>(pawn), 0x2B00))
            {
                return false;
            }

            std::uintptr_t component = 0;
            if (!readPointer(pawn + kPawnAnimationComponentOffset, component) || component == 0)
            {
                return false;
            }

            std::uintptr_t physics = 0;
            if (!readPointer(pawn + kPawnPhysicsOffset, physics) || physics == 0)
            {
                return false;
            }

            std::uintptr_t owner = 0;
            if (!readPointer(pawn + kPawnOwnerOffset, owner))
            {
                return false;
            }

            return owner == player + kPlayerOwnerBias;
        }
    }

    bool initialize() noexcept
    {
        const HMODULE module = GetModuleHandleA("theHunterCotW_F.exe");
        if (module == nullptr)
        {
            return false;
        }

        moduleBase = reinterpret_cast<std::uintptr_t>(module);
        return moduleBase != 0;
    }

    bool isAvailable() noexcept
    {
        return moduleBase != 0;
    }

    std::uintptr_t base() noexcept
    {
        return moduleBase;
    }

    bool readFloat(const std::uintptr_t address, float& value) noexcept
    {
        return platform::readBytes(reinterpret_cast<const void*>(address), &value, sizeof(value));
    }

    bool readPointer(const std::uintptr_t address, std::uintptr_t& value) noexcept
    {
        return platform::readBytes(reinterpret_cast<const void*>(address), &value, sizeof(value));
    }

    bool readInteger(const std::uintptr_t address, std::int32_t& value) noexcept
    {
        return platform::readBytes(reinterpret_cast<const void*>(address), &value, sizeof(value));
    }

    bool readViewProjection(float (&matrix)[16]) noexcept
    {
        if (!isAvailable())
        {
            return false;
        }

        if (!platform::readBytes(
                reinterpret_cast<const void*>(moduleBase + kViewProjectionRva),
                matrix,
                sizeof(matrix)))
        {
            return false;
        }

        bool hasNonZero = false;
        for (std::size_t index = 0; index < 16; ++index)
        {
            if (!isFinite(matrix[index]))
            {
                return false;
            }

            const float absolute = matrix[index] < 0.0f ? -matrix[index] : matrix[index];
            if (absolute > 1.0e8f)
            {
                return false;
            }

            if (absolute > 1.0e-6f)
            {
                hasNonZero = true;
            }
        }

        return hasNonZero;
    }

    bool readCameraPosition(float (&position)[3]) noexcept
    {
        if (!isAvailable())
        {
            return false;
        }

        if (!platform::readBytes(
                reinterpret_cast<const void*>(moduleBase + kCameraPositionRva),
                position,
                sizeof(position)))
        {
            return false;
        }

        return isFinite(position[0]) && isFinite(position[1]) && isFinite(position[2]);
    }

    std::uintptr_t findPlayer() noexcept
    {
        if (!isAvailable())
        {
            return 0;
        }

        if (cachedPlayer != 0 && validatePlayer(cachedPlayer))
        {
            return cachedPlayer;
        }

        cachedPlayer = 0;

        std::uintptr_t playerManager = 0;
        if (!readPointer(moduleBase + kPlayerManagerRva, playerManager) || playerManager == 0)
        {
            return 0;
        }

        std::uintptr_t entry = 0;
        if (!readPointer(playerManager + kPlayerManagerEntryOffset, entry) || entry == 0)
        {
            return 0;
        }

        std::uintptr_t owner = 0;
        if (!readPointer(entry + kPlayerManagerOwnerOffset, owner) || owner <= kPlayerOwnerBias)
        {
            return 0;
        }

        const std::uintptr_t candidate = owner - kPlayerOwnerBias;
        if (!validatePlayer(candidate))
        {
            return 0;
        }

        cachedPlayer = candidate;
        return cachedPlayer;
    }

    bool validatePlayer(const std::uintptr_t player) noexcept
    {
        if (!isAvailable() || player == 0)
        {
            return false;
        }

        if (!platform::isReadable(reinterpret_cast<const void*>(player), 0x2710))
        {
            return false;
        }

        std::uintptr_t vtable = 0;
        if (!readPointer(player, vtable) || vtable != moduleBase + kPlayerVtableRva)
        {
            return false;
        }

        const std::uintptr_t pawn = playerPawn(player);
        return pawn != 0 && validatePawn(pawn, player);
    }

    std::uintptr_t playerPawn(const std::uintptr_t player) noexcept
    {
        if (player == 0)
        {
            return 0;
        }

        std::uintptr_t pawn = 0;
        if (readPointer(player + kPlayerPawnOffset, pawn) && validatePawn(pawn, player))
        {
            return pawn;
        }

        return 0;
    }
}
