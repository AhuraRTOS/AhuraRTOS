# Ahura Kernel - source

The kernel itself: architecture-independent core, the Cortex-M port layer, and
the self-test suite.

```text
kernel/
├── ahura.h                 <- the single public header; applications include only this
├── core/                   <- portable C11: scheduler, sync/IPC, timers, memory, log
├── arch/arm/               <- the port layer, one directory per Cortex-M core
├── test/                   <- the self-test suite, built as a separate os_test library
├── template/               <- the three files you copy into your project
│   ├── os_config.h         <- copy into your project as os_config.h
│   ├── os_cb.c             <- copy into your project as os_cb.c
│   └── os_main.c           <- copy into your project as os_main.c
└── CMakeLists.txt          <- builds the ahura_kernel library
```

**📖 The full kernel reference - every API, every `os_config.h` option, how the
scheduler, context switch and priority inheritance actually work - is
[`doc/kernel.md`](../doc/kernel.md).**

To get this into your own project, start at
[Installation](../doc/installation.md).

## License

MIT - every source file carries `SPDX-License-Identifier: MIT` in its header.
See the [project LICENSE](../LICENSE).
