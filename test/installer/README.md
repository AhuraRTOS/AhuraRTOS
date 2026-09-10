# Installer transaction regressions

Run from the repository root:

```text
python -B -m unittest discover -s test/installer -v
```

The 12 tests execute installer code against fresh temporary projects. They cover
successful replacement, BOM and line-ending preservation, overlapping checkouts,
destinations outside the project, staging failures, every commit rename failure,
new-directory cleanup, persistent recovery failure, cancellation, and selecting
the requested online update ref. The download selection test substitutes the
network download; it does not require network access.

Cancellation checks inject `KeyboardInterrupt` while restoring a backup and
immediately after an original file-tree rename succeeds. A failed recovery must
retain the exact original checkout in the reported transaction directory; a
completed rename must be recovered or retained even when Python never recorded
its completion. Both schedules lost the original checkout before the cancellation
fix and now pass, alongside the other ten tests.

These checks cover handled I/O failures and injected cancellation. They do not
simulate filesystem corruption, abrupt power loss, or unrelated processes changing
the same project during installation.
