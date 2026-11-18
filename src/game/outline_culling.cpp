#include "game/outline_culling.hpp"

#include "application/feature_state.hpp"
#include "game/game_memory.hpp"
#include "game/game_offsets.hpp"
#include "platform/hook.hpp"

#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace cotw::game
{
    namespace
    {
        using DynamicSceneCallbackFunction = char(__fastcall*)(
            std::uintptr_t,
            int,
            std::uintptr_t,
            std::uintptr_t*);
        using CullDynamicSceneFunction = std::uintptr_t(__fastcall*)(
            std::uintptr_t,
            std::uint32_t,
            const std::uint32_t*,
            std::uintptr_t,
            const std::uintptr_t*,
            std::uint16_t,
            std::uintptr_t,
            DynamicSceneCallbackFunction,
            std::uintptr_t);

        class SharedSrwGuard
        {
        public:
            explicit SharedSrwGuard(SRWLOCK& lock) noexcept
                : lock_(lock)
            {
                AcquireSRWLockShared(&lock_);
            }

            ~SharedSrwGuard() noexcept
            {
                ReleaseSRWLockShared(&lock_);
            }

            SharedSrwGuard(const SharedSrwGuard&) = delete;
            SharedSrwGuard& operator=(const SharedSrwGuard&) = delete;
            SharedSrwGuard(SharedSrwGuard&&) = delete;
            SharedSrwGuard& operator=(SharedSrwGuard&&) = delete;

        private:
            SRWLOCK& lock_;
        };

        class ExclusiveSrwGuard
        {
        public:
            explicit ExclusiveSrwGuard(SRWLOCK& lock) noexcept
                : lock_(lock)
            {
                AcquireSRWLockExclusive(&lock_);
            }

            ~ExclusiveSrwGuard() noexcept
            {
                ReleaseSRWLockExclusive(&lock_);
            }

            ExclusiveSrwGuard(const ExclusiveSrwGuard&) = delete;
            ExclusiveSrwGuard& operator=(const ExclusiveSrwGuard&) = delete;
            ExclusiveSrwGuard(ExclusiveSrwGuard&&) = delete;
            ExclusiveSrwGuard& operator=(ExclusiveSrwGuard&&) = delete;

        private:
            SRWLOCK& lock_;
        };

        class PassBusyGuard
        {
        public:
            explicit PassBusyGuard(std::atomic<bool>& busyFlag) noexcept
                : busyFlag_(busyFlag)
                , acquired_(!busyFlag_.exchange(true, std::memory_order_acquire))
            {
            }

            ~PassBusyGuard() noexcept
            {
                if (acquired_)
                {
                    busyFlag_.store(false, std::memory_order_release);
                }
            }

            [[nodiscard]] bool isAcquired() const noexcept
            {
                return acquired_;
            }

            PassBusyGuard(const PassBusyGuard&) = delete;
            PassBusyGuard& operator=(const PassBusyGuard&) = delete;
            PassBusyGuard(PassBusyGuard&&) = delete;
            PassBusyGuard& operator=(PassBusyGuard&&) = delete;

        private:
            std::atomic<bool>& busyFlag_;
            bool acquired_;
        };

        constexpr std::size_t kRegistryCapacity = 1024;
        constexpr std::size_t kInvalidSlot = kRegistryCapacity;
        constexpr std::uintptr_t kEmptySlot = 0;
        constexpr std::uint32_t kStaleEntryFrameAge = 300u;
        constexpr std::size_t kMaxTrackedRenderables = 512;

        constexpr std::uint8_t kPackedRed =
            static_cast<std::uint8_t>(kOutlineAlpha * kOutlineRed * 255.0f);
        constexpr std::uint8_t kPackedGreen =
            static_cast<std::uint8_t>(kOutlineAlpha * kOutlineGreen * 255.0f);
        constexpr std::uint8_t kPackedBlue =
            static_cast<std::uint8_t>(kOutlineAlpha * kOutlineBlue * 255.0f);
        constexpr std::uint8_t kPackedAlpha =
            static_cast<std::uint8_t>(kOutlineAlpha * kOutlineAlpha * 255.0f);

        CullDynamicSceneFunction originalCullDynamicScene = nullptr;
        DynamicSceneCallbackFunction originalDynamicSceneCallback = nullptr;

        SRWLOCK registryLock = SRWLOCK_INIT;
        std::uintptr_t registryAddresses[kRegistryCapacity]{};
        std::atomic<std::uint32_t> registryLastSeen[kRegistryCapacity]{};
        std::atomic<std::size_t> activeRegistryEntries{0};
        std::atomic<std::uint32_t> cullFrameCounter{1u};

        std::atomic<bool> cullPassBusy{false};
        std::uintptr_t trackedRenderables[kMaxTrackedRenderables]{};
        volatile long trackedVisited[kMaxTrackedRenderables]{};
        std::size_t trackedCount = 0;
        std::uintptr_t occludedPairs[kMaxTrackedRenderables * 2u]{};

        float absoluteValue(const float value) noexcept
        {
            return value < 0.0f ? -value : value;
        }

        float clampMaximumDistance(const float distance) noexcept
        {
            if (distance < 0.0f)
            {
                return 0.0f;
            }
            if (distance > 2000.0f)
            {
                return 2000.0f;
            }
            return distance;
        }

        std::size_t hashAddress(const std::uintptr_t address) noexcept
        {
            const std::uint64_t mixed = (static_cast<std::uint64_t>(address) >> 4u)
                * 0x9E3779B97F4A7C15ull;
            return static_cast<std::size_t>(mixed);
        }

        std::size_t probeRegistrySlot(const std::uintptr_t address) noexcept
        {
            constexpr std::size_t kMask = kRegistryCapacity - 1u;
            std::size_t slot = hashAddress(address) & kMask;

            for (std::size_t step = 0; step < kRegistryCapacity; ++step)
            {
                const std::uintptr_t entry = registryAddresses[slot];
                if (entry == address)
                {
                    return slot;
                }
                if (entry == kEmptySlot)
                {
                    return kInvalidSlot;
                }

                slot = (slot + 1u) & kMask;
            }

            return kInvalidSlot;
        }

        bool isRenderableBoundsInHighlightVolume(const std::uintptr_t renderableAddress) noexcept
        {
            const auto* bounds = reinterpret_cast<const float*>(
                renderableAddress + kRenderableWorldBoundsOffset);
            if (bounds[3] < bounds[0] || bounds[4] < bounds[1] || bounds[5] < bounds[2])
            {
                return false;
            }

            const float centerX = (bounds[0] + bounds[3]) * 0.5f;
            const float centerY = (bounds[1] + bounds[4]) * 0.5f;
            const float centerZ = (bounds[2] + bounds[5]) * 0.5f;
            const float halfExtentSum = (bounds[3] - bounds[0]) * 0.5f
                + (bounds[4] - bounds[1]) * 0.5f
                + (bounds[5] - bounds[2]) * 0.5f;

            return isPointInHighlightVolume(
                centerX,
                centerY,
                centerZ,
                halfExtentSum + 2.0f);
        }

        void enforceTrackedRenderableState(const std::uintptr_t renderableAddress) noexcept
        {
            auto* flagsPtr = reinterpret_cast<volatile long*>(
                renderableAddress + kRenderableFlagsOffset);
            const std::uint32_t currentFlags = static_cast<std::uint32_t>(*flagsPtr);

            constexpr std::uint32_t kRequiredBits =
                kRenderableOutlineActiveBit
                | kRenderableIgnoreSizeCullBit
                | kRenderableDisableDitherBit;
            if ((currentFlags & kRequiredBits) != kRequiredBits)
            {
                _InterlockedOr(flagsPtr, static_cast<long>(kRequiredBits));
            }

            constexpr std::uint32_t kClearedBits =
                kRenderableMaxDistanceCullBit | kRenderableSurfaceGlowBit;
            if ((currentFlags & kClearedBits) != 0u)
            {
                _InterlockedAnd(flagsPtr, ~static_cast<long>(kClearedBits));
            }

            enforceRenderableOutlineBytes(renderableAddress);

            const std::uint32_t updatedFlags = static_cast<std::uint32_t>(*flagsPtr);
            if ((updatedFlags & kRenderableRegisteredInSceneBit) != 0u
                && trackedCount < kMaxTrackedRenderables)
            {
                trackedRenderables[trackedCount] = renderableAddress;
                trackedVisited[trackedCount] = 0;
                ++trackedCount;
            }
        }

        void collectTrackedRenderables(
            const std::uintptr_t cullingWorld,
            const std::uint32_t currentFrame) noexcept
        {
            trackedCount = 0;
            if (activeRegistryEntries.load(std::memory_order_acquire) == 0)
            {
                return;
            }

            if (!isAlignedUserAddress(cullingWorld, alignof(std::uintptr_t)))
            {
                return;
            }

            const auto aabbBegin = *reinterpret_cast<const std::uintptr_t*>(
                cullingWorld + kCullingWorldAabbBeginOffset);
            const auto aabbEnd = *reinterpret_cast<const std::uintptr_t*>(
                cullingWorld + kCullingWorldAabbEndOffset);
            const auto userDataAddress = *reinterpret_cast<const std::uintptr_t*>(
                cullingWorld + kCullingWorldUserDataOffset);
            const auto slotFlagsAddress = *reinterpret_cast<const std::uintptr_t*>(
                cullingWorld + kCullingWorldFlagsOffset);
            if (!isAlignedUserAddress(aabbBegin, alignof(float))
                || aabbEnd <= aabbBegin
                || !isAlignedUserAddress(userDataAddress, alignof(std::uintptr_t))
                || !isAlignedUserAddress(slotFlagsAddress, alignof(std::uint32_t)))
            {
                return;
            }

            const std::size_t slotCount = (aabbEnd - aabbBegin) / kCullingWorldAabbStride;
            const auto* userData = reinterpret_cast<const std::uintptr_t*>(userDataAddress);
            const auto* slotFlags = reinterpret_cast<const std::uint32_t*>(slotFlagsAddress);

            constexpr std::uint32_t kActiveMask =
                kCullingSlotIgnoreSizeBit | kCullingSlotDisabledBit;
            const SharedSrwGuard sharedLock(registryLock);
            for (std::size_t slot = 0; slot < slotCount; ++slot)
            {
                if ((slotFlags[slot] & kActiveMask) != kCullingSlotIgnoreSizeBit)
                {
                    continue;
                }

                const std::uintptr_t renderableAddress = userData[slot];
                if (!isAlignedUserAddress(renderableAddress, alignof(std::uintptr_t)))
                {
                    continue;
                }

                const std::size_t registrySlot = probeRegistrySlot(renderableAddress);
                if (registrySlot == kInvalidSlot)
                {
                    continue;
                }

                registryLastSeen[registrySlot].store(currentFrame, std::memory_order_relaxed);
                if (!isRenderableBoundsInHighlightVolume(renderableAddress))
                {
                    continue;
                }

                enforceTrackedRenderableState(renderableAddress);
            }
        }

        char __fastcall cullDynamicSceneCallback(
            const std::uintptr_t cullingWorld,
            const int count,
            const std::uintptr_t userValue,
            std::uintptr_t* pairs) noexcept
        {
            if (count > 0 && pairs != nullptr && trackedCount > 0)
            {
                const std::size_t pairCount = static_cast<std::size_t>(count);
                for (std::size_t index = 0; index < pairCount; ++index)
                {
                    const std::uintptr_t renderableAddress = pairs[index * 2u];
                    if (!isAlignedUserAddress(renderableAddress, alignof(std::uintptr_t)))
                    {
                        continue;
                    }

                    const auto flags = *reinterpret_cast<const std::uint32_t*>(
                        renderableAddress + kRenderableFlagsOffset);
                    if ((flags & kRenderableOutlineActiveBit) == 0u)
                    {
                        continue;
                    }

                    for (std::size_t tracked = 0; tracked < trackedCount; ++tracked)
                    {
                        if (trackedRenderables[tracked] == renderableAddress)
                        {
                            _InterlockedExchange(&trackedVisited[tracked], 1);
                            break;
                        }
                    }
                }
            }

            return originalDynamicSceneCallback(cullingWorld, count, userValue, pairs);
        }

        void submitOccludedRenderables(const std::uintptr_t cullingWorld) noexcept
        {
            std::size_t occludedCount = 0;
            for (std::size_t tracked = 0; tracked < trackedCount; ++tracked)
            {
                if (trackedVisited[tracked] == 0)
                {
                    occludedPairs[occludedCount * 2u] = trackedRenderables[tracked];
                    occludedPairs[occludedCount * 2u + 1u] = 0;
                    ++occludedCount;
                }
            }

            if (occludedCount > 0)
            {
                originalDynamicSceneCallback(
                    cullingWorld,
                    static_cast<int>(occludedCount),
                    0,
                    occludedPairs);
            }

            trackedCount = 0;
        }

        std::uintptr_t __fastcall cullDynamicSceneDetour(
            const std::uintptr_t cullingWorld,
            const std::uint32_t filterCount,
            const std::uint32_t* filterTypes,
            const std::uintptr_t filterData,
            const std::uintptr_t* filterMasks,
            const std::uint16_t flags,
            const std::uintptr_t scene,
            const DynamicSceneCallbackFunction callback,
            const std::uintptr_t statsOut) noexcept
        {
            if (originalCullDynamicScene == nullptr)
            {
                return 0;
            }

            if (originalDynamicSceneCallback == nullptr || callback != originalDynamicSceneCallback)
            {
                return originalCullDynamicScene(
                    cullingWorld,
                    filterCount,
                    filterTypes,
                    filterData,
                    filterMasks,
                    flags,
                    scene,
                    callback,
                    statsOut);
            }

            const PassBusyGuard busyGuard(cullPassBusy);
            if (!busyGuard.isAcquired())
            {
                return originalCullDynamicScene(
                    cullingWorld,
                    filterCount,
                    filterTypes,
                    filterData,
                    filterMasks,
                    flags,
                    scene,
                    callback,
                    statsOut);
            }

            const std::uint32_t currentFrame =
                cullFrameCounter.fetch_add(1u, std::memory_order_relaxed) + 1u;
            collectTrackedRenderables(cullingWorld, currentFrame);

            const std::uintptr_t result = originalCullDynamicScene(
                cullingWorld,
                filterCount,
                filterTypes,
                filterData,
                filterMasks,
                flags,
                scene,
                &cullDynamicSceneCallback,
                statsOut);

            submitOccludedRenderables(cullingWorld);
            return result;
        }
    }

    bool isAlignedUserAddress(
        const std::uintptr_t address,
        const std::size_t alignment) noexcept
    {
        return address >= 0x10000ull
            && address <= 0x00007FFFFFFEFFFFull
            && (address & (alignment - 1u)) == 0;
    }

    bool isPointInHighlightVolume(
        const float worldX,
        const float worldY,
        const float worldZ,
        const float extraMargin) noexcept
    {
        const std::uintptr_t moduleBase = base();
        if (moduleBase == 0)
        {
            return false;
        }

        const float maximumDistance = clampMaximumDistance(
            application::features.highlightDistance);
        if (maximumDistance <= 0.0f)
        {
            return false;
        }

        const float effectiveDistance = maximumDistance + extraMargin;
        const auto* camera = reinterpret_cast<const float*>(moduleBase + kCameraPositionRva);
        const float deltaX = worldX - camera[0];
        const float deltaY = worldY - camera[1];
        const float deltaZ = worldZ - camera[2];
        const float distanceSquared = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
        if (distanceSquared > effectiveDistance * effectiveDistance)
        {
            return false;
        }

        if (distanceSquared <= 64.0f)
        {
            return true;
        }

        const auto* viewProjection = reinterpret_cast<const float*>(
            moduleBase + kViewProjectionRva);
        const float perspectiveWeight =
            viewProjection[3] * viewProjection[3]
            + viewProjection[7] * viewProjection[7]
            + viewProjection[11] * viewProjection[11];
        if (perspectiveWeight <= 0.1f)
        {
            return true;
        }

        const float clipX = worldX * viewProjection[0]
            + worldY * viewProjection[4]
            + worldZ * viewProjection[8]
            + viewProjection[12];
        const float clipY = worldX * viewProjection[1]
            + worldY * viewProjection[5]
            + worldZ * viewProjection[9]
            + viewProjection[13];
        const float clipW = worldX * viewProjection[3]
            + worldY * viewProjection[7]
            + worldZ * viewProjection[11]
            + viewProjection[15];

        const float frustumMargin = extraMargin < 6.0f ? extraMargin : 6.0f;
        if (clipW <= 0.1f - frustumMargin)
        {
            return false;
        }

        const float positiveW = clipW > 0.0f ? clipW : 0.0f;
        const float clipLimit = positiveW * 1.35f + 3.0f + frustumMargin;
        return absoluteValue(clipX) <= clipLimit && absoluteValue(clipY) <= clipLimit;
    }

    void enforceRenderableOutlineBytes(const std::uintptr_t renderableAddress) noexcept
    {
        if (!isAlignedUserAddress(renderableAddress, alignof(std::uintptr_t)))
        {
            return;
        }

        auto* dirtyFlagPtr = reinterpret_cast<volatile long*>(
            renderableAddress + kRenderableDirtyFlagOffset);
        auto* outlineTypePtr = reinterpret_cast<std::uint8_t*>(
            renderableAddress + kRenderableOutlineTypeOffset);
        auto* rgbaPtr = reinterpret_cast<std::uint8_t*>(
            renderableAddress + kRenderableOutlineRgbaOffset);

        if (*outlineTypePtr != 0u
            || rgbaPtr[0] != kPackedRed
            || rgbaPtr[1] != kPackedGreen
            || rgbaPtr[2] != kPackedBlue
            || rgbaPtr[3] != kPackedAlpha)
        {
            *outlineTypePtr = 0u;
            rgbaPtr[0] = kPackedRed;
            rgbaPtr[1] = kPackedGreen;
            rgbaPtr[2] = kPackedBlue;
            rgbaPtr[3] = kPackedAlpha;
            _InterlockedExchange(dirtyFlagPtr, 1);
        }

        auto* flagsPtr = reinterpret_cast<volatile long*>(
            renderableAddress + kRenderableFlagsOffset);
        const std::uint32_t currentFlags = static_cast<std::uint32_t>(*flagsPtr);
        if ((currentFlags & kRenderableSpawnFadeActiveBit) == 0u)
        {
            if ((currentFlags & kRenderableDitherFadeActiveBit) != 0u)
            {
                _InterlockedAnd(flagsPtr, ~static_cast<long>(kRenderableDitherFadeActiveBit));
            }

            auto* opacityPtr = reinterpret_cast<std::uint8_t*>(
                renderableAddress + kRenderableOpacityByteOffset);
            if (*opacityPtr != 0xFFu)
            {
                *opacityPtr = 0xFFu;
                _InterlockedExchange(dirtyFlagPtr, 1);
            }
        }
    }

    void registerHighlightedRenderable(const std::uintptr_t renderableAddress) noexcept
    {
        if (!isAlignedUserAddress(renderableAddress, alignof(std::uintptr_t)))
        {
            return;
        }

        {
            const SharedSrwGuard sharedLock(registryLock);
            const std::size_t found = probeRegistrySlot(renderableAddress);
            if (found != kInvalidSlot)
            {
                const std::uint32_t frame = cullFrameCounter.load(std::memory_order_relaxed);
                registryLastSeen[found].store(frame, std::memory_order_relaxed);
                return;
            }
        }

        const ExclusiveSrwGuard exclusiveLock(registryLock);
        const std::uint32_t currentFrame = cullFrameCounter.load(std::memory_order_relaxed);
        const std::size_t existing = probeRegistrySlot(renderableAddress);
        if (existing != kInvalidSlot)
        {
            registryLastSeen[existing].store(currentFrame, std::memory_order_relaxed);
            return;
        }

        if (activeRegistryEntries.load(std::memory_order_relaxed) >= kRegistryCapacity - 1u)
        {
            return;
        }

        constexpr std::size_t kMask = kRegistryCapacity - 1u;
        std::size_t slot = hashAddress(renderableAddress) & kMask;
        for (std::size_t step = 0; step < kRegistryCapacity; ++step)
        {
            const std::uintptr_t entry = registryAddresses[slot];
            const std::uint32_t lastSeen = registryLastSeen[slot].load(std::memory_order_relaxed);
            const bool isStale = currentFrame > lastSeen
                && (currentFrame - lastSeen) > kStaleEntryFrameAge;
            if (entry == kEmptySlot || isStale)
            {
                registryAddresses[slot] = renderableAddress;
                registryLastSeen[slot].store(currentFrame, std::memory_order_relaxed);
                if (entry == kEmptySlot)
                {
                    activeRegistryEntries.fetch_add(1u, std::memory_order_relaxed);
                }
                return;
            }

            slot = (slot + 1u) & kMask;
        }
    }

    void unregisterHighlightedRenderable(const std::uintptr_t renderableAddress) noexcept
    {
        if (!isAlignedUserAddress(renderableAddress, alignof(std::uintptr_t)))
        {
            return;
        }

        const ExclusiveSrwGuard exclusiveLock(registryLock);
        const std::size_t found = probeRegistrySlot(renderableAddress);
        if (found == kInvalidSlot)
        {
            return;
        }

        constexpr std::size_t kMask = kRegistryCapacity - 1u;
        std::size_t hole = found;
        std::size_t cursor = (hole + 1u) & kMask;
        while (registryAddresses[cursor] != kEmptySlot)
        {
            const std::size_t home = hashAddress(registryAddresses[cursor]) & kMask;
            const std::size_t distanceFromHome = (cursor - home) & kMask;
            const std::size_t distanceFromHole = (cursor - hole) & kMask;
            if (distanceFromHome >= distanceFromHole)
            {
                registryAddresses[hole] = registryAddresses[cursor];
                registryLastSeen[hole].store(
                    registryLastSeen[cursor].load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                hole = cursor;
            }

            cursor = (cursor + 1u) & kMask;
        }

        registryAddresses[hole] = kEmptySlot;
        registryLastSeen[hole].store(0u, std::memory_order_relaxed);
        activeRegistryEntries.fetch_sub(1u, std::memory_order_relaxed);
    }

    bool installOutlineCullingHook() noexcept
    {
        if (!isAvailable())
        {
            return false;
        }

        const auto cullTarget = reinterpret_cast<void*>(base() + kCullDynamicSceneRva);
        originalCullDynamicScene = reinterpret_cast<CullDynamicSceneFunction>(cullTarget);
        originalDynamicSceneCallback = reinterpret_cast<DynamicSceneCallbackFunction>(
            base() + kDynamicSceneVisibleCallbackRva);

        const auto cullCallSite = reinterpret_cast<void*>(
            base() + kCullDynamicSceneCallSiteRva);
        return platform::installCallHook(
            cullCallSite,
            reinterpret_cast<void*>(&cullDynamicSceneDetour),
            cullTarget);
    }
}
