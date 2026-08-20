#include "Unreal.hpp"

#include "Config.hpp"
#include "Offsets.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

struct RemoteArray {
    std::uint64_t data;
    std::int32_t count;
    std::int32_t capacity;
};

struct Vector3 {
    double x;
    double y;
    double z;
};

struct Rotator {
    double pitch;
    double yaw;
    double roll;
};

struct Quaternion {
    double x;
    double y;
    double z;
    double w;
};

struct Transform {
    Quaternion rotation;
    Vector3 translation;
    std::array<std::uint8_t, 8> translationPadding;
    Vector3 scale;
    std::array<std::uint8_t, 8> scalePadding;
};

struct ResolvedBone {
    Vector3 position;
    int index;
};

struct CachedAimBones {
    int head = -1;
    int torso = -1;
};

struct LlamaAimTarget {
    std::uint64_t actor;
    Vector3 position;
};

struct HighlightingData {
    std::uint8_t flags;
    std::uint8_t localPlayerStencil;
    std::uint8_t friendlyStencil;
    std::uint8_t enemyStencil;
    std::array<std::uint8_t, 4> padding;
};

struct CachedHighlight {
    std::uint64_t component;
    HighlightingData original;
    HighlightingData applied;
};

struct CameraView {
    Vector3 location;
    Rotator rotation;
};

static_assert(sizeof(Transform) == 0x60);
static_assert(sizeof(HighlightingData) == 8);

constexpr std::uint8_t ShouldHighlightMask = 1U << 0;
constexpr std::uint8_t PlayerHighlightStencil = 12;
std::unordered_map<std::uint64_t, CachedHighlight> cachedHighlights;

bool plausiblePointer(std::uint64_t address) {
    return address >= 0x10000 && address <= 0x00007FFFFFFFFFFF && (address & 0x7) == 0;
}

template <typename T>
std::optional<T> read(ProcessInstance<>& memory, std::uint64_t address) {
    T value{};
    auto* bytes = reinterpret_cast<std::uint8_t*>(&value);
    if (memory.read_raw_into(address, CSliceMut<std::uint8_t>(reinterpret_cast<char*>(bytes), sizeof(T))) != 0) return std::nullopt;
    return value;
}

template <typename T>
bool write(ProcessInstance<>& memory, std::uint64_t address, const T& value) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
    return memory.write_raw(address, CSliceRef<std::uint8_t>(reinterpret_cast<const char*>(bytes), sizeof(T))) == 0;
}

template <typename T>
void addRead(std::vector<ReadData>& reads, std::uint64_t address, T& value) {
    auto* bytes = reinterpret_cast<std::uint8_t*>(&value);
    reads.push_back(ReadData{address, CSliceMut<std::uint8_t>(reinterpret_cast<char*>(bytes), sizeof(T))});
}

bool runReads(ProcessInstance<>& memory, std::vector<ReadData>& reads) {
    return reads.empty() || memory.read_raw_list(CSliceMut<ReadData>(reads)) == 0;
}

std::string lowercaseAscii(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const unsigned char character : value) result.push_back(static_cast<char>(std::tolower(character)));
    return result;
}

bool appendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7F) output.push_back(static_cast<char>(codePoint));
    else if (codePoint <= 0x7FF) {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        if (codePoint >= 0xD800 && codePoint <= 0xDFFF) return false;
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0x10FFFF) {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else return false;
    return true;
}

std::optional<std::string> readTextData(ProcessInstance<>& memory, std::uint64_t textData, std::uint64_t stringOffset, std::uint64_t lengthOffset) {
    const auto stringAddress = read<std::uint64_t>(memory, textData + stringOffset);
    const auto length = read<std::int32_t>(memory, textData + lengthOffset);
    if (!stringAddress.has_value() || !plausiblePointer(*stringAddress) || !length.has_value() || *length < 1 || *length > 256) return std::nullopt;

    std::vector<char16_t> characters(static_cast<std::size_t>(*length));
    auto* bytes = reinterpret_cast<std::uint8_t*>(characters.data());
    if (memory.read_raw_into(*stringAddress, CSliceMut<std::uint8_t>(reinterpret_cast<char*>(bytes), characters.size() * sizeof(char16_t))) != 0) return std::nullopt;
    while (!characters.empty() && characters.back() == u'\0') characters.pop_back();
    if (characters.empty()) return std::nullopt;

    std::string result;
    result.reserve(characters.size());
    for (std::size_t index = 0; index < characters.size(); ++index) {
        std::uint32_t codePoint = characters[index];
        if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
            if (++index >= characters.size()) return std::nullopt;
            const std::uint32_t low = characters[index];
            if (low < 0xDC00 || low > 0xDFFF) return std::nullopt;
            codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
        } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) return std::nullopt;
        if (codePoint < 0x20 || codePoint == 0x7F) return std::nullopt;
        if (!appendUtf8(result, codePoint)) return std::nullopt;
    }
    return result;
}

std::optional<std::string> readItemName(ProcessInstance<>& memory, std::uint64_t definition) {
    const auto textData = read<std::uint64_t>(memory, definition + Offsets::ItemDefinitionItemName);
    if (!textData.has_value() || !plausiblePointer(*textData)) return std::nullopt;
    const auto sdkLayout = readTextData(memory, *textData, Offsets::TextDataSdkString, Offsets::TextDataSdkLength);
    if (sdkLayout.has_value()) return sdkLayout;
    const auto legacyLayout = readTextData(memory, *textData, Offsets::TextDataString, Offsets::TextDataLength);
    if (legacyLayout.has_value()) return legacyLayout;
    for (std::uint64_t stringOffset = 0; stringOffset <= 0x48; stringOffset += sizeof(std::uint64_t)) {
        if (stringOffset == Offsets::TextDataString || stringOffset == Offsets::TextDataSdkString) continue;
        const auto candidate = readTextData(memory, *textData, stringOffset, stringOffset + sizeof(std::uint64_t));
        if (candidate.has_value()) return candidate;
    }
    return std::nullopt;
}

std::optional<Unreal::WeaponCategory> weaponCategoryFromName(std::string_view name) {
    const std::string normalized = lowercaseAscii(name);
    if (normalized.find("sniper") != std::string::npos) return Unreal::WeaponCategory::Sniper;
    if (normalized.find("shotgun") != std::string::npos) return Unreal::WeaponCategory::Shotgun;
    if (normalized.find("submachine") != std::string::npos || normalized.find("smg") != std::string::npos) return Unreal::WeaponCategory::Smg;
    if (normalized.find("pistol") != std::string::npos || normalized.find("revolver") != std::string::npos || normalized.find("hand cannon") != std::string::npos) return Unreal::WeaponCategory::Pistol;
    if (normalized.find("launcher") != std::string::npos) return Unreal::WeaponCategory::Launcher;
    if (normalized.find("minigun") != std::string::npos) return Unreal::WeaponCategory::Minigun;
    if (normalized.find("bow") != std::string::npos) return Unreal::WeaponCategory::Bow;
    if (normalized.find("rifle") != std::string::npos || normalized.find("dmr") != std::string::npos) return Unreal::WeaponCategory::Rifle;
    if (normalized.find("pickaxe") != std::string::npos || normalized.find("harvesting tool") != std::string::npos) return Unreal::WeaponCategory::Melee;
    return std::nullopt;
}

double normalizedAngle(double angle) {
    return std::remainder(angle, 360.0);
}

std::optional<Rotator> lookAt(const Vector3& origin, const Vector3& target) {
    const double dx = target.x - origin.x;
    const double dy = target.y - origin.y;
    const double dz = target.z - origin.z;
    const double horizontal = std::hypot(dx, dy);
    if (!std::isfinite(horizontal) || horizontal < 0.001) return std::nullopt;

    constexpr double radiansToDegrees = 57.295779513082320876;
    const Rotator result{std::atan2(dz, horizontal) * radiansToDegrees, std::atan2(dy, dx) * radiansToDegrees, 0.0};
    if (!std::isfinite(result.pitch) || !std::isfinite(result.yaw)) return std::nullopt;
    return result;
}

std::optional<CameraView> readCamera(ProcessInstance<>& memory, const Unreal::ActorSnapshot& snapshot) {
    if (plausiblePointer(snapshot.cameraLocationPointer) && plausiblePointer(snapshot.cameraRotationPointer)) {
        double encodedA{};
        double encodedB{};
        double encodedC{};
        Vector3 location{};
        std::vector<ReadData> reads;
        reads.reserve(4);
        addRead(reads, snapshot.cameraRotationPointer + Offsets::EncodedCameraRotationA, encodedA);
        addRead(reads, snapshot.cameraRotationPointer + Offsets::EncodedCameraRotationB, encodedB);
        addRead(reads, snapshot.cameraRotationPointer + Offsets::EncodedCameraRotationC, encodedC);
        addRead(reads, snapshot.cameraLocationPointer, location);
        if (runReads(memory, reads) && std::isfinite(encodedA) && std::isfinite(encodedB) && std::isfinite(encodedC) && std::abs(encodedC) <= 1.001 && std::isfinite(location.x) && std::isfinite(location.y) && std::isfinite(location.z)) {
            constexpr double radiansToDegrees = 57.295779513082320876;
            return CameraView{location, Rotator{std::asin(std::clamp(encodedC, -1.0, 1.0)) * radiansToDegrees, std::atan2(-encodedA, encodedB) * radiansToDegrees, 0.0}};
        }
    }

    if (!plausiblePointer(snapshot.localController)) return std::nullopt;
    const auto cameraManager = read<std::uint64_t>(memory, snapshot.localController + Offsets::PlayerControllerCameraManager);
    if (!cameraManager.has_value() || !plausiblePointer(*cameraManager)) return std::nullopt;
    const std::uint64_t pov = *cameraManager + Offsets::PlayerCameraManagerCameraCache + Offsets::CameraCachePov;
    CameraView view{};
    std::vector<ReadData> reads;
    reads.reserve(2);
    addRead(reads, pov + Offsets::MinimalViewLocation, view.location);
    addRead(reads, snapshot.localController + Offsets::ControllerControlRotation, view.rotation);
    if (!runReads(memory, reads)) return std::nullopt;
    if (!std::isfinite(view.rotation.pitch) || !std::isfinite(view.rotation.yaw)) {
        const auto rotation = read<Rotator>(memory, pov + Offsets::MinimalViewRotation);
        if (!rotation.has_value()) return std::nullopt;
        view.rotation = *rotation;
    }
    if (!std::isfinite(view.location.x) || !std::isfinite(view.location.y) || !std::isfinite(view.location.z)) return std::nullopt;
    if (!std::isfinite(view.rotation.pitch) || !std::isfinite(view.rotation.yaw)) return std::nullopt;
    return view;
}

bool isVisible(const Unreal::ActorPosition& actor, double worldSeconds) {
    return plausiblePointer(actor.mesh) && std::isfinite(actor.lastRenderTime) && worldSeconds - static_cast<double>(actor.lastRenderTime) <= 0.06;
}

bool matchingHighlight(const HighlightingData& left, const HighlightingData& right) {
    return std::memcmp(&left, &right, sizeof(HighlightingData)) == 0;
}

HighlightingData playerHighlight(const HighlightingData& original) {
    HighlightingData applied = original;
    applied.flags |= ShouldHighlightMask;
    applied.localPlayerStencil = PlayerHighlightStencil;
    applied.friendlyStencil = PlayerHighlightStencil;
    applied.enemyStencil = PlayerHighlightStencil;
    return applied;
}

bool restoreCachedHighlight(ProcessInstance<>& memory, std::uint64_t pawn, const CachedHighlight& cached) {
    const auto component = read<std::uint64_t>(memory, pawn + Offsets::PlayerPawnCustomDepthComponent);
    if (!component.has_value() || *component != cached.component) return false;
    const auto current = read<HighlightingData>(memory, cached.component + Offsets::CustomDepthDefaultHighlightingData);
    if (!current.has_value()) return false;
    if (!matchingHighlight(*current, cached.applied)) return true;
    if (!write(memory, cached.component + Offsets::CustomDepthDefaultHighlightingData, cached.original)) return false;
    const auto verify = read<HighlightingData>(memory, cached.component + Offsets::CustomDepthDefaultHighlightingData);
    return verify.has_value() && matchingHighlight(*verify, cached.original);
}

bool plausibleTransform(const Transform& transform) {
    const double quaternionNorm = transform.rotation.x * transform.rotation.x + transform.rotation.y * transform.rotation.y + transform.rotation.z * transform.rotation.z + transform.rotation.w * transform.rotation.w;
    if (!std::isfinite(quaternionNorm) || quaternionNorm < 0.5 || quaternionNorm > 1.5) return false;
    if (!std::isfinite(transform.translation.x) || !std::isfinite(transform.translation.y) || !std::isfinite(transform.translation.z)) return false;
    if (!std::isfinite(transform.scale.x) || !std::isfinite(transform.scale.y) || !std::isfinite(transform.scale.z)) return false;
    return std::abs(transform.translation.x) < 10000000.0 && std::abs(transform.translation.y) < 10000000.0 && std::abs(transform.translation.z) < 10000000.0;
}

Vector3 rotateVector(const Quaternion& rotation, const Vector3& vector) {
    const Vector3 doubledCross{
        2.0 * (rotation.y * vector.z - rotation.z * vector.y),
        2.0 * (rotation.z * vector.x - rotation.x * vector.z),
        2.0 * (rotation.x * vector.y - rotation.y * vector.x)
    };
    return Vector3{
        vector.x + rotation.w * doubledCross.x + rotation.y * doubledCross.z - rotation.z * doubledCross.y,
        vector.y + rotation.w * doubledCross.y + rotation.z * doubledCross.x - rotation.x * doubledCross.z,
        vector.z + rotation.w * doubledCross.z + rotation.x * doubledCross.y - rotation.y * doubledCross.x
    };
}

std::optional<Vector3> boneWorldPosition(ProcessInstance<>& memory, std::uint64_t boneArray, int index, const Transform& componentToWorld) {
    const auto bone = read<Transform>(memory, boneArray + static_cast<std::uint64_t>(index) * sizeof(Transform));
    if (!bone.has_value() || !plausibleTransform(*bone)) return std::nullopt;
    const Vector3 scaled{
        bone->translation.x * componentToWorld.scale.x,
        bone->translation.y * componentToWorld.scale.y,
        bone->translation.z * componentToWorld.scale.z
    };
    const Vector3 rotated = rotateVector(componentToWorld.rotation, scaled);
    return Vector3{rotated.x + componentToWorld.translation.x, rotated.y + componentToWorld.translation.y, rotated.z + componentToWorld.translation.z};
}

bool readBoneTransforms(ProcessInstance<>& memory, std::uint64_t boneArray, std::array<Transform, 128>& transforms) {
    auto* bytes = reinterpret_cast<std::uint8_t*>(transforms.data());
    return memory.read_raw_into(boneArray, CSliceMut<std::uint8_t>(reinterpret_cast<char*>(bytes), sizeof(transforms))) == 0;
}

bool plausibleMeshBone(const Transform& componentToWorld, const Vector3& position) {
    const double distance = std::hypot(std::hypot(position.x - componentToWorld.translation.x, position.y - componentToWorld.translation.y), position.z - componentToWorld.translation.z);
    return std::isfinite(distance) && distance <= 500.0;
}

std::optional<ResolvedBone> scanAimBone(ProcessInstance<>& memory, std::uint64_t boneArray, const Transform& componentToWorld, bool head) {
    std::array<Transform, 128> transforms{};
    if (!readBoneTransforms(memory, boneArray, transforms)) return std::nullopt;

    std::optional<ResolvedBone> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < transforms.size(); ++index) {
        const Transform& bone = transforms[index];
        if (!plausibleTransform(bone)) continue;
        const Vector3 scaled{
            bone.translation.x * componentToWorld.scale.x,
            bone.translation.y * componentToWorld.scale.y,
            bone.translation.z * componentToWorld.scale.z
        };
        const Vector3 rotated = rotateVector(componentToWorld.rotation, scaled);
        const Vector3 position{rotated.x + componentToWorld.translation.x, rotated.y + componentToWorld.translation.y, rotated.z + componentToWorld.translation.z};
        const double horizontal = std::hypot(position.x - componentToWorld.translation.x, position.y - componentToWorld.translation.y);
        const double vertical = position.z - componentToWorld.translation.z;
        if (!std::isfinite(horizontal) || !std::isfinite(vertical)) continue;

        if (head) {
            if (horizontal > 45.0 || vertical < 35.0 || vertical > 160.0) continue;
            const double score = std::abs(vertical - 75.0) + horizontal * 0.10;
            if (score >= bestScore) continue;
            bestScore = score;
        } else {
            if (horizontal > 60.0 || vertical < -30.0 || vertical > 90.0) continue;
            const double score = std::abs(vertical - 25.0) + horizontal * 0.10;
            if (score >= bestScore) continue;
            bestScore = score;
        }
        best = ResolvedBone{position, static_cast<int>(index)};
    }
    return best;
}

std::optional<ResolvedBone> actorBoneWorldPosition(ProcessInstance<>& memory, const Unreal::ActorPosition& actor, int preferredIndex) {
    static std::unordered_map<std::uint64_t, CachedAimBones> cachedIndices;
    if (!plausiblePointer(actor.mesh)) return std::nullopt;
    Transform componentToWorld{};
    std::array<std::uint64_t, 2> boneArrays{};
    std::vector<ReadData> reads;
    reads.reserve(3);
    addRead(reads, actor.mesh + Offsets::SkeletalMeshComponentToWorld, componentToWorld);
    addRead(reads, actor.mesh + Offsets::SkeletalMeshBoneArray, boneArrays[0]);
    addRead(reads, actor.mesh + Offsets::SkeletalMeshBoneArrayCache, boneArrays[1]);
    if (!runReads(memory, reads) || !plausibleTransform(componentToWorld)) return std::nullopt;
    const bool head = preferredIndex == 110;
    CachedAimBones& cached = cachedIndices[actor.mesh];
    int& cachedIndex = head ? cached.head : cached.torso;

    for (const std::uint64_t boneArray : boneArrays) {
        if (!plausiblePointer(boneArray)) continue;
        const auto preferredPosition = boneWorldPosition(memory, boneArray, preferredIndex, componentToWorld);
        if (preferredPosition.has_value() && plausibleMeshBone(componentToWorld, *preferredPosition)) {
            cachedIndex = preferredIndex;
            return ResolvedBone{*preferredPosition, preferredIndex};
        }
        if (cachedIndex >= 0) {
            const auto cachedPosition = boneWorldPosition(memory, boneArray, cachedIndex, componentToWorld);
            if (cachedPosition.has_value() && plausibleMeshBone(componentToWorld, *cachedPosition)) return ResolvedBone{*cachedPosition, cachedIndex};
            cachedIndex = -1;
        }
        const auto scanned = scanAimBone(memory, boneArray, componentToWorld, head);
        if (scanned.has_value()) {
            cachedIndex = scanned->index;
            return scanned;
        }
    }
    return std::nullopt;
}

std::optional<std::uint64_t> resolveWorld(ProcessInstance<>& memory, std::uint64_t imageBase) {
    static std::uint64_t cachedImageBase = 0;
    static std::uint64_t cachedViewport = 0;
    if (cachedImageBase != imageBase) {
        cachedImageBase = imageBase;
        cachedViewport = 0;
    }
    if (plausiblePointer(cachedViewport)) {
        const auto world = read<std::uint64_t>(memory, cachedViewport + Offsets::ViewportWorld);
        if (world.has_value() && plausiblePointer(*world)) return world;
        cachedViewport = 0;
    }
    const auto engine = read<std::uint64_t>(memory, imageBase + Offsets::GEngine);
    if (!engine.has_value() || !plausiblePointer(*engine)) return std::nullopt;
    const auto viewport = read<std::uint64_t>(memory, *engine + Offsets::GameViewport);
    if (!viewport.has_value() || !plausiblePointer(*viewport)) return std::nullopt;
    cachedViewport = *viewport;
    const auto world = read<std::uint64_t>(memory, cachedViewport + Offsets::ViewportWorld);
    if (world.has_value() && plausiblePointer(*world)) return world;
    return std::nullopt;
}

std::optional<std::vector<std::uint64_t>> readPointerArray(ProcessInstance<>& memory, const RemoteArray& array) {
    if (array.count < 0 || array.capacity < array.count || static_cast<std::size_t>(array.count) > Config::MaximumActorCount) return std::nullopt;
    if (array.count == 0) return std::vector<std::uint64_t>{};
    if (!plausiblePointer(array.data)) return std::nullopt;

    std::vector<std::uint64_t> pointers(static_cast<std::size_t>(array.count));
    auto* bytes = reinterpret_cast<std::uint8_t*>(pointers.data());
    const std::size_t byteCount = pointers.size() * sizeof(std::uint64_t);
    if (memory.read_raw_into(array.data, CSliceMut<std::uint8_t>(reinterpret_cast<char*>(bytes), byteCount)) != 0) return std::nullopt;
    return pointers;
}

std::vector<LlamaAimTarget> llamaAimTargets(ProcessInstance<>& memory, const Unreal::ActorSnapshot& snapshot) {
    static std::uint64_t cachedWorld = 0;
    static std::vector<std::uint64_t> cachedActors;
    static std::chrono::steady_clock::time_point nextRefresh{};
    const auto now = std::chrono::steady_clock::now();
    if (cachedWorld != snapshot.world || cachedActors.empty() || now >= nextRefresh) {
        cachedWorld = snapshot.world;
        cachedActors.clear();
        nextRefresh = now + std::chrono::seconds(3);

        const auto mapInfo = read<std::uint64_t>(memory, snapshot.gameState + Offsets::GameStateMapInfo);
        if (mapInfo.has_value() && plausiblePointer(*mapInfo)) {
            const auto llamaClass = read<std::uint64_t>(memory, *mapInfo + Offsets::MapInfoLlamaClass);
            const auto levelsArray = read<RemoteArray>(memory, snapshot.world + Offsets::WorldLevels);
            if (llamaClass.has_value() && plausiblePointer(*llamaClass) && levelsArray.has_value()) {
                const auto levels = readPointerArray(memory, *levelsArray);
                if (levels.has_value()) {
                    for (const std::uint64_t level : *levels) {
                        if (!plausiblePointer(level)) continue;
                        const auto actorArray = read<RemoteArray>(memory, level + Offsets::LevelActors);
                        if (!actorArray.has_value()) continue;
                        const auto actors = readPointerArray(memory, *actorArray);
                        if (!actors.has_value()) continue;
                        for (const std::uint64_t actor : *actors) {
                            if (!plausiblePointer(actor)) continue;
                            const auto actorClass = read<std::uint64_t>(memory, actor + Offsets::ObjectClass);
                            if (actorClass.has_value() && *actorClass == *llamaClass) cachedActors.push_back(actor);
                        }
                    }
                }
            }
        }
    }

    std::vector<LlamaAimTarget> targets;
    targets.reserve(cachedActors.size());
    for (const std::uint64_t actor : cachedActors) {
        const auto root = read<std::uint64_t>(memory, actor + Offsets::ActorRootComponent);
        if (!root.has_value() || !plausiblePointer(*root)) continue;
        const auto position = read<Vector3>(memory, *root + Offsets::SceneComponentRelativeLocation);
        if (!position.has_value() || !std::isfinite(position->x) || !std::isfinite(position->y) || !std::isfinite(position->z)) continue;
        targets.push_back(LlamaAimTarget{actor, Vector3{position->x, position->y, position->z + Config::LlamaAimHeight}});
    }
    return targets;
}

Unreal::GroundItemSnapshot groundItemTargets(ProcessInstance<>& memory, const Unreal::ActorSnapshot& snapshot, std::string_view itemName, std::optional<Unreal::ItemRarity> rarity) {
    struct DefinitionName {
        std::string display;
        std::string normalized;
    };
    struct PickupMetadata {
        bool pickup{};
        std::uint64_t definition{};
        std::chrono::steady_clock::time_point checked{};
    };
    struct PickupProbe {
        std::uint64_t actor{};
        std::uint8_t flags{};
        std::uint64_t definition{};
        std::uint64_t parentPickup{};
        std::int32_t rarityLevel{-1};
    };
    static std::uint64_t cachedWorld = 0;
    static std::string cachedQuery;
    static std::optional<Unreal::ItemRarity> cachedRarity;
    static std::chrono::steady_clock::time_point nextRefresh{};
    static bool scanIncomplete = false;
    static Unreal::GroundItemSnapshot cachedSnapshot{};
    static std::unordered_map<std::uint64_t, DefinitionName> definitionNames;
    static std::unordered_map<std::uint64_t, PickupMetadata> actorMetadata;
    static std::unordered_map<std::uint64_t, Unreal::ItemRarity> pickupRarities;

    const std::string query = lowercaseAscii(itemName);
    const auto now = std::chrono::steady_clock::now();
    if (cachedWorld == snapshot.world && cachedQuery == query && cachedRarity == rarity && now < nextRefresh && !scanIncomplete) return cachedSnapshot;
    if (cachedWorld != snapshot.world) {
        definitionNames.clear();
        actorMetadata.clear();
        pickupRarities.clear();
    }
    cachedWorld = snapshot.world;
    cachedQuery = query;
    cachedRarity = rarity;
    nextRefresh = now + Config::GroundItemRefreshInterval;
    cachedSnapshot = Unreal::GroundItemSnapshot{};
    if (query.empty()) return cachedSnapshot;

    const auto levelsArray = read<RemoteArray>(memory, snapshot.world + Offsets::WorldLevels);
    if (!levelsArray.has_value()) return cachedSnapshot;
    const auto levels = readPointerArray(memory, *levelsArray);
    if (!levels.has_value()) return cachedSnapshot;

    std::unordered_set<std::uint64_t> visitedActors;
    std::vector<std::uint64_t> worldActors;
    for (const std::uint64_t level : *levels) {
        if (!plausiblePointer(level)) continue;
        const auto actorArray = read<RemoteArray>(memory, level + Offsets::LevelActors);
        if (!actorArray.has_value()) continue;
        const auto actorPointers = readPointerArray(memory, *actorArray);
        if (!actorPointers.has_value()) continue;
        cachedSnapshot.scannedActors += actorPointers->size();

        for (const std::uint64_t actor : *actorPointers) {
            if (!plausiblePointer(actor) || !visitedActors.insert(actor).second) continue;
            worldActors.push_back(actor);
        }
    }

    std::vector<PickupProbe> probes;
    probes.reserve(std::min<std::size_t>(worldActors.size(), Config::GroundItemProbeBudget));
    scanIncomplete = false;
    std::size_t unresolvedActors = 0;
    for (const std::uint64_t actor : worldActors) {
        const auto known = actorMetadata.find(actor);
        if (known != actorMetadata.end() && (known->second.pickup || now - known->second.checked < Config::NegativePickupCacheLifetime)) continue;
        ++unresolvedActors;
        if (probes.size() >= Config::GroundItemProbeBudget) {
            scanIncomplete = true;
            continue;
        }
        probes.push_back(PickupProbe{actor, 0, 0, 0, -1});
    }
    std::vector<ReadData> probeReads;
    probeReads.reserve(probes.size() * (rarity.has_value() ? 4 : 2));
    for (PickupProbe& probe : probes) {
        addRead(probeReads, probe.actor + Offsets::PickupFlags, probe.flags);
        addRead(probeReads, probe.actor + Offsets::PickupPrimaryItemEntry + Offsets::ItemEntryItemDefinition, probe.definition);
        if (rarity.has_value()) {
            addRead(probeReads, probe.actor + Offsets::PickupEffectParentPickupActor, probe.parentPickup);
            addRead(probeReads, probe.actor + Offsets::PickupsParentRarityLevel, probe.rarityLevel);
        }
    }
    runReads(memory, probeReads);
    for (const PickupProbe& probe : probes) {
        if (rarity.has_value() && plausiblePointer(probe.parentPickup) && probe.rarityLevel >= 0 && probe.rarityLevel <= static_cast<std::int32_t>(Unreal::ItemRarity::Unattainable)) pickupRarities[probe.parentPickup] = static_cast<Unreal::ItemRarity>(probe.rarityLevel);
        PickupMetadata metadata{false, 0, now};
        if ((probe.flags & Offsets::PickupPickedUpMask) == 0 && plausiblePointer(probe.definition)) {
            auto knownName = definitionNames.find(probe.definition);
            if (knownName == definitionNames.end()) {
                const auto resolvedName = readItemName(memory, probe.definition);
                const std::string display = resolvedName.value_or(std::string{});
                knownName = definitionNames.emplace(probe.definition, DefinitionName{display, lowercaseAscii(display)}).first;
            }
            if (!knownName->second.display.empty()) metadata = PickupMetadata{true, probe.definition, now};
        }
        actorMetadata[probe.actor] = metadata;
    }
    cachedSnapshot.pendingActors = unresolvedActors > probes.size() ? unresolvedActors - probes.size() : 0;

    struct MatchedPickup {
        std::uint64_t actor{};
        std::uint64_t definition{};
        const DefinitionName* name{};
        std::optional<Unreal::ItemRarity> rarity;
        std::uint64_t root{};
        std::uint8_t flags{};
        Vector3 position{};
    };
    std::vector<MatchedPickup> matches;
    for (const std::uint64_t actor : worldActors) {
        const auto metadata = actorMetadata.find(actor);
        if (metadata == actorMetadata.end() || !metadata->second.pickup) continue;
        const auto name = definitionNames.find(metadata->second.definition);
        if (name == definitionNames.end() || name->second.normalized.find(query) == std::string::npos) continue;
        const auto knownRarity = pickupRarities.find(actor);
        if (rarity.has_value() && (knownRarity == pickupRarities.end() || knownRarity->second < *rarity)) continue;
        const std::optional<Unreal::ItemRarity> matchedRarity = knownRarity == pickupRarities.end() ? std::nullopt : std::optional<Unreal::ItemRarity>{knownRarity->second};
        matches.push_back(MatchedPickup{actor, metadata->second.definition, &name->second, matchedRarity, 0, 0, {}});
    }

    std::vector<ReadData> rootReads;
    rootReads.reserve(matches.size() * 2);
    for (MatchedPickup& match : matches) {
        addRead(rootReads, match.actor + Offsets::ActorRootComponent, match.root);
        addRead(rootReads, match.actor + Offsets::PickupFlags, match.flags);
    }
    runReads(memory, rootReads);
    std::vector<ReadData> locationReads;
    locationReads.reserve(matches.size());
    for (MatchedPickup& match : matches) {
        if ((match.flags & Offsets::PickupPickedUpMask) == 0 && plausiblePointer(match.root)) addRead(locationReads, match.root + Offsets::SceneComponentRelativeLocation, match.position);
    }
    runReads(memory, locationReads);
    for (const MatchedPickup& match : matches) {
        if ((match.flags & Offsets::PickupPickedUpMask) != 0 || !plausiblePointer(match.root) || !std::isfinite(match.position.x) || !std::isfinite(match.position.y) || !std::isfinite(match.position.z)) continue;
        cachedSnapshot.positions.push_back(Unreal::GroundItemPosition{match.actor, match.definition, match.name->display, match.rarity, match.position.x, match.position.y, match.position.z});
    }

    for (auto iterator = actorMetadata.begin(); iterator != actorMetadata.end();) {
        if (visitedActors.find(iterator->first) == visitedActors.end()) iterator = actorMetadata.erase(iterator);
        else ++iterator;
    }
    for (auto iterator = pickupRarities.begin(); iterator != pickupRarities.end();) {
        if (visitedActors.find(iterator->first) == visitedActors.end()) iterator = pickupRarities.erase(iterator);
        else ++iterator;
    }
    if (scanIncomplete) nextRefresh = now;
    return cachedSnapshot;
}

}

namespace Unreal {

std::optional<ActorSnapshot> actorPositions(ProcessInstance<>& memory, std::uint64_t imageBase, bool ignoreIsABot) {
    const auto world = resolveWorld(memory, imageBase);
    if (!world.has_value()) return std::nullopt;

    ActorSnapshot snapshot{};
    snapshot.world = *world;
    std::uint64_t gameInstance{};
    std::vector<ReadData> worldReads;
    worldReads.reserve(5);
    addRead(worldReads, *world + Offsets::WorldGameState, snapshot.gameState);
    addRead(worldReads, *world + Offsets::WorldOwningGameInstance, gameInstance);
    addRead(worldReads, *world + Offsets::WorldCameraLocationPointer, snapshot.cameraLocationPointer);
    addRead(worldReads, *world + Offsets::WorldCameraRotationPointer, snapshot.cameraRotationPointer);
    addRead(worldReads, *world + Offsets::WorldSeconds, snapshot.worldSeconds);
    if (!runReads(memory, worldReads) || !plausiblePointer(snapshot.gameState)) return std::nullopt;

    const auto playerArray = read<RemoteArray>(memory, snapshot.gameState + Offsets::GameStatePlayerArray);
    if (!playerArray.has_value()) return std::nullopt;
    const auto playerStates = readPointerArray(memory, *playerArray);
    if (!playerStates.has_value()) return std::nullopt;

    snapshot.rawCount = static_cast<std::size_t>(playerArray->count);
    snapshot.positions.reserve(playerStates->size());

    struct PlayerFields {
        std::uint64_t playerState{};
        std::uint64_t pawn{};
        std::uint8_t botFlags{};
        std::uint8_t team{0xFF};
    };
    std::vector<PlayerFields> playerFields(playerStates->size());
    std::vector<ReadData> playerReads;
    playerReads.reserve(playerStates->size() * 3);
    for (std::size_t index = 0; index < playerStates->size(); ++index) {
        PlayerFields& fields = playerFields[index];
        fields.playerState = (*playerStates)[index];
        if (!plausiblePointer(fields.playerState)) continue;
        addRead(playerReads, fields.playerState + Offsets::PlayerStatePawnPrivate, fields.pawn);
        addRead(playerReads, fields.playerState + Offsets::PlayerStateTeamIndex, fields.team);
        if (!ignoreIsABot) addRead(playerReads, fields.playerState + Offsets::PlayerStateBotFlags, fields.botFlags);
    }
    runReads(memory, playerReads);

    struct PawnFields {
        std::uint64_t playerState{};
        std::uint64_t pawn{};
        std::uint64_t root{};
        std::uint64_t mesh{};
        std::uint8_t team{0xFF};
    };
    std::vector<PawnFields> pawns;
    pawns.reserve(playerFields.size());
    for (const PlayerFields& fields : playerFields) {
        if (!plausiblePointer(fields.playerState)) continue;
        if (!ignoreIsABot && (fields.botFlags & Offsets::PlayerStateBotMask) != 0) {
            ++snapshot.filteredBotCount;
            continue;
        }
        ++snapshot.reportedCount;
        if (!plausiblePointer(fields.pawn)) {
            ++snapshot.missingPawnCount;
            continue;
        }
        pawns.push_back(PawnFields{fields.playerState, fields.pawn, 0, 0, fields.team});
    }

    std::vector<ReadData> pawnReads;
    pawnReads.reserve(pawns.size() * 2);
    for (PawnFields& fields : pawns) {
        addRead(pawnReads, fields.pawn + Offsets::ActorRootComponent, fields.root);
        addRead(pawnReads, fields.pawn + Offsets::PlayerPawnMesh, fields.mesh);
    }
    runReads(memory, pawnReads);

    struct ComponentFields {
        Vector3 location{};
        float lastRenderTime{std::numeric_limits<float>::lowest()};
    };
    std::vector<ComponentFields> components(pawns.size());
    std::vector<ReadData> componentReads;
    componentReads.reserve(pawns.size() * 2);
    for (std::size_t index = 0; index < pawns.size(); ++index) {
        if (plausiblePointer(pawns[index].root)) addRead(componentReads, pawns[index].root + Offsets::SceneComponentRelativeLocation, components[index].location);
        if (plausiblePointer(pawns[index].mesh)) addRead(componentReads, pawns[index].mesh + Offsets::PrimitiveComponentLastRenderTime, components[index].lastRenderTime);
    }
    runReads(memory, componentReads);

    for (std::size_t index = 0; index < pawns.size(); ++index) {
        const PawnFields& fields = pawns[index];
        const ComponentFields& component = components[index];
        if (!plausiblePointer(fields.root)) {
            ++snapshot.missingRootCount;
            continue;
        }
        if (!std::isfinite(component.location.x) || !std::isfinite(component.location.y) || !std::isfinite(component.location.z)) {
            ++snapshot.invalidLocationCount;
            continue;
        }
        snapshot.positions.push_back(ActorPosition{fields.pawn, fields.playerState, fields.mesh, fields.team, component.lastRenderTime, component.location.x, component.location.y, component.location.z});
    }

    if (plausiblePointer(gameInstance)) {
        const auto localPlayers = read<RemoteArray>(memory, gameInstance + Offsets::GameInstanceLocalPlayers);
        if (localPlayers.has_value() && localPlayers->count > 0 && plausiblePointer(localPlayers->data)) {
            const auto localPlayer = read<std::uint64_t>(memory, localPlayers->data);
            if (localPlayer.has_value() && plausiblePointer(*localPlayer)) {
                const auto controller = read<std::uint64_t>(memory, *localPlayer + Offsets::LocalPlayerPlayerController);
                if (controller.has_value() && plausiblePointer(*controller)) {
                    snapshot.localController = *controller;
                    const auto acknowledgedPawn = read<std::uint64_t>(memory, *controller + Offsets::PlayerControllerAcknowledgedPawn);
                    const std::uint64_t localPawn = acknowledgedPawn.value_or(0);
                    if (plausiblePointer(localPawn)) {
                        snapshot.localPawn = localPawn;
                        const auto local = std::find_if(snapshot.positions.begin(), snapshot.positions.end(), [&snapshot](const ActorPosition& position) { return position.actor == snapshot.localPawn; });
                        if (local != snapshot.positions.end()) {
                            snapshot.localTeam = local->team;
                            snapshot.hasLocalPosition = true;
                            snapshot.localX = local->x;
                            snapshot.localY = local->y;
                            snapshot.localZ = local->z;
                        } else {
                            const auto localPlayerState = read<std::uint64_t>(memory, snapshot.localPawn + Offsets::PawnPlayerState);
                            if (localPlayerState.has_value() && plausiblePointer(*localPlayerState)) {
                                const auto team = read<std::uint8_t>(memory, *localPlayerState + Offsets::PlayerStateTeamIndex);
                                if (team.has_value()) snapshot.localTeam = *team;
                            }
                            const auto root = read<std::uint64_t>(memory, snapshot.localPawn + Offsets::ActorRootComponent);
                            if (root.has_value() && plausiblePointer(*root)) {
                                const auto location = read<Vector3>(memory, *root + Offsets::SceneComponentRelativeLocation);
                                if (location.has_value() && std::isfinite(location->x) && std::isfinite(location->y) && std::isfinite(location->z)) {
                                    snapshot.hasLocalPosition = true;
                                    snapshot.localX = location->x;
                                    snapshot.localY = location->y;
                                    snapshot.localZ = location->z;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return snapshot;
}

GroundItemSnapshot findGroundItems(ProcessInstance<>& memory, const ActorSnapshot& snapshot, std::string_view itemName, std::optional<ItemRarity> rarity) {
    return groundItemTargets(memory, snapshot, itemName, rarity);
}

std::optional<std::uint64_t> currentVehicle(ProcessInstance<>& memory, const ActorSnapshot& snapshot) {
    if (!plausiblePointer(snapshot.localPawn)) return std::nullopt;
    const auto vehicle = read<std::uint64_t>(memory, snapshot.localPawn + Offsets::PlayerPawnCurrentVehicle);
    if (!vehicle.has_value()) return std::nullopt;
    if (*vehicle != 0 && !plausiblePointer(*vehicle)) return std::nullopt;
    return *vehicle;
}

std::string_view weaponCategoryName(WeaponCategory category) {
    switch (category) {
        case WeaponCategory::Unknown: return "unknown";
        case WeaponCategory::Pistol: return "pistol";
        case WeaponCategory::Shotgun: return "shotgun";
        case WeaponCategory::Rifle: return "rifle";
        case WeaponCategory::Smg: return "smg";
        case WeaponCategory::Sniper: return "sniper";
        case WeaponCategory::Launcher: return "launcher";
        case WeaponCategory::Bow: return "bow";
        case WeaponCategory::Minigun: return "minigun";
        case WeaponCategory::Melee: return "melee";
        case WeaponCategory::Utility: return "utility";
        case WeaponCategory::Unarmed: return "unarmed";
        case WeaponCategory::Count: break;
    }
    return "unknown";
}

std::optional<WeaponSnapshot> currentWeapon(ProcessInstance<>& memory, const ActorSnapshot& snapshot) {
    if (!plausiblePointer(snapshot.localPawn)) return std::nullopt;
    const auto weapon = read<std::uint64_t>(memory, snapshot.localPawn + Offsets::PawnCurrentWeapon);
    if (!weapon.has_value()) return std::nullopt;
    const std::uint64_t weaponActor = *weapon;
    if (weaponActor == 0) return WeaponSnapshot{0, 0, {}, WeaponCategory::Unarmed};
    if (!plausiblePointer(weaponActor)) return std::nullopt;
    const auto definitionRead = read<std::uint64_t>(memory, weaponActor + Offsets::WeaponData);
    const std::uint64_t definition = definitionRead.value_or(0);
    std::string name;
    if (plausiblePointer(definition)) {
        const auto resolvedName = readItemName(memory, definition);
        if (resolvedName.has_value()) name = *resolvedName;
    }
    const auto nameCategory = weaponCategoryFromName(name);
    return WeaponSnapshot{weaponActor, plausiblePointer(definition) ? definition : 0, std::move(name), nameCategory.value_or(WeaponCategory::Unknown)};
}

std::optional<AimResult> aimAtNearestPawn(ProcessInstance<>& memory, const ActorSnapshot& snapshot, double smoothing, bool activationDown, bool ignoreTeams) {
    static bool wasAiming = false;
    static bool activationWasDown = false;
    static std::uint64_t previousController = 0;
    static std::uint64_t lockedTarget = 0;
    const std::uint64_t currentController = snapshot.localController;

    const auto clearAimState = [&memory, &currentController]() {
        bool cleared = true;
        if (plausiblePointer(currentController)) cleared = write(memory, currentController + Offsets::PlayerControllerRotationInput, Rotator{});
        if (wasAiming && plausiblePointer(previousController) && previousController != currentController) cleared = write(memory, previousController + Offsets::PlayerControllerRotationInput, Rotator{}) && cleared;
        wasAiming = false;
        previousController = 0;
        return cleared;
    };

    if (!plausiblePointer(currentController) || !plausiblePointer(snapshot.localPawn)) return std::nullopt;

    if (!activationDown) {
        activationWasDown = false;
        lockedTarget = 0;
        if (!wasAiming) return AimResult{false, true, snapshot.localPawn, 0, snapshot.localTeam, 0xFF, -1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        const bool cleared = clearAimState();
        return AimResult{false, cleared, snapshot.localPawn, 0, snapshot.localTeam, 0xFF, -1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    }

    const auto camera = readCamera(memory, snapshot);
    if (!camera.has_value() || !std::isfinite(snapshot.worldSeconds)) return std::nullopt;
    const Vector3& cameraLocation = camera->location;
    const Rotator& cameraRotation = camera->rotation;
    const std::uint8_t localTeam = snapshot.localTeam;

    const ActorPosition* bestTarget = nullptr;
    Rotator bestDesired{};
    int bestTargetBone = -1;
    double bestTargetDistance = 0.0;
    double bestDistanceSquared = std::numeric_limits<double>::infinity();
    for (const ActorPosition& candidate : snapshot.positions) {
        if (candidate.actor == snapshot.localPawn) continue;
        if (!ignoreTeams && localTeam != 0xFF && candidate.team != 0xFF && candidate.team == localTeam) continue;
        if (!isVisible(candidate, snapshot.worldSeconds)) continue;
        if (activationDown && activationWasDown && lockedTarget != 0 && candidate.actor != lockedTarget) continue;

        const double targetDistance = std::hypot(std::hypot(candidate.x - cameraLocation.x, candidate.y - cameraLocation.y), candidate.z - cameraLocation.z);
        if (!std::isfinite(targetDistance)) continue;
        const int targetBone = targetDistance <= Config::HeadAimMaximumDistance ? 110 : 3;
        const auto targetPosition = actorBoneWorldPosition(memory, candidate, targetBone);
        if (!targetPosition.has_value()) continue;
        const auto desired = lookAt(cameraLocation, targetPosition->position);
        if (!desired.has_value()) continue;
        const double pitchDelta = normalizedAngle(desired->pitch - cameraRotation.pitch);
        const double yawDelta = normalizedAngle(desired->yaw - cameraRotation.yaw);
        const double distanceSquared = pitchDelta * pitchDelta + yawDelta * yawDelta;
        if (activationDown && lockedTarget == 0 && distanceSquared > Config::MaximumAimAngle * Config::MaximumAimAngle) continue;
        if (distanceSquared >= bestDistanceSquared) continue;
        bestDistanceSquared = distanceSquared;
        bestTarget = &candidate;
        bestDesired = *desired;
        bestTargetBone = targetPosition->index;
        bestTargetDistance = targetDistance;
    }

    if (bestTarget == nullptr) {
        const bool cleared = clearAimState();
        if (activationDown) activationWasDown = true;
        return AimResult{activationDown, cleared, snapshot.localPawn, 0, localTeam, 0xFF, -1, 0.0, 0.0, cameraRotation.pitch, cameraRotation.yaw, 0.0, 0.0, 0.0, 0.0};
    }

    if (!activationWasDown || lockedTarget == 0) lockedTarget = bestTarget->actor;
    activationWasDown = true;

    const double pitchDelta = normalizedAngle(bestDesired.pitch - cameraRotation.pitch);
    const double yawDelta = normalizedAngle(bestDesired.yaw - cameraRotation.yaw);
    const Rotator rotationInput{pitchDelta / smoothing, yawDelta / smoothing, 0.0};
    if (wasAiming && previousController != currentController && plausiblePointer(previousController)) {
        write(memory, previousController + Offsets::PlayerControllerRotationInput, Rotator{});
    }
    const bool wrote = write(memory, currentController + Offsets::PlayerControllerRotationInput, rotationInput);
    previousController = currentController;
    wasAiming = true;
    return AimResult{true, wrote, snapshot.localPawn, bestTarget->actor, localTeam, bestTarget->team, bestTargetBone, bestTargetDistance / 100.0, std::sqrt(bestDistanceSquared), cameraRotation.pitch, cameraRotation.yaw, bestDesired.pitch, bestDesired.yaw, rotationInput.pitch, rotationInput.yaw};
}

std::optional<AimResult> aimAtNearestLlama(ProcessInstance<>& memory, const ActorSnapshot& snapshot, bool activationDown) {
    static bool wasAiming = false;
    static bool activationWasDown = false;
    static std::uint64_t previousController = 0;
    static std::uint64_t lockedTarget = 0;
    const std::uint64_t currentController = snapshot.localController;

    const auto clearAimState = [&memory, &currentController]() {
        bool cleared = true;
        if (plausiblePointer(currentController)) cleared = write(memory, currentController + Offsets::PlayerControllerRotationInput, Rotator{});
        if (wasAiming && plausiblePointer(previousController) && previousController != currentController) cleared = write(memory, previousController + Offsets::PlayerControllerRotationInput, Rotator{}) && cleared;
        wasAiming = false;
        previousController = 0;
        return cleared;
    };

    if (!plausiblePointer(currentController) || !plausiblePointer(snapshot.localPawn)) return std::nullopt;

    if (!activationDown) {
        activationWasDown = false;
        lockedTarget = 0;
        if (!wasAiming) return AimResult{false, true, snapshot.localPawn, 0, snapshot.localTeam, 0xFF, -2, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        const bool cleared = clearAimState();
        return AimResult{false, cleared, snapshot.localPawn, 0, snapshot.localTeam, 0xFF, -2, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    }

    const auto camera = readCamera(memory, snapshot);
    if (!camera.has_value()) return std::nullopt;
    const Vector3& cameraLocation = camera->location;
    const Rotator& cameraRotation = camera->rotation;
    const std::uint8_t localTeam = snapshot.localTeam;

    const Vector3 selectionOrigin = cameraLocation;
    const auto targets = llamaAimTargets(memory, snapshot);
    const LlamaAimTarget* bestTarget = nullptr;
    Rotator bestDesired{};
    double bestTargetDistance = 0.0;
    double bestAngularDistanceSquared = 0.0;
    double bestSelectionDistanceSquared = std::numeric_limits<double>::infinity();
    for (const LlamaAimTarget& candidate : targets) {
        if (activationWasDown && lockedTarget != 0 && candidate.actor != lockedTarget) continue;
        const double selectionDx = candidate.position.x - selectionOrigin.x;
        const double selectionDy = candidate.position.y - selectionOrigin.y;
        const double selectionDz = candidate.position.z - selectionOrigin.z;
        const double selectionDistanceSquared = selectionDx * selectionDx + selectionDy * selectionDy + selectionDz * selectionDz;
        if (!std::isfinite(selectionDistanceSquared) || selectionDistanceSquared >= bestSelectionDistanceSquared) continue;
        const auto desired = lookAt(cameraLocation, candidate.position);
        if (!desired.has_value()) continue;
        const double pitchDelta = normalizedAngle(desired->pitch - cameraRotation.pitch);
        const double yawDelta = normalizedAngle(desired->yaw - cameraRotation.yaw);
        bestTarget = &candidate;
        bestDesired = *desired;
        bestTargetDistance = std::hypot(std::hypot(candidate.position.x - cameraLocation.x, candidate.position.y - cameraLocation.y), candidate.position.z - cameraLocation.z);
        bestAngularDistanceSquared = pitchDelta * pitchDelta + yawDelta * yawDelta;
        bestSelectionDistanceSquared = selectionDistanceSquared;
    }

    if (bestTarget == nullptr) {
        lockedTarget = 0;
        activationWasDown = true;
        const bool cleared = clearAimState();
        return AimResult{true, cleared, snapshot.localPawn, 0, localTeam, 0xFF, -2, 0.0, 0.0, cameraRotation.pitch, cameraRotation.yaw, 0.0, 0.0, 0.0, 0.0};
    }

    if (!activationWasDown || lockedTarget == 0) lockedTarget = bestTarget->actor;
    activationWasDown = true;
    const double pitchDelta = normalizedAngle(bestDesired.pitch - cameraRotation.pitch);
    const double yawDelta = normalizedAngle(bestDesired.yaw - cameraRotation.yaw);
    const Rotator rotationInput{pitchDelta / Config::LlamaAimSmoothing, yawDelta / Config::LlamaAimSmoothing, 0.0};
    if (wasAiming && previousController != currentController && plausiblePointer(previousController)) write(memory, previousController + Offsets::PlayerControllerRotationInput, Rotator{});
    const bool wrote = write(memory, currentController + Offsets::PlayerControllerRotationInput, rotationInput);
    previousController = currentController;
    wasAiming = true;
    return AimResult{true, wrote, snapshot.localPawn, bestTarget->actor, localTeam, 0xFF, -2, bestTargetDistance / 100.0, std::sqrt(bestAngularDistanceSquared), cameraRotation.pitch, cameraRotation.yaw, bestDesired.pitch, bestDesired.yaw, rotationInput.pitch, rotationInput.yaw};
}

std::optional<AimResult> aimAtNearestGroundItem(ProcessInstance<>& memory, const ActorSnapshot& snapshot, const GroundItemSnapshot& items, bool activationDown) {
    static bool wasAiming = false;
    static bool activationWasDown = false;
    static std::uint64_t previousController = 0;
    static std::uint64_t lockedTarget = 0;
    const std::uint64_t currentController = snapshot.localController;

    const auto clearAimState = [&memory, &currentController]() {
        bool cleared = true;
        if (plausiblePointer(currentController)) cleared = write(memory, currentController + Offsets::PlayerControllerRotationInput, Rotator{});
        if (wasAiming && plausiblePointer(previousController) && previousController != currentController) cleared = write(memory, previousController + Offsets::PlayerControllerRotationInput, Rotator{}) && cleared;
        wasAiming = false;
        previousController = 0;
        return cleared;
    };

    if (!plausiblePointer(currentController) || !plausiblePointer(snapshot.localPawn)) return std::nullopt;

    if (!activationDown) {
        activationWasDown = false;
        lockedTarget = 0;
        if (!wasAiming) return AimResult{false, true, snapshot.localPawn, 0, snapshot.localTeam, 0xFF, -3, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        const bool cleared = clearAimState();
        return AimResult{false, cleared, snapshot.localPawn, 0, snapshot.localTeam, 0xFF, -3, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    }

    const auto camera = readCamera(memory, snapshot);
    if (!camera.has_value()) return std::nullopt;
    const Vector3& cameraLocation = camera->location;
    const Rotator& cameraRotation = camera->rotation;
    const std::uint8_t localTeam = snapshot.localTeam;

    const GroundItemPosition* bestTarget = nullptr;
    Rotator bestDesired{};
    double bestTargetDistance = 0.0;
    double bestAngularDistanceSquared = 0.0;
    double bestSelectionDistanceSquared = std::numeric_limits<double>::infinity();
    for (const GroundItemPosition& candidate : items.positions) {
        if (activationWasDown && lockedTarget != 0 && candidate.actor != lockedTarget) continue;
        const Vector3 position{candidate.x, candidate.y, candidate.z};
        const double selectionDx = position.x - cameraLocation.x;
        const double selectionDy = position.y - cameraLocation.y;
        const double selectionDz = position.z - cameraLocation.z;
        const double selectionDistanceSquared = selectionDx * selectionDx + selectionDy * selectionDy + selectionDz * selectionDz;
        if (!std::isfinite(selectionDistanceSquared) || selectionDistanceSquared >= bestSelectionDistanceSquared) continue;
        const auto desired = lookAt(cameraLocation, position);
        if (!desired.has_value()) continue;
        const double pitchDelta = normalizedAngle(desired->pitch - cameraRotation.pitch);
        const double yawDelta = normalizedAngle(desired->yaw - cameraRotation.yaw);
        bestTarget = &candidate;
        bestDesired = *desired;
        bestTargetDistance = std::sqrt(selectionDistanceSquared);
        bestAngularDistanceSquared = pitchDelta * pitchDelta + yawDelta * yawDelta;
        bestSelectionDistanceSquared = selectionDistanceSquared;
    }

    if (bestTarget == nullptr) {
        lockedTarget = 0;
        activationWasDown = true;
        const bool cleared = clearAimState();
        return AimResult{true, cleared, snapshot.localPawn, 0, localTeam, 0xFF, -3, 0.0, 0.0, cameraRotation.pitch, cameraRotation.yaw, 0.0, 0.0, 0.0, 0.0};
    }

    if (!activationWasDown || lockedTarget == 0) lockedTarget = bestTarget->actor;
    activationWasDown = true;
    const double pitchDelta = normalizedAngle(bestDesired.pitch - cameraRotation.pitch);
    const double yawDelta = normalizedAngle(bestDesired.yaw - cameraRotation.yaw);
    const Rotator rotationInput{pitchDelta / Config::GroundItemAimSmoothing, yawDelta / Config::GroundItemAimSmoothing, 0.0};
    if (wasAiming && previousController != currentController && plausiblePointer(previousController)) write(memory, previousController + Offsets::PlayerControllerRotationInput, Rotator{});
    const bool wrote = write(memory, currentController + Offsets::PlayerControllerRotationInput, rotationInput);
    previousController = currentController;
    wasAiming = true;
    return AimResult{true, wrote, snapshot.localPawn, bestTarget->actor, localTeam, 0xFF, -3, bestTargetDistance / 100.0, std::sqrt(bestAngularDistanceSquared), cameraRotation.pitch, cameraRotation.yaw, bestDesired.pitch, bestDesired.yaw, rotationInput.pitch, rotationInput.yaw};
}

HighlightResult updatePlayerHighlights(ProcessInstance<>& memory, const ActorSnapshot& snapshot, bool enabled, bool ignoreTeams) {
    HighlightResult result{};
    if (!enabled && cachedHighlights.empty()) return result;
    std::unordered_set<std::uint64_t> eligiblePawns;
    const std::uint8_t localTeam = snapshot.localTeam;

    if (enabled) {
        eligiblePawns.reserve(snapshot.positions.size());
        for (const ActorPosition& candidate : snapshot.positions) {
            if (candidate.actor == snapshot.localPawn) continue;
            if (!ignoreTeams && localTeam != 0xFF && candidate.team != 0xFF && candidate.team == localTeam) continue;
            eligiblePawns.insert(candidate.actor);
        }
    }
    result.eligible = eligiblePawns.size();

    for (auto iterator = cachedHighlights.begin(); iterator != cachedHighlights.end();) {
        const auto eligible = eligiblePawns.find(iterator->first);
        if (eligible != eligiblePawns.end()) {
            const auto component = read<std::uint64_t>(memory, iterator->first + Offsets::PlayerPawnCustomDepthComponent);
            if (component.has_value() && *component == iterator->second.component) {
                ++iterator;
                continue;
            }
        }
        if (restoreCachedHighlight(memory, iterator->first, iterator->second)) ++result.restored;
        else ++result.failed;
        iterator = cachedHighlights.erase(iterator);
    }

    for (const std::uint64_t pawn : eligiblePawns) {
        if (cachedHighlights.find(pawn) != cachedHighlights.end()) continue;
        const auto component = read<std::uint64_t>(memory, pawn + Offsets::PlayerPawnCustomDepthComponent);
        if (!component.has_value() || !plausiblePointer(*component)) {
            ++result.failed;
            continue;
        }
        const auto owner = read<std::uint64_t>(memory, *component + Offsets::ObjectOuter);
        const auto original = read<HighlightingData>(memory, *component + Offsets::CustomDepthDefaultHighlightingData);
        if (!owner.has_value() || *owner != pawn || !original.has_value()) {
            ++result.failed;
            continue;
        }

        const HighlightingData applied = playerHighlight(*original);
        if (!write(memory, *component + Offsets::CustomDepthDefaultHighlightingData, applied)) {
            ++result.failed;
            continue;
        }
        const auto verify = read<HighlightingData>(memory, *component + Offsets::CustomDepthDefaultHighlightingData);
        if (!verify.has_value() || !matchingHighlight(*verify, applied)) {
            write(memory, *component + Offsets::CustomDepthDefaultHighlightingData, *original);
            ++result.failed;
            continue;
        }
        cachedHighlights.emplace(pawn, CachedHighlight{*component, *original, applied});
        ++result.applied;
    }
    result.active = cachedHighlights.size();
    return result;
}

HighlightResult restorePlayerHighlights(ProcessInstance<>& memory) {
    HighlightResult result{};
    for (const auto& [pawn, cached] : cachedHighlights) {
        if (restoreCachedHighlight(memory, pawn, cached)) ++result.restored;
        else ++result.failed;
    }
    cachedHighlights.clear();
    return result;
}

bool clearAimOffsets(ProcessInstance<>& memory, std::uint64_t world) {
    const auto gameInstance = read<std::uint64_t>(memory, world + Offsets::WorldOwningGameInstance);
    if (!gameInstance.has_value() || !plausiblePointer(*gameInstance)) return false;
    const auto localPlayers = read<RemoteArray>(memory, *gameInstance + Offsets::GameInstanceLocalPlayers);
    if (!localPlayers.has_value() || localPlayers->count < 1 || !plausiblePointer(localPlayers->data)) return false;
    const auto localPlayer = read<std::uint64_t>(memory, localPlayers->data);
    if (!localPlayer.has_value() || !plausiblePointer(*localPlayer)) return false;
    const auto controller = read<std::uint64_t>(memory, *localPlayer + Offsets::LocalPlayerPlayerController);
    if (!controller.has_value() || !plausiblePointer(*controller)) return false;
    return write(memory, *controller + Offsets::PlayerControllerRotationInput, Rotator{});
}

std::vector<BoneSnapshot> probeBones(ProcessInstance<>& memory, const ActorSnapshot& snapshot) {
    constexpr std::array<int, 17> sampleIndices{0, 3, 9, 10, 11, 38, 39, 40, 66, 67, 71, 72, 75, 78, 79, 82, 110};
    std::vector<BoneSnapshot> snapshots;
    for (const ActorPosition& actor : snapshot.positions) {
        const auto mesh = read<std::uint64_t>(memory, actor.actor + Offsets::PlayerPawnMesh);
        if (!mesh.has_value() || !plausiblePointer(*mesh)) continue;
        const auto componentToWorld = read<Transform>(memory, *mesh + Offsets::SkeletalMeshComponentToWorld);
        if (!componentToWorld.has_value() || !plausibleTransform(*componentToWorld)) continue;

        struct Candidate {
            std::uint64_t address;
            BoneArraySource source;
            std::int32_t count;
        };
        std::vector<Candidate> candidates;
        const auto primary = read<std::uint64_t>(memory, *mesh + Offsets::SkeletalMeshBoneArray);
        if (primary.has_value() && plausiblePointer(*primary)) candidates.push_back(Candidate{*primary, BoneArraySource::Primary, 0});
        const auto cache = read<std::uint64_t>(memory, *mesh + Offsets::SkeletalMeshBoneArrayCache);
        if (cache.has_value() && plausiblePointer(*cache)) candidates.push_back(Candidate{*cache, BoneArraySource::Cache, 0});

        for (std::size_t candidateIndex = 0; candidateIndex < candidates.size(); ++candidateIndex) {
            const Candidate& candidate = candidates[candidateIndex];
            bool duplicate = false;
            for (std::size_t earlier = 0; earlier < candidateIndex; ++earlier) {
                if (candidates[earlier].address == candidate.address) duplicate = true;
            }
            if (duplicate) continue;

            BoneSnapshot boneSnapshot{actor.actor, *mesh, candidate.address, candidate.source, candidate.count, actor.x, actor.y, actor.z, {}};
            for (const int index : sampleIndices) {
                if (candidate.count > 0 && index >= candidate.count) continue;
                const auto position = boneWorldPosition(memory, candidate.address, index, *componentToWorld);
                if (!position.has_value()) continue;
                const double distance = std::hypot(std::hypot(position->x - actor.x, position->y - actor.y), position->z - actor.z);
                if (!std::isfinite(distance) || distance > 1000.0) continue;
                boneSnapshot.positions.push_back(BonePosition{index, position->x, position->y, position->z});
            }
            if (!boneSnapshot.positions.empty()) snapshots.push_back(std::move(boneSnapshot));
        }
    }
    return snapshots;
}

}
