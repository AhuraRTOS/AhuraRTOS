# Ahura SoC package: Raspberry Pi RP2040.
#
#     set(AHURA_SOC raspberrypi/rp2040)          # before add_subdirectory(AhuraRTOS/kernel)
#
# "rp" is Raspberry Pi's prefix for their CHIPS, and that is what this layer is about. It is
# deliberately not "rp2", which their SDK uses for the same grouping (src/rp2_common): standing
# alone as a directory name that reads as "Pico 2", and Pico 2 is a BOARD. The misreading would
# not be harmless - the RP2040 is the chip in the original Pico, so anyone parsing a board name
# would draw the wrong conclusion about what is supported.
#
# This package is the RP2040 alone. It is split from the RP235x because the two are different
# silicon generations, not two steppings: Cortex-M0+ against Cortex-M33, no Security Extension
# against TrustZone, and no doorbells against doorbells. There is no rp2040_riscv and never will
# be, which is why this one carries no architecture suffix.
#
# Almost everything else is shared and lives in ../common/soc_common.c, compiled into this package
# rather than into a library of its own: the core id, the spinlock, the CPU clock, the SysTick
# vector and booting core 1 are identical across RP2 chips because they are SIO or plain SDK.
#
# Requires pico_sdk_init() to have run first, which every SDK project does near the top of its
# CMakeLists.txt, well before it reaches the kernel.

set(AHURA_SOC_NAME "Raspberry Pi RP2040")

# The SDK's vector table names entry 14 isr_pendsv, not the CMSIS-Pack PendSV_Handler the kernel
# defaults to - see crt0.S, where it sits between isr_svcall and isr_systick. Both are weak stubs
# there, so the kernel's handler simply replaces it at link time. Without this line the kernel
# builds and links cleanly and then traps at os_start(), because its boot-time vector check reads
# entry 14 through VTOR and finds the SDK's breakpoint stub rather than its own handler.
set(AHURA_SOC_PENDSV_HANDLER isr_pendsv)

# Same story for SVC. The kernel does not use it, but the self-test suite installs a handler to
# reach ISR context, and crt0.S names that vector isr_svcall. With the CMSIS name the handler
# links but nothing points at it, so the suite's first `svc #0` lands on crt0.S's breakpoint and
# the board hangs mid-run with no output to say why.
set(AHURA_SOC_SVC_HANDLER isr_svcall)

# Route the kernel spinlock to the SDK's spin_lock API rather than the built-in LDREX/STREX
# backend, because delegating is strictly better than either built-in answer here.
#
# The Cortex-M0+ has no exclusive instructions at all, so the kernel's built-in backend cannot
# work here - the callback backend is the only option, and the SDK's SIO hardware spinlocks are
# what it routes to.
#
# Going through the SDK also means the kernel picks up any future erratum workaround automatically
# instead of carrying its own copy, and keeps both chips on a single code path.
set(AHURA_SOC_SPINLOCK_BACKEND 1)

set(AHURA_SOC_ARCH cortex_m0plus)

set(AHURA_SOC_SOURCES
	"${CMAKE_CURRENT_LIST_DIR}/../common/soc_common.c"
	"${CMAKE_CURRENT_LIST_DIR}/soc_cb.c"
)

# Only ../common, for soc_common.h. This package exposes no public header: every entry point
# it has is a _cb the kernel calls itself, so there is nothing for an application to include.
set(AHURA_SOC_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

# Only what the sources actually include. pico_stdlib is deliberately not here: it drags in stdio
# and its transports, which are the application's choice, not the kernel's.
# Forces soc_cb.c into the link. Every symbol it defines sits behind a #if and has a weak default
# in the kernel, so a build that turns those options off pulls the object from the archive not at
# all and loses the whole package to those defaults, silently. See the anchor in soc_cb.c.
set(AHURA_SOC_LINK_OPTIONS -u soc_rp2040_anchor)

set(AHURA_SOC_LINK_LIBRARIES
	hardware_clocks
	hardware_sync
	hardware_watchdog
	pico_multicore
	pico_platform
)

# Refuse a build for the wrong chip rather than producing one that links and then misbehaves.
if(PICO_PLATFORM AND NOT PICO_PLATFORM MATCHES "rp2040")
	message(FATAL_ERROR
		"AHURA_SOC=raspberrypi/rp2040 selected with PICO_PLATFORM='${PICO_PLATFORM}'.
"
		"Use raspberrypi/rp235x_arm for the RP2350 and RP2354.")
endif()
