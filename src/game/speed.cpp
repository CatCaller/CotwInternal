#include "game/speed.hpp"

#include <Windows.h>

#include <atomic>

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
        using CharacterStepFunction = std::uintptr_t(__fastcall*)(void*, float, void*);
        using DesiredVelocityTransformFunction =
            float* (__fastcall*)(const void*, float*, const float*);

        CharacterStepFunction originalCharacterStep = nullptr;
        DesiredVelocityTransformFunction originalDesiredVelocityTransform = nullptr;
        std::uintptr_t playerAddress = 0;
        std::uintptr_t pawnAddress = 0;
        std::atomic<std::uintptr_t> localPhysicsAddress{0};

        unsigned long long lastPlayerScan = 0;
        unsigned long long lastPlayerVerify = 0;
        unsigned long long lastPlayerLog = 0;
        bool hookInstalled = false;
        std::atomic<bool> desiredSpeedActiveLogged{false};
        std::atomic<bool> stepMatchedLogged{false};
        std::atomic<bool> stepActiveLogged{false};

        float* __fastcall desiredVelocityTransformDetour(
            const void* matrix,
            float* outVelocity,
            const float* localVelocity) noexcept
        {
            if (originalDesiredVelocityTransform == nullptr)
            {
                return outVelocity;
            }

            float* result = originalDesiredVelocityTransform(matrix, outVelocity, localVelocity);
            const float multiplier = application::features.speedMultiplier;
            if (application::features.speedEnabled
                && multiplier > 1.0001f
                && outVelocity != nullptr
                && platform::isWritable(outVelocity, sizeof(float) * 3))
            {
                if (!desiredSpeedActiveLogged.exchange(true, std::memory_order_relaxed))
                {
                    platform::writeLogLine("First local desired velocity transform active");
                }
                outVelocity[0] *= multiplier;
                outVelocity[2] *= multiplier;
            }

            return result;
        }

        std::uintptr_t __fastcall characterStepDetour(
            void* physics,
            const float deltaTime,
            void* input) noexcept
        {
            if (originalCharacterStep == nullptr)
            {
                return 0;
            }

            const auto physicsAddress = reinterpret_cast<std::uintptr_t>(physics);
            const std::uintptr_t trackedPhysics = localPhysicsAddress.load(
                std::memory_order_acquire);
            const float multiplier = application::features.speedMultiplier;

            if (trackedPhysics != 0
                && physicsAddress == trackedPhysics
                && !stepMatchedLogged.exchange(true, std::memory_order_relaxed))
            {
                platform::writeLogLine("First local character step matched");
            }

            if (application::features.speedEnabled
                && multiplier > 1.0001f
                && trackedPhysics != 0
                && physicsAddress == trackedPhysics)
            {
                if (!stepActiveLogged.exchange(true, std::memory_order_relaxed))
                {
                    platform::writeLogLine("First local character step active");
                }

                float* maxVelocityPtr = nullptr;
                float originalMaxVelocity = 0.0f;
                std::uintptr_t contextAddress = 0;
                if (readPointer(physicsAddress + kPhysicsContextOffset, contextAddress)
                    && contextAddress != 0)
                {
                    const auto clampEnableAddress = reinterpret_cast<const void*>(
                        contextAddress + kPhysicsContextClampEnableOffset);
                    unsigned char clampEnabled = 0;
                    if (platform::readBytes(
                            clampEnableAddress,
                            &clampEnabled,
                            sizeof(clampEnabled))
                        && clampEnabled != 0)
                    {
                        auto* candidate = reinterpret_cast<float*>(
                            contextAddress + kPhysicsContextMaxVelocityOffset);
                        if (platform::isWritable(candidate, sizeof(float)))
                        {
                            maxVelocityPtr = candidate;
                            originalMaxVelocity = *candidate;
                            *candidate = originalMaxVelocity * multiplier;
                        }
                    }
                }

                const std::uintptr_t result =
                    originalCharacterStep(physics, deltaTime, input);

                if (maxVelocityPtr != nullptr)
                {
                    *maxVelocityPtr = originalMaxVelocity;
                }

                return result;
            }

            return originalCharacterStep(physics, deltaTime, input);
        }

        void refreshPlayer() noexcept
        {
            const unsigned long long now = GetTickCount64();
            if (now - lastPlayerScan < 2000)
            {
                return;
            }

            lastPlayerScan = now;
            playerAddress = findPlayer();
            if (playerAddress == 0)
            {
                pawnAddress = 0;
                localPhysicsAddress.store(0, std::memory_order_release);
                return;
            }

            pawnAddress = playerPawn(playerAddress);
            if (pawnAddress == 0)
            {
                playerAddress = 0;
                localPhysicsAddress.store(0, std::memory_order_release);
                return;
            }

            std::uintptr_t physics = 0;
            if (!readPointer(pawnAddress + kPawnPhysicsOffset, physics) || physics == 0)
            {
                localPhysicsAddress.store(0, std::memory_order_release);
                return;
            }

            localPhysicsAddress.store(physics, std::memory_order_release);

            if (now - lastPlayerLog > 5000)
            {
                lastPlayerLog = now;
                platform::TextBuilder builder;
                builder.append("Local player found at ");
                builder.appendHex(playerAddress);
                builder.append(", pawn ");
                builder.appendHex(pawnAddress);
                builder.append(", physics ");
                builder.appendHex(physics);
                platform::writeLogLine(builder.c_str());
            }
        }
    }

    bool initializeSpeed(const config::Config& config) noexcept
    {
        (void)config;
        playerAddress = 0;
        pawnAddress = 0;
        localPhysicsAddress.store(0, std::memory_order_release);
        lastPlayerScan = 0;
        lastPlayerVerify = 0;
        lastPlayerLog = 0;
        hookInstalled = false;

        if (!isAvailable())
        {
            platform::writeLogLine("Speed module unavailable");
            return false;
        }

        desiredSpeedActiveLogged.store(false, std::memory_order_relaxed);
        stepMatchedLogged.store(false, std::memory_order_relaxed);
        stepActiveLogged.store(false, std::memory_order_relaxed);

        const auto desiredTarget = reinterpret_cast<void*>(
            base() + kDesiredVelocityTransformRva);
        originalDesiredVelocityTransform =
            reinterpret_cast<DesiredVelocityTransformFunction>(desiredTarget);
        const auto desiredSite = reinterpret_cast<void*>(
            base() + kDesiredVelocityTransformCallSiteRva);
        const bool desiredHookInstalled = platform::installCallHook(
            desiredSite,
            reinterpret_cast<void*>(&desiredVelocityTransformDetour),
            desiredTarget);

        const auto targetFunction = reinterpret_cast<void*>(
            base() + kCharacterStepFunctionRva);
        originalCharacterStep = reinterpret_cast<CharacterStepFunction>(targetFunction);

        std::size_t stepHooksInstalled = 0;
        for (const std::uintptr_t siteRva : kCharacterStepCallSites)
        {
            const auto site = reinterpret_cast<void*>(base() + siteRva);
            if (platform::installCallHook(
                    site,
                    reinterpret_cast<void*>(&characterStepDetour),
                    targetFunction))
            {
                ++stepHooksInstalled;
            }
        }

        const std::size_t installedCount =
            stepHooksInstalled + (desiredHookInstalled ? 1u : 0u);
        const std::size_t totalHooks =
            (sizeof(kCharacterStepCallSites) / sizeof(kCharacterStepCallSites[0])) + 1u;

        hookInstalled = desiredHookInstalled && stepHooksInstalled > 0;

        platform::TextBuilder builder;
        builder.append("Speed module initialized (");
        builder.appendUnsigned(installedCount);
        builder.append("/");
        builder.appendUnsigned(totalHooks);
        builder.append(" hooks)");
        platform::writeLogLine(builder.c_str());
        return hookInstalled;
    }

    void updateSpeed() noexcept
    {
        if (!isAvailable() || !hookInstalled)
        {
            return;
        }

        const unsigned long long now = GetTickCount64();
        if (playerAddress != 0 && pawnAddress != 0 && now - lastPlayerVerify >= 100)
        {
            lastPlayerVerify = now;
            std::uintptr_t vtable = 0;
            if (!readPointer(playerAddress, vtable) || vtable != base() + kPlayerVtableRva)
            {
                playerAddress = 0;
                pawnAddress = 0;
                localPhysicsAddress.store(0, std::memory_order_release);
            }
            else
            {
                std::uintptr_t physics = 0;
                if (!readPointer(pawnAddress + kPawnPhysicsOffset, physics) || physics == 0)
                {
                    localPhysicsAddress.store(0, std::memory_order_release);
                }
                else
                {
                    localPhysicsAddress.store(physics, std::memory_order_release);
                }
            }
        }

        if (playerAddress == 0
            || pawnAddress == 0
            || localPhysicsAddress.load(std::memory_order_acquire) == 0)
        {
            refreshPlayer();
        }
    }
}
