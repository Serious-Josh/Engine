# Engine local edits to the NRD integration layer

`NRDIntegration.h` and `NRDIntegration.hpp` are NVIDIA's NRD 4.17.3 integration layer (integration version 22) with the
local edits below. After an NRD update, copy the new files over these and re-apply each edit. Every edit is marked in the
source; search for the marker text.

## R.1: texture pool states (`NRDIntegration.h`)

Marker: `ENGINE LOCAL ADDITION (R.1)`.

- Adds `GetPoolStateNum`, `GetPoolStates` and `SetPoolStates` to `nrd::Integration`.
- `NrdDenoiser` uses them to restore the pool's tracked barrier states when a command list that contains `Denoise` is
  discarded unsubmitted.
- Re-apply: paste the three inline functions back into the public part of `Integration`.

## Allocator P3: application-supplied texture pool (`NRDIntegration.h`, `NRDIntegration.hpp`)

Marker: `Engine local edit begin (allocator P3` up to `Engine local edit end`, plus two one-line edits marked
`Engine local edit (allocator P3`.

What it changes:
- `NRDIntegration.h`: two members at the end of `IntegrationCreationDesc`, `createTextureD3D12` and
  `createTextureUserArg`. They are null by default, which keeps NRD's own behaviour.
- `NRDIntegration.hpp`, in `_CreateResources`:
  - a local `applicationTextures` flag at the top of the function;
  - a block that replaces the pool texture's `m_iCore.CreateTexture` line;
  - a line in "Bind resources to memory" that sets the texture group's `textureNum` to 0 when the application supplied
    the textures.

What it does:
- With `RecreateD3D12` and a non-null `createTextureD3D12`, each pool texture comes from the application: an
  `ID3D12Resource` in COMMON state.
- The integration wraps it with `CreateTextureD3D12`, the same wrapper `DenoiseD3D12` uses for inputs and outputs, and
  NRI allocates no heap for the pool.
- `Destroy` is unchanged: `DestroyTexture` drops the wrapper's reference.

Why: Engine keeps GPU memory in its own heaps and never returns it to the OS during a session
(`src/Renderer/TextureMemoryAllocator.h`, `Documentation/Rendering/FgTiming_ManyLight_Switch_Hangs.md` §7). NRI's own
pool heaps are created per instance and freed on every resize or retirement.

Re-apply:
- Copy the two members into `IntegrationCreationDesc`.
- Copy the marked block and the two one-line edits into `_CreateResources`.
- A reference patch is in `build/mlhang/review/allocator-p3-nrd.patch` (local, not tracked).
