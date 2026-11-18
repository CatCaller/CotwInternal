#include "platform/hook.hpp"

#include <Windows.h>
#include <intrin.h>

#include <cstdint>

#include "platform/memory.hpp"

namespace cotw::platform
{
    namespace
    {
        bool isWithinRelativeRange(const void* source, const void* destination) noexcept
        {
            const auto sourceAddress = reinterpret_cast<std::intptr_t>(source);
            const auto destinationAddress = reinterpret_cast<std::intptr_t>(destination);
            const std::intptr_t delta = destinationAddress - (sourceAddress + 5);
            return delta >= -0x7FFF0000ll && delta <= 0x7FFF0000ll;
        }

        void* tryAllocatePage(const std::uintptr_t candidate, void* target) noexcept
        {
            void* memory = VirtualAlloc(
                reinterpret_cast<void*>(candidate),
                0x1000,
                MEM_COMMIT | MEM_RESERVE,
                PAGE_READWRITE);
            if (memory == nullptr)
            {
                return nullptr;
            }

            if (isWithinRelativeRange(target, memory))
            {
                return memory;
            }

            VirtualFree(memory, 0, MEM_RELEASE);
            return nullptr;
        }

        void* allocateNear(void* target) noexcept
        {
            if (target == nullptr)
            {
                return nullptr;
            }

            SYSTEM_INFO systemInformation{};
            GetSystemInfo(&systemInformation);
            const std::uintptr_t granularity = systemInformation.dwAllocationGranularity != 0
                ? systemInformation.dwAllocationGranularity
                : 0x10000;

            const auto targetAddress = reinterpret_cast<std::uintptr_t>(target);
            const std::uintptr_t minAllowed = 0x10000;
            const std::uintptr_t maxAllowed = 0x7FFFFFFF0000ull;
            const std::uintptr_t searchStart = targetAddress > 0x7FFF0000ull + minAllowed
                ? targetAddress - 0x7FFF0000ull
                : minAllowed;
            const std::uintptr_t searchEnd = targetAddress + 0x7FFF0000ull > targetAddress
                && targetAddress + 0x7FFF0000ull < maxAllowed
                ? targetAddress + 0x7FFF0000ull
                : maxAllowed;

            std::uintptr_t cursor = searchStart;
            while (cursor < searchEnd)
            {
                MEMORY_BASIC_INFORMATION information{};
                if (VirtualQuery(reinterpret_cast<const void*>(cursor), &information, sizeof(information)) == 0)
                {
                    break;
                }

                const auto regionStart = reinterpret_cast<std::uintptr_t>(information.BaseAddress);
                const auto regionEnd = regionStart + information.RegionSize;
                if (regionEnd <= cursor)
                {
                    break;
                }

                if (information.State == MEM_FREE)
                {
                    std::uintptr_t startCandidate = regionStart > searchStart ? regionStart : searchStart;
                    startCandidate = (startCandidate + granularity - 1) & ~(granularity - 1);
                    const std::uintptr_t limit = regionEnd < searchEnd ? regionEnd : searchEnd;
                    for (std::uintptr_t candidate = startCandidate;
                         candidate + 0x1000 <= limit;
                         candidate += granularity)
                    {
                        void* memory = tryAllocatePage(candidate, target);
                        if (memory != nullptr)
                        {
                            return memory;
                        }
                    }
                }

                cursor = regionEnd;
            }

            return nullptr;
        }

        bool writeAbsoluteJump(void* address, void* destination) noexcept
        {
            unsigned char jump[14]{};
            jump[0] = 0xFF;
            jump[1] = 0x25;
            const auto target = reinterpret_cast<std::uintptr_t>(destination);
            copyBytes(jump + 6, &target, sizeof(target));
            copyBytes(address, jump, sizeof(jump));

            DWORD ignored = 0;
            if (VirtualProtect(address, 0x1000, PAGE_EXECUTE_READ, &ignored) == 0)
            {
                return false;
            }

            FlushInstructionCache(GetCurrentProcess(), address, sizeof(jump));
            return true;
        }
    }

    bool installCallHook(void* site, void* detour, void* expectedTarget) noexcept
    {
        if (site == nullptr || detour == nullptr || !isReadable(site, 8))
        {
            return false;
        }

        const auto* bytes = static_cast<const unsigned char*>(site);
        const unsigned char opcode = bytes[0];
        if (opcode != 0xE8 && opcode != 0xE9)
        {
            return false;
        }

        std::int32_t relative = 0;
        copyBytes(&relative, bytes + 1, sizeof(relative));
        const auto target = reinterpret_cast<void*>(
            reinterpret_cast<std::uintptr_t>(site) + 5 + static_cast<std::intptr_t>(relative));

        if (expectedTarget != nullptr && target != expectedTarget)
        {
            return false;
        }

        void* trampoline = allocateNear(site);
        if (trampoline == nullptr)
        {
            return false;
        }

        if (!writeAbsoluteJump(trampoline, detour))
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        const auto displacement = reinterpret_cast<std::intptr_t>(trampoline)
            - reinterpret_cast<std::intptr_t>(site) - 5;
        if (displacement < -0x7FFF0000ll || displacement > 0x7FFF0000ll)
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        DWORD oldProtection = 0;
        if (VirtualProtect(site, 8, PAGE_EXECUTE_READWRITE, &oldProtection) == 0)
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        LONG64 expectedBlock = 0;
        copyBytes(&expectedBlock, site, sizeof(expectedBlock));

        LONG64 patchedBlock = expectedBlock;
        unsigned char patch[5]{};
        patch[0] = opcode;
        const auto relativeDisplacement = static_cast<std::int32_t>(displacement);
        copyBytes(patch + 1, &relativeDisplacement, sizeof(relativeDisplacement));
        copyBytes(&patchedBlock, patch, sizeof(patch));

        const LONG64 previous = _InterlockedCompareExchange64(
            static_cast<volatile LONG64*>(site),
            patchedBlock,
            expectedBlock);

        DWORD ignored = 0;
        VirtualProtect(site, 8, oldProtection, &ignored);

        if (previous != expectedBlock)
        {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return false;
        }

        FlushInstructionCache(GetCurrentProcess(), site, 5);
        return true;
    }

    bool installVtableHook(void** vtable, const std::size_t index, void* detour, void** original) noexcept
    {
        if (vtable == nullptr || detour == nullptr || !isReadable(vtable + index, sizeof(void*)))
        {
            return false;
        }

        void* current = vtable[index];
        if (current == nullptr)
        {
            return false;
        }

        if (current == detour)
        {
            return true;
        }

        if (original != nullptr)
        {
            *original = current;
            _WriteBarrier();
        }

        DWORD oldProtection = 0;
        if (VirtualProtect(vtable + index, sizeof(void*), PAGE_READWRITE, &oldProtection) == 0)
        {
            return false;
        }

        void* exchanged = InterlockedCompareExchangePointer(
            reinterpret_cast<volatile PVOID*>(vtable + index),
            detour,
            current);

        DWORD ignored = 0;
        VirtualProtect(vtable + index, sizeof(void*), oldProtection, &ignored);

        if (exchanged != current)
        {
            if (original != nullptr)
            {
                *original = exchanged;
            }
            return exchanged == detour;
        }

        return true;
    }
}
