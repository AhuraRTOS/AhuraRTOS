# AhuraRTOS audit

Date: **2026-09-08**  
Source: `E:\GitHub\AhuraRTOS`  
Audited baseline revision: `899df71b766be549fec3e583f73340c5d1ac938e`  
Remediation date: **2026-09-09** (working-tree changes, not committed)  
Test projects: `E:\GitHub\ahura_testing` (the six existing projects)  
Scope: correctness, concurrency, timekeeping, architecture/SoC portability, performance, API/build integrity, installation, source organization and regression coverage.

## Assessment

The existing separation into `kernel/`, `arch/` and `soc/` is a useful foundation. Static task storage, generation-tagged task identities, per-priority ready queues, a ready bitmap, delta timeout lists, and application-owned configuration are worth preserving. All six existing Release configurations compiled and linked during this audit, and all six embedded RTOS copies matched the main repository byte for byte across 142 files at the start of the review.

**A01-A21: Fixed in the remediated source**, with verification and remaining hardware gates recorded below. Section 11 adds coordinated Pico 2 Arm two-core DEEP sleep; its hardware validation is pending. The original findings and baseline line references remain as historical evidence; they describe the audited revision, not the corrected code.

The existing `kernel/` file and folder structure is preserved, as requested. No kernel source was split, renamed or moved. New standalone regression suites live under `test/` and do not enlarge the built-in board self-test image.

These fixes do not establish that the RTOS has no possible errors or is portable to every architecture. Section 3 retains separate lifecycle/contract limitations, and performance and broader portability work remain open. No board was flashed by the audit tooling. Section 10 records the user's successful Pico 2 Arm board rerun. Broader hardware configuration coverage, MVE register preservation, timing drift, power and WCET measurements remain open. Deterministic emulation below executes compiled C with hardware/scheduler substitutions and is identified separately from board execution.

### Remediation verification

- All six existing Release configurations compile and link with the corrected RTOS in isolated project copies. Application configuration files are unchanged.
- Eleven ARM core wrappers and M23/M33 secure/non-secure compile probes pass.
- All 17 kernel modules compile warning-free in full, full SMP, all-off, allocator-off and mutex-off probes. Invalid main priority `0U` is rejected, and C++17 public headers compile with message support on and off.
- Ten kernel and eight IPC/logging C scenarios pass under deterministic ARM/Unicorn execution. Baseline sensitivity checks fail at their intended assertions (three original-source kernel scenarios, two additional pre-fix owner-transition scenarios and all eight IPC scenarios); message-disabled C++ also fails on the original source.
- Sixteen architecture timing/port scenarios pass, including real port C with controlled MMIO for partial reloads, SysTick boundary ownership, external-timer isolation and shared SMP counter selection. Twelve installer regressions pass. Coverage boundaries are recorded in Section 8.

The G431 self-test image is close to its flash limit. See the remediated size table in Section 8; no features were disabled to make the fixes fit. Keep sufficient application headroom and measure the added synchronization cost before deployment.

### Meaning of Fixed

**Fixed** means the identified source defect has been corrected and the stated source/compiler/regression checks passed. It does not mean all hardware configurations have been executed. In particular, SMP tickless entry can wait for core 0 to wake, restore clocks and announce time. Its hooks must be bounded, nonblocking, and must not wait for peer kernel API work. This latency and callback contract are part of the porting requirements.

The sections below preserve the **original audit results** unless explicitly labeled remediation. Baseline failures and sizes are retained for comparison.

## 1. What was checked

### Review coverage

| Area | Review performed |
|---|---|
| Kernel | All 17 kernel C modules and their relevant public/private contracts; task lifecycle, IDs, scheduler, delta lists, waits/wakes, timeout conversion, critical sections, scheduler locking and SMP ownership |
| IPC and services | Mutex inheritance, semaphores, queues, variable messages, events, notifications, timer/deferred-call lists, allocation/coalescing, logging and atomic wrappers |
| Architecture | Shared ARMv6-M, ARMv7-M and ARMv8-M implementations; RV32 frame/handoff; interrupt masks, atomics, cycle fallback, tickless flow and optional register state |
| SoC | STM32, RP2040, RP235x Arm and RP235x RISC-V packages; clock/tick integration, sleep hooks, IPI/spinlock integration and SDK assumptions |
| Build and integration | Root/test CMake, the six project configurations and integration points, seven installer Python files, public-header feature combinations and architecture wrappers |
| Documentation/testing | Public behavior against relevant API/porting/design documentation; existing self-test coverage, example organization and style guidance |

The vendor HAL/CMSIS/Pico SDK trees were inspected where integration required them, not audited as complete third-party codebases. Examples and documents were reviewed for contracts and consistency; they were not each run as independent firmware applications. Runtime behavior of every possible feature combination remains outside the evidence gathered here.

### Six-project build results

All projects currently enable the self-test and use a 1 kHz tick. These are **compile/link results**, not newly observed on-board PASS results.

| Project | Port | Cores | Tickless | Configuration coverage | Release result |
|---|---|---:|---|---|---|
| `rpi_pico` | Cortex-M0+ / RP2040 | 2 | On | All listed optional kernel services enabled | PASS |
| `rpi_pico2_arm` | Cortex-M33 / RP235x | 2 | On | All listed optional kernel services enabled | PASS |
| `rpi_pico2_riscv` | Hazard3 / RP235x | 2 | On | All listed optional kernel services enabled | PASS |
| `stm32g431rb` | Cortex-M4 | 1 | On | Semaphore, queue, event, log and CPU usage disabled | PASS |
| `stm32h503rb` | Cortex-M33 | 1 | On | Semaphore, queue, message, event, log and CPU usage disabled | PASS |
| `stm32h743zi` | Cortex-M7 | 1 | Off | All listed optional kernel services enabled | PASS; one unused self-test helper warning |

All six have mutexes enabled and TrustZone mode set to `OS_CONFIG_TRUSTZONE_DISABLED`. Thus none catches A12, and the C-only application sources do not catch A13. The current H743 configuration does not validate tickless behavior even though repository documentation describes prior hardware tickless verification. That historical statement was not independently revalidated.

Build tools used: STM32 GCC 14.3.1, Arm GNU GCC 15.2.1, RISC-V GCC 15.2.0, Pico SDK 2.3.0, CMake and Ninja. Builds were generated in `.audit-work/builds`, separate from the existing project build directories. An initial compiler access restriction and a RISC-V build-directory permission conflict were resolved before recording PASS.

The STM32 self-test images use:

| Image | Flash | RAM as reported by linker |
|---|---:|---:|
| H503 | 123,788 / 131,072 bytes (94.44%) | 21,280 / 32,768 bytes |
| G431 | 129,624 / 131,072 bytes (98.90%) | 21,496 / 32,768 bytes |
| H743 | 173,048 / 2,097,152 bytes | 24,864 bytes in DTCMRAM |

These include test code, application and vendor support, not just the kernel. G431 has only 1,448 bytes of flash headroom in this image; split the test suite into selectable groups so coverage does not depend on disabling features to fit one monolithic image.

### Additional checks actually executed

| Check | Outcome |
|---|---|
| Eleven ARM wrappers, C11, soft ABI, tickless enabled | M0, M0+, M3, M4, M7, M23, M33, M35P, M52, M55 and M85 compiled |
| M23/M33 secure and non-secure port compile probes | All four compiled; no secure firmware was linked/executed |
| All 17 kernel modules, full features / full SMP / allocator disabled | All compiled with `-Wall -Wextra -Wundef`, no warnings in these probes |
| All optional features disabled | Failed: unguarded `os_mutex_t`; disabling only mutexes reproduced it |
| C++17 umbrella header, messages enabled/disabled | Enabled passed; disabled failed with unmatched closing brace |
| Invalid main priority (`0U`) | All kernel modules compiled; missing validation permits silent startup failure |
| M52/M55/M85 integer-only MVE assembly | Compiled; expected s16-s31 save/restore instructions absent |
| Offline installer dry-run against each existing project | All six returned success and reported already installed |
| Python syntax parsing | All seven installer files parsed |
| Installer failure injection in disposable fixtures | Same-source update and rollback paths lost original fixture data |
| Message/timing/handoff arithmetic models | Reproduced the selected source-derived failure schedules; not kernel execution |

## 2. Prioritized findings

Evidence labels: **C** = actual compiler/assembly result; **R** = actual installer code executed in disposable fixtures; **S** = source-confirmed reachable failure sequence; **M** = supporting arithmetic/state model, not target execution. P1 means serious failure under the stated supported usage/configuration; P2 means an important build, integration or bounded correctness defect; P3 means lower-impact behavior/documentation mismatch. No finding is labeled a hardware reproduction.

| ID | Priority | Finding | Applies to | Evidence | Status |
|---|---|---|---|---|---|
| A01 | P1 | RV32 publishes a migratable task before abandoning its stack | RV32 SMP | S, M | **Fixed** |
| A02 | P1 | Stale core identity can delete a running remote task or corrupt mutex ownership | Migratable tasks on SMP | S | **Fixed** |
| A03 | P1 | Runtime priority changes violate mutex inheritance | Mutex builds, single-core and SMP | S | **Fixed** |
| A04 | P1 | Suppressed past time is charged to newly armed deadlines | SMP + tickless | S | **Fixed** |
| A05 | P1 | SysTick-derived busy waits can hang in an ISR/critical section | M0/M0+ and no-DWT fallback | S, M | **Fixed** |
| A06 | P1 | Integer-only MVE registers are not preserved | M52/M55/M85 with integer MVE | C | **Fixed** |
| A07 | P1 | Early self-SysTick wake loses fractional tick phase | v8m self-SysTick tickless, including H503 LIGHT | S, M | **Fixed** |
| A08 | P1 | Arm SoC LIGHT tickless can count partial time twice | RP2040 / RP235x Arm | S, M | **Fixed** |
| A09 | P1 | Message producers remain blocked despite sufficient space | Concurrent message-buffer producers | S, M | **Fixed** |
| A10 | P1 | Updating from an installed checkout can delete that checkout | Installer source equals destination | R, S | **Fixed** |
| A11 | P1 | Installer rollback loses previous files/checkout | Replacement or forced-template failure | R | **Fixed** |
| A12 | P2 | Disabling mutexes fails to compile | `OS_CONFIG_MUTEX_ENABLE=0` | C | **Fixed** |
| A13 | P2 | Disabling messages breaks C++ inclusion | C++ + `OS_CONFIG_MSG_ENABLE=0` | C | **Fixed** |
| A14 | P2 | Undersized message receiver consumes the only wakeup | Multiple blocked message receivers | S, M | **Fixed** |
| A15 | P2 | Startup ignores mandatory task creation failures | Invalid configuration / failed port initialization | C, S | **Fixed** |
| A16 | P2 | External-tick suppression modifies application SysTick | External tick + Arm SoC suppression | S | **Fixed** |
| A17 | P2 | Remote CPU usage sampling races with tick updates | SMP diagnostics | S | **Fixed** |
| A18 | P3 | Cumulative dropped-log counter resets during drain | Logging enabled | S | **Fixed** |
| A19 | P2 | Disabling tickless also removes Pico 2 Arm inter-core callbacks; RP sleep configuration lacks feature guards | RP235x Arm SMP; RP configuration templates | C, S | **Fixed** |
| A20 | P3 | Board logging self-test still expects the pre-A18 counter reset | Logging self-test enabled | C, S, R | **Fixed** |

### A01 - RV32 SMP stack ownership handoff

**Status: Fixed (2026-09-09).** RV32 records a per-hart scheduler stack at first dispatch and switches to it before publishing the outgoing task. The stack is the retained boot stack, whose lifetime and sizing contract are documented. Validation: six-project RV32 link and context assembly review; on-board migration/stack stress remains required.

**Locations:** `arch/riscv/common/os_arch_port_rv32.c:113-115,126,166-169`; `kernel/os_task.c:1843-1876,1932-1971`.

The SWI saves the outgoing context on the task's own stack, then calls both scheduler C functions on that same stack. `os_task_stack_save_current()` clears `running_core`, makes the task READY and releases the global lock. Another core may now resume the task while the first core still has live scheduler C frames below its saved context. Ordinary calls in the resumed task can overwrite the first core's locals/return addresses. The first core changes SP only after `select_next()` returns. This was independently checked by both scheduler and port reviews.

**Fix:** change to a per-hart scheduler/interrupt stack before publishing migration eligibility, or implement an explicit handoff that publishes only after the old stack is abandoned. Extending the C lock alone is insufficient if release still precedes the SP change. **Regression:** inject a peer dispatch immediately after publication; migrate tasks with deep call frames and verify return flow and stack guards. The supporting address-range model demonstrates overlapping live storage, not an executed CPU fault.

### A02 - Migration invalidates current-core/current-task reads

**Status: Fixed (2026-09-09).** Core identity is sampled after masking/critical entry, and current-task identity/value accessors protect the complete read. Validation: real-source deterministic ARM tests inject migration before critical entry and at an unmasked core-ID read; the original source fails the regression.

**Locations:** `kernel/os_task.c:298-300,317-319,356-368,522-528,553-561,1461-1465`; `kernel/os_mutex.c:110,120-126,247-249`.

Pause/delete cache the calling core before entering the critical section. If task C reads core 0, is preempted, then resumes on core 1 while D runs on core 0, `os_task_delete(&D)` can compare D against the stale core's current pointer and classify it as self. The remote-running protection is bypassed; D's TCB/current pointer can be cleared while D still executes. Pause has the equivalent classification error.

`os_task_current_id_get()` also reads core ID and the corresponding current pointer without excluding migration. It can return the new occupant of the old core. A mutex can then record that wrong owner ID while linking its owner node into the actual caller's list.

**Fix:** obtain the core ID only after scheduling/migration is excluded, and protect the whole current-pointer access. Make current-task accessors safe rather than requiring every caller to reconstruct the rule. **Regression:** deterministic migration between core-ID read and current-pointer/critical access; assert remote delete/pause returns BUSY and mutex identity stays correct.

### A03 - Dynamic priority changes break inheritance

**Status: Fixed (2026-09-09).** Base-priority changes use the shared max(base, owned-mutex waiters) recomputation and propagate changes through blocked owners. Waiter departure revokes inheritance at unlink, including timeout, pause and forced wake. A new mutex owner inherits from waiters already queued, and departure cleanup targets the current owner. Validation: executed real-source priority-chain/reorder/departure and owner-transition regressions; pre-fix sources fail. The existing eight-task chain limit remains documented in Section 3.

**Locations:** `kernel/os_task.c:634-643,2682-2752`; compare correct recomputation in `kernel/os_task_mutex.c:220-241,265-287`.

Two valid sequences fail. First, an owner with base/effective priority 20 holds a mutex with a priority-15 waiter. Lowering its base to 5 sets effective priority to 5 because `effective > old_base` was false; it should remain 15. Second, raising a blocked waiter from 5 to 20 reorders its waiter node but does not propagate the change to its mutex owner. Medium-priority runnable work can then starve the owner and indirectly starve the highest-priority waiter.

**Fix:** use the same `max(base, waiters on all owned mutexes)` recomputation for every base-priority change, then propagate along the blocked-on owner chain. **Regression:** both sequences with medium-priority load; check effective priorities before any unlock. Existing acquisition-order tests do not establish this invariant.

### A04 - SMP tickless deadlines use an outdated time origin

**Status: Fixed (2026-09-09).** Remote kernel entry wakes core 0 and waits for its suppressed window to be measured and announced before observing the tick epoch or arming a deadline. The global lock is released between retries and is not held over sleep. The public tickless entry masks before reading core ownership, preventing migration from opening the window on the wrong core. Validation: source/lock-call review and targeted regressions; real SMP wake latency remains a hardware gate.

**Locations:** `kernel/os_tick.c:193-211,271-290,399-445,503-516,551-562`; `kernel/os_task.c:1024-1041,1137-1149,1239-1256`.

Core 0 can suppress the shared tick while core 1 keeps running. Suppose the clock is 100 when suppression starts; fifty real tick periods later core 1 arms a ten-tick timeout. The new timeout enters a list still describing tick 100. Its IPI correctly wakes core 0, but the ensuing announcement of fifty elapsed ticks immediately expires the new timeout. The IPI solves notification, not the time origin. Timer starts and wait-start accounting have the same class of problem.

**Fix:** reconcile elapsed time before publishing newly armed relative deadlines, or timestamp all deadlines against a common monotonic clock. A simpler temporary policy is coordinated all-core idle before suppressing the shared timebase. **Regression:** keep one core active during suppression, arm a short delay/timer late in the window, and compare expiry against hardware time measured from the arm operation.

### A05 - Busy waits can stop progressing without SysTick service

**Status: Fixed (2026-09-09).** Busy waits use a separate IRQ-independent counter contract: DWT/mcycle on single-core targets, the RP hardware timer on no-DWT targets, or an explicit board counter callback. SMP waits require a shared reference clock so migration cannot compare different per-core counter epochs. Missing hardware support traps for nonzero waits instead of spinning on an ISR-fed counter. Counter units and port requirements are documented; the synthesized profiling counter is no longer used for busy waits.

**Locations:** `arch/arm/common/os_arch_cycle_systick.c:234-267,294-310`; `kernel/os_delay.c:198-215,230-246`; `arch/arm/common/os_arch_port_v6m.c:382-384`.

On M0/M0+, and mainline fallback without DWT, the apparent cycle counter derives whole periods from the SysTick ISR. A higher-priority ISR calling the supported busy-wait path, such as `os_delay_ms(3)` at 1 kHz, prevents that ISR from running. PENDSTSET represents only one pending event. After additional wraps, the monotonic clamp can hold the counter forever below the requested duration. The same applies with interrupts masked throughout the delay.

**Fix:** use an independent monotonic timer on such SoCs, with explicit units/frequency; RP2040 already has a hardware microsecond timer. If a port cannot support an interval/context, reject it explicitly. **Regression:** actual ISR and nested critical-section delays spanning several SysTick periods, checked against an independent timer. The model reaches at most 1,999 apparent cycles over 10,000 simulated cycles for a 3,000-cycle request.

### A06 - Integer-only MVE context is omitted

**Status: Fixed (2026-09-09).** Extended context capability includes integer MVE as well as scalar FP, covering context save/restore, bootstrap and stack sizing. Validation: M52/M55/M85 integer-only MVE assembly includes the required s16-s31 transfers. Actual MVE register-pattern testing still requires suitable hardware or a full-system emulator.

**Locations:** `arch/arm/common/os_arch_port_v8m.c:146-150,194-198,441-451`; `arch/arm/common/os_arch_port_common.h:122-128`.

GCC accepts `-mcpu=cortex-m55+nofp -mthumb -mfloat-abi=softfp`, defines `__ARM_FEATURE_MVE=1`, and leaves `__ARM_FP` undefined. The port gates s16-s31 save/restore only on `__ARM_FP`, omitting Q4-Q7, which alias those registers. The same assumption controls bootstrap state and minimum stack checking. Actual M52/M55/M85 assembly probes all compiled and contained no vstm/vldm save/restore instructions.

**Fix:** one extended-context capability covering scalar FP or MVE, used consistently for context preservation, initialization and size validation; otherwise reject integer-MVE configurations until implemented. **Regression:** two preempting integer-MVE tasks preserving distinct Q4-Q7 patterns. Compiler behavior is supported by [GCC ARM options](https://gcc.gnu.org/onlinedocs/gcc/ARM-Options.html) and the [GCC integer-MVE implementation change](https://gcc.gnu.org/pipermail/gcc-cvs/2020-October/335162.html).

### A07 - Early self-SysTick wake slows the kernel clock

**Status: Fixed (2026-09-09).** The self-SysTick path rejects an entry race with an already-pending tick and restores the remaining first tick interval after early wake before returning to the normal reload. Validation: shared timing arithmetic regressions and ARM port compilation. Register timing/drift must still be measured on H503 hardware.

**Locations:** `arch/arm/common/os_arch_port_v8m.c:703-718,762-790`; planner guard `kernel/os_tick.c:403-418`.

On an early wake, the close path computes whole tick boundaries, then resets CVR and reloads a full ordinary period. It discards the residual part of the current tick. Example: tick at 0 ms, idle starts at 0.1 ms and wakes at 0.5 ms before a boundary; zero elapsed ticks are announced, but the next ordinary tick now arrives around 1.5 ms instead of 1 ms. Repeating this schedule slows deadlines. The arithmetic model gives 100 ticks over 150 tick-periods.

The kernel's one-plan-per-tick/deadline-generation guard prevents concluding that an unrelated periodic IRQ alone freezes time forever; that stronger claim is **not** made here. This self-reprogramming path is in v8m, including the current H503 LIGHT setup with DWT, not the v6m/v7m ports.

**Fix:** retain the residual first interval when restoring periodic operation; handle pending-tick/sample races consistently. **Regression:** early wake at every phase, comparing kernel ticks to an independent reference across repeated windows.

### A08 - Raspberry Pi Arm LIGHT windows can count time twice

**Status: Fixed (2026-09-09).** Arm LIGHT suppression reconstructs the actual unchanged SysTick grid from start/end counter phase and an independent RP reference timer. Pending ISR credit is counted once; a second fractional-duration accumulator no longer supplies the announced tick count. Validation: boundary/phase arithmetic regressions and both RP Arm firmware links; on-board mixed ordinary/tickless timing remains required.

**Locations:** `arch/arm/common/os_arch_tickless.c:42-61`; `arch/common/os_arch_tickless.c:205-221,257-273`; `soc/raspberrypi/rp2040/soc_cb.c:331-339,363-374`; `soc/raspberrypi/rp235x_arm/soc_cb.c:267-271,296-307`.

The Arm adapter disables TICKINT but leaves SysTick counting on its original grid. The SoC callback independently accumulates the duration of each sleep window. Those are different accounting schemes. Consider RP2040 sleeping from +0.1 to +0.9 ms of each 1 ms period, then running across the ordinary boundary. No boundary was suppressed; ordinary ISRs deliver 100 ticks in 100 ms. The sleep-duration accumulator nevertheless adds another 80 ticks from the 100 partial windows. These are planned long windows cut short by valid early wakeups. The 180/100 ratio is a constructed arithmetic example, not a board measurement.

RP235x Arm has the same independent-duration scheme with a millisecond POWMAN source whose phase need not match SysTick. This proof concerns LIGHT mode; it is not automatically a proof against STM32 DEEP, where clock stopping differs. RV32's comparator grid does not have this exact defect.

**Fix:** a single absolute tick grid, accounting only for missing boundaries and already-pending interrupts; alternatively stop/restart the periodic source with retained fractional phase. **Regression:** many early windows crossing zero/one/multiple SysTick boundaries and an independent reference timer.

### A09 - Eligible message producers can remain blocked

**Status: Fixed (2026-09-09).** Blocked producers record payload-plus-header space in existing wait metadata; receives wake every individually eligible producer. Each producer retries the locked capacity check using its original timeout budget. Validation: real-source multi-producer, oversized-head, contention/remaining-budget and maximum-size metadata regressions; original source fails.

**Locations:** `kernel/os_msg.c:105-116,138-151,220-228`; `kernel/os_task.c:1415-1438`.

A receive wakes only one producer. A 32-byte buffer full of one 30-byte payload can have two producers waiting forever to send one byte each (three bytes including header). Receiving the large message wakes H; H succeeds and leaves 29 bytes free, but only wakes a receiver. L stays blocked although its message fits. A second case wakes a large-message producer that cannot fit while a smaller eligible producer remains asleep.

**Fix:** record required space and wake satisfiable producers, relaying/repeating wakeups while space permits. Waking all producers to retry under the existing lock is a simpler first correction, with performance measured afterward. **Regression:** multiple blocked producers with equal and unequal payload sizes, finite and infinite waits, and no later receive to accidentally rescue the stranded task.

### A10 - Installer can delete its own update source

**Status: Fixed (2026-09-09).** Installers reject equal, nested or aliased source/destination trees before mutation. Online --update downloads the requested ref unless --source explicitly selects a local checkout. Validation: isolated path-rejection and mocked online-update regressions.

**Locations:** `tools/install_stm32_offline.py:141-168`, `tools/install_stm32_online.py:185-213` and corresponding RPi bootstraps; `tools/internal/engine.py:655-659,518-529`; `tools/internal/stm32.py:472-474` and `raspberrypi.py:400-402`.

Running an installed bootstrap from `<project>/AhuraRTOS/tools` with `--update`, or explicitly using that installed directory as `--source`, can select the installed checkout as both source and destination. The bootstrap prefers the checkout beside itself; the update flag requests a tree copy; `apply()` recursively deletes the destination before copying the source. It therefore destroys the source and fails. Executing this backend path in a disposable valid checkout fixture produced `FileNotFoundError` and left the checkout absent.

**Fix:** canonicalize and reject/handle identical or overlapping source/destination trees before mutation; stage the entire incoming tree first. An online update should explicitly select/download the requested revision rather than silently prefer the installed source. **Regression:** actual CLI `--update` and `--source` tests with identical, nested and separate paths, including Windows path aliases. No real checkout was deleted during this audit.

### A11 - Installer rollback is not transactional

**Status: Fixed (2026-09-09).** Every replacement is staged before commit; original files/trees are retained by same-filesystem rename. Rollback inspects actual rename results so cancellation between a rename and Python bookkeeping cannot lose the original. Recovery failure, including cancellation, retains backups and reports their location. Validation: copy/template failures, every commit rename, rollback errors and interrupted commit/recovery tests. This is caught-error recovery, not a power-loss journal.

**Locations:** `tools/internal/engine.py:512-553`.

The existing tree is deleted without a backup. If copying the replacement fails, rollback cannot restore it. With `--force-templates`, an existing user file is overwritten and then added to the `created` list; failure later unlinks it rather than restoring its original bytes. Both paths lost original data in controlled fixtures. The function's promise to leave no half-install is therefore incorrect.

**Fix:** stage replacements on the same filesystem, retain originals/backups until all operations commit, distinguish created files from replaced files, and restore exact original bytes/metadata on failure. Also preserve the BOM when restoring edited text; the current reconstruction omits it. **Regression:** inject failure at every copy/replace step, verify exact original hashes, and verify failed staging leaves a usable checkout and configuration.

### A12 - Mutex-disabled builds fail

**Status: Fixed (2026-09-09).** The private mutex declaration is guarded by the mutex feature flag. Validation: all 17 kernel modules compile with all optional features disabled and with only mutexes disabled.

**Location:** `kernel/os_internal.h:533`, outside the mutex feature guard ending at line 445.

`os_task_mutex_blocked_on_set(const os_mutex_t *, ...)` is declared when the feature-disabled public header has removed `os_mutex_t`. Minimal compilation fails in 16 of 17 kernel translation units; changing only `OS_CONFIG_MUTEX_ENABLE` to zero reproduces the error in `os_kernel.c`.

**Fix:** guard the declaration consistently with the type and implementation. **Regression:** all-off plus each-feature-disabled C builds; this is a direct counterexample to the promise that every feature independently compiles away.

### A13 - Message-disabled C++ header fails

**Status: Fixed (2026-09-09).** The C++ linkage block consistently surrounds the message feature guard. Validation: actual C++17 umbrella-header compilation with messages enabled and disabled; disabled baseline fails.

**Locations:** `kernel/os_msg.h:37-50,186-190`; resulting error at `ahura.h:163`.

The `extern "C"` opening brace is inside the message feature conditional; the closing brace is outside. C++17 inclusion fails when messages are disabled. Otherwise identical enabled-feature probes compile.

**Fix:** place both linkage braces at the same conditional level. **Regression:** C and C++ public umbrella-header checks for every supported feature profile, plus public macro usage in the documented C++ standard.

### A14 - Small message destination strands another receiver

**Status: Fixed (2026-09-09).** An undersized receive relays the receiver wake; a successful receive also relays it when messages remain. Validation: real-source multiple-receiver regressions preserve FIFO payloads and prove progress; original source fails.

**Locations:** `kernel/os_msg.c:113-114,201-217,233-239`.

H waits with a four-byte destination and L with a 16-byte destination. An eight-byte message arrives, waking H. H returns the documented INVALID_ARG without consuming the message, but does not relay the wake to L. L remains blocked despite a suitable queued message.

**Fix:** relay wakeups on destination rejection or use receiver size metadata with a documented selection/error policy. **Regression:** two blocked receivers of different capacities, one send, and no later send to mask the failure. The existing same-task retry test does not cover this.

### A15 - Mandatory initialization failures are discarded

**Status: Fixed (2026-09-09).** Mandatory idle/timer/log/main/test creation and start results are checked. Startup completion is tracked, invalid startup/restart traps even with assertions disabled, and enabled task priorities/stacks have compile-time checks. Validation: executed boot success/failure cases, partial idle initialization retry, start-before-init and negative main-priority compilation.

**Locations:** `kernel/os_kernel.c:131-157,169-176,382-406`; architecture tick-init failure paths also return void.

For example, `OS_CONFIG_MAIN_TASK_PRIORITY=0U` compiles, but main-task creation rejects it. `os_init()` discards the status and `os_start()` checks only the idle task. The system can run idle/services forever without running the application. Service creation and a port's stack-initialization failure similarly need explicit handling. Checking core 0's idle task alone does not establish all secondary idle stacks exist.

**Fix:** static configuration validation plus a checked initialization result or unconditional diagnosed failure before scheduler start. Handle invalid clock/reload failure in the same way; release builds must not lose the check when assertions are disabled. **Regression:** invalid built-in task priorities/stack sizes and injected service/secondary-idle initialization failures.

### A16 - External-tick mode still touches SysTick

**Status: Fixed (2026-09-09).** SysTick silence/restore/phase adjustment is compiled only when the kernel owns SysTick. External-tick builds use their callback-owned suppression path. Validation: external-tick ARM compile/source probes; application-timer hardware integration remains a port acceptance test.

**Locations:** `arch/arm/common/os_arch_tickless.c:42-61`; callers in `arch/common/os_arch_tickless.c:205-221,257-273`; ownership contract in `arch/arm/common/os_arch_port_common.h:201-217`.

With an external kernel tick and a usable SoC suppress backend, the Arm adapter still clears/restores SysTick TICKINT unconditionally. An application using its promised application-owned SysTick timebase has that timebase suppressed during the window.

**Fix:** access SysTick only when the kernel owns it; put external-source suppression wholly behind its own backend. **Regression:** register-mock test requiring zero SysTick writes in external mode, followed by a board test with an independent SysTick application timebase.

### A17 - CPU usage snapshot/reset races on SMP

**Status: Fixed (2026-09-09).** CPU usage counter increments and tickless additions take the same critical section as snapshot/reset. Validation: producer/consumer lock-path review and SMP builds. Sampling remains tick-based, and no hardware performance or latency claim is made.

**Locations:** `kernel/os_tick.c:70-79,142-147,195-198,239-245`.

`os_cpu_usage_get()` reads and resets counters under the global critical lock; the core-0 tick increments them without taking that lock. A core-1 reader can mix numerator and denominator from different intervals or overwrite new increments during reset. The assumption that only core 0 writes is false because the reader resets are writes. The metric also describes core 0 rather than aggregate multicore load.

**Fix:** per-core cumulative counters with a defined consistent snapshot/delta method, or have core 0 own sampling/reset. Document the metric. **Regression:** controlled remote snapshot interleavings, tickless batches and accounting for every tick exactly once.

### A18 - Dropped-log count is not cumulative

**Status: Fixed (2026-09-09).** Lifetime dropped-log count is separate from pending loss-report credit. The consumer emits notices directly outside the lock so concurrent ring refill does not discard a consumed notice. Validation: real-source drain, concurrent producer and full-ring notice regressions; original source fails. The lifetime counter wraps modulo 2^32.

**Locations:** `kernel/os_log.c:182-194,290-293`; `kernel/os_log.h:82-85`.

The getter promises a cumulative dropped-line count, but the drain task resets that same counter to zero when preparing a loss notice. Pollers can miss loss counts or wrap a subtraction-based delta. Concurrent producers can also fill the ring before the notice is enqueued.

**Fix:** separate lifetime total from pending-report count and preserve unreported losses until the notice is committed. **Regression:** overflow, drain and repeated polling with concurrent producers; verify the total never resets unexpectedly.

## 3. Additional contract and hardening work

These items matter, but should not be confused with independently reproduced valid-use failures above.

| Item | Evidence and recommendation |
|---|---|
| PI chain depth | `os_internal.h:474-482` and `os_task_mutex.c:265-287` cap traversal at eight. This is a documented-in-source implementation limit, not arbitrary-chain inheritance. Separate deadlock-report capacity from correctness; bound traversal by live tasks with cycle handling, or explicitly document/enforce the supported limit. |
| PI timeout departure | **Fixed with A03.** Inheritance is recomputed at the actual unlink transition before the waiter is dispatched; timeout/pause/forced-wake regressions exercise it. |
| Watermark identity/lifetime | `os_task.c:800-850` scans outside the lock then resolves the handle's current ID, without preserving the original generation. Reuse of the same handle/stack can pass the recheck. Capture the generation; define and protect stack storage lifetime. A check after scanning cannot make freed storage safe. |
| Same-handle concurrent creation | `os_task.c:2067-2105` can write stack memory before the second live-handle check. The source treats concurrent construction as caller misuse. Document required serialization or reserve a construction state before touching the stack. |
| Dynamic queue reuse | `os_queue.c:466-482,541-550` retains OVERWRITE through cleanup/re-init, while `os_queue.h:145-146` says every queue starts NORMAL. Decide whether configuration persists; reset mode or document that persistence. Treat this as lifecycle ambiguity until the intended contract is settled. |
| Invalid allocator pointers | `os_mem.c:184-200` dereferences the candidate header before the later alignment check. A misaligned in-heap foreign pointer can fault on strict-alignment CPUs. Validate integer address, alignment and full header bounds first; narrow the broad foreign-pointer promise if misuse is outside the API. No valid-allocation coalescing defect was confirmed here. |
| Object lifetime after wake | Cleanup's empty waiter list does not prove there are no READY tasks with in-flight calls or `woken_from` references. Define quiescence requirements before freeing an IPC object; BUSY is not a complete lifetime guarantee. |
| Re-init without cleanup | Queue/message initialization can leak an existing allocation, but their zeroed-object precondition makes this misuse. A checked lifecycle can reject it more clearly. |
| Libc and TLS | Logging formats outside the kernel lock, and RV32 deliberately omits task-specific `tp`. Specify task-local libc/errno/TLS and reentrancy support; buffer serialization alone does not make libc internals safe. Avoid implying full language-runtime integration from C++ header compatibility. |
| STM32 external-tick installer path | `tools/internal/stm32.py:297-299` returns before PendSV/SVC checks, although an external tick does not surrender PendSV ownership. Validate non-tick vectors independently. Also test install/uninstall/regenerate: uninstall currently does not restore removed `.ioc` IRQ-generation settings. |

## 4. Portability and source organization

### Preserve the existing boundaries, strengthen their contracts

Keep the existing kernel file structure. Strengthen contracts and add regression coverage within the current modules, while keeping the public `#include "ahura.h"` and ordinary application integration stable. Folder reorganization is outside this remediation scope by user request.

The portable core currently assumes downward-growing stacks, a `uint32_t *` saved context, a canary at the lowest address, minimum stack geometry partly hard-coded in `os_task.c`, a flat shared address space and compiler/port attributes. The available implementations are privileged Cortex-M and RV32, with homogeneous scheduling cores. This is a bounded portability model, not yet a contract for any architecture or any memory system.

Define one architecture-neutral port interface and one common configuration schema. The 1,395-line ARM and 1,016-line RV32 common headers currently duplicate kernel options, constants and validation. A new port should supply CPU/ABI facts, not copy the entire kernel configuration policy.

| Layer | Owns | Must explicitly specify |
|---|---|---|
| Compiler adaptation | Attributes, alignment, inline/noreturn/weak, barriers/intrinsics | Supported compilers and language versions; no silent unsupported fallback |
| Kernel | Scheduling, IPC state machines, timeout semantics, allocation policy | Locked/unlocked helper rules, task/object lifetime, wait completion, ownership handoff |
| Architecture/ABI | Context frame, exception entry/return, local masking, register banks | Context representation, stack alignment/direction/headroom, IRQ stack, FP/MVE/TZ/TLS state, migration-safe handoff |
| SoC | Timer hardware, clock domains, core IDs/start, IPIs, spinlocks/cache facts | Monotonic source frequency/wrap, sleep survival, IRQ priority constraints, coherent/exclusive memory support |
| Board | Oscillators, memory placement, timer/pin reservations, power wiring | Actual resources and board sleep constraints |
| Application | Feature/priority/stack budgets, callbacks and product sleep policy | Object ownership, callback blocking rules, selected supported configuration |

Board resources are currently expressed through application/SoC configuration; a separate board layer can be introduced gradually when multiple boards share a chip. The important distinction is ownership, not a compulsory new directory.

### Retained organization

`kernel/` keeps its existing modules and private headers. Architecture/ABI code remains in `arch/`, hardware integration in `soc/`, application configuration in its existing project folders, and tooling in `tools/`. The new standalone tests are under `test/audit_kernel`, `test/audit_ipc`, `test/audit_arch` and `test/installer`.

Improve portability through explicit interfaces, capability checks and port acceptance tests. No proposed `kernel/internal`, `kernel/sched`, `kernel/ipc` or other kernel directory split is part of the plan. Public include paths should expose public APIs; the current repository-root include path still permits private-header inclusion, which remains an API-boundary improvement to evaluate without moving kernel files.

### Port acceptance contract

A new architecture/SoC port should be accepted only when it supplies and validates all of these:

1. Explicit family/ABI/compiler selection; compile-time rejection of incompatible flags or unsupported register extensions.
2. Initial context and save/restore of every task-visible register bank, with task return, nested IRQ and stack alignment tests.
3. A documented point where outgoing stack ownership ends and another core may dispatch the task.
4. Save/restore interrupt masks, nesting behavior, ISR-callable priority rules, barriers and shared-memory atomic capabilities.
5. A monotonic clock contract: units, width, wrap, phase, variable frequency, sleep behavior, and synchronization before new deadlines are armed.
6. One owner for each vector/timer; no external-tick writes to unrelated hardware; checked startup/link extraction.
7. SMP core mapping, bring-up, IPI delivery, shared-memory/cache rules and resource reservation.
8. Feature/ABI compile tests plus the relevant on-target functional, migration and timing tests, with archived evidence.

The timer contract should expose one coherent time origin. Preserve absolute deadlines or reconcile elapsed time under synchronization; account for fractional phases once. Do not make Cortex-M SysTick the conceptual name for every architecture's default tick source. Define hardware capabilities separately: scalar FP, integer MVE, DWT, global exclusive support, coherent memory, stack-limit support and TrustZone are different facts.

### Build and coding-style improvements

- Use target-scoped CMake requirements such as `target_compile_features(ahura_kernel PUBLIC c_std_11)`. State compiler/ABI requirements on the target instead of relying on the parent project to set global C flags.
- Let a port/SoC descriptor state family and variant explicitly. Keep flag-based detection as a convenience with precise diagnostics. Current CMake messages still say only Arm is ported, and SoC variant knowledge cannot resolve the earlier family-selection failure on every toolchain layout.
- Keep `os_task.c` and the other kernel files in place. Clarify helper responsibilities and retain one authoritative mutation path for each state transition. Selectable groups for the separate board self-test remain a useful improvement because of G431 flash headroom.
- Keep shared implementation reuse, but mark textual includes clearly (`.inc` or a private implementation header) and keep the explicit source list. A private `.c` file that cannot be compiled independently is easy for new build-system users to misuse.
- Add the actual `.clang-format`/`.editorconfig` and a check target. `CSTYLE.md` refers to supporting files that are absent, contains an early-return summary that conflicts with its later single-exit rule, and reads partly like copied skill instructions. Make it a concise, internally consistent repository policy.
- Keep naming, braces and license headers consistent, but prioritize contracts, bounds and critical-section invariants over decorative alignment. Move historical explanations out of long hot-path comments into design records; retain the reason an invariant is necessary.
- Correct stale documentation: callback weak/strong ownership, RISC-V support, tickless status, public/private header exposure and configuration ownership. Record hardware-verified configurations separately from merely compiled wrappers.
- If MISRA conformance is a project objective, define scope, tool coverage and documented deviations and run an actual checker. A style guide and single-return functions do not establish conformance; no MISRA assessment was performed here.

## 5. Performance plan

Performance changes should follow the ownership/timekeeping fixes. This audit did not measure target cycles or establish a hard interrupt-latency bound. Existing best/worst microbenchmark samples are useful observations, not WCET proofs.

| Hot path | Current characteristic | Improvement to evaluate | Measurement required |
|---|---|---|---|
| Single-core dispatch | Bitmap + FIFO ready head, O(1) | Preserve it; avoid unnecessary abstraction on the hot path | Switch latency with integer/FP contexts and code in flash/RAM |
| SMP dispatch | Shared global lock and affinity/ownership scans | Targeted IPIs; later per-core eligible queues if profiling justifies them | Lock hold/spin time, migration correctness, fairness and maximum dispatch delay |
| Timeout/wait insertion | Ordered/delta lists, O(n) insertion | Keep short no-expiry tick; choose bounded alternatives only for needed scale | Maximum tasks/waiters, cancellation and clustered expiry bursts |
| Queue/message copying | Payload copy occurs in global critical section | Descriptor/pointer queues or fixed-size pools for large transfers | Largest configured payload, interrupt masking and remote-core blocking |
| Heap | First-fit scan and coalescing under lock | Optional fixed-block pools or bounded allocator for strict-latency applications | Fragmented worst case, failure path, peak/free/largest-block statistics |
| Timer expiry bursts | Periodic timers reinsert into sorted list under lock | Avoid repeated full insertion scans; choose coalescing/phase policy explicitly | Many equal expiries; possible quadratic burst cost; callback backlog |
| Message ring arithmetic | Runtime modulo in cursor paths | Conditional wrap/subtraction where correct | M0 software-division cost and realistic message lengths |
| Logging | Libc formatting outside lock; buffer serialization inside | Small bounded formatter or specified reentrant libc; reserve loss reports | Stack usage, maximum format latency, sustained overflow and drain throughput |
| Tickless | Multiple timer/cycle conversion paths | Shared monotonic contract before optimization | Drift vs hardware time, early wake distribution, IRQ count and power |

For periodic timers, explicitly choose whether late delivery preserves phase, coalesces missed expiries or reschedules from delivery. `os_timer.c:599-605` currently reloads a full period after multi-tick announcement overshoot. Preserve phase with a remainder if that is the intended contract, without accidentally producing an unbounded callback catch-up burst.

## 6. Regression and reliability gates

The built-in self-test is substantial and exercises real ISR/SMP paths, but it cannot currently serve as the sole regression gate. `.github/` contains funding metadata and no test workflow. Make the following reproducible and automatic.

| Gate | Minimum contents | Success criterion |
|---|---|---|
| Compile matrix | All-off, all-on, each feature off, no allocator, UP/SMP, C11/C++17, supported ABIs, integer MVE, TZ modes | Supported configurations compile/link; invalid combinations fail with a specific diagnostic |
| Deterministic kernel tests | Controlled clock and switch hooks; IDs/generations, list membership, wake credit, PI and task ownership | Every task belongs to the right state/list; one executing core per task; no lost eligible wake |
| Timing tests | Zero/one/many missed boundaries, all early-wake phases, tick wrap, mixed ordinary/suppressed ticks, new remote deadline | No early expiry beyond stated tick quantization; bounded late jitter; no accumulated drift |
| Context tests | Distinct integer/FP/MVE register patterns, nested IRQs, task return, deep stacks and rapid migration | Every supported state survives; no shared live task stack |
| Stress/lifetime | Create/start/block/wake/pause/delete under contention; object cleanup/reuse; tiny buffers and allocation exhaustion | No leaked TCBs/objects, stale owners, list corruption or unaccounted messages |
| Installer/integration | Dry-run/idempotence, install/update/uninstall, same/nested paths, failure injection, CubeMX regeneration | Correct vector ownership and exact rollback of originals; no self-deletion |
| Six-board hardware | Repeatable runner, timeout/watchdog recovery, raw console logs and exact build/config hashes | Suite finishes and every required case runs; SKIP is not PASS |
| Performance/soak | Interrupt latency and lock bounds under maximum configured load; long-running tickless/SMP tests | Project-specific budgets met and sustained operation with reference-clock checks |

Include assertions-on and assertions-off builds, BASEPRI and PRIMASK where supported, tickless on/off, non-1-kHz rates, near tick-counter wrap and multiple compiler optimization levels. Initial compile probes here mostly used soft ABI; the existing STM32/Pico application builds cover their current production flags, not every ABI combination. Clang/armclang and TrustZone runtime support still need separate evidence.

Record the exact revision, configuration hash, compiler/SDK version, board revision, clock/sleep source, enabled features and raw results with every hardware run. Report feature-specific PASS/SKIP and failure location instead of concluding all RTOS features were verified from a build with disabled modules.

## 7. Remaining work after these fixes

1. Run the board self-test on all six targets and retain exact firmware/configuration hashes and console logs.
2. Stress SMP ownership, deep-stack migration and timing against an independent hardware reference; exercise early wake, new remote deadlines, IRQ priority modes and long-duration drift.
3. Validate integer-MVE and TrustZone register/context behavior on appropriate targets before claiming runtime support for those profiles.
4. Resolve the separately listed lifecycle and API-contract items in Section 3, including the explicit priority-inheritance depth limit.
5. Measure interrupt masking, cross-core waiting, stack headroom and flash headroom; apply performance changes only against measured budgets.
6. Improve configuration/port contracts and build automation while preserving the existing kernel file structure.

The completed A01-A20 fixes and deterministic tests are a reliability improvement, not an unqualified promise of no future errors.

## 8. Evidence and reproduction files

Audit working directory: `E:\GitHub\ahura_testing\stm32h503rb\.audit-work`.

| Artifact | Purpose |
|---|---|
| `source-manifest.json`, `source-comparison.json`, `project-configs.json` | Reviewed file hashes, six-copy equality and configuration inventory |
| `build_matrix.py`, `build-results.json`, `builds/<project>/*.log` | Six isolated configure/build commands and results |
| `compile_probes.py`, `compile-probes/*/results.json` | Real-source architecture, feature and C++ compiler probes; negative cases are intentionally retained |
| `arch/m*-integer-mve.s` | Compiled integer-MVE context-switch assembly |
| `arch_models.py`, `arch/model-results.json` | Explicitly labeled timing/counter/stack-overlap models |
| `ipc/run_header_probes.py`, `ipc/header_probe_results.json` | Enabled/disabled message C++ control probes |
| `ipc/wakeup_models.py` | Source-linked message waiter schedules |
| `installer_repros.py`, `installer-repro-results.json` | Actual backend failure injection, using only disposable audit fixtures |
| `installer-dry-run-results.json`, `*-installer-dry-run.log` | Read-only installer checks against the six real projects |
| `kernel-findings.md`, `ipc-findings.md`, `arch-findings.md` | Detailed supporting review notes, including rejected suspicions and scope limits |

The original scripts and their recorded results above are **historical baseline evidence**.
To reproduce baseline failures, first point them at a separate checkout of the audited revision
and use separate output directories. Do not run their original hard-coded commands against the
updated master: some expect the bug to remain present and overwrite the baseline JSON/logs.
Use the remediation commands and standalone test runners below for the fixed source.

The scripts use the local paths/tool versions recorded above. The models are intentionally not described as RTOS execution. The installer repro script creates fresh fixtures under `.audit-work`; it must never be redirected to a real checkout as its disposable fixture directory. The source manifest was rechecked during review with no production source changes detected.

### Remediation evidence (2026-09-09)

The source baseline and initial audit artifacts above remain unchanged for comparison. Fixed-source evidence is separate:

| Artifact | Result / purpose |
|---|---|
| `fix-build-results.json`, `fix-builds/<project>/*.log` | All six Release builds pass using isolated project copies with unchanged application configuration |
| `fix-build-source-manifest.json` | Exact RTOS file hashes supplied to the staged build matrix |
| `fix-compile-probes/summary.json` and per-probe results | 23 probe groups: supported cases pass; invalid main priority fails as required |
| `test/audit_kernel/` | Ten real-source ARM scenarios pass; README documents hardware substitutions |
| `test/audit_ipc/`, `ipc-fixed-build/results.json` | Eight real-source IPC/log scenarios and two C++ header probes pass |
| `ipc-baseline-build/results.json` | Original-source controls fail the eight intended runtime regressions and disabled-message C++ check |
| `test/audit_arch/`, `arch-tests/results.json`, `arch-port-tests/results.json` | Sixteen targeted timing/port scenarios pass; controlled MMIO, not a cycle-accurate peripheral or dual-CPU simulation |
| `test/audit_arch/compile_run.py`, `arch-compile-tests/results.json` | Seven checks: integer-only M52/M55/M85 save/restore, minimum MVE stack rejection, M0/M33 external tick, and assembled RV32 scheduler-stack handoff |
| `fix-build-input-verification.json` | Compiled RTOS hashes match; all six application configurations unchanged; RP Arm reference capability is present in the actual compiler commands |
| `fix-verification/results.json` and suite logs | Final standalone regression/compiler run, recorded separately from board builds |
| `test/installer/test_transactions.py` | Twelve disposable install/update/rollback, path and cancellation regressions pass |
| `fix-kernel-status.md`, `fix-ipc-status.md`, `fix-arch-status.md` | Detailed implementation and validation notes in the local audit work directory |
| `fix-sync-manifest.json`, `fix-sync-results.json` | Changed/new-file hashes, pre-write drift check, retained backup location and final synchronization verification |

Reproduce staged build/configuration checks from the H503 project:

```powershell
python -B .audit-work\stage_fix_projects.py
python -B .audit-work\build_matrix.py --staged
python -B .audit-work\compile_probes.py --staged
```

Run the standalone regression commands documented in each `test/audit_*/README.md`;
run installer tests from the RTOS root with:

```text
python -B -m unittest discover -s test/installer -v
```

The new test programs are intentionally outside `test/CMakeLists.txt` and execute separately;
they do not add test payload to the six board firmware images.

Remediated STM32 linker sizes (application, vendor support and self-test included):

| Project | Linker memory use |
|---|---|
| `stm32h503rb` | RAM:       21280 B        32 KB     64.94%; FLASH:      124488 B       128 KB     94.98% |
| `stm32g431rb` | RAM:       21496 B        32 KB     65.60%; FLASH:      130524 B       128 KB     99.58% |
| `stm32h743zi` | DTCMRAM:       24872 B       128 KB     18.98%; RAM:           0 B       512 KB      0.00%; FLASH:      174044 B         2 MB      8.30% |

The retained boot stack is now used by the RV32 scheduler. A new RV32 port must reserve that per-hart storage for the scheduler's lifetime and size it for scheduler/exception call depth; this audit did not measure its high-water mark on hardware.

## 9. Tickless configuration follow-up

### A19 - Tickless-disabled Pico 2 Arm fails to link

**Status: Fixed (2026-09-10).** With `OS_CONFIG_TICKLESS_ENABLE=0U` and two cores,
the complete Pico 2 Arm firmware failed to link with undefined references to
`soc_ipi_arm` and `os_arch_core_ipi_request_cb`. The original tickless `#if` in
`soc/raspberrypi/rp235x_arm/soc_cb.c` incorrectly enclosed the entire inter-core
interrupt implementation. Its final `#endif` comment named a core-count guard
which had never been opened.

The tickless block now ends before the IPI code, which has its own
`OS_CONFIG_CORE_COUNT > 1U` guard. The tickless reference-clock callback is
compiled only with tickless enabled; ordinary busy-wait reference clocks remain
available in both modes. All three RP packages diagnose a missing sleep-mode
setting only when tickless is enabled.

All three Raspberry Pi `soc_config.h` templates and the three existing Pico
application headers now include `os_config.h` and guard their sleep-only settings
with `#if (OS_CONFIG_TICKLESS_ENABLE == 1U)`. The STM32 template and all three
STM32 application headers already had the guard. Existing configuration values
are preserved, including Pico 2 Arm's disabled tickless setting. No kernel file
or folder changes are part of this follow-up.

**Coverage correction:** the earlier six-project matrix tested each project's
selected configuration, while the optional-feature compile probes covered kernel
and architecture units. It did not compile every complete SoC integration with
both tickless settings. That gap allowed this link defect to survive. The new
matrix uses the complete current project inputs and changes the tickless/core
settings only in isolated copies.

| Complete firmware project | Tickless off | Tickless on |
|---|---|---|
| `stm32h503rb`, 1 core | PASS | PASS |
| `stm32g431rb`, 1 core | PASS | PASS |
| `stm32h743zi`, 1 core | PASS | PASS |
| `rpi_pico`, 2 cores | PASS | PASS |
| `rpi_pico2_arm`, 2 cores | PASS | PASS |
| `rpi_pico2_riscv`, 2 cores | PASS | PASS |
| `rpi_pico2_arm`, 1 core (additional coverage) | PASS | PASS |

The unmodified Pico 2 Arm project reproduced the link failure before the patch.
Fourteen corrected complete Release firmware builds pass. Separate real-SDK
SoC compile/object-symbol checks cover all three RP packages with tickless on/off
and one/two cores (12 cases), plus three enabled-only missing-setting rejection
cases. Twelve header-first preprocessing checks cover the three RP template and
application headers with tickless on/off.

Run the permanent SoC regression against a configured Pico SDK project:

```powershell
python -B test/audit_arch/soc_compile_run.py --compile-commands <project-build>/compile_commands.json --build <temporary-output>
```

Run it once with each RP2040, RP235x Arm and RP235x RISC-V compilation database.
It compiles the current RTOS checkout with isolated configurations and checks the
actual object symbols. It leaves the application's configuration unchanged.

Evidence under `E:\GitHub\ahura_testing\stm32h503rb\.audit-work\tickless-review`:

- `builds/baseline/current/rpi_pico2_arm/build.log`: reproduced unmodified failure.
- `candidate-build-results.json`, `builds/candidate/<profile>/<project>/*.log`: 14 complete builds.
- `soc-probes/<project>/results.json`: 12 SoC combinations and 3 negative checks.
- `config-guard-results.json`: 12 header preprocessing checks.
- `source-fix-status.md`, `config-fix-status.md`: implementation and verification details.
- `sync-manifest.json`, `sync-results.json`: exact patches, original backups and final copy verification.

These are compile/link and preprocessing results. Hardware behavior and timing
remain separate acceptance gates; passing this matrix is not a claim that every
RTOS feature combination has been tested.

## 10. Pico 2 Arm logging self-test follow-up

### A20 - Self-test rejects the corrected cumulative dropped-log counter

**Status: Fixed (2026-09-10).** The user-provided Pico 2 Arm console log reports
753 passed and one failed check with two cores and tickless disabled. The failing
check at the original `test/os_test.c:3887` expects the dropped count to become
zero after its notice is delivered, but reads 482.

The A18 change made `os_log_dropped_get()` honor its documented cumulative
contract: reporting consumes only the private pending-notice count. The board
self-test assertion was missed in that remediation. The reported value is
consistent with the corrected kernel behavior; this failure does not implicate
tickless or the Arm context switch.

The self-test now snapshots the count after reporting and checks that it equals
the count saved after the controlled flood. Its recovery phase also checks that
an already reported loss is not emitted again. Both checks remain active; the
failure is not removed by skipping the test or resetting the kernel counter.
Kernel sources, their file/folder structure and application configurations are
unchanged by this follow-up.

**Validation:** all six complete Release integrations compile and link with the
corrected self-test and their current application settings. The unchanged IPC
regression runner passes eight real-source deterministic ARM scenarios and two
C++ header checks, including cumulative-count retention with a concurrent
injected loss and notice delivery when the ring is refilled. These emulated
checks verify the logging contract independently of the board assertion.
**User-reported board rerun (2026-09-10): 755 passed, 0 failed.** After the
corrected Pico 2 Arm firmware was rebuilt, the user supplied this successful
self-test summary and its benchmark output: ARMv8-M mainline, FPU/DSP,
150 MHz CPU and 1 kHz tick. The rebuilt project retained two cores with
`OS_CONFIG_TICKLESS_ENABLE=0U`. This confirms the reported A20 failure is resolved
in that run; it does not add tickless LIGHT or DEEP runtime coverage.

The reported context-switch estimate is 401 cycles (2673 ns) best and 1369
cycles (9126 ns) worst observed across 2000 samples. These are the benchmark's
per-switch estimates from a two-switch round trip, not guaranteed latency bounds.
The user-provided excerpt is summarized in `board-rerun/user-result.json` below.

Evidence under `E:\GitHub\ahura_testing\stm32h503rb\.audit-work\log-selftest-review`:

- `user-pico2-arm-before.txt`: supplied runtime failure report.
- `candidate-build-results.json`, `builds/candidate/current/<project>/*.log`: six complete builds.
- `ipc-regressions/results.json`: deterministic regression checks and compiler command.
- `sync-manifest.json`, `sync-results.json`: reviewed file changes, backups and copy verification.

## 11. Pico 2 Arm two-core DEEP sleep

### A21 - Shared-clock DEEP sleep rejected with two cores

**Status: Fixed in source (2026-09-10); hardware validation pending.** The user's
current Pico 2 Arm configuration enables tickless idle, two cores and DEEP sleep.
The unchanged project reproduced the explicit single-core-only compilation error.
The missing piece was coordinated idle and wake-up in this RTOS SoC port.

The kernel now offers paired SoC prepare/finish callbacks in its existing files.
Preparation runs with the local scheduling mask held, without the global lock,
before opening the tickless window. Deadlines are sampled after preparation.
Every successful prepare is finished, including a plan too short to sleep.
Finish runs after clocks, elapsed-time accounting and remote kernel entry are
restored. Other SoCs use the default no-op preparation and finish behavior.

The RP235x Arm port requests a numbered idle acknowledgement from core 1, waiting
at most 100 microseconds before falling back. Core 1 only acknowledges from idle,
saves its interrupt/SysTick/sleep state, and parks with configurable interrupts
masked. Pending peer work signals core 0 through the actual scheduling IPI, so it
wakes the owner's WFI; an event alone would not suffice. Core 1 remains parked
until the owner restores the clock tree and kernel time. Cancellation, late
acknowledgements and repeated requests cannot authorize a different generation.

DEEP now means a preference for PLL_SYS-off idle when eligible. A busy peer or
clock-dependent peripheral uses tickless LIGHT with normal clocks. Initial local
pending work declines the pass. The port rechecks activity immediately before
changing clocks, saves/restores PLL power and clock selection, and retains XOSC.
This does not implement the deepest POWMAN power-off state. RP2040 and RP235x
RISC-V DEEP support remain unchanged.

Eligibility covers DMA, PIO, PWM, relevant serial/HSTX activity, direct PLL_SYS
clock consumers and clock outputs. An enabled USB controller also vetoes DEEP
because slowing clk_sys while retaining clk_usb violates the RP2350-E12 clock
requirement. External activity that registers cannot identify, such as polled
asynchronous UART receive, must be covered by the optional
`soc_deep_sleep_allowed_cb()` board veto. This callback must not wait or call
kernel APIs. See `doc/raspberry-pi.md` and `doc/porting.md` for the contracts.

**Verification: 18 complete Release firmware builds pass**, with no compiler or
linker warnings in these production build logs. The checked production sources
match the staged implementation in every build:

| Configuration | Complete build results |
|---|---|
| All six projects, tickless off and on | 12 PASS |
| Pico 2 Arm LIGHT, one and two cores | 2 PASS |
| Pico 2 Arm DEEP, one core | 1 PASS |
| Pico 2 Arm tickless off, one core | 1 PASS |
| Pico 2 Arm DEEP, two cores, FIFO IPI | 1 PASS |
| Pico 2 Arm DEEP, two cores, PRIMASK kernel masking | 1 PASS |

The main six-project on matrix includes Pico 2 Arm DEEP with two cores and the
default doorbell IPI. Other projects retain their selected sleep modes in that
matrix. Temporary variants change only isolated copies. The user's application
configuration values are preserved; only the Pico 2 Arm sleep-support comment is
updated. The kernel's file and folder structure is unchanged.

**53 deterministic real-source checks pass:** 11 kernel timing, 24 SoC protocol
and peripheral, 10 kernel lifecycle/concurrency, and 8 ARM port timing scenarios.
The SoC harness compiles the actual port against the installed Pico SDK headers
and runs two ARM CPU contexts with shared RAM and controlled register/event
behavior. It checks cancellation, timeout, stale idle hints, peer IRQ wake during
PLL-off sleep, pending-tick retention, saved masks/SCR/SysTick, restore-before-
release, board vetoes and peripheral vetoes. The synthetic port harness linker
reports RWX test-segment warnings; these are separate from production firmware.

The current G431 tickless-on self-test image uses 130,740 of 131,072 flash bytes
(332 bytes free). No features were disabled to obtain these results.

**Hardware acceptance remains open.** Rebuild/flash the Pico 2 Arm image and run
the board suite with two cores and DEEP selected. In a test-enabled image, inspect
`soc_sleep_deep_entries`: it must increase with both cores idle and eligible, and
stay unchanged during busy-peer/peripheral LIGHT fallbacks. Exercise a core-1-only
interrupt during sleep, repeated wake cycles, external time reference and power
measurements. A passing self-test or successful compilation alone does not prove
that the PLL-off path was entered. The earlier 755-check user run had tickless
disabled and is not runtime evidence for A21.

Evidence under `E:\GitHub\ahura_testing\stm32h503rb\.audit-work\smp-deep-review`:

- `builds/baseline/current/rpi_pico2_arm/build.log`: original two-core DEEP rejection.
- `verification.json`, `builds/candidate/<profile>/<project>/{result.json,configure.log,build.log}`: 18 complete builds and production source hashes.
- `kernel-tests/results.json`, `soc-tests/results.json`, `kernel-regressions.json`, `port-tests/results.json`: 53 deterministic checks.
- `sync-manifest.json`, `sync-results.json`: exact changes, backups and seven-copy verification.
