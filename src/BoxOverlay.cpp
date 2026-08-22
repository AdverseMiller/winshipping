#include "BoxOverlay.hpp"

#include "Config.hpp"
#include "box_stream_protocol.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {

constexpr const char* DefaultLauncher = "/home/andrew/programs/vm/vgk/tools/tu104-bar1-overlay/render-current-pool.sh";
// NVIDIA format 0xd1 is A2B10G10R10: R occupies bits 0..9, G 10..19,
// B 20..29, and the two alpha bits are 30..31.
constexpr std::uint32_t VisibleColor = 0xC00FFC00U;
constexpr std::uint32_t OccludedColor = 0xC00003FFU;
constexpr std::uint32_t ContrastColor = 0xC0000000U;
constexpr std::uint16_t ContrastMargin = 2;

}

BoxOverlay::~BoxOverlay() {
    stop();
}

bool BoxOverlay::start(std::uint32_t repaintHz) {
    if (running()) return true;
    const std::string repaintHzText = std::to_string(repaintHz);
    int pipes[2]{};
    if (pipe2(pipes, O_CLOEXEC) != 0) {
        std::cerr << "could not create BAR1 box pipe: " << std::strerror(errno) << '\n';
        return false;
    }
    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "could not fork BAR1 renderer: " << std::strerror(errno) << '\n';
        close(pipes[0]);
        close(pipes[1]);
        return false;
    }
    if (child == 0) {
        close(pipes[1]);
        if (dup2(pipes[0], STDIN_FILENO) < 0) _exit(126);
        close(pipes[0]);
        setenv("BAR1_BOX_STREAM", "1", 1);
        setenv("BAR1_HZ", repaintHzText.c_str(), 1);
        const char* configured = std::getenv("WINSHIPPING_BOX_RENDERER");
        const char* launcher = configured != nullptr && configured[0] != '\0' ? configured : DefaultLauncher;
        execl(launcher, launcher, static_cast<char*>(nullptr));
        _exit(127);
    }
    close(pipes[0]);
    const int flags = fcntl(pipes[1], F_GETFL);
    if (flags < 0 || fcntl(pipes[1], F_SETFL, flags | O_NONBLOCK) < 0) {
        std::cerr << "could not configure BAR1 box pipe: " << std::strerror(errno) << '\n';
        close(pipes[1]);
        kill(child, SIGTERM);
        waitpid(child, nullptr, 0);
        return false;
    }
    writeFd_ = pipes[1];
    childPid_ = child;
    sequence_ = 0;
    droppedFrames_ = 0;
    return true;
}

bool BoxOverlay::running() {
    if (childPid_ < 0 || writeFd_ < 0) return false;
    int status = 0;
    const pid_t result = waitpid(childPid_, &status, WNOHANG);
    if (result == 0) return true;
    close(writeFd_);
    writeFd_ = -1;
    childPid_ = -1;
    return false;
}

bool BoxOverlay::submit(const std::vector<Unreal::PlayerBox>& boxes) {
    if (!running()) return false;
    tu104_box_frame frame{};
    frame.magic = TU104_BOX_STREAM_MAGIC;
    frame.version = TU104_BOX_STREAM_VERSION;
    const std::size_t targetCount = std::min<std::size_t>(
        boxes.size(), TU104_BOX_STREAM_MAX_BOXES / 2);
    if (boxes.size() > targetCount) {
        static bool warned = false;
        if (!warned) {
            std::cerr << "BAR1 box stream capacity exceeded: submitting "
                      << targetCount << " of " << boxes.size() << " players\n";
            warned = true;
        }
    }
    frame.count = static_cast<std::uint16_t>(targetCount * 2);
    frame.sequence = ++sequence_;
    for (std::size_t index = 0; index < targetCount; ++index) {
        const Unreal::PlayerBox& box = boxes[index];
        const std::uint16_t shadowX = box.x > ContrastMargin
            ? static_cast<std::uint16_t>(box.x - ContrastMargin) : 0;
        const std::uint16_t shadowY = box.y > ContrastMargin
            ? static_cast<std::uint16_t>(box.y - ContrastMargin) : 0;
        const std::uint32_t shadowRight = std::min<std::uint32_t>(
            Config::BoxScreenWidth,
            static_cast<std::uint32_t>(box.x) + box.width + ContrastMargin);
        const std::uint32_t shadowBottom = std::min<std::uint32_t>(
            Config::BoxScreenHeight,
            static_cast<std::uint32_t>(box.y) + box.height + ContrastMargin);
        frame.boxes[index * 2] = tu104_box{
            shadowX,
            shadowY,
            static_cast<std::uint16_t>(shadowRight - shadowX),
            static_cast<std::uint16_t>(shadowBottom - shadowY),
            ContrastColor
        };
        frame.boxes[index * 2 + 1] = tu104_box{
            box.x,
            box.y,
            box.width,
            box.height,
            box.visible ? VisibleColor : OccludedColor
        };
    }
    for (;;) {
        const ssize_t result = write(writeFd_, &frame, sizeof(frame));
        if (result == static_cast<ssize_t>(sizeof(frame))) return true;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            ++droppedFrames_;
            return true;
        }
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && errno == EPIPE) {
            running();
            return false;
        }
        std::cerr << "BAR1 box-frame write failed: "
                  << (result < 0 ? std::strerror(errno) : "partial pipe write") << '\n';
        return false;
    }
}

void BoxOverlay::stop() {
    if (writeFd_ >= 0) {
        close(writeFd_);
        writeFd_ = -1;
    }
    if (childPid_ < 0) return;
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (waitpid(childPid_, nullptr, WNOHANG) == childPid_) {
            childPid_ = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(childPid_, SIGTERM);
    waitpid(childPid_, nullptr, 0);
    childPid_ = -1;
}
