#include "game/organ_highlight.hpp"

#include "application/feature_state.hpp"
#include "game/animal_highlight.hpp"
#include "game/game_memory.hpp"
#include "game/game_offsets.hpp"
#include "game/outline_culling.hpp"
#include "platform/hook.hpp"
#include "platform/log.hpp"
#include "platform/memory.hpp"
#include "platform/text.hpp"

#include <Windows.h>
#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace cotw::game
{
    namespace
    {
        using AddAnimalOrgansFunction = void(__fastcall*)(
            void*,
            const std::uint32_t* const*);
        using UpdateAnimalOrgansFunction = void(__fastcall*)(void*);
        using HasModelResourceFunction = std::uint8_t(__fastcall*)(void*, const char*);
        using ResetRenderableTransformFunction = void(__fastcall*)(void*, const void*);
        using RemoveRenderableFromSceneFunction = std::uint8_t(__fastcall*)(
            void*,
            void*,
            std::uint8_t);
        using ClearAnimalOrganMapFunction = void*(__fastcall*)(void*);
        using OrganRenderableCallback = void(*)(std::uintptr_t);

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

        struct OwnedOrganAnimal
        {
            std::uintptr_t animalAddress;
            std::uintptr_t speciesAddress;
            std::uint32_t lastSyncedTick;
            std::uint32_t unscopedTicks;
            bool attempted;
        };

        constexpr std::size_t kMaxTrackedOrgans = 64u;
        constexpr std::size_t kMaxSpeciesTreeNodes = 256u;
        constexpr std::size_t kMaxOrganPathLength = 256u;
        constexpr std::size_t kMaxOwnedOrganAnimals = 64u;
        constexpr std::uint32_t kUnscopedTeardownTicks = 180u;
        constexpr std::uint32_t kOrganGlintRgba = 0xFF4C4C4Cu;
        constexpr std::uint32_t kOrganVisibilityBits =
            kRenderableVisibleBit | kRenderableRegisteredInSceneBit;
        constexpr float kReticleMaxRadiusSquared = 0.25f;
        constexpr float kOcclusionBypassTimerValue = -1000.0f;

        UpdateAnimalOrgansFunction originalUpdateAnimalOrgans = nullptr;
        AddAnimalOrgansFunction addAnimalOrgans = nullptr;
        HasModelResourceFunction hasModelResource = nullptr;
        ResetRenderableTransformFunction resetRenderableTransform = nullptr;
        RemoveRenderableFromSceneFunction removeRenderableFromScene = nullptr;
        ClearAnimalOrganMapFunction clearAnimalOrganMap = nullptr;

        SRWLOCK organStateLock = SRWLOCK_INIT;
        OwnedOrganAnimal ownedAnimals[kMaxOwnedOrganAnimals]{};
        std::size_t ownedAnimalCount = 0;

        std::atomic<std::uintptr_t> activeOrganTarget{0};
        std::atomic<bool> activeScopingState{false};
        std::atomic<std::uint32_t> currentAnimalTick{0};
        std::atomic<std::uint32_t> lastEvaluatedTick{0};
        std::atomic<bool> hasEvaluatedTick{false};
        std::atomic<bool> organScopingLogged{false};
        std::atomic<bool> organTargetLogged{false};
        std::atomic<bool> organEmptyLogged{false};
        std::atomic<bool> organActiveLogged{false};

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

        bool containsAsciiToken(
            const char* text,
            const std::size_t length,
            const char* token,
            const std::size_t tokenLength) noexcept
        {
            if (text == nullptr || token == nullptr || tokenLength == 0u || length < tokenLength)
            {
                return false;
            }

            const std::size_t lastStart = length - tokenLength;
            for (std::size_t index = 0; index <= lastStart; ++index)
            {
                bool matched = true;
                for (std::size_t offset = 0; offset < tokenLength; ++offset)
                {
                    if (text[index + offset] != token[offset])
                    {
                        matched = false;
                        break;
                    }
                }
                if (matched)
                {
                    return true;
                }
            }

            return false;
        }

        bool readMsvcString(
            const std::uintptr_t stringAddress,
            const char*& data,
            std::size_t& length) noexcept
        {
            data = nullptr;
            length = 0;

            if (!isAlignedUserAddress(stringAddress, alignof(std::uintptr_t))
                || !platform::isReadable(reinterpret_cast<const void*>(stringAddress), 32u))
            {
                return false;
            }

            const std::size_t stringLength = *reinterpret_cast<const std::size_t*>(
                stringAddress + 16u);
            const std::size_t stringCapacity = *reinterpret_cast<const std::size_t*>(
                stringAddress + 24u);
            if (stringLength == 0u
                || stringLength >= kMaxOrganPathLength
                || stringCapacity < stringLength
                || stringCapacity >= 4096u)
            {
                return false;
            }

            const char* chars = nullptr;
            if (stringCapacity >= 16u)
            {
                const std::uintptr_t heapAddress = *reinterpret_cast<const std::uintptr_t*>(
                    stringAddress);
                if (!isAlignedUserAddress(heapAddress, 1u))
                {
                    return false;
                }
                chars = reinterpret_cast<const char*>(heapAddress);
            }
            else
            {
                chars = reinterpret_cast<const char*>(stringAddress);
            }

            if (!platform::isReadable(chars, stringLength))
            {
                return false;
            }

            data = chars;
            length = stringLength;
            return true;
        }

        void appendChars(
            char* destination,
            std::size_t& cursor,
            const char* source,
            const std::size_t count) noexcept
        {
            for (std::size_t index = 0; index < count; ++index)
            {
                destination[cursor++] = source[index];
            }
        }

        bool buildOrganModelPath(
            const char* speciesName,
            const std::size_t speciesLength,
            const char* separator,
            const std::size_t separatorLength,
            const char* organName,
            const std::size_t organLength,
            char (&buffer)[kMaxOrganPathLength]) noexcept
        {
            constexpr char kModelsPrefix[] = "models/hp_characters/animals/";
            constexpr std::size_t kModelsPrefixLength = sizeof(kModelsPrefix) - 1u;
            constexpr char kOrgansFolder[] = "/internal_organs/";
            constexpr std::size_t kOrgansFolderLength = sizeof(kOrgansFolder) - 1u;
            constexpr char kExtension[] = ".modelc";
            constexpr std::size_t kExtensionLength = sizeof(kExtension) - 1u;

            const std::size_t totalLength =
                kModelsPrefixLength
                + speciesLength
                + kOrgansFolderLength
                + speciesLength
                + separatorLength
                + organLength
                + kExtensionLength;
            if (totalLength >= kMaxOrganPathLength)
            {
                return false;
            }

            std::size_t cursor = 0;
            appendChars(buffer, cursor, kModelsPrefix, kModelsPrefixLength);
            appendChars(buffer, cursor, speciesName, speciesLength);
            appendChars(buffer, cursor, kOrgansFolder, kOrgansFolderLength);
            appendChars(buffer, cursor, speciesName, speciesLength);
            appendChars(buffer, cursor, separator, separatorLength);
            appendChars(buffer, cursor, organName, organLength);
            appendChars(buffer, cursor, kExtension, kExtensionLength);
            buffer[cursor] = '\0';
            return true;
        }

        bool hasOrganModelResource(
            const std::uintptr_t resourceManager,
            const char* speciesName,
            const std::size_t speciesLength,
            const char* organName,
            const std::size_t organLength,
            const std::uint8_t animalGender) noexcept
        {
            if (hasModelResource == nullptr
                || !isAlignedUserAddress(resourceManager, alignof(std::uintptr_t)))
            {
                return true;
            }

            char pathBuffer[kMaxOrganPathLength]{};
            if (animalGender == 2u
                && buildOrganModelPath(
                    speciesName,
                    speciesLength,
                    "_female_",
                    8u,
                    organName,
                    organLength,
                    pathBuffer)
                && hasModelResource(reinterpret_cast<void*>(resourceManager), pathBuffer) != 0u)
            {
                return true;
            }

            if (!buildOrganModelPath(
                    speciesName,
                    speciesLength,
                    "_",
                    1u,
                    organName,
                    organLength,
                    pathBuffer))
            {
                return false;
            }

            return hasModelResource(reinterpret_cast<void*>(resourceManager), pathBuffer) != 0u;
        }

        bool isReadableTreeNode(const std::uintptr_t nodeAddress) noexcept
        {
            if (!isAlignedUserAddress(nodeAddress, alignof(std::uintptr_t))
                || !platform::isReadable(reinterpret_cast<const void*>(nodeAddress), 32u))
            {
                return false;
            }

            return *reinterpret_cast<const std::uint8_t*>(
                nodeAddress + kOrganTreeNodeIsNilOffset) == 0u;
        }

        std::uintptr_t advanceOrganTreeNode(
            std::uintptr_t node,
            const std::uintptr_t sentinel) noexcept
        {
            const std::uintptr_t right = *reinterpret_cast<const std::uintptr_t*>(
                node + kOrganTreeNodeRightOffset);
            if (isReadableTreeNode(right))
            {
                node = right;
                for (std::size_t step = 0; step < kMaxSpeciesTreeNodes; ++step)
                {
                    const std::uintptr_t left = *reinterpret_cast<const std::uintptr_t*>(
                        node + kOrganTreeNodeLeftOffset);
                    if (!isReadableTreeNode(left))
                    {
                        break;
                    }
                    node = left;
                }
                return node;
            }

            std::uintptr_t parent = *reinterpret_cast<const std::uintptr_t*>(
                node + kOrganTreeNodeParentOffset);
            for (std::size_t step = 0; step < kMaxSpeciesTreeNodes; ++step)
            {
                if (parent == sentinel || !isReadableTreeNode(parent))
                {
                    return parent;
                }

                const std::uintptr_t parentRight = *reinterpret_cast<const std::uintptr_t*>(
                    parent + kOrganTreeNodeRightOffset);
                if (node != parentRight)
                {
                    break;
                }

                node = parent;
                parent = *reinterpret_cast<const std::uintptr_t*>(
                    parent + kOrganTreeNodeParentOffset);
            }

            return parent;
        }

        std::size_t scanOrganTreeNodes(
            const std::uintptr_t sentinel,
            const std::uintptr_t resourceManager,
            const char* speciesName,
            const std::size_t speciesLength,
            const std::uint8_t animalGender,
            const bool requireInternalToken,
            std::uint32_t (&hashes)[kMaxTrackedOrgans]) noexcept
        {
            std::uintptr_t node = *reinterpret_cast<const std::uintptr_t*>(
                sentinel + kOrganTreeNodeLeftOffset);
            std::size_t count = 0;

            for (std::size_t step = 0;
                node != sentinel && step < kMaxSpeciesTreeNodes;
                ++step)
            {
                if (!isReadableTreeNode(node)
                    || !platform::isReadable(reinterpret_cast<const void*>(node), 96u))
                {
                    break;
                }

                const char* organName = nullptr;
                std::size_t organLength = 0;
                if (readMsvcString(node + kOrganTreeNodeNameOffset, organName, organLength)
                    && (!requireInternalToken
                        || containsAsciiToken(organName, organLength, "internal", 8u)))
                {
                    const std::uint8_t nodeGender = *reinterpret_cast<const std::uint8_t*>(
                        node + kOrganTreeNodeGenderOffset);
                    const bool genderCompatible = nodeGender == 0u
                        || nodeGender == 3u
                        || animalGender == 0u
                        || nodeGender == animalGender;
                    if (genderCompatible
                        && hasOrganModelResource(
                            resourceManager,
                            speciesName,
                            speciesLength,
                            organName,
                            organLength,
                            animalGender))
                    {
                        const std::uint32_t organHash = *reinterpret_cast<const std::uint32_t*>(
                            node + kOrganTreeNodeHashOffset);
                        bool duplicate = false;
                        for (std::size_t existing = 0; existing < count; ++existing)
                        {
                            if (hashes[existing] == organHash)
                            {
                                duplicate = true;
                                break;
                            }
                        }

                        if (!duplicate && count < kMaxTrackedOrgans)
                        {
                            hashes[count++] = organHash;
                        }
                    }
                }

                node = advanceOrganTreeNode(node, sentinel);
            }

            return count;
        }

        std::size_t collectOrganHashes(
            const std::uintptr_t animalAddress,
            std::uint32_t (&hashes)[kMaxTrackedOrgans]) noexcept
        {
            const std::uintptr_t speciesAddress = *reinterpret_cast<const std::uintptr_t*>(
                animalAddress + kAnimalSpeciesOffset);
            if (!isAlignedUserAddress(speciesAddress, alignof(std::uintptr_t)))
            {
                return 0;
            }

            const char* speciesName = nullptr;
            std::size_t speciesLength = 0;
            if (!readMsvcString(speciesAddress + kSpeciesNameOffset, speciesName, speciesLength))
            {
                return 0;
            }

            const std::uintptr_t sentinel = *reinterpret_cast<const std::uintptr_t*>(
                speciesAddress + kSpeciesOrganMapOffset);
            if (!isAlignedUserAddress(sentinel, alignof(std::uintptr_t))
                || !platform::isReadable(reinterpret_cast<const void*>(sentinel), 32u))
            {
                return 0;
            }

            const std::uint8_t animalGender = static_cast<std::uint8_t>(
                *reinterpret_cast<const std::uint32_t*>(animalAddress + kAnimalOutlineFlagOffset)
                & kAnimalGenderMask);
            const std::uintptr_t resourceManager = *reinterpret_cast<const std::uintptr_t*>(
                base() + kModelResourceManagerRva);

            const std::size_t strictCount = scanOrganTreeNodes(
                sentinel,
                resourceManager,
                speciesName,
                speciesLength,
                animalGender,
                true,
                hashes);
            if (strictCount > 0u)
            {
                return strictCount;
            }

            return scanOrganTreeNodes(
                sentinel,
                resourceManager,
                speciesName,
                speciesLength,
                animalGender,
                false,
                hashes);
        }

        void iterateAnimalOrganRenderables(
            const std::uintptr_t animalAddress,
            const OrganRenderableCallback callback) noexcept
        {
            const std::uintptr_t listHead = *reinterpret_cast<const std::uintptr_t*>(
                animalAddress + kAnimalOrganListOffset);
            if (!isAlignedUserAddress(listHead, alignof(std::uintptr_t))
                || !platform::isReadable(reinterpret_cast<const void*>(listHead), 32u))
            {
                return;
            }

            std::uintptr_t node = *reinterpret_cast<const std::uintptr_t*>(
                listHead + kOrganListNodeNextOffset);
            for (std::size_t step = 0; node != listHead && step < kMaxTrackedOrgans; ++step)
            {
                if (!isAlignedUserAddress(node, alignof(std::uintptr_t))
                    || !platform::isReadable(reinterpret_cast<const void*>(node), 32u))
                {
                    break;
                }

                const std::uintptr_t next = *reinterpret_cast<const std::uintptr_t*>(
                    node + kOrganListNodeNextOffset);
                const std::uintptr_t renderable = *reinterpret_cast<const std::uintptr_t*>(
                    node + kOrganListNodeRenderableOffset);
                if (isAlignedUserAddress(renderable, alignof(std::uintptr_t)))
                {
                    callback(renderable);
                }

                node = next;
            }
        }

        void enforceVisibleOrganRenderable(const std::uintptr_t renderableAddress) noexcept
        {
            auto* rgbaPtr = reinterpret_cast<std::uint32_t*>(
                renderableAddress + kRenderableOutlineRgbaOffset);
            if (*rgbaPtr != kOrganGlintRgba)
            {
                *rgbaPtr = kOrganGlintRgba;
                auto* dirtyFlagPtr = reinterpret_cast<volatile long*>(
                    renderableAddress + kRenderableDirtyFlagOffset);
                _InterlockedExchange(dirtyFlagPtr, 1);
            }

            auto* flagsPtr = reinterpret_cast<volatile long*>(
                renderableAddress + kRenderableFlagsOffset);
            const std::uint32_t currentFlags = static_cast<std::uint32_t>(*flagsPtr);

            constexpr std::uint32_t kEnableBits =
                kRenderableSurfaceGlowBit
                | kRenderableIgnoreSizeCullBit
                | kOrganVisibilityBits;
            if ((currentFlags & kEnableBits) != kEnableBits)
            {
                _InterlockedOr(flagsPtr, static_cast<long>(kEnableBits));
            }

            constexpr std::uint32_t kClearBits =
                kRenderableOutlineActiveBit | kRenderableMaxDistanceCullBit;
            if ((currentFlags & kClearBits) != 0u)
            {
                _InterlockedAnd(flagsPtr, ~static_cast<long>(kClearBits));
            }

            updateCullingSlotIgnoreSize(renderableAddress, true);
        }

        void hideOrganRenderable(const std::uintptr_t renderableAddress) noexcept
        {
            auto* flagsPtr = reinterpret_cast<volatile long*>(
                renderableAddress + kRenderableFlagsOffset);
            const std::uint32_t currentFlags = static_cast<std::uint32_t>(*flagsPtr);
            if ((currentFlags & kOrganVisibilityBits) != 0u)
            {
                _InterlockedAnd(flagsPtr, ~static_cast<long>(kOrganVisibilityBits));
            }
        }

        void detachOrganRenderable(const std::uintptr_t renderableAddress) noexcept
        {
            auto* flagsPtr = reinterpret_cast<volatile long*>(
                renderableAddress + kRenderableFlagsOffset);
            constexpr std::uint32_t kClearBits =
                kOrganVisibilityBits | kRenderableIgnoreSizeCullBit;
            _InterlockedAnd(flagsPtr, ~static_cast<long>(kClearBits));
            updateCullingSlotIgnoreSize(renderableAddress, false);

            if (resetRenderableTransform != nullptr)
            {
                const auto identityMatrix = reinterpret_cast<const void*>(
                    base() + kIdentityMatrix4x4Rva);
                resetRenderableTransform(
                    reinterpret_cast<void*>(renderableAddress),
                    identityMatrix);
            }

            if (removeRenderableFromScene != nullptr)
            {
                const std::uintptr_t dynamicScene = *reinterpret_cast<const std::uintptr_t*>(
                    base() + kDynamicRenderSceneRva);
                if (isAlignedUserAddress(dynamicScene, alignof(std::uintptr_t)))
                {
                    removeRenderableFromScene(
                        reinterpret_cast<void*>(dynamicScene),
                        reinterpret_cast<void*>(renderableAddress),
                        0u);
                }
            }
        }

        void teardownAnimalOrgans(const std::uintptr_t animalAddress) noexcept
        {
            const std::uint64_t count = *reinterpret_cast<const std::uint64_t*>(
                animalAddress + kAnimalOrganCountOffset);
            if (count == 0u)
            {
                return;
            }

            iterateAnimalOrganRenderables(animalAddress, &detachOrganRenderable);

            if (clearAnimalOrganMap != nullptr)
            {
                clearAnimalOrganMap(reinterpret_cast<void*>(
                    animalAddress + kAnimalOrganMapOffset));
            }
        }

        std::size_t findOwnedAnimalIndexLocked(const std::uintptr_t animalAddress) noexcept
        {
            for (std::size_t index = 0; index < ownedAnimalCount; ++index)
            {
                if (ownedAnimals[index].animalAddress == animalAddress)
                {
                    return index;
                }
            }
            return kMaxOwnedOrganAnimals;
        }

        bool isOwnedAnimal(const std::uintptr_t animalAddress) noexcept
        {
            const ExclusiveSrwGuard lock(organStateLock);
            return findOwnedAnimalIndexLocked(animalAddress) != kMaxOwnedOrganAnimals;
        }

        void recordAnimalSyncedTick(
            const std::uintptr_t animalAddress,
            const std::uint32_t tick) noexcept
        {
            const ExclusiveSrwGuard lock(organStateLock);
            const std::size_t index = findOwnedAnimalIndexLocked(animalAddress);
            if (index != kMaxOwnedOrganAnimals)
            {
                ownedAnimals[index].lastSyncedTick = tick;
            }
        }

        void eraseOwnedAnimal(const std::uintptr_t animalAddress) noexcept
        {
            const ExclusiveSrwGuard lock(organStateLock);
            const std::size_t index = findOwnedAnimalIndexLocked(animalAddress);
            if (index != kMaxOwnedOrganAnimals)
            {
                ownedAnimals[index] = ownedAnimals[ownedAnimalCount - 1u];
                ownedAnimals[--ownedAnimalCount] = OwnedOrganAnimal{};
            }
        }

        bool readLocalScopingState() noexcept
        {
            const std::uintptr_t playerManager = *reinterpret_cast<const std::uintptr_t*>(
                base() + kPlayerManagerRva);
            if (!isAlignedUserAddress(playerManager, alignof(std::uintptr_t)))
            {
                return false;
            }

            const std::uintptr_t entry = *reinterpret_cast<const std::uintptr_t*>(
                playerManager + kPlayerManagerEntryOffset);
            if (!isAlignedUserAddress(entry, alignof(std::uintptr_t)))
            {
                return false;
            }

            const std::uintptr_t pawn = *reinterpret_cast<const std::uintptr_t*>(
                entry + kPlayerManagerActivePawnOffset);
            if (!isAlignedUserAddress(pawn, alignof(std::uintptr_t))
                || !platform::isReadable(
                    reinterpret_cast<const void*>(pawn + kPawnAimStateOffset),
                    1u))
            {
                return false;
            }

            const std::uint8_t aimState = *reinterpret_cast<const std::uint8_t*>(
                pawn + kPawnAimStateOffset);
            return (aimState & kPawnAimStateActiveBit) != 0u;
        }

        bool containsLiveAnimal(
            const std::uintptr_t* animals,
            const std::size_t count,
            const std::uintptr_t animalAddress) noexcept
        {
            for (std::size_t index = 0; index < count; ++index)
            {
                if (animals[index] == animalAddress)
                {
                    return true;
                }
            }
            return false;
        }

        void pruneOwnedAnimals(
            const std::uintptr_t* liveAnimals,
            const std::size_t liveCount,
            const bool scoping) noexcept
        {
            const ExclusiveSrwGuard lock(organStateLock);
            std::size_t index = 0;
            while (index < ownedAnimalCount)
            {
                const std::uintptr_t animal = ownedAnimals[index].animalAddress;
                if (!containsLiveAnimal(liveAnimals, liveCount, animal)
                    || *reinterpret_cast<const std::uintptr_t*>(animal + kAnimalSpeciesOffset)
                        != ownedAnimals[index].speciesAddress)
                {
                    ownedAnimals[index] = ownedAnimals[ownedAnimalCount - 1u];
                    ownedAnimals[--ownedAnimalCount] = OwnedOrganAnimal{};
                    continue;
                }

                if (scoping)
                {
                    ownedAnimals[index].unscopedTicks = 0u;
                }
                else
                {
                    ++ownedAnimals[index].unscopedTicks;
                }
                ++index;
            }
        }

        std::uintptr_t selectReticleTarget(
            const std::uintptr_t* liveAnimals,
            const std::size_t liveCount) noexcept
        {
            const float maximumDistance = clampMaximumDistance(
                application::features.highlightDistance);
            if (maximumDistance <= 0.0f)
            {
                return 0;
            }

            float cameraPosition[3]{};
            float viewProjection[16]{};
            if (!readCameraPosition(cameraPosition) || !readViewProjection(viewProjection))
            {
                return 0;
            }

            const float maximumDistanceSquared = maximumDistance * maximumDistance;
            float bestScore = kReticleMaxRadiusSquared;
            std::uintptr_t bestAnimal = 0;

            for (std::size_t index = 0; index < liveCount; ++index)
            {
                const std::uintptr_t candidate = liveAnimals[index];
                if (!isAlignedUserAddress(candidate, alignof(std::uintptr_t)))
                {
                    continue;
                }

                const std::uintptr_t character = *reinterpret_cast<const std::uintptr_t*>(
                    candidate + kAnimalCharacterOffset);
                std::uintptr_t verifiedAnimal = 0;
                if (!resolveAnimalFromCharacter(character, verifiedAnimal)
                    || verifiedAnimal != candidate
                    || !isAnimalAlive(candidate))
                {
                    continue;
                }

                float worldX = 0.0f;
                float worldY = 0.0f;
                float worldZ = 0.0f;
                if (!readCharacterWorldCenter(character, worldX, worldY, worldZ))
                {
                    continue;
                }

                const float deltaX = worldX - cameraPosition[0];
                const float deltaY = worldY - cameraPosition[1];
                const float deltaZ = worldZ - cameraPosition[2];
                const float distanceSquared =
                    deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
                if (distanceSquared > maximumDistanceSquared)
                {
                    continue;
                }

                const float clipW = worldX * viewProjection[3]
                    + worldY * viewProjection[7]
                    + worldZ * viewProjection[11]
                    + viewProjection[15];
                if (clipW <= 0.05f)
                {
                    continue;
                }

                const float clipX = worldX * viewProjection[0]
                    + worldY * viewProjection[4]
                    + worldZ * viewProjection[8]
                    + viewProjection[12];
                const float clipY = worldX * viewProjection[1]
                    + worldY * viewProjection[5]
                    + worldZ * viewProjection[9]
                    + viewProjection[13];

                const float inverseW = 1.0f / clipW;
                const float ndcX = clipX * inverseW;
                const float ndcY = clipY * inverseW;
                const float score = ndcX * ndcX + ndcY * ndcY;
                if (score <= bestScore)
                {
                    bestScore = score;
                    bestAnimal = candidate;
                }
            }

            return bestAnimal;
        }

        void refreshScopedTargetForTick(const std::uintptr_t updateContextAddress) noexcept
        {
            if (!isAlignedUserAddress(updateContextAddress, alignof(std::uintptr_t)))
            {
                return;
            }

            const std::uint32_t tick = *reinterpret_cast<const std::uint32_t*>(
                updateContextAddress + kUpdateContextTickOffset);
            if (hasEvaluatedTick.load(std::memory_order_acquire)
                && lastEvaluatedTick.load(std::memory_order_relaxed) == tick)
            {
                return;
            }

            lastEvaluatedTick.store(tick, std::memory_order_relaxed);
            hasEvaluatedTick.store(true, std::memory_order_release);
            currentAnimalTick.store(tick, std::memory_order_release);

            const std::uintptr_t animalManager = *reinterpret_cast<const std::uintptr_t*>(
                updateContextAddress + kUpdateContextAnimalManagerOffset);
            if (!isAlignedUserAddress(animalManager, alignof(std::uintptr_t)))
            {
                return;
            }

            const std::uintptr_t vectorBegin = *reinterpret_cast<const std::uintptr_t*>(
                animalManager + kAnimalManagerVectorOffset);
            const std::uintptr_t vectorEnd = *reinterpret_cast<const std::uintptr_t*>(
                animalManager + kAnimalManagerVectorEndOffset);
            if (!isAlignedUserAddress(vectorBegin, alignof(std::uintptr_t))
                || vectorEnd < vectorBegin)
            {
                return;
            }

            std::size_t liveCount = (vectorEnd - vectorBegin) / sizeof(std::uintptr_t);
            if (liveCount > kMaximumAnimals)
            {
                liveCount = kMaximumAnimals;
            }

            const auto* liveAnimals = reinterpret_cast<const std::uintptr_t*>(vectorBegin);
            const bool scoping = readLocalScopingState();
            pruneOwnedAnimals(liveAnimals, liveCount, scoping);

            std::uintptr_t nextTarget = 0;
            if (scoping)
            {
                if (!organScopingLogged.exchange(true, std::memory_order_relaxed))
                {
                    platform::writeLogLine("First local scoping detected");
                }
                nextTarget = selectReticleTarget(liveAnimals, liveCount);
                if (nextTarget != 0 && !organTargetLogged.exchange(true, std::memory_order_relaxed))
                {
                    platform::writeLogLine("First scoped organ target matched");
                }
            }

            activeScopingState.store(scoping, std::memory_order_release);
            activeOrganTarget.store(nextTarget, std::memory_order_release);
        }

        void invokeUpdateOrgansForTarget(
            const std::uintptr_t animalAddress,
            const std::uintptr_t characterAddress,
            const std::uint32_t tick) noexcept
        {
            if (originalUpdateAnimalOrgans == nullptr)
            {
                return;
            }

            const std::uintptr_t animComponent = *reinterpret_cast<const std::uintptr_t*>(
                characterAddress + kPawnAnimationComponentOffset);
            if (!isAlignedUserAddress(animComponent, alignof(std::uintptr_t)))
            {
                return;
            }

            auto* occlusionTimer = reinterpret_cast<float*>(
                characterAddress + kCharacterOcclusionTimerOffset);
            const float previousTimer = *occlusionTimer;
            *occlusionTimer = kOcclusionBypassTimerValue;
            originalUpdateAnimalOrgans(reinterpret_cast<void*>(animalAddress));
            *occlusionTimer = previousTimer;

            iterateAnimalOrganRenderables(animalAddress, &enforceVisibleOrganRenderable);
            recordAnimalSyncedTick(animalAddress, tick);
        }

        void __fastcall updateAnimalOrgansDetour(void* animal) noexcept
        {
            if (originalUpdateAnimalOrgans == nullptr)
            {
                return;
            }

            const auto animalAddress = reinterpret_cast<std::uintptr_t>(animal);
            if (!isAlignedUserAddress(animalAddress, alignof(std::uintptr_t)))
            {
                originalUpdateAnimalOrgans(animal);
                return;
            }

            const std::uintptr_t target = activeOrganTarget.load(std::memory_order_acquire);
            const bool scoping = readLocalScopingState();
            const std::uint32_t tick = currentAnimalTick.load(std::memory_order_acquire);

            if (scoping && animalAddress == target)
            {
                const std::uintptr_t character = *reinterpret_cast<const std::uintptr_t*>(
                    animalAddress + kAnimalCharacterOffset);
                if (isAlignedUserAddress(character, alignof(std::uintptr_t)))
                {
                    invokeUpdateOrgansForTarget(animalAddress, character, tick);
                    return;
                }
            }

            originalUpdateAnimalOrgans(animal);

            if (isOwnedAnimal(animalAddress))
            {
                iterateAnimalOrganRenderables(animalAddress, &hideOrganRenderable);
                recordAnimalSyncedTick(animalAddress, tick);
            }
        }
    }

    bool installOrganHighlight() noexcept
    {
        if (!isAvailable())
        {
            return false;
        }

        const std::uintptr_t moduleBase = base();
        addAnimalOrgans = reinterpret_cast<AddAnimalOrgansFunction>(
            moduleBase + kAddAnimalOrgansRva);
        hasModelResource = reinterpret_cast<HasModelResourceFunction>(
            moduleBase + kHasModelResourceRva);
        resetRenderableTransform = reinterpret_cast<ResetRenderableTransformFunction>(
            moduleBase + kResetRenderableTransformRva);
        removeRenderableFromScene = reinterpret_cast<RemoveRenderableFromSceneFunction>(
            moduleBase + kRemoveRenderableFromSceneRva);
        clearAnimalOrganMap = reinterpret_cast<ClearAnimalOrganMapFunction>(
            moduleBase + kClearAnimalOrganMapRva);

        const auto updateOrgansTarget = reinterpret_cast<void*>(
            moduleBase + kUpdateAnimalOrgansRva);
        originalUpdateAnimalOrgans = reinterpret_cast<UpdateAnimalOrgansFunction>(
            updateOrgansTarget);
        const auto updateOrgansCallSite = reinterpret_cast<void*>(
            moduleBase + kUpdateAnimalOrgansCallSiteRva);

        organScopingLogged.store(false, std::memory_order_relaxed);
        organTargetLogged.store(false, std::memory_order_relaxed);
        organEmptyLogged.store(false, std::memory_order_relaxed);
        organActiveLogged.store(false, std::memory_order_relaxed);
        hasEvaluatedTick.store(false, std::memory_order_relaxed);

        const bool installed = platform::installCallHook(
            updateOrgansCallSite,
            reinterpret_cast<void*>(&updateAnimalOrgansDetour),
            updateOrgansTarget);

        platform::TextBuilder builder;
        builder.append("Organ highlight hooks installed (");
        builder.appendUnsigned(installed ? 1u : 0u);
        builder.append("/1)");
        platform::writeLogLine(builder.c_str());

        return installed;
    }

    void updateOrganHighlightForAnimal(
        const std::uintptr_t animalAddress,
        const std::uintptr_t characterAddress,
        const std::uintptr_t updateContextAddress,
        const bool isAlive) noexcept
    {
        if (addAnimalOrgans == nullptr || originalUpdateAnimalOrgans == nullptr)
        {
            return;
        }

        refreshScopedTargetForTick(updateContextAddress);

        const std::uintptr_t target = activeOrganTarget.load(std::memory_order_acquire);
        const bool scoping = activeScopingState.load(std::memory_order_acquire);
        const std::uint32_t tick = currentAnimalTick.load(std::memory_order_acquire);

        bool isOwned = false;
        bool attempted = false;
        std::uint32_t lastSyncedTick = 0;
        std::uint32_t unscopedTicks = 0;
        {
            const ExclusiveSrwGuard lock(organStateLock);
            const std::size_t index = findOwnedAnimalIndexLocked(animalAddress);
            if (index != kMaxOwnedOrganAnimals)
            {
                isOwned = true;
                attempted = ownedAnimals[index].attempted;
                lastSyncedTick = ownedAnimals[index].lastSyncedTick;
                unscopedTicks = ownedAnimals[index].unscopedTicks;
            }
        }

        const bool switchedAway = target != 0 && animalAddress != target;
        const bool staleUnscoped = !scoping && unscopedTicks >= kUnscopedTeardownTicks;
        if (isOwned && (!isAlive || switchedAway || staleUnscoped))
        {
            eraseOwnedAnimal(animalAddress);
            teardownAnimalOrgans(animalAddress);
            return;
        }

        const std::uint64_t existingOrganCount = *reinterpret_cast<const std::uint64_t*>(
            animalAddress + kAnimalOrganCountOffset);

        if (scoping && isAlive && animalAddress == target)
        {
            if (existingOrganCount == 0u && !attempted)
            {
                const std::uintptr_t speciesAddress = *reinterpret_cast<const std::uintptr_t*>(
                    animalAddress + kAnimalSpeciesOffset);
                {
                    const ExclusiveSrwGuard lock(organStateLock);
                    if (findOwnedAnimalIndexLocked(animalAddress) == kMaxOwnedOrganAnimals
                        && ownedAnimalCount < kMaxOwnedOrganAnimals)
                    {
                        ownedAnimals[ownedAnimalCount++] = OwnedOrganAnimal{
                            animalAddress,
                            speciesAddress,
                            tick,
                            0u,
                            true,
                        };
                    }
                }

                std::uint32_t hashes[kMaxTrackedOrgans]{};
                const std::size_t hashCount = collectOrganHashes(animalAddress, hashes);
                if (hashCount > 0u)
                {
                    const std::uint32_t* const range[2] = {hashes, hashes + hashCount};
                    addAnimalOrgans(reinterpret_cast<void*>(animalAddress), range);

                    const std::uint64_t spawnedCount = *reinterpret_cast<const std::uint64_t*>(
                        animalAddress + kAnimalOrganCountOffset);
                    if (spawnedCount > 0u)
                    {
                        invokeUpdateOrgansForTarget(animalAddress, characterAddress, tick);
                        if (!organActiveLogged.exchange(true, std::memory_order_relaxed))
                        {
                            platform::TextBuilder builder;
                            builder.append("First scoped organ highlight active (count=");
                            builder.appendUnsigned(spawnedCount);
                            builder.append(")");
                            platform::writeLogLine(builder.c_str());
                        }
                    }
                }
                else if (!organEmptyLogged.exchange(true, std::memory_order_relaxed))
                {
                    platform::writeLogLine("Organ hash lookup returned zero entries for target");
                }
            }
            else if (existingOrganCount > 0u && (tick - lastSyncedTick) >= 2u)
            {
                invokeUpdateOrgansForTarget(animalAddress, characterAddress, tick);
            }

            return;
        }

        if (isOwned && existingOrganCount > 0u && (tick - lastSyncedTick) >= 2u)
        {
            iterateAnimalOrganRenderables(animalAddress, &hideOrganRenderable);
            recordAnimalSyncedTick(animalAddress, tick);
        }
    }
}
