cmake_minimum_required(VERSION 3.22)

# Ahura kernel self-test suite: a small static library so any application
# that already links ahura_kernel can add it.
#
# It supplies os_test(), which ahura.h declares and NOTHING in the kernel
# defines - not even a weak stub. That is deliberate: with
# OS_CONFIG_TEST_ENABLE at 1, os_init() creates a task whose whole job is to
# run os_test(), so forgetting to link this library must fail at link time
# ("undefined reference to `os_test'") rather than produce a test build that
# boots and silently tests nothing.
#
# So the two halves have to agree, and neither can check the other:
#
#   OS_CONFIG_TEST_ENABLE 1U  in os_config.h   ->  creates the test task
#   target_link_libraries(<app> os_test)       ->  supplies what it runs
#
# No header here: the only symbol this library exports is already declared in
# ahura.h. Independent from the top-level project: link this anywhere
# ahura_kernel is available to run the suite on that target/device.

add_library(os_test STATIC
	os_test.c
)

target_link_libraries(os_test PUBLIC
	ahura_kernel
)
