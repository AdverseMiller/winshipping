#include "Dtb.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace {

constexpr std::uint64_t PageSize = 0x1000;
constexpr std::uint64_t EntryAddressMask = 0x000FFFFFFFFFF000;
constexpr std::uint64_t PhysicalAddressMask = (1ULL << 52) - 1;
constexpr std::uint64_t Present = 1;
constexpr std::uint64_t LargePage = 1ULL << 7;
constexpr std::uint64_t Q35LowRamEnd = 0x80000000;
constexpr std::uint64_t Q35HighRamStart = 0x100000000;
constexpr std::size_t BatchPages = 16384;

bool addressInRanges(std::uint64_t address, const Dtb::PhysicalRanges& ranges) {
    return std::any_of(ranges.begin(), ranges.end(), [address](const auto& range) {
        return address >= range.first && address < range.second;
    });
}

bool plausibleEntry(std::uint64_t entry, const Dtb::PhysicalRanges& ranges) {
    return (entry & Present) != 0 && addressInRanges(entry & EntryAddressMask, ranges);
}

template <typename T>
bool readPhysical(MemoryView& memory, std::uint64_t address, T& value) {
    std::array<std::uint8_t, sizeof(T)> bytes{};
    if (memory.read_raw_into(address, CSliceMut<std::uint8_t>(bytes)) != 0) return false;
    std::memcpy(&value, bytes.data(), sizeof(T));
    return true;
}

std::optional<std::uint64_t> translateX64(MemoryView& memory, std::uint64_t dtb, std::uint64_t virtualAddress, const Dtb::PhysicalRanges& ranges) {
    const std::array<std::uint64_t, 4> indexes{
        (virtualAddress >> 39) & 0x1FF,
        (virtualAddress >> 30) & 0x1FF,
        (virtualAddress >> 21) & 0x1FF,
        (virtualAddress >> 12) & 0x1FF,
    };

    std::uint64_t pml4e = 0;
    if (!readPhysical(memory, (dtb & EntryAddressMask) + indexes[0] * 8, pml4e) || !plausibleEntry(pml4e, ranges)) return std::nullopt;

    std::uint64_t pdpte = 0;
    if (!readPhysical(memory, (pml4e & EntryAddressMask) + indexes[1] * 8, pdpte) || !plausibleEntry(pdpte, ranges)) return std::nullopt;
    if ((pdpte & LargePage) != 0) return (pdpte & 0x000FFFFFC0000000) + (virtualAddress & 0x3FFFFFFF);

    std::uint64_t pde = 0;
    if (!readPhysical(memory, (pdpte & EntryAddressMask) + indexes[2] * 8, pde) || !plausibleEntry(pde, ranges)) return std::nullopt;
    if ((pde & LargePage) != 0) return (pde & 0x000FFFFFFFE00000) + (virtualAddress & 0x1FFFFF);

    std::uint64_t pte = 0;
    if (!readPhysical(memory, (pde & EntryAddressMask) + indexes[3] * 8, pte) || !plausibleEntry(pte, ranges)) return std::nullopt;
    return (pte & EntryAddressMask) + (virtualAddress & 0xFFF);
}

bool isPeImage(MemoryView& memory, std::uint64_t physicalAddress) {
    const std::uint64_t pageBase = physicalAddress & ~(PageSize - 1);
    const std::size_t pageOffset = static_cast<std::size_t>(physicalAddress & (PageSize - 1));
    std::array<std::uint8_t, PageSize> page{};
    if (memory.read_raw_into(pageBase, CSliceMut<std::uint8_t>(page)) != 0) return false;
    if (pageOffset + 0x40 > page.size() || page[pageOffset] != 'M' || page[pageOffset + 1] != 'Z') return false;

    std::uint32_t lfanew = 0;
    std::memcpy(&lfanew, page.data() + pageOffset + 0x3C, sizeof(lfanew));
    const std::size_t signature = pageOffset + lfanew;
    return signature + 4 <= page.size() && page[signature] == 'P' && page[signature + 1] == 'E' && page[signature + 2] == 0 && page[signature + 3] == 0;
}

std::vector<std::uint64_t> pml4Candidates(MemoryView& memory, std::uint64_t virtualAddress, const Dtb::PhysicalRanges& ranges) {
    const std::uint64_t pml4Index = (virtualAddress >> 39) & 0x1FF;
    std::vector<std::uint64_t> candidates;

    for (const auto& range : ranges) {
        std::uint64_t batchStart = range.first;
        while (batchStart < range.second) {
            const std::uint64_t remainingPages = (range.second - batchStart) / PageSize;
            const std::size_t pageCount = static_cast<std::size_t>(std::min<std::uint64_t>(remainingPages, BatchPages));
            if (pageCount == 0) break;

            std::vector<std::uint64_t> entries(pageCount);
            std::vector<ReadData> reads;
            reads.reserve(pageCount);
            for (std::size_t index = 0; index < pageCount; ++index) {
                const std::uint64_t address = batchStart + index * PageSize + pml4Index * 8;
                auto* bytes = reinterpret_cast<std::uint8_t*>(&entries[index]);
                reads.push_back(ReadData{address, CSliceMut<std::uint8_t>(reinterpret_cast<char*>(bytes), sizeof(entries[index]))});
            }

            if (memory.read_raw_list(CSliceMut<ReadData>(reads)) == 0) {
                for (std::size_t index = 0; index < entries.size(); ++index) {
                    if (plausibleEntry(entries[index], ranges)) candidates.push_back(batchStart + index * PageSize);
                }
            }
            batchStart += pageCount * PageSize;
        }
    }
    return candidates;
}

}

namespace Dtb {

std::uint64_t maskReported(std::uint64_t reportedDtb) {
    return reportedDtb & PhysicalAddressMask;
}

std::optional<PhysicalRanges> physicalRanges(const PhysicalMemoryMetadata& metadata) {
    if (metadata.max_address == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
    const std::uint64_t end = metadata.max_address + 1;
    if (end == metadata.real_size) return PhysicalRanges{{0, end}};
    if (end >= Q35HighRamStart && metadata.real_size >= Q35LowRamEnd && end - Q35HighRamStart + Q35LowRamEnd == metadata.real_size) {
        return PhysicalRanges{{0, Q35LowRamEnd}, {Q35HighRamStart, end}};
    }
    return std::nullopt;
}

bool mapsPeImage(MemoryView& memory, std::uint64_t dtb, std::uint64_t imageBase, const PhysicalRanges& ranges) {
    const auto physical = translateX64(memory, dtb, imageBase, ranges);
    return physical.has_value() && isPeImage(memory, *physical);
}

std::vector<std::uint64_t> recoverPeCandidates(MemoryView& memory, std::uint64_t imageBase, const PhysicalRanges& ranges) {
    std::vector<std::uint64_t> recovered;
    for (const std::uint64_t candidate : pml4Candidates(memory, imageBase, ranges)) {
        if (mapsPeImage(memory, candidate, imageBase, ranges)) recovered.push_back(candidate);
    }
    std::sort(recovered.begin(), recovered.end());
    recovered.erase(std::unique(recovered.begin(), recovered.end()), recovered.end());
    return recovered;
}

}
