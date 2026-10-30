# OpenOrbis: contadores e eventos de stream-out

Fonte para um template OpenOrbis com GnmDriver e libkernel. Os casos configuram ES→GS→copy→PS, offset inicial de 5 dwords e stride de 4 dwords. Conferem continuação entre dois draws, emissão condicional vazia, `GS_OP_EMIT_CUT`, emissão em `GS_DONE`, `SO_VGTSTREAMOUT_FLUSH`, espera de `OFFSET_UPDATE_DONE` e salvamento com `SOURCE_SELECT=NONE`, em dwords e bytes. O contador atômico separado confere uma execução do GS por draw.

`STREAMOUT_CASE` registra os valores observados; `STREAMOUT_RESULT` compara as contagens esperadas e a equivalência entre os dois modos de backing. O programa não valida a seleção/layout dos descritores nem múltiplos streams. A fonte foi preparada, sem execução ou resultado atribuído a hardware.

Contrato PM4: [documentação AMD](https://docs.amd.com/api/khub/documents/ApP4PzuRytQl9QLD9lgkmA/content).
