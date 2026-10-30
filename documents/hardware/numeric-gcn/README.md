# OpenOrbis: FP64 e denorm FP32

Fonte GnmDriver/libkernel com adicoes FP64 que diferem da reducao FP32 e multiplicacoes FP32 que distinguem flush/preserve na entrada e saida. Resultado lido por fence e comparado em bits com a referencia CPU.

Validacao local somente sintatica. Nao cobre arredondamentos direcionados, NaN payload, modos hibridos, cross-lane ou emulacao FP64 por software.

Referencias: [ISA Southern Islands, AMD](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/southern-islands-instruction-set-architecture.pdf) e [regras numericas Direct3D, Microsoft](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm).
