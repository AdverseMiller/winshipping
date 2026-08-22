#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace Config {

inline constexpr std::uint16_t BoxScreenWidth = 1920;
inline constexpr std::uint16_t BoxScreenHeight = 1080;
inline constexpr std::uint32_t DefaultBoxRepaintHz = 2000;

// Fill this in with the Windows executable name before running the program.
inline constexpr std::string_view TargetProcessName = "FortniteClient-Win64-Shipping.exe";

inline constexpr std::string_view QemuTarget = "win-gaming";
inline constexpr std::chrono::milliseconds DisplayInterval{200};
inline constexpr std::chrono::milliseconds ActorRefreshInterval{16};
inline constexpr std::chrono::milliseconds BoxAnchorRefreshInterval{4};
inline constexpr std::chrono::microseconds BoxRefreshInterval{2778};
inline constexpr std::uint8_t BoxTargetGraceRebuilds = 8;
inline constexpr std::chrono::milliseconds ActorSnapshotExpiry{100};
inline constexpr std::chrono::milliseconds HighlightRefreshInterval{16};
inline constexpr std::chrono::milliseconds VehicleStateRefreshInterval{100};
inline constexpr std::chrono::milliseconds WeaponStateRefreshInterval{100};
inline constexpr std::chrono::milliseconds ActiveLoopInterval{4};
inline constexpr std::chrono::milliseconds IdleLoopInterval{8};
inline constexpr std::chrono::seconds GroundItemRefreshInterval{2};
inline constexpr std::chrono::seconds NegativePickupCacheLifetime{30};
inline constexpr std::chrono::seconds LlamaDiscoveryRefreshInterval{10};
inline constexpr std::chrono::seconds LlamaDiscoveryRetryInterval{1};
inline constexpr std::size_t GroundItemProbeBudget = 32;
inline constexpr std::size_t ExpectedLlamaCount = 3;
inline constexpr double DefaultSmoothing = 11.0;
inline constexpr double HeadAimMaximumDistance = 5000.0;
inline constexpr double MaximumAimAngle = 30.0;
inline constexpr double LlamaAimSmoothing = 1.0;
inline constexpr double LlamaAimHeight = 75.0;
inline constexpr double LlamaMinimumSpread = 10000.0;
inline constexpr double LlamaMinimumSpawnOffset = 1.0;
inline constexpr double LlamaMaximumSpawnOffset = 1000.0;
inline constexpr double GroundItemAimSmoothing = 1.0;
inline constexpr std::uint64_t ExpectedImageBase = 0x140000000;
inline constexpr std::uint64_t EprocessSectionBaseOffset = 0x2B0;
inline constexpr std::size_t MaximumActorCount = 100000;

}
