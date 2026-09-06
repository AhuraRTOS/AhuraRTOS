# Examples

**[→ The full catalogue is doc/examples.md](../doc/examples.md)** - what each
file shows, which `os_config.h` switch it needs, how many tasks it creates, and
what to check when nothing appears on the console.

This file is the signpost for anyone who arrives in this directory first.

## What these are

One small, runnable program per kernel feature, in
[`kernel/`](kernel/). Each file **is** a complete `os_main.c`: it defines
`os_main()` and nothing else.

```bash
cp examples/kernel/os_main_mutex.c  <your project>/os_main.c   # or Core/Src/os_main.c
```

Rebuild and read the console. Nothing is added to the build and nothing is
removed from it, there is no build system here to invoke, and there is no
example to "select".

Two things that are not obvious and cost people an afternoon:

- **`OS_CONFIG_TEST_ENABLE` must be `0`.** With it at `1`, `os_init()` runs the
  self-test suite *instead of* the application task, so no example runs at all -
  whichever file is in the build.
- **A silent console is almost always the libc, not the example.** Every one of
  these prints through `printf`, which needs a retarget. See
  [Nothing on the terminal?](../doc/self-test.md#nothing-on-the-terminal-check-the-libc-before-the-kernel).

## Naming

`os_main_<feature>.c`, one feature per file, and the feature is the
`os_config.h` switch it needs: `os_main_mutex.c` needs
`OS_CONFIG_MUTEX_ENABLE`, `os_main_event.c` needs `OS_CONFIG_EVENT_ENABLE`, and
so on. Six of them - `hello`, `task`, `delay`, `critical`, `kernel_lock` and
`list` - need nothing beyond the defaults.

A feature switched off does not fail at the call site with a warning: the API is
not declared at all, so the compiler reports an implicit declaration naming the
function. Look the function up in
[the catalogue](../doc/examples.md#the-examples).
