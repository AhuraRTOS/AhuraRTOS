cmake_minimum_required(VERSION 3.22)

project(ahura_kernel C)

# ***********************************************************************************************
# SoC package (optional)
#
# A SoC package supplies the callbacks that are facts about the silicon rather than product
# decisions: the core id, the inter-core IPI, the kernel spinlock, an external tick timer,
# TrustZone banking, the tickless sleep hooks. It also states the two configuration values that
# belong to the target rather than to the application - the PendSV vector name and the spinlock
# backend - which is why neither appears in os_config.h.
#
#     set(AHURA_SOC raspberrypi/rp235x_arm)  # before add_subdirectory(AhuraRTOS)
#
# Leaving it unset is fully supported and is what every project did before packages existed: the
# kernel builds exactly as it always has, and the application supplies that group itself by
# copying template/soc_cb.c. That is what keeps every unpackaged MCU usable.
#
# The dispatch lives here, inside the kernel, rather than beside it. The PendSV name has to be
# known while the kernel's own port .c files compile - they paste it into naked assembly - so the
# package has to be read BEFORE the ahura_kernel target is finished. Doing that here means the
# application never has to get an add_subdirectory() order right.
# ***********************************************************************************************

set(AHURA_SOC "" CACHE STRING "SoC package under soc/, e.g. raspberrypi/rp235x_arm or st/stm32")

if(NOT AHURA_SOC STREQUAL "")
	set(_os_soc_dir "${CMAKE_CURRENT_SOURCE_DIR}/soc/${AHURA_SOC}")

	if(NOT EXISTS "${_os_soc_dir}/soc.cmake")
		# Name what IS available, so the error is actionable rather than just a rejection.
		file(GLOB _os_soc_found "${CMAKE_CURRENT_SOURCE_DIR}/soc/*/*/soc.cmake")
		set(_os_soc_names "")
		foreach(_os_soc_candidate IN LISTS _os_soc_found)
			get_filename_component(_os_soc_path "${_os_soc_candidate}" DIRECTORY)
			file(RELATIVE_PATH _os_soc_name "${CMAKE_CURRENT_SOURCE_DIR}/soc" "${_os_soc_path}")
			list(APPEND _os_soc_names "${_os_soc_name}")
		endforeach()
		list(JOIN _os_soc_names ", " _os_soc_names)

		message(FATAL_ERROR
			"Unknown AHURA_SOC='${AHURA_SOC}': no ${_os_soc_dir}/soc.cmake.\n"
			"Packaged today: ${_os_soc_names}.\n"
			"For an unpackaged target leave AHURA_SOC unset and copy "
			"template/soc_cb.c into the application instead (see doc/soc.md).")
	endif()

	# Sets AHURA_SOC_NAME and, as the target needs them, AHURA_SOC_PENDSV_HANDLER,
	# AHURA_SOC_SVC_HANDLER, AHURA_SOC_SPINLOCK_BACKEND, AHURA_SOC_ARCH, AHURA_SOC_SOURCES, AHURA_SOC_INCLUDE_DIRS,
	# AHURA_SOC_LINK_LIBRARIES and AHURA_SOC_COMPILE_DEFINITIONS. See doc/soc.md.
	include("${_os_soc_dir}/soc.cmake")

	message(STATUS "Ahura SoC package: ${AHURA_SOC} (${AHURA_SOC_NAME})")
else()
	message(STATUS "Ahura SoC package: none - the application supplies soc_cb.c itself")
endif()

# ***********************************************************************************************
# Architecture selection
#
# Two steps, because they answer different questions: which INSTRUCTION SET the kernel is being
# built for, and then which core folder within it. Only "arm" exists today; RISC-V and Xtensa are
# on the roadmap, and splitting the lookup now is what stops a future non-ARM build from silently
# resolving to an ARM port.
# ***********************************************************************************************

set(OS_ARCH_FAMILY "" CACHE STRING "Instruction set family under arch/ (currently: arm, riscv)")

if(OS_ARCH_FAMILY STREQUAL "")
	if(CMAKE_C_FLAGS MATCHES "-mcpu=cortex-m" OR CMAKE_C_FLAGS MATCHES "-march=armv[678]")
		set(OS_ARCH_FAMILY "arm")
	elseif(CMAKE_C_FLAGS MATCHES "-march=rv[0-9]" OR CMAKE_C_FLAGS MATCHES "-mcpu=hazard3")
		# Detected so the failure below names the real gap ("no port for riscv") instead of
		# claiming the architecture could not be worked out. RP2350 in its RISC-V mode lands here.
		set(OS_ARCH_FAMILY "riscv")
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^([Aa][Rr][Mm]|[Cc]ortex)")
		set(OS_ARCH_FAMILY "arm")
	else()
		# Defaulting here is what the old "fall back to cortex_m33" line did, and it is exactly
		# the wrong thing on a non-ARM target: the build would succeed against a port for an
		# instruction set the compiler is not generating.
		message(FATAL_ERROR
			"Cannot tell which instruction set to build the Ahura kernel for.\n"
			"CMAKE_C_FLAGS='${CMAKE_C_FLAGS}', CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'.\n"
			"Set -DOS_ARCH_FAMILY=arm (the only family ported today).")
	endif()
endif()

if(NOT IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/arch/${OS_ARCH_FAMILY}")
	message(FATAL_ERROR
		"No Ahura port for OS_ARCH_FAMILY='${OS_ARCH_FAMILY}': "
		"missing ${CMAKE_CURRENT_SOURCE_DIR}/arch/${OS_ARCH_FAMILY}. Ported today: arm.")
endif()

# Allow manual override: -DOS_ARCH_VARIANT=cortex_m4 (for example)
set(OS_ARCH_VARIANT "" CACHE STRING "Core folder name under arch/${OS_ARCH_FAMILY}")

if(OS_ARCH_VARIANT STREQUAL "")
	set(_os_arch_candidates "")

	# A SoC package knows its own core, which is the reliable answer on toolchains that carry the
	# architecture in a linker script or a preset rather than in CMAKE_C_FLAGS.
	if(DEFINED AHURA_SOC_ARCH AND NOT AHURA_SOC_ARCH STREQUAL "")
		list(APPEND _os_arch_candidates "${AHURA_SOC_ARCH}")
	endif()

	string(REGEX MATCH "-mcpu=([^ ]+)" _os_mcpu_match "${CMAKE_C_FLAGS}")
	if(CMAKE_MATCH_1)
		string(TOLOWER "${CMAKE_MATCH_1}" _os_mcpu)
		# Drop feature suffixes such as +nofp, +nodsp or .small-multiply
		# before the folder lookup.
		string(REGEX REPLACE "[+.].*" "" _os_mcpu "${_os_mcpu}")
		string(REPLACE "-" "_" _os_mcpu "${_os_mcpu}")
		list(APPEND _os_arch_candidates "${_os_mcpu}")
	endif()

	# No usable -mcpu (e.g. older toolchains building newer cores with
	# -march=armv8.1-m.main+mve): map the architecture to a representative
	# core folder. All folders of one profile include the same shared port,
	# so any core of the right architecture is equivalent here.
	string(REGEX MATCH "-march=([^ ]+)" _os_march_match "${CMAKE_C_FLAGS}")
	if(CMAKE_MATCH_1)
		string(TOLOWER "${CMAKE_MATCH_1}" _os_march)
		# Drop feature suffixes (+mve.fp, +nofp, ...) but keep profile dots
		# such as armv8-m.main.
		string(REGEX REPLACE "\\+.*" "" _os_march "${_os_march}")
		if(_os_march MATCHES "^armv6s?-m$")
			list(APPEND _os_arch_candidates "cortex_m0")
		elseif(_os_march STREQUAL "armv7-m")
			list(APPEND _os_arch_candidates "cortex_m3")
		elseif(_os_march STREQUAL "armv7e-m")
			list(APPEND _os_arch_candidates "cortex_m4")
		elseif(_os_march STREQUAL "armv8-m.base")
			list(APPEND _os_arch_candidates "cortex_m23")
		elseif(_os_march STREQUAL "armv8-m.main")
			list(APPEND _os_arch_candidates "cortex_m33")
		elseif(_os_march STREQUAL "armv8.1-m.main")
			list(APPEND _os_arch_candidates "cortex_m55")
		endif()
	endif()

	if(DEFINED CMAKE_SYSTEM_PROCESSOR AND NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "")
		string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _os_proc)
		string(REPLACE "-" "_" _os_proc "${_os_proc}")
		list(APPEND _os_arch_candidates "${_os_proc}")
	endif()

	foreach(_os_candidate IN LISTS _os_arch_candidates)
		if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/arch/${OS_ARCH_FAMILY}/${_os_candidate}/os_arch_port.c")
			set(OS_ARCH_VARIANT "${_os_candidate}")
			break()
		endif()
	endforeach()
endif()

# There used to be a final "cortex_m33" candidate here, so an undetected core still built. It
# produced a kernel for the wrong core with no diagnostic - fatal on an M0+, whose instruction set
# does not have what the v8m port emits - and it would have quietly mis-answered every non-ARM
# build once a second family lands. An unanswerable question is now an error naming what to set.
if(OS_ARCH_VARIANT STREQUAL "")
	message(FATAL_ERROR
		"Cannot tell which ${OS_ARCH_FAMILY} core to build the Ahura kernel for.\n"
		"CMAKE_C_FLAGS='${CMAKE_C_FLAGS}', CMAKE_SYSTEM_PROCESSOR='${CMAKE_SYSTEM_PROCESSOR}'.\n"
		"Set -DOS_ARCH_VARIANT=<core> (for example cortex_m33), or select a SoC package with "
		"AHURA_SOC, which states its own core.")
endif()

set(OS_ARCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arch/${OS_ARCH_FAMILY}/${OS_ARCH_VARIANT}")

if(NOT EXISTS "${OS_ARCH_DIR}/os_arch_port.c")
	message(FATAL_ERROR "Unsupported OS_ARCH_VARIANT='${OS_ARCH_VARIANT}'. Missing ${OS_ARCH_DIR}/os_arch_port.c")
endif()

message(STATUS "Ahura kernel arch: ${OS_ARCH_FAMILY}/${OS_ARCH_VARIANT}")

add_library(ahura_kernel STATIC
	arch/${OS_ARCH_FAMILY}/${OS_ARCH_VARIANT}/os_arch_port.c
	kernel/os_atomic.c
	kernel/os_critical.c
	kernel/os_delay.c
	kernel/os_event.c
	kernel/os_kernel.c
	kernel/os_list.c
	kernel/os_log.c
	kernel/os_mem.c
	kernel/os_msg.c
	kernel/os_mutex.c
	kernel/os_notify.c
	kernel/os_queue.c
	kernel/os_sem.c
	kernel/os_task.c
	kernel/os_tick.c
	kernel/os_timer.c
)

# Two directories, and only two: this one for ahura.h - the single public header, which is why it
# sits at the top rather than inside a source directory - and the selected port directory for
# os_arch_port.h, which ahura.h includes and therefore cannot be PRIVATE.
#
# os_internal.h is NOT reachable through either, even though kernel/ is a subdirectory of the first:
# an include path names one directory, not a tree. The core sources find it because a quoted include
# searches the including file's own directory first, which is where it sits - so the internal
# contract stays internal without a second target or an install step to enforce it.
target_include_directories(ahura_kernel PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${OS_ARCH_DIR}
)

# ***********************************************************************************************
# SoC package contribution
#
# The package's sources join the kernel library rather than forming one of their own. That is a
# deliberate choice about link behaviour, not packaging taste: every callback in the group is
# CALLED by the kernel and DEFINED nowhere else, so the reference is unresolved until this object
# supplies it and the linker always pulls it in. A separate archive holding only weak definitions
# that nothing has yet referenced is the case where it might not.
#
# It also means an application's CMakeLists needs no change at all to gain a SoC package: it still
# links ahura_kernel and nothing else.
# ***********************************************************************************************

if(NOT AHURA_SOC STREQUAL "")
	if(DEFINED AHURA_SOC_SOURCES)
		target_sources(ahura_kernel PRIVATE ${AHURA_SOC_SOURCES})
	endif()

	# PUBLIC: the application includes the package's own header (soc.h) to reach whatever it
	# exposes, on the packages that expose anything.
	if(DEFINED AHURA_SOC_INCLUDE_DIRS)
		target_include_directories(ahura_kernel PUBLIC ${AHURA_SOC_INCLUDE_DIRS})
	endif()

	# PUBLIC for the same reason as OS_CONFIG_DIR below: a vendor SDK target carries include paths
	# and defines that the kernel and the application must agree on.
	# Some SoC packages have no symbol the kernel references by name - every entry point they have
	# is a callback with a weak default in the kernel itself. The linker then never extracts the
	# package object from the archive at all, and every one of those callbacks silently falls back
	# to the weak default: no SoC init, no wake source, no diagnostics, and nothing reported. A
	# package that can be in that position names a symbol here and the -u forces the extraction.
	if(DEFINED AHURA_SOC_LINK_OPTIONS)
		target_link_options(ahura_kernel PUBLIC ${AHURA_SOC_LINK_OPTIONS})
	endif()

	if(DEFINED AHURA_SOC_LINK_LIBRARIES)
		target_link_libraries(ahura_kernel PUBLIC ${AHURA_SOC_LINK_LIBRARIES})
	endif()

	if(DEFINED AHURA_SOC_COMPILE_DEFINITIONS)
		target_compile_definitions(ahura_kernel PUBLIC ${AHURA_SOC_COMPILE_DEFINITIONS})
	endif()

	# The two target facts that os_config.h deliberately does not carry. PUBLIC so the application
	# and the self-test suite compile against the same values the kernel did; os_arch_port_common.h
	# supplies the defaults when a package states neither.
	if(DEFINED AHURA_SOC_PENDSV_HANDLER AND NOT AHURA_SOC_PENDSV_HANDLER STREQUAL "")
		target_compile_definitions(ahura_kernel PUBLIC
			OS_CONFIG_ARCH_PENDSV_HANDLER=${AHURA_SOC_PENDSV_HANDLER})
		message(STATUS "Ahura PendSV vector: ${AHURA_SOC_PENDSV_HANDLER}")
	endif()

	# The SVC vector name, on the same terms. The kernel never uses SVC, but the self-test suite
	# installs a handler to reach ISR context, and a wrong name there hangs rather than failing to
	# link - the vector keeps pointing at the SDK's breakpoint stub.
	if(DEFINED AHURA_SOC_SVC_HANDLER AND NOT AHURA_SOC_SVC_HANDLER STREQUAL "")
		target_compile_definitions(ahura_kernel PUBLIC
			OS_CONFIG_ARCH_SVC_HANDLER=${AHURA_SOC_SVC_HANDLER})
		message(STATUS "Ahura SVC vector:    ${AHURA_SOC_SVC_HANDLER}")
	endif()

	if(DEFINED AHURA_SOC_SPINLOCK_BACKEND AND NOT AHURA_SOC_SPINLOCK_BACKEND STREQUAL "")
		target_compile_definitions(ahura_kernel PUBLIC
			OS_CONFIG_SPINLOCK_SOC_BACKEND=${AHURA_SOC_SPINLOCK_BACKEND}U)
	endif()
endif()

# Application configuration: set OS_CONFIG_DIR (before add_subdirectory, or as
# a cache variable) to the directory holding the project's os_config.h —
# copied from template/os_config.h; the project layout does not matter,
# any directory works. PUBLIC so the kernel library and everything linking it
# see the exact same configuration — a mismatch would silently change
# structure sizes.
if(OS_CONFIG_DIR)
	target_include_directories(ahura_kernel PUBLIC ${OS_CONFIG_DIR})
else()
	message(WARNING
		"OS_CONFIG_DIR is not set: the Ahura kernel needs the directory containing "
		"your os_config.h (copy template/os_config.h). Set it "
		"before add_subdirectory(AhuraRTOS), or add the directory to the "
		"ahura_kernel target include paths yourself.")
endif()

# Note: the files under template/ are deliberately NOT part of this build.
# They sit in a directory of their own precisely because nothing here
# compiles them: the source list above names every file individually, so a
# template cannot be swept into the library by a stray glob later.
# - template/os_config.h: headers are never compiled; copy it into the
#   application as os_config.h (see OS_CONFIG_DIR above).
# - template/os_cb.c: copy it into the application source tree as os_cb.c
#   and add it to the APPLICATION build; its functions are the application's
#   half of the callback contract (log output, assertions, stack overflow).
# - template/soc_cb.c: the SoC-owned half of the same contract - core id,
#   IPI, spinlocks, external tick, TrustZone, tickless. Copy it ONLY when no
#   SoC package covers the target; selecting AHURA_SOC supplies the whole
#   group instead, and two definitions of one callback is the failure the
#   split exists to prevent. See doc/soc.md.
# - template/os_main.c: copy it into the application source tree as
#   os_main.c and add it to the APPLICATION build; it defines os_main() -
#   the default application task's body (not a "_cb" function: this is
#   where the application's own code runs). Task creation itself is
#   unconditional in os_kernel.c's os_init(): tsk_main is always created,
#   except in a test build (OS_CONFIG_TEST_ENABLE 1), where the self-test
#   task takes its place. See doc/api.md "Default application task".
#
# The self-test suite (test/) is different: it IS a build
# target (its own CMakeLists.txt), not a template - see
# test/CMakeLists.txt and doc/testing.md "Self-test suite".
