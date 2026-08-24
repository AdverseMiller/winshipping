#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <immintrin.h>
#include <inttypes.h>
#include <linux/vfio.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "box_stream_protocol.h"

enum {
    BAR1_MAP_SIZE = 0x10000000,
    PAGE_2M = 0x200000,
    MAX_SURFACES = 16,
};

static volatile sig_atomic_t stop_requested;

struct hardware_profile {
    uint32_t pci_device_id;
    uint32_t chipset;
    uint64_t pbus_bar0_window;
    uint64_t pramin_offset;
    uint64_t pramin_size;
    uint64_t bar1_block;
    uint64_t bar1_bind_status;
    uint64_t mmu_invalidate_pdb;
    uint64_t mmu_invalidate_upper_pdb;
    uint64_t mmu_invalidate;
    uint64_t instance_pdb_offset;
    uint64_t instance_limit_offset;
    unsigned page_table_levels;
};

struct device {
    int fd;
    int wc_fd;
    volatile uint8_t *bar0;
    volatile uint8_t *bar1;
    bool bar1_wc;
    uint64_t bar0_size;
    uint64_t bar1_size;
    uint64_t bar0_offset;
    uint64_t bar1_offset;
    struct hardware_profile hw;
    uint32_t saved_pramin;
    uint32_t current_pramin;
};

static struct device *active_device;

struct map_state_header {
    char magic[8];
    uint32_t version;
    uint32_t count;
    uint64_t instance;
    uint64_t root;
    uint64_t pd0;
    uint64_t bar1_va;
    uint64_t vram_base;
    uint64_t span;
};

struct map_state_entry {
    uint64_t lo;
    uint64_t hi;
};

struct sparse_state_header {
    char magic[8];
    uint32_t version;
    uint32_t page_count;
    uint32_t surface_count;
    uint32_t reserved;
    uint64_t instance;
    uint64_t root;
    uint64_t pd0;
    uint64_t surface_size;
};

struct sparse_state_entry {
    uint32_t pde_index;
    uint32_t reserved;
    uint64_t vram_page;
    uint64_t old_lo;
    uint64_t old_hi;
};

struct sparse_state {
    struct sparse_state_header header;
    struct sparse_state_entry *entries;
    uint64_t *surfaces;
};

struct options {
    pid_t pid;
    int vfio_fd;
    const char *wc_path;
    const char *state_path;
    uint64_t bar1_va;
    uint64_t vram_base;
    uint64_t span;
    uint64_t surfaces[MAX_SURFACES];
    size_t surface_count;
    uint64_t surface_size;
    unsigned frame_width;
    unsigned frame_height;
    unsigned pitch_blocks;
    unsigned log2_gobs_y;
    unsigned x, y, width, height;
    uint32_t color;
    unsigned hz;
    unsigned duration_ms;
    int cpu;
    int rt_priority;
    int rect_stream_fd;
    struct hardware_profile hw;
};

struct segment {
    uint32_t offset;
    uint32_t length;
};

struct colored_segment {
    uint32_t offset;
    uint32_t length;
    uint32_t color;
};

struct colored_pixel {
    uint32_t offset;
    uint32_t color;
};

static void die(const char *message)
{
    perror(message);
    exit(EXIT_FAILURE);
}

static uint64_t parse_u64(const char *text)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 0);
    if (errno || !end || *end)
        die("invalid integer");
    return (uint64_t)value;
}

static uint32_t mmio_read32(volatile uint8_t *base, size_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static void mmio_write32(volatile uint8_t *base, size_t offset, uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static int duplicate_fd(pid_t pid, int fd_number)
{
    int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0)
        die("pidfd_open");
    int fd = (int)syscall(SYS_pidfd_getfd, pidfd, fd_number, 0);
    int saved = errno;
    close(pidfd);
    errno = saved;
    if (fd < 0)
        die("pidfd_getfd");
    return fd;
}

static struct vfio_region_info vfio_region(int fd, uint32_t index)
{
    struct vfio_region_info region = {
        .argsz = sizeof(region),
        .index = index,
    };
    if (ioctl(fd, VFIO_DEVICE_GET_REGION_INFO, &region) < 0)
        die("VFIO_DEVICE_GET_REGION_INFO");
    if (region.argsz < sizeof(region) || region.index != index || !region.size)
        die("invalid VFIO region metadata");
    return region;
}

static void validate_hardware_profile(const struct hardware_profile *hw,
                                      uint64_t bar0_size)
{
    const uint64_t registers[] = {
        hw->pbus_bar0_window, hw->bar1_block, hw->bar1_bind_status,
        hw->mmu_invalidate_pdb, hw->mmu_invalidate_upper_pdb,
        hw->mmu_invalidate,
    };
    if (!hw->pramin_size || (hw->pramin_size & (hw->pramin_size - 1)) ||
        hw->pramin_offset > bar0_size || hw->pramin_size > bar0_size - hw->pramin_offset ||
        !hw->page_table_levels || hw->page_table_levels > 5 ||
        hw->instance_pdb_offset > UINT32_MAX ||
        hw->instance_limit_offset > UINT32_MAX) {
        fprintf(stderr, "invalid hardware profile geometry\n");
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < sizeof(registers) / sizeof(registers[0]); ++i)
        if (registers[i] > bar0_size || 4 > bar0_size - registers[i]) {
            fprintf(stderr, "hardware-profile register lies outside BAR0\n");
            exit(EXIT_FAILURE);
        }
}

static void device_open(struct device *dev, const struct options *opt, bool need_bar1)
{
    memset(dev, 0, sizeof(*dev));
    dev->fd = duplicate_fd(opt->pid, opt->vfio_fd);
    dev->wc_fd = -1;
    const struct vfio_region_info bar0 = vfio_region(dev->fd, VFIO_PCI_BAR0_REGION_INDEX);
    const struct vfio_region_info bar1 = vfio_region(dev->fd, VFIO_PCI_BAR1_REGION_INDEX);
    const struct vfio_region_info config = vfio_region(dev->fd, VFIO_PCI_CONFIG_REGION_INDEX);
    const uint32_t required = VFIO_REGION_INFO_FLAG_READ |
                              VFIO_REGION_INFO_FLAG_WRITE |
                              VFIO_REGION_INFO_FLAG_MMAP;
    if ((bar0.flags & required) != required || (bar1.flags & required) != required ||
        bar0.size > SIZE_MAX || bar1.size < BAR1_MAP_SIZE) {
        fprintf(stderr, "VFIO BAR geometry is unsupported\n");
        exit(EXIT_FAILURE);
    }
    uint8_t pci_config[12];
    if (pread(dev->fd, pci_config, sizeof(pci_config), (off_t)config.offset) !=
        (ssize_t)sizeof(pci_config))
        die("read VFIO PCI config");
    uint16_t vendor, device_id;
    memcpy(&vendor, pci_config, sizeof(vendor));
    memcpy(&device_id, pci_config + 2, sizeof(device_id));
    const uint32_t class_code = (uint32_t)pci_config[9] |
                                (uint32_t)pci_config[10] << 8 |
                                (uint32_t)pci_config[11] << 16;
    if (vendor != 0x10de || class_code >> 16 != 0x03 ||
        device_id != opt->hw.pci_device_id) {
        fprintf(stderr, "VFIO descriptor is not an NVIDIA display function\n");
        exit(EXIT_FAILURE);
    }
    dev->bar0_size = bar0.size;
    dev->bar1_size = bar1.size;
    dev->bar0_offset = bar0.offset;
    dev->bar1_offset = bar1.offset;
    dev->hw = opt->hw;
    validate_hardware_profile(&dev->hw, dev->bar0_size);
    dev->bar0 = mmap(NULL, (size_t)dev->bar0_size, PROT_READ | PROT_WRITE,
                     MAP_SHARED, dev->fd, (off_t)dev->bar0_offset);
    if (dev->bar0 == MAP_FAILED)
        die("mmap VFIO BAR0");
    const uint32_t boot0 = mmio_read32(dev->bar0, 0);
    if (((boot0 >> 20) & 0x1ffu) != dev->hw.chipset) {
        fprintf(stderr, "hardware descriptor chipset differs from PMC_BOOT_0\n");
        exit(EXIT_FAILURE);
    }
    dev->saved_pramin = mmio_read32(dev->bar0, (size_t)dev->hw.pbus_bar0_window);
    dev->current_pramin = dev->saved_pramin;
    active_device = dev;

    if (!need_bar1)
        return;
    if (opt->wc_path) {
        dev->wc_fd = open(opt->wc_path, O_RDWR | O_CLOEXEC);
        if (dev->wc_fd >= 0) {
            dev->bar1 = mmap(NULL, BAR1_MAP_SIZE, PROT_READ | PROT_WRITE,
                             MAP_SHARED, dev->wc_fd, 0);
            if (dev->bar1 != MAP_FAILED)
                dev->bar1_wc = true;
            else
                dev->bar1 = NULL;
        }
    }
    if (!dev->bar1) {
        dev->bar1 = mmap(NULL, BAR1_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                         dev->fd, (off_t)dev->bar1_offset);
        if (dev->bar1 == MAP_FAILED)
            die("mmap VFIO BAR1");
        dev->bar1_wc = false;
    }
    (void)madvise((void *)dev->bar1, BAR1_MAP_SIZE,
                  MADV_DONTFORK | MADV_DONTDUMP);
}

static void restore_pramin(struct device *dev)
{
    if (!dev->bar0 || dev->current_pramin == dev->saved_pramin)
        return;
    mmio_write32(dev->bar0, (size_t)dev->hw.pbus_bar0_window,
                 dev->saved_pramin);
    dev->current_pramin = mmio_read32(dev->bar0,
                                     (size_t)dev->hw.pbus_bar0_window);
    if (dev->current_pramin != dev->saved_pramin) {
        fprintf(stderr, "fatal: PRAMIN selector restoration failed\n");
        _exit(125);
    }
}

static void emergency_restore_pramin(void)
{
    if (active_device)
        restore_pramin(active_device);
}

static void device_close(struct device *dev)
{
    restore_pramin(dev);
    if (dev->bar1)
        munmap((void *)dev->bar1, BAR1_MAP_SIZE);
    if (dev->bar0)
        munmap((void *)dev->bar0, (size_t)dev->bar0_size);
    if (dev->wc_fd >= 0)
        close(dev->wc_fd);
    if (dev->fd >= 0)
        close(dev->fd);
    if (active_device == dev)
        active_device = NULL;
}

static void pramin_select(struct device *dev, uint64_t address)
{
    const uint64_t base = address & ~(dev->hw.pramin_size - 1);
    const uint32_t target = dev->saved_pramin & 0x03000000u;
    const uint32_t selector = target | (uint32_t)((base >> 16) & 0x00ffffffu);
    if (selector == dev->current_pramin)
        return;
    mmio_write32(dev->bar0, (size_t)dev->hw.pbus_bar0_window, selector);
    dev->current_pramin = mmio_read32(dev->bar0,
                                     (size_t)dev->hw.pbus_bar0_window);
    if (dev->current_pramin != selector) {
        fprintf(stderr, "PRAMIN selector readback mismatch\n");
        exit(EXIT_FAILURE);
    }
}

static void pramin_read(struct device *dev, uint64_t address, void *output, size_t length)
{
    uint8_t *dst = output;
    while (length) {
        pramin_select(dev, address);
        size_t offset = (size_t)(address & (dev->hw.pramin_size - 1));
        size_t count = (size_t)dev->hw.pramin_size - offset;
        if (count > length)
            count = length;
        memcpy(dst, (const void *)(dev->bar0 + dev->hw.pramin_offset + offset), count);
        dst += count;
        address += count;
        length -= count;
    }
}

static void pramin_write(struct device *dev, uint64_t address,
                         const void *input, size_t length)
{
    const uint8_t *src = input;
    while (length) {
        pramin_select(dev, address);
        size_t offset = (size_t)(address & (dev->hw.pramin_size - 1));
        size_t count = (size_t)dev->hw.pramin_size - offset;
        if (count > length)
            count = length;
        memcpy((void *)(dev->bar0 + dev->hw.pramin_offset + offset), src, count);
        (void)mmio_read32(dev->bar0, (size_t)dev->hw.pramin_offset + offset);
        src += count;
        address += count;
        length -= count;
    }
}

static uint64_t pde_address(uint64_t entry)
{
    return (entry & ~UINT64_C(0xff)) << 4;
}

static void locate_pd0(struct device *dev, uint64_t *instance_out,
                       uint64_t *root_out, uint64_t *pd0_out)
{
    uint32_t block = mmio_read32(dev->bar0, (size_t)dev->hw.bar1_block);
    if (!(block & 0x80000000u)) {
        fprintf(stderr, "BAR1 is not in virtual mode\n");
        exit(EXIT_FAILURE);
    }
    uint64_t instance = ((uint64_t)block & 0x0fffffffu) << 12;
    const uint64_t last = dev->hw.instance_pdb_offset > dev->hw.instance_limit_offset ?
                          dev->hw.instance_pdb_offset : dev->hw.instance_limit_offset;
    if (last > SIZE_MAX - 8)
        die("instance profile overflow");
    const size_t inst_size = (size_t)last + 8;
    uint8_t *inst = calloc(1, inst_size);
    if (!inst)
        die("allocate BAR1 instance block");
    pramin_read(dev, instance, inst, inst_size);
    uint64_t pdb, limit;
    memcpy(&pdb, inst + dev->hw.instance_pdb_offset, sizeof(pdb));
    memcpy(&limit, inst + dev->hw.instance_limit_offset, sizeof(limit));
    free(inst);
    /* AD10x leaves the legacy NV_RAMIN_ADR_LIMIT pair clear for a large
     * resizable BAR1.  The live VFIO region is authoritative in that case;
     * all older profiles retain the encoded-limit check. */
    if (limit == 0 && dev->hw.chipset >= 0x190u && dev->hw.chipset <= 0x19fu)
        limit = dev->bar1_size;
    else
        limit++;
    if (limit != dev->bar1_size) {
        fprintf(stderr, "BAR1 instance limit 0x%" PRIx64
                        " differs from VFIO size 0x%" PRIx64 "\n",
                limit, dev->bar1_size);
        exit(EXIT_FAILURE);
    }
    uint64_t root = pdb & ~UINT64_C(0xfff);
    uint64_t table = root;
    for (unsigned level = 0; level < dev->hw.page_table_levels; ++level) {
        uint64_t entry;
        pramin_read(dev, table, &entry, sizeof(entry));
        table = pde_address(entry);
        if (!table) {
            fprintf(stderr, "missing BAR1 page-table level %u\n", level);
            exit(EXIT_FAILURE);
        }
    }
    *instance_out = instance;
    *root_out = root;
    *pd0_out = table;
}

static void tlb_invalidate(struct device *dev, uint64_t root)
{
    _mm_sfence();
    mmio_write32(dev->bar0, (size_t)dev->hw.mmu_invalidate_pdb,
                 (uint32_t)(root >> 8));
    mmio_write32(dev->bar0, (size_t)dev->hw.mmu_invalidate_upper_pdb,
                 (uint32_t)(root >> 40));
    mmio_write32(dev->bar0, (size_t)dev->hw.mmu_invalidate, 0x80000007u);
    for (unsigned iteration = 0; iteration < 2000000; ++iteration) {
        if (!(mmio_read32(dev->bar0, (size_t)dev->hw.mmu_invalidate) &
              0x80000000u))
            return;
        if ((iteration & 0xfff) == 0)
            sched_yield();
    }
    fprintf(stderr, "BAR1 TLB invalidation timed out\n");
    exit(EXIT_FAILURE);
}

static void probe_hardware(const struct options *opt)
{
    struct device dev;
    device_open(&dev, opt, false);
    uint64_t instance, root, pd0;
    locate_pd0(&dev, &instance, &root, &pd0);
    printf("validated NVIDIA hardware profile: BAR0=0x%" PRIx64
           " BAR1=0x%" PRIx64 " instance=0x%" PRIx64
           " root=0x%" PRIx64 " leaf=0x%" PRIx64 "\n",
           dev.bar0_size, dev.bar1_size, instance, root, pd0);
    device_close(&dev);
}

static void state_write(const char *path, const struct map_state_header *header,
                        const struct map_state_entry *entries)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        die("create state file");
    size_t bytes = sizeof(*header) + header->count * sizeof(*entries);
    uint8_t *blob = malloc(bytes);
    if (!blob)
        die("malloc state");
    memcpy(blob, header, sizeof(*header));
    memcpy(blob + sizeof(*header), entries, header->count * sizeof(*entries));
    ssize_t written = write(fd, blob, bytes);
    if (written != (ssize_t)bytes)
        die("write state file");
    if (fsync(fd) < 0)
        die("fsync state file");
    free(blob);
    close(fd);
}

static struct map_state_entry *state_read(const char *path,
                                          struct map_state_header *header)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        die("open state file");
    if (read(fd, header, sizeof(*header)) != (ssize_t)sizeof(*header))
        die("read state header");
    if (memcmp(header->magic, "T104B1\0", 8) || header->version != 1 ||
        !header->count || header->count > 128)
        die("invalid state file");
    struct map_state_entry *entries = calloc(header->count, sizeof(*entries));
    if (!entries)
        die("calloc state entries");
    size_t bytes = header->count * sizeof(*entries);
    if (read(fd, entries, bytes) != (ssize_t)bytes)
        die("read state entries");
    close(fd);
    return entries;
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t aa = *(const uint64_t *)a, bb = *(const uint64_t *)b;
    return (aa > bb) - (aa < bb);
}

static void write_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *cursor = buffer;
    while (length) {
        ssize_t result = write(fd, cursor, length);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            die("write sparse state");
        }
        if (!result) {
            errno = EIO;
            die("write sparse state");
        }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
}

static void read_all(int fd, void *buffer, size_t length)
{
    uint8_t *cursor = buffer;
    while (length) {
        ssize_t result = read(fd, cursor, length);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            die("read sparse state");
        }
        if (!result) {
            errno = EIO;
            die("read sparse state");
        }
        cursor += (size_t)result;
        length -= (size_t)result;
    }
}

static uint64_t *sorted_surfaces(const struct options *opt)
{
    if (!opt->surface_count || opt->surface_count > MAX_SURFACES ||
        !opt->surface_size) {
        fprintf(stderr, "sparse mapping requires surfaces and a surface size\n");
        exit(EXIT_FAILURE);
    }
    uint64_t *surfaces = malloc(opt->surface_count * sizeof(*surfaces));
    if (!surfaces)
        die("malloc surfaces");
    memcpy(surfaces, opt->surfaces, opt->surface_count * sizeof(*surfaces));
    qsort(surfaces, opt->surface_count, sizeof(*surfaces), compare_u64);
    for (size_t i = 0; i < opt->surface_count; ++i) {
        if (surfaces[i] > UINT64_MAX - opt->surface_size) {
            fprintf(stderr, "surface range overflows address space\n");
            exit(EXIT_FAILURE);
        }
        if (i && surfaces[i] == surfaces[i - 1]) {
            fprintf(stderr, "duplicate surface 0x%" PRIx64 "\n", surfaces[i]);
            exit(EXIT_FAILURE);
        }
    }
    return surfaces;
}

static uint64_t *build_sparse_pages(const uint64_t *surfaces,
                                    size_t surface_count,
                                    uint64_t surface_size,
                                    uint32_t *count_out)
{
    uint64_t *pages = calloc(BAR1_MAP_SIZE / PAGE_2M, sizeof(*pages));
    if (!pages)
        die("calloc sparse pages");
    uint32_t count = 0;
    for (size_t surface = 0; surface < surface_count; ++surface) {
        uint64_t first = surfaces[surface] & ~((uint64_t)PAGE_2M - 1);
        uint64_t last = (surfaces[surface] + surface_size - 1) &
                        ~((uint64_t)PAGE_2M - 1);
        for (uint64_t page = first;; page += PAGE_2M) {
            bool present = false;
            for (uint32_t i = 0; i < count; ++i)
                if (pages[i] == page) {
                    present = true;
                    break;
                }
            if (!present) {
                if (count == BAR1_MAP_SIZE / PAGE_2M) {
                    fprintf(stderr, "surface pages exceed the BAR1 aperture\n");
                    exit(EXIT_FAILURE);
                }
                pages[count++] = page;
            }
            if (page == last)
                break;
        }
    }
    qsort(pages, count, sizeof(*pages), compare_u64);
    *count_out = count;
    return pages;
}

static void sparse_state_write(const char *path,
                               const struct sparse_state *state)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        die("create sparse state file");
    write_all(fd, &state->header, sizeof(state->header));
    write_all(fd, state->entries,
              state->header.page_count * sizeof(*state->entries));
    write_all(fd, state->surfaces,
              state->header.surface_count * sizeof(*state->surfaces));
    if (fsync(fd) < 0)
        die("fsync sparse state file");
    if (close(fd) < 0)
        die("close sparse state file");
}

static struct sparse_state sparse_state_read(const char *path)
{
    struct sparse_state state = {0};
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        die("open sparse state file");
    read_all(fd, &state.header, sizeof(state.header));
    if (memcmp(state.header.magic, "T104SP1\0", 8) ||
        state.header.version != 1 || !state.header.page_count ||
        state.header.page_count > BAR1_MAP_SIZE / PAGE_2M ||
        !state.header.surface_count ||
        state.header.surface_count > MAX_SURFACES ||
        !state.header.surface_size) {
        fprintf(stderr, "invalid sparse state header\n");
        exit(EXIT_FAILURE);
    }
    state.entries = calloc(state.header.page_count, sizeof(*state.entries));
    state.surfaces = calloc(state.header.surface_count, sizeof(*state.surfaces));
    if (!state.entries || !state.surfaces)
        die("calloc sparse state");
    read_all(fd, state.entries,
             state.header.page_count * sizeof(*state.entries));
    read_all(fd, state.surfaces,
             state.header.surface_count * sizeof(*state.surfaces));
    uint8_t trailing;
    ssize_t result = read(fd, &trailing, 1);
    if (result != 0) {
        fprintf(stderr, "sparse state file has trailing or unreadable data\n");
        exit(EXIT_FAILURE);
    }
    close(fd);
    for (uint32_t i = 0; i < state.header.page_count; ++i) {
        if (state.entries[i].pde_index >= BAR1_MAP_SIZE / PAGE_2M ||
            (state.entries[i].vram_page & (PAGE_2M - 1))) {
            fprintf(stderr, "invalid sparse state entry %u\n", i);
            exit(EXIT_FAILURE);
        }
        for (uint32_t j = 0; j < i; ++j)
            if (state.entries[i].pde_index == state.entries[j].pde_index ||
                state.entries[i].vram_page == state.entries[j].vram_page) {
                fprintf(stderr, "duplicate sparse state entry\n");
                exit(EXIT_FAILURE);
            }
    }
    return state;
}

static void sparse_state_free(struct sparse_state *state)
{
    free(state->entries);
    free(state->surfaces);
    memset(state, 0, sizeof(*state));
}

static void validate_sparse_surfaces(const struct options *opt,
                                     const struct sparse_state *state)
{
    uint64_t *surfaces = sorted_surfaces(opt);
    if (opt->surface_count != state->header.surface_count ||
        opt->surface_size != state->header.surface_size ||
        memcmp(surfaces, state->surfaces,
               opt->surface_count * sizeof(*surfaces))) {
        fprintf(stderr, "live surface set differs from sparse mapping state\n");
        exit(EXIT_FAILURE);
    }
    free(surfaces);
}

static const struct sparse_state_entry *sparse_entry_for_page(
    const struct sparse_state *state, uint64_t vram_page)
{
    for (uint32_t i = 0; i < state->header.page_count; ++i)
        if (state->entries[i].vram_page == vram_page)
            return &state->entries[i];
    return NULL;
}

static void install_mapping(const struct options *opt)
{
    if ((opt->bar1_va | opt->vram_base | opt->span) & (PAGE_2M - 1) ||
        !opt->span || opt->bar1_va + opt->span > BAR1_MAP_SIZE) {
        fprintf(stderr, "mapping addresses/span must be 2 MiB aligned and in BAR1\n");
        exit(EXIT_FAILURE);
    }
    uint32_t count = (uint32_t)(opt->span / PAGE_2M);
    struct device dev;
    device_open(&dev, opt, true);
    uint64_t instance, root, pd0;
    locate_pd0(&dev, &instance, &root, &pd0);
    uint32_t first = (uint32_t)(opt->bar1_va / PAGE_2M);
    struct map_state_entry *old = calloc(count, sizeof(*old));
    if (!old)
        die("calloc mapping backup");
    pramin_read(&dev, pd0 + (uint64_t)first * 16, old,
                (size_t)count * sizeof(*old));
    for (uint32_t i = 0; i < count; ++i) {
        if (!((old[i].lo == 0 || old[i].lo == 8) && old[i].hi == 0)) {
            fprintf(stderr, "BAR1 PDE %u is not invalid/sparse: %016" PRIx64
                            " %016" PRIx64 "\n", first + i, old[i].lo, old[i].hi);
            exit(EXIT_FAILURE);
        }
    }
    struct map_state_header header = {
        .magic = {'T','1','0','4','B','1','\0','\0'}, .version = 1,
        .count = count, .instance = instance, .root = root, .pd0 = pd0,
        .bar1_va = opt->bar1_va, .vram_base = opt->vram_base, .span = opt->span,
    };
    state_write(opt->state_path, &header, old);
    for (uint32_t i = 0; i < count; ++i) {
        struct map_state_entry mapped = {
            .lo = ((opt->vram_base + (uint64_t)i * PAGE_2M) >> 4) | 1,
            .hi = 0,
        };
        pramin_write(&dev, pd0 + (uint64_t)(first + i) * 16,
                     &mapped, sizeof(mapped));
    }
    tlb_invalidate(&dev, root);
    struct map_state_entry *verify = calloc(count, sizeof(*verify));
    if (!verify)
        die("calloc verify");
    pramin_read(&dev, pd0 + (uint64_t)first * 16, verify,
                (size_t)count * sizeof(*verify));
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t expected = ((opt->vram_base + (uint64_t)i * PAGE_2M) >> 4) | 1;
        if (verify[i].lo != expected || verify[i].hi != 0) {
            fprintf(stderr, "PDE verification failed at %u\n", first + i);
            exit(EXIT_FAILURE);
        }
    }
    const uint64_t sample_offsets[] = {0, opt->span / 2, opt->span - 64};
    for (size_t sample = 0; sample < 3; ++sample) {
        uint64_t sample_offset = sample_offsets[sample];
        uint8_t physical[64], translated[64];
        pramin_read(&dev, opt->vram_base + sample_offset,
                    physical, sizeof(physical));
        memcpy(translated,
               (const void *)(dev.bar1 + opt->bar1_va + sample_offset),
               sizeof(translated));
        if (memcmp(physical, translated, sizeof(physical))) {
            fprintf(stderr, "BAR1 translation verification failed at +0x%" PRIx64 "\n",
                    sample_offset);
            exit(EXIT_FAILURE);
        }
    }
    free(verify);
    free(old);
    restore_pramin(&dev);
    printf("installed %u x 2MiB mappings: BAR1 0x%08" PRIx64
           " -> VRAM 0x%08" PRIx64 " (%s mapping)\n",
           count, opt->bar1_va, opt->vram_base,
           dev.bar1_wc ? "write-combining" : "VFIO uncached");
    device_close(&dev);
}

static void restore_mapping(const struct options *opt)
{
    struct map_state_header header;
    struct map_state_entry *old = state_read(opt->state_path, &header);
    struct device dev;
    device_open(&dev, opt, false);
    uint64_t instance, root, pd0;
    locate_pd0(&dev, &instance, &root, &pd0);
    if (instance != header.instance || root != header.root || pd0 != header.pd0) {
        fprintf(stderr, "live BAR1 hierarchy differs from saved state\n");
        exit(EXIT_FAILURE);
    }
    uint32_t first = (uint32_t)(header.bar1_va / PAGE_2M);
    for (uint32_t i = 0; i < header.count; ++i) {
        struct map_state_entry current;
        pramin_read(&dev, pd0 + (uint64_t)(first + i) * 16,
                    &current, sizeof(current));
        uint64_t expected = ((header.vram_base + (uint64_t)i * PAGE_2M) >> 4) | 1;
        if (current.lo != expected || current.hi != 0) {
            fprintf(stderr, "refusing restore: PDE %u changed behind us\n", first + i);
            exit(EXIT_FAILURE);
        }
    }
    pramin_write(&dev, pd0 + (uint64_t)first * 16, old,
                 (size_t)header.count * sizeof(*old));
    tlb_invalidate(&dev, root);
    restore_pramin(&dev);
    device_close(&dev);
    free(old);
    if (unlink(opt->state_path) < 0)
        die("unlink state file");
    printf("restored %u BAR1 PDEs exactly\n", header.count);
}

static void verify_mapping(const struct options *opt)
{
    struct map_state_header header;
    struct map_state_entry *old = state_read(opt->state_path, &header);
    free(old);
    struct device dev;
    device_open(&dev, opt, false);
    uint64_t instance, root, pd0;
    locate_pd0(&dev, &instance, &root, &pd0);
    if (instance != header.instance || root != header.root || pd0 != header.pd0 ||
        opt->bar1_va != header.bar1_va || opt->vram_base != header.vram_base ||
        opt->span != header.span) {
        fprintf(stderr, "BAR1 hierarchy or mapping geometry changed\n");
        exit(EXIT_FAILURE);
    }
    uint32_t first = (uint32_t)(header.bar1_va / PAGE_2M);
    for (uint32_t i = 0; i < header.count; ++i) {
        struct map_state_entry current;
        pramin_read(&dev, pd0 + (uint64_t)(first + i) * 16,
                    &current, sizeof(current));
        uint64_t expected = ((header.vram_base + (uint64_t)i * PAGE_2M) >> 4) | 1;
        if (current.lo != expected || current.hi != 0) {
            fprintf(stderr, "BAR1 mapping fingerprint mismatch at PDE %u\n",
                    first + i);
            exit(EXIT_FAILURE);
        }
    }
    restore_pramin(&dev);
    device_close(&dev);
    printf("verified %u BAR1 PDEs exactly\n", header.count);
}

static void validate_sparse_hierarchy(struct device *dev,
                                      const struct sparse_state *state,
                                      uint64_t *root_out, uint64_t *pd0_out)
{
    uint64_t instance, root, pd0;
    locate_pd0(dev, &instance, &root, &pd0);
    if (instance != state->header.instance || root != state->header.root ||
        pd0 != state->header.pd0) {
        fprintf(stderr, "live BAR1 hierarchy differs from sparse state\n");
        exit(EXIT_FAILURE);
    }
    if (root_out)
        *root_out = root;
    if (pd0_out)
        *pd0_out = pd0;
}

static void validate_sparse_pdes(struct device *dev,
                                 const struct sparse_state *state)
{
    for (uint32_t i = 0; i < state->header.page_count; ++i) {
        const struct sparse_state_entry *entry = &state->entries[i];
        struct map_state_entry current;
        pramin_read(dev,
                    state->header.pd0 + (uint64_t)entry->pde_index * 16,
                    &current, sizeof(current));
        uint64_t expected = (entry->vram_page >> 4) | 1;
        if (current.lo != expected || current.hi != 0) {
            fprintf(stderr,
                    "sparse mapping fingerprint mismatch at PDE %u\n",
                    entry->pde_index);
            exit(EXIT_FAILURE);
        }
    }
}

static void install_sparse_mapping(const struct options *opt)
{
    uint64_t *surfaces = sorted_surfaces(opt);
    uint32_t page_count;
    uint64_t *pages = build_sparse_pages(surfaces, opt->surface_count,
                                         opt->surface_size, &page_count);
    struct device dev;
    device_open(&dev, opt, true);
    uint64_t instance, root, pd0;
    locate_pd0(&dev, &instance, &root, &pd0);

    struct map_state_entry live[BAR1_MAP_SIZE / PAGE_2M];
    pramin_read(&dev, pd0, live, sizeof(live));
    struct sparse_state state = {
        .header = {
            .magic = {'T','1','0','4','S','P','1','\0'},
            .version = 1,
            .page_count = page_count,
            .surface_count = (uint32_t)opt->surface_count,
            .instance = instance,
            .root = root,
            .pd0 = pd0,
            .surface_size = opt->surface_size,
        },
        .entries = calloc(page_count, sizeof(*state.entries)),
        .surfaces = surfaces,
    };
    if (!state.entries)
        die("calloc sparse entries");

    uint32_t assigned = 0;
    for (uint32_t index = BAR1_MAP_SIZE / PAGE_2M;
         index-- > 0 && assigned < page_count;) {
        if ((live[index].lo == 0 || live[index].lo == 8) && live[index].hi == 0) {
            state.entries[assigned] = (struct sparse_state_entry) {
                .pde_index = index,
                .vram_page = pages[assigned],
                .old_lo = live[index].lo,
                .old_hi = live[index].hi,
            };
            ++assigned;
        }
    }
    if (assigned != page_count) {
        fprintf(stderr, "need %u free BAR1 PDEs, found %u\n",
                page_count, assigned);
        exit(EXIT_FAILURE);
    }

    /* Persist the exact rollback record before touching any live PDE. */
    sparse_state_write(opt->state_path, &state);
    for (uint32_t i = 0; i < page_count; ++i) {
        struct map_state_entry mapped = {
            .lo = (state.entries[i].vram_page >> 4) | 1,
            .hi = 0,
        };
        pramin_write(&dev, pd0 + (uint64_t)state.entries[i].pde_index * 16,
                     &mapped, sizeof(mapped));
    }
    tlb_invalidate(&dev, root);
    validate_sparse_pdes(&dev, &state);

    for (uint32_t i = 0; i < page_count; ++i) {
        uint8_t physical[64], translated[64];
        pramin_read(&dev, state.entries[i].vram_page,
                    physical, sizeof(physical));
        memcpy(translated,
               (const void *)(dev.bar1 +
                              (uint64_t)state.entries[i].pde_index * PAGE_2M),
               sizeof(translated));
        if (memcmp(physical, translated, sizeof(physical))) {
            fprintf(stderr,
                    "sparse BAR1 translation verification failed for VRAM 0x%" PRIx64 "\n",
                    state.entries[i].vram_page);
            exit(EXIT_FAILURE);
        }
    }
    restore_pramin(&dev);
    printf("installed %u sparse 2MiB mappings for %zu surfaces (%s mapping)\n",
           page_count, opt->surface_count,
           dev.bar1_wc ? "write-combining" : "VFIO uncached");
    device_close(&dev);
    sparse_state_free(&state);
    free(pages);
}

static void verify_sparse_mapping(const struct options *opt)
{
    struct sparse_state state = sparse_state_read(opt->state_path);
    validate_sparse_surfaces(opt, &state);
    struct device dev;
    device_open(&dev, opt, false);
    validate_sparse_hierarchy(&dev, &state, NULL, NULL);
    validate_sparse_pdes(&dev, &state);
    restore_pramin(&dev);
    device_close(&dev);
    printf("verified %u sparse BAR1 PDEs for %u surfaces exactly\n",
           state.header.page_count, state.header.surface_count);
    sparse_state_free(&state);
}

static void restore_sparse_mapping(const struct options *opt)
{
    struct sparse_state state = sparse_state_read(opt->state_path);
    struct device dev;
    device_open(&dev, opt, false);
    uint64_t root, pd0;
    validate_sparse_hierarchy(&dev, &state, &root, &pd0);
    for (uint32_t i = 0; i < state.header.page_count; ++i) {
        const struct sparse_state_entry *entry = &state.entries[i];
        struct map_state_entry current;
        pramin_read(&dev, pd0 + (uint64_t)entry->pde_index * 16,
                    &current, sizeof(current));
        uint64_t expected = (entry->vram_page >> 4) | 1;
        bool ours = current.lo == expected && current.hi == 0;
        bool original = current.lo == entry->old_lo && current.hi == entry->old_hi;
        if (!ours && !original) {
            fprintf(stderr,
                    "refusing sparse restore: PDE %u changed behind us\n",
                    entry->pde_index);
            exit(EXIT_FAILURE);
        }
    }
    for (uint32_t i = 0; i < state.header.page_count; ++i) {
        const struct sparse_state_entry *entry = &state.entries[i];
        struct map_state_entry original = {
            .lo = entry->old_lo,
            .hi = entry->old_hi,
        };
        pramin_write(&dev, pd0 + (uint64_t)entry->pde_index * 16,
                     &original, sizeof(original));
    }
    tlb_invalidate(&dev, root);
    restore_pramin(&dev);
    device_close(&dev);
    if (unlink(opt->state_path) < 0)
        die("unlink sparse state file");
    printf("restored %u sparse BAR1 PDEs exactly\n", state.header.page_count);
    sparse_state_free(&state);
}

static uint32_t blocklinear_offset(unsigned x, unsigned y,
                                   unsigned pitch_blocks, unsigned log2_gobs_y)
{
    unsigned block_height = 8u << log2_gobs_y;
    unsigned block_x = (x * 4u) / 64u;
    unsigned block_y = y / block_height;
    unsigned iy = y % block_height;
    unsigned swizzle = (x & 3u) | ((iy & 3u) << 2) | ((x & 4u) << 2) |
                       ((iy & 4u) << 3) | ((x & 8u) << 3) | ((iy >> 3) << 7);
    return (block_y * pitch_blocks + block_x) * 64u * block_height + swizzle * 4u;
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t aa = *(const uint32_t *)a, bb = *(const uint32_t *)b;
    return (aa > bb) - (aa < bb);
}

static inline uint64_t colored_pixel_key(const struct colored_pixel *pixel)
{
    return ((uint64_t)pixel->color << 32) | pixel->offset;
}

static void radix_sort_colored_pixels(struct colored_pixel *pixels, size_t count)
{
    if (count < 2)
        return;
    struct colored_pixel *scratch = malloc(count * sizeof(*scratch));
    uint32_t *buckets = calloc(UINT32_C(1) << 16, sizeof(*buckets));
    if (!scratch || !buckets)
        die("allocate colored-pixel radix workspace");

    struct colored_pixel *source = pixels;
    struct colored_pixel *destination = scratch;
    for (unsigned pass = 0; pass < 4; ++pass) {
        const unsigned shift = pass * 16;
        memset(buckets, 0, (UINT32_C(1) << 16) * sizeof(*buckets));
        for (size_t index = 0; index < count; ++index)
            ++buckets[(colored_pixel_key(&source[index]) >> shift) & 0xffffu];
        uint32_t cursor = 0;
        for (size_t bucket = 0; bucket < (UINT32_C(1) << 16); ++bucket) {
            const uint32_t entries = buckets[bucket];
            buckets[bucket] = cursor;
            cursor += entries;
        }
        for (size_t index = 0; index < count; ++index) {
            const uint64_t key = colored_pixel_key(&source[index]);
            destination[buckets[(key >> shift) & 0xffffu]++] = source[index];
        }
        struct colored_pixel *swap = source;
        source = destination;
        destination = swap;
    }
    free(scratch);
    free(buckets);
}

static uint32_t *build_blocklinear_offset_lut(const struct options *opt)
{
    if (!opt->frame_width || !opt->frame_height || !opt->pitch_blocks ||
        opt->log2_gobs_y > 5 ||
        opt->frame_width > SIZE_MAX / opt->frame_height)
        die("invalid framebuffer geometry for offset lookup table");
    const size_t count = (size_t)opt->frame_width * opt->frame_height;
    uint32_t *offsets = malloc(count * sizeof(*offsets));
    if (!offsets)
        die("allocate block-linear offset lookup table");
    for (unsigned y = 0; y < opt->frame_height; ++y)
        for (unsigned x = 0; x < opt->frame_width; ++x)
            offsets[(size_t)y * opt->frame_width + x] = blocklinear_offset(
                x, y, opt->pitch_blocks, opt->log2_gobs_y);
    return offsets;
}

static struct segment *build_segments(const struct options *opt, size_t *count_out)
{
    if (!opt->frame_width || !opt->frame_height || !opt->pitch_blocks ||
        opt->log2_gobs_y > 5 || !opt->surface_size ||
        !opt->width || !opt->height ||
        opt->x > opt->frame_width || opt->width > opt->frame_width - opt->x ||
        opt->y > opt->frame_height || opt->height > opt->frame_height - opt->y) {
        fprintf(stderr, "invalid framebuffer geometry or rectangle\n");
        exit(EXIT_FAILURE);
    }
    size_t pixels = (size_t)opt->width * opt->height;
    uint32_t *offsets = malloc(pixels * sizeof(*offsets));
    if (!offsets)
        die("malloc pixel offsets");
    size_t cursor = 0;
    for (unsigned y = opt->y; y < opt->y + opt->height; ++y)
        for (unsigned x = opt->x; x < opt->x + opt->width; ++x)
            offsets[cursor++] = blocklinear_offset(x, y, opt->pitch_blocks,
                                                   opt->log2_gobs_y);
    qsort(offsets, pixels, sizeof(*offsets), compare_u32);
    struct segment *segments = malloc(pixels * sizeof(*segments));
    if (!segments)
        die("malloc segments");
    size_t count = 0;
    for (size_t i = 0; i < pixels;) {
        uint32_t start = offsets[i], end = start + 4;
        ++i;
        while (i < pixels && offsets[i] == end) {
            end += 4;
            ++i;
        }
        segments[count++] = (struct segment){.offset = start, .length = end - start};
    }
    if (count &&
        (uint64_t)segments[count - 1].offset + segments[count - 1].length >
            opt->surface_size) {
        fprintf(stderr, "rectangle layout exceeds the allocation size\n");
        exit(EXIT_FAILURE);
    }
    free(offsets);
    *count_out = count;
    return segments;
}

static struct segment *build_sparse_draw_segments(
    const struct options *opt, const struct sparse_state *state,
    const struct segment *source, size_t source_count, size_t *count_out)
{
    size_t capacity = source_count * opt->surface_count + 16;
    struct segment *draw = malloc(capacity * sizeof(*draw));
    if (!draw)
        die("malloc sparse draw segments");
    size_t count = 0;
    for (size_t surface = 0; surface < opt->surface_count; ++surface) {
        for (size_t run = 0; run < source_count; ++run) {
            uint64_t address = opt->surfaces[surface] + source[run].offset;
            uint32_t remaining = source[run].length;
            while (remaining) {
                uint64_t page = address & ~((uint64_t)PAGE_2M - 1);
                const struct sparse_state_entry *entry =
                    sparse_entry_for_page(state, page);
                if (!entry) {
                    fprintf(stderr,
                            "no sparse BAR1 mapping for VRAM page 0x%" PRIx64 "\n",
                            page);
                    exit(EXIT_FAILURE);
                }
                uint32_t in_page = (uint32_t)(address - page);
                uint32_t length = PAGE_2M - in_page;
                if (length > remaining)
                    length = remaining;
                if (count == capacity) {
                    capacity *= 2;
                    struct segment *larger = realloc(draw,
                                                      capacity * sizeof(*draw));
                    if (!larger)
                        die("realloc sparse draw segments");
                    draw = larger;
                }
                draw[count++] = (struct segment) {
                    .offset = entry->pde_index * PAGE_2M + in_page,
                    .length = length,
                };
                address += length;
                remaining -= length;
            }
        }
    }
    *count_out = count;
    return draw;
}

static bool valid_box(const struct options *opt, const struct tu104_box *box)
{
    return box->width && box->height && box->x <= opt->frame_width &&
           box->width <= opt->frame_width - box->x &&
           box->y <= opt->frame_height &&
           box->height <= opt->frame_height - box->y;
}

static size_t outline_pixel_count(const struct tu104_box *box)
{
    const unsigned thickness = 2;
    const unsigned top = box->height < thickness ? box->height : thickness;
    const unsigned bottom_space = box->height - top;
    const unsigned bottom = bottom_space < thickness ? bottom_space : thickness;
    const unsigned middle = box->height - top - bottom;
    const unsigned left = box->width < thickness ? box->width : thickness;
    const unsigned right_space = box->width - left;
    const unsigned right = right_space < thickness ? right_space : thickness;
    return (size_t)(top + bottom) * box->width +
           (size_t)middle * (left + right);
}

static void append_outline_pixels(const struct options *opt,
                                  const uint32_t *offset_lut,
                                  const struct tu104_box *box,
                                  struct colored_pixel *pixels,
                                  size_t *cursor)
{
    const unsigned thickness = 2;
    const unsigned top = box->height < thickness ? box->height : thickness;
    const unsigned bottom_space = box->height - top;
    const unsigned bottom = bottom_space < thickness ? bottom_space : thickness;
    const unsigned left = box->width < thickness ? box->width : thickness;
    const unsigned right_space = box->width - left;
    const unsigned right = right_space < thickness ? right_space : thickness;
    const unsigned x0 = box->x;
    const unsigned x1 = box->x + box->width;
    const unsigned y0 = box->y;
    const unsigned y1 = box->y + box->height;

#define APPEND_PIXEL(px, py) do {                                                \
        pixels[*cursor].offset = offset_lut[                                    \
            (size_t)(py) * opt->frame_width + (px)];                            \
        pixels[*cursor].color = box->color;                                     \
        ++*cursor;                                                               \
    } while (0)

    for (unsigned y = y0; y < y0 + top; ++y)
        for (unsigned x = x0; x < x1; ++x)
            APPEND_PIXEL(x, y);
    for (unsigned y = y1 - bottom; y < y1; ++y)
        for (unsigned x = x0; x < x1; ++x)
            APPEND_PIXEL(x, y);
    for (unsigned y = y0 + top; y < y1 - bottom; ++y) {
        for (unsigned x = x0; x < x0 + left; ++x)
            APPEND_PIXEL(x, y);
        for (unsigned x = x1 - right; x < x1; ++x)
            APPEND_PIXEL(x, y);
    }
#undef APPEND_PIXEL
}

static struct colored_segment *build_box_draw_segments(
    const struct options *opt, const struct sparse_state *state,
    const uint32_t *offset_lut, const struct tu104_box_frame *frame,
    size_t *count_out)
{
    size_t pixel_count = 0;
    for (uint16_t box_index = 0; box_index < frame->count; ++box_index) {
        const struct tu104_box *box = &frame->boxes[box_index];
        if (!valid_box(opt, box))
            return NULL;
        size_t count = outline_pixel_count(box);
        if (count > SIZE_MAX - pixel_count)
            die("box pixel count overflow");
        pixel_count += count;
    }

    if (!pixel_count) {
        *count_out = 0;
        return NULL;
    }

    struct colored_pixel *pixels = malloc(pixel_count * sizeof(*pixels));
    if (!pixels)
        die("malloc batched box pixels");
    size_t pixel_cursor = 0;
    for (uint16_t box_index = 0; box_index < frame->count; ++box_index)
        append_outline_pixels(opt, offset_lut, &frame->boxes[box_index], pixels,
                              &pixel_cursor);
    radix_sort_colored_pixels(pixels, pixel_count);

    struct colored_segment *source = malloc(pixel_count * sizeof(*source));
    if (!source)
        die("malloc batched box segments");
    size_t source_count = 0;
    for (size_t i = 0; i < pixel_count;) {
        const uint32_t color = pixels[i].color;
        const uint32_t start = pixels[i].offset;
        uint32_t end = start + 4;
        ++i;
        while (i < pixel_count && pixels[i].color == color) {
            if (pixels[i].offset < end) {
                ++i;
                continue;
            }
            if (pixels[i].offset != end)
                break;
            end += 4;
            ++i;
        }
        if ((uint64_t)end > opt->surface_size) {
            free(pixels);
            free(source);
            return NULL;
        }
        source[source_count++] = (struct colored_segment) {
            .offset = start,
            .length = end - start,
            .color = color,
        };
    }
    free(pixels);

    size_t capacity = source_count * opt->surface_count + 16;
    struct colored_segment *result = malloc(capacity * sizeof(*result));
    if (!result)
        die("malloc sparse colored segments");
    size_t count = 0;
    for (size_t surface = 0; surface < opt->surface_count; ++surface) {
        for (size_t run = 0; run < source_count; ++run) {
            uint64_t address = opt->surfaces[surface] + source[run].offset;
            uint32_t remaining = source[run].length;
            while (remaining) {
                const uint64_t page = address & ~((uint64_t)PAGE_2M - 1);
                const struct sparse_state_entry *entry =
                    sparse_entry_for_page(state, page);
                if (!entry) {
                    fprintf(stderr,
                            "no sparse BAR1 mapping for VRAM page 0x%" PRIx64 "\n",
                            page);
                    exit(EXIT_FAILURE);
                }
                const uint32_t in_page = (uint32_t)(address - page);
                uint32_t length = PAGE_2M - in_page;
                if (length > remaining)
                    length = remaining;
                if (count == capacity) {
                    capacity *= 2;
                    struct colored_segment *larger = realloc(
                        result, capacity * sizeof(*result));
                    if (!larger)
                        die("realloc sparse colored segments");
                    result = larger;
                }
                result[count++] = (struct colored_segment) {
                    .offset = entry->pde_index * PAGE_2M + in_page,
                    .length = length,
                    .color = source[run].color,
                };
                address += length;
                remaining -= length;
            }
        }
    }
    free(source);
    *count_out = count;
    return result;
}

static inline void fill_segment(volatile uint8_t *destination, uint32_t bytes,
                                uint32_t color, bool wc)
{
    uintptr_t ptr = (uintptr_t)destination;
    const uint64_t color64 = (uint64_t)color | ((uint64_t)color << 32);
    if (wc) {
        while (bytes && (ptr & 7u)) {
            *(volatile uint32_t *)ptr = color;
            ptr += 4;
            bytes -= 4;
        }
#ifdef __AVX512F__
        while (bytes >= 8 && (ptr & 63u)) {
            _mm_stream_si64((long long *)ptr, (long long)color64);
            ptr += 8;
            bytes -= 8;
        }
        const __m512i colors512 = _mm512_set1_epi32((int)color);
        while (bytes >= 64) {
            _mm512_stream_si512((void *)ptr, colors512);
            ptr += 64;
            bytes -= 64;
        }
#endif
        while (bytes >= 8 && (ptr & 31u)) {
            _mm_stream_si64((long long *)ptr, (long long)color64);
            ptr += 8;
            bytes -= 8;
        }
        const __m256i colors = _mm256_set1_epi32((int)color);
        while (bytes >= 32) {
            _mm256_stream_si256((__m256i *)ptr, colors);
            ptr += 32;
            bytes -= 32;
        }
        while (bytes >= 8) {
            _mm_stream_si64((long long *)ptr, (long long)color64);
            ptr += 8;
            bytes -= 8;
        }
        if (bytes)
            *(volatile uint32_t *)ptr = color;
        return;
    }

    while (bytes && (ptr & 31u)) {
        *(volatile uint32_t *)ptr = color;
        ptr += 4;
        bytes -= 4;
    }
    const __m256i colors = _mm256_set1_epi32((int)color);
    while (bytes >= 32) {
        _mm256_store_si256((__m256i *)ptr, colors);
        ptr += 32;
        bytes -= 32;
    }
    while (bytes) {
        *(volatile uint32_t *)ptr = color;
        ptr += 4;
        bytes -= 4;
    }
}

static void signal_handler(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void configure_realtime(const struct options *opt)
{
    if (opt->cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(opt->cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) < 0)
            die("sched_setaffinity");
    }
    if (opt->rt_priority > 0) {
        struct sched_param param = {.sched_priority = opt->rt_priority};
        if (sched_setscheduler(0, SCHED_FIFO, &param) < 0)
            die("sched_setscheduler");
    }
}

static inline uint64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) +
           (uint64_t)now.tv_nsec;
}

static void render(const struct options *opt)
{
    if (!opt->surface_count || !opt->span || !opt->hz)
        die("render requires surfaces, span, and nonzero hz");
    struct device dev;
    device_open(&dev, opt, true);
    struct map_state_header state;
    struct map_state_entry *state_entries = state_read(opt->state_path, &state);
    free(state_entries);
    uint32_t live_block = mmio_read32(dev.bar0, (size_t)dev.hw.bar1_block);
    uint64_t live_instance = ((uint64_t)live_block & 0x0fffffffu) << 12;
    if (!(live_block & 0x80000000u) || live_instance != state.instance ||
        opt->bar1_va != state.bar1_va || opt->vram_base != state.vram_base ||
        opt->span != state.span) {
        fprintf(stderr, "saved BAR1 mapping is stale or does not match render arguments\n");
        exit(EXIT_FAILURE);
    }
    for (size_t i = 0; i < opt->surface_count; ++i) {
        if (opt->surfaces[i] < opt->vram_base ||
            opt->surfaces[i] + 0x870000 > opt->vram_base + opt->span) {
            fprintf(stderr, "surface 0x%" PRIx64 " is outside mapped VRAM span\n",
                    opt->surfaces[i]);
            exit(EXIT_FAILURE);
        }
    }
    size_t segment_count;
    struct segment *segments = build_segments(opt, &segment_count);
    configure_realtime(opt);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t period = UINT64_C(1000000000) / opt->hz;
    uint64_t frames = 0;
    struct timespec start = next;
    printf("rendering %zu coalesced runs to %zu surfaces at %u Hz via %s BAR1\n",
           segment_count, opt->surface_count, opt->hz,
           dev.bar1_wc ? "write-combining" : "uncached VFIO");
    fflush(stdout);
    while (!stop_requested) {
        for (size_t surface = 0; surface < opt->surface_count; ++surface) {
            uint64_t translated = opt->bar1_va + opt->surfaces[surface] - opt->vram_base;
            volatile uint8_t *base = dev.bar1 + translated;
            for (size_t run = 0; run < segment_count; ++run)
                fill_segment(base + segments[run].offset, segments[run].length,
                             opt->color, dev.bar1_wc);
        }
        _mm_sfence();
        ++frames;
        if (opt->duration_ms) {
            uint64_t elapsed = frames * period;
            if (elapsed >= (uint64_t)opt->duration_ms * 1000000)
                break;
        }
        uint64_t ns = (uint64_t)next.tv_nsec + period;
        next.tv_sec += (time_t)(ns / UINT64_C(1000000000));
        next.tv_nsec = (long)(ns % UINT64_C(1000000000));
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR &&
               !stop_requested) {}
    }
    struct timespec finish;
    clock_gettime(CLOCK_MONOTONIC, &finish);
    double seconds = (double)(finish.tv_sec - start.tv_sec) +
                     (double)(finish.tv_nsec - start.tv_nsec) / 1e9;
    printf("frames=%" PRIu64 " elapsed=%.6f effective_hz=%.1f\n",
           frames, seconds, seconds > 0.0 ? (double)frames / seconds : 0.0);
    free(segments);
    device_close(&dev);
}

static void render_sparse(const struct options *opt)
{
    if (!opt->surface_count || !opt->hz)
        die("render-sparse requires surfaces and nonzero hz");
    struct sparse_state state = sparse_state_read(opt->state_path);
    validate_sparse_surfaces(opt, &state);
    struct device dev;
    device_open(&dev, opt, true);
    validate_sparse_hierarchy(&dev, &state, NULL, NULL);
    validate_sparse_pdes(&dev, &state);

    size_t source_count, draw_count;
    struct segment *source = build_segments(opt, &source_count);
    struct segment *draw = build_sparse_draw_segments(
        opt, &state, source, source_count, &draw_count);
    free(source);

    configure_realtime(opt);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t period = UINT64_C(1000000000) / opt->hz;
    uint64_t frames = 0;
    struct timespec start = next;
    printf("rendering %zu pretranslated runs to %zu process-owned surfaces "
           "at %u Hz via %s BAR1\n",
           draw_count, opt->surface_count, opt->hz,
           dev.bar1_wc ? "write-combining" : "uncached VFIO");
    fflush(stdout);
    while (!stop_requested) {
        for (size_t run = 0; run < draw_count; ++run)
            fill_segment(dev.bar1 + draw[run].offset, draw[run].length,
                         opt->color, dev.bar1_wc);
        _mm_sfence();
        ++frames;
        if (opt->duration_ms &&
            frames * period >= (uint64_t)opt->duration_ms * 1000000)
            break;
        uint64_t ns = (uint64_t)next.tv_nsec + period;
        next.tv_sec += (time_t)(ns / UINT64_C(1000000000));
        next.tv_nsec = (long)(ns % UINT64_C(1000000000));
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR &&
               !stop_requested) {}
    }
    struct timespec finish;
    clock_gettime(CLOCK_MONOTONIC, &finish);
    double seconds = (double)(finish.tv_sec - start.tv_sec) +
                     (double)(finish.tv_nsec - start.tv_nsec) / 1e9;
    printf("frames=%" PRIu64 " elapsed=%.6f effective_hz=%.1f\n",
           frames, seconds, seconds > 0.0 ? (double)frames / seconds : 0.0);
    free(draw);
    sparse_state_free(&state);
    device_close(&dev);
}

struct box_stream_reader {
    uint8_t bytes[sizeof(struct tu104_box_frame)];
    size_t used;
};

struct box_build_worker {
    const struct options *opt;
    const struct sparse_state *state;
    const uint32_t *offset_lut;
    pthread_mutex_t mutex;
    struct colored_segment *pending;
    size_t pending_count;
    bool pending_ready;
    atomic_bool done;
    uint64_t stream_updates;
    uint64_t geometry_builds;
    uint64_t memoized_frames;
    uint64_t sequence_drops;
    uint64_t last_sequence;
    uint64_t build_total_ns;
    uint64_t build_max_ns;
    struct tu104_box_frame last_geometry;
    bool have_last_geometry;
};

static bool same_box_geometry(const struct tu104_box_frame *left,
                              const struct tu104_box_frame *right)
{
    return left->count == right->count &&
           memcmp(left->boxes, right->boxes,
                  (size_t)left->count * sizeof(left->boxes[0])) == 0;
}

static bool read_latest_box_frame(struct box_stream_reader *reader, int fd,
                                  struct tu104_box_frame *frame, bool *eof)
{
    bool updated = false;
    *eof = false;
    for (;;) {
        ssize_t result = read(fd, reader->bytes + reader->used,
                              sizeof(reader->bytes) - reader->used);
        if (result > 0) {
            reader->used += (size_t)result;
            if (reader->used != sizeof(reader->bytes))
                continue;
            struct tu104_box_frame candidate;
            memcpy(&candidate, reader->bytes, sizeof(candidate));
            reader->used = 0;
            if (candidate.magic != TU104_BOX_STREAM_MAGIC ||
                candidate.version != TU104_BOX_STREAM_VERSION ||
                candidate.count > TU104_BOX_STREAM_MAX_BOXES) {
                fprintf(stderr, "ignored invalid box-stream frame\n");
                continue;
            }
            *frame = candidate;
            updated = true;
            continue;
        }
        if (result == 0) {
            if (reader->used)
                fprintf(stderr, "box stream ended with a partial frame\n");
            *eof = true;
            return updated;
        }
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return updated;
        die("read box stream");
    }
}

static void *box_build_thread(void *opaque)
{
    struct box_build_worker *worker = opaque;
    struct box_stream_reader reader = {0};
    struct tu104_box_frame frame = {
        .magic = TU104_BOX_STREAM_MAGIC,
        .version = TU104_BOX_STREAM_VERSION,
    };
    struct pollfd descriptor = {
        .fd = worker->opt->rect_stream_fd,
        .events = POLLIN | POLLHUP,
    };

    while (!stop_requested) {
        int polled = poll(&descriptor, 1, 100);
        if (polled < 0) {
            if (errno == EINTR)
                continue;
            die("poll box stream");
        }
        if (!polled)
            continue;

        bool eof = false;
        if (read_latest_box_frame(&reader, descriptor.fd, &frame, &eof)) {
            ++worker->stream_updates;
            if (worker->last_sequence &&
                frame.sequence > worker->last_sequence + 1)
                worker->sequence_drops +=
                    frame.sequence - worker->last_sequence - 1;
            worker->last_sequence = frame.sequence;

            if (worker->have_last_geometry &&
                same_box_geometry(&frame, &worker->last_geometry)) {
                ++worker->memoized_frames;
                if (eof)
                    break;
                continue;
            }

            const uint64_t build_start = monotonic_ns();
            size_t candidate_count = 0;
            struct colored_segment *candidate = build_box_draw_segments(
                worker->opt, worker->state, worker->offset_lut, &frame,
                &candidate_count);
            const uint64_t build_ns = monotonic_ns() - build_start;
            worker->build_total_ns += build_ns;
            if (build_ns > worker->build_max_ns)
                worker->build_max_ns = build_ns;
            ++worker->geometry_builds;

            if (!candidate && frame.count) {
                fprintf(stderr,
                        "ignored box frame %" PRIu64 " with invalid geometry\n",
                        frame.sequence);
            } else {
                worker->last_geometry = frame;
                worker->have_last_geometry = true;
                pthread_mutex_lock(&worker->mutex);
                free(worker->pending);
                worker->pending = candidate;
                worker->pending_count = candidate_count;
                worker->pending_ready = true;
                pthread_mutex_unlock(&worker->mutex);
            }
        }
        if (eof)
            break;
    }
    atomic_store_explicit(&worker->done, true, memory_order_release);
    return NULL;
}

static void render_box_stream(const struct options *opt)
{
    if (!opt->surface_count || !opt->hz || opt->rect_stream_fd < 0)
        die("render-boxes requires surfaces, nonzero hz, and a stream fd");
    struct sparse_state state = sparse_state_read(opt->state_path);
    validate_sparse_surfaces(opt, &state);
    struct device dev;
    device_open(&dev, opt, true);
    validate_sparse_hierarchy(&dev, &state, NULL, NULL);
    validate_sparse_pdes(&dev, &state);
    uint32_t *offset_lut = build_blocklinear_offset_lut(opt);

    int flags = fcntl(opt->rect_stream_fd, F_GETFL);
    if (flags < 0 || fcntl(opt->rect_stream_fd, F_SETFL, flags | O_NONBLOCK) < 0)
        die("configure box stream");
    configure_realtime(opt);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    struct box_build_worker worker = {
        .opt = opt,
        .state = &state,
        .offset_lut = offset_lut,
    };
    if (pthread_mutex_init(&worker.mutex, NULL) != 0)
        die("initialize box worker mutex");
    atomic_init(&worker.done, false);
    pthread_t worker_thread;
    int thread_result = pthread_create(
        &worker_thread, NULL, box_build_thread, &worker);
    if (thread_result != 0) {
        errno = thread_result;
        die("create box build thread");
    }
    struct colored_segment *draw = NULL;
    size_t draw_count = 0;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t period = UINT64_C(1000000000) / opt->hz;
    uint64_t frames = 0;
    uint64_t draw_total_ns = 0, draw_max_ns = 0;
    uint64_t deadline_misses = 0;
    struct timespec start = next;
    printf("waiting for box frames; rendering at %u Hz via %s BAR1\n",
           opt->hz, dev.bar1_wc ? "write-combining" : "uncached VFIO");
    fflush(stdout);

    while (!stop_requested) {
        int lock_result = pthread_mutex_trylock(&worker.mutex);
        if (lock_result == 0) {
            if (worker.pending_ready) {
                free(draw);
                draw = worker.pending;
                draw_count = worker.pending_count;
                worker.pending = NULL;
                worker.pending_count = 0;
                worker.pending_ready = false;
            }
            pthread_mutex_unlock(&worker.mutex);
        } else if (lock_result != EBUSY) {
            errno = lock_result;
            die("lock box worker result");
        }
        if (atomic_load_explicit(&worker.done, memory_order_acquire))
            break;
        const uint64_t draw_start = monotonic_ns();
        for (size_t run = 0; run < draw_count; ++run)
            fill_segment(dev.bar1 + draw[run].offset, draw[run].length,
                         draw[run].color, dev.bar1_wc);
        _mm_sfence();
        const uint64_t draw_ns = monotonic_ns() - draw_start;
        draw_total_ns += draw_ns;
        if (draw_ns > draw_max_ns)
            draw_max_ns = draw_ns;
        ++frames;
        uint64_t ns = (uint64_t)next.tv_nsec + period;
        next.tv_sec += (time_t)(ns / UINT64_C(1000000000));
        next.tv_nsec = (long)(ns % UINT64_C(1000000000));
        if (monotonic_ns() > (uint64_t)next.tv_sec * UINT64_C(1000000000) +
                                 (uint64_t)next.tv_nsec)
            ++deadline_misses;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR &&
               !stop_requested) {}
    }
    struct timespec finish;
    clock_gettime(CLOCK_MONOTONIC, &finish);
    pthread_join(worker_thread, NULL);
    double seconds = (double)(finish.tv_sec - start.tv_sec) +
                     (double)(finish.tv_nsec - start.tv_nsec) / 1e9;
    printf("box stream stopped: frames=%" PRIu64 " elapsed=%.6f "
           "effective_hz=%.1f updates=%" PRIu64 " builds=%" PRIu64
           " memo_hits=%" PRIu64 " sequence_drops=%" PRIu64
           " deadline_misses=%" PRIu64 " build_avg_us=%.2f build_max_us=%.2f "
           "draw_avg_us=%.2f draw_max_us=%.2f\n",
           frames, seconds, seconds > 0.0 ? (double)frames / seconds : 0.0,
           worker.stream_updates, worker.geometry_builds, worker.memoized_frames,
           worker.sequence_drops, deadline_misses,
           worker.geometry_builds ?
               (double)worker.build_total_ns / (double)worker.geometry_builds / 1e3 : 0.0,
           (double)worker.build_max_ns / 1e3,
           frames ? (double)draw_total_ns / (double)frames / 1e3 : 0.0,
           (double)draw_max_ns / 1e3);
    free(draw);
    free(worker.pending);
    free(offset_lut);
    pthread_mutex_destroy(&worker.mutex);
    sparse_state_free(&state);
    device_close(&dev);
}

static void usage(FILE *stream)
{
    fprintf(stream,
        "usage: tu104-bar1-overlay COMMAND [options]\n"
        "commands: probe-hardware, install, verify, restore, render, install-sparse, "
        "verify-sparse, restore-sparse, render-sparse, render-boxes\n"
        "common: --pid N --vfio-fd N --state PATH --bar1-va N --vram-base N --span N\n"
        "sparse: --surface N (repeat) --surface-size N\n"
        "render: --surface N (repeat) --rect X,Y,W,H --color AARRGGBB --hz N\n"
        "        [--frame-width N --frame-height N --pitch-blocks N --log2-gobs-y N]\n"
        "        [--duration-ms N] [--cpu N] [--rt-priority N] [--wc-path PATH]\n"
        "hardware descriptor: --pbus-bar0-window N --pramin-offset N --pramin-size N\n"
        "        --bar1-block N --bar1-bind-status N --mmu-invalidate-pdb N\n"
        "        --mmu-invalidate-upper-pdb N --mmu-invalidate N\n"
        "        --instance-pdb-offset N --instance-limit-offset N\n"
        "        --page-table-levels N\n");
}

enum hardware_option {
    OPT_PBUS_BAR0_WINDOW = 1000,
    OPT_PCI_DEVICE_ID,
    OPT_CHIPSET,
    OPT_PRAMIN_OFFSET,
    OPT_PRAMIN_SIZE,
    OPT_BAR1_BLOCK,
    OPT_BAR1_BIND_STATUS,
    OPT_MMU_INVALIDATE_PDB,
    OPT_MMU_INVALIDATE_UPPER_PDB,
    OPT_MMU_INVALIDATE,
    OPT_INSTANCE_PDB_OFFSET,
    OPT_INSTANCE_LIMIT_OFFSET,
    OPT_PAGE_TABLE_LEVELS,
};

int main(int argc, char **argv)
{
    if (atexit(emergency_restore_pramin) != 0)
        die("atexit");
    if (argc < 2) {
        usage(stderr);
        return EXIT_FAILURE;
    }
    const char *command = argv[1];
    struct options opt = {
        .vfio_fd = 26,
        .wc_path = "/dev/tu104-bar1-wc",
        .state_path = "/run/tu104-bar1-overlay.state",
        .surface_size = 0x870000,
        .frame_width = 1920, .frame_height = 1080,
        .pitch_blocks = 120, .log2_gobs_y = 4,
        .x = 900, .y = 450, .width = 96, .height = 96,
        .color = 0xffff00ffu, .hz = 2000, .cpu = -1,
        .rect_stream_fd = -1,
        .hw = {
            .pci_device_id = 0x1e81,
            .chipset = 0x164,
            .pbus_bar0_window = 0x1700,
            .pramin_offset = 0x700000,
            .pramin_size = 0x100000,
            .bar1_block = 0xb80f40,
            .bar1_bind_status = 0xb80f50,
            .mmu_invalidate_pdb = 0xb830a0,
            .mmu_invalidate_upper_pdb = 0xb830a4,
            .mmu_invalidate = 0xb830b0,
            .instance_pdb_offset = 0x200,
            .instance_limit_offset = 0x208,
            .page_table_levels = 3,
        },
    };
    static const struct option long_options[] = {
        {"pid", required_argument, NULL, 'p'},
        {"vfio-fd", required_argument, NULL, 'f'},
        {"state", required_argument, NULL, 's'},
        {"bar1-va", required_argument, NULL, 'b'},
        {"vram-base", required_argument, NULL, 'v'},
        {"span", required_argument, NULL, 'n'},
        {"surface", required_argument, NULL, 'u'},
        {"surface-size", required_argument, NULL, 'i'},
        {"frame-width", required_argument, NULL, 'j'},
        {"frame-height", required_argument, NULL, 'k'},
        {"pitch-blocks", required_argument, NULL, 'l'},
        {"log2-gobs-y", required_argument, NULL, 'g'},
        {"rect", required_argument, NULL, 'r'},
        {"color", required_argument, NULL, 'c'},
        {"hz", required_argument, NULL, 'z'},
        {"duration-ms", required_argument, NULL, 'd'},
        {"cpu", required_argument, NULL, 'a'},
        {"rt-priority", required_argument, NULL, 't'},
        {"wc-path", required_argument, NULL, 'w'},
        {"rect-stream-fd", required_argument, NULL, 'e'},
        {"pbus-bar0-window", required_argument, NULL, OPT_PBUS_BAR0_WINDOW},
        {"pci-device-id", required_argument, NULL, OPT_PCI_DEVICE_ID},
        {"chipset", required_argument, NULL, OPT_CHIPSET},
        {"pramin-offset", required_argument, NULL, OPT_PRAMIN_OFFSET},
        {"pramin-size", required_argument, NULL, OPT_PRAMIN_SIZE},
        {"bar1-block", required_argument, NULL, OPT_BAR1_BLOCK},
        {"bar1-bind-status", required_argument, NULL, OPT_BAR1_BIND_STATUS},
        {"mmu-invalidate-pdb", required_argument, NULL, OPT_MMU_INVALIDATE_PDB},
        {"mmu-invalidate-upper-pdb", required_argument, NULL,
         OPT_MMU_INVALIDATE_UPPER_PDB},
        {"mmu-invalidate", required_argument, NULL, OPT_MMU_INVALIDATE},
        {"instance-pdb-offset", required_argument, NULL, OPT_INSTANCE_PDB_OFFSET},
        {"instance-limit-offset", required_argument, NULL, OPT_INSTANCE_LIMIT_OFFSET},
        {"page-table-levels", required_argument, NULL, OPT_PAGE_TABLE_LEVELS},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    optind = 2;
    int option;
    while ((option = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (option) {
        case 'p': opt.pid = (pid_t)parse_u64(optarg); break;
        case 'f': opt.vfio_fd = (int)parse_u64(optarg); break;
        case 's': opt.state_path = optarg; break;
        case 'b': opt.bar1_va = parse_u64(optarg); break;
        case 'v': opt.vram_base = parse_u64(optarg); break;
        case 'n': opt.span = parse_u64(optarg); break;
        case 'u':
            if (opt.surface_count == MAX_SURFACES)
                die("too many surfaces");
            opt.surfaces[opt.surface_count++] = parse_u64(optarg);
            break;
        case 'i': opt.surface_size = parse_u64(optarg); break;
        case 'j': opt.frame_width = (unsigned)parse_u64(optarg); break;
        case 'k': opt.frame_height = (unsigned)parse_u64(optarg); break;
        case 'l': opt.pitch_blocks = (unsigned)parse_u64(optarg); break;
        case 'g': opt.log2_gobs_y = (unsigned)parse_u64(optarg); break;
        case 'r':
            if (sscanf(optarg, "%u,%u,%u,%u", &opt.x, &opt.y,
                       &opt.width, &opt.height) != 4)
                die("invalid rectangle");
            break;
        case 'c': opt.color = (uint32_t)parse_u64(optarg); break;
        case 'z': opt.hz = (unsigned)parse_u64(optarg); break;
        case 'd': opt.duration_ms = (unsigned)parse_u64(optarg); break;
        case 'a': opt.cpu = (int)parse_u64(optarg); break;
        case 't': opt.rt_priority = (int)parse_u64(optarg); break;
        case 'w': opt.wc_path = optarg; break;
        case 'e': opt.rect_stream_fd = (int)parse_u64(optarg); break;
        case OPT_PBUS_BAR0_WINDOW: opt.hw.pbus_bar0_window = parse_u64(optarg); break;
        case OPT_PCI_DEVICE_ID: opt.hw.pci_device_id = (uint32_t)parse_u64(optarg); break;
        case OPT_CHIPSET: opt.hw.chipset = (uint32_t)parse_u64(optarg); break;
        case OPT_PRAMIN_OFFSET: opt.hw.pramin_offset = parse_u64(optarg); break;
        case OPT_PRAMIN_SIZE: opt.hw.pramin_size = parse_u64(optarg); break;
        case OPT_BAR1_BLOCK: opt.hw.bar1_block = parse_u64(optarg); break;
        case OPT_BAR1_BIND_STATUS: opt.hw.bar1_bind_status = parse_u64(optarg); break;
        case OPT_MMU_INVALIDATE_PDB: opt.hw.mmu_invalidate_pdb = parse_u64(optarg); break;
        case OPT_MMU_INVALIDATE_UPPER_PDB:
            opt.hw.mmu_invalidate_upper_pdb = parse_u64(optarg);
            break;
        case OPT_MMU_INVALIDATE: opt.hw.mmu_invalidate = parse_u64(optarg); break;
        case OPT_INSTANCE_PDB_OFFSET:
            opt.hw.instance_pdb_offset = parse_u64(optarg);
            break;
        case OPT_INSTANCE_LIMIT_OFFSET:
            opt.hw.instance_limit_offset = parse_u64(optarg);
            break;
        case OPT_PAGE_TABLE_LEVELS:
            opt.hw.page_table_levels = (unsigned)parse_u64(optarg);
            break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return EXIT_FAILURE;
        }
    }
    if (geteuid() != 0 || opt.pid <= 0) {
        fprintf(stderr, "run through /bin/csu and provide --pid\n");
        return EXIT_FAILURE;
    }
    if (!strcmp(command, "probe-hardware"))
        probe_hardware(&opt);
    else if (!strcmp(command, "install"))
        install_mapping(&opt);
    else if (!strcmp(command, "verify"))
        verify_mapping(&opt);
    else if (!strcmp(command, "restore"))
        restore_mapping(&opt);
    else if (!strcmp(command, "render"))
        render(&opt);
    else if (!strcmp(command, "install-sparse"))
        install_sparse_mapping(&opt);
    else if (!strcmp(command, "verify-sparse"))
        verify_sparse_mapping(&opt);
    else if (!strcmp(command, "restore-sparse"))
        restore_sparse_mapping(&opt);
    else if (!strcmp(command, "render-sparse"))
        render_sparse(&opt);
    else if (!strcmp(command, "render-boxes"))
        render_box_stream(&opt);
    else {
        usage(stderr);
        return EXIT_FAILURE;
    }
    return 0;
}
