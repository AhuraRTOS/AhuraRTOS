/* Deterministic SoC reference clock for actual port tests. */
#include <stdint.h>
uint64_t test_reference;
uint32_t os_arch_reference_clock_hz_cb(void) { return 1000000U; }
uint32_t os_arch_tick_reference_clock_hz_cb(void) { return 1000000U; }
uint64_t os_arch_reference_clock_get_cb(void) { return test_reference; }
