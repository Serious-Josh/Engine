# Asset reconciliation transactions

`AssetReconciliationTransaction::Commit(paths, replacements, error)` replaces existing
project files from opaque, fully prepared byte strings. Callers validate the JSON;
the transaction does not reconstruct documents, so unknown fields can be preserved.
Only `Assets.registry.json`, `.material`, and `.scene` targets are eligible. Targets
must be project-relative ordinary files, with no traversal, alternate streams,
reparse-point ancestors, hardlinks, case-insensitive duplicates, or unsupported types.

This API is intended for project-load reconciliation, before dependent data is loaded.
It must not be called against live unsaved Inspector/scene state. No reconciliation
caller is enabled yet. Engine calls `Recover()` before registry deserialization and
fails initialization if recovery cannot establish safety. There is currently only
one project-opening path in Engine; future project-switching code must also call it.

## Storage and commit point

The project-local `.asset-reconciliation/` directory contains:

- `journal.json`: version 1, normalized absolute project root, ordered targets with
  project-relative paths and SHA-256 `oldHash`/`newHash` values.
- `N.old` and `N.new`: immutable original and replacement bytes.
- `N.apply`: disposable installation copy.
- `N.removed`: the displaced target, retained during replacement/rollback.
- `done`: atomically published completion marker (`committed` or `rolledback`).

Journal and marker publication use flushed temporary files and no-replace,
write-through renames. Payload files are flushed before journal publication.
All originals and replacement bytes are staged before any target is changed.
Original target handles and parent directories are pinned while committing.
Replacement uses handle-based no-replace renames; an unrelated destination is
never replaced. A project-local exclusive `.asset-reconciliation.lock` coordinates
transaction/recovery calls and remains as an inert file afterward.

The commit point is publication of `done` containing `committed`, after installing
every replacement. Before that point recovery always rolls back; after it recovery
only finishes cleanup. Originals are retained until the marker exists. A crash can
temporarily leave a target absent between its two renames; startup recovery handles
that gap. This is recoverable multi-file persistence, not simultaneous multi-file
visibility to external readers.

## Recovery and failure

Recovery validates the complete journal, all backup checksums, path safety, aggregate
limits, and all current targets before restoring anything. Current bytes must match
either the original or replacement. An absent target requires a displaced recovery
file. External edits, missing/corrupt backups, corrupt journals, access failures, or
unsafe paths block startup and retain recovery data with an explicit location.
Rollback writes copies of originals, preserving backups for repeated attempts.
A durable `rolledback` marker precedes cleanup. Interrupted cleanup is restartable.
No recursive deletion is used; unknown recovery files cause an error.

Preparation debris with no published journal may be removed only if no displaced
originals exist. A missing journal with displaced originals requires manual recovery.
Ordinary commit failures attempt immediate rollback. `Commit()` returns false before
the commit point; if cleanup fails after commit, it returns true with a diagnostic
in `error`, because the replacement has already committed. Callers must log that
diagnostic and recover remaining housekeeping before another transaction.

Bounds: 4,096 targets, 64 MiB per file, 256 MiB combined original/replacement bytes.
These limits intentionally restrict this mechanism to project metadata. It does not
copy textures or GPU resources. It does not change rename/delete/import behavior.

Durability follows Windows checked file flush/rename semantics. Tests simulate process
interruption; storage-device failure, power loss, malicious journal modification, and
concurrent external writers ignoring the transaction protocol are not guaranteed.
Ambiguous recovery fails closed rather than overwriting user edits.
