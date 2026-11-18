#include "game/senses.hpp"

#include <Windows.h>

#include "application/feature_state.hpp"
#include "game/game_memory.hpp"
#include "game/game_offsets.hpp"
#include "platform/hook.hpp"
#include "platform/log.hpp"
#include "platform/memory.hpp"
#include "platform/text.hpp"

namespace cotw::game
{
    namespace
    {
        using SenseCopyFunction = void* (__fastcall*)(void*, void*, void*);

        constexpr std::uintptr_t kSenseManagerVtableRva = 0x1A4F6C0;
        constexpr std::size_t kSenseManagerSize = 2248;
        constexpr std::size_t kSenseManagerSentinelOffset = 0x20;
        constexpr std::size_t kSenseManagerCountOffset = 0x28;
        constexpr std::size_t kSenseEntryNextOffset = 0x00;
        constexpr std::size_t kSenseEntryKeyOffset = 0x10;
        constexpr std::size_t kSenseEntryConfigOffset = 20;

        SenseCopyFunction originalSenseCopy = nullptr;
        bool patchLogged = false;
        bool managerPatchLogged = false;
        std::uintptr_t managerAddress = 0;

        std::uintptr_t resolveSenseManager() noexcept
        {
            if (!isAvailable())
            {
                return 0;
            }

            std::uintptr_t senseWorld = 0;
            if (!readPointer(base() + kSenseSystemWorldRva, senseWorld) || senseWorld == 0)
            {
                return 0;
            }

            std::uintptr_t aiSystemInner = 0;
            if (!readPointer(senseWorld + kSenseSystemOwnerOffset, aiSystemInner)
                || aiSystemInner <= kSenseSystemOwnerBias)
            {
                return 0;
            }

            const std::uintptr_t aiSystem = aiSystemInner - kSenseSystemOwnerBias;
            std::uintptr_t candidate = 0;
            if (!readPointer(aiSystem + kSenseSystemManagerOffset, candidate) || candidate == 0)
            {
                return 0;
            }

            if (!platform::isReadable(reinterpret_cast<const void*>(candidate), kSenseManagerSize))
            {
                return 0;
            }

            std::uintptr_t vtable = 0;
            if (!readPointer(candidate, vtable) || vtable != base() + kSenseManagerVtableRva)
            {
                return 0;
            }

            return candidate;
        }

        void zeroSenses(void* configuration) noexcept
        {
            if (configuration == nullptr)
            {
                return;
            }

            auto& state = application::features;
            if (!state.noSensesEnabled && !state.noSmellEnabled)
            {
                return;
            }

            const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(configuration);
            const std::size_t start = state.noSensesEnabled ? kSensesStartOffset : kScentStartOffset;
            const std::size_t end = state.noSensesEnabled ? kSensesEndOffset : kScentEndOffset;
            if (end <= start)
            {
                return;
            }

            void* target = reinterpret_cast<void*>(address + start);
            const std::size_t size = end - start;
            if (!platform::isWritable(target, size))
            {
                return;
            }

            platform::zeroBytes(target, size);
            if (!patchLogged)
            {
                patchLogged = true;
                platform::TextBuilder builder;
                builder.append("Animal sense configuration patched at ");
                builder.appendHex(address);
                builder.append(" (");
                builder.appendUnsigned(size);
                builder.append(" bytes)");
                platform::writeLogLine(builder.c_str());
            }
        }

        bool configurationLooksSane(const std::uintptr_t configuration) noexcept
        {
            if (!platform::isReadable(
                    reinterpret_cast<const void*>(configuration + kSensesStartOffset),
                    kSensesEndOffset - kSensesStartOffset))
            {
                return false;
            }

            float samples[3]{};
            if (!readFloat(configuration + kSensesStartOffset, samples[0])
                || !readFloat(configuration + kScentStartOffset, samples[1])
                || !readFloat(configuration + 576, samples[2]))
            {
                return false;
            }

            for (const float sample : samples)
            {
                if (sample != sample || sample > 1.0e6f || sample < -1.0e6f)
                {
                    return false;
                }
            }

            return true;
        }

        void patchExistingConfigurations() noexcept
        {
            if (managerAddress != 0)
            {
                std::uintptr_t managerVtable = 0;
                if (!readPointer(managerAddress, managerVtable)
                    || managerVtable != base() + kSenseManagerVtableRva)
                {
                    managerAddress = 0;
                }
            }

            if (managerAddress == 0)
            {
                managerAddress = resolveSenseManager();
                if (managerAddress != 0)
                {
                    platform::TextBuilder builder;
                    builder.append("Sense manager found at ");
                    builder.appendHex(managerAddress);
                    platform::writeLogLine(builder.c_str());
                }
            }

            if (managerAddress == 0)
            {
                return;
            }

            std::uintptr_t sentinel = 0;
            std::uintptr_t count = 0;
            if (!readPointer(managerAddress + kSenseManagerSentinelOffset, sentinel)
                || !readPointer(managerAddress + kSenseManagerCountOffset, count)
                || sentinel == 0
                || count == 0
                || count > 4096)
            {
                return;
            }

            std::uintptr_t entry = 0;
            if (!readPointer(sentinel + kSenseEntryNextOffset, entry))
            {
                return;
            }

            std::size_t visited = 0;
            std::size_t patched = 0;
            while (entry != 0 && entry != sentinel && visited < 4096)
            {
                std::int32_t entryHash = 0;
                if (!readInteger(entry + kSenseEntryKeyOffset, entryHash) || entryHash == 0)
                {
                    break;
                }

                const std::uintptr_t configuration = entry + kSenseEntryConfigOffset;
                if (configurationLooksSane(configuration))
                {
                    zeroSenses(reinterpret_cast<void*>(configuration));
                    ++patched;
                }

                std::uintptr_t next = 0;
                if (!readPointer(entry + kSenseEntryNextOffset, next) || next == entry)
                {
                    break;
                }

                entry = next;
                ++visited;
            }

            if (!managerPatchLogged && patched > 0)
            {
                managerPatchLogged = true;
                platform::TextBuilder builder;
                builder.append("Existing animal sense configurations patched (");
                builder.appendUnsigned(patched);
                builder.append("/");
                builder.appendUnsigned(visited);
                builder.append(")");
                platform::writeLogLine(builder.c_str());
            }
        }

        void* __fastcall senseCopyDetour(void* first, void* configuration, void* third) noexcept
        {
            void* result = originalSenseCopy(first, configuration, third);
            zeroSenses(configuration);
            return result;
        }
    }

    bool installSenseSuppression(const config::Config& config) noexcept
    {
        (void)config;
        if (!isAvailable())
        {
            return false;
        }

        const auto functionTarget = reinterpret_cast<void*>(base() + kSenseCopyFunctionRva);
        const auto callSite = reinterpret_cast<void*>(base() + kSenseCopyCallSiteRva);
        originalSenseCopy = reinterpret_cast<SenseCopyFunction>(functionTarget);

        if (!platform::installCallHook(callSite, reinterpret_cast<void*>(&senseCopyDetour), functionTarget))
        {
            platform::writeLogLine("Sense suppression hook was not installed");
            return false;
        }

        platform::writeLogLine("Sense suppression hook installed");
        return true;
    }

    void updateSenseSuppression() noexcept
    {
        patchExistingConfigurations();
    }
}
