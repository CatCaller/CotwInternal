#pragma once

#include <cstddef>
#include <cstdint>

namespace cotw::game
{
    inline constexpr std::uintptr_t kPlayerVtableRva = 0x1EE24A0;
    inline constexpr std::uintptr_t kPlayerManagerRva = 0x27F1478;
    inline constexpr std::uintptr_t kAnimalWorldRva = 0x27C5DC8;
    inline constexpr std::uintptr_t kSenseSystemWorldRva = 0x27C5D50;
    inline constexpr std::uintptr_t kDynamicRenderSceneRva = 0x27C3A88;
    inline constexpr std::uintptr_t kModelResourceManagerRva = 0x27B8F90;
    inline constexpr std::uintptr_t kViewProjectionRva = 0x27E7404;
    inline constexpr std::uintptr_t kCameraPositionRva = 0x27E72F4;
    inline constexpr std::uintptr_t kIdentityMatrix4x4Rva = 0x25AA5F0;

    inline constexpr std::uintptr_t kDynamicSceneVisibleCallbackRva = 0x501340;
    inline constexpr std::uintptr_t kCullDynamicSceneRva = 0xD9DEA0;
    inline constexpr std::uintptr_t kCullDynamicSceneCallSiteRva = 0x509178;
    inline constexpr std::uintptr_t kSenseCopyFunctionRva = 0x6014B0;
    inline constexpr std::uintptr_t kSenseCopyCallSiteRva = 0x5F931D;
    inline constexpr std::uintptr_t kDesiredVelocityTransformRva = 0xDAB70;
    inline constexpr std::uintptr_t kDesiredVelocityTransformCallSiteRva = 0x6FE14D;
    inline constexpr std::uintptr_t kClearCharacterOutlineRva = 0x69E480;
    inline constexpr std::uintptr_t kSetCharacterOutlineRva = 0x6A0D70;
    inline constexpr std::uintptr_t kCharacterUpdateOutlineRva = 0x6B1810;
    inline constexpr std::uintptr_t kCharacterUpdateOutlineCallSiteRva = 0x6B31FF;
    inline constexpr std::uintptr_t kCharacterUpdateLodRva = 0x6B0670;
    inline constexpr std::uintptr_t kCharacterUpdateLodCallSiteRva = 0x6B3ED9;
    inline constexpr std::uintptr_t kAnimalSetOutlineCallSiteRva = 0x8B1017;
    inline constexpr std::uintptr_t kAnimalUpdateRva = 0x8BF8A0;
    inline constexpr std::uintptr_t kAnimalUpdateCallSiteRva = 0x8BEFDB;
    inline constexpr std::uintptr_t kAnimalClearOutlineCallSiteRva = 0x8BFDF8;
    inline constexpr std::uintptr_t kHasModelResourceRva = 0x2A69D0;
    inline constexpr std::uintptr_t kResetRenderableTransformRva = 0x2A6750;
    inline constexpr std::uintptr_t kRemoveRenderableFromSceneRva = 0x500290;
    inline constexpr std::uintptr_t kClearAnimalOrganMapRva = 0x6B8CA0;
    inline constexpr std::uintptr_t kAddAnimalOrgansRva = 0x876230;
    inline constexpr std::uintptr_t kUpdateAnimalOrgansRva = 0x8BF2F0;
    inline constexpr std::uintptr_t kUpdateAnimalOrgansCallSiteRva = 0x68B5EE;
    inline constexpr std::uintptr_t kCharacterStepFunctionRva = 0x25A310;
    inline constexpr std::uintptr_t kCharacterStepCallSites[] = {
        0x66C338,
        0x6ABC47,
        0x6AFA23,
        0x6AFE56,
        0x6B0231,
        0x6B050D,
    };

    inline constexpr std::size_t kPlayerManagerEntryOffset = 0x60;
    inline constexpr std::size_t kPlayerManagerOwnerOffset = 0x160;
    inline constexpr std::size_t kPlayerManagerActivePawnOffset = 0x170;
    inline constexpr std::size_t kPlayerOwnerBias = 0x10;

    inline constexpr std::size_t kPlayerPawnOffset = 0x68;
    inline constexpr std::size_t kPawnAimStateOffset = 0x1AFA;
    inline constexpr std::uint8_t kPawnAimStateActiveBit = 0x01u;
    inline constexpr std::size_t kPawnAnimationComponentOffset = 0x2168;
    inline constexpr std::size_t kPawnPhysicsOffset = 0x1AD0;
    inline constexpr std::size_t kPawnPositionOffset = 0x2294;
    inline constexpr std::size_t kPawnOwnerOffset = 0x2400;
    inline constexpr std::size_t kCharacterInputVelocityOffset = 0x04;
    inline constexpr std::size_t kPhysicsContextOffset = 0xC8;
    inline constexpr std::size_t kPhysicsContextClampEnableOffset = 0x28;
    inline constexpr std::size_t kPhysicsContextMaxVelocityOffset = 0x30;
    inline constexpr std::size_t kCharacterRenderableInterfaceOffset = 0x08;
    inline constexpr std::size_t kCharacterGetRenderableVtableOffset = 0xE8;
    inline constexpr std::size_t kCharacterPrimaryRenderableCountOffset = 0x1098;
    inline constexpr std::size_t kCharacterLodFlagsOffset = 0x21BC;
    inline constexpr std::uint32_t kCharacterLodHideRenderablesBit = 0x200u;
    inline constexpr std::uint32_t kCharacterLodKeepAnimatedVisibleMask = 0x23A2u;
    inline constexpr std::size_t kCharacterCurrentOutlineColorOffset = 0x2458;
    inline constexpr std::size_t kCharacterCurrentOutlineAlphaOffset = 0x2464;
    inline constexpr std::size_t kCharacterOutlineColorOffset = 0x2468;
    inline constexpr std::size_t kCharacterOutlineAlphaOffset = 0x2474;
    inline constexpr std::size_t kCharacterOutlineDurationOffset = 0x2478;
    inline constexpr std::size_t kCharacterOutlineFadeInOffset = 0x247C;
    inline constexpr std::size_t kCharacterOutlineFadeOutOffset = 0x2480;
    inline constexpr std::size_t kCharacterAnimalComponentOffset = 0x2AC8;
    inline constexpr std::size_t kCharacterOcclusionTimerOffset = 0x2AF8;

    inline constexpr std::size_t kAnimalWorldManagerOffset = 0xB8;
    inline constexpr std::size_t kAnimalManagerVectorOffset = 0x28;
    inline constexpr std::size_t kAnimalManagerVectorEndOffset = 0x30;
    inline constexpr std::size_t kAnimalComponentAttachmentsBeginOffset = 0xA0;
    inline constexpr std::size_t kAnimalComponentAttachmentsEndOffset = 0xA8;
    inline constexpr std::size_t kAnimalComponentOwnerOffset = 0x168;
    inline constexpr std::size_t kAnimalAttachmentActiveOffset = 0x18C;
    inline constexpr std::size_t kAnimalAttachmentRenderableOffset = 0x198;
    inline constexpr std::size_t kAnimalCharacterOffset = 0x00;
    inline constexpr std::size_t kAnimalSpeciesOffset = 0x28;
    inline constexpr std::size_t kAnimalOutlineFlagOffset = 0xD4;
    inline constexpr std::uint32_t kAnimalGenderMask = 0x3u;
    inline constexpr std::uint32_t kAnimalOutlineEnabledBit = 0x4u;
    inline constexpr std::size_t kAnimalLifecycleStateOffset = 0xE0;
    inline constexpr std::size_t kAnimalPositionOffset = 0x148;
    inline constexpr std::size_t kAnimalGroundedYOffset = 0x154;
    inline constexpr std::size_t kAnimalOrganMapOffset = 0x218;
    inline constexpr std::size_t kAnimalOrganListOffset = 0x220;
    inline constexpr std::size_t kAnimalOrganCountOffset = 0x228;
    inline constexpr std::size_t kSpeciesNameOffset = 0x68;
    inline constexpr std::size_t kSpeciesRagdollPathOffset = 0xC0;
    inline constexpr std::size_t kSpeciesExcludedFlagOffset = 0x121;
    inline constexpr std::size_t kSpeciesOrganMapOffset = 0x330;

    inline constexpr std::size_t kOrganTreeNodeLeftOffset = 0x00;
    inline constexpr std::size_t kOrganTreeNodeParentOffset = 0x08;
    inline constexpr std::size_t kOrganTreeNodeRightOffset = 0x10;
    inline constexpr std::size_t kOrganTreeNodeIsNilOffset = 0x19;
    inline constexpr std::size_t kOrganTreeNodeHashOffset = 0x20;
    inline constexpr std::size_t kOrganTreeNodeNameOffset = 0x38;
    inline constexpr std::size_t kOrganTreeNodeGenderOffset = 0x5C;
    inline constexpr std::size_t kOrganListNodeNextOffset = 0x00;
    inline constexpr std::size_t kOrganListNodeRenderableOffset = 0x18;
    inline constexpr std::size_t kUpdateContextTickOffset = 0x08;
    inline constexpr std::size_t kUpdateContextAnimalManagerOffset = 0x670;

    inline constexpr std::size_t kRenderableFlagsOffset = 0x0C;
    inline constexpr std::uint32_t kRenderableVisibleBit = 0x1u;
    inline constexpr std::uint32_t kRenderableMaxDistanceCullBit = 0x2u;
    inline constexpr std::uint32_t kRenderableRegisteredInSceneBit = 0x10u;
    inline constexpr std::uint32_t kRenderableSurfaceGlowBit = 0x40u;
    inline constexpr std::uint32_t kRenderableDisableDitherBit = 0x1000u;
    inline constexpr std::uint32_t kRenderableIgnoreSizeCullBit = 0x4000u;
    inline constexpr std::uint32_t kRenderableSpawnFadeActiveBit = 0x8000u;
    inline constexpr std::uint32_t kRenderableDitherFadeActiveBit = 0x10000u;
    inline constexpr std::uint32_t kRenderableOutlineActiveBit = 0x400000u;
    inline constexpr std::size_t kRenderableCullingWorldOffset = 0x20;
    inline constexpr std::size_t kRenderableCullingHandleOffset = 0x28;
    inline constexpr std::size_t kRenderableDirtyFlagOffset = 0x38;
    inline constexpr std::size_t kRenderableOpacityByteOffset = 0x69;
    inline constexpr std::size_t kRenderableOutlineTypeOffset = 0x6C;
    inline constexpr std::size_t kRenderableOutlineRgbaOffset = 0x6E;
    inline constexpr std::size_t kRenderableWorldBoundsOffset = 0x140;

    inline constexpr std::size_t kCullingWorldAabbBeginOffset = 0x08;
    inline constexpr std::size_t kCullingWorldAabbEndOffset = 0x10;
    inline constexpr std::size_t kCullingWorldAabbStride = 24;
    inline constexpr std::size_t kCullingWorldUserDataOffset = 0x28;
    inline constexpr std::size_t kCullingWorldFlagsOffset = 0x48;
    inline constexpr std::size_t kCullingWorldRemapOffset = 0x68;
    inline constexpr std::uint32_t kCullingSlotIgnoreSizeBit = 0x40000u;
    inline constexpr std::uint32_t kCullingSlotDisabledBit = 0x80000u;

    inline constexpr std::size_t kSenseSystemOwnerOffset = 0x20290;
    inline constexpr std::size_t kSenseSystemOwnerBias = 0x18;
    inline constexpr std::size_t kSenseSystemManagerOffset = 0x220;

    inline constexpr std::size_t kSensesStartOffset = 112;
    inline constexpr std::size_t kSensesEndOffset = 604;
    inline constexpr std::size_t kScentStartOffset = 352;
    inline constexpr std::size_t kScentEndOffset = 604;

    inline constexpr std::size_t kMaximumAnimals = 512;
}
