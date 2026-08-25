#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
Install AhuraRTOS into a Raspberry Pi Pico SDK project, over the network.

Covers the RP2040, RP2350 and RP2354 - one SDK, one SoC package.

Run it from the root of your project, the directory holding the top-level
CMakeLists.txt and pico_sdk_import.cmake. Nothing needs to be saved there
first; piping the script straight into Python leaves no installer file behind:

    curl -fsSL <raw-url>/tools/install_rpi_online.py | python3 -    # macOS, Linux
    irm <raw-url>/tools/install_rpi_online.py | python -            # Windows

It prints the diff and asks before writing. Options go after the `-`:

    ... | python - --dry-run     # show the diff and exit, writing nothing
    ... | python - --yes         # apply without asking
    ... | python - --uninstall   # take the integration back out

A saved copy works the same way (`python install_rpi_online.py --dry-run`).

For a machine with no route out, use install_rpi_offline.py instead: same
steps, byte-identical result, and it only ever reads a copy already on disk.

WHAT IT DOES

    1. put the AhuraRTOS repository at AhuraRTOS/ in the project
    2. copy AhuraRTOS/template/{os_config.h,os_cb.c,os_main.c} into it,
       plus the package's soc_config.h if it has one
    3. append the kernel block to the top-level CMakeLists.txt, selecting the
       SoC package with set(AHURA_SOC raspberrypi/<chip>), chosen from the board
    4. boot it: os_init() / os_start() in main() - no SoC call, the package
       hooks os_init() through os_arch_soc_init_cb()

HOW THIS DIFFERS FROM THE STM32 INSTALLER

Shorter, and the missing steps are the interesting part.

There is no PendSV step. CubeMX generates a competing PendSV_Handler that has
to be disabled; the SDK's crt0.S declares its vector entry `isr_pendsv` weak,
so the kernel's port simply replaces it at link time. What the kernel does need
is to be told that name, and the SoC package does that - which is why step 3
sets AHURA_SOC rather than editing os_config.h.

There is no tick step either. On STM32 the installer writes os_tick_handler()
into the generated SysTick_Handler. The SDK generates no such function, so the
SoC package supplies `isr_systick` instead. Nothing to patch.

And there is no USER CODE step, because there are no USER CODE sections: a Pico
project's main.c is written by hand, not regenerated. That cuts both ways - the
installer cannot rely on an anchor being there, so it brace-matches main() and
puts the boot calls in front of the first loop, then checks the result landed
where it has to before writing anything.

WHAT IT DOES NOT COPY

template/soc_cb.c. That file is the SoC half of the callback
contract, and the selected raspberrypi package IS that file for these chips. Copying
it as well would put a second definition of every SoC callback into the build,
where an empty weak stub compiled into the application silently displaces the
package's real one - see doc/soc.md. os_cb.c, which holds the application's own
half, is copied as usual.

SELF-CONTAINED: everything this installer needs is in this one file - the
shared machinery (diffing, rollback, downloading the repository) is merged in
below, so piping this file into Python works with nothing else on disk.

Requires Python 3.8 or newer and nothing else - standard library only.

@copyright (c) 2026 Ahura Project Contributors
            See LICENSE in the project root for the full license text.
"""

import argparse
import contextlib
import re
import sys
from pathlib import Path
import difflib
import io
import os
import shutil
import tarfile
import tempfile
import urllib.error
import urllib.request

def add_common_arguments(parser):
    """The flags every installer accepts, so they cannot drift apart."""
    parser.add_argument("--project", default=".", metavar="DIR",
                        help="project root (default: the current directory)")
    parser.add_argument("--source", metavar="PATH",
                        help="use this AhuraRTOS checkout instead of downloading")
    parser.add_argument("--update", action="store_true",
                        help="replace an AhuraRTOS/ already in the project with the "
                             "current version (otherwise it is left alone)")
    parser.add_argument("--force-templates", action="store_true",
                        help="overwrite an existing os_config.h / os_cb.c / os_main.c")
    parser.add_argument("--dry-run", action="store_true",
                        help="show the diff and exit without writing")
    parser.add_argument("-y", "--yes", action="store_true",
                        help="do not ask for confirmation")
    parser.add_argument("--uninstall", action="store_true",
                        help="remove the managed blocks and the AhuraRTOS directory")
    return parser


REPO = "AhuraRTOS/AhuraRTOS"


TARBALL = "https://codeload.github.com/{repo}/tar.gz/{ref}"


BEGIN_TEXT = ">>> AhuraRTOS BEGIN - managed block, changes here are overwritten"


END_TEXT = "<<< AhuraRTOS END"


class Fatal(Exception):
    """A problem the user has to fix before anything can be written."""


class SourceFile:
    """A text file whose byte-level identity survives editing.

    CubeMX writes CRLF on Windows and sometimes leaves a UTF-8 BOM or a stray
    cp1252 byte in a comment. A naive read/write cycle would rewrite every line
    ending in the file and bury the real change in a whole-file diff, so
    encoding, BOM and newline style are captured on load and restored on save.
    Internally the text always uses '\\n'.
    """

    def __init__(self, path: Path):
        self.path = path
        raw = path.read_bytes()

        self.bom = raw.startswith(b"\xef\xbb\xbf")
        if self.bom:
            raw = raw[3:]

        for encoding in ("utf-8", "cp1252"):
            try:
                text = raw.decode(encoding)
            except UnicodeDecodeError:
                continue
            self.encoding = encoding
            break
        else:
            raise Fatal("cannot decode {} as UTF-8 or cp1252".format(path))

        self.newline = "\r\n" if "\r\n" in text else "\n"
        self.text = text.replace("\r\n", "\n")
        self.original = self.text

    def to_bytes(self) -> bytes:
        out = self.text.replace("\n", self.newline).encode(self.encoding)
        return b"\xef\xbb\xbf" + out if self.bom else out

    @property
    def changed(self) -> bool:
        return self.text != self.original

    def diff(self, root: Path) -> str:
        rel = relative(self.path, root)
        return "".join(difflib.unified_diff(
            self.original.splitlines(keepends=True),
            self.text.splitlines(keepends=True),
            fromfile=rel, tofile=rel, n=3,
        ))


def ask(prompt: str):
    """Read one line from the user, or None if there is no terminal to ask at.

    input() is not enough. In the one-line install the script itself arrives on
    stdin - `curl ... | python -` - so stdin is the source code, already read to
    EOF, and input() would raise immediately. The controlling terminal is still
    open in that case, so ask it directly: /dev/tty on POSIX, CONIN$ on Windows.
    """
    if sys.stdin.isatty():
        try:
            return input(prompt)
        except EOFError:
            return None

    # stdin is the script itself, so ask the terminal directly - but only when
    # something is likely to answer. That /dev/tty opens proves a terminal
    # device exists, not that a human is watching it: Git Bash and CI runners
    # both provide one, and a blocking read there hangs the install forever.
    # An unredirected stdout is the usable signal - that is where the prompt
    # goes, so if it is not a terminal, nobody is reading the question either.
    if not sys.stdout.isatty():
        return None

    try:
        terminal = open("CONIN$" if os.name == "nt" else "/dev/tty")
    except OSError:
        return None
    with terminal:
        sys.stdout.write(prompt)
        sys.stdout.flush()
        line = terminal.readline()
    return line or None


def relative(path: Path, root: Path) -> str:
    """Project-relative POSIX path, for messages and for CMake."""
    try:
        return path.resolve().relative_to(root.resolve()).as_posix()
    except ValueError:
        return path.as_posix()


def strip_comments(text: str) -> str:
    """Blank out comments and string literals so a search sees code, not prose.

    Length-preserving - every removed character becomes a space and newlines
    stay - so offsets into the result still index the original text. Without
    this, a commented-out HAL_IncTick() reads as a live call, and the word
    "os_init()" inside our own explanatory comment would satisfy a placement
    check that nothing actually calls it.
    """
    return re.sub(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\])*\"",
                  lambda m: re.sub(r"[^\n]", " ", m.group(0)), text, flags=re.S)


def function_span(text: str, name: str):
    """(start, end) of a whole `<type> name(void) { ... }`, or None.

    Brace-matched over comment-stripped text, so a brace inside a comment or a
    string cannot end the function early.
    """
    code = strip_comments(text)
    m = re.search(r"^[ \t]*(?:__weak[ \t]+)?(?:void|int)[ \t]+" + re.escape(name) +
                  r"[ \t]*\([ \t]*void[ \t]*\)[ \t]*\n?[ \t]*\{", code, re.M)
    if not m:
        return None
    depth, i = 1, m.end()
    while i < len(code) and depth:
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
        i += 1
    if depth:
        return None
    while i < len(text) and text[i] == "\n":
        i += 1
    return m.start(), i


def function_body(text: str, name: str):
    """The inside of a function, or None."""
    span = function_span(text, name)
    if span is None:
        return None
    start, end = span
    return text[text.index("{", start) + 1:text.rindex("}", start, end)]


def managed_block(payload: str, indent: str, comment: str = "c") -> str:
    """Wrap `payload` in the sentinel markers, indented to match its context."""
    if comment == "cmake":
        begin, end = "# " + BEGIN_TEXT, "# " + END_TEXT
    else:
        begin, end = "/* " + BEGIN_TEXT + " */", "/* " + END_TEXT + " */"

    lines = [indent + begin]
    lines += [(indent + line).rstrip() for line in payload.strip("\n").split("\n")]
    lines.append(indent + end)
    return "\n".join(lines) + "\n"


def find_managed(text: str, start: int = 0, stop: int = None):
    """Offsets of the whole managed region (markers included), or None."""
    window = text[start:stop if stop is not None else len(text)]

    b = re.search(r"^[ \t]*(?:/\*|#)[ \t]*" + re.escape(BEGIN_TEXT), window, re.M)
    if not b:
        return None
    e = re.search(r"^[ \t]*(?:/\*|#)[ \t]*" + re.escape(END_TEXT) + r".*$\n?",
                  window[b.start():], re.M)
    if not e:
        raise Fatal("found an unterminated AhuraRTOS block - remove it by hand and re-run")

    return start + b.start(), start + b.start() + e.end()


def drop_managed(src: SourceFile) -> int:
    """Remove every managed region from the file. Returns how many went.

    A region that only added code is deleted. A region that *disabled* code
    carries the original verbatim inside `#if 0 ... #endif`, and removing it
    means putting that code back - deleting it would throw away generated code
    this script does not own.
    """
    removed = 0
    while True:
        span = find_managed(src.text)
        if not span:
            return removed
        start, end = span

        restored = re.search(r"^#if 0\n(.*?)^#endif\n", src.text[start:end], re.S | re.M)
        if restored:
            replacement = restored.group(1)
        else:
            replacement = ""
            # Take the blank line we added ahead of the block back out with it.
            while start >= 2 and src.text[start - 2:start] == "\n\n":
                start -= 1

        src.text = src.text[:start] + replacement + src.text[end:]
        removed += 1


def unmanaged_integration(cmake) -> bool:
    """True when the CMakeLists already links the kernel OUTSIDE a managed block.

    An installer only owns what sits between its own sentinels: every run drops
    those regions and rebuilds them, which is what makes a second run a no-op.
    A hand-written integration - added before the installer existed, or by
    following doc/installation.md - carries no sentinels, so it is invisible to
    that reconciliation. Appending beside it produces two add_subdirectory()
    calls for one directory, which CMake rejects, and two OS_CONFIG_DIR values,
    which it does not.

    So this is checked before anything is written, and it is a stop rather than
    a merge: which of the two blocks is authoritative is the user's decision,
    not something to guess at.
    """
    text = cmake.text
    kept, pos = [], 0
    while True:
        span = find_managed(text, pos)
        if span is None:
            break
        kept.append(text[pos:span[0]])
        pos = span[1]
    kept.append(text[pos:])

    # CMake comments, so the explanatory prose in a managed block that someone
    # copied by hand does not read as a live call.
    lines = [ln for ln in "".join(kept).split("\n")
             if not ln.lstrip().startswith("#")]
    return bool(re.search(r"\bahura_kernel\b", "\n".join(lines)))


UNMANAGED_MESSAGE = """{where} already integrates AhuraRTOS, but not in a block this installer wrote -
  there are no '>>> AhuraRTOS BEGIN' markers around it.

  That happens when the kernel was added by hand, by following
  doc/installation.md, or with a version of this script from before the managed
  blocks existed. An installer only owns what it marked - every run drops those
  regions and rebuilds them, which is what makes a second run a no-op - so a
  hand-written integration is invisible to it. Appending beside one leaves two
  integrations in a single file: two add_subdirectory() calls for one directory,
  which CMake rejects, and two OS_CONFIG_DIR values, which it does not.

  Delete the existing AhuraRTOS lines from {where} - the add_subdirectory(), the
  OS_CONFIG_DIR, the target_sources() and the ahura_kernel link - then run this
  again. It writes the same integration back, in a block it can keep up to date
  from then on.

  Your os_config.h, os_cb.c and os_main.c are untouched either way."""


def refuse_unmanaged(cmake, root):
    """Raise the standard explanation for unmanaged_integration()."""
    if not unmanaged_integration(cmake):
        return

    where = relative(cmake.path, root)
    raise Fatal(UNMANAGED_MESSAGE.format(where=where))


def walk_sources(root: Path, prune=frozenset()):
    """Every .c/.h under root, skipping `prune` and dot-directories."""
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in prune and not d.startswith(".")]
        for name in filenames:
            if name.endswith((".c", ".h")):
                yield Path(dirpath) / name


def looks_like_ahura(path: Path) -> bool:
    """A usable AhuraRTOS tree: the kernel and the three templates to copy."""
    return ((path / "ahura.h").is_file()
            and (path / "template" / "os_config.h").is_file())


@contextlib.contextmanager
def repo_source(source: str, ref: str):
    """Yield a directory that is the AhuraRTOS repository root."""
    if source:
        given = Path(source).expanduser().resolve()
        if looks_like_ahura(given):
            yield given
            return
        raise Fatal("--source {} is not an AhuraRTOS checkout "
                    "(no ahura.h in it)".format(source))

    # Running from inside a checkout - tools/install_stm32.py beside ahura.h and kernel/.
    #
    # __file__ is not a reliable signal on its own: piped in as `python -` it is
    # set to the literal string "<stdin>", which resolves against the working
    # directory and would point two levels above the project. Requiring it to be
    # an existing file rules that out.
    here = globals().get("__file__")
    if here and Path(here).is_file():
        local = Path(here).resolve().parent.parent
        if looks_like_ahura(local):
            yield local
            return

    url = TARBALL.format(repo=REPO, ref=ref)
    print("  downloading {} ...".format(url))
    try:
        with urllib.request.urlopen(url, timeout=60) as response:
            payload = response.read()
    except (urllib.error.URLError, OSError) as exc:
        raise Fatal(
            "could not download AhuraRTOS: {}\n"
            "  Clone it yourself and point the script at the clone instead:\n"
            "      git clone https://github.com/{}.git\n"
            "      python {} --source AhuraRTOS".format(exc, REPO, Path(sys.argv[0]).name))

    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as tar:
            # A GitHub tarball has no absolute or parent paths, no symlinks and
            # no hardlinks, but this is archive extraction: check rather than
            # trust. The link check matters specifically for the fallback
            # below - filter="data" rejects escaping links itself, but on
            # Python < 3.12 there is no filter, and a link member whose
            # linkname points outside the directory is the one way a member
            # with a perfectly innocent name still writes outside tmp.
            for member in tar.getmembers():
                if member.name.startswith("/") or ".." in Path(member.name).parts:
                    raise Fatal("refusing to extract unsafe path from tarball: " + member.name)
                if member.issym() or member.islnk():
                    raise Fatal("refusing to extract link member from tarball: {} -> {}"
                                .format(member.name, member.linkname))
            try:
                tar.extractall(tmp, filter="data")   # filter= is 3.12+
            except TypeError:
                tar.extractall(tmp)
        roots = [p for p in Path(tmp).iterdir() if p.is_dir()]
        if len(roots) != 1 or not looks_like_ahura(roots[0]):
            raise Fatal("downloaded archive does not look like the AhuraRTOS repository")
        yield roots[0]


def apply(edits, copies, root: Path):
    """Write everything, or put it all back. Originals are held in memory, so a
    failure halfway - a read-only file, a full disk - leaves no half-install."""
    written, created = [], []
    try:
        for kind, src, dest in copies:
            if kind == "repo":
                if dest.exists():
                    if not looks_like_ahura(dest):
                        raise Fatal(
                            "{} exists but is not an AhuraRTOS checkout (no\n"
                            "  ahura.h in it). Refusing to delete it - move it aside\n"
                            "  and re-run.".format(relative(dest, root)))
                    # Updating is a replacement, never a merge: a file dropped
                    # upstream would otherwise linger and keep compiling.
                    shutil.rmtree(dest)
                shutil.copytree(src, dest, ignore=shutil.ignore_patterns(".git"))
                created.append(dest)
                print("  tree    -> {}".format(relative(dest, root)))
            else:
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(src, dest)
                created.append(dest)
                print("  copied  -> {}".format(relative(dest, root)))

        for src in edits:
            tmp = src.path.with_suffix(src.path.suffix + ".ahura-tmp")
            tmp.write_bytes(src.to_bytes())
            os.replace(tmp, src.path)
            written.append(src)
            print("  patched -> {}".format(relative(src.path, root)))

    except Exception:
        print("\n! failed - rolling back", file=sys.stderr)
        for src in written:
            src.path.write_bytes(
                src.original.replace("\n", src.newline).encode(src.encoding))
        for path in created:
            if path.is_dir():
                shutil.rmtree(path, ignore_errors=True)
            else:
                path.unlink(missing_ok=True)
        raise


def finish(args, project, root, edits, copies, notes):
    for src in edits:
        print(src.diff(root))

    for kind, src, dest in copies:
        print("  {}  {} -> {}".format("tree  " if kind == "repo" else "copy  ",
                                      "AhuraRTOS" if kind == "repo" else src.name,
                                      relative(dest, root)))
    for note in notes:
        print("  note: {}".format(note))
    print()

    if args.dry_run:
        print("Dry run - nothing was written.")
        return 0

    if not args.yes:
        answer = ask("Apply these changes? [y/N] ")
        if answer is None:
            print("Nothing written: there is no terminal to confirm at.\n"
                  "Add --yes to apply, or --dry-run to see this diff again.")
            return 1
        if answer.strip().lower() not in ("y", "yes"):
            print("Cancelled - nothing was written.")
            return 0

    if args.uninstall:
        apply(edits, [], root)
        ahura = root / "AhuraRTOS"
        if ahura.is_dir() and looks_like_ahura(ahura):
            shutil.rmtree(ahura)
            print("  removed -> {}".format(relative(ahura, root)))
        print("\nUninstalled. os_config.h, os_cb.c and os_main.c were left in place,")
        print("and any PendSV_Handler that was disabled is back exactly as it was.")
        return 0

    apply(edits, copies, root)
    print("\nInstalled. Next:")
    print("  1. build - the kernel prints the port it chose: 'Ahura kernel arch: ...'")
    print("  2. write your code in {}".format(relative(project.src_dir / "os_main.c", root)))
    print("  3. to prove the port on this board first, set OS_CONFIG_TEST_ENABLE to 1")
    print("     in {} and rebuild (doc/self-test.md)"
          .format(relative(project.inc_dir / "os_config.h", root)))
    return 0

# Directories never searched for project sources.
PRUNE = {
    ".git", ".vscode", ".settings", "__pycache__",
    "build", "Build", "Debug", "Release", "cmake-build-debug", "cmake-build-release",
    "AhuraRTOS", "pico-sdk", "lib",
}

# The SoC packages this installer can select between, and how a board name maps onto them. Only
# the chip matters to the kernel, but a Pico SDK project names a BOARD, so the mapping is here
# rather than asking the user to know which chip is on theirs.
SOC_RP2040 = "raspberrypi/rp2040"
SOC_RP235X = "raspberrypi/rp235x_arm"
SOC_RP235X_RISCV = "raspberrypi/rp235x_riscv"

# What the CMake block says about the package it selected. Written into the user's CMakeLists.txt,
# so it has to describe the core that was actually chosen: the two halves of the RP2350 own
# different vectors and are told about them in different ways.
SOC_NOTE_ARM = """# The SoC package for this chip. This is what tells the kernel that the SDK's vector table calls
# entry 14 isr_pendsv rather than PendSV_Handler - without it the build links cleanly and then
# traps at os_start(). It also supplies isr_systick, SystemCoreClock, the core id, the inter-core
# doorbell and the SIO hardware spinlocks. See AhuraRTOS/doc/soc.md."""

SOC_NOTE_RISCV = """# The SoC package for this chip's Hazard3 RISC-V cores. There is no PendSV on RISC-V: the vector
# the kernel owns is the machine software interrupt (trap cause 3), which the SDK's crt0_riscv.S
# names isr_riscv_machine_soft_irq and declares weak so exactly this can replace it. The package
# also supplies the tick off SIO_MTIMECMP, the CPU clock, the core id, the SIO_RISCV_SOFTIRQ
# register that serves as both yield and inter-core interrupt, and the hardware spinlocks. It also
# places the context-switch handler in RAM, which on this chip is a link-time requirement rather
# than a preference. See AhuraRTOS/doc/soc.md.""" 

# Matched against PICO_PLATFORM first, then PICO_BOARD, as substrings and in this order - so
# "pico2_w" hits the RP2350 rule before the "pico" one could claim it.
SOC_RULES = (
    ("rp2350",  SOC_RP235X),
    ("rp2354",  SOC_RP235X),
    ("pico2",   SOC_RP235X),
    ("pico_2",  SOC_RP235X),
    ("rp2040",  SOC_RP2040),
    ("pico",    SOC_RP2040),
)


# ---------------------------------------------------------------------------
# Reading the project
# ---------------------------------------------------------------------------

def main_span(text: str):
    """(start, end) of `int main(...) { ... }`, or None.

    Not function_span: that one requires a literal `(void)`, and
    the SDK's own project template writes `int main()`. Both spellings, and an
    argc/argv main, have to match. Brace-matched over comment-stripped text so a
    brace in a comment or a string cannot end the function early.
    """
    code = strip_comments(text)
    m = re.search(r"^[ \t]*(?:int|void)[ \t]+main[ \t]*\([^)]*\)[ \t]*\n?[ \t]*\{", code, re.M)
    if not m:
        return None

    depth, i = 1, m.end()
    while i < len(code) and depth:
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
        i += 1
    return (m.end(), i - 1) if not depth else None


class Project:
    """Everything the installer needs, all of it read from the project's own
    sources."""

    def __init__(self, root: Path, soc_override: str = None):
        self.root = root

        top = root / "CMakeLists.txt"
        if not top.is_file():
            raise Fatal(
                "no CMakeLists.txt here.\n"
                "  Run this from the root of your Pico project - the directory holding\n"
                "  CMakeLists.txt and pico_sdk_import.cmake.")
        self.cmake = SourceFile(top)

        if "pico_sdk_init" not in self.cmake.text:
            raise Fatal(
                "this CMakeLists.txt does not call pico_sdk_init(), so it is not a\n"
                "  Pico SDK project. Installing into an STM32CubeMX project? Use\n"
                "  install_stm32_online.py instead.")

        sources = list(walk_sources(root, PRUNE))
        self.main_c = self._pick(
            [p for p in sources if p.suffix == ".c"
             and main_span(p.read_text(errors="replace")) is not None],
            "a .c file defining main()")

        self.src_dir = self.main_c.parent
        self.inc_dir = self.src_dir
        self.target = self._cmake_target()
        self.board = self._board()
        self.platform = self._cmake_string("PICO_PLATFORM")
        self.soc = soc_override or self._detect_soc()

    def _pick(self, candidates, what: str) -> Path:
        if not candidates:
            raise Fatal("could not find {} under {}".format(what, self.root))
        if len(candidates) > 1:
            listing = "\n".join("    " + relative(p, self.root) for p in sorted(candidates))
            raise Fatal(
                "found more than one {}:\n{}\n"
                "  The kernel is installed into one of them. Move the others aside, or\n"
                "  install by hand from doc/installation.md.".format(what, listing))
        return candidates[0]

    def _cmake_target(self) -> str:
        """The executable to link against.

        A Pico project names it literally in add_executable(), unlike CubeMX's
        ${CMAKE_PROJECT_NAME} indirection. Where several exist, the one whose
        sources include the main.c found above is the right answer.
        """
        text = strip_comments(self.cmake.text)
        targets = re.findall(r"add_executable\s*\(\s*([A-Za-z0-9_.${}+-]+)", text)
        if not targets:
            raise Fatal("no add_executable() in CMakeLists.txt - is this the project root?")
        if len(targets) == 1:
            return targets[0]

        wanted = self.main_c.name
        for name in targets:
            m = re.search(r"add_executable\s*\(\s*" + re.escape(name) + r"\b([^)]*)\)", text, re.S)
            if m and wanted in m.group(1):
                return name
        raise Fatal(
            "several add_executable() targets and none of them names {}:\n    {}\n"
            "  Cannot tell which one the kernel belongs to."
            .format(wanted, ", ".join(targets)))

    def _cmake_string(self, name: str):
        """The value of a set(<name> <value>) in the top-level CMakeLists, or None."""
        m = re.search(r"set\s*\(\s*" + re.escape(name) + r"\s+([A-Za-z0-9_.-]+)",
                      strip_comments(self.cmake.text))
        return m.group(1) if m else None

    def _board(self):
        """PICO_BOARD, for the summary and for choosing the SoC package. Absent is fine."""
        return self._cmake_string("PICO_BOARD")

    def _is_riscv(self) -> bool:
        """Whether this project targets the RP2350's Hazard3 RISC-V cores rather than its M33s.

        Two places say so, and the second is the one that usually does. A project may set
        PICO_PLATFORM=rp2350-riscv itself; far more often it does not, because the VS Code extension
        writes only a toolchain name and lets pico-vscode.cmake derive the platform from it:

            if(PICO_TOOLCHAIN_PATH MATCHES "RISCV")
                set(PICO_PLATFORM rp2350-riscv CACHE STRING "Pico Platform")

        Reading the toolchain name here the same way is what stops a perfectly unambiguous project
        being told the installer cannot tell which chip it targets.
        """
        for value in (self.platform, self._cmake_string("toolchainVersion")):
            if value and ("riscv" in value.lower()):
                return True

        return False

    def _detect_soc(self) -> str:
        """Which SoC package this project needs, from PICO_PLATFORM then PICO_BOARD.

        The RP2040 and the RP235x are separate packages because they are separate silicon
        generations, so picking the wrong one is not a near miss - it would arm the wrong
        inter-core interrupt and build against the wrong core. Guessing is therefore not on:
        an unrecognised board is an error naming both choices.
        """
        riscv = self._is_riscv()

        for source in (self.platform, self.board):
            if not source:
                continue
            lowered = source.lower()
            for token, soc in SOC_RULES:
                if token in lowered:
                    if not riscv:
                        return soc

                    # Same chip, other core. The RP235x packages are siblings and the toolchain has
                    # already decided between them.
                    if soc == SOC_RP235X:
                        return SOC_RP235X_RISCV

                    raise Fatal(
                        "this project builds for RISC-V, but its board names an RP2040." + "\n" +
                        "  The RP2040 is Cortex-M0+ only and has no RISC-V cores, so there is no" + "\n" +
                        "  package for that combination. Check PICO_BOARD and the toolchain.")

        raise Fatal(
            "cannot tell which chip this project targets.\n"
            "  PICO_PLATFORM={}, PICO_BOARD={}\n"
            "  Pass the package explicitly:\n"
            "      --soc {}           (RP2040)\n"
            "      --soc {}      (RP2350, RP2354 - Arm cores)\n"
            "      --soc {}    (RP2350, RP2354 - RISC-V cores)"
            .format(self.platform or "<unset>", self.board or "<unset>",
                    SOC_RP2040, SOC_RP235X, SOC_RP235X_RISCV))

    def check(self):
        """Anything that would make the build fail, checked before writing."""
        # Before anything else: an integration this installer did not write cannot be
        # reconciled, only duplicated. See refuse_unmanaged().
        refuse_unmanaged(self.cmake, self.root)

        text = strip_comments(self.cmake.text)
        if re.search(r"\bFREERTOS|\bfreertos\b", text) or (self.root / "FreeRTOS-Kernel").is_dir():
            raise Fatal(
                "this project already has FreeRTOS in it.\n"
                "  Both kernels claim PendSV and the tick, so they cannot coexist.\n"
                "  Remove the FreeRTOS import from CMakeLists.txt and re-run.")

        if main_span(self.main_c.read_text(errors="replace")) is None:
            raise Fatal("could not brace-match main() in {}"
                        .format(relative(self.main_c, self.root)))


# ---------------------------------------------------------------------------
# The plan
# ---------------------------------------------------------------------------

CMAKE_BLOCK = """\
# Everything the kernel needs from this build lives in this one block, appended after the
# generated part rather than woven into it: target_sources() and target_link_libraries() both
# APPEND, so a later call adds to what is already there. It also has to come after
# pico_sdk_init(), which it does by sitting at the end of the file.

{soc_note}
set(AHURA_SOC {soc})

# OS_CONFIG_DIR must be set BEFORE add_subdirectory: it tells the kernel library where this
# project's os_config.h lives. The kernel and the application have to compile against the same
# configuration - if only the application saw the file, their structure sizes would silently
# disagree.
set(OS_CONFIG_DIR {cfg})
add_subdirectory({ahura})

# A header is not normally a configure-time dependency, so CMake would not notice a changed
# os_config.h until the next manual re-run. Listing it here makes editing it re-run CMake.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${{OS_CONFIG_DIR}}/os_config.h)

# Self-test suite, driven by os_config.h so there is only one switch to flip. With
# OS_CONFIG_TEST_ENABLE at 1, os_init() runs os_test() instead of os_main(), and nothing in the
# kernel defines that function - so forgetting the library is a link error rather than a test
# build that silently tests nothing.
file(READ ${{OS_CONFIG_DIR}}/os_config.h _os_config_contents)
if(_os_config_contents MATCHES "#define[ \\t]+OS_CONFIG_TEST_ENABLE[ \\t]+1")
    message(STATUS "Ahura self-test suite: ENABLED (os_main() is not run in this build)")
    add_subdirectory({ahura}/test)
    set(AHURA_TEST_LIB os_test)
else()
    set(AHURA_TEST_LIB "")
endif()
unset(_os_config_contents)

# Application-owned kernel files. These belong to the APPLICATION build, never to the kernel
# library: os_cb.c holds the application's callbacks, os_main.c the default task's body. The SoC
# half of the callback contract is NOT here - the {soc} package above provides it.
target_sources({target} PRIVATE
    {src}os_cb.c
    {src}os_main.c
)

target_link_libraries({target}
    ahura_kernel
    ${{AHURA_TEST_LIB}}   # empty unless OS_CONFIG_TEST_ENABLE is 1, see above
)"""

INCLUDE_PAYLOAD = """\
/* The kernel's one public header, and the only one an application needs. The SoC package has
   no public header at all: every entry point it has is a callback the kernel invokes itself,
   secondary-core start-up included. */
#include "ahura.h" """.rstrip()

BOOT_PAYLOAD = """\
/* Nothing SoC-specific to call here: the SoC package hooks os_init() itself, through
   os_arch_soc_init_cb(), so the CPU clock, the hardware spinlock and this core's inter-core
   interrupt are all ready before the tick is programmed. os_init() then creates the idle task,
   the kernel service tasks and the default application task running os_main(); os_start()
   switches to task context and never returns, so anything below here is unreachable. */
os_init();
os_start();"""


def insert_include(src: SourceFile) -> bool:
    """Put the include after the last existing one, or at the top of the file."""
    last = None
    for m in re.finditer(r"^[ \t]*#[ \t]*include[^\n]*\n", src.text, re.M):
        last = m
    at = last.end() if last else 0
    src.text = src.text[:at] + "\n" + managed_block(INCLUDE_PAYLOAD, "") + src.text[at:]
    return True


def insert_boot(src: SourceFile) -> bool:
    """Put the boot calls in main(), ahead of its first loop.

    A Pico main() is hand-written, so there is no marker to aim at. The first
    `while`/`for` inside the body is the reliable landmark: the SDK's own
    template ends in one, and anything the application set up - stdio, GPIO,
    clocks - is above it, which is exactly where the kernel wants to start.
    With no loop at all the calls go at the end of the body, which is the same
    position by a different route.
    """
    span = main_span(src.text)
    if span is None:
        raise Fatal("could not find main() in {}".format(src.path.name))
    body_start, body_end = span

    code = strip_comments(src.text)
    loop = re.search(r"^[ \t]*(?:while|for)[ \t]*\(", code[body_start:body_end], re.M)
    at = body_start + loop.start() if loop else body_end

    # Match the indentation of whatever follows, so the block sits with the code.
    line_start = src.text.rfind("\n", 0, at) + 1
    indent = re.match(r"[ \t]*", src.text[line_start:]).group(0) or "    "

    src.text = (src.text[:line_start]
                + managed_block(BOOT_PAYLOAD, indent) + "\n"
                + src.text[line_start:])
    return True


def verify_placement(mc: SourceFile):
    """Prove the calls ended up inside main(), and in order, before writing."""
    span = main_span(mc.text)
    if span is None:
        raise Fatal("could not find main() in {} after editing".format(mc.path.name))

    body = strip_comments(mc.text)[span[0]:span[1]]
    positions = {name: body.find(name + "()")
                 for name in ("os_init", "os_start")}
    missing = [n for n, at in positions.items() if at < 0]
    if missing:
        raise Fatal("{}() did not land inside main() in {} - refusing to write a build "
                    "that would never start".format(", ".join(missing), mc.path.name))

    if not positions["os_init"] < positions["os_start"]:
        raise Fatal("the boot calls landed out of order in {} - os_init() must run before "
                    "os_start()".format(mc.path.name))

    loop = re.search(r"^[ \t]*(?:while|for)[ \t]*\(", body, re.M)
    if loop and positions["os_init"] > loop.start():
        raise Fatal("os_init() landed inside the loop in {} - it has to run before it"
                    .format(mc.path.name))


def plan(project: Project, repo_dir: Path, force: bool, copy_tree: bool):
    """Return (file edits, copy actions, notes) without touching the disk."""
    edits, copies, notes = [], [], []

    ahura_dest = project.root / "AhuraRTOS"
    if copy_tree:
        copies.append(("repo", repo_dir, ahura_dest))
    else:
        notes.append("AhuraRTOS/ is already in the project - left as it is "
                     "(--update fetches the current version)")

    # The three files become yours the moment they exist, so an existing one is
    # kept. soc_cb.c is deliberately not among them: the SoC package is it.
    templates = [(repo_dir / "template" / "os_config.h", project.inc_dir),
                 (repo_dir / "template" / "os_cb.c", project.src_dir),
                 (repo_dir / "template" / "os_main.c", project.src_dir)]

    # The SoC package's own options, if it has any. Optional in a way os_config.h is not - the
    # package defaults every one of them - so a package without the file contributes nothing here
    # rather than failing.
    soc_config = repo_dir / "soc" / project.soc / "template" / "soc_config.h"
    if soc_config.is_file():
        templates.append((soc_config, project.inc_dir))

    for source, dest_dir in templates:
        dest = dest_dir / source.name
        if dest.is_file() and not force:
            notes.append("{} is already there - kept as it is "
                         "(--force-templates replaces it)".format(relative(dest, project.root)))
        else:
            copies.append(("template", source, dest))

    # relative() answers "." for a directory that IS the project root, which is the normal
    # layout for a Pico project - main.c beside CMakeLists.txt. Emitting that verbatim would give
    # "${CMAKE_CURRENT_SOURCE_DIR}/." and "./os_cb.c": both work, both look like a bug.
    cfg = relative(project.inc_dir, project.root)
    src = relative(project.src_dir, project.root)

    body = CMAKE_BLOCK.format(
        soc=project.soc,
        soc_note=SOC_NOTE_RISCV if project.soc == SOC_RP235X_RISCV else SOC_NOTE_ARM,
        cfg="${CMAKE_CURRENT_SOURCE_DIR}" + ("" if cfg == "." else "/" + cfg),
        src="" if src == "." else src + "/",
        ahura=relative(ahura_dest, project.root),
        target=project.target,
    )
    drop_managed(project.cmake)
    project.cmake.text = project.cmake.text.rstrip("\n") + "\n\n" + managed_block(
        body, "", comment="cmake")
    if project.cmake.changed:
        edits.append(project.cmake)

    # Reconciled, not patched: every managed region comes out first and is
    # rebuilt at the right anchor, which is what makes a second run a no-op.
    mc = SourceFile(project.main_c)
    drop_managed(mc)
    insert_include(mc)
    insert_boot(mc)
    verify_placement(mc)
    if mc.changed:
        edits.append(mc)

    if project.soc == SOC_RP235X_RISCV:
        notes.append("the tick and the context-switch vector need no edits here - the {} package "
                     "drives the tick off SIO_MTIMECMP and the kernel replaces the SDK's weak "
                     "isr_riscv_machine_soft_irq".format(project.soc))
    else:
        notes.append("the tick and the PendSV vector need no edits here - the {} package "
                     "supplies isr_systick and names isr_pendsv for the kernel".format(project.soc))

    return edits, copies, notes


def plan_uninstall(project: Project):
    edits = []
    for path in (project.root / "CMakeLists.txt", project.main_c):
        src = SourceFile(path)
        if drop_managed(src) and src.changed:
            edits.append(src)
    return edits


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main(argv=None) -> int:
    parser = argparse.ArgumentParser(
        prog="install_rpi_online.py",
        description="Install AhuraRTOS into a Raspberry Pi Pico SDK project "
                    "(RP2040, RP2350, RP2354). Run it from the project root.")
    add_common_arguments(parser)
    parser.add_argument("--ref", default="main", metavar="REF",
                        help="branch or tag to download (default: main)")
    parser.add_argument("--soc", metavar="PKG",
                        help="SoC package to install (default: chosen from PICO_PLATFORM, "
                             "the toolchain name and PICO_BOARD); raspberrypi/rp2040, "
                             "raspberrypi/rp235x_arm or raspberrypi/rp235x_riscv")
    args = parser.parse_args(argv)

    root = Path(args.project).expanduser().resolve()

    try:
        project = Project(root, args.soc)

        print("AhuraRTOS installer")
        print("  project   {}".format(root))
        if project.board:
            print("  board     {}".format(project.board))
        print("  soc       {}".format(project.soc))
        print("  target    {}".format(project.target))
        print("  sources   {}".format(relative(project.main_c, root)))
        print()

        if args.uninstall:
            edits, notes = plan_uninstall(project), []
            if (root / "AhuraRTOS").is_dir() and looks_like_ahura(root / "AhuraRTOS"):
                notes.append("AhuraRTOS/ will be deleted")
            notes.append("your os_config.h, os_cb.c and os_main.c are left alone - "
                         "they are your files now")
            if not edits and not notes:
                print("Nothing installed here.")
                return 0
            return finish(args, project, root, edits, [], notes)

        project.check()

        installed = looks_like_ahura(root / "AhuraRTOS")
        source = contextlib.nullcontext(root / "AhuraRTOS") \
            if installed and not args.update and not args.source \
            else repo_source(args.source, args.ref)

        with source as repo_dir:
            edits, copies, notes = plan(
                project, repo_dir, args.force_templates,
                copy_tree=not installed or args.update or bool(args.source))
            if not edits and not copies:
                print("Already installed, and everything is where it should be.")
                return 0
            return finish(args, project, root, edits, copies, notes)

    except Fatal as exc:
        print("\nerror: {}".format(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\ncancelled", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
