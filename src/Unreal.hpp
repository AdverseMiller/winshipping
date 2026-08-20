#pragma once

#include "memflow.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Unreal {

struct ActorPosition {
    std::uint64_t actor;
    std::uint64_t playerState;
    std::uint64_t mesh;
    std::uint8_t team;
    float lastRenderTime;
    double x;
    double y;
    double z;
};

struct ActorSnapshot {
    std::uint64_t world{};
    std::uint64_t gameState{};
    std::uint64_t localController{};
    std::uint64_t localPawn{};
    std::uint64_t cameraLocationPointer{};
    std::uint64_t cameraRotationPointer{};
    std::uint8_t localTeam{0xFF};
    double worldSeconds{};
    std::size_t rawCount{};
    std::size_t filteredBotCount{};
    std::size_t reportedCount{};
    std::size_t missingPawnCount{};
    std::size_t missingRootCount{};
    std::size_t invalidLocationCount{};
    bool hasLocalPosition{};
    double localX{};
    double localY{};
    double localZ{};
    std::vector<ActorPosition> positions;
};

struct AimResult {
    bool activationDown;
    bool wroteAngles;
    std::uint64_t localPawn;
    std::uint64_t target;
    std::uint8_t localTeam;
    std::uint8_t targetTeam;
    int targetBone;
    double targetDistanceMeters;
    double angularDistance;
    double cameraPitch;
    double cameraYaw;
    double desiredPitch;
    double desiredYaw;
    double outputPitch;
    double outputYaw;
};

struct HighlightResult {
    std::size_t eligible;
    std::size_t active;
    std::size_t applied;
    std::size_t restored;
    std::size_t failed;
};

enum class ItemRarity : std::uint8_t {
    Common,
    Uncommon,
    Rare,
    Epic,
    Legendary,
    Mythic,
    Transcendent,
    Unattainable
};

struct GroundItemPosition {
    std::uint64_t actor;
    std::uint64_t definition;
    std::string name;
    std::optional<ItemRarity> rarity;
    double x;
    double y;
    double z;
};

struct GroundItemSnapshot {
    std::size_t scannedActors;
    std::size_t pendingActors;
    std::vector<GroundItemPosition> positions;
};

enum class WeaponCategory : std::uint8_t {
    Unknown,
    Pistol,
    Shotgun,
    Rifle,
    Smg,
    Sniper,
    Launcher,
    Bow,
    Minigun,
    Melee,
    Utility,
    Unarmed,
    Count
};

struct WeaponSnapshot {
    std::uint64_t actor;
    std::uint64_t definition;
    std::string name;
    WeaponCategory category;
};

enum class BoneArraySource {
    Primary,
    Cache
};

struct BonePosition {
    int index;
    double x;
    double y;
    double z;
};

struct BoneSnapshot {
    std::uint64_t actor;
    std::uint64_t mesh;
    std::uint64_t boneArray;
    BoneArraySource source;
    std::int32_t reportedCount;
    double actorX;
    double actorY;
    double actorZ;
    std::vector<BonePosition> positions;
};

std::optional<ActorSnapshot> actorPositions(ProcessInstance<>& memory, std::uint64_t imageBase, bool ignoreIsABot = false);
std::optional<AimResult> aimAtNearestPawn(ProcessInstance<>& memory, const ActorSnapshot& snapshot, double smoothing, bool activationDown, bool ignoreTeams = false);
std::optional<AimResult> aimAtNearestLlama(ProcessInstance<>& memory, const ActorSnapshot& snapshot, bool activationDown);
GroundItemSnapshot findGroundItems(ProcessInstance<>& memory, const ActorSnapshot& snapshot, std::string_view itemName, std::optional<ItemRarity> rarity);
std::optional<std::uint64_t> currentVehicle(ProcessInstance<>& memory, const ActorSnapshot& snapshot);
std::optional<WeaponSnapshot> currentWeapon(ProcessInstance<>& memory, const ActorSnapshot& snapshot);
std::string_view weaponCategoryName(WeaponCategory category);
std::optional<AimResult> aimAtNearestGroundItem(ProcessInstance<>& memory, const ActorSnapshot& snapshot, const GroundItemSnapshot& items, bool activationDown);
HighlightResult updatePlayerHighlights(ProcessInstance<>& memory, const ActorSnapshot& snapshot, bool enabled, bool ignoreTeams = false);
HighlightResult restorePlayerHighlights(ProcessInstance<>& memory);
bool clearAimOffsets(ProcessInstance<>& memory, std::uint64_t world);
std::vector<BoneSnapshot> probeBones(ProcessInstance<>& memory, const ActorSnapshot& snapshot);

}
