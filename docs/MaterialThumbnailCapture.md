# Persistent material thumbnails

Request registered saved materials with `Renderer::RequestSavedMaterialThumbnail()`
between frames. A successful return means queued/accepted, not image-ready. Requests
are deduplicated into a 32-item FIFO; one preparation attempt runs per normal pre-recording
poll. Capacity-deferred requests rotate to the back, and permanent failures are logged.
Inspector draws retain priority. Asset Browser requests only visible registered Material
records whose files have the `.material` extension, and draws identity-specific saved
sphere images inside its existing 96-pixel tiles. Pending tiles use a Material placeholder.
Texture thumbnails retain their own cache and existing image presentation.

Use `GetSavedMaterialThumbnailImage(paths, id)` to retrieve an identity-specific image.
The legacy no-argument getter refers to the last successfully accepted requested identity.
Calling either getter marks a current-frame display reference. The returned descriptor
is borrowed for that ImGui frame; callers must query again for subsequent frames.
`DiscardSavedMaterialThumbnailRequests()` cancels queued work, not recorded/submitted work.
A new project automatically discards the previous project's queued requests. Completed
images from other projects remain protected and can be reclaimed by ordinary safe LRU.

## GPU display cache

Up to 16 independent 256x256 RGBA8 images are retained, each with its own RTV/SRV,
project/material identity, validated fingerprint, revision, readiness/staleness state,
generation/upload fence, submitted display fence, current-UI protection, and LRU serial.
There is also one private worker output and one upload/readback allocation. The worker
renders or uploads once, then copies to the reserved cache image in the same command list.
The shared sphere, shader pipelines, G-buffer, depth, HDR and studio setup are unchanged.

An entry is reusable only when it is not the active worker's destination, has no
unconfirmed recorded commands or current UI references, and both generation and display
fences have completed. Pending readback pins the worker destination. If no entry is safe,
work defers; it never drains the renderer or reuses a protected descriptor. Deferred UI
frames retain display protection until a later successful submission supplies a fence.
Failed submissions do not publish readiness or release recorded-resource protections.
The browser begins a visible working set each frame. Its first 16 visible registered
materials in stable listing order are admitted and pinned; overflow tiles show placeholders
and issue no requests/lookups. Only images outside that set are LRU eviction candidates,
except a material may safely replace its own stale version. Superseded stale images
are not residency-pinned, but still require all lifetime guards before reuse. Newly visible materials become
eligible as scrolling changes the set. Queue entries outside the final set are discarded.
There is no forced display withholding or continuous overflow cycling.

Nonresident queued requests check available capacity/fences before parsing, validation,
disk reads or resource creation. They remain pending while old references retire. Resident
validation retains its existing behavior. Replacement moves the existing fixed-size render
target through a bookkeeping reset, preserving its GPU allocation, descriptor indices and
tracked state. Generation/display fences and current-UI references must already be retired.

Repeated requests reuse GPU images without file reads/hashing for one second after
validation. Subsequent requests enqueue metadata/dependency revalidation; unchanged
fingerprints reuse the existing image without disk pixel loading or sphere rendering.
Changed fingerprints mark old images stale before preparing replacements. Explicit
`InvalidateSavedMaterialThumbnailImages()` immediately hides images until revalidated.
External edits can remain visible until the next requested revalidation (one-second
throttle plus queue latency). Merely retrieving an image does not validate files; consumers
must continue requesting visible identities, or invalidate on known edits/Refresh.

## GPU lifecycle

Initialization allocates one persistent upload buffer and one persistent readback
buffer using the display texture's D3D12 copyable footprint. A miss records sphere
rendering followed by `COPY_SOURCE` to readback, then restores shader-readable state.
A disk hit writes tightly packed cached pixels into footprint-aligned upload rows;
the frame records `COPY_DEST` from that upload buffer and restores shader-readable state.
Disk hits do not render the sphere or load project material textures.

Only successful submission publishes image readiness and the generation fence.
`PollThumbnailCapture()` runs during normal preview preparation.
It maps readback only after that fence completes; zero, pending and device-removal
completion values do not authorize mapping. Row padding is removed before storage.
There are no GPU waits in this capture/cache-hit path. Existing cold dependency loading
on a miss still uses the established synchronous uploader.

The worker cannot be reused until its generation/copy fence and readback complete.
Completed cache images remain independently displayable while it processes later jobs.
Shutdown releases all resources only under the existing checked Renderer completion
contract; it cancels queued work and pending cache publication without adding waits.

## Storage and identity

Files live below `%LOCALAPPDATA%/Engine/MaterialThumbnails/<project-hash>/`.
Storage follows the texture cache's safe-path, exclusive file, checksum and atomic
temporary-publication conventions, in a separate namespace. Version 1 stores eight
little-endian uint64 header fields, the complete identity, and tightly packed RGBA8
display pixels. Dimensions, DXGI format 28, exact payload size, identity, and payload
checksum are validated. Invalid entries are misses. Pixels are already AgX tone-mapped;
no HDR/color conversion or resizing is applied.

Pruning bounds each project bucket to 128 entries (approximately 32 MiB of pixels),
with a bounded 4096-entry directory inspection. Cache write/read failures are optional
and do not invalidate an otherwise usable rendered image.

The identity includes canonical project/material paths, AssetID, SHA-256 of saved
material contents, all seven references, interpretation, registry type/path, resolver
status, and available texture path/size/timestamp/content digest. Missing/restored assets
therefore change identity. A rendering-version token describes canonical geometry,
camera/orientation, studio setup, POM, AgX, extent and format. **Bump this token when
preview shaders, shared BRDFs, tone mapping, geometry or studio constants change.**

Queued validation rechecks dependencies; identical fingerprints reuse the matching
GPU image. Fingerprints are checked again after loading and before disk publication.
Available textures that fail decoding/upload reject the job. Dependency-cache pressure
defers preparation; neither case publishes a scalar-fallback thumbnail.

## Limits

Sixteen display images, one worker output, one upload allocation, one readback allocation,
and one active generation/readback job are retained. Shared dependency textures remain
bounded at 32 with consumer pins and fence-safe LRU retirement. Inspector and saved jobs
retain separate dependency state; content-sensitive saved-thumbnail keys remain unchanged.

CPU hashing, disk reads/writes and cold texture preparation remain synchronous; only
GPU readback/upload submission is nonblocking. No background workers or disk-cache
polling are introduced. Concurrent external edits can cause a request/capture to be
rejected; callers may retry. Changes after the final validation are discovered on the
next queued revalidation. Rendering-version maintenance is explicit, not automatic shader
source hashing. Runtime capture/restart/visual and D3D12 behavior require manual testing.

## Browser refresh and frame ordering

Browser UI requests and image lookups run before preview preparation/Renderer recording.
Lookup registers each displayed image's current-frame use. Preview preparation services
one queued request, recording remains Inspector-first, and Renderer confirms generation
and display uses only after successful graphics submission. Deferred UI frames do not
clear protections. No browser-specific submission or completion assumptions are added.

Navigation/hiding discards obsolete queued requests without clearing completed material
images; active jobs remain protected. Refresh, project changes, import, rename, and delete
schedule explicit material image invalidation. External material/texture edits and
missing/restored dependencies use the existing throttled fingerprint revalidation.
Inspector sphere rotation does not invalidate browser images. Existing texture-cache
Refresh/navigation drains are unchanged; material capacity pressure adds no drain.

## Metadata-backed validation

`MaterialThumbnailValidation` retains up to 256 CPU source entries, separate from the
unchanged 16-image GPU cache. Absolute resolved paths share SHA-256 digests across
materials and slots. Each entry records file size/timestamp, digest, LRU usage, and an
optional validated parsed material. Unchanged metadata reuses the digest and parsed
snapshot. Fingerprint construction still resolves all references against the supplied
registry, so missing/restored files, type/path changes and project identity affect the
same existing persistent fingerprint. The disk format and render-version token are unchanged.

Cold sources, changed metadata, explicit source invalidation and CPU-cache eviction
require full hashing. Cold/changed materials are parsed once and associated with that
digest; preparation verifies that the snapshot digest still matches before rendering.
Post-preparation and readback checks reuse the digests after rechecking metadata and
resolution. Changed sources invalidate the result before disk publication.

Successful Inspector saves send a one-shot source invalidation through Editor to Renderer.
This bypasses cached metadata even when size/timestamp are preserved, without rehashing
unrelated textures. Refresh/project-wide invalidation clears validation entries; project
changes and shutdown clear them too. No GPU lifetime or eviction behavior changes.

Metadata cannot detect every same-size, timestamp-preserving external edit immediately.
Entries become eligible for full verification after 60 seconds, serviced oldest-first
with at most one 256 KiB chunk per normal preview poll. Large files span polls; deadlines
are eligibility targets, not completion guarantees. This eventually detects such edits
for retained entries while polling continues; image revalidation then sees the new digest.
No file handle stays open between chunks. Metadata changes during verification discard
that attempt. Concurrent edits preserving all checked metadata remain a best-effort race,
not a cryptographic snapshot guarantee. Filesystem latency itself is not time-bounded.

The periodic path bounds bytes read, but cold/changed-file full hashing, material parsing,
source texture uploads, disk-cache publication, navigation GPU drains remain potential hitches. Cold work and navigation drains are not
addressed by stable residency.
