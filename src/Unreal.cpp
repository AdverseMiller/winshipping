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
#include <cstdlib>
#include <iostream>
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

struct ProtectedSlotStorage {
    std::array<std::uint8_t, 0x68> bytes{};
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
    std::uint8_t originalComponentFlags;
    std::uint8_t appliedComponentFlags;
    HighlightingData original;
    HighlightingData applied;
    std::uint64_t mesh;
    std::uint8_t originalMeshFlags;
    std::uint8_t appliedMeshFlags;
    std::int32_t originalMeshStencil;
};

struct CameraView {
    Vector3 location;
    Rotator rotation;
    double fov{std::numeric_limits<double>::quiet_NaN()};
};

static_assert(sizeof(Transform) == 0x60);
static_assert(sizeof(HighlightingData) == 8);

constexpr std::uint8_t ShouldHighlightMask = 1U << 0;
constexpr std::uint8_t CustomDepthEnabledMask = 1U << 0;
constexpr std::uint8_t RenderCustomDepthMask = 1U << 2;
constexpr std::uint8_t PlayerHighlightStencil = 12;
std::unordered_map<std::uint64_t, CachedHighlight> cachedHighlights;
std::uint64_t cachedHighlightComponentClass = 0;

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

std::optional<double> resolveControllerFov(ProcessInstance<>& memory, std::uint64_t controller) {
    if (!plausiblePointer(controller)) return std::nullopt;
    const auto scale = read<float>(memory, controller + Offsets::CameraFov);
    if (!scale.has_value() || !std::isfinite(*scale) || *scale < 0.25F || *scale > 4.0F) return std::nullopt;

    // Fortnite stores LocalPlayerCachedLODDistanceFactor here and derives the
    // horizontal angle by multiplying it by 90.  The live default value
    // 0.888889 therefore resolves to the game's normal 80-degree field of view.
    const double fov = static_cast<double>(*scale) * 90.0;
    if (!std::isfinite(fov) || fov < 20.0 || fov > 170.0) return std::nullopt;
    return fov;
}

std::optional<double> resolveCameraFov(ProcessInstance<>& memory, std::uint64_t manager, const Vector3& decodedLocation, const Rotator& decodedRotation) {
    if (!plausiblePointer(manager)) return std::nullopt;
    struct FovLayout {
        std::uint64_t locationOffset{};
        std::uint64_t rotationDelta{};
        std::uint64_t fovDelta{};
        double fov{};
    };
    static std::uint64_t cachedManager = 0;
    static FovLayout cached{};
    const auto validate = [&](const FovLayout& layout) -> std::optional<double> {
        const auto location = read<Vector3>(memory, manager + layout.locationOffset);
        const auto rotation = read<Rotator>(memory, manager + layout.locationOffset + layout.rotationDelta);
        const auto fov = read<float>(memory, manager + layout.locationOffset + layout.fovDelta);
        if (!location.has_value() || !rotation.has_value() || !fov.has_value() || !std::isfinite(*fov) || *fov < 20.0F || *fov > 170.0F) return std::nullopt;
        const double locationError = std::hypot(std::hypot(location->x - decodedLocation.x, location->y - decodedLocation.y), location->z - decodedLocation.z);
        const double pitchError = std::abs(normalizedAngle(rotation->pitch - decodedRotation.pitch));
        const double yawError = std::abs(normalizedAngle(rotation->yaw - decodedRotation.yaw));
        if (!std::isfinite(locationError) || locationError > 2.0 || pitchError > 1.0 || yawError > 1.0) return std::nullopt;
        return *fov;
    };
    if (cachedManager == manager && cached.locationOffset != 0) {
        const auto fov = validate(cached);
        if (fov.has_value()) return fov;
        cached = {};
    }

    std::vector<FovLayout> candidates;
    for (std::uint64_t locationOffset = 0x100; locationOffset < 0x3000; locationOffset += 8) {
        const auto location = read<Vector3>(memory, manager + locationOffset);
        if (!location.has_value()) continue;
        const double locationError = std::hypot(std::hypot(location->x - decodedLocation.x, location->y - decodedLocation.y), location->z - decodedLocation.z);
        if (!std::isfinite(locationError) || locationError > 2.0) continue;
        for (const auto [rotationDelta, fovDelta] : {std::pair<std::uint64_t, std::uint64_t>{0x18, 0x30}, {0x28, 0x50}}) {
            const auto rotation = read<Rotator>(memory, manager + locationOffset + rotationDelta);
            const auto fov = read<float>(memory, manager + locationOffset + fovDelta);
            if (!rotation.has_value() || !fov.has_value() || !std::isfinite(*fov) || *fov < 20.0F || *fov > 170.0F) continue;
            const double pitchError = std::abs(normalizedAngle(rotation->pitch - decodedRotation.pitch));
            const double yawError = std::abs(normalizedAngle(rotation->yaw - decodedRotation.yaw));
            if (pitchError > 1.0 || yawError > 1.0) continue;
            candidates.push_back(FovLayout{locationOffset, rotationDelta, fovDelta, *fov});
        }
    }
    if (candidates.empty()) return std::nullopt;
    const double value = candidates.front().fov;
    if (std::any_of(candidates.begin(), candidates.end(), [value](const FovLayout& candidate) { return std::abs(candidate.fov - value) >= 0.01; })) return std::nullopt;
    cachedManager = manager;
    cached = candidates.front();
    return value;
}

void printFovCandidates(ProcessInstance<>& memory, std::string_view label, std::uint64_t base, std::uint64_t size) {
    if (!plausiblePointer(base)) return;
    std::cerr << "box FOV candidates " << label << " base=0x" << std::hex << base << std::dec << ':';
    std::size_t printed = 0;
    for (std::uint64_t offset = 0; offset + sizeof(float) <= size && printed < 80; offset += 4) {
        const auto value = read<float>(memory, base + offset);
        if (!value.has_value() || !std::isfinite(*value) || *value < 20.0F || *value > 170.0F) continue;
        std::cerr << " +0x" << std::hex << offset << std::dec << "=" << *value;
        ++printed;
    }
    std::cerr << '\n';
}

void printCameraValueCandidates(ProcessInstance<>& memory, std::uint64_t base, const Vector3& location, const Rotator& rotation) {
    if (!plausiblePointer(base)) return;
    std::cerr << "box camera-value candidates:";
    for (std::uint64_t offset = 0; offset + sizeof(double) <= 0x3000; offset += 8) {
        const auto value = read<double>(memory, base + offset);
        if (!value.has_value() || !std::isfinite(*value)) continue;
        const char* label = nullptr;
        if (std::abs(*value - location.x) < 2.0) label = "lx";
        else if (std::abs(*value - location.y) < 2.0) label = "ly";
        else if (std::abs(*value - location.z) < 2.0) label = "lz";
        else if (std::abs(normalizedAngle(*value - rotation.pitch)) < 0.2) label = "pitch";
        else if (std::abs(normalizedAngle(*value - rotation.yaw)) < 0.2) label = "yaw";
        if (label) std::cerr << " +0x" << std::hex << offset << std::dec << '=' << label << '(' << *value << ')';
    }
    std::cerr << '\n';
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
    static bool cameraDiagnosticPrinted = false;
    std::uint64_t cameraManagerAddress = 0;
    double cameraFov = resolveControllerFov(memory, snapshot.localController).value_or(std::numeric_limits<double>::quiet_NaN());
    if (plausiblePointer(snapshot.localController)) {
        const auto manager = read<std::uint64_t>(memory, snapshot.localController + Offsets::PlayerControllerCameraManager);
        if (manager.has_value() && plausiblePointer(*manager)) {
            cameraManagerAddress = *manager;
            const auto fov = read<float>(memory, cameraManagerAddress + Offsets::PlayerCameraManagerCameraCache + Offsets::CameraCachePov + Offsets::MinimalViewFov);
            if (fov.has_value() && std::isfinite(*fov) && *fov >= 20.0F && *fov <= 170.0F) cameraFov = *fov;
            if (!cameraDiagnosticPrinted && std::getenv("WINSHIPPING_BOX_DEBUG") != nullptr) {
                std::cerr << "box camera: controller=0x" << std::hex << snapshot.localController << " manager=0x" << cameraManagerAddress << " pov=0x" << (cameraManagerAddress + Offsets::PlayerCameraManagerCameraCache + Offsets::CameraCachePov) << std::dec << " fov=" << (fov.has_value() ? std::to_string(*fov) : std::string("unreadable")) << '\n';
            }
        }
    }
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
            const Rotator rotation{std::asin(std::clamp(encodedC, -1.0, 1.0)) * radiansToDegrees, std::atan2(-encodedA, encodedB) * radiansToDegrees, 0.0};
            if (!std::isfinite(cameraFov)) cameraFov = resolveCameraFov(memory, cameraManagerAddress, location, rotation).value_or(cameraFov);
            if (!cameraDiagnosticPrinted && std::getenv("WINSHIPPING_BOX_DEBUG") != nullptr) {
                std::cerr << "box camera resolved: location=(" << location.x << ',' << location.y << ',' << location.z << ") rotation=(" << rotation.pitch << ',' << rotation.yaw << ") fov=" << cameraFov << '\n';
                if (!std::isfinite(cameraFov)) {
                    const auto worldFov = read<float>(memory, snapshot.world + Offsets::CameraFov);
                    const auto controllerFov = read<float>(memory, snapshot.localController + Offsets::CameraFov);
                    const auto locationFov = read<float>(memory, snapshot.cameraLocationPointer + Offsets::CameraFov);
                    const auto rotationFov = read<float>(memory, snapshot.cameraRotationPointer + Offsets::CameraFov);
                    std::cerr << "box supplied FOV +0x" << std::hex << Offsets::CameraFov << std::dec
                              << " world=" << (worldFov.has_value() ? std::to_string(*worldFov) : "unreadable")
                              << " controller=" << (controllerFov.has_value() ? std::to_string(*controllerFov) : "unreadable")
                              << " location=" << (locationFov.has_value() ? std::to_string(*locationFov) : "unreadable")
                              << " rotation=" << (rotationFov.has_value() ? std::to_string(*rotationFov) : "unreadable") << '\n';
                    printFovCandidates(memory, "world", snapshot.world, 0x300);
                    printFovCandidates(memory, "controller", snapshot.localController, 0x1000);
                    printFovCandidates(memory, "manager", cameraManagerAddress, 0x3000);
                    printCameraValueCandidates(memory, cameraManagerAddress, location, rotation);
                    printFovCandidates(memory, "camera-location", snapshot.cameraLocationPointer, 0x400);
                    printFovCandidates(memory, "camera-rotation", snapshot.cameraRotationPointer, 0x400);
                }
                cameraDiagnosticPrinted = true;
            }
            return CameraView{location, rotation, cameraFov};
        }
    }

    if (!plausiblePointer(snapshot.localController) || !plausiblePointer(cameraManagerAddress)) return std::nullopt;
    const std::uint64_t pov = cameraManagerAddress + Offsets::PlayerCameraManagerCameraCache + Offsets::CameraCachePov;
    CameraView view{};
    view.fov = cameraFov;
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

std::optional<std::pair<double, double>> worldToScreen(const Vector3& world, const CameraView& camera, std::uint16_t width, std::uint16_t height, double* cameraDepth = nullptr) {
    if (!width || !height || !std::isfinite(camera.fov) || camera.fov < 20.0 || camera.fov > 170.0) return std::nullopt;
    constexpr double degreesToRadians = 0.017453292519943295769;
    const double pitch = camera.rotation.pitch * degreesToRadians;
    const double yaw = camera.rotation.yaw * degreesToRadians;
    const double sinPitch = std::sin(pitch);
    const double cosPitch = std::cos(pitch);
    const double sinYaw = std::sin(yaw);
    const double cosYaw = std::cos(yaw);
    const Vector3 delta{world.x - camera.location.x, world.y - camera.location.y, world.z - camera.location.z};
    const Vector3 forward{cosPitch * cosYaw, cosPitch * sinYaw, sinPitch};
    const Vector3 right{-sinYaw, cosYaw, 0.0};
    const Vector3 up{-sinPitch * cosYaw, -sinPitch * sinYaw, cosPitch};
    const double depth = delta.x * forward.x + delta.y * forward.y + delta.z * forward.z;
    if (cameraDepth != nullptr) *cameraDepth = depth;
    if (!std::isfinite(depth) || depth <= 1.0) return std::nullopt;
    const double horizontal = delta.x * right.x + delta.y * right.y + delta.z * right.z;
    const double vertical = delta.x * up.x + delta.y * up.y + delta.z * up.z;
    const double focal = static_cast<double>(width) * 0.5 / std::tan(camera.fov * degreesToRadians * 0.5);
    const double x = static_cast<double>(width) * 0.5 + horizontal * focal / depth;
    const double y = static_cast<double>(height) * 0.5 - vertical * focal / depth;
    if (!std::isfinite(x) || !std::isfinite(y)) return std::nullopt;
    return std::pair<double, double>{x, y};
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
    const auto mesh = read<std::uint64_t>(memory, pawn + Offsets::PlayerPawnMesh);
    if (!component.has_value() || *component != cached.component || !mesh.has_value() || *mesh != cached.mesh) return false;

    bool restored = true;
    const auto currentComponentFlags = read<std::uint8_t>(memory, cached.component + Offsets::CustomDepthEnabledFlags);
    if (!currentComponentFlags.has_value()) restored = false;
    else if (*currentComponentFlags == cached.appliedComponentFlags && !write(memory, cached.component + Offsets::CustomDepthEnabledFlags, cached.originalComponentFlags)) restored = false;

    const auto currentHighlight = read<HighlightingData>(memory, cached.component + Offsets::CustomDepthDefaultHighlightingData);
    if (!currentHighlight.has_value()) restored = false;
    else if (matchingHighlight(*currentHighlight, cached.applied) && !write(memory, cached.component + Offsets::CustomDepthDefaultHighlightingData, cached.original)) restored = false;

    const auto currentStencil = read<std::int32_t>(memory, cached.mesh + Offsets::PrimitiveCustomDepthStencilValue);
    if (!currentStencil.has_value()) restored = false;
    else if (*currentStencil == PlayerHighlightStencil && !write(memory, cached.mesh + Offsets::PrimitiveCustomDepthStencilValue, cached.originalMeshStencil)) restored = false;

    const auto currentMeshFlags = read<std::uint8_t>(memory, cached.mesh + Offsets::PrimitiveRenderCustomDepthFlags);
    if (!currentMeshFlags.has_value()) restored = false;
    else if (*currentMeshFlags == cached.appliedMeshFlags && !write(memory, cached.mesh + Offsets::PrimitiveRenderCustomDepthFlags, cached.originalMeshFlags)) restored = false;
    return restored;
}

bool applyCachedHighlight(ProcessInstance<>& memory, const CachedHighlight& cached) {
    const std::uint8_t disabledMeshFlags = cached.appliedMeshFlags & static_cast<std::uint8_t>(~RenderCustomDepthMask);
    if (!write(memory, cached.component + Offsets::CustomDepthEnabledFlags, cached.appliedComponentFlags)) return false;
    if (!write(memory, cached.component + Offsets::CustomDepthDefaultHighlightingData, cached.applied)) return false;
    if (!write(memory, cached.mesh + Offsets::PrimitiveRenderCustomDepthFlags, disabledMeshFlags)) return false;
    if (!write(memory, cached.mesh + Offsets::PrimitiveCustomDepthStencilValue, static_cast<std::int32_t>(PlayerHighlightStencil))) return false;
    if (!write(memory, cached.mesh + Offsets::PrimitiveRenderCustomDepthFlags, cached.appliedMeshFlags)) return false;

    const auto componentFlags = read<std::uint8_t>(memory, cached.component + Offsets::CustomDepthEnabledFlags);
    const auto highlight = read<HighlightingData>(memory, cached.component + Offsets::CustomDepthDefaultHighlightingData);
    const auto meshFlags = read<std::uint8_t>(memory, cached.mesh + Offsets::PrimitiveRenderCustomDepthFlags);
    const auto meshStencil = read<std::int32_t>(memory, cached.mesh + Offsets::PrimitiveCustomDepthStencilValue);
    return componentFlags.has_value() && *componentFlags == cached.appliedComponentFlags && highlight.has_value() && matchingHighlight(*highlight, cached.applied) && meshFlags.has_value() && *meshFlags == cached.appliedMeshFlags && meshStencil.has_value() && *meshStencil == PlayerHighlightStencil;
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

std::optional<Quaternion> normalizedQuaternion(const Quaternion& rotation) {
    const double normSquared = rotation.x * rotation.x +
        rotation.y * rotation.y + rotation.z * rotation.z +
        rotation.w * rotation.w;
    if (!std::isfinite(normSquared) || normSquared < 1.0e-12) return std::nullopt;
    const double inverseNorm = 1.0 / std::sqrt(normSquared);
    return Quaternion{
        rotation.x * inverseNorm,
        rotation.y * inverseNorm,
        rotation.z * inverseNorm,
        rotation.w * inverseNorm
    };
}

std::optional<Vector3> meshLocalVector(const Transform& meshWorld,
                                       const Vector3& worldVector) {
    const auto rotation = normalizedQuaternion(meshWorld.rotation);
    if (!rotation.has_value() ||
        std::abs(meshWorld.scale.x) < 1.0e-6 ||
        std::abs(meshWorld.scale.y) < 1.0e-6 ||
        std::abs(meshWorld.scale.z) < 1.0e-6)
        return std::nullopt;
    const Quaternion inverse{
        -rotation->x, -rotation->y, -rotation->z, rotation->w
    };
    const Vector3 localScaled = rotateVector(inverse, worldVector);
    const Vector3 local{
        localScaled.x / meshWorld.scale.x,
        localScaled.y / meshWorld.scale.y,
        localScaled.z / meshWorld.scale.z
    };
    if (!std::isfinite(local.x) || !std::isfinite(local.y) ||
        !std::isfinite(local.z))
        return std::nullopt;
    return local;
}

std::optional<Vector3> meshWorldVector(const Transform& meshWorld,
                                       const Vector3& localVector) {
    const auto rotation = normalizedQuaternion(meshWorld.rotation);
    if (!rotation.has_value()) return std::nullopt;
    const Vector3 scaled{
        localVector.x * meshWorld.scale.x,
        localVector.y * meshWorld.scale.y,
        localVector.z * meshWorld.scale.z
    };
    const Vector3 world = rotateVector(*rotation, scaled);
    if (!std::isfinite(world.x) || !std::isfinite(world.y) ||
        !std::isfinite(world.z))
        return std::nullopt;
    return world;
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

std::array<std::uint64_t, 4> protectedLowSlots(const ProtectedSlotStorage& storage) {
    std::array<std::uint64_t, 4> slots{};
    for (std::size_t index = 0; index < slots.size(); ++index) std::memcpy(&slots[index], storage.bytes.data() + (index * 0x20), sizeof(slots[index]));
    return slots;
}

std::vector<LlamaAimTarget> llamaAimTargets(ProcessInstance<>& memory, const Unreal::ActorSnapshot& snapshot) {
    static std::uint64_t cachedWorld = 0;
    static std::vector<std::uint64_t> cachedActors;
    static std::chrono::steady_clock::time_point nextRefresh{};
    const auto now = std::chrono::steady_clock::now();
    if (cachedWorld != snapshot.world) {
        cachedWorld = snapshot.world;
        cachedActors.clear();
        nextRefresh = now;
    }
    if (cachedActors.empty() || now >= nextRefresh) {
        std::vector<std::uint64_t> worldActors;
        std::unordered_set<std::uint64_t> visitedActors;
        const auto levelsArray = read<RemoteArray>(memory, snapshot.world + Offsets::WorldLevels);
        const auto levels = levelsArray.has_value() ? readPointerArray(memory, *levelsArray) : std::nullopt;
        if (levels.has_value()) {
            for (const std::uint64_t level : *levels) {
                if (!plausiblePointer(level)) continue;
                const auto actorArray = read<RemoteArray>(memory, level + Offsets::LevelActors);
                const auto actors = actorArray.has_value() ? readPointerArray(memory, *actorArray) : std::nullopt;
                if (!actors.has_value()) continue;
                for (const std::uint64_t actor : *actors) {
                    if (plausiblePointer(actor) && visitedActors.insert(actor).second) worldActors.push_back(actor);
                }
            }
        }

        struct ActorSlots {
            std::uint64_t actor{};
            ProtectedSlotStorage storage{};
        };
        std::vector<ActorSlots> actorSlots(worldActors.size());
        std::vector<ReadData> actorSlotReads;
        actorSlotReads.reserve(actorSlots.size());
        for (std::size_t index = 0; index < worldActors.size(); ++index) {
            actorSlots[index].actor = worldActors[index];
            addRead(actorSlotReads, worldActors[index] + Offsets::ObjectProtectedSlots, actorSlots[index].storage);
        }
        runReads(memory, actorSlotReads);

        std::unordered_set<std::uint64_t> uniqueCandidates;
        for (const ActorSlots& actor : actorSlots) {
            for (const std::uint64_t candidate : protectedLowSlots(actor.storage)) {
                if (plausiblePointer(candidate)) uniqueCandidates.insert(candidate);
            }
        }
        struct CandidateSlots {
            std::uint64_t object{};
            ProtectedSlotStorage storage{};
        };
        std::vector<CandidateSlots> candidateSlots;
        candidateSlots.reserve(uniqueCandidates.size());
        for (const std::uint64_t candidate : uniqueCandidates) candidateSlots.push_back(CandidateSlots{candidate, {}});
        std::vector<ReadData> candidateSlotReads;
        candidateSlotReads.reserve(candidateSlots.size());
        for (CandidateSlots& candidate : candidateSlots) addRead(candidateSlotReads, candidate.object + Offsets::ObjectProtectedSlots, candidate.storage);
        runReads(memory, candidateSlotReads);

        std::unordered_map<std::uint64_t, std::array<std::uint64_t, 4>> slotsByObject;
        std::unordered_map<std::uint64_t, std::size_t> nestedPointerVotes;
        slotsByObject.reserve(candidateSlots.size());
        for (const CandidateSlots& candidate : candidateSlots) {
            const auto slots = protectedLowSlots(candidate.storage);
            slotsByObject.emplace(candidate.object, slots);
            std::unordered_set<std::uint64_t> uniqueNestedPointers;
            for (const std::uint64_t nested : slots) {
                if (plausiblePointer(nested)) uniqueNestedPointers.insert(nested);
            }
            for (const std::uint64_t nested : uniqueNestedPointers) ++nestedPointerVotes[nested];
        }
        std::uint64_t metaClass = 0;
        std::size_t metaClassVotes = 0;
        for (const auto& [candidate, votes] : nestedPointerVotes) {
            if (votes > metaClassVotes) {
                metaClass = candidate;
                metaClassVotes = votes;
            }
        }

        std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> actorsByClass;
        if (plausiblePointer(metaClass)) {
            for (const ActorSlots& actor : actorSlots) {
                for (const std::uint64_t candidate : protectedLowSlots(actor.storage)) {
                    const auto candidateStorage = slotsByObject.find(candidate);
                    if (candidateStorage == slotsByObject.end()) continue;
                    if (std::find(candidateStorage->second.begin(), candidateStorage->second.end(), metaClass) == candidateStorage->second.end()) continue;
                    actorsByClass[candidate].push_back(actor.actor);
                    break;
                }
            }
        }

        struct PositionedCandidate {
            std::uint64_t actor{};
            std::uint64_t actorClass{};
            std::uint64_t root{};
            Vector3 position{};
            float supplyDropSpawnOffset{};
        };
        std::vector<PositionedCandidate> positionedCandidates;
        for (const auto& [actorClass, actors] : actorsByClass) {
            if (actors.size() != Config::ExpectedLlamaCount) continue;
            for (const std::uint64_t actor : actors) positionedCandidates.push_back(PositionedCandidate{actor, actorClass, 0, {}, 0.0F});
        }
        std::vector<ReadData> rootReads;
        rootReads.reserve(positionedCandidates.size() * 2);
        for (PositionedCandidate& candidate : positionedCandidates) {
            addRead(rootReads, candidate.actor + Offsets::ActorRootComponent, candidate.root);
            addRead(rootReads, candidate.actor + Offsets::AthenaSupplyDropSpawnOffsetZ, candidate.supplyDropSpawnOffset);
        }
        runReads(memory, rootReads);
        std::vector<ReadData> positionReads;
        positionReads.reserve(positionedCandidates.size());
        for (PositionedCandidate& candidate : positionedCandidates) {
            if (plausiblePointer(candidate.root)) addRead(positionReads, candidate.root + Offsets::SceneComponentRelativeLocation, candidate.position);
        }
        runReads(memory, positionReads);

        std::unordered_map<std::uint64_t, std::vector<PositionedCandidate>> positionedByClass;
        for (const PositionedCandidate& candidate : positionedCandidates) {
            if (!std::isfinite(candidate.position.x) || !std::isfinite(candidate.position.y) || !std::isfinite(candidate.position.z)) continue;
            positionedByClass[candidate.actorClass].push_back(candidate);
        }
        std::vector<std::uint64_t> matchingClasses;
        for (const auto& [actorClass, actors] : positionedByClass) {
            if (actors.size() != Config::ExpectedLlamaCount) continue;
            double spreadSquared = 0.0;
            bool validSupplyDropOffsets = true;
            bool hasPositiveSupplyDropOffset = false;
            for (const PositionedCandidate& actor : actors) {
                const double spawnOffset = static_cast<double>(actor.supplyDropSpawnOffset);
                if (!std::isfinite(spawnOffset) || std::abs(spawnOffset) > Config::LlamaMaximumSpawnOffset) {
                    validSupplyDropOffsets = false;
                    break;
                }
                if (spawnOffset >= Config::LlamaMinimumSpawnOffset) hasPositiveSupplyDropOffset = true;
            }
            if (!validSupplyDropOffsets || !hasPositiveSupplyDropOffset) continue;
            for (std::size_t left = 0; left < actors.size(); ++left) {
                for (std::size_t right = left + 1; right < actors.size(); ++right) {
                    const double dx = actors[left].position.x - actors[right].position.x;
                    const double dy = actors[left].position.y - actors[right].position.y;
                    const double dz = actors[left].position.z - actors[right].position.z;
                    spreadSquared = std::max(spreadSquared, (dx * dx) + (dy * dy) + (dz * dz));
                }
            }
            if (spreadSquared >= Config::LlamaMinimumSpread * Config::LlamaMinimumSpread) matchingClasses.push_back(actorClass);
        }
        if (matchingClasses.size() == 1) {
            const std::uint64_t llamaClass = matchingClasses.front();
            std::vector<std::uint64_t> discoveredActors;
            for (const PositionedCandidate& candidate : positionedByClass[llamaClass]) discoveredActors.push_back(candidate.actor);
            if (discoveredActors.size() == Config::ExpectedLlamaCount) cachedActors = std::move(discoveredActors);
        }
        nextRefresh = now + (cachedActors.empty() ? Config::LlamaDiscoveryRetryInterval : Config::LlamaDiscoveryRefreshInterval);
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
        snapshot.positions.push_back(ActorPosition{fields.pawn, fields.playerState,
            fields.root, fields.mesh, fields.team, component.lastRenderTime,
            component.location.x, component.location.y, component.location.z});
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

std::vector<PlayerBoxTarget> playerBoxTargets(ProcessInstance<>& memory,
                                               const ActorSnapshot& snapshot,
                                               const std::vector<PlayerBoxTarget>& previousTargets,
                                               bool ignoreTeams) {
    std::vector<PlayerBoxTarget> targets;
    targets.reserve(std::max(snapshot.positions.size(), previousTargets.size()));
    std::unordered_map<std::uint64_t, const PlayerBoxTarget*> previousByActor;
    previousByActor.reserve(previousTargets.size());
    for (const PlayerBoxTarget& previous : previousTargets)
        previousByActor.emplace(previous.actor, &previous);
    std::unordered_set<std::uint64_t> emittedActors;
    std::unordered_set<std::uint64_t> blockedActors;
    emittedActors.reserve(previousTargets.size() + snapshot.positions.size());
    blockedActors.reserve(snapshot.positions.size());

    const auto retainPrevious = [&](const PlayerBoxTarget& previous) {
        if (previous.missedRebuilds >= Config::BoxTargetGraceRebuilds) return;
        PlayerBoxTarget retained = previous;
        ++retained.missedRebuilds;
        targets.push_back(retained);
        emittedActors.insert(retained.actor);
    };

    for (const ActorPosition& candidate : snapshot.positions) {
        if (candidate.actor == snapshot.localPawn ||
            (!ignoreTeams && snapshot.localTeam != 0xFF &&
             candidate.team != 0xFF && candidate.team == snapshot.localTeam)) {
            blockedActors.insert(candidate.actor);
            continue;
        }
        const auto previousIterator = previousByActor.find(candidate.actor);
        const PlayerBoxTarget* previous = previousIterator != previousByActor.end()
            ? previousIterator->second : nullptr;
        const bool matchingIdentity = previous != nullptr &&
            previous->root == candidate.root && previous->mesh == candidate.mesh;
        if (!plausiblePointer(candidate.root) || !plausiblePointer(candidate.mesh)) {
            if (matchingIdentity) retainPrevious(*previous);
            else blockedActors.insert(candidate.actor);
            continue;
        }
        if (matchingIdentity) {
            PlayerBoxTarget refreshed = *previous;
            refreshed.visible = isVisible(candidate, snapshot.worldSeconds);
            refreshed.missedRebuilds = 0;
            targets.push_back(refreshed);
            emittedActors.insert(candidate.actor);
            continue;
        }
        const auto meshWorld = read<Transform>(
            memory, candidate.mesh + Offsets::SkeletalMeshComponentToWorld);
        if (!meshWorld.has_value() || !plausibleTransform(*meshWorld)) {
            blockedActors.insert(candidate.actor);
            continue;
        }
        const Vector3 worldMeshToRoot{
            candidate.x - meshWorld->translation.x,
            candidate.y - meshWorld->translation.y,
            candidate.z - meshWorld->translation.z
        };
        const auto localMeshToRoot = meshLocalVector(*meshWorld, worldMeshToRoot);
        if (!localMeshToRoot.has_value() ||
            std::hypot(std::hypot(worldMeshToRoot.x, worldMeshToRoot.y),
                       worldMeshToRoot.z) > 250.0) {
            blockedActors.insert(candidate.actor);
            continue;
        }

        targets.push_back(PlayerBoxTarget{
            candidate.actor,
            candidate.root,
            candidate.mesh,
            localMeshToRoot->x,
            localMeshToRoot->y,
            localMeshToRoot->z,
            candidate.x,
            candidate.y,
            candidate.z,
            isVisible(candidate, snapshot.worldSeconds),
            0
        });
        emittedActors.insert(candidate.actor);
    }

    for (const PlayerBoxTarget& previous : previousTargets) {
        if (emittedActors.find(previous.actor) != emittedActors.end() ||
            blockedActors.find(previous.actor) != blockedActors.end())
            continue;
        retainPrevious(previous);
    }
    return targets;
}

bool refreshPlayerBoxTargets(ProcessInstance<>& memory,
                             std::vector<PlayerBoxTarget>& targets,
                             double worldSeconds) {
    struct TargetUpdate {
        Vector3 location{};
        Transform meshWorld{};
        float lastRenderTime{std::numeric_limits<float>::lowest()};
    };
    struct MotionSample {
        Vector3 root{};
        Vector3 mesh{};
        bool initialized{};
    };
    static std::unordered_map<std::uint64_t, MotionSample> previousMotion;
    static auto diagnosticStart = std::chrono::steady_clock::now();
    static std::uint64_t diagnosticPolls = 0;
    static std::uint64_t diagnosticRootChanges = 0;
    static std::uint64_t diagnosticMeshChanges = 0;
    const bool diagnoseMotion = std::getenv("WINSHIPPING_BOX_DEBUG_MOTION") != nullptr;
    std::vector<TargetUpdate> updates(targets.size());
    std::vector<ReadData> reads;
    reads.reserve(targets.size() * 3);
    for (std::size_t index = 0; index < targets.size(); ++index) {
        if (plausiblePointer(targets[index].root))
            addRead(reads, targets[index].root + Offsets::SceneComponentRelativeLocation,
                    updates[index].location);
        if (plausiblePointer(targets[index].mesh))
            addRead(reads, targets[index].mesh + Offsets::SkeletalMeshComponentToWorld,
                    updates[index].meshWorld);
        if (plausiblePointer(targets[index].mesh))
            addRead(reads, targets[index].mesh + Offsets::PrimitiveComponentLastRenderTime,
                    updates[index].lastRenderTime);
    }
    const bool readsSucceeded = runReads(memory, reads);
    if (!readsSucceeded) return false;
    for (std::size_t index = 0; index < targets.size(); ++index) {
        const TargetUpdate& update = updates[index];
        const bool validRoot = std::isfinite(update.location.x) &&
            std::isfinite(update.location.y) && std::isfinite(update.location.z);
        const bool validMesh = plausibleTransform(update.meshWorld);
        if (diagnoseMotion && (validRoot || validMesh)) {
            MotionSample& previous = previousMotion[targets[index].actor];
            if (previous.initialized) {
                if (validRoot && std::hypot(
                        std::hypot(update.location.x - previous.root.x,
                                   update.location.y - previous.root.y),
                        update.location.z - previous.root.z) > 0.001)
                    ++diagnosticRootChanges;
                if (validMesh && std::hypot(
                        std::hypot(update.meshWorld.translation.x - previous.mesh.x,
                                   update.meshWorld.translation.y - previous.mesh.y),
                        update.meshWorld.translation.z - previous.mesh.z) > 0.001)
                    ++diagnosticMeshChanges;
            }
            if (validRoot) previous.root = update.location;
            if (validMesh) previous.mesh = update.meshWorld.translation;
            previous.initialized = true;
        }
        if (validMesh) {
            const Vector3 localMeshToRoot{
                targets[index].meshLocalToRootX,
                targets[index].meshLocalToRootY,
                targets[index].meshLocalToRootZ
            };
            const auto worldMeshToRoot = meshWorldVector(
                update.meshWorld, localMeshToRoot);
            if (worldMeshToRoot.has_value()) {
                targets[index].rootX = update.meshWorld.translation.x + worldMeshToRoot->x;
                targets[index].rootY = update.meshWorld.translation.y + worldMeshToRoot->y;
                targets[index].rootZ = update.meshWorld.translation.z + worldMeshToRoot->z;
            } else if (validRoot) {
                targets[index].rootX = update.location.x;
                targets[index].rootY = update.location.y;
                targets[index].rootZ = update.location.z;
            }
        } else if (validRoot) {
            targets[index].rootX = update.location.x;
            targets[index].rootY = update.location.y;
            targets[index].rootZ = update.location.z;
        }
        targets[index].visible = std::isfinite(update.lastRenderTime) &&
            std::isfinite(worldSeconds) &&
            worldSeconds - static_cast<double>(update.lastRenderTime) <= 0.06;
    }
    if (diagnoseMotion) {
        ++diagnosticPolls;
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(
            now - diagnosticStart).count();
        if (seconds >= 1.0) {
            std::cerr << "box motion samples: seconds=" << seconds
                      << " polls=" << diagnosticPolls
                      << " root_changes=" << diagnosticRootChanges
                      << " mesh_changes=" << diagnosticMeshChanges << '\n';
            diagnosticStart = now;
            diagnosticPolls = 0;
            diagnosticRootChanges = 0;
            diagnosticMeshChanges = 0;
        }
    }
    return readsSucceeded;
}

std::string_view boxProjectionFailureName(BoxProjectionFailure failure) {
    switch (failure) {
        case BoxProjectionFailure::None: return "none";
        case BoxProjectionFailure::CameraUnavailable: return "camera-unavailable";
        case BoxProjectionFailure::BehindCamera: return "behind-camera";
        case BoxProjectionFailure::InvalidProjection: return "invalid-projection";
        case BoxProjectionFailure::InvalidExtent: return "invalid-extent";
        case BoxProjectionFailure::Offscreen: return "offscreen";
    }
    return "unknown";
}

std::vector<PlayerBox> projectPlayerBoxes(ProcessInstance<>& memory, const ActorSnapshot& snapshot, const std::vector<PlayerBoxTarget>& targets, std::uint16_t screenWidth, std::uint16_t screenHeight, BoxProjectionDebug* debug) {
    struct SmoothedExtent {
        double halfWidth;
        double halfHeight;
        std::chrono::steady_clock::time_point updated;
    };
    static std::unordered_map<std::uint64_t, SmoothedExtent> smoothedExtents;
    constexpr double capsuleHalfHeight = 90.0;
    constexpr double capsuleRadius = 42.0;
    constexpr double smoothingTimeSeconds = 0.020;

    std::vector<PlayerBox> boxes;
    const auto camera = readCamera(memory, snapshot);
    if (debug != nullptr) {
        debug->cameraValid = camera.has_value() && std::isfinite(camera->fov);
        debug->targetCount = targets.size();
        debug->projectedCount = 0;
        debug->players.clear();
        debug->players.reserve(targets.size());
        if (camera.has_value()) {
            debug->cameraX = camera->location.x;
            debug->cameraY = camera->location.y;
            debug->cameraZ = camera->location.z;
            debug->cameraPitch = camera->rotation.pitch;
            debug->cameraYaw = camera->rotation.yaw;
            debug->cameraFov = camera->fov;
        }
    }
    if (!camera.has_value() || !std::isfinite(camera->fov)) {
        if (debug != nullptr) {
            for (const PlayerBoxTarget& candidate : targets) {
                debug->players.push_back(PlayerBoxDebug{
                    candidate.actor, candidate.root, candidate.mesh,
                    candidate.rootX, candidate.rootY, candidate.rootZ, 0.0,
                    candidate.visible, false,
                    BoxProjectionFailure::CameraUnavailable, {}});
            }
        }
        return boxes;
    }
    const auto now = std::chrono::steady_clock::now();
    std::unordered_set<std::uint64_t> seenActors;
    boxes.reserve(targets.size());
    seenActors.reserve(targets.size());
    for (const PlayerBoxTarget& candidate : targets) {
        PlayerBoxDebug playerDebug{
            candidate.actor, candidate.root, candidate.mesh,
            candidate.rootX, candidate.rootY, candidate.rootZ,
            std::numeric_limits<double>::infinity(), candidate.visible,
            false, BoxProjectionFailure::None, {}};
        double rawLeft = std::numeric_limits<double>::infinity();
        double rawTop = std::numeric_limits<double>::infinity();
        double rawRight = -std::numeric_limits<double>::infinity();
        double rawBottom = -std::numeric_limits<double>::infinity();
        bool projected = true;
        bool behindCamera = false;
        for (const double zOffset : {-capsuleHalfHeight, capsuleHalfHeight}) {
            for (const double xOffset : {-capsuleRadius, capsuleRadius}) {
                for (const double yOffset : {-capsuleRadius, capsuleRadius}) {
                    double depth = std::numeric_limits<double>::quiet_NaN();
                    const auto point = worldToScreen(
                        Vector3{candidate.rootX + xOffset,
                                candidate.rootY + yOffset,
                                candidate.rootZ + zOffset},
                        *camera, screenWidth, screenHeight, &depth);
                    if (std::isfinite(depth))
                        playerDebug.minimumDepth = std::min(playerDebug.minimumDepth, depth);
                    if (!point.has_value()) {
                        if (std::isfinite(depth) && depth <= 1.0) behindCamera = true;
                        projected = false;
                        break;
                    }
                    rawLeft = std::min(rawLeft, point->first);
                    rawTop = std::min(rawTop, point->second);
                    rawRight = std::max(rawRight, point->first);
                    rawBottom = std::max(rawBottom, point->second);
                }
                if (!projected) break;
            }
            if (!projected) break;
        }
        if (!projected) {
            playerDebug.failure = behindCamera
                ? BoxProjectionFailure::BehindCamera
                : BoxProjectionFailure::InvalidProjection;
            if (debug != nullptr) debug->players.push_back(playerDebug);
            continue;
        }

        const double centerX = (rawLeft + rawRight) * 0.5;
        const double centerY = (rawTop + rawBottom) * 0.5;
        const double rawHalfWidth = (rawRight - rawLeft) * 0.5;
        const double rawHalfHeight = (rawBottom - rawTop) * 0.5;
        if (!std::isfinite(rawHalfWidth) || !std::isfinite(rawHalfHeight) ||
            rawHalfWidth < 2.0 || rawHalfHeight < 4.0 ||
            rawHalfHeight > static_cast<double>(screenHeight)) {
            playerDebug.failure = BoxProjectionFailure::InvalidExtent;
            if (debug != nullptr) debug->players.push_back(playerDebug);
            continue;
        }

        seenActors.insert(candidate.actor);
        auto [extentIterator, inserted] = smoothedExtents.try_emplace(
            candidate.actor, SmoothedExtent{rawHalfWidth, rawHalfHeight, now});
        SmoothedExtent& extent = extentIterator->second;
        if (!inserted) {
            const double elapsed = std::chrono::duration<double>(now - extent.updated).count();
            const double alpha = std::clamp(
                1.0 - std::exp(-elapsed / smoothingTimeSeconds), 0.05, 1.0);
            extent.halfWidth += (rawHalfWidth - extent.halfWidth) * alpha;
            extent.halfHeight += (rawHalfHeight - extent.halfHeight) * alpha;
            extent.updated = now;
        }
        const double left = centerX - extent.halfWidth;
        const double top = centerY - extent.halfHeight;
        const double right = centerX + extent.halfWidth;
        const double bottom = centerY + extent.halfHeight;
        if (right <= 0.0 || bottom <= 0.0 || left >= screenWidth || top >= screenHeight) {
            playerDebug.failure = BoxProjectionFailure::Offscreen;
            if (debug != nullptr) debug->players.push_back(playerDebug);
            continue;
        }
        const double clippedLeft = std::clamp(left, 0.0, static_cast<double>(screenWidth - 1));
        const double clippedTop = std::clamp(top, 0.0, static_cast<double>(screenHeight - 1));
        const double clippedRight = std::clamp(right, clippedLeft + 1.0, static_cast<double>(screenWidth));
        const double clippedBottom = std::clamp(bottom, clippedTop + 1.0, static_cast<double>(screenHeight));
        const PlayerBox box{
            candidate.actor,
            static_cast<std::uint16_t>(std::lround(clippedLeft)),
            static_cast<std::uint16_t>(std::lround(clippedTop)),
            static_cast<std::uint16_t>(std::max(1.0, std::round(clippedRight - clippedLeft))),
            static_cast<std::uint16_t>(std::max(1.0, std::round(clippedBottom - clippedTop))),
            candidate.visible
        };
        boxes.push_back(box);
        if (debug != nullptr) {
            playerDebug.projected = true;
            playerDebug.box = box;
            debug->players.push_back(playerDebug);
            ++debug->projectedCount;
        }
    }
    for (auto iterator = smoothedExtents.begin(); iterator != smoothedExtents.end();) {
        if (seenActors.find(iterator->first) == seenActors.end())
            iterator = smoothedExtents.erase(iterator);
        else
            ++iterator;
    }
    return boxes;
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
            const auto mesh = read<std::uint64_t>(memory, iterator->first + Offsets::PlayerPawnMesh);
            if (component.has_value() && *component == iterator->second.component && mesh.has_value() && *mesh == iterator->second.mesh) {
                if (!applyCachedHighlight(memory, iterator->second)) ++result.failed;
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
        const auto componentClass = read<std::uint64_t>(memory, *component + Offsets::ObjectClass);
        const auto componentFlags = read<std::uint8_t>(memory, *component + Offsets::CustomDepthEnabledFlags);
        const auto original = read<HighlightingData>(memory, *component + Offsets::CustomDepthDefaultHighlightingData);
        const auto mesh = read<std::uint64_t>(memory, pawn + Offsets::PlayerPawnMesh);
        if (!componentClass.has_value() || !plausiblePointer(*componentClass) || !componentFlags.has_value() || !original.has_value() || !mesh.has_value() || !plausiblePointer(*mesh)) {
            ++result.failed;
            continue;
        }
        if (cachedHighlightComponentClass == 0) cachedHighlightComponentClass = *componentClass;
        if (*componentClass != cachedHighlightComponentClass) {
            ++result.failed;
            continue;
        }

        const HighlightingData applied = playerHighlight(*original);
        const auto meshFlags = read<std::uint8_t>(memory, *mesh + Offsets::PrimitiveRenderCustomDepthFlags);
        const auto meshStencil = read<std::int32_t>(memory, *mesh + Offsets::PrimitiveCustomDepthStencilValue);
        if (!meshFlags.has_value() || !meshStencil.has_value()) {
            ++result.failed;
            continue;
        }
        const std::uint8_t appliedComponentFlags = *componentFlags | CustomDepthEnabledMask;
        const std::uint8_t appliedMeshFlags = *meshFlags | RenderCustomDepthMask;
        const CachedHighlight cached{*component, *componentFlags, appliedComponentFlags, *original, applied, *mesh, *meshFlags, appliedMeshFlags, *meshStencil};
        if (!applyCachedHighlight(memory, cached)) {
            restoreCachedHighlight(memory, pawn, cached);
            ++result.failed;
            continue;
        }
        cachedHighlights.emplace(pawn, cached);
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
