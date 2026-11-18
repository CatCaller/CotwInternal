#include "game/animal_highlight.hpp"

#include "game/game_memory.hpp"
#include "game/game_offsets.hpp"
#include "game/organ_highlight.hpp"
#include "game/outline_culling.hpp"
#include "platform/hook.hpp"
#include "platform/log.hpp"
#include "platform/memory.hpp"
#include "platform/text.hpp"

#include <intrin.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace cotw::game
{
    namespace
    {
        using CharacterUpdateLodFunction = std::uint8_t(__fastcall*)(void*);
        using CharacterUpdateOutlineFunction = void(__fastcall*)(void*, float);
        using AnimalUpdateFunction = void(__fastcall*)(void*, void*);
        using ClearCharacterOutlineFunction = void(__fastcall*)(void*);
        using SetCharacterOutlineFunction = void(__fastcall*)(
            void*,
            const float*,
            float,
            float,
            float);
        using GetRenderableFunction = std::uintptr_t(__fastcall*)(void*, std::uint32_t);

        enum class RenderableHighlightMode : std::uint8_t
        {
            Enabled,
            EnabledClearSurfaceGlow,
            Restored,
        };

        struct CurrentOutlineColor
        {
            float color[4];
        };

        struct OutlineParameters
        {
            float color[4];
            float duration;
            float fadeIn;
            float fadeOut;
        };

        static_assert(sizeof(CurrentOutlineColor) == 16);
        static_assert(sizeof(OutlineParameters) == 28);
        static_assert(
            kCharacterCurrentOutlineAlphaOffset == kCharacterCurrentOutlineColorOffset + 12);
        static_assert(
            kCharacterOutlineDurationOffset == kCharacterOutlineColorOffset + 16);
        static_assert(
            kCharacterOutlineFadeInOffset == kCharacterOutlineColorOffset + 20);
        static_assert(
            kCharacterOutlineFadeOutOffset == kCharacterOutlineColorOffset + 24);

        constexpr std::uint32_t kMaxRenderablesPerCharacter = 32u;
        constexpr std::uint16_t kInvalidCullingHandle = 0xFFFFu;

        constexpr CurrentOutlineColor kDesiredCurrentOutline{
            {kOutlineRed, kOutlineGreen, kOutlineBlue, kOutlineAlpha},
        };

        constexpr OutlineParameters kDesiredOutline{
            {kOutlineRed, kOutlineGreen, kOutlineBlue, kOutlineAlpha},
            -1.0f,
            0.0f,
            0.0f,
        };

        constexpr OutlineParameters kClearedOutline{
            {0.0f, 0.0f, 0.0f, 0.0f},
            0.0f,
            0.0f,
            0.0f,
        };

        CharacterUpdateLodFunction originalCharacterUpdateLod = nullptr;
        CharacterUpdateOutlineFunction originalCharacterUpdateOutline = nullptr;
        AnimalUpdateFunction originalAnimalUpdate = nullptr;
        ClearCharacterOutlineFunction originalClearCharacterOutline = nullptr;
        SetCharacterOutlineFunction originalSetCharacterOutline = nullptr;
        std::atomic<bool> outlineActiveLogged{false};

        bool isExecutableAddress(const std::uintptr_t address) noexcept
        {
            return address >= 0x10000ull && address <= 0x00007FFFFFFEFFFFull;
        }

        float absoluteDifference(const float left, const float right) noexcept
        {
            const float delta = left - right;
            return delta < 0.0f ? -delta : delta;
        }

        bool currentColorEquals(
            const CurrentOutlineColor& left,
            const CurrentOutlineColor& right) noexcept
        {
            return left.color[0] == right.color[0]
                && left.color[1] == right.color[1]
                && left.color[2] == right.color[2]
                && left.color[3] == right.color[3];
        }

        void writeCurrentColor(
            CurrentOutlineColor& destination,
            const CurrentOutlineColor& source) noexcept
        {
            destination.color[0] = source.color[0];
            destination.color[1] = source.color[1];
            destination.color[2] = source.color[2];
            destination.color[3] = source.color[3];
        }

        bool outlineEquals(
            const OutlineParameters& left,
            const OutlineParameters& right) noexcept
        {
            return left.color[0] == right.color[0]
                && left.color[1] == right.color[1]
                && left.color[2] == right.color[2]
                && left.color[3] == right.color[3]
                && left.duration == right.duration
                && left.fadeIn == right.fadeIn
                && left.fadeOut == right.fadeOut;
        }

        void writeOutlineParameters(
            OutlineParameters& destination,
            const OutlineParameters& source) noexcept
        {
            destination.color[0] = source.color[0];
            destination.color[1] = source.color[1];
            destination.color[2] = source.color[2];
            destination.color[3] = source.color[3];
            destination.duration = source.duration;
            destination.fadeIn = source.fadeIn;
            destination.fadeOut = source.fadeOut;
        }

        bool isOurOrangeOutline(const OutlineParameters& parameters) noexcept
        {
            return parameters.duration < 0.0f
                && parameters.fadeIn == 0.0f
                && parameters.fadeOut == 0.0f
                && parameters.color[3] >= 0.10f
                && parameters.color[3] <= 0.35f
                && absoluteDifference(parameters.color[0], kOutlineRed) < 0.01f
                && absoluteDifference(parameters.color[1], kOutlineGreen) < 0.01f
                && absoluteDifference(parameters.color[2], kOutlineBlue) < 0.01f;
        }

        bool evaluateHighlightTarget(const std::uintptr_t characterAddress) noexcept
        {
            const auto* position = reinterpret_cast<const float*>(
                characterAddress + kPawnPositionOffset);
            return isPointInHighlightVolume(
                position[0],
                position[1] + 0.75f,
                position[2],
                0.0f);
        }

        bool shouldHighlightCharacter(const std::uintptr_t characterAddress) noexcept
        {
            std::uintptr_t animalAddress = 0;
            return resolveAnimalFromCharacter(characterAddress, animalAddress)
                && isAnimalAlive(animalAddress)
                && evaluateHighlightTarget(characterAddress);
        }

        std::uintptr_t readCharacterRenderableAt(
            const std::uintptr_t characterAddress,
            const std::uint32_t index) noexcept
        {
            const std::uintptr_t renderInterface =
                characterAddress + kCharacterRenderableInterfaceOffset;
            const auto vtable = *reinterpret_cast<const std::uintptr_t*>(renderInterface);
            if (!isAlignedUserAddress(vtable, alignof(std::uintptr_t)))
            {
                return 0;
            }

            const auto getterAddress = *reinterpret_cast<const std::uintptr_t*>(
                vtable + kCharacterGetRenderableVtableOffset);
            if (!isExecutableAddress(getterAddress))
            {
                return 0;
            }

            const auto getRenderable =
                reinterpret_cast<GetRenderableFunction>(getterAddress);
            return getRenderable(reinterpret_cast<void*>(renderInterface), index);
        }

        void updateSingleRenderable(
            const std::uintptr_t renderableAddress,
            const RenderableHighlightMode mode) noexcept
        {
            if (!isAlignedUserAddress(renderableAddress, alignof(std::uintptr_t)))
            {
                return;
            }

            auto* flagsPtr = reinterpret_cast<volatile long*>(
                renderableAddress + kRenderableFlagsOffset);
            const std::uint32_t currentFlags = static_cast<std::uint32_t>(*flagsPtr);

            if (mode == RenderableHighlightMode::Restored)
            {
                unregisterHighlightedRenderable(renderableAddress);

                constexpr std::uint32_t kClearMask =
                    kRenderableOutlineActiveBit | kRenderableIgnoreSizeCullBit;
                if ((currentFlags & kClearMask) != 0u)
                {
                    _InterlockedAnd(flagsPtr, ~static_cast<long>(kClearMask));
                }

                if ((currentFlags & kRenderableMaxDistanceCullBit) == 0u)
                {
                    _InterlockedOr(flagsPtr, static_cast<long>(kRenderableMaxDistanceCullBit));
                }

                updateCullingSlotIgnoreSize(renderableAddress, false);
                return;
            }

            constexpr std::uint32_t kEnableMask =
                kRenderableOutlineActiveBit
                | kRenderableIgnoreSizeCullBit
                | kRenderableDisableDitherBit;
            if ((currentFlags & kEnableMask) != kEnableMask)
            {
                _InterlockedOr(flagsPtr, static_cast<long>(kEnableMask));
            }

            std::uint32_t clearMask = kRenderableMaxDistanceCullBit;
            if (mode == RenderableHighlightMode::EnabledClearSurfaceGlow)
            {
                clearMask |= kRenderableSurfaceGlowBit;
            }
            if ((currentFlags & clearMask) != 0u)
            {
                _InterlockedAnd(flagsPtr, ~static_cast<long>(clearMask));
            }

            enforceRenderableOutlineBytes(renderableAddress);
            updateCullingSlotIgnoreSize(renderableAddress, true);
            registerHighlightedRenderable(renderableAddress);
        }

        void updateCharacterRenderables(
            const std::uintptr_t characterAddress,
            const RenderableHighlightMode mode) noexcept
        {
            std::uint32_t primaryCount = *reinterpret_cast<const std::uint32_t*>(
                characterAddress + kCharacterPrimaryRenderableCountOffset);
            if (primaryCount > kMaxRenderablesPerCharacter)
            {
                primaryCount = kMaxRenderablesPerCharacter;
            }

            if (primaryCount > 0)
            {
                for (std::uint32_t index = 0; index < primaryCount; ++index)
                {
                    const std::uintptr_t renderable = readCharacterRenderableAt(
                        characterAddress,
                        index);
                    updateSingleRenderable(renderable, mode);
                }
            }

            const auto componentAddress = *reinterpret_cast<const std::uintptr_t*>(
                characterAddress + kCharacterAnimalComponentOffset);
            if (!isAlignedUserAddress(componentAddress, alignof(std::uintptr_t)))
            {
                return;
            }

            const auto attachmentsBegin = *reinterpret_cast<const std::uintptr_t*>(
                componentAddress + kAnimalComponentAttachmentsBeginOffset);
            const auto attachmentsEnd = *reinterpret_cast<const std::uintptr_t*>(
                componentAddress + kAnimalComponentAttachmentsEndOffset);
            if (!isAlignedUserAddress(attachmentsBegin, alignof(std::uintptr_t))
                || attachmentsEnd < attachmentsBegin)
            {
                return;
            }

            std::size_t attachmentCount =
                (attachmentsEnd - attachmentsBegin) / sizeof(std::uintptr_t);
            if (attachmentCount > kMaxRenderablesPerCharacter)
            {
                attachmentCount = kMaxRenderablesPerCharacter;
            }

            const auto* attachments =
                reinterpret_cast<const std::uintptr_t*>(attachmentsBegin);
            for (std::size_t index = 0; index < attachmentCount; ++index)
            {
                const std::uintptr_t attachment = attachments[index];
                if (!isAlignedUserAddress(attachment, alignof(std::uintptr_t)))
                {
                    break;
                }

                const auto activeCount = *reinterpret_cast<const std::int32_t*>(
                    attachment + kAnimalAttachmentActiveOffset);
                if (activeCount <= 0)
                {
                    break;
                }

                const auto renderable = *reinterpret_cast<const std::uintptr_t*>(
                    attachment + kAnimalAttachmentRenderableOffset);
                updateSingleRenderable(renderable, mode);
            }
        }

        void applyOrangeOutline(
            const std::uintptr_t characterAddress,
            const RenderableHighlightMode mode) noexcept
        {
            auto* currentPtr = reinterpret_cast<CurrentOutlineColor*>(
                characterAddress + kCharacterCurrentOutlineColorOffset);
            if (!currentColorEquals(*currentPtr, kDesiredCurrentOutline))
            {
                writeCurrentColor(*currentPtr, kDesiredCurrentOutline);
            }

            auto* outlinePtr = reinterpret_cast<OutlineParameters*>(
                characterAddress + kCharacterOutlineColorOffset);
            if (!outlineEquals(*outlinePtr, kDesiredOutline))
            {
                writeOutlineParameters(*outlinePtr, kDesiredOutline);
            }

            updateCharacterRenderables(characterAddress, mode);

            if (!outlineActiveLogged.exchange(true, std::memory_order_relaxed))
            {
                platform::writeLogLine("First animal 3D outline active");
            }
        }

        void clearOrangeOutlineIfOwned(const std::uintptr_t characterAddress) noexcept
        {
            auto* outlinePtr = reinterpret_cast<OutlineParameters*>(
                characterAddress + kCharacterOutlineColorOffset);
            if (isOurOrangeOutline(*outlinePtr))
            {
                writeOutlineParameters(*outlinePtr, kClearedOutline);
                *reinterpret_cast<float*>(
                    characterAddress + kCharacterCurrentOutlineAlphaOffset) = 0.0f;
                updateCharacterRenderables(
                    characterAddress,
                    RenderableHighlightMode::Restored);
            }
        }

        std::uint8_t __fastcall characterUpdateLodDetour(void* character) noexcept
        {
            if (originalCharacterUpdateLod == nullptr)
            {
                return 0;
            }

            const std::uint8_t result = originalCharacterUpdateLod(character);
            const auto characterAddress = reinterpret_cast<std::uintptr_t>(character);
            if (shouldHighlightCharacter(characterAddress))
            {
                auto* lodFlags = reinterpret_cast<std::uint32_t*>(
                    characterAddress + kCharacterLodFlagsOffset);
                *lodFlags &= ~kCharacterLodKeepAnimatedVisibleMask;
            }

            return result;
        }

        void __fastcall characterUpdateOutlineDetour(
            void* character,
            const float deltaTime) noexcept
        {
            if (originalCharacterUpdateOutline == nullptr)
            {
                return;
            }

            const auto characterAddress = reinterpret_cast<std::uintptr_t>(character);
            std::uintptr_t animalAddress = 0;
            if (resolveAnimalFromCharacter(characterAddress, animalAddress))
            {
                if (isAnimalAlive(animalAddress) && evaluateHighlightTarget(characterAddress))
                {
                    applyOrangeOutline(characterAddress, RenderableHighlightMode::Enabled);
                    return;
                }

                clearOrangeOutlineIfOwned(characterAddress);
            }

            originalCharacterUpdateOutline(character, deltaTime);
        }

        void __fastcall animalUpdateDetour(void* animal, void* updateContext) noexcept
        {
            if (originalAnimalUpdate == nullptr)
            {
                return;
            }

            originalAnimalUpdate(animal, updateContext);

            const auto animalAddress = reinterpret_cast<std::uintptr_t>(animal);
            if (!isAlignedUserAddress(animalAddress, alignof(std::uintptr_t)))
            {
                return;
            }

            const auto characterAddress = *reinterpret_cast<const std::uintptr_t*>(
                animalAddress + kAnimalCharacterOffset);
            std::uintptr_t verifiedAnimal = 0;
            if (!resolveAnimalFromCharacter(characterAddress, verifiedAnimal)
                || verifiedAnimal != animalAddress)
            {
                return;
            }

            const bool alive = isAnimalAlive(animalAddress);
            if (alive && evaluateHighlightTarget(characterAddress))
            {
                applyOrangeOutline(
                    characterAddress,
                    RenderableHighlightMode::EnabledClearSurfaceGlow);
            }
            else
            {
                clearOrangeOutlineIfOwned(characterAddress);
            }

            updateOrganHighlightForAnimal(
                animalAddress,
                characterAddress,
                reinterpret_cast<std::uintptr_t>(updateContext),
                alive);
        }

        void __fastcall animalClearOutlineDetour(void* character) noexcept
        {
            if (originalClearCharacterOutline == nullptr)
            {
                return;
            }

            if (shouldHighlightCharacter(reinterpret_cast<std::uintptr_t>(character)))
            {
                return;
            }

            originalClearCharacterOutline(character);
        }

        void __fastcall animalSetOutlineDetour(
            void* character,
            const float* rgba,
            const float fadeIn,
            const float fadeOut,
            const float duration) noexcept
        {
            if (originalSetCharacterOutline == nullptr)
            {
                return;
            }

            if (shouldHighlightCharacter(reinterpret_cast<std::uintptr_t>(character)))
            {
                return;
            }

            originalSetCharacterOutline(character, rgba, fadeIn, fadeOut, duration);
        }
    }

    bool installAnimalOutlines() noexcept
    {
        if (!isAvailable())
        {
            return false;
        }

        outlineActiveLogged.store(false, std::memory_order_relaxed);

        const auto outlineTarget = reinterpret_cast<void*>(
            base() + kCharacterUpdateOutlineRva);
        originalCharacterUpdateOutline =
            reinterpret_cast<CharacterUpdateOutlineFunction>(outlineTarget);
        const auto outlineCallSite = reinterpret_cast<void*>(
            base() + kCharacterUpdateOutlineCallSiteRva);

        const auto lodTarget = reinterpret_cast<void*>(
            base() + kCharacterUpdateLodRva);
        originalCharacterUpdateLod =
            reinterpret_cast<CharacterUpdateLodFunction>(lodTarget);
        const auto lodCallSite = reinterpret_cast<void*>(
            base() + kCharacterUpdateLodCallSiteRva);

        const auto animalTarget = reinterpret_cast<void*>(
            base() + kAnimalUpdateRva);
        originalAnimalUpdate =
            reinterpret_cast<AnimalUpdateFunction>(animalTarget);
        const auto animalCallSite = reinterpret_cast<void*>(
            base() + kAnimalUpdateCallSiteRva);

        const auto clearTarget = reinterpret_cast<void*>(
            base() + kClearCharacterOutlineRva);
        originalClearCharacterOutline =
            reinterpret_cast<ClearCharacterOutlineFunction>(clearTarget);
        const auto clearCallSite = reinterpret_cast<void*>(
            base() + kAnimalClearOutlineCallSiteRva);

        const auto setTarget = reinterpret_cast<void*>(
            base() + kSetCharacterOutlineRva);
        originalSetCharacterOutline =
            reinterpret_cast<SetCharacterOutlineFunction>(setTarget);
        const auto setCallSite = reinterpret_cast<void*>(
            base() + kAnimalSetOutlineCallSiteRva);

        std::size_t installedCount = 0;
        if (platform::installCallHook(
                outlineCallSite,
                reinterpret_cast<void*>(&characterUpdateOutlineDetour),
                outlineTarget))
        {
            ++installedCount;
        }

        if (platform::installCallHook(
                lodCallSite,
                reinterpret_cast<void*>(&characterUpdateLodDetour),
                lodTarget))
        {
            ++installedCount;
        }

        if (platform::installCallHook(
                animalCallSite,
                reinterpret_cast<void*>(&animalUpdateDetour),
                animalTarget))
        {
            ++installedCount;
        }

        if (platform::installCallHook(
                clearCallSite,
                reinterpret_cast<void*>(&animalClearOutlineDetour),
                clearTarget))
        {
            ++installedCount;
        }

        if (platform::installCallHook(
                setCallSite,
                reinterpret_cast<void*>(&animalSetOutlineDetour),
                setTarget))
        {
            ++installedCount;
        }

        if (installOutlineCullingHook())
        {
            ++installedCount;
        }

        platform::TextBuilder builder;
        builder.append("Animal outline hooks installed (");
        builder.appendUnsigned(installedCount);
        builder.append("/6)");
        platform::writeLogLine(builder.c_str());

        return installedCount == 6;
    }

    bool resolveAnimalFromCharacter(
        const std::uintptr_t characterAddress,
        std::uintptr_t& animalAddress) noexcept
    {
        animalAddress = 0;

        if (!isAlignedUserAddress(characterAddress, alignof(std::uintptr_t)))
        {
            return false;
        }

        const auto componentAddress = *reinterpret_cast<const std::uintptr_t*>(
            characterAddress + kCharacterAnimalComponentOffset);
        if (!isAlignedUserAddress(componentAddress, alignof(std::uintptr_t)))
        {
            return false;
        }

        const auto candidateAnimal = *reinterpret_cast<const std::uintptr_t*>(
            componentAddress + kAnimalComponentOwnerOffset);
        if (!isAlignedUserAddress(candidateAnimal, alignof(std::uintptr_t)))
        {
            return false;
        }

        const auto ownerCharacter = *reinterpret_cast<const std::uintptr_t*>(
            candidateAnimal + kAnimalCharacterOffset);
        if (ownerCharacter != characterAddress)
        {
            return false;
        }

        const auto outlineFlags = *reinterpret_cast<const std::uint32_t*>(
            candidateAnimal + kAnimalOutlineFlagOffset);
        if ((outlineFlags & kAnimalOutlineEnabledBit) == 0u)
        {
            return false;
        }

        const auto speciesAddress = *reinterpret_cast<const std::uintptr_t*>(
            candidateAnimal + kAnimalSpeciesOffset);
        if (!isAlignedUserAddress(speciesAddress, alignof(std::uintptr_t)))
        {
            return false;
        }

        const auto excludedFlag = *reinterpret_cast<const std::uint8_t*>(
            speciesAddress + kSpeciesExcludedFlagOffset);
        if (excludedFlag != 0u)
        {
            return false;
        }

        animalAddress = candidateAnimal;
        return true;
    }

    bool isAnimalAlive(const std::uintptr_t animalAddress) noexcept
    {
        const auto lifecycleState = *reinterpret_cast<const std::uint8_t*>(
            animalAddress + kAnimalLifecycleStateOffset);
        const auto shifted = static_cast<std::uint8_t>(lifecycleState - 7u);
        return shifted > 2u;
    }

    bool readCharacterWorldCenter(
        const std::uintptr_t characterAddress,
        float& worldX,
        float& worldY,
        float& worldZ) noexcept
    {
        if (!isAlignedUserAddress(characterAddress, alignof(std::uintptr_t)))
        {
            return false;
        }

        const std::uint32_t primaryCount = *reinterpret_cast<const std::uint32_t*>(
            characterAddress + kCharacterPrimaryRenderableCountOffset);
        if (primaryCount > 0u && primaryCount <= 256u)
        {
            const std::uintptr_t renderable = readCharacterRenderableAt(characterAddress, 0u);
            if (isAlignedUserAddress(renderable, alignof(std::uintptr_t))
                && platform::isReadable(
                    reinterpret_cast<const void*>(renderable + kRenderableWorldBoundsOffset),
                    6u * sizeof(float)))
            {
                const auto* bounds = reinterpret_cast<const float*>(
                    renderable + kRenderableWorldBoundsOffset);
                if (bounds[3] >= bounds[0]
                    && bounds[4] >= bounds[1]
                    && bounds[5] >= bounds[2])
                {
                    worldX = (bounds[0] + bounds[3]) * 0.5f;
                    worldY = (bounds[1] + bounds[4]) * 0.5f;
                    worldZ = (bounds[2] + bounds[5]) * 0.5f;
                    return true;
                }
            }
        }

        const auto* position = reinterpret_cast<const float*>(
            characterAddress + kPawnPositionOffset);
        worldX = position[0];
        worldY = position[1] + 0.75f;
        worldZ = position[2];
        return true;
    }

    void updateCullingSlotIgnoreSize(
        const std::uintptr_t renderableAddress,
        const bool enableIgnoreSize) noexcept
    {
        const auto handle = *reinterpret_cast<const std::uint16_t*>(
            renderableAddress + kRenderableCullingHandleOffset);
        const auto cullingWorld = *reinterpret_cast<const std::uintptr_t*>(
            renderableAddress + kRenderableCullingWorldOffset);
        if (handle == kInvalidCullingHandle
            || !isAlignedUserAddress(cullingWorld, alignof(std::uintptr_t)))
        {
            return;
        }

        const auto remapTable = *reinterpret_cast<const std::uintptr_t*>(
            cullingWorld + kCullingWorldRemapOffset);
        const auto flagsArray = *reinterpret_cast<const std::uintptr_t*>(
            cullingWorld + kCullingWorldFlagsOffset);
        if (!isAlignedUserAddress(remapTable, alignof(std::uint16_t))
            || !isAlignedUserAddress(flagsArray, alignof(std::uint32_t)))
        {
            return;
        }

        const auto slot = *reinterpret_cast<const std::uint16_t*>(
            remapTable + static_cast<std::size_t>(handle) * sizeof(std::uint16_t));
        auto* slotFlags = reinterpret_cast<volatile long*>(
            flagsArray + static_cast<std::size_t>(slot) * sizeof(std::uint32_t));
        const long ignoreSizeBit = static_cast<long>(kCullingSlotIgnoreSizeBit);

        if (enableIgnoreSize)
        {
            if ((*slotFlags & ignoreSizeBit) == 0)
            {
                _InterlockedOr(slotFlags, ignoreSizeBit);
            }
        }
        else
        {
            if ((*slotFlags & ignoreSizeBit) != 0)
            {
                _InterlockedAnd(slotFlags, ~ignoreSizeBit);
            }
        }
    }
}
