#pragma once

#include <cstdint>

namespace Offsets {

// Keep world resolution on the GEngine/GameViewport path. DirectUWorld is
// recorded for diagnostics but is intentionally not used by the runtime path.
inline constexpr std::uint64_t DirectUWorld = 0x1B156318;
inline constexpr std::uint64_t GEngine = 0x1B157C88;
inline constexpr std::uint64_t GameViewport = 0xB70;
inline constexpr std::uint64_t ViewportWorld = 0x78;

inline constexpr std::uint64_t WorldGameState = 0x1C0;
inline constexpr std::uint64_t WorldOwningGameInstance = 0x238;
inline constexpr std::uint64_t WorldCameraLocationPointer = 0x168;
inline constexpr std::uint64_t WorldCameraRotationPointer = 0x178;
inline constexpr std::uint64_t WorldSeconds = 0x188;
inline constexpr std::uint64_t CameraFov = 0x374;
inline constexpr std::uint64_t WorldLevels = 0x1D8;
inline constexpr std::uint64_t EncodedCameraRotationA = 0x0;
inline constexpr std::uint64_t EncodedCameraRotationB = 0x20;
inline constexpr std::uint64_t EncodedCameraRotationC = 0x1D0;
inline constexpr std::uint64_t GameStatePlayerArray = 0x288;
inline constexpr std::uint64_t GameInstanceLocalPlayers = 0x38;
inline constexpr std::uint64_t LocalPlayerPlayerController = 0x30;
inline constexpr std::uint64_t PlayerControllerAcknowledgedPawn = 0x318;
inline constexpr std::uint64_t PlayerControllerCameraManager = 0x328;
inline constexpr std::uint64_t ControllerControlRotation = 0x2E8;
inline constexpr std::uint64_t PlayerControllerNetConnection = 0x4A8;
inline constexpr std::uint64_t PlayerControllerRotationInput = 0x4B0;
inline constexpr std::uint64_t PlayerControllerPlayerAimOffset = 0x2310;
inline constexpr std::uint64_t PlayerControllerWeaponRecoilOffset = 0x2328;
inline constexpr std::uint64_t PlayerControllerWeaponOffsetCorrection = 0x2340;
inline constexpr std::uint64_t PlayerCameraManagerCameraCache = 0x1590;
inline constexpr std::uint64_t CameraCachePov = 0x10;
inline constexpr std::uint64_t MinimalViewLocation = 0x0;
inline constexpr std::uint64_t MinimalViewRotation = 0x18;
inline constexpr std::uint64_t MinimalViewFov = 0x30;
inline constexpr std::uint64_t PlayerStatePawnPrivate = 0x2E8;
inline constexpr std::uint64_t PlayerStateBotFlags = 0x27A;
inline constexpr std::uint8_t PlayerStateBotMask = 1U << 3;
inline constexpr std::uint64_t PlayerStateTeamIndex = 0xF61;
inline constexpr std::uint64_t ActorRootComponent = 0x1B0;
inline constexpr std::uint64_t AthenaSupplyDropSpawnOffsetZ = 0xA30;
inline constexpr std::uint64_t LevelActors = 0x38;
inline constexpr std::uint64_t PickupFlags = 0x28C;
inline constexpr std::uint8_t PickupPickedUpMask = 1U << 2;
inline constexpr std::uint64_t PickupPrimaryItemEntry = 0x368;
inline constexpr std::uint64_t ItemEntryItemDefinition = 0x10;
inline constexpr std::uint64_t ItemDefinitionItemName = 0x38;
inline constexpr std::uint64_t PickupEffectParentPickupActor = 0x290;
inline constexpr std::uint64_t PickupsParentRarityLevel = 0x374;
inline constexpr std::uint64_t TextDataString = 0x20;
inline constexpr std::uint64_t TextDataLength = 0x28;
inline constexpr std::uint64_t TextDataSdkString = 0x28;
inline constexpr std::uint64_t TextDataSdkLength = 0x30;
inline constexpr std::uint64_t ObjectProtectedSlots = 0x20;
inline constexpr std::uint64_t ObjectClass = 0x20;
inline constexpr std::uint64_t PlayerPawnMesh = 0x2F0;
inline constexpr std::uint64_t PawnCurrentWeapon = 0x9A0;
inline constexpr std::uint64_t PlayerPawnCurrentVehicle = 0x2B00;
inline constexpr std::uint64_t PlayerPawnCustomDepthComponent = 0x4AF8;
inline constexpr std::uint64_t CustomDepthEnabledFlags = 0xD0;
inline constexpr std::uint64_t CustomDepthDefaultHighlightingData = 0xF0;
inline constexpr std::uint64_t PrimitiveRenderCustomDepthFlags = 0x291;
inline constexpr std::uint64_t PrimitiveCustomDepthStencilValue = 0x2A4;
inline constexpr std::uint64_t SkeletalMeshComponentToWorld = 0x1E0;
inline constexpr std::uint64_t SkeletalMeshBoneArray = 0x660;
inline constexpr std::uint64_t SkeletalMeshBoneArrayCache = 0x670;
inline constexpr std::uint64_t PawnPlayerState = 0x290;
inline constexpr std::uint64_t SceneComponentRelativeLocation = 0x140;
inline constexpr std::uint64_t PrimitiveComponentLastRenderTime = 0x290;
inline constexpr std::uint64_t WeaponData = 0x648;

}
