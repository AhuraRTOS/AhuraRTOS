# Kernel audit regressions

This standalone harness compiles the repository's actual `os_task.c`, `os_task_mutex.c`,
`os_list.c`, `os_mutex.c` and `os_kernel.c` and executes their ARM instructions in Unicorn.
It includes those sources directly so the tests can inspect effective priorities and seed
deterministic scheduler states without exposing TCB internals in the public API.

Requirements: Arm GNU GCC with its matching `nm`, Python 3 and the `unicorn` Python package.
From the repository root:

```text
python test/audit_kernel/run.py --gcc /path/to/arm-none-eabi-gcc
```

Use `--python-deps /path/to/packages` for a local package installation. Build products use a
temporary directory and are removed after the run; `--build-dir` selects its parent directory.
`--tests` optionally selects one or more test entry names printed by `--help`.

The tests cover:

- Migration before critical entry when deleting or pausing a remote running task, and migration
  injected at an unmasked core-ID read for current-task identity, wait results and mutex ownership.
- Lowering an owner's base priority while it has waiters, increasing/decreasing a blocked
  waiter's priority across a mutex-owner chain, waiter reordering, and immediate inheritance
  revocation on timeout, pause and forced wake before a waiter gets CPU time again.
- Mutex ownership changes with waiters still queued: a peer that acquires before the signaled
  waiter immediately inherits from the remaining waiters, and their timeout, pause or deletion
  removes the boost from the current owner rather than a previously recorded owner.
- Partial initialization of per-core idle tasks and retry of only the missing core.
- Successful startup and injected stack-initialization failure in the first idle task,
  second idle task and main task, plus scheduler start before initialization. Assertions are
  disabled in this harness so startup failure cannot be caught only by a debug check.

The test port replaces hardware interrupt masking, core IDs, spinlocks, stack-frame creation,
tick initialization and context switching. Migration is a deterministic injection, not concurrent
execution of two simulated cores. Wait lists, priority inheritance, task lifecycle and mandatory
startup checks execute production code. These tests do not validate exception assembly, real
interrupt timing, SMP memory ordering, service-task scheduling or hardware stack preservation;
those still need board or full-system emulator testing.
