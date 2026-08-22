#pragma once

#include "Unreal.hpp"

#include <cstdint>
#include <vector>

class BoxOverlay {
public:
    BoxOverlay() = default;
    ~BoxOverlay();
    BoxOverlay(const BoxOverlay&) = delete;
    BoxOverlay& operator=(const BoxOverlay&) = delete;

    bool start(std::uint32_t repaintHz);
    bool submit(const std::vector<Unreal::PlayerBox>& boxes);
    void stop();
    bool running();
    std::uint64_t submittedFrames() const { return sequence_; }
    std::uint64_t droppedFrames() const { return droppedFrames_; }

private:
    int writeFd_{-1};
    int childPid_{-1};
    std::uint64_t sequence_{};
    std::uint64_t droppedFrames_{};
};
