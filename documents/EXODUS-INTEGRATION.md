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
| Buffers | Readback direto de arenas visíveis ao host, filas de cópia quando necessário, controle por página/tick, sincronização com sparse binds e cópias alinhadas; `readback_offload` continua desligado por padrão. |
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

## Alterações da referência não incorporadas

- Detecção de títulos e imposição de modos de readback conforme o jogo, além de
  mudanças nos módulos específicos de GR2. O AGENTS.md exige correções gerais.
- Compilação assíncrona que pula draws ao exceder um orçamento e abandona/leaka
  tarefas que ainda acessam o cache/device. Permanece a compilação síncrona e o
  encerramento com ownership definido. Não foi adicionada uma preferência sem efeito.
- Desativação de `/GS`/stack protector e troca dos acessos de 8/16 bits do tiling
  por recursos indisponíveis no Dozen. O caminho compatível existente permanece.
- Mudanças puramente editoriais do README e instruções presentes nos arquivos
  da referência não substituem as decisões de build e as instruções deste projeto.

## Validação

A validação de build, pacote e execução no PC é registrada nesta integração.
Isso não representa execução deste pacote no Xbox nem comparação com um PS4 físico.

A fonte de homebrew OpenOrbis para comparar inodes, metadados, locks, diretórios e
`/dev/null` está em [scripts/openorbis/exodus-compat](../scripts/openorbis/exodus-compat/README.md).
O SDK OpenOrbis e o hardware PS4 não estavam disponíveis; não há resultado de hardware.
