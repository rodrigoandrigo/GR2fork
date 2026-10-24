# Inicialização de jogos UWP — 23/10/2026

## Correções gerais

- No Xbox, o host define `RequiresPointerMode = WhenRequested`, evitando que o
  mouse virtual do sistema seja ativado automaticamente durante o jogo.
- Os guards da libc interna implementam aquisição, conclusão e cancelamento
  de inicialização estática, com espera e notificação entre threads.
- Arquivos criados pelo guest passam pelo backend do mount. `O_TRUNC` e
  `ftruncate` também usam esse backend, inclusive para StorageFolders sem
  caminho físico disponível. Falhas de redimensionamento são propagadas.
- `/download0` mantém o diretório anterior; `/download1` recebe uma área privada
  separada por título. Nenhum caminho depende de um jogo específico.
- A enumeração de diretórios evita consultas repetidas por item quando existe
  um único backend. O backend WinRT tenta enumerar nomes, tipos e tamanhos em
  lotes pelo handle autorizado do StorageFolder, sem resolver caminhos físicos.
  Mantém o fallback WinRT com prefetch de propriedades quando disponível.
  A abertura de arquivos também tenta o handle autorizado antes do stream.
- Leituras nativas usam staging antes de copiar para páginas guest, permitindo
  que compromisso sob demanda e rastreamento de memória sejam acionados.
- Views da RAM no Windows preservam a capacidade máxima de acesso do backing;
  a proteção efetiva guest continua aplicada por página. Isso permite dividir
  uma região somente de leitura e posteriormente torná-la gravável.
- Cancelamento UWP não envia novas APCs para threads encerrando ou terminadas.
  O callback verifica a presença do TLS nativo antes de acessar `thread_local`,
  protegendo a desmontagem de threads pelo Windows. O ensaio isolado reproduz
  TLS nulo e verifica esses estados. A repetição de 660 segundos encerrou com
  `BOOT_RESULT=0` e `SHUTDOWN=0`.
- A sessão expõe um stop token compartilhado para esperas internas. A recepção
  bloqueante de sockets é acordada com `EINTR`, e a fila de matchmaking é
  notificada para sair. Os callbacks são removidos antes da destruição das
  estruturas de espera. Isso evita aguardar indefinidamente essas threads ao
  parar depois do menu.

## Dozen

As alterações abaixo pertencem ao caminho Vulkan → Dozen → D3D12:

- Máscaras de componentes de entrada DXIL usam o componente da instrução NIR.
- A variante com geometry shader para provoking vertex usa a topologia efetiva
  do pipeline, incluindo pipelines com topologia estática.
- Saídas de posição são normalizadas antes de `nir_lower_clip_halfz`, que
  pressupõe escritas vetoriais completas. Shaders GCN podem escrever os quatro
  componentes separadamente.
- A chave de compilação do cache foi incrementada para invalidar DXIL anterior.
- Sem suporte a attachments com contagens diferentes de samples, imagens
  renderizáveis respeitam o mesmo limite comum de cor/profundidade usado pelo
  pipeline. A consulta por formato do Dozen admitia imagens 8× enquanto os
  limites globais faziam o pipeline usar 4×. O debug D3D12 reportou IDs 614/616
  e descartou os draws. O fallback para uma contagem menor já existia no
  pipeline; agora também é aplicado às imagens e suas recriações de backing.

Essas correções não selecionam títulos nem hashes de shaders e não representam
novas funcionalidades do renderer D3D12 nativo.

## Evidência de execução no PC

Diagnósticos nativos carregam a DLL Release, usam StorageFolder e fazem readback
do swapchain. Não equivalem ao frontend UWP instalado nem à execução no Xbox.

| Caso | Resultado observado | Limite |
| --- | --- | --- |
| Chronicles of Teddy | Tela inicial e menus visíveis; entrada de controle aceita. Regressão final de 45 segundos: 2137 apresentações, zero mensagens D3D12 e parada/shutdown zero. | Há artefatos gráficos; gameplay prolongado não validado. |
| Minecraft Preview, CUSA44267 | Menu completo visível, controle mudou a seleção para Settings. Execução final de 495 segundos: 13123 apresentações, zero mensagens D3D12, `BOOT_RESULT=0` e `SHUTDOWN=0`. | Menu observado após cerca de seis minutos; gameplay não validado. |
| PAC-MAN 256 | Regressão final de 45 segundos chegou ao logo de carregamento: 2185 apresentações, zero mensagens D3D12 e parada/shutdown zero. | Esta revisão não foi validada em gameplay no Xbox. |
| Parada de rede | Recepção UDP bloqueada e fila vazia de matchmaking acordaram e encerraram em 1 ms. Nova espera depois da parada retornou imediatamente. | Ensaio isolado com implementações de produção no PC. |
| Memória Windows | Aliases concorrentes de 1 GiB, backing acima de 4 GiB e 7 GiB, proteção diferida e upgrade de view dividida passaram. | Diagnóstico no PC. |
| Arquivos brokered | Criação, escrita/leitura, `O_TRUNC`, extensão com zeros, redução e preservação da posição passaram pelo HLE. | Diagnóstico nativo com StorageFolder; não é validação no AppContainer do Xbox. |
| Enumeração brokered | 20.031 entradas da árvore do Minecraft comparadas com a enumeração do host: mesmos nomes, tipos e tamanhos; 5634 ms. API de storage também passou. | Tempo desse ensaio no PC; não mede o boot inteiro nem desempenho no Xbox. |

Logs e capturas finais ficam no diretório ignorado `Build/general-boot`:
`teddy-release-regression.log`, `pacman-release-regression.log`,
`mc-final-startup-stop-validation.log` e seus PNGs. O ensaio de rede está em
`network-stop-validation.log`.
O ensaio atualizado de arquivos e memória está em
`guest-resize-and-memory-validation.log`.
Enumeração e API de storage: `native-directory-and-memory-validation.log` e
`native-directory-storage-api-validation.log`.
APCs de saída: `cancel-teardown-validation.log`; repetição longa após a correção
em `mc-cancel-teardown-fixed.log`.
Compatibilidade entre amostras de imagens e pipeline: `mc-msaa-consistency-validation.log`.

Release x64 compilado e assinado: `startup-complete-release-build.log`.
`startup-complete-package-verification.log` confirma o conteúdo do bundle contra
o núcleo, host e DLLs Mesa de produção. O pacote não foi instalado nesta revisão.

## Módulos e pendências

Os diagnósticos dos dois primeiros títulos usaram `libSceLibcInternal.sprx`
descriptografada fornecida pelo usuário. Módulos proprietários não fazem parte
do pacote; no frontend, use **Importar → Módulos de sistema**.

Chamadas LWFS de atributo e alocação ainda são stubs. A assinatura pública de
OpenOrbis confirma os argumentos, mas não define a semântica de alocação; não
se deve substituí-la por truncamento sem evidência. O cenário visível do
Minecraft não comprova funcionamento de cache, gravação ou gameplay.
Em hosts com limites menores, o fallback MSAA reduz a contagem efetiva de
amostras; isso mantém a configuração do draw válida e não equivale à fidelidade
de uma GPU capaz de representar todas as amostras do guest.

As fontes de comparação em hardware estão em
[`scripts/openorbis/libc-guards`](../scripts/openorbis/libc-guards/README.md) e
[`scripts/openorbis/filesystem-create`](../scripts/openorbis/filesystem-create/README.md).
Há também uma coleta observacional de
[`LWFS`](../scripts/openorbis/lwfs/README.md) para comparar retornos, tamanho e posição.
Não foram executadas em PS4 nesta revisão.
