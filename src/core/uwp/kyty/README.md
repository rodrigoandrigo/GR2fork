# Origem dos componentes de memória

Fonte: `C:/Users/rodri/Dev1/Projetos/KytyPS5-xbox-on-upstream`.

`lazyRegions.h`, `pagerStore.h` e `lazyCounters.h` derivam dos componentes
correspondentes em `src/xbox`. A licença upstream acompanha os arquivos em
`COPYING`. `upstream-manifest.csv` identifica por SHA-256 os arquivos usados na
migração, incluindo os pontos de integração do kernel e do renderer.

`memory_backend.cpp` integra `Xbox::Lazy::Regions` e `Xbox::PagerStore` ao backing
físico do GR2fork. `memory_api.cpp` fornece a interface usada pelo address space,
pelos faults e pelas leases existentes. Esse backend substitui o antigo backend C
na compilação UWP.

A integração mantém os contratos de backing físico, aliases, cópias do
renderer, proteções por página e leases do GR2fork. O adaptador indexa as janelas
pelo offset físico para manter aliases coerentes. `lazyRegions.h` possui
adaptações locais para substituir slots, descartar janelas físicas e conferir
o sucesso das mudanças de proteção antes de publicar o novo estado.
Pins temporários usam slots com geração e contadores esparsos por janela;
o adaptador mantém descartes adiados por janela física. Cópias síncronas
mantêm os locks de residência e mapeamento sem duplicar leases externas.
Views residentes mantêm o backing compartilhado; o adaptador não usa a
conversão upstream para memória privada durante a execução guest.
O page-in compartilha uma seção por janela física de 64 KiB, reutiliza seções
liberadas e mantém faults transitórios pendentes sem descartar o conteúdo do store.
O store prepara a metadata dos slots antes de alocá-los
para permitir devolução sem novas alocações. O manifesto registra os hashes da
origem antes dessas adaptações. A implementação e a validação no PC estão descritas em
`documents/KYTY-MEMORY-MIGRATION.md`.
