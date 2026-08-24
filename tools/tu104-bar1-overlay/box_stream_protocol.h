#pragma once

#include <stdint.h>

#define TU104_BOX_STREAM_MAGIC UINT32_C(0x31425854)
#define TU104_BOX_STREAM_VERSION UINT16_C(2)
// Each logical player box is represented by a contrast outline and a colored
// outline. 256 entries therefore cover 128 players while keeping the complete
// 3088-byte frame below Linux PIPE_BUF (4096), preserving atomic nonblocking
// writes between the producer and renderer.
#define TU104_BOX_STREAM_MAX_BOXES 256
#define TU104_BOX_STREAM_ATOMIC_LIMIT 4096

struct tu104_box {
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
    uint32_t color;
};

struct tu104_box_frame {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint64_t sequence;
    struct tu104_box boxes[TU104_BOX_STREAM_MAX_BOXES];
};

#if defined(__cplusplus)
static_assert(sizeof(tu104_box) == 12);
static_assert(sizeof(tu104_box_frame) == 3088);
static_assert(sizeof(tu104_box_frame) <= TU104_BOX_STREAM_ATOMIC_LIMIT);
#else
_Static_assert(sizeof(struct tu104_box) == 12, "box protocol ABI");
_Static_assert(sizeof(struct tu104_box_frame) == 3088, "frame protocol ABI");
_Static_assert(sizeof(struct tu104_box_frame) <= TU104_BOX_STREAM_ATOMIC_LIMIT,
               "box stream frame must remain an atomic pipe write");
#endif
