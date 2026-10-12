# Native D3D12 source snapshot

Copied from `C:/Users/rodri/Dev1/Projetos/xbox-series-d3d12-main` on 2026-10-12.
`SHA256SUMS` records the copied files before local integration. Original source
files and SPDX notices are preserved. The project root LICENSE is included.

Included: device context, resource allocator, command encoder, pipeline cache,
VideoCore command bridge, the original command contract and shader foundation.
The foreign application, tests, fixtures and diagnostic probes are not imported.
The shader translation sources are not yet connected to the production shader
compiler in this checkout. Authored IR fixtures and compiler stubs are excluded.

The DLL builds the five native platform components. Its PM4 command contract
uses `src/video_core/gpu_command_sink.h`, adapted for this fork's indirect SGPR
offsets and packet/fence hooks. The original contract under `include/` records
the source boundary. FrontendRenderer uses the copied device context.

This snapshot is a foundation, not a complete PS4 game renderer. The original
VideoCore bridge rejects draw, dispatch, GDS and image download operations.
The shader translator rejects resources/stages outside its authored fixtures.
