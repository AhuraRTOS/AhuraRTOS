"""Installer regressions. All mutations are confined to fresh temporary fixtures.

Run: python -B -m unittest discover -s test/installer -v
"""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.dont_write_bytecode = True
REPO = Path(__file__).resolve().parents[2]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


engine = load("audit_engine", REPO / "tools/internal/engine.py")


def make_repo(path, marker):
    (path / "kernel").mkdir(parents=True)
    (path / "template").mkdir()
    (path / "ahura.h").write_bytes(marker)
    (path / "template/os_config.h").write_bytes(marker)


def snapshot(path):
    return {p.relative_to(path).as_posix(): p.read_bytes()
            for p in path.rglob("*") if p.is_file()}


class TransactionTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="ahura-regression-")
        self.base = Path(self.tmp.name).resolve()
        self.assertEqual(self.base.parent, Path(tempfile.gettempdir()).resolve())
        self.root = self.base / "project"
        self.root.mkdir()
        self.source = self.base / "source"
        self.installed = self.root / "AhuraRTOS"
        make_repo(self.source, b"new kernel")
        make_repo(self.installed, b"original kernel")
        (self.installed / "local.txt").write_bytes(b"original local data")
        self.config = self.root / "os_config.h"
        self.config.write_bytes(b"original user config\r\n")
        self.cmake = self.root / "CMakeLists.txt"
        self.cmake.write_bytes(b"\xef\xbb\xbforiginal cmake\r\n")
        self.edit = engine.SourceFile(self.cmake)
        self.edit.text = "new cmake\n"
        self.copies = [("repo", self.source, self.installed),
                       ("template", self.source / "template/os_config.h", self.config)]
        self.before = snapshot(self.root)

    def tearDown(self):
        # TemporaryDirectory owns this fresh path, verified against the system temp root.
        self.tmp.cleanup()

    def apply(self, edits=None, copies=None):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            engine.apply([self.edit] if edits is None else edits,
                         self.copies if copies is None else copies, self.root)

    def test_success_replaces_tree_and_preserves_edit_encoding(self):
        (self.source / ".git").mkdir()
        (self.source / ".git/config").write_bytes(b"must not copy")
        self.apply()
        self.assertEqual((self.installed / "ahura.h").read_bytes(), b"new kernel")
        self.assertFalse((self.installed / "local.txt").exists())
        self.assertFalse((self.installed / ".git").exists())
        self.assertEqual(self.config.read_bytes(), b"new kernel")
        self.assertEqual(self.cmake.read_bytes(), b"\xef\xbb\xbfnew cmake\r\n")
        self.assertFalse(list(self.root.glob(".ahura-install-*")))

    def test_same_source_is_rejected_before_mutation(self):
        with self.assertRaises(engine.Fatal):
            self.apply([], [("repo", self.installed, self.installed)])
        self.assertEqual(snapshot(self.root), self.before)

    def test_nested_source_and_destination_are_rejected(self):
        for source, destination in [(self.installed, self.installed / "nested"),
                                    (self.installed / "nested", self.installed)]:
            with self.subTest(source=source, destination=destination):
                with self.assertRaises(engine.Fatal):
                    self.apply([], [("repo", source, destination)])
                self.assertEqual(snapshot(self.root), self.before)

    def test_destination_outside_project_is_rejected(self):
        with self.assertRaises(engine.Fatal):
            self.apply([], [("template", self.config, self.base / "outside.h")])
        self.assertFalse((self.base / "outside.h").exists())
        self.assertEqual(snapshot(self.root), self.before)

    def test_copy_failure_leaves_all_originals(self):
        def fail_after_partial_copy(source, destination, **kwargs):
            Path(destination).mkdir()
            (Path(destination) / "partial").write_bytes(b"partial")
            raise OSError("injected copy failure")
        with mock.patch.object(engine.shutil, "copytree", side_effect=fail_after_partial_copy):
            with self.assertRaises(OSError):
                self.apply()
        self.assertEqual(snapshot(self.root), self.before)

    def test_forced_template_staging_failure_preserves_user_file(self):
        copies = [("template", self.source / "template/os_config.h", self.config),
                  ("template", self.base / "absent.h", self.root / "second.h")]
        with self.assertRaises(FileNotFoundError):
            self.apply([], copies)
        self.assertEqual(snapshot(self.root), self.before)

    def test_each_commit_rename_failure_restores_exact_originals(self):
        replace = engine.os.replace
        for fail_at in range(1, 7):
            with self.subTest(fail_at=fail_at):
                calls = 0
                def fail_once(source, destination):
                    nonlocal calls
                    calls += 1
                    if calls == fail_at:
                        raise OSError("injected commit failure")
                    return replace(source, destination)
                with mock.patch.object(engine.os, "replace", side_effect=fail_once):
                    with self.assertRaises(OSError):
                        self.apply()
                self.assertEqual(snapshot(self.root), self.before)

    def test_new_parent_directories_removed_on_rollback(self):
        replace = engine.os.replace
        destination = self.root / "new/nested/new.h"
        def fail_install(source, target):
            if Path(target) == destination:
                raise OSError("injected new file failure")
            return replace(source, target)
        with mock.patch.object(engine.os, "replace", side_effect=fail_install):
            with self.assertRaises(OSError):
                self.apply([], [("template", self.config, destination)])
        self.assertFalse((self.root / "new").exists())
        self.assertEqual(snapshot(self.root), self.before)

    def test_recovery_failure_keeps_backup_and_reports_location(self):
        replace = engine.os.replace
        calls = 0
        def fail_commit_and_recovery(source, target):
            nonlocal calls
            calls += 1
            if calls in (2, 3):
                raise OSError("injected persistent failure")
            return replace(source, target)
        with mock.patch.object(engine.os, "replace", side_effect=fail_commit_and_recovery):
            with self.assertRaisesRegex(engine.Fatal, "originals and staged files remain"):
                self.apply()
        transaction, = self.root.glob(".ahura-install-*")
        self.assertEqual((transaction / "old-0/ahura.h").read_bytes(), b"original kernel")
        self.assertEqual((transaction / "old-0/local.txt").read_bytes(), b"original local data")

    def test_interrupt_during_rollback_preserves_original_backup(self):
        original = snapshot(self.installed)
        replace = engine.os.replace
        calls = 0

        def interrupt_recovery(source, target):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise OSError("injected commit failure")
            if calls == 3:
                raise KeyboardInterrupt("injected interruption during rollback")
            return replace(source, target)

        with mock.patch.object(engine.os, "replace", side_effect=interrupt_recovery):
            with self.assertRaises((KeyboardInterrupt, engine.Fatal)):
                self.apply()

        backups = [path / "old-0" for path in self.root.glob(".ahura-install-*")]
        self.assertTrue(any(path.exists() and snapshot(path) == original for path in backups),
                        "a cancelled rollback must retain the original checkout")

    def test_interrupt_after_backup_rename_recovers_or_preserves_original(self):
        original = snapshot(self.installed)
        replace = engine.os.replace
        calls = 0

        def interrupt_after_rename(source, target):
            nonlocal calls
            calls += 1
            result = replace(source, target)
            if calls == 1:
                # Models asynchronous cancellation after the filesystem rename
                # committed, before Python records that the original was saved.
                raise KeyboardInterrupt("injected interruption after backup rename")
            return result

        with mock.patch.object(engine.os, "replace", side_effect=interrupt_after_rename):
            with self.assertRaises((KeyboardInterrupt, engine.Fatal)):
                self.apply()

        locations = [self.installed]
        locations.extend(path / "old-0" for path in self.root.glob(".ahura-install-*"))
        self.assertTrue(any(path.exists() and snapshot(path) == original for path in locations),
                        "a completed backup rename must not become an untracked original")

    def test_online_update_downloads_requested_ref_from_local_bootstrap(self):
        for filename in ("install_rpi_online.py", "install_stm32_online.py"):
            with self.subTest(bootstrap=filename):
                bootstrap = load(filename, REPO / "tools" / filename)
                with mock.patch.object(bootstrap, "download", return_value=self.source) as download:
                    with bootstrap.checkout(None, "release-test", self.root, True) as selected:
                        self.assertEqual(selected, self.source)
                self.assertEqual(download.call_count, 1)
                self.assertEqual(download.call_args.args[0], "release-test")


if __name__ == "__main__":
    unittest.main()
