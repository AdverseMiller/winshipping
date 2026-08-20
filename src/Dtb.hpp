#pragma once

#include "memflow.hpp"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Dtb {

using PhysicalRanges = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

std::uint64_t maskReported(std::uint64_t reportedDtb);
std::optional<PhysicalRanges> physicalRanges(const PhysicalMemoryMetadata& metadata);
bool mapsPeImage(MemoryView& memory, std::uint64_t dtb, std::uint64_t imageBase, const PhysicalRanges& ranges);
std::vector<std::uint64_t> recoverPeCandidates(MemoryView& memory, std::uint64_t imageBase, const PhysicalRanges& ranges);

}
