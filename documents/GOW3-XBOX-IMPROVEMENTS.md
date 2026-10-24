# Melhorias adaptadas do launcher Xbox

## Origem e seleção

Integração em 23/10/2026 a partir de
`C:/Users/rodri/Dev1/Projetos/gow3_xbox-xbox-on-upstream-2026-10-20-9749bd5`.
As referências principais foram `gpu/shadps4/video_core/cache_storage.cpp`,
`gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_serialization.cpp` e
as seções de cache/perfil de desempenho em `xbox/README.md`.

A cópia do recompiler na origem é mais antiga que a árvore local. As correções
GCN listadas em `gpu/VENDOR.txt` já estão presentes no destino; substituir os
arquivos inteiros removeria correções e funcionalidades locais. Foram
selecionadas as melhorias gerais de armazenamento e cache Vulkan/Dozen.
O renderer D3D12 nativo já possui seu próprio cache persistente e não foi
substituído pelo renderer Vulkan do launcher.

## Implementação

- Criação do `VkPipelineCache` antes do pré-carregamento de pipelines. Antes,
  o warm-up podia tentar usar o handle ainda não criado.
- Persistência do cache do driver em `driver.vkcache`, além dos shaders e
  das chaves já armazenados. A leitura valida versão do cabeçalho, tamanho,
  vendor/device e UUID do driver. Dados ausentes ou incompatíveis deixam o
  caminho normal de compilação disponível.
- Limite de 64 MiB para o blob do driver. A gravação compara o hash do conteúdo,
  em vez de apenas seu tamanho. Há checkpoints no warm-up e a cada 64 novos
  pipelines, começando pelo primeiro, além da sincronização/destruição do cache.
- Reconstrução do cache após alteração ou corrupção do perfil. A limpeza
  remove apenas os tipos de arquivo gerados pelo cache, ou seu arquivo ZIP;
  arquivos de outros tipos na pasta são preservados.
- Fila de I/O com um único mutex para seus pedidos, sem contador compartilhado
  sem sincronização. O encerramento drena os pedidos pendentes antes de fechar
  o armazenamento. Os vetores são movidos para a fila, evitando a cópia adicional.
- Gravação de blobs individuais por arquivo temporário e substituição por
  rename. Leituras incompletas e tamanhos incompatíveis com o elemento são
  rejeitados. O ZIP distingue corretamente os estados reader e writer,
  inclusive em criação, reconstrução e fechamento.
- Declaração antecipada de `Liverpool` alinhada ao tipo `struct` real, evitando
  dependência da ordem dos includes na assinatura do construtor do cache.

As modificações não incluem patches de jogo, QTE, configurações gráficas fixas,
assets pré-compilados de um título ou descarte de draws enquanto shaders compilam.
O pager e os caminhos paralelos da origem dependem de contratos de memória e
recursos próprios; não foram transplantados para substituir os mecanismos locais.

## Validação

Diagnóstico temporário em `Build/cache-transfer-probe.cpp`, ligado aos objetos
de produção, com diretório de dados isolado em `Build/gow3-integration/user`:

- Quatro produtores, 512 blobs de 4 KiB, fechamento imediato após enfileirar:
  todos os bytes conferidos após reabrir, em arquivos e ZIP.
- Sobrescrita de blob do driver, limpeza e regravação do perfil nos dois formatos.
- Rejeição de blob com três bytes lido como DWORDs e preservação de arquivo
  alheio ao cache durante a reconstrução.
- Criação real de pipeline compute pelo Dozen, gravação e restauração do cache
  do driver, rejeição de cabeçalho inválido e reconstrução de perfil truncado.

Resultados em `Build/cache-transfer-msaa-validation.log`; log de compilação
do diagnóstico em `Build/gow3-integration/cache-probe-build.log`.
O nome histórico do log não representa um teste novo de MSAA.

ShadPS4-UWP Release x64 compilado e bundle assinado (`CMS_SIGNATURE=PASS`).
As DLLs incluídas coincidem com o runtime e com as saídas de produção;
logs em `Build/gow3-integration/uwp-build.log` e
`Build/gow3-integration/package-verification.log`.

Esses resultados validam armazenamento e criação/restauração de pipelines no PC.
Não medem ganho de FPS nem comprovam comportamento em jogos, AppContainer ou Xbox.
