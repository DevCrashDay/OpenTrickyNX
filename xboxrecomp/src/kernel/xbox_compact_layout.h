/*
 * xbox_compact_layout.h - the guest address space on hosts without the
 * Windows mirror layout (Linux, Nintendo Switch).
 *
 * On Windows the 140 MB of guest RAM is a file mapping viewed again every
 * 140 MB up to Xbox VA 0xF5000000, and the 48 MB hardware aperture sits at a
 * fixed native address 0xFD000000 above the base (xbox_memory_layout.c). That
 * needs about 4 GB of address space at a fixed distance from the base, and
 * aliased views of one mapping.
 *
 * The Switch has neither: an application cannot map the same memory twice,
 * and the region svcMapMemory maps into is smaller than 4 GB. So here the
 * guest is one contiguous host block instead:
 *
 *     host offset 0              guest RAM, Xbox VA 0x00000000-0x08BFFFFF
 *     host offset RAM size       hardware aperture, Xbox VA 0xFD000000-0xFFFFFFFF
 *
 * and every guest VA is folded onto it: RAM addresses map to themselves, the
 * aperture to the slot after RAM, and everything in between wraps modulo the
 * RAM size -- the same bytes the Windows mirror views alias. The uncached
 * (0x80000000) and write-combined (0xF0000000) aliases are folded before this,
 * in xbox_resolve_uncached_alias (recomp_types.h), exactly as on Windows.
 *
 * The fold costs nothing on the hot path: ordinary RAM addresses already take
 * one compare there, and only addresses outside RAM reach the slow path.
 */
#ifndef XBOX_COMPACT_LAYOUT_H
#define XBOX_COMPACT_LAYOUT_H

#include <stdint.h>
#include <stddef.h>

#if !defined(XBOX_COMPACT_LAYOUT)
#if defined(_WIN32)
#define XBOX_COMPACT_LAYOUT 0
#else
#define XBOX_COMPACT_LAYOUT 1
#endif
#endif

/* Must equal XBOX_TOTAL_RAM (xbox_memory_layout.h checks it). */
#define XBOX_COMPACT_RAM_SIZE   (140u * 1024u * 1024u)
#define XBOX_HW_APERTURE_VA     0xFD000000u
#define XBOX_HW_APERTURE_SIZE   0x03000000u
#define XBOX_COMPACT_HOST_SIZE  ((size_t)XBOX_COMPACT_RAM_SIZE + XBOX_HW_APERTURE_SIZE)

/* Guest VA -> offset in the host block. */
static inline uint32_t xbox_compact_fold(uint32_t va)
{
    if (va < XBOX_COMPACT_RAM_SIZE) return va;
    if (va >= XBOX_HW_APERTURE_VA) return XBOX_COMPACT_RAM_SIZE + (va - XBOX_HW_APERTURE_VA);
    return va % XBOX_COMPACT_RAM_SIZE;
}

/* Offset in the host block -> the canonical guest VA (for fault reports). */
static inline uint32_t xbox_compact_unfold(uint32_t off)
{
    return off >= XBOX_COMPACT_RAM_SIZE ? XBOX_HW_APERTURE_VA + (off - XBOX_COMPACT_RAM_SIZE) : off;
}

extern ptrdiff_t g_xbox_mem_offset;

/* Host pointer for a guest VA, for host code that addresses the hardware
 * aperture directly (the PFIFO pump, the MMIO traps). Identity plus the base
 * offset on Windows, the fold above elsewhere. */
static inline void *xbox_host_ptr(uint32_t va)
{
#if XBOX_COMPACT_LAYOUT
    return (void *)((uintptr_t)g_xbox_mem_offset + xbox_compact_fold(va));
#else
    return (void *)((uintptr_t)g_xbox_mem_offset + va);
#endif
}

#endif /* XBOX_COMPACT_LAYOUT_H */
