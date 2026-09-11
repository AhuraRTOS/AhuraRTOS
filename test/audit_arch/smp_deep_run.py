"""Run the actual RP235x Arm DEEP handshake with two emulated register contexts.

The SDK compilation database supplies real chip types and masks. Shared RAM is
mirrored between two Unicorn CPUs; WFE/SEV, timer reads and clock-ready transitions
are controlled, not cycle accurate. No board power/timing claim is made.
SPDX-License-Identifier: GPL-3.0-or-later
"""
from pathlib import Path
import argparse
import json
import struct
import subprocess
import sys

from soc_compile_run import command_arguments, setting


def compile_test(root, database, build):
    entries = json.loads(database.read_text())
    entry = next(e for e in entries if e['file'].replace('\\', '/').endswith('/rp235x_arm/soc_cb.c'))
    previous = Path(entry['file']).parents[3]
    config = (previous.parent / 'os_config.h').read_text()
    for key, value in [('OS_CONFIG_CORE_COUNT', '2U'), ('OS_CONFIG_TICKLESS_ENABLE', '1U'),
                       ('OS_CONFIG_TICKLESS_DEEP_ENABLE', '1U')]:
        config = setting(config, key, value)
    (build / 'os_config.h').write_text(config)
    soc = (root / 'soc/raspberrypi/rp235x_arm/template/soc_config.h').read_text()
    soc = setting(soc, 'SOC_CONFIG_IPI_DOORBELL', '1U')
    (build / 'soc_config.h').write_text(soc)
    args = command_arguments(entry)
    command = [args[0], '-I' + str(build)]
    skip = False
    for arg in args[1:]:
        if skip:
            skip = False
        elif arg in ('-o', '-MF', '-MT', '-MQ'):
            skip = True
        elif arg in ('-c', '-MD', '-MMD') or arg.replace('\\', '/') == entry['file'].replace('\\', '/'):
            continue
        else:
            command.append(arg.replace(str(previous), str(root)).replace(previous.as_posix(), root.as_posix()))
    linker = build / 'harness.ld'
    linker.write_text('SECTIONS { . = 0x10000; .text : { *(.text*) *(.rodata*) } '
                      '. = 0x20000000; .data : { *(.data*) } .bss : { *(.bss*) *(COMMON) } '
                      '/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) } }\n')
    board = build / 'board.c'
    board.write_text('#include <stdbool.h>\n#include <stdint.h>\nextern volatile uint32_t audit_board_allowed;\n'
                     'bool soc_deep_sleep_allowed_cb(void) { return audit_board_allowed != 0U; }\n')
    elf = build / 'smp_deep.elf'
    exports = ['audit_prepare', 'audit_finish', 'audit_peer', 'audit_sleep', 'audit_eligible',
               'audit_registers', 'audit_constants']
    command += ['-O1', '-Wall', '-Wextra', '-Wundef', '-Werror', '-ffunction-sections', '-fdata-sections',
                str(root / 'test/audit_arch/smp_deep_regression.c'), str(board), '-nostdlib', '-nostartfiles',
                '-Wl,--gc-sections', '-Wl,-T,' + str(linker), '-Wl,-e,audit_prepare',
                *['-Wl,--undefined=' + name for name in exports], '-lgcc', '-o', str(elf)]
    proc = subprocess.run(command, cwd=entry['directory'], capture_output=True, text=True)
    (build / 'compile.log').write_text(proc.stdout + proc.stderr)
    if proc.returncode:
        raise RuntimeError(proc.stdout + proc.stderr)
    nm = Path(args[0]).with_name(Path(args[0]).name.replace('gcc', 'nm'))
    symbols = {f[2]: int(f[0], 16) for line in subprocess.check_output([str(nm), '-n', str(elf)], text=True).splitlines()
               if len(f := line.split()) == 3}
    return elf.read_bytes(), symbols, command


def run_tests(data, symbols):
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    from unicorn.arm_const import UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_PRIMASK
    regs = ['sys_ctrl', 'sys_div', 'sys_selected', 'ref_ctrl', 'ref_selected', 'peri_ctrl',
            'pll_cs', 'pll_pwr', 'pll_fb', 'pll_prim', 'icsr', 'scr', 'iser', 'ispr', 'systick',
            'cpuid', 'doorbell', 'dma', 'uart_cr', 'uart_fr', 'uart_imsc', 'pio', 'pwm',
            'spi_cr', 'spi_sr', 'i2c_enable', 'i2c_con', 'hstx', 'usb_clk', 'gpout', 'usb_main']
    constants = ['sys_src', 'ref_src', 'pll_lock', 'pll_dsm', 'pll_pd', 'sevonpend', 'sleepdeep',
                 'pendst', 'pendsv', 'dma_busy', 'uart_enable', 'uart_empty', 'uart_busy', 'uart_rxirq',
                 'spi_slave', 'spi_empty', 'i2c_enabled', 'hstx_enabled', 'usb_pll_sys', 'gpout_sys', 'usb_enabled']
    stop = 0x300000

    class Machine:
        def __init__(self):
            self.cpus = [Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS) for _ in range(2)]
            self.active = [False, False]
            self.blocked = [None, None]
            self.event = [False, False]
            self.returned = [None, None]
            self.clock_writes = []
            self.ipis = 0
            self.pause_peer_before_ack = False
            phoff = struct.unpack_from('<I', data, 28)[0]
            phsize, phcount = struct.unpack_from('<HH', data, 42)
            for core, cpu in enumerate(self.cpus):
                for base, size in [(0x10000, 0x200000), (stop, 0x1000), (0x20000000, 0x100000),
                                   (0x40000000, 0x100000), (0x50000000, 0x500000),
                                   (0xD0000000, 0x10000), (0xE0000000, 0x100000)]:
                    cpu.mem_map(base, size)
                for i in range(phcount):
                    kind, offset, virtual, _, filesz, _, _, _ = struct.unpack_from('<IIIIIIII', data, phoff+i*phsize)
                    if kind == 1 and filesz:
                        cpu.mem_write(virtual, data[offset:offset+filesz])
                cpu.reg_write(UC_ARM_REG_SP, 0x200F0000-core*0x10000)
            self.r = dict(zip(regs, struct.unpack('<'+'I'*len(regs), self.cpus[0].mem_read(symbols['audit_registers'], 4*len(regs)))))
            self.k = dict(zip(constants, struct.unpack('<'+'I'*len(constants), self.cpus[0].mem_read(symbols['audit_constants'], 4*len(constants)))))
            for core, cpu in enumerate(self.cpus):
                self.write(self.r['cpuid'], core, core)
                self.write(self.r['systick'], 7, core)
                self.write(self.r['scr'], 0, core)
                self.write(self.r['iser'], 1, core)
                cpu.hook_add(UC_HOOK_CODE, self.code, core)
                cpu.hook_add(UC_HOOK_MEM_WRITE, self.store, core)
            for key, value in [('sys_ctrl', self.k['sys_src']), ('sys_div', 1<<16),
                               ('sys_selected', 1<<self.k['sys_src']), ('ref_ctrl', self.k['ref_src']),
                               ('ref_selected', 1<<self.k['ref_src']), ('pll_cs', self.k['pll_lock']|1),
                               ('pll_pwr', self.k['pll_dsm']), ('pll_fb', 125), ('pll_prim', 0x62000)]:
                self.write(self.r[key], value)

        def read(self, address, core=0):
            return int.from_bytes(self.cpus[core].mem_read(address, 4), 'little')

        def write(self, address, value, core=None):
            for cpu in self.cpus if core is None else [self.cpus[core]]:
                cpu.mem_write(address, struct.pack('<I', value & 0xFFFFFFFF))

        def var(self, name, value=None):
            if value is not None:
                self.write(symbols[name], value)
            return self.read(symbols[name])

        def wake(self, core):
            self.event[core] = True
            self.blocked[core] = None

        def store(self, cpu, access, address, size, value, core):
            if 0x20000000 <= address < 0x20100000:
                self.cpus[1-core].mem_write(address, value.to_bytes(size, 'little'))
            elif 0x40000000 <= address < 0x60000000:
                self.cpus[1-core].mem_write(address, value.to_bytes(size, 'little'))
            if address in [self.r['sys_ctrl'], self.r['pll_pwr']]:
                self.clock_writes.append((address, value))
            if address == self.r['sys_ctrl']:
                self.write(self.r['sys_selected'], 1<<(value & 1))
            if address == self.r['pll_pwr']:
                cs = self.read(self.r['pll_cs'])
                self.write(self.r['pll_cs'], cs & ~self.k['pll_lock'] if value & self.k['pll_pd'] else cs | self.k['pll_lock'])
            if address == self.r['doorbell']:
                self.ipis += 1
                self.write(self.r['ispr'], self.read(self.r['ispr'], 1-core)|1, 1-core)
                self.wake(1-core)
            if (core == 1 and self.pause_peer_before_ack and address == self.r['scr']
                    and value & self.k['sleepdeep']):
                self.pause_peer_before_ack = False
                self.blocked[core] = 'pause'
                cpu.emu_stop()

        def code(self, cpu, address, size, core):
            opcode = bytes(cpu.mem_read(address, 2))
            if opcode == b'\x40\xbf':  # SEV: a latched event, not a WFI interrupt.
                for peer in range(2):
                    self.event[peer] = True
                    if self.blocked[peer] == 'wfe':
                        self.blocked[peer] = None
                cpu.reg_write(UC_ARM_REG_PC, address+2)
                cpu.emu_stop()
            elif opcode in (b'\x20\xbf', b'\x30\xbf'):
                kind = 'wfe' if opcode == b'\x20\xbf' else 'wfi'
                pending = bool(self.read(self.r['icsr'], core) & (self.k['pendst'] | self.k['pendsv'])
                               or self.read(self.r['ispr'], core) & self.read(self.r['iser'], core))
                if kind == 'wfe' and self.event[core]:
                    self.event[core] = False
                elif kind == 'wfi' and pending:
                    pass
                else:
                    self.blocked[core] = kind
                cpu.reg_write(UC_ARM_REG_PC, address+2)
                cpu.emu_stop()

        def begin(self, core, name):
            assert not self.active[core]
            cpu = self.cpus[core]
            cpu.reg_write(UC_ARM_REG_PC, symbols[name] | 1)
            cpu.reg_write(UC_ARM_REG_LR, stop | 1)
            self.active[core] = True
            self.blocked[core] = None
            self.returned[core] = None

        def step(self, core, count=200):
            if self.active[core] and not self.blocked[core]:
                cpu = self.cpus[core]
                cpu.emu_start(cpu.reg_read(UC_ARM_REG_PC) | 1, stop, count=count)
                if cpu.reg_read(UC_ARM_REG_PC) == stop:
                    self.active[core] = False
                    self.returned[core] = cpu.reg_read(UC_ARM_REG_R0)

        def until(self, predicate, cores=(0, 1), limit=10000):
            for _ in range(limit):
                if predicate():
                    return
                for core in cores:
                    self.step(core)
            raise AssertionError('progress limit: ' + str((self.active, self.blocked, [hex(c.reg_read(UC_ARM_REG_PC)) for c in self.cpus])))

        def call(self, core, name):
            self.begin(core, name)
            self.until(lambda: not self.active[core], cores=(core,))
            return self.returned[core]

        def park(self):
            self.var('soc_sleep_peer_idle', 1)
            self.begin(0, 'audit_prepare')
            self.until(lambda: self.var('soc_sleep_request') != 0, cores=(0,))
            self.begin(1, 'audit_peer')
            self.until(lambda: not self.active[0])
            assert self.returned[0] == 1
            self.until(lambda: self.blocked[1] == 'wfe', cores=(1,))
            assert self.cpus[0].reg_read(UC_ARM_REG_PRIMASK) == 1
            assert self.cpus[1].reg_read(UC_ARM_REG_PRIMASK) == 1
            assert self.read(self.r['systick'], 1) & 3 == 0

        def release(self):
            self.call(0, 'audit_finish')
            self.until(lambda: not self.active[1])
            assert self.var('soc_sleep_ack') == 0
            assert self.cpus[0].reg_read(UC_ARM_REG_PRIMASK) == 0
            assert self.cpus[1].reg_read(UC_ARM_REG_PRIMASK) == 0
            assert self.read(self.r['systick'], 1) == 7
            assert self.read(self.r['scr'], 1) == 0

    results = []
    def passed(name):
        results.append({'test': name, 'result': 'PASS'})

    m = Machine()
    assert m.call(0, 'audit_prepare') == 1
    assert m.var('soc_sleep_request') == 0 and m.cpus[0].reg_read(UC_ARM_REG_PRIMASK) == 0
    m.begin(0, 'audit_sleep')
    m.until(lambda: m.blocked[0] == 'wfi', cores=(0,))
    assert not m.clock_writes
    m.wake(0)
    m.until(lambda: not m.active[0], cores=(0,))
    m.call(0, 'audit_finish')
    passed('busy_peer_uses_light_without_clock_changes')

    m = Machine()
    m.var('soc_sleep_peer_idle', 1)
    assert m.call(0, 'audit_prepare') == 1
    assert m.var('audit_time_us') <= 102 and m.var('soc_sleep_request') == 0
    m.call(1, 'audit_peer')
    assert m.var('soc_sleep_ack') == 0 and not m.clock_writes
    passed('bounded_request_timeout_and_late_peer')

    m = Machine()
    m.var('soc_sleep_peer_idle', 1)
    m.var('soc_sleep_ack', 123)
    assert m.call(0, 'audit_prepare') == 1 and m.var('soc_sleep_request') == 0
    passed('outstanding_ack_blocks_new_request')

    m = Machine()
    m.var('soc_sleep_peer_idle', 1)
    m.begin(0, 'audit_prepare')
    m.until(lambda: m.var('soc_sleep_request') != 0, cores=(0,))
    first = m.var('soc_sleep_request')
    m.pause_peer_before_ack = True
    m.begin(1, 'audit_peer')
    m.until(lambda: m.blocked[1] == 'pause', cores=(1,))
    m.until(lambda: not m.active[0], cores=(0,))
    assert m.returned[0] == 1 and m.var('soc_sleep_ack') == 0
    m.call(0, 'audit_finish')
    m.begin(0, 'audit_prepare')
    m.until(lambda: m.var('soc_sleep_request') != 0, cores=(0,))
    assert m.var('soc_sleep_request') != first
    m.wake(1)
    m.until(lambda: not m.active[1])
    m.until(lambda: not m.active[0])
    assert m.returned[0] == 1 and m.var('soc_sleep_ack') == 0 and not m.clock_writes
    passed('cancelled_peer_cannot_acknowledge_new_generation')

    m = Machine()
    m.park()
    assert m.var('soc_sleep_ack') == m.var('soc_sleep_request') != 0
    m.release()
    passed('peer_park_and_finish_restore_masks_systick_scr')

    m = Machine()
    for cpu in m.cpus:
        cpu.reg_write(UC_ARM_REG_PRIMASK, 1)
    m.write(m.r['scr'], m.k['sevonpend'], 1)
    m.park()
    m.call(0, 'audit_finish')
    m.until(lambda: not m.active[1])
    assert all(c.reg_read(UC_ARM_REG_PRIMASK) == 1 for c in m.cpus)
    assert m.read(m.r['scr'], 1) == m.k['sevonpend']
    assert m.read(m.r['systick'], 1) == 7
    passed('preexisting_primask_and_scr_bits_are_preserved')

    m = Machine()
    m.park()
    m.write(m.r['ispr'], 1, 1)
    m.wake(1)
    m.until(lambda: m.ipis != 0, cores=(1,))
    assert m.var('soc_sleep_abort') == m.var('soc_sleep_request')
    assert m.cpus[1].reg_read(UC_ARM_REG_PRIMASK) == 1 and m.active[1]
    m.release()
    assert m.read(m.r['ispr'], 1) == 1
    passed('peer_interrupt_wakes_owner_but_waits_for_release')

    m = Machine()
    m.write(m.r['icsr'], m.k['pendst'], 0)
    assert m.call(0, 'audit_prepare') == 0
    assert m.var('soc_sleep_request') == 0 and not m.clock_writes
    assert m.cpus[0].reg_read(UC_ARM_REG_PRIMASK) == 0
    passed('pending_work_declines_before_request')

    m = Machine()
    m.var('soc_sleep_peer_idle', 1)
    m.begin(0, 'audit_prepare')
    m.until(lambda: m.var('soc_sleep_request') != 0, cores=(0,))
    m.var('audit_idle', 0)
    m.call(1, 'audit_peer')
    m.until(lambda: not m.active[0], cores=(0,))
    assert m.returned[0] == 1 and m.var('soc_sleep_ack') == 0
    assert m.cpus[1].reg_read(UC_ARM_REG_PRIMASK) == 0 and not m.clock_writes
    passed('stale_idle_hint_cannot_park_running_task')

    m = Machine()
    m.park()
    saved_ctrl = m.read(m.r['sys_ctrl'])
    saved_pwr = m.read(m.r['pll_pwr'])
    m.begin(0, 'audit_sleep')
    m.until(lambda: m.blocked[0] == 'wfi', cores=(0,))
    m.write(m.r['ispr'], 1, 1)
    m.wake(1)
    m.until(lambda: not m.active[0])
    assert m.ipis == 1 and m.active[1] and m.cpus[1].reg_read(UC_ARM_REG_PRIMASK) == 1
    assert m.read(m.r['sys_ctrl']) == saved_ctrl and m.read(m.r['pll_pwr']) == saved_pwr
    m.release()
    assert m.read(m.r['ispr'], 1) == 1
    passed('peer_only_interrupt_ends_pll_off_sleep_before_release')

    m = Machine()
    m.park()
    saved = {k: m.read(m.r[k]) for k in ('sys_ctrl', 'sys_div', 'sys_selected', 'pll_pwr', 'pll_fb', 'pll_prim', 'scr')}
    m.begin(0, 'audit_sleep')
    m.until(lambda: m.blocked[0] == 'wfi', cores=(0,))
    assert m.read(m.r['pll_pwr']) & m.k['pll_pd']
    assert m.var('soc_sleep_deep_entries') == 1
    assert m.cpus[1].reg_read(UC_ARM_REG_PRIMASK) == 1 and m.active[1]
    m.wake(0)
    m.until(lambda: not m.active[0], cores=(0,))
    for key, value in saved.items():
        assert m.read(m.r[key]) == value, key
    assert m.var('soc_sleep_request') != 0 and m.cpus[1].reg_read(UC_ARM_REG_PRIMASK) == 1
    m.release()
    passed('clock_power_and_selection_restored_before_release')

    m = Machine()
    m.park()
    m.write(m.r['icsr'], m.k['pendst'], 0)
    m.call(0, 'audit_sleep')
    assert not m.clock_writes
    m.release()
    assert m.read(m.r['icsr'], 0) == m.k['pendst']
    passed('owner_pending_tick_aborts_without_consuming_it')

    m = Machine()
    m.park()
    m.var('audit_board_allowed', 0)
    m.begin(0, 'audit_sleep')
    m.until(lambda: m.blocked[0] == 'wfi', cores=(0,))
    assert not m.clock_writes
    m.wake(0)
    m.until(lambda: not m.active[0], cores=(0,))
    m.release()
    passed('board_veto_rechecked_before_clock_change')

    for name, key, value in [('dma_busy', 'dma', 'dma_busy'), ('uart_tx_busy', 'uart_fr', 'uart_busy'),
                              ('uart_rx_irq', 'uart_imsc', 'uart_rxirq'), ('spi_slave', 'spi_cr', 'spi_slave'),
                              ('i2c_slave', 'i2c_enable', 'i2c_enabled'), ('hstx_enabled', 'hstx', 'hstx_enabled'),
                              ('usb_uses_pll_sys', 'usb_clk', 'usb_pll_sys'), ('gpout_uses_sys', 'gpout', 'gpout_sys'),
                              ('usb_controller_active', 'usb_main', 'usb_enabled')]:
        m = Machine()
        assert m.call(0, 'audit_eligible') == 1
        if key.startswith('uart'):
            m.write(m.r['uart_cr'], m.k['uart_enable'])
            m.write(m.r['uart_fr'], m.k['uart_empty'])
        m.write(m.r[key], m.k[value])
        assert m.call(0, 'audit_eligible') == 0, name
        passed('peripheral_veto_' + name)
    for key in ('pio', 'pwm'):
        m = Machine()
        m.write(m.r[key], 1)
        assert m.call(0, 'audit_eligible') == 0
        passed('peripheral_veto_' + key)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--compile-commands', required=True, type=Path)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--python-deps', type=Path)
    args = parser.parse_args()
    if args.python_deps:
        sys.path.insert(0, str(args.python_deps.resolve()))
    root = Path(__file__).resolve().parents[2]
    build = args.build.resolve()
    build.mkdir(parents=True, exist_ok=True)
    data, symbols, command = compile_test(root, args.compile_commands.resolve(), build)
    results = run_tests(data, symbols)
    (build / 'results.json').write_text(json.dumps({'command': command, 'tests': results}, indent=2))
    print(json.dumps(results, indent=2))


if __name__ == '__main__':
    main()
