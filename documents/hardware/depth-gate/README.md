# OpenOrbis: gate por depth e efeitos laterais

Fonte para template OpenOrbis com GnmDriver/libkernel. Emite cinco instâncias
ES→GS→copy com a mesma geometria e depth 0,5, sobre depth inicial 1. Configura
cor em depth-fail e supressão em depth-pass; varia late/early e stencil com
incremento em depth-fail/decremento em stencil-fail. O GS incrementa seu contador
uma vez por instância; o PS incrementa outro contador. `DEPTH_GATE` registra
hash de cor, contador GS e submissão; `DEPTH_GATE_PS` registra as invocações PS.
Os hashes dos backings on-chip/off-chip devem coincidir e o contador GS deve ser 5.

O último caso remove o PS e habilita bounds [0,25; 0,75] sobre depth armazenado 1:
o target permanece zerado, sem invocações PS. Os contadores PS dos demais casos
são observações para confrontar a seleção de early/late e o gate guest.

Foi conferida somente a sintaxe com headers stub. Não foi produzido ELF OpenOrbis
nem obtido resultado de execução desse fonte. O diagnóstico PC equivalente está
registrado em D3D12-RENDERER.md; não cobre as combinações F13 ainda rejeitadas.
