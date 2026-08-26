# Platforms

[← Documentation index](README.md)

AhuraRTOS is written to be multi-platform. The kernel core is
architecture-independent C, and each architecture is added as a port behind a
fixed interface - so supporting a new CPU means writing that port, not changing
the kernel.

---

## Available today

### ARM Cortex-M

ARMv6-M through ARMv8.1-M, covered by **three** shared port implementations:

| Architecture profile | Cortex-M cores | Port | TrustZone |
|---|---|---|---|
| ARMv6-M | M0, M0+ | `v6m` | No - the Security Extension is absent |
| ARMv7-M / ARMv7E-M | M3 / M4, M7 | `v7m` | No |
| ARMv8-M baseline | M23 | `v6m` | Yes, optional per device |
| ARMv8-M mainline | M33, M35P | `v8m` | Yes, optional per device |
| ARMv8.1-M | M52, M55, M85 | `v8m` | Yes, optional per device |

Eleven cores across three files is itself the evidence that the port interface
is small: what differs between them is the instruction set, not the kernel.
The Cortex-M1 (ARMv6-M, FPGA) is deliberately not supported.

### RISC-V

RV32, on the Hazard3 cores of the RP2350 / RP2354:

| Architecture | Cores | Port | Notes |
|---|---|---|---|
| RV32 | Hazard3 | `hazard3` | Machine software interrupt instead of PendSV, `mtime`/`mtimecmp` tick, `mhartid` core id |

The port exists to prove the boundary rather than to serve one chip: the kernel
above it did not change to gain a second instruction set. Dual-core SMP is
verified on silicon here as well as on the Arm side.

### Every vendor

**Every one of those cores is supported on every silicon vendor.** Nothing in
the kernel or the ports names a vendor, a family, or a HAL. The only
device-specific symbol anywhere is CMSIS `SystemCoreClock`, and that has a
documented one-line fallback for devices whose startup code omits it.

What differs between vendors is only their *tooling* - specifically whether the
code generator emits a competing `PendSV_Handler` - and
[Vendor notes](vendor-notes.md) covers it, vendor by vendor.

Parts that genuinely need silicon-specific glue - a non-CMSIS vector name,
inter-core signalling, hardware spinlocks - can have it packaged under
`soc/<vendor>/<family>/` instead of hand-written into every project. That
layer is optional and additive: with no package the kernel builds exactly as it
always has, which is what keeps the claim above true. See
[SoC packages](soc.md).

STM32 is the primary bring-up and testing target, because that is the hardware
on hand - not because anything in the kernel is STM32-specific.

### Toolchains

| Toolchain | Status |
|---|---|
| **GCC** (`arm-none-eabi-gcc`, 10 and later) | Supported. The primary build and test toolchain |
| **LLVM/Clang** for bare-metal Arm | Supported |
| **Arm Compiler 6** (`armclang`) | Supported |
| Arm Compiler 5 (`armcc`) | **Not** supported - end of life, and no GCC-style inline assembly |
| IAR EWARM (`iccarm`) | **Not** supported yet |

The dividing line is GCC-style inline assembly, which armclang and Clang both
implement and `armcc` / `iccarm` do not. The port layer needs it for the context
switch and the atomics; the entire `kernel/` tree is ordinary portable C11 and
would build anywhere. An IAR port is therefore a contained piece of work,
confined to four files, with nothing in `kernel/` changing.

## Planned

**Xtensa (ESP32)** is the next architecture. RISC-V was the first proof that
the port boundary holds against a different instruction set, and it is now in
the supported list above rather than this one.

Note that vendor families built on Cortex-M - NXP, TI, Nordic, Renesas and the
rest - are already covered by the ARM ports today: what a new port adds is a new
*instruction set*, not a new vendor. See the [roadmap](roadmap.md).

## Experimental

- **Tickless idle.** Real SysTick suppression is implemented on the ARMv8-M
  mainline port; the v6m and v7m ports still need the same change, and the idle
  task does not yet call into it - so the switch changes nothing at run time.
- **TrustZone.** Compiles, and the secure-context callbacks are wired, but it
  has never been exercised on a part with the Security Extension enabled.

Multi-core (SMP) is no longer on this list: it is verified on silicon on the
RP2350's Cortex-M33 pair, the same chip's Hazard3 pair, and the RP2040's
Cortex-M0+ pair. See [Pico SDK → running both
cores](pico-sdk.md#running-both-cores) for how to turn it on.

Both of the above are documented in full in [Platform support](porting.md).
