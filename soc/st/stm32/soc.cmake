# Ahura SoC package: STMicroelectronics STM32.
#
#     set(AHURA_SOC st/stm32)                 # before add_subdirectory(AhuraRTOS/kernel)
#
# STM32 asks less of a package than most parts, because CMSIS-Pack startup files already give the
# kernel what it needs: the PendSV vector carries the default name, SystemCoreClock exists and is
# maintained by the generated SystemInit(), and single-core parts have no core id, no inter-core
# IPI and no hardware spinlock to supply. So there is no vector name to state here and no spinlock
# backend to force - both are left unset, which selects the kernel's own defaults.
#
# What the package does contribute is in soc_cb.c: a SystemCoreClock refresh before the kernel
# programs its tick, and tickless sleep hooks that suspend the HAL timebase so a suppressed sleep
# is not cut short at that timer's period. Both are optional and both degrade to nothing when the
# HAL is absent.
#
# One family fact deliberately NOT stated: AHURA_SOC_ARCH. The core varies across the whole STM32
# range - M0+ on C0/G0/L0, M4 on F4/L4/G4, M7 on F7/H7, M33 on H5/U5/WBA - and CubeMX always puts
# a -mcpu in CMAKE_C_FLAGS for the kernel's own detection to read, so a value here could only be
# wrong.
#
# What STM32 projects DO need is not configuration but CubeMX settings: stopping the generator
# emitting its own PendSV_Handler, and moving the HAL timebase off SysTick. Those are project
# edits rather than kernel ones, which is why they live in doc/vendor-notes.md and are applied by
# install_stm32_online.py.

set(AHURA_SOC_NAME "STMicroelectronics STM32")

set(AHURA_SOC_SOURCES "${CMAKE_CURRENT_LIST_DIR}/soc_cb.c")

# No AHURA_SOC_INCLUDE_DIRS: this package has no public header, because it has nothing for the
# application to call. Every entry point it has is a _cb the kernel invokes itself, declared by
# the kernel in ahura.h and os_arch_port_common.h. A header here could only have restated that.
#
# soc_cb.c reaches the HAL through the header named by SOC_CONFIG_HAL_HEADER. OS_CONFIG_DIR already
# puts THAT header's own directory on the kernel's include path - but not the HAL tree behind it,
# and on a CubeMX project that header is main.h, which includes stm32<family>_hal.h. Without the
# rest of the chain the kernel library fails with 'stm32xxxx_hal.h: No such file or directory',
# naming a header the application itself can see perfectly well.
#
# CubeMX generates an INTERFACE target carrying exactly those include directories, and adds it
# before the kernel block, so it is already defined by the time this file is read.
#
# Guarded on TARGET rather than assumed: a hand-written or non-CubeMX STM32 project has no such
# target and names its HAL include paths some other way, and referring to a target that does not
# exist is a CMake error rather than a no-op.
if(TARGET stm32cubemx)
	set(AHURA_SOC_LINK_LIBRARIES stm32cubemx)
endif()
