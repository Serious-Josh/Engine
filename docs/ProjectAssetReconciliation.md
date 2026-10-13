# Project-load missing-asset reconciliation

Engine's project load now runs transaction recovery, loads/validates the registry,
then calls `ProjectAssetReconciliation::Run()` before initializing dependent assets,
scenes, the editor, or GPU resources. Engine currently has one project opening path;
any future project-switch implementation must use this same sequence before loading
dependent state. This operation must never run against unsaved live editor state.

Only resolver-confirmed missing files with safe ordinary project paths are removed
from the registry. Unsafe/reparse paths, unavailable category directories, access
errors and indeterminate entries retain their registrations and produce diagnostics.
No physical asset files are deleted. A missing project root blocks loading.

When missing registrations exist, reference discovery walks the project tree, including
unregistered `.material` and `.scene` files in nested directories. Linked entries or
enumeration/read errors abort discovery rather than allowing incomplete cleanup.
No links are traversed by document reads; pinned ancestors and exclusive ordinary-file
handles reuse the transaction layer's path rules. Duplicate JSON keys and unsupported
schema versions abort preparation. Malformed material data or scene binding shapes
also abort preparation.

The supported reference fields are:

- Material `textures.baseColor`, `normal`, `roughness`, `metallic`, `arm`, `ao`, `height`:
  matching canonical references become JSON null.
- Scene `objects[].meshReference` and `materialReference`: matching references become
  empty strings, using the existing meshless/default-material runtime behavior.

These are all currently supported persisted AssetID binding fields. Unknown/future
reference fields are preserved rather than guessed at. Built-in and valid references
remain unchanged. Ordered JSON trees are patched directly, preserving unknown and
unrelated properties and scene array ordering. Changed documents are formatted as
JSON; unaffected documents retain their exact original bytes.

Registry, material and scene replacements commit as one recoverable transaction.
Every changed file includes its captured original bytes as a commit precondition,
so changed targets cannot overwrite external edits made during preparation. Missing
asset status is rechecked immediately before commit. The cleaned in-memory registry
and modification/reference counts are published only after commit and successful
recovery/housekeeping. Any preparation, transaction or recovery failure blocks project
loading and leaves rollback/recovery responsible for a consistent disk state.

Logs report missing/removed registration counts, material/scene reference counts,
modified files and skipped entries. Cleanup changes produce a concise warning.
The operation runs once during load, never per frame. Transaction size limits still
apply. Concurrent external project editing is not a supported project-load workflow;
external file creation/restoration after the final scan cannot be globally locked.

CPU tests use isolated temporary projects, including missing textures/materials/meshes,
unknown-property preservation, untouched documents, skipped unsafe/unavailable paths,
read failures, transaction faults/interruption, recovery, repeated loads, registry
reload and external-edit preconditions. Engine/D3D12 runtime behavior requires manual
verification. This does not alter import, rename/delete, previews, thumbnails or shaders.
