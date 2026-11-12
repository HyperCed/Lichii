# Hypervisor CPU backend (macOS, Apple Silicon)

`CPU > Backend > Hypervisor` (`cpu_backend=2`) runs 64-bit guest code on the host CPU through
Hypervisor.framework instead of recompiling it with Dynarmic. It exists only in arm64 macOS builds
(`HAS_HVF`). 32-bit programs, programs other than the application, and runs without fastmem keep
Dynarmic; the log says why. A run that uses it logs `CPU backend: Hypervisor.framework`.

The program must carry the `com.apple.security.hypervisor` entitlement
(`dist/eglantine.entitlements`). The build signs `lichii` and `eden-cli` ad hoc with it; without
it the VM is refused and Dynarmic is used.

## How it works

Code is in `src/core/arm/hvf/`.

- **One VM, one vCPU per host core thread.** `VirtualMachine` maps the emulated physical memory
  (the fastmem backing) into the guest at 4 GiB. The interfaces of a core share that thread's
  vCPU, and an interface's registers stay in the vCPU until another one needs it.
- **Guest page tables.** `AddressSpace` holds the process's stage 1 tables with 4 KiB pages,
  whatever the host's page size. `Memory` updates them wherever it updates the fastmem arena, so
  the GPU's page protection and read traps apply as they do with fastmem. Removing access shoots
  the TLBs of the vCPUs down before it returns.
- **A one-page guest kernel.** Guest code runs at EL0. Every exception it takes (SVC, abort,
  trapped register) lands on an EL1 vector that leaves the guest with `hvc`; the guest is entered
  again with `eret`, optionally after a TLB flush.
- **Faults.** A store to a page the GPU caches is interpreted through `Memory`, as Dynarmic's
  slow path does: the caches learn of the write and usually let the page go, so the stores after
  it run in the guest. A load brings the GPU's data through the slow read path, then runs in
  the guest; it is interpreted only where the page stays protected. Any other invalid access is
  logged and its instruction skipped. `EDEN_NX2_HVF_STATS=1` logs the exits per second by kind.

## Costs and limits

- Each SVC and each read of `CNTPCT_EL0`/`CNTFRQ_EL0` leaves the guest: about 1 µs on an M2,
  against 0.1 µs and 0.03 µs with Dynarmic. Plain code runs at host speed.
- Guest memory permissions are enforced: a write to read-only memory, which Dynarmic lets through,
  is skipped.
- No single-stepping, and no watchpoints.
