// The kernel's reports for SYS_KINFO, as the text the shell prints.
#include "kernel.h"
#include "task.h"
#include "mm.h"
#include "fs.h"
#include "drivers.h"
#include "net.h"
#include "ringtail/abi.h"

int kinfo_format(int what, sbuf_t* sb) {
    switch (what) {
    case KINFO_VERSION:
        sbprintf(sb, "RingTail OS v%s \"%s\"\n", RINGTAIL_VERSION, RINGTAIL_CODENAME);
        return 0;
    case KINFO_MEM: {
        uint32_t total, free;
        frame_stats(&total, &free);
        heap_stats_t h;
        heap_get_stats(&h);
        sbprintf(sb, "Frames: %u KB free of %u KB\n", free * 4, total * 4);
        sbprintf(sb, "Heap:   %u KB free of %u KB\n", h.free / 1024, h.total / 1024);
        return 0;
    }
    case KINFO_HEAP: {
        heap_stats_t h;
        heap_get_stats(&h);
        sbprintf(sb, "Heap at %p: %u KB total, %u bytes used, %u free in %u blocks (largest %u)\n",
                 (void*)h.start, h.total / 1024, h.used, h.free, h.free_blocks, h.largest_free);
        return 0;
    }
    case KINFO_TASKS:
        task_format(sb);
        return 0;
    case KINFO_FAULTS:
        faultlog_format(sb);
        return 0;
    case KINFO_MMAP:
        for (uint32_t i = 0; i < boot_info.mmap_count; i++) {
            const mmap_entry_t* e = &boot_info.mmap[i];
            uint64_t end = e->base + e->len;
            sbprintf(sb, "  %08x-%08x  %s\n", (uint32_t)e->base, (uint32_t)(end - 1),
                     e->type == 1 ? "usable" : "reserved");
        }
        return 0;
    case KINFO_DISK: {
        if (!diskfs_ready()) return -EIO;
        uint32_t clusters, free, cb;
        diskfs_stats(&clusters, &free, &cb);
        sbprintf(sb, "%u KB free of %u KB\n", free * (cb / 512) / 2, clusters * (cb / 512) / 2);
        return 0;
    }
    case KINFO_NET:
        net_format(sb);
        return 0;
    case KINFO_PCI:
        pci_format(sb);
        return 0;
    }
    return -EINVAL;
}
