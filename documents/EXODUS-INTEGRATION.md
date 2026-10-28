# Integração das melhorias do EXODUS

Referência: árvore local `GR2fork-GR2fork-EXODUS`, comparada com a base
`11ee32fe` e com as alterações existentes neste projeto, em 24/10/2026.
O suporte UWP, D3D12 nativo, Dozen, biblioteca PKG e interface foi preservado.

## Melhorias integradas

| Área | Mudança |
| --- | --- |
| Shaders GCN | Fluxo entre blocos, término de caminhos estruturados, simplificação determinística de PHI, preservação de `GetUserData`, divergência das barreiras conforme o tamanho de cada dimensão do workgroup. |
| Instruções | `BUFFER_WBINVL1`/`BUFFER_WBINVL1_SC` emitem barreira de memória; reconhecimento de derivadas coarse, indefinidos de 64 bits, `S_SLEEP` e leitura suportada dos bits de shader engine de `S_GETREG_B32`. |
| Compilação | Opção `fast_shader_math`, desligada por padrão, aplicada aos perfis Vulkan e D3D12 nativo; invalidação do cache Vulkan com a nova versão binária. |
| Comandos Vulkan/Dozen | Gravação opcional em thread dedicada com cópia dos argumentos temporários, fila limitada e espera pela submissão; viewport/scissor com fallback para os recursos disponíveis no UWP. |
| Pipelines | Destruição diferida dos pipelines substituídos até a GPU terminar de usá-los; salvamento do cache existente mantido. |
| CPU de comando | Fila própria para a constant engine, sincronização com a graphics engine e descarte dos coroutines ao encerrar. |
| Buffers | Readback direto de arenas visíveis ao host, filas de cópia quando necessário, controle por página/tick, sincronização com sparse binds e cópias alinhadas; `readback_offload` passa a ativado por padrão no complemento de 26/10/2026. |
| Texturas | Rastreamento de episódios de escrita, comparação do backing, coleta e readback assíncrono; barreiras de tiling e rastreamento de escritas das fences. |
| Arquivos | Identidade/inode, metadados pelo handle, modos de abertura, acesso somente leitura/escrita, `flock`, dispositivos e eventos de I/O. Os backends UWP e PKG implementam a nova interface de metadados. |
| Áudio e vídeo | Correções de ATRAC9/AJM e gerenciamento dos pacotes/frames do decodificador de vídeo; serviço de música com decodificação MP3/FLAC/FFmpeg e desligamento da reprodução ao encerrar o convidado. |
| Serviços | USB storage em pasta privada `/mnt/usb0`, app content/licenças, PlayGo, voz, signin, RTC e autenticação NP/ShadNet com atualização do protocolo protobuf. |
| Entrada | Melhorias de mouse/controle e opção de vibração respeitada pelo backend Windows.Gaming.Input do UWP. O mapeamento de touchpad existente foi preservado. |
| CPU host | Relocação correta de operandos RIP/EIP nos patches e atualização dos caminhos Rosetta; hashing inline nos pontos quentes sem desabilitar o stack protector. |

As novas preferências usam o catálogo já existente da interface UWP e seus perfis
globais/por jogo. Não há uma segunda janela nem um arquivo paralelo de configurações.
`one_core_gpu_thread_pin` aparece indisponível no UWP porque as funções desktop de
afinidade não são usadas no AppContainer. O Dozen continua sendo o backend padrão.

## Adaptações de ciclo de vida

Falhas da thread de gravação acordam os consumidores e são propagadas ao dono da
sessão. Falhas das filas de readback concluem/cancelam os pedidos pendentes. O
apresentador drena os callbacks de readback antes de destruir os caches e imagens.
Esperas de autenticação, eventos e locks respeitam o encerramento do UWP.
Na apresentação UWP, uma espera por um tick da thread de gravação só entra na fila
depois que a submissão que sinaliza esse tick foi emitida, evitando inversão de
dependências na mesma fila gráfica.

As requisições NP são abortadas antes do descarte do convidado, e os estados de
voz, signin e enumeração USB são reinicializados para a próxima sessão. O alias
`/temp0` permanece junto de `/temp`; as alterações do EXODUS não removem esse caminho.

## Complemento de 26/10/2026

- Compilação de pipelines gráficos em segundo plano no Vulkan/Dozen, com `GPU.pipeline_sync_budget_ms` global ou por jogo (padrão 50 ms, faixa 0–50000 ms). Draws cujo pipeline não fica pronto dentro do orçamento são descartados. Há no máximo quatro tarefas pendentes por cache.
- Cada tarefa mantém cópias dos shaders, user data, runtime info e vertex sharps, além de um cache privado do driver. Os caches são mesclados somente após a conclusão. Substituição de shaders e encerramento drenam as tarefas antes de liberar os objetos usados por elas.
- Depois de 30 segundos, o resultado do pipeline é abandonado e seus draws continuam descartados. A tarefa permanece proprietária dos dados até retornar. Diferentemente do vazamento da referência, não há liberação do cache/device com uma thread ainda usando seus objetos. Uma chamada permanentemente travada dentro do driver ainda pode atrasar o encerramento; não foi introduzido cancelamento forçado ou destruição concorrente desses recursos.
- `/GS` foi desativado nos alvos do núcleo e do D3D12 nativo e no frontend UWP. A alternativa GCC/Clang usa `-fno-stack-protector`. Bibliotecas de terceiros mantêm suas próprias opções de build.
- Os ajustes de Gravity Rush foram autorizados explicitamente pelo usuário como exceção à regra geral do AGENTS.md: identificação compartilhada das versões de GR2/Remastered, readback linear assíncrono obrigatório no Remastered e desativado em GR2, integração da captura JPEG com `PhotoReadback` e espera de pelo menos 700 ms pelos pipelines usados em fotos.
- `readback_offload` passa a `true` por padrão. Configurações globais antigas, sem `pipeline_sync_budget_ms`, são migradas uma vez para ativá-lo; depois da migração uma escolha salva é respeitada. O catálogo existente da interface expõe as duas opções, sem uma segunda configuração paralela.

## Alterações da referência não incorporadas

- O vazamento de futures e o descarte do cache/device enquanto há tarefas usando esses objetos foram substituídos pelo abandono do resultado com vida útil preservada, descrito acima.
- A troca dos acessos de 8/16 bits do tiling por recursos indisponíveis no Dozen não foi solicitada neste complemento e permanece fora da integração.
- Mudanças puramente editoriais do README da referência não substituem as instruções de build deste projeto.

## Validação

A validação de build, pacote e execução no PC é registrada nesta integração.
Isso não representa execução deste pacote no Xbox nem comparação com um PS4 físico.

A fonte de homebrew OpenOrbis para comparar inodes, metadados, locks, diretórios e
`/dev/null` está em [scripts/openorbis/exodus-compat](../scripts/openorbis/exodus-compat/README.md).
O SDK OpenOrbis e o hardware PS4 não estavam disponíveis; não há resultado de hardware.


### Validação do complemento de 26/10/2026

- Núcleo Release x64 e frontend UWP recompilados; as linhas de compilação confirmam `/GS-`.
- Configuração antiga preparada com `readback_offload=false`, sem a nova chave: após a inicialização ela foi persistida com offload ativado.
- PAC-MAN em host nativo no PC, orçamento 0 ms, 60 segundos: 3.223 apresentações, 19 pipelines concluídos, 41 draws descartados, nenhuma tarefa pendente ao final, `D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`, `SHUTDOWN=0` e saída zero. A captura da tela foi inspecionada visualmente.
- Probe diagnóstico com uma criação de pipeline atrasada por 33 segundos, orçamento 0 ms e execução total de 65 segundos: abandono aos 30 segundos, retorno e descarte posterior do resultado, `pending=0`, `abandoned=1`, 3.212 apresentações e encerramento normal, sem erros de validação D3D12.
- A validação acima é do host nativo e do núcleo no PC. Não representa instalação UWP, teste no Xbox, teste de um travamento permanente do driver ou execução de Gravity Rush. Não houve confirmação das fotografias/álbum no jogo nem comparação em hardware PS4.
- SHA-256 do núcleo entregue: `0f4301f2e3e2191aaba6690cb9bd610b3f55ab2344bfc22ab79a9fd4750afaad`.

- Auditoria da API: 133 opções aceitas, nove rejeições esperadas de valores/tipos inválidos, override por jogo do orçamento validado e padrão de 50 ms confirmado.
- Execução adicional com orçamento padrão de 50 ms e offload ativado: PAC-MAN por 25 segundos, `D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`, `SHUTDOWN=0` e saída zero.
- Bundle Release assinado e verificado: `CMS_SIGNATURE=PASS`, `PAYLOAD_HASHES=PASS` e `SOURCE_WHITESPACE=PASS`. O pacote não foi instalado.
