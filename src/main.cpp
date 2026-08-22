#include "Config.hpp"
#include "BoxOverlay.hpp"
#include "Dtb.hpp"
#include "Unreal.hpp"

#include "memflow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <pwd.h>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

volatile std::sig_atomic_t stopRequested = 0;

void requestStop(int) {
    stopRequested = 1;
}

void addPluginDirectory(Inventory* inventory, std::set<std::string>& addedDirectories, const std::filesystem::path& directory) {
    std::error_code error;
    if (!std::filesystem::is_directory(directory, error)) return;
    bool containsPlugin = false;
    for (std::filesystem::directory_iterator iterator(directory, error); !error && iterator != std::filesystem::directory_iterator(); iterator.increment(error)) {
        const std::string filename = iterator->path().filename().string();
        const std::string extension = iterator->path().extension().string();
        if (filename.rfind("libmemflow_", 0) == 0 && (extension == ".so" || extension == ".dylib" || extension == ".dll")) {
            containsPlugin = true;
            break;
        }
    }
    if (!containsPlugin) return;
    const std::string path = directory.string();
    if (!addedDirectories.insert(path).second) return;
    if (mf_inventory_add_dir(inventory, path.c_str()) == 0) std::cerr << "added memflow plugin search path: " << path << '\n';
}

void addUserPluginDirectory(Inventory* inventory, std::set<std::string>& addedDirectories, uid_t uid) {
    const passwd* user = getpwuid(uid);
    if (user == nullptr || user->pw_dir == nullptr || user->pw_dir[0] == '\0') return;
    addPluginDirectory(inventory, addedDirectories, std::filesystem::path(user->pw_dir) / ".local/lib/memflow");
}

void addOwnedPathPluginDirectory(Inventory* inventory, std::set<std::string>& addedDirectories, std::filesystem::path path) {
    while (!path.empty()) {
        struct stat status{};
        if (stat(path.c_str(), &status) == 0) addUserPluginDirectory(inventory, addedDirectories, status.st_uid);
        if (path == path.root_path()) break;
        path = path.parent_path();
    }
}

void addEnvironmentUserPluginDirectory(Inventory* inventory, std::set<std::string>& addedDirectories, const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') return;
    char* end = nullptr;
    const unsigned long uid = std::strtoul(value, &end, 10);
    if (end != value && *end == '\0') addUserPluginDirectory(inventory, addedDirectories, static_cast<uid_t>(uid));
}

Inventory* discoverMemflowPlugins() {
    Inventory* inventory = mf_inventory_scan();
    if (inventory == nullptr) return nullptr;

    std::set<std::string> addedDirectories;
    const char* sudoUser = std::getenv("SUDO_USER");
    if (sudoUser != nullptr && sudoUser[0] != '\0') {
        const passwd* user = getpwnam(sudoUser);
        if (user != nullptr) addUserPluginDirectory(inventory, addedDirectories, user->pw_uid);
    }
    addEnvironmentUserPluginDirectory(inventory, addedDirectories, "SUDO_UID");
    addEnvironmentUserPluginDirectory(inventory, addedDirectories, "PKEXEC_UID");
    addUserPluginDirectory(inventory, addedDirectories, getuid());
    addUserPluginDirectory(inventory, addedDirectories, geteuid());

    std::error_code error;
    const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (!error) {
        addOwnedPathPluginDirectory(inventory, addedDirectories, executable);
        addPluginDirectory(inventory, addedDirectories, executable.parent_path());
        addPluginDirectory(inventory, addedDirectories, executable.parent_path() / "memflow");
        addPluginDirectory(inventory, addedDirectories, executable.parent_path().parent_path() / "lib/memflow");
    }
    const std::filesystem::path workingDirectory = std::filesystem::current_path(error);
    if (!error) addOwnedPathPluginDirectory(inventory, addedDirectories, workingDirectory);

    addPluginDirectory(inventory, addedDirectories, "/usr/local/lib/memflow");
    addPluginDirectory(inventory, addedDirectories, "/usr/local/lib64/memflow");
    addPluginDirectory(inventory, addedDirectories, "/usr/lib/memflow");
    addPluginDirectory(inventory, addedDirectories, "/usr/lib64/memflow");
    return inventory;
}

struct Arguments {
    double smoothing{Config::DefaultSmoothing};
    std::optional<double> vehicleSmoothing;
    std::array<std::optional<double>, static_cast<std::size_t>(Unreal::WeaponCategory::Count)> weaponSmoothing;
    bool clearAim{};
    bool readOnly{};
    bool boneProbe{};
    bool ignoreIsABot{};
    bool ignoreTeams{};
    bool printActors{};
    bool chams{};
    bool boxes{};
    bool debug{};
    std::uint32_t boxRepaintHz{Config::DefaultBoxRepaintHz};
    std::optional<Unreal::ItemRarity> rarity;
    std::string itemName;
};

std::optional<std::uint32_t> parseBoxRepaintHz(std::string_view value) {
    const std::string text(value);
    char* end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        parsed == 0 || parsed > 100000) return std::nullopt;
    return static_cast<std::uint32_t>(parsed);
}

std::optional<Unreal::WeaponCategory> parseWeaponCategory(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char character : value) normalized.push_back(static_cast<char>(std::tolower(character)));
    if (normalized == "unknown" || normalized == "other") return Unreal::WeaponCategory::Unknown;
    if (normalized == "pistol") return Unreal::WeaponCategory::Pistol;
    if (normalized == "shotgun") return Unreal::WeaponCategory::Shotgun;
    if (normalized == "rifle") return Unreal::WeaponCategory::Rifle;
    if (normalized == "smg") return Unreal::WeaponCategory::Smg;
    if (normalized == "sniper") return Unreal::WeaponCategory::Sniper;
    if (normalized == "launcher") return Unreal::WeaponCategory::Launcher;
    if (normalized == "bow") return Unreal::WeaponCategory::Bow;
    if (normalized == "minigun") return Unreal::WeaponCategory::Minigun;
    if (normalized == "melee") return Unreal::WeaponCategory::Melee;
    if (normalized == "utility") return Unreal::WeaponCategory::Utility;
    if (normalized == "unarmed") return Unreal::WeaponCategory::Unarmed;
    return std::nullopt;
}

std::optional<double> parseSmoothingValue(std::string_view value) {
    std::size_t consumed = 0;
    double parsed{};
    try {
        parsed = std::stod(std::string(value), &consumed);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (consumed != value.size() || !std::isfinite(parsed) || parsed < 1.0) return std::nullopt;
    return parsed;
}

bool hasWeaponSmoothing(const Arguments& arguments) {
    return std::any_of(arguments.weaponSmoothing.begin(), arguments.weaponSmoothing.end(), [](const std::optional<double>& smoothing) { return smoothing.has_value(); });
}

std::optional<double> configuredWeaponSmoothing(const Arguments& arguments, const std::optional<Unreal::WeaponSnapshot>& weapon) {
    if (!weapon.has_value()) return std::nullopt;
    const std::size_t index = static_cast<std::size_t>(weapon->category);
    if (index >= arguments.weaponSmoothing.size()) return std::nullopt;
    return arguments.weaponSmoothing[index];
}

double effectivePawnSmoothing(const Arguments& arguments, const std::optional<std::uint64_t>& vehicle, const std::optional<Unreal::WeaponSnapshot>& weapon) {
    double smoothing = arguments.smoothing;
    if (arguments.vehicleSmoothing.has_value() && vehicle.has_value() && *vehicle != 0) smoothing = *arguments.vehicleSmoothing;
    const auto weaponSmoothing = configuredWeaponSmoothing(arguments, weapon);
    if (weaponSmoothing.has_value()) smoothing = *weaponSmoothing;
    return smoothing;
}

std::string_view itemRarityName(Unreal::ItemRarity rarity) {
    switch (rarity) {
        case Unreal::ItemRarity::Common: return "Common";
        case Unreal::ItemRarity::Uncommon: return "Uncommon";
        case Unreal::ItemRarity::Rare: return "Rare";
        case Unreal::ItemRarity::Epic: return "Epic";
        case Unreal::ItemRarity::Legendary: return "Legendary";
        case Unreal::ItemRarity::Mythic: return "Mythic";
        case Unreal::ItemRarity::Transcendent: return "Transcendent";
        case Unreal::ItemRarity::Unattainable: return "Unattainable";
    }
    return "Unknown";
}

std::optional<Unreal::ItemRarity> parseItemRarity(std::string_view value) {
    std::string normalized;
    normalized.reserve(value.size());
    for (const unsigned char character : value) normalized.push_back(static_cast<char>(std::tolower(character)));
    if (normalized == "common") return Unreal::ItemRarity::Common;
    if (normalized == "uncommon") return Unreal::ItemRarity::Uncommon;
    if (normalized == "rare") return Unreal::ItemRarity::Rare;
    if (normalized == "epic") return Unreal::ItemRarity::Epic;
    if (normalized == "legendary") return Unreal::ItemRarity::Legendary;
    if (normalized == "mythic") return Unreal::ItemRarity::Mythic;
    if (normalized == "transcendent") return Unreal::ItemRarity::Transcendent;
    if (normalized == "unattainable") return Unreal::ItemRarity::Unattainable;
    return std::nullopt;
}

void printUsage(std::ostream& output, const char* executable) {
    output << "usage: " << executable << " [--smoothing FLOAT] [--vehicle-smoothing FLOAT] [--weapon-smoothing TYPE=FLOAT] [--item-name \"NAME\" [--rarity RARITY]] [--chams|--boxes] [--hz HZ] [--debug] [--clear-aim] [--read-only] [--bone-probe] [--ignore-isabot] [--ignore-teams] [--print-actors]\n";
    output << "  --smoothing FLOAT  divide pitch/yaw correction by FLOAT each iteration (default " << Config::DefaultSmoothing << ")\n";
    output << "  --vehicle-smoothing FLOAT  use FLOAT instead while the local pawn is on a vehicle\n";
    output << "  --weapon-smoothing TYPE=FLOAT  repeatable held-weapon override; TYPE is pistol, shotgun, rifle, smg, sniper, launcher, bow, minigun, melee, utility, unarmed, or unknown\n";
    output << "  --item-name NAME   find ground pickups containing NAME and aim at the nearest while Y is held\n";
    output << "  --rarity RARITY    restrict item-name matches to RARITY or higher (Common through Unattainable)\n";
    output << "  --chams            apply the engine highlight to aim-eligible player pawns\n";
    output << "  --boxes            draw host-side boxes; green when visible, red when occluded\n";
    output << "  --hz HZ            BAR1 repaint rate for --boxes (default " << Config::DefaultBoxRepaintHz << ", range 1-100000)\n";
    output << "  --debug            print one-second box, camera, player, and delivery diagnostics\n";
    output << "  --clear-aim        clear pending controller rotation input and exit\n";
    output << "  --read-only        disable targeting and memory writes while still reporting right-click\n";
    output << "  --bone-probe       resolve sample bones once and exit without writing memory\n";
    output << "  --ignore-isabot    include AI-controlled player states in output, aim, chams, and boxes\n";
    output << "  --ignore-teams     allow aim, chams, and boxes to affect pawns on the local team\n";
    output << "  --print-actors     print every resolved pawn instead of the closest five\n";
}

std::optional<Arguments> parseArguments(int argc, char** argv) {
    Arguments arguments{};
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            printUsage(std::cout, argv[0]);
            return std::nullopt;
        }
        if (argument == "--clear-aim") {
            arguments.clearAim = true;
            continue;
        }
        if (argument == "--read-only") {
            arguments.readOnly = true;
            continue;
        }
        if (argument == "--bone-probe") {
            arguments.boneProbe = true;
            continue;
        }
        if (argument == "--ignore-isabot") {
            arguments.ignoreIsABot = true;
            continue;
        }
        if (argument == "--ignore-teams") {
            arguments.ignoreTeams = true;
            continue;
        }
        if (argument == "--print-actors") {
            arguments.printActors = true;
            continue;
        }
        if (argument == "--chams") {
            arguments.chams = true;
            continue;
        }
        if (argument == "--boxes") {
            arguments.boxes = true;
            continue;
        }
        if (argument == "--debug") {
            arguments.debug = true;
            continue;
        }
        if ((argument == "--hz" || argument == "--box-hz") && index + 1 < argc) {
            const std::string value = argv[++index];
            const auto parsed = parseBoxRepaintHz(value);
            if (!parsed.has_value()) {
                std::cerr << argument << " must be an integer from 1 through 100000\n";
                return std::nullopt;
            }
            arguments.boxRepaintHz = *parsed;
            continue;
        }
        if (argument == "--item-name" && index + 1 < argc) {
            arguments.itemName = argv[++index];
            if (arguments.itemName.empty()) {
                std::cerr << "--item-name cannot be empty\n";
                return std::nullopt;
            }
            continue;
        }
        if (argument == "--rarity" && index + 1 < argc) {
            const std::string value = argv[++index];
            arguments.rarity = parseItemRarity(value);
            if (!arguments.rarity.has_value()) {
                std::cerr << "invalid rarity: " << value << " (expected Common, Uncommon, Rare, Epic, Legendary, Mythic, Transcendent, or Unattainable)\n";
                return std::nullopt;
            }
            continue;
        }
        if (argument == "--weapon-smoothing" && index + 1 < argc) {
            const std::string value = argv[++index];
            const std::size_t separator = value.find('=');
            if (separator == std::string::npos || separator == 0 || separator + 1 >= value.size()) {
                std::cerr << "invalid weapon smoothing: " << value << " (expected TYPE=FLOAT)\n";
                return std::nullopt;
            }
            const auto category = parseWeaponCategory(std::string_view(value).substr(0, separator));
            if (!category.has_value()) {
                std::cerr << "invalid weapon category: " << value.substr(0, separator) << '\n';
                return std::nullopt;
            }
            const auto smoothing = parseSmoothingValue(std::string_view(value).substr(separator + 1));
            if (!smoothing.has_value()) {
                std::cerr << "weapon smoothing must be a finite number greater than or equal to 1: " << value << '\n';
                return std::nullopt;
            }
            arguments.weaponSmoothing[static_cast<std::size_t>(*category)] = *smoothing;
            continue;
        }
        if ((argument != "--smoothing" && argument != "--vehicle-smoothing") || index + 1 >= argc) {
            std::cerr << "unknown or incomplete argument: " << argument << '\n';
            printUsage(std::cerr, argv[0]);
            return std::nullopt;
        }

        const std::string value = argv[++index];
        const auto parsedSmoothing = parseSmoothingValue(value);
        if (!parsedSmoothing.has_value()) {
            std::cerr << argument << " must be a finite number greater than or equal to 1\n";
            return std::nullopt;
        }
        if (argument == "--smoothing") arguments.smoothing = *parsedSmoothing;
        else arguments.vehicleSmoothing = *parsedSmoothing;
    }
    if (arguments.clearAim && arguments.readOnly) {
        std::cerr << "--clear-aim and --read-only cannot be used together\n";
        return std::nullopt;
    }
    if (arguments.clearAim && arguments.boneProbe) {
        std::cerr << "--clear-aim and --bone-probe cannot be used together\n";
        return std::nullopt;
    }
    if (arguments.rarity.has_value() && arguments.itemName.empty()) {
        std::cerr << "--rarity requires --item-name\n";
        return std::nullopt;
    }
    if (arguments.chams && arguments.boxes) {
        std::cerr << "--chams and --boxes are alternatives and cannot be combined\n";
        return std::nullopt;
    }
    if (arguments.readOnly && arguments.boxes) {
        std::cerr << "--boxes writes scanout VRAM and cannot be combined with --read-only\n";
        return std::nullopt;
    }
    if (arguments.debug && !arguments.boxes) {
        std::cerr << "--debug requires --boxes\n";
        return std::nullopt;
    }
    return arguments;
}

const char* boneSourceName(Unreal::BoneArraySource source) {
    if (source == Unreal::BoneArraySource::Primary) return "mesh+0x660";
    return "mesh+0x670";
}

bool validPrimaryModule(ProcessInstance<>& process, std::uint64_t imageBase) {
    ModuleInfo module{};
    if (process.primary_module(&module) != 0) return false;
    return module.base == imageBase && module.name != nullptr && module.name[0] != '\0';
}

bool validProcessDtb(OsInstance<>& os, Pid pid, std::uint64_t dtb, std::uint64_t imageBase) {
    ProcessInfo candidateInfo{};
    if (os.process_info_by_pid(pid, &candidateInfo) != 0) return false;
    candidateInfo.dtb1 = dtb;
    candidateInfo.dtb2 = Address_INVALID;

    ProcessInstance<> candidate;
    if (os.process_by_info(candidateInfo, &candidate) != 0) return false;
    return validPrimaryModule(candidate, imageBase);
}

std::optional<std::uint64_t> discoverImageBase(OsInstance<>& os, const ProcessInfo& processInfo, MemoryView& physicalMemory, std::uint64_t dtb, const Dtb::PhysicalRanges& ranges) {
    std::array<std::uint8_t, 0x400> eprocess{};
    if (os.read_raw_into(processInfo.address, CSliceMut<std::uint8_t>(eprocess)) != 0) return std::nullopt;

    const auto plausibleImageBase = [](std::uint64_t address) {
        return address >= 0x10000 && address <= 0x00007FFFFFFFFFFF && (address & 0xFFFF) == 0;
    };

    std::uint64_t sectionBase = 0;
    std::memcpy(&sectionBase, eprocess.data() + Config::EprocessSectionBaseOffset, sizeof(sectionBase));
    if (plausibleImageBase(sectionBase)) {
        std::cerr << "read image base 0x" << std::hex << sectionBase << " from EPROCESS+0x" << Config::EprocessSectionBaseOffset << std::dec << '\n';
        return sectionBase;
    }

    std::vector<std::pair<std::size_t, std::uint64_t>> candidates;
    for (std::size_t offset = 0; offset + sizeof(std::uint64_t) <= eprocess.size(); offset += sizeof(std::uint64_t)) {
        std::uint64_t candidate = 0;
        std::memcpy(&candidate, eprocess.data() + offset, sizeof(candidate));
        if (!plausibleImageBase(candidate)) continue;
        if (std::find_if(candidates.begin(), candidates.end(), [candidate](const auto& entry) { return entry.second == candidate; }) == candidates.end()) candidates.emplace_back(offset, candidate);
    }

    for (const auto& [offset, candidate] : candidates) {
        if (!Dtb::mapsPeImage(physicalMemory, dtb, candidate, ranges)) continue;
        std::cerr << "discovered image base 0x" << std::hex << candidate << " in EPROCESS+0x" << offset << " using the reported DTB" << std::dec << '\n';
        return candidate;
    }

    if (candidates.size() == 1) {
        std::cerr << "discovered sole aligned image-base candidate 0x" << std::hex << candidates.front().second << " in EPROCESS+0x" << candidates.front().first << std::dec << '\n';
        return candidates.front().second;
    }

    for (const auto& [offset, candidate] : candidates) {
        const auto recoveredDtbs = Dtb::recoverPeCandidates(physicalMemory, candidate, ranges);
        for (const std::uint64_t recoveredDtb : recoveredDtbs) {
            if (!validProcessDtb(os, processInfo.pid, recoveredDtb, candidate)) continue;
            std::cerr << "validated image base 0x" << std::hex << candidate << " in EPROCESS+0x" << offset << " with DTB 0x" << recoveredDtb << std::dec << '\n';
            return candidate;
        }
    }
    return std::nullopt;
}

struct DtbCacheRecord {
    std::uint64_t processAddress;
    std::uint32_t pid;
    std::uint64_t imageBase;
    std::uint64_t reportedDtb;
    std::uint64_t recoveredDtb;
};

std::filesystem::path dtbCachePath() {
    std::error_code error;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    return (error ? std::filesystem::path("/tmp") : directory) / "winshipping-dtb-v1.cache";
}

std::optional<DtbCacheRecord> loadDtbCache() {
    std::ifstream input(dtbCachePath());
    DtbCacheRecord record{};
    if (!(input >> std::hex >> record.processAddress >> record.pid >> record.imageBase >> record.reportedDtb >> record.recoveredDtb)) return std::nullopt;
    return record;
}

void saveDtbCache(const ProcessInfo& processInfo, std::uint64_t imageBase, std::uint64_t recoveredDtb) {
    std::ofstream output(dtbCachePath(), std::ios::trunc);
    if (!output) return;
    output << std::hex << processInfo.address << ' ' << static_cast<std::uint32_t>(processInfo.pid) << ' ' << imageBase << ' ' << processInfo.dtb1 << ' ' << recoveredDtb << '\n';
}

std::optional<std::uint64_t> configureDtb(OsInstance<>& os, const ProcessInfo& processInfo, MemoryView& physicalMemory, std::uint64_t imageBase, const Dtb::PhysicalRanges& ranges) {
    const Pid pid = processInfo.pid;
    const std::uint64_t reportedDtb = processInfo.dtb1;
    const std::uint64_t maskedDtb = Dtb::maskReported(reportedDtb);
    if (reportedDtb != maskedDtb) std::cerr << "masked protected DTB: 0x" << std::hex << reportedDtb << " -> 0x" << maskedDtb << std::dec << '\n';

    if (Dtb::mapsPeImage(physicalMemory, maskedDtb, imageBase, ranges) && validProcessDtb(os, pid, maskedDtb, imageBase)) return maskedDtb;

    const auto cached = loadDtbCache();
    if (cached.has_value() && cached->processAddress == processInfo.address && cached->pid == static_cast<std::uint32_t>(pid) && cached->imageBase == imageBase && cached->reportedDtb == reportedDtb) {
        if (Dtb::mapsPeImage(physicalMemory, cached->recoveredDtb, imageBase, ranges) && validProcessDtb(os, pid, cached->recoveredDtb, imageBase)) {
            std::cerr << "reused validated cached DTB 0x" << std::hex << cached->recoveredDtb << std::dec << '\n';
            return cached->recoveredDtb;
        }
    }

    std::cerr << "reported DTB did not produce a valid image and PEB; scanning physical PML4 candidates\n";
    const auto candidates = Dtb::recoverPeCandidates(physicalMemory, imageBase, ranges);
    std::cerr << "found " << candidates.size() << " PE-valid DTB candidates\n";
    for (const std::uint64_t candidate : candidates) {
        if (!validProcessDtb(os, pid, candidate, imageBase)) continue;
        saveDtbCache(processInfo, imageBase, candidate);
        return candidate;
    }
    return std::nullopt;
}

std::string renderFrame(const Unreal::ActorSnapshot& snapshot, const std::optional<Unreal::AimResult>& aim, const std::optional<Unreal::HighlightResult>& highlights, const std::optional<Unreal::GroundItemSnapshot>& groundItems, std::uint64_t imageBase, const Arguments& arguments, const std::optional<std::uint64_t>& currentVehicle, const std::optional<Unreal::WeaponSnapshot>& currentWeapon, const std::optional<std::size_t>& boxCount, double pawnSmoothing, bool rightButtonDown, bool llamaButtonDown, bool itemButtonDown) {
    std::ostringstream frame;
    frame << Config::TargetProcessName << " | base=0x" << std::hex << imageBase << " world=0x" << snapshot.world << " game_state=0x" << snapshot.gameState << std::dec;
    frame << " | players=" << snapshot.reportedCount << " positioned=" << snapshot.positions.size() << " raw=" << snapshot.rawCount << " bots_filtered=" << snapshot.filteredBotCount << '\n';
    frame << "unresolved: pawn=" << snapshot.missingPawnCount << " root=" << snapshot.missingRootCount << " location=" << snapshot.invalidLocationCount << '\n';
    frame << "chams=";
    if (!arguments.chams) frame << "disabled";
    else if (arguments.readOnly) frame << "disabled (read-only)";
    else if (!highlights.has_value()) frame << "context unavailable";
    else frame << "active=" << highlights->active << " eligible=" << highlights->eligible << " applied=" << highlights->applied << " restored=" << highlights->restored << " failed=" << highlights->failed;
    frame << '\n';
    frame << "boxes=";
    if (!arguments.boxes) frame << "disabled";
    else if (!boxCount.has_value()) frame << "renderer unavailable";
    else frame << "active=" << *boxCount << " green=visible red=occluded repaint="
               << arguments.boxRepaintHz << "Hz";
    frame << '\n';
    frame << "ground-item=";
    if (arguments.itemName.empty()) frame << "disabled";
    else if (!groundItems.has_value()) frame << "context unavailable query=\"" << arguments.itemName << '"';
    else frame << "query=\"" << arguments.itemName << '"';
    if (!arguments.itemName.empty() && arguments.rarity.has_value()) frame << " min-rarity=" << itemRarityName(*arguments.rarity);
    if (groundItems.has_value() && !arguments.itemName.empty()) frame << " matches=" << groundItems->positions.size() << " scanned=" << groundItems->scannedActors << " pending=" << groundItems->pendingActors;
    frame << '\n';
    frame << "vehicle=";
    if (!arguments.vehicleSmoothing.has_value()) frame << "detection disabled";
    else if (!currentVehicle.has_value()) frame << "unavailable";
    else if (*currentVehicle == 0) frame << "off";
    else frame << "on actor=0x" << std::hex << *currentVehicle << std::dec;
    if (arguments.vehicleSmoothing.has_value()) frame << " vehicle-smoothing=" << std::fixed << std::setprecision(2) << *arguments.vehicleSmoothing;
    frame << '\n';
    frame << "weapon=";
    if (!hasWeaponSmoothing(arguments)) frame << "detection disabled";
    else if (!currentWeapon.has_value()) frame << "unavailable";
    else {
        if (currentWeapon->actor == 0) frame << "unarmed";
        else frame << "actor=0x" << std::hex << currentWeapon->actor << " definition=0x" << currentWeapon->definition << std::dec;
        frame << " type=" << Unreal::weaponCategoryName(currentWeapon->category);
        if (!currentWeapon->name.empty()) frame << " name=\"" << currentWeapon->name << '"';
        const auto overrideSmoothing = configuredWeaponSmoothing(arguments, currentWeapon);
        if (overrideSmoothing.has_value()) frame << " weapon-smoothing=" << std::fixed << std::setprecision(2) << *overrideSmoothing;
    }
    frame << '\n';
    frame << "aim=";
    if (arguments.readOnly) frame << "disabled (read-only)";
    else if (!aim.has_value()) frame << "context unavailable";
    else if (aim->target == 0) frame << (itemButtonDown ? "no matching ground item" : (llamaButtonDown ? "no llama" : (aim->activationDown ? "no enemy pawn" : "idle (no enemy pawn)")));
    else {
        frame << (aim->activationDown ? (aim->wroteAngles ? "active" : "write failed") : "idle") << " local=0x" << std::hex << aim->localPawn << " target=0x" << aim->target << std::dec;
        if (aim->targetBone == -3) frame << " type=ground-item";
        else if (aim->targetBone == -2) frame << " type=llama";
        else frame << " teams=" << static_cast<unsigned int>(aim->localTeam) << "->" << static_cast<unsigned int>(aim->targetTeam) << " bone=" << aim->targetBone;
        frame << " distance=" << std::fixed << std::setprecision(1) << aim->targetDistanceMeters << "m fov_delta=" << std::setprecision(2) << aim->angularDistance;
        frame << " camera=(" << aim->cameraPitch << ',' << aim->cameraYaw << ") desired=(" << aim->desiredPitch << ',' << aim->desiredYaw << ") output=(" << aim->outputPitch << ',' << aim->outputYaw << ')';
    }
    frame << " activation=right-click:" << (rightButtonDown ? "down" : "up") << " llama-U:" << (llamaButtonDown ? "down" : "up") << " item-Y:" << (itemButtonDown ? "down" : "up") << " smoothing=" << std::fixed << std::setprecision(2) << ((llamaButtonDown || itemButtonDown) ? 1.0 : pawnSmoothing) << '\n';
    std::vector<const Unreal::ActorPosition*> positions;
    positions.reserve(snapshot.positions.size());
    for (const Unreal::ActorPosition& position : snapshot.positions) {
        if (position.actor != snapshot.localPawn) positions.push_back(&position);
    }
    if (snapshot.hasLocalPosition) {
        std::sort(positions.begin(), positions.end(), [&snapshot](const Unreal::ActorPosition* left, const Unreal::ActorPosition* right) {
            const double leftDistance = std::hypot(std::hypot(left->x - snapshot.localX, left->y - snapshot.localY), left->z - snapshot.localZ);
            const double rightDistance = std::hypot(std::hypot(right->x - snapshot.localX, right->y - snapshot.localY), right->z - snapshot.localZ);
            return leftDistance < rightDistance;
        });
    }
    const std::size_t displayedCount = arguments.printActors ? positions.size() : std::min<std::size_t>(5, positions.size());
    frame << std::left << std::setw(7) << "INDEX" << std::setw(19) << "PAWN" << std::right << std::setw(7) << "TEAM" << std::setw(12) << "DIST(m)" << std::setw(16) << "X" << std::setw(16) << "Y" << std::setw(16) << "Z" << '\n';

    for (std::size_t index = 0; index < displayedCount; ++index) {
        const auto& position = *positions[index];
        const double distanceMeters = snapshot.hasLocalPosition ? std::hypot(std::hypot(position.x - snapshot.localX, position.y - snapshot.localY), position.z - snapshot.localZ) / 100.0 : 0.0;
        frame << std::left << std::setw(7) << index << "0x" << std::right << std::hex << std::setw(16) << std::setfill('0') << position.actor << std::setfill(' ') << std::dec;
        frame << std::right << std::setw(7) << static_cast<unsigned int>(position.team) << std::fixed << std::setprecision(1) << std::setw(12) << distanceMeters;
        frame << std::setprecision(3) << std::setw(16) << position.x << std::setw(16) << position.y << std::setw(16) << position.z << '\n';
    }
    return frame.str();
}

struct BoxDebugCounters {
    std::uint64_t actorRefreshes{};
    std::uint64_t actorRefreshFailures{};
    std::uint64_t targetRebuilds{};
    std::uint64_t anchorRefreshes{};
    std::uint64_t anchorRefreshFailures{};
    std::uint64_t projectionFrames{};
    std::uint64_t cameraFailures{};
    std::uint64_t emptyFramesWithTargets{};
    std::uint64_t previousSubmitted{};
    std::uint64_t previousDropped{};
};

void printBoxDebug(const Unreal::ActorSnapshot& snapshot,
                   const Unreal::BoxProjectionDebug& projection,
                   BoxDebugCounters& counters,
                   const BoxOverlay& overlay) {
    const std::uint64_t submitted = overlay.submittedFrames();
    const std::uint64_t dropped = overlay.droppedFrames();
    std::cout << "\n[box-debug] actor_refresh=" << counters.actorRefreshes
              << " actor_fail=" << counters.actorRefreshFailures
              << " target_rebuild=" << counters.targetRebuilds
              << " anchor_refresh=" << counters.anchorRefreshes
              << " anchor_fail=" << counters.anchorRefreshFailures
              << " projection_frames=" << counters.projectionFrames
              << " camera_fail=" << counters.cameraFailures
              << " empty_with_targets=" << counters.emptyFramesWithTargets
              << " submitted=" << (submitted - counters.previousSubmitted)
              << " pipe_dropped=" << (dropped - counters.previousDropped) << '\n';
    std::cout << "[box-debug] snapshot raw=" << snapshot.rawCount
              << " reported=" << snapshot.reportedCount
              << " positioned=" << snapshot.positions.size()
              << " targets=" << projection.targetCount
              << " projected=" << projection.projectedCount
              << " world_seconds=" << std::fixed << std::setprecision(6)
              << snapshot.worldSeconds << '\n';
    std::cout << "[box-debug] camera=" << (projection.cameraValid ? "valid" : "INVALID")
              << " location=(" << std::setprecision(3)
              << projection.cameraX << ',' << projection.cameraY << ','
              << projection.cameraZ << ") rotation=("
              << projection.cameraPitch << ',' << projection.cameraYaw
              << ") fov=" << projection.cameraFov << '\n';

    for (const Unreal::PlayerBoxDebug& player : projection.players) {
        const auto position = std::find_if(
            snapshot.positions.begin(), snapshot.positions.end(),
            [&player](const Unreal::ActorPosition& candidate) {
                return candidate.actor == player.actor;
            });
        std::cout << "[box-player] actor=0x" << std::hex << player.actor
                  << " root=0x" << player.root << " mesh=0x" << player.mesh
                  << std::dec;
        if (position != snapshot.positions.end()) {
            const double renderAge = snapshot.worldSeconds -
                static_cast<double>(position->lastRenderTime);
            std::cout << " team=" << static_cast<unsigned int>(position->team)
                      << " raw=(" << std::fixed << std::setprecision(3)
                      << position->x << ',' << position->y << ',' << position->z
                      << ") render_age=" << std::setprecision(6) << renderAge;
        } else {
            std::cout << " raw=missing";
        }
        std::cout << " anchor=(" << std::fixed << std::setprecision(3)
                  << player.rootX << ',' << player.rootY << ',' << player.rootZ
                  << ") min_depth=" << player.minimumDepth
                  << " visible=" << (player.visible ? "yes" : "no");
        if (player.projected) {
            std::cout << " box=(" << player.box.x << ',' << player.box.y << ','
                      << player.box.width << ',' << player.box.height << ')';
        } else {
            std::cout << " rejected="
                      << Unreal::boxProjectionFailureName(player.failure);
        }
        std::cout << '\n';
    }
    std::cout << std::flush;

    counters.actorRefreshes = 0;
    counters.actorRefreshFailures = 0;
    counters.targetRebuilds = 0;
    counters.anchorRefreshes = 0;
    counters.anchorRefreshFailures = 0;
    counters.projectionFrames = 0;
    counters.cameraFailures = 0;
    counters.emptyFramesWithTargets = 0;
    counters.previousSubmitted = submitted;
    counters.previousDropped = dropped;
}

}

int main(int argc, char** argv) {
    const auto arguments = parseArguments(argc, argv);
    if (!arguments.has_value()) return argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") ? 0 : 2;

    if (Config::TargetProcessName.empty()) {
        std::cerr << "TargetProcessName is empty; set it in src/Config.hpp before running\n";
        return 2;
    }

    mf_log_init(LevelFilter::LevelFilter_Warn);
    Inventory* inventory = discoverMemflowPlugins();
    if (inventory == nullptr) {
        std::cerr << "could not initialize memflow plugin discovery\n";
        return 1;
    }

    ConnectorInstance<> connector;
    if (mf_inventory_create_connector(inventory, "qemu", Config::QemuTarget.data(), &connector) != 0) {
        std::cerr << "could not find or attach memflow-qemu to " << Config::QemuTarget << "; install the plugin in the standard memflow directory or set MEMFLOW_PLUGIN_PATH\n";
        mf_inventory_free(inventory);
        return 1;
    }

    ConnectorInstance<> scanner = connector.clone();
    const PhysicalMemoryMetadata metadata = scanner.metadata();
    const auto ranges = Dtb::physicalRanges(metadata);
    if (!ranges.has_value()) {
        std::cerr << "unsupported guest physical layout: max=0x" << std::hex << metadata.max_address << " real_size=0x" << metadata.real_size << std::dec << '\n';
        mf_inventory_free(inventory);
        return 1;
    }
    MemoryView physicalMemory = scanner.phys_view();

    OsInstance<> os;
    if (mf_inventory_create_os(inventory, "win32", "", &connector, &os) != 0) {
        std::cerr << "could not initialize memflow-win32\n";
        mf_inventory_free(inventory);
        return 1;
    }
    mf_inventory_free(inventory);

    std::optional<ProcessInfo> exactProcessInfo;
    std::vector<ProcessInfo> protectedNameMatches;
    const std::string_view targetName = Config::TargetProcessName;
    const std::size_t suffix = targetName.find("-Win64");
    const std::string_view protectedName = suffix == std::string_view::npos ? targetName : targetName.substr(0, suffix);
    os.process_info_list_callback([&](ProcessInfo info) {
        const std::string_view name(info.name);
        if (name == targetName) exactProcessInfo = info;
        else if (name == protectedName) protectedNameMatches.push_back(info);
        return true;
    });
    if (!exactProcessInfo.has_value() && protectedNameMatches.size() == 1) {
        exactProcessInfo = protectedNameMatches.front();
        std::cerr << "using unique protected EPROCESS name " << protectedName << " for " << targetName << '\n';
    }
    if (!exactProcessInfo.has_value()) {
        std::cerr << "Windows process " << Config::TargetProcessName << " was not found unambiguously (protected-name matches=" << protectedNameMatches.size() << ")\n";
        return 1;
    }
    const ProcessInfo processInfo = *exactProcessInfo;

    std::uint64_t imageBase = Config::ExpectedImageBase;
    const std::uint64_t maskedDtb = Dtb::maskReported(processInfo.dtb1);
    const auto discoveredBase = discoverImageBase(os, processInfo, physicalMemory, maskedDtb, *ranges);
    if (discoveredBase.has_value()) imageBase = *discoveredBase;

    const auto dtb = configureDtb(os, processInfo, physicalMemory, imageBase, *ranges);
    if (!dtb.has_value()) {
        std::cerr << "no PE-valid DTB candidate produced a valid target PEB module\n";
        return 1;
    }

    ProcessInfo correctedInfo{};
    if (os.process_info_by_pid(processInfo.pid, &correctedInfo) != 0) {
        std::cerr << "target process disappeared during DTB recovery\n";
        return 1;
    }
    correctedInfo.dtb1 = *dtb;
    correctedInfo.dtb2 = Address_INVALID;

    ProcessInstance<> process;
    if (os.process_by_info(correctedInfo, &process) != 0) {
        std::cerr << "could not create the corrected target process memory view\n";
        return 1;
    }

    std::cout << "attached to " << Config::TargetProcessName << " (PID " << correctedInfo.pid << ", base 0x" << std::hex << imageBase << ", DTB 0x" << *dtb << std::dec << ")\n";
    if (arguments->boneProbe) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto snapshot = Unreal::actorPositions(process, imageBase, arguments->ignoreIsABot);
            if (!snapshot.has_value()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            const auto bones = Unreal::probeBones(process, *snapshot);
            if (bones.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            for (const Unreal::BoneSnapshot& boneSnapshot : bones) {
                std::cout << "pawn=0x" << std::hex << boneSnapshot.actor << " mesh=0x" << boneSnapshot.mesh << " array=0x" << boneSnapshot.boneArray << std::dec;
                std::cout << " source=" << boneSourceName(boneSnapshot.source) << " count=" << boneSnapshot.reportedCount << " samples=" << boneSnapshot.positions.size();
                std::cout << " actor=(" << std::fixed << std::setprecision(3) << boneSnapshot.actorX << ", " << boneSnapshot.actorY << ", " << boneSnapshot.actorZ << ")\n";
                for (const Unreal::BonePosition& position : boneSnapshot.positions) {
                    std::cout << "  bone[" << position.index << "] = (" << std::fixed << std::setprecision(3) << position.x << ", " << position.y << ", " << position.z << ")\n";
                }
            }
            return 0;
        }
        std::cerr << "could not resolve a plausible bone array\n";
        return 1;
    }
    if (arguments->clearAim) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            const auto snapshot = Unreal::actorPositions(process, imageBase, arguments->ignoreIsABot);
            if (snapshot.has_value() && Unreal::clearAimOffsets(process, snapshot->world)) {
                std::cout << "cleared pending controller rotation input\n";
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::cerr << "could not resolve the local controller to clear rotation input\n";
        return 1;
    }
    KeyboardBase<> keyboard;
    if (os.keyboard(&keyboard) != 0) {
        std::cerr << "could not resolve the Windows guest keyboard/mouse-button state\n";
        return 1;
    }
    constexpr int RightMouseButton = 0x02;
    constexpr int LlamaAimKey = 0x55;
    constexpr int GroundItemAimKey = 0x59;
    BoxOverlay boxOverlay;
    std::optional<std::size_t> boxCount;
    std::signal(SIGPIPE, SIG_IGN);
    if (arguments->boxes && !boxOverlay.start(arguments->boxRepaintHz)) {
        std::cerr << "could not start the host BAR1 box renderer\n";
        return 1;
    }
    std::cout << "\x1B[2J\x1B[H";
    std::signal(SIGINT, requestStop);
    std::signal(SIGTERM, requestStop);
    const auto start = std::chrono::steady_clock::now();
    auto nextDisplay = start;
    auto nextActorRefresh = start;
    auto nextBoxAnchorRefresh = start;
    auto nextBoxRefresh = start;
    auto nextBoxDebug = start + std::chrono::seconds(1);
    auto nextHighlightRefresh = start;
    auto nextVehicleStateRefresh = start;
    auto nextWeaponStateRefresh = start;
    auto lastActorRefresh = start;
    std::uint64_t lastWorld = 0;
    enum class SpecialAimMode {
        None,
        Pawn,
        Llama,
        GroundItem
    };
    SpecialAimMode specialAimMode = SpecialAimMode::None;
    std::optional<Unreal::ActorSnapshot> snapshot;
    std::optional<Unreal::AimResult> aim;
    std::optional<Unreal::HighlightResult> highlights;
    std::optional<Unreal::GroundItemSnapshot> groundItems;
    std::optional<std::uint64_t> currentVehicle;
    std::optional<Unreal::WeaponSnapshot> currentWeapon;
    std::vector<Unreal::PlayerBoxTarget> boxTargets;
    Unreal::BoxProjectionDebug boxProjectionDebug;
    BoxDebugCounters boxDebugCounters;
    const bool weaponSmoothingConfigured = hasWeaponSmoothing(*arguments);
    while (!stopRequested) {
        KeyboardStateBase<> keyboardState;
        const bool hasKeyboardState = keyboard.state(&keyboardState) == 0;
        const bool rightButtonDown = hasKeyboardState ? keyboardState.is_down(RightMouseButton) : keyboard.is_down(RightMouseButton);
        const bool llamaButtonDown = hasKeyboardState ? keyboardState.is_down(LlamaAimKey) : keyboard.is_down(LlamaAimKey);
        const bool itemButtonDown = !arguments->itemName.empty() && (hasKeyboardState ? keyboardState.is_down(GroundItemAimKey) : keyboard.is_down(GroundItemAimKey));
        const auto now = std::chrono::steady_clock::now();
        double pawnSmoothing = effectivePawnSmoothing(*arguments, currentVehicle, currentWeapon);
        bool actorRefreshed = false;
        if (!snapshot.has_value() || now >= nextActorRefresh) {
            if (arguments->debug) ++boxDebugCounters.actorRefreshes;
            const auto refreshed = Unreal::actorPositions(process, imageBase, arguments->ignoreIsABot);
            if (refreshed.has_value()) {
                if (snapshot.has_value() && lastWorld != 0 && refreshed->world != lastWorld) {
                    if (specialAimMode == SpecialAimMode::Pawn) Unreal::aimAtNearestPawn(process, *snapshot, pawnSmoothing, false, arguments->ignoreTeams);
                    else if (specialAimMode == SpecialAimMode::Llama) Unreal::aimAtNearestLlama(process, *snapshot, false);
                    else if (specialAimMode == SpecialAimMode::GroundItem) Unreal::aimAtNearestGroundItem(process, *snapshot, groundItems.value_or(Unreal::GroundItemSnapshot{}), false);
                    groundItems.reset();
                    highlights.reset();
                    currentVehicle.reset();
                    currentWeapon.reset();
                    nextVehicleStateRefresh = now;
                    nextWeaponStateRefresh = now;
                    specialAimMode = SpecialAimMode::None;
                }
                snapshot = std::move(*refreshed);
                lastWorld = snapshot->world;
                lastActorRefresh = now;
                actorRefreshed = true;
            } else {
                if (arguments->debug) ++boxDebugCounters.actorRefreshFailures;
                if (snapshot.has_value() && now - lastActorRefresh >= Config::ActorSnapshotExpiry) {
                if (specialAimMode == SpecialAimMode::Pawn) Unreal::aimAtNearestPawn(process, *snapshot, pawnSmoothing, false, arguments->ignoreTeams);
                else if (specialAimMode == SpecialAimMode::Llama) Unreal::aimAtNearestLlama(process, *snapshot, false);
                else if (specialAimMode == SpecialAimMode::GroundItem) Unreal::aimAtNearestGroundItem(process, *snapshot, groundItems.value_or(Unreal::GroundItemSnapshot{}), false);
                snapshot.reset();
                aim.reset();
                currentVehicle.reset();
                currentWeapon.reset();
                nextVehicleStateRefresh = now;
                nextWeaponStateRefresh = now;
                specialAimMode = SpecialAimMode::None;
                }
            }
            nextActorRefresh = now + Config::ActorRefreshInterval;
        }

        if (arguments->boxes && actorRefreshed && snapshot.has_value()) {
            boxTargets = Unreal::playerBoxTargets(
                process, *snapshot, boxTargets, arguments->ignoreTeams);
            if (arguments->debug) ++boxDebugCounters.targetRebuilds;
            nextBoxAnchorRefresh = now + Config::BoxAnchorRefreshInterval;
        } else if (arguments->boxes && snapshot.has_value() &&
                   now >= nextBoxAnchorRefresh) {
            const bool refreshed = Unreal::refreshPlayerBoxTargets(
                process, boxTargets, snapshot->worldSeconds);
            if (arguments->debug) {
                ++boxDebugCounters.anchorRefreshes;
                if (!refreshed) ++boxDebugCounters.anchorRefreshFailures;
            }
            nextBoxAnchorRefresh = now + Config::BoxAnchorRefreshInterval;
        }
        if (arguments->boxes && snapshot.has_value() && now >= nextBoxRefresh) {
            const auto boxes = Unreal::projectPlayerBoxes(
                process, *snapshot, boxTargets, Config::BoxScreenWidth,
                Config::BoxScreenHeight,
                arguments->debug ? &boxProjectionDebug : nullptr);
            if (arguments->debug) {
                ++boxDebugCounters.projectionFrames;
                if (!boxProjectionDebug.cameraValid)
                    ++boxDebugCounters.cameraFailures;
                if (!boxTargets.empty() && boxes.empty())
                    ++boxDebugCounters.emptyFramesWithTargets;
            }
            if (!boxOverlay.submit(boxes)) {
                std::cerr << "\nBAR1 box renderer stopped unexpectedly\n";
                stopRequested = 1;
            } else boxCount = boxes.size();
            nextBoxRefresh = now + Config::BoxRefreshInterval;
        } else if (arguments->boxes && !snapshot.has_value() && boxCount.value_or(0) != 0) {
            boxTargets.clear();
            if (boxOverlay.submit({})) boxCount = 0;
            else stopRequested = 1;
        }

        if (snapshot.has_value()) {
            if (arguments->vehicleSmoothing.has_value() && now >= nextVehicleStateRefresh) {
                currentVehicle = Unreal::currentVehicle(process, *snapshot);
                nextVehicleStateRefresh = now + Config::VehicleStateRefreshInterval;
            }
            if (weaponSmoothingConfigured && now >= nextWeaponStateRefresh) {
                currentWeapon = Unreal::currentWeapon(process, *snapshot);
                nextWeaponStateRefresh = now + Config::WeaponStateRefreshInterval;
            }
            pawnSmoothing = effectivePawnSmoothing(*arguments, currentVehicle, currentWeapon);
            if (now >= nextHighlightRefresh) {
                highlights = Unreal::updatePlayerHighlights(process, *snapshot, arguments->chams && !arguments->readOnly, arguments->ignoreTeams);
                nextHighlightRefresh = now + Config::HighlightRefreshInterval;
            }
            if (actorRefreshed && !arguments->itemName.empty()) groundItems = Unreal::findGroundItems(process, *snapshot, arguments->itemName, arguments->rarity);

            SpecialAimMode requestedMode = SpecialAimMode::None;
            if (!arguments->readOnly && itemButtonDown && groundItems.has_value()) requestedMode = SpecialAimMode::GroundItem;
            else if (!arguments->readOnly && llamaButtonDown) requestedMode = SpecialAimMode::Llama;
            else if (!arguments->readOnly && rightButtonDown) requestedMode = SpecialAimMode::Pawn;

            if (requestedMode != specialAimMode) {
                if (specialAimMode == SpecialAimMode::Pawn) aim = Unreal::aimAtNearestPawn(process, *snapshot, pawnSmoothing, false, arguments->ignoreTeams);
                else if (specialAimMode == SpecialAimMode::Llama) aim = Unreal::aimAtNearestLlama(process, *snapshot, false);
                else if (specialAimMode == SpecialAimMode::GroundItem) aim = Unreal::aimAtNearestGroundItem(process, *snapshot, groundItems.value_or(Unreal::GroundItemSnapshot{}), false);
                specialAimMode = requestedMode;
            }

            if (specialAimMode == SpecialAimMode::GroundItem) aim = Unreal::aimAtNearestGroundItem(process, *snapshot, *groundItems, true);
            else if (specialAimMode == SpecialAimMode::Llama) aim = Unreal::aimAtNearestLlama(process, *snapshot, true);
            else if (specialAimMode == SpecialAimMode::Pawn) aim = Unreal::aimAtNearestPawn(process, *snapshot, pawnSmoothing, true, arguments->ignoreTeams);
            else if (!aim.has_value() || aim->activationDown) {
                aim = Unreal::AimResult{false, true, snapshot->localPawn, 0, snapshot->localTeam, 0xFF, -1, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
            }
        }

        if (arguments->debug && snapshot.has_value() && now >= nextBoxDebug) {
            printBoxDebug(*snapshot, boxProjectionDebug, boxDebugCounters,
                          boxOverlay);
            nextBoxDebug = now + std::chrono::seconds(1);
        }

        if (!arguments->debug && now >= nextDisplay && snapshot.has_value()) {
            std::cout << "\r\x1B[H" << renderFrame(*snapshot, aim, highlights, groundItems, imageBase, *arguments, currentVehicle, currentWeapon, boxCount, pawnSmoothing, rightButtonDown, llamaButtonDown, itemButtonDown) << "\x1B[J" << std::flush;
            nextDisplay = now + Config::DisplayInterval;
        } else if (!arguments->debug && now >= nextDisplay) {
            std::cout << "\r\x1B[H" << Config::TargetProcessName << " | actor snapshot unavailable\n\x1B[J" << std::flush;
            nextDisplay = now + Config::DisplayInterval;
        }
        auto loopInterval = std::chrono::duration_cast<std::chrono::microseconds>(
            specialAimMode == SpecialAimMode::None ? Config::IdleLoopInterval : Config::ActiveLoopInterval);
        if (arguments->boxes) loopInterval = std::min(loopInterval, Config::BoxRefreshInterval);
        std::this_thread::sleep_for(loopInterval);
    }
    if (!arguments->readOnly && lastWorld != 0) Unreal::clearAimOffsets(process, lastWorld);
    if (!arguments->readOnly && arguments->chams) {
        const Unreal::HighlightResult restored = Unreal::restorePlayerHighlights(process);
        std::cout << "\nrestored player highlights=" << restored.restored << " failed=" << restored.failed << '\n';
    }
    boxOverlay.stop();
    std::cout << '\n';
    return 0;
}
