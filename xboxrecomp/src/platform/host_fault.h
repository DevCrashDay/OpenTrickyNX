/*
 * host_fault.h - trapped MMIO apertures on POSIX and the Switch.
 *
 * A few Xbox hardware register blocks (the AC'97 interface, the MCPX APU, the
 * DAC palette page) need device semantics on every access, not plain memory.
 * On Windows each block's page is protected and a vectored exception handler
 * decodes the faulting x86-64 instruction (apu_mmio_hook.c, aci_mmio.c,
 * nv2a_mmio_hook.c). This is the same mechanism for the other hosts: a range
 * registered here is protected, and a fault inside it is decoded -- x86-64 on
 * a Linux PC, AArch64 on the Switch or an ARM Linux machine -- performed
 * through the range's handler, and the faulting instruction is skipped.
 *
 * Only ordinary loads and stores are decoded, which is what the generated
 * code's volatile MEM8/16/32 accesses compile to. Anything else in a trapped
 * range is reported and left to crash, as on Windows.
 */
#ifndef HOST_FAULT_H
#define HOST_FAULT_H

#if !defined(_WIN32)

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One access to a trapped range. `guest_va` is the Xbox address; `size` is
 * 1, 2, 4 or 8. A read stores the value in *val. Returns false if the
 * device declines the access (the fault then propagates). */
typedef bool (*host_mmio_fn)(uint32_t guest_va, unsigned size, int is_write,
                             uint64_t *val);

/* Protect [host, host + size) -- page aligned -- and route faults in it to
 * `fn`, reporting addresses as guest VAs from `guest_va` up. Installs the
 * fault handler on first use. */
bool host_trap_register(void *host, uint32_t guest_va, size_t size, host_mmio_fn fn);

/* Find the range covering a faulting host address and perform the access.
 * Used by the per-host fault entry points; exposed for tests. */
bool host_trap_access(uintptr_t fault_addr, unsigned size, int is_write, uint64_t *val);

/* Is this host address inside a registered range? */
bool host_trap_covers(uintptr_t addr);

#ifdef __cplusplus
}
#endif

#endif /* !_WIN32 */
#endif /* HOST_FAULT_H */
