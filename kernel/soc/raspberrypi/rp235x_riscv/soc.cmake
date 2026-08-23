# Ahura SoC package: Raspberry Pi RP2350 and RP2354, Hazard3 RISC-V cores.
#
#     set(AHURA_SOC raspberrypi/rp235x_riscv)      # before add_subdirectory(AhuraRTOS/kernel)
#
# The sibling of raspberrypi/rp235x_arm: same chip, other core. They are separate packages rather
# than one with an #if because almost everything the kernel asks of the silicon is answered
# differently - the context-switch trap, the tick, the idle instruction and the fault report have no
# shared implementation between a Cortex-M33 and a Hazard3, only a shared chip underneath.
#
# Requires pico_sdk_init() to have run first, as every SDK project does near the top of its
# CMakeLists.txt.

set(AHURA_SOC_NAME "Raspberry Pi RP2350 / RP2354, Hazard3 RISC-V cores")

# The core folder under arch/riscv/. Stated here because the Pico SDK builds RISC-V with -march
# rather than -mcpu, so the kernel's own -mcpu parsing finds nothing to go on. (CMAKE_SYSTEM_PROCESSOR
# is also "hazard3" in the SDK's toolchain file, so this is belt and braces - but a package stating
# its own core is the reliable answer and costs one line.)
set(AHURA_SOC_ARCH hazard3)

# There is no PendSV on RISC-V and therefore no AHURA_SOC_PENDSV_HANDLER. The vector the kernel owns
# is the machine software interrupt, trap cause 3, which crt0_riscv.S names isr_riscv_machine_soft_irq
# and declares weak so exactly this can happen. That name is the port's default, so nothing to set.

# Route the kernel spinlock to the SDK's spin_lock API rather than the port's own lr.w/sc.w, for the
# same reason as on the Arm side: the SDK carries the errata workarounds for these locks, and going
# through it means the kernel inherits them rather than keeping a second copy.
set(AHURA_SOC_SPINLOCK_BACKEND 1)

# Put the context-switch handler in RAM.
#
# The SDK places the vectored mtvec table in .data (RAM) unless PICO_NO_RAM_VECTOR_TABLE is set,
# while code runs from flash - and on this chip those are 256MB apart. Each table entry is a JAL,
# which reaches +/-1MB, so a handler left in .text does not link: "relocation truncated to fit".
#
# .time_critical is the SDK's RAM-code section, the same one __not_in_flash_func() uses, so the
# handler ends up near the table. It is also where an RTOS wants its context switch anyway: it is
# the hottest path in the kernel and this stops it paying XIP latency on every switch.
set(AHURA_SOC_COMPILE_DEFINITIONS
	"OS_CONFIG_ARCH_SWI_SECTION=\".time_critical.ahura_switch\""
)

set(AHURA_SOC_SOURCES
	"${CMAKE_CURRENT_LIST_DIR}/soc_cb.c"
)

# No public header: every entry point this package has is a _cb the kernel calls itself.
set(AHURA_SOC_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}")

# Only what the sources include. pico_stdlib is deliberately absent: it drags in stdio and its
# transports, which are the application's choice rather than the kernel's.
set(AHURA_SOC_LINK_LIBRARIES
	hardware_clocks
	hardware_irq
	hardware_riscv_platform_timer
	hardware_sync
	pico_multicore
	pico_platform
)

# Refuse a build for the wrong core rather than producing one that links and then misbehaves.
if(NOT PICO_PLATFORM MATCHES "riscv")
	message(FATAL_ERROR
		"AHURA_SOC=raspberrypi/rp235x_riscv selected with PICO_PLATFORM='${PICO_PLATFORM}'.\n"
		"This package is the Hazard3 RISC-V side of the RP235x. Build with PICO_PLATFORM=rp2350-riscv, "
		"or use raspberrypi/rp235x_arm for the Cortex-M33 side.")
endif()
