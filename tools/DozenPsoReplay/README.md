# Dozen PSO capture and replay

The Xbox driver keeps owned snapshots of pending native PSOs. Pressing **LT + RT**
saves the pending snapshots and adds `worker.dozen.pso_capture.directory` to the
presentation manifest. Obtain those directories through Device Portal, under the
package's `LocalCache/dozen-pso-captures` directory.

Each directory contains:

- `pso.bin`: versioned x64 archive with the stream, referenced arrays/strings,
  shader bytecode, serialized root signatures and relocation records.
- `shader-1.dxil` through `shader-6.dxil`: VS, PS, DS, HS, GS and CS when present.
- `root-N.rootsig`: serialized root signatures.
- `capture.json`: capture key, archive version and sizes.

Snapshots are copied before admission to the native call and released when that
call returns. The limit is 16 MiB per snapshot and 64 MiB of registered snapshots;
export failures are reported without changing pipeline creation. Exported files
are retained for collection. Desktop snapshot collection is disabled by default;
private hosts can enable it with the optional `mesa_uwp_set_pso_capture(1)` export.
`mesa_uwp_set_pso_legacy(1)` lets those hosts select the legacy API without an
AppContainer cache folder.

Build on Windows from the repository root:

```powershell
./tools/DozenPsoReplay/build.ps1
./tools/DozenPsoReplay/compare.ps1 -Archive 'C:/captures/<key>/pso.bin' -DebugLayer
```

Each API runs in a separate process on the selected DXGI adapter (`-Adapter 0` by
default). The script records adapter identity, HRESULT, duration and debug-layer
messages. A 60-second timeout terminates only the replay process, never the
emulator. Use `-TimeoutSeconds` to change it. Remove `-DebugLayer` if Graphics Tools
is unavailable. Driver-internal or disk caches can survive process boundaries;
repeat in reverse order when comparing performance. Individual invocation:

```powershell
./Build/DozenPsoReplay/DozenPsoReplay.exe 'C:/captures/<key>/pso.bin' legacy 0 debug
```

For comparison **on Xbox in the emulator**, put `dozen-pso-api.txt` in the package's
`LocalCache`, containing `legacy`, and restart the app. Removing it restores the
stream API. The selection affects native creation after a pipeline-library miss;
a cache hit does not exercise either creation API. Keep the same cache state when
comparing. `d3d12_create_graphics_pso` identifies the legacy call; the stream call
uses `d3d12_create_pso`.

Legacy conversion preserves the representable stream states and D3D12 defaults,
including the sample mask. States without a faithful legacy representation,
such as enabled depth bounds or view instancing, return `E_NOTIMPL` in standalone
replay. In the emulator they explicitly log `legacy_pso_not_representable` and
use the original stream API. No shader or title is special-cased.

A successful replay on PC verifies reconstruction for that adapter. It does not
establish Xbox compiler behavior or visual correctness. A native call that has
not returned is not proof of a deadlock.
