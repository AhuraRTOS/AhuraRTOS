# SoC packages

Optional per-silicon support, selected with one CMake variable:

```cmake
set(AHURA_SOC raspberrypi/rp235x_arm)
```

A package answers the questions that are **facts about the chip** rather than
decisions about the product: which symbol the PendSV vector uses, how a core
learns its own index, how one core interrupts another, which construct excludes
reliably across cores. The kernel already had a hole shaped like this - the
`_cb` callback contract - and a package fills it so the application does not
have to.

Leaving `AHURA_SOC` unset is fully supported and is what every project did
before packages existed. The kernel builds exactly as it always has, and the
application supplies that group itself by copying
[`../template/soc_cb.c`](../template/soc_cb.c). That is what keeps every
unpackaged MCU usable, on any vendor.

## Layout

```
soc/<vendor>/<family>/
├── soc.cmake        # required: the build contract, read before ahura_kernel is finished
├── soc.h            # only if the package exposes something to call; most do not
│
└── ../common/       # optional: code shared by sibling packages of one vendor,
                     # compiled into whichever one the build selected
├── soc_cb.c         # optional: the SoC-owned callbacks
└── README.md        # what the package assumes and what it claims
```

Vendor directory names follow the standard vendor prefixes, so `raspberrypi`
and `infineon` rather than `rpi` and `infinion`. The family level is kept even
where one family is the vendor's whole line, so that `AHURA_SOC` values and the
CMake dispatch never need a special case.

## Packaged today

| `AHURA_SOC` | Parts | Contributes |
|---|---|---|
| [`raspberrypi/rp235x_arm`](raspberrypi/rp235x_arm/README.md) | RP2350, RP2354 - Arm cores (Pico 2) | `isr_pendsv`, `isr_systick`, `SystemCoreClock`, core id, doorbell IPI, SIO spinlocks |
| [`raspberrypi/rp2040`](raspberrypi/rp2040/README.md) | RP2040 (Pico, Pico W) | The same group, with the IPI on the SIO FIFO |
| [`st/stm32`](st/stm32/README.md) | Every STM32 | Nothing: CMSIS-Pack startup already satisfies the kernel. See its README |

## Writing one

The full contract - every variable `soc.cmake` may set, which callbacks a
package may own, and the one-definition linker rule that makes the split safe -
is in **[`../../doc/soc.md`](../../doc/soc.md)**.

The short version: start from `template/soc_cb.c` in an application, get the
target working, then move the finished file here beside a `soc.cmake`. A
package is that file plus its build contract, which is why porting to a new
part and packaging it are the same work done twice rather than two jobs.
