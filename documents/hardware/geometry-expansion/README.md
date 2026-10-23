# OpenOrbis: GS e expansão de primitivas

Fonte para um template OpenOrbis com GnmDriver e libkernel. A superfície RGBA8 tem 16×16 pixels e pitch 64. O programa configura ES→GS→copy→PS com rings coerentes, compara hashes entre `ONCHIP=0` e `ONCHIP=3` e confere um incremento atômico por invocação do GS. Os quatro modos são fill, point fill, wireframe largo e wireframe com stipple. `GS_CASE` registra hash, contador e resultado da submissão; `GEOMETRY_RESULT` compara os pares.

Os descritores/rings usam memória direta de 64 KiB e o caso on-chip reserva LDS no ES. A medição é de equivalência dos dois modos de backing e não cobre tessellation, GS com streams múltiplos ou todos os detalhes de cobertura/interpolação.

Contratos: [registradores CIK da AMD](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/programmer-references/CIK_3D_registers_v2.pdf) e [contadores de stream output D3D12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/stream-output-counters).
