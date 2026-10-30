# OpenOrbis: SDWA e escalares GCN

Fonte GnmDriver/libkernel com 294 casos de seletores SDWA, extensao e preservacao de subpalavras; verifica tambem ORN2/NOR/XNOR/CMOV e extensao de sinal de 8/16 bits, incluindo SCC. Resultados comparados com referencias CPU e lidos apos fence. Validacao local somente sintatica.

Contratos: [GCN3, AMD](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/gcn3-instruction-set-architecture.pdf) e [seletores SDWA, AMD](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/vega-shader-instruction-set-architecture.pdf).

Nao cobre DPP, append/consume, todas as operacoes GCN ou todos os estagios graficos.

A extensão `SCALAR64` registra CMOV, WQM, BREV, BCNT0, FF0, FLBIT signed e QUADMASK de 64 bits, incluindo SCC, zeros, sinais e valores mistos. O programa reserva 16 SGPRs para que entradas e destinos usados estejam na alocação declarada. A fonte é referência OpenOrbis; verificação local por sintaxe não confirma fidelidade numérica.
