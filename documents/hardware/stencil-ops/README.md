# OpenOrbis: operações e referências de stencil

Fonte para template OpenOrbis com GnmDriver/libkernel. Executa as 16 operações
em cinco instâncias sobre stencil inicial 0x5a e compara hashes de cor/stencil
entre rings on-chip/off-chip. Os primeiros 16 casos usam test=0x33, op=0x5a e
write mask=0xa5 nas duas faces, com backface stencil desabilitado; a referência CPU aplica a operação cinco vezes
com merge pela máscara e exige 256 bytes modificados quando o resultado difere
do inicial. Os 16 seguintes usam refs/read masks/write masks diferentes por face
e verificam os valores possíveis e a equivalência dos dois backings.

`STENCIL_OP`, `STENCIL_PS` e `STENCIL_BYTES` registram hash, contadores e referência;
`STENCIL_RESULT` resume os pares. O GS deve executar cinco vezes; o PS deve
executar, e os helpers do renderer não devem repetir seus efeitos laterais.
O fonte usa z-pass; os três resultados stencil-fail/depth-fail/pass, todos os
valores de byte, masks parciais e ordem entre faces são confrontados no diagnóstico
PC registrado em D3D12-RENDERER.md.

Foi conferida somente a sintaxe com headers stub. Não foi produzido ELF OpenOrbis
nem obtido resultado de execução deste fonte. MSAA, discard e exportações do PS
não são cobertos pelo fallback nesta atualização.

Contrato: [CIK 3D Registers v2, DB_STENCIL_CONTROL e DB_STENCILREFMASK](https://www.scribd.com/document/622467575/CIK-3D-registers-v2).
