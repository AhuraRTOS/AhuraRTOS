cmake_minimum_required(VERSION 3.22)

project(ahura_kernel C)

# Allow manual override: -DOS_ARCH_VARIANT=cortex_m4 (for example)
set(OS_ARCH_VARIANT "" CACHE STRING "ARM Cortex arch folder name under ahura_kernel/arch/arm")

if(OS_ARCH_VARIANT STREQUAL "")
	set(_os_arch_candidates "")

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

	list(APPEND _os_arch_candidates "cortex_m33")

	foreach(_os_candidate IN LISTS _os_arch_candidates)
		if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/arch/arm/${_os_candidate}/os_arch_port.c")
			set(OS_ARCH_VARIANT "${_os_candidate}")
			break()
		endif()
	endforeach()
endif()

set(OS_ARCH_DIR "${CMAKE_CURRENT_SOURCE_DIR}/arch/arm/${OS_ARCH_VARIANT}")

if(NOT EXISTS "${OS_ARCH_DIR}/os_arch_port.c")
	message(FATAL_ERROR "Unsupported OS_ARCH_VARIANT='${OS_ARCH_VARIANT}'. Missing ${OS_ARCH_DIR}/os_arch_port.c")
endif()

message(STATUS "Ahura kernel arch: ${OS_ARCH_VARIANT}")

add_library(ahura_kernel STATIC
	arch/arm/${OS_ARCH_VARIANT}/os_arch_port.c
	core/os_atomic.c
	core/os_critical.c
	core/os_delay.c
	core/os_event.c
	core/os_kernel.c
	core/os_list.c
	core/os_log.c
	core/os_mem.c
	core/os_mutex.c
	core/os_notify.c
	core/os_queue.c
	core/os_semaphore.c
	core/os_task.c
	core/os_tick.c
	core/os_timer.c
	core/os_work.c
)

target_include_directories(ahura_kernel PUBLIC
	${CMAKE_CURRENT_SOURCE_DIR}
	${OS_ARCH_DIR}
)

# Application configuration: set OS_CONFIG_DIR (before add_subdirectory, or as
# a cache variable) to the directory holding the project's os_config.h —
# copied from os_config_template.h; the project layout does not matter,
# any directory works. PUBLIC so the kernel library and everything linking it
# see the exact same configuration — a mismatch would silently change
# structure sizes.
if(OS_CONFIG_DIR)
	target_include_directories(ahura_kernel PUBLIC ${OS_CONFIG_DIR})
else()
	message(WARNING
		"OS_CONFIG_DIR is not set: the Ahura kernel needs the directory containing "
		"your os_config.h (copy ahura_kernel/os_config_template.h). Set it "
		"before add_subdirectory(ahura_kernel), or add the directory to the "
		"ahura_kernel target include paths yourself.")
endif()

# Note: the template files are deliberately NOT part of this build.
# - os_config_template.h: headers are never compiled; copy it into the
#   application as os_config.h (see OS_CONFIG_DIR above).
# - os_cb_template.c: copy it into the application source tree as os_cb.c
#   and add it to the APPLICATION build; its functions override the
#   kernel's weak _cb defaults (platform clock, tickless sleep, TrustZone,
#   multi-core SoC glue).
# - os_main_template.c: copy it into the application source tree as
#   os_main.c and add it to the APPLICATION build; it defines os_main() -
#   the default application task's body (not a "_cb" function: this is
#   where the application's own code runs). Task creation itself is
#   unconditional in os_kernel.c's os_init(), gated by
#   OS_CONFIG_MAIN_TASK_ENABLE; see README "Default application task".
#
# The self-test suite (ahura_kernel/test/) is different: it IS a build
# target (its own CMakeLists.txt), not a template - see
# ahura_kernel/test/CMakeLists.txt and README "Self-test suite".
