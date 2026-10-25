# Inicialização de jogos UWP — 23/10/2026

## Compilação compute e views de storage — 24/10/2026

A inicialização de Bloodborne (`CUSA00900`, versão 01.00) revelou dois problemas
gerais no caminho Vulkan/Dozen, corrigidos sem condições por título ou shader:

- O lowering de leitura coerente para uma operação atômica mantinha
  `ACCESS_NON_WRITEABLE` no recurso e na instrução. Isso produzia um handle SRV
  usado por `atomicBinOp` e falhava na validação DXIL com
  `Non-UAV destination to atomic intrinsic`. O lowering agora identifica a
  variável/binding correto, inclusive seu descriptor set, e promove o acesso
  atômico para UAV. A leitura coerente não é removida ou substituída por um stub.
- A criação de imagens UWP só verificava storage no formato base. Uma view
  compatível de escrita podia então receber uma imagem sem `Storage`, resultando
  em um descritor UAV inválido e remoção do device D3D12. A seleção agora considera
  os formatos de view compatíveis, mantendo `MutableFormat`/`ExtendedUsage` e as
  restrições existentes de depth e multisampling.

Núcleo, Dozen, compilador SPIR-V/DXIL e frontend Release x64 recompilados. Bundle
assinado, assinatura CMS e igualdade dos payloads com os binários locais verificadas.

No diagnóstico nativo de PC usando o núcleo UWP e o Dozen, duas inicializações
chegaram à tela **Bloodborne — Press the X Button**. A execução de 75 segundos
apresentou 1.845 frames; a repetição de 35 segundos apresentou 661. Ambas tiveram
`D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0` e `SHUTDOWN=0`. A captura visual foi
inspecionada. Isso comprova a inicialização até essa tela, sem afirmar gameplay
posterior, instalação/execução do bundle no AppContainer ou teste no Xbox.

Registros locais: `Build/bloodborne-boot-image-fix.log`,
`Build/bloodborne-boot-repeat.log`, `Build/bloodborne-package-sign.log` e
`Build/bloodborne-package-verify.log`.
O Pac-Man 256 também passou pela verificação de regressão de inicialização e
encerramento, com zero mensagens D3D12, em `Build/bloodborne-pacman-regression.log`.

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

## Compilação de coordenadas baricêntricas — 24/10/2026

Os logs da instalação UWP mostraram uma falha posterior à tela inicial de
Bloodborne: o validador DXIL rejeitou `dx.op.evalSnapped` com colunas 4, 5 e 6,
e a criação do pipeline gráfico terminou com `ErrorUnknown`.

As instruções NIR de coordenadas baricêntricas têm tamanho de resultado fixo e
não possuem o índice `BASE`. A gravação desse índice sobrescrevia memória da
instrução em Release, alterando a quantidade de componentes. O compilador agora
localiza a entrada `SV_Barycentrics` pelo modo de interpolação e usa a quantidade
de componentes da definição SSA. A correção é geral, sem seleção de jogo ou shader.

O par SPIR-V extraído do cache da instalação reproduziu exatamente os três erros
com o compilador anterior. Com a correção, ambos os estágios passaram no validador
DXIL (shader model 6.6, validator 1.6). Evidências no diretório ignorado
`Build/interpolation-fix`: `validated-before.log`, `validated-after.log` e
`production-rebuilt.log`. A ferramenta temporária de diagnóstico foi removida
das fontes de produção. A validação isolada do shader não comprova gameplay
completo nem execução no Xbox.

Na regressão nativa no PC, Bloodborne executou por 35 segundos, apresentou
562 frames (captura no logo FromSoftware), registrou zero mensagens D3D12 e
encerrou com `BOOT_RESULT=0`, `SHUTDOWN=0` e código de processo zero. Esse percurso
não alcançou o trecho posterior do jogo; a falha desse trecho foi validada pelo
par de shaders original. O bundle Release x64 foi recompilado, assinado e
verificado com `CMS_SIGNATURE=PASS` e `PAYLOAD_HASHES=PASS`. Não foi instalado
nem executado no Xbox nesta revisão.

PAC-MAN 256 também passou na regressão de 20 segundos pelo mesmo backend,
com zero mensagens D3D12 e parada/shutdown normais; log
`Build/interpolation-fix/pacman-regression.log`.

## Crash do compilador AMD ao criar pipeline — 24/10/2026

A execução seguinte da instalação avançou além da falha anterior, mas encerrou
na thread `shadPS4:VkRecord`, com acesso nulo em `0x30`. O dump local
`shadPS4-UWP.exe.17280.dmp` identifica a instrução em
`amdxc64.dll+0x969a99`, durante a compilação de um pipeline D3D12.

Um programa nativo isolado, usando o par de shaders do cache já validado em
DXIL, reproduziu a mesma exceção na mesma DLL e no mesmo deslocamento.
A causa foi a emissão de `evalSnapped` para ler coordenadas baricêntricas no
centro do pixel. `SV_Barycentrics` é uma entrada gerada pelo sistema, sem
armazenamento de atributo interpolável. O compilador agora emite `loadInput`
para essa leitura, preservando o modo de interpolação e os três componentes.
Não há seleção por título, hash de shader ou fabricante de GPU.

Com a correção, o mesmo par passou no validador DXIL e o mesmo programa criou
o pipeline D3D12 com `S_OK`, sem mensagens da camada de diagnóstico. O dump
do programa anterior confirma `amdxc64.dll+0x969a99`. Evidências no diretório
ignorado `Build/record-crash`: `dump-analysis.log`, `reproducer-dump.log`,
`pipeline-before.log`, `pipeline-after.log` e `dxil-validation.log`.
Referência do contrato: [SV_Barycentrics no DirectXShaderCompiler](https://github.com/microsoft/DirectXShaderCompiler/wiki/SV_Barycentrics).

Mesa/Dozen, compilador DXIL e host UWP Release x64 foram compilados. O bundle
foi assinado e seu conteúdo verificado (`CMS_SIGNATURE=PASS`,
`PAYLOAD_HASHES=PASS`). A ferramenta temporária usada para validar os shaders
foi removida das fontes de produção. O pacote não foi instalado nesta revisão.

Regressão nativa no PC: Bloodborne executou por 110 segundos com entrada
sintética de controle e menus de configuração visíveis; 2833 apresentações,
zero mensagens D3D12, `BOOT_RESULT=0`, `SHUTDOWN=0` e saída do processo zero.
Log/captura: `after-game.log` e `after-game.png`. Esse percurso não valida
gameplay completo; o crash foi reproduzido e corrigido com o pipeline isolado.
A execução no Xbox continua pendente.

PAC-MAN 256 passou na regressão de 20 segundos, com zero mensagens D3D12,
`BOOT_RESULT=0`, `SHUTDOWN=0` e saída zero
(`Build/record-crash/pacman-regression.log`).

## Entrada de nome e diálogo IME no frontend UWP — 24/10/2026

A tela de criação do personagem permanecia em `Enter Name / Entry Incomplete`.
O log registrava `sceImeDialogInit` com sucesso e estado `Running`, mas o
frontend UWP não desenha as camadas ImGui do núcleo. O diálogo invisível também
mantinha a captura do controle, deixando o jogo aguardando uma resposta.

Foi adicionada uma ponte geral de entrada de texto entre o núcleo e o frontend.
Ela publica uma cópia do título, texto inicial, placeholder, limite e opções do
diálogo. O frontend abre um `ContentDialog` com `TextBox`/`PasswordBox`, foco e
solicitação do teclado de sistema; a mesma janela permite confirmar ou cancelar.
Durante a edição, os comandos do controle ficam capturados pelo frontend.

A resposta fica em uma fila protegida e é consumida ao consultar o status IME
na thread chamadora do guest. Os filtros de texto existentes são mantidos, a
confirmação copia UTF-16 para o buffer do jogo, e cancelar preserva o conteúdo
original. São rejeitados texto excessivo, pares substitutos inválidos,
respostas duplicadas ou referentes a solicitações antigas. Solicitações são
limpas ao terminar, abortar, fechar à força ou parar a sessão. O encerramento do
guest também remove o diálogo e libera a captura do controle. A implementação
não depende de nome, serial ou shader de jogo.

Na validação nativa do PC, o núcleo real executando Bloodborne foi chamado por
um diagnóstico isolado que exercitou `sceImeDialogInit/GetStatus/GetResult/Term`
com memória guest mapeada. Passaram confirmação com acentos e caractere fora do
BMP, cancelamento, preservação do texto inicial, limite de tamanho, guardas de
memória, resposta antiga/duplicada, modo numérico, senha e fechamento à força.
Resultado: `IME_HLE_ALL=PASS`, zero mensagens D3D12 e parada/shutdown zero no
log ignorado `Build/ime-host-hle-validation.log`. Esse ensaio valida o protocolo
e os buffers; não equivale à inspeção visual do diálogo XAML no aplicativo
instalado ou à execução no Xbox.

O núcleo e o frontend UWP Release x64 foram compilados. O bundle assinado tem
seu conteúdo conferido contra os binários de produção. O pacote não foi
instalado durante esta revisão. A fonte de comparação no PS4 está em
[OpenOrbis IME dialog](../scripts/openorbis/ime-dialog/README.md); não foi
compilada nem executada em hardware.

Com o pacote atualizado, selecione **Enter Name**, pressione **A** no controle
Xbox (equivalente a **X** no PlayStation), digite o nome e escolha **Confirmar**.
Depois selecione **Finish** na tela do jogo.

O diagnóstico do próprio jogo também chegou ao pedido `Please Enter Name`
(limite de 16 unidades UTF-16), confirmou `Hunter` em quatro aberturas e recebeu
`GetResult` com `endstatus=0`, seguido de `Term`. A captura
`Build/ime-host-name-entry.png` mostra `Hunter` no contrato e a seleção em
`Finish`, comprovando a remoção de `Entry Incomplete` e a devolução do controle.
Foram 190 segundos e 5278 apresentações, com parada e shutdown zero
(`Build/ime-host-name-entry-validation.log`). O nome foi fornecido apenas pelo
programa de diagnóstico; o produto sempre solicita que o usuário o digite.

Esse percurso mais adiante também registrou 1024 mensagens D3D12 de estado de
recurso (`D3D_MESSAGE=538`, `COPY_DEST` versus leitura/`COPY_SOURCE`). Portanto,
a validação da entrada de nome não constitui aprovação da sincronização gráfica
desse trecho nem de gameplay completo. A janela XAML do pacote e o teclado do
Xbox ainda precisam de confirmação no aplicativo instalado.

Após a compilação final, a repetição com a DLL destinada ao pacote também passou:
`Build/ime-host-final-hle-validation.log` contém `IME_HLE_ALL=PASS`, zero
mensagens D3D12, `BOOT_RESULT=0`, `SHUTDOWN=0` e saída zero.

## Após finalizar o contrato: personagem controlável — 24/10/2026

O dump instalado `shadPS4-UWP.exe.33016.dmp`, correspondente ao núcleo de
produção, mostrou uma exceção em `AddressSpace::Impl::MapGuestView` ao aplicar
o orçamento de RAM. O limite UWP era artificialmente reduzido a 5120 MiB,
independentemente do limite informado pelo sistema. A decisão agora usa
`MemoryManager::AppMemoryUsageLimit()` e mantém a reserva de 256 MiB. Isso
preserva o orçamento efetivo de cada plataforma, sem impor ao PC o teto fixo.

A passagem após confirmar **Finalizar contrato → Sim** revelou também DXIL
inválido em atributos por vértice: um `trunc i32 to i8` não permitido e uma
assinatura interpolada usada por `attributeAtVertex`. As entradas PerVertexKHR
agora recebem assinatura sem interpolação. Índices constantes são emitidos como
imediatos; índices dinâmicos selecionam entre leituras com índices imediatos
0, 1 e 2, mantendo a semântica do acesso por vértice.

O carregamento seguinte expôs erros D3D12 de buffers de cópia reutilizados como
SRV ou origem. `CmdCopyImageToBuffer2` agora prepara o acesso de destino antes
da cópia e o libera para uso comum depois, tanto com barreiras legadas quanto
com barreiras aprimoradas. As correções são gerais e não selecionam títulos,
hashes de shader ou fabricante de GPU.

### Evidência funcional no PC

Foi usado o núcleo UWP de produção, backend Dozen, armazenamento real e
injeção de controle no diagnóstico nativo. A criação e confirmação do contrato
foram exercitadas; as repetições após as correções usaram o checkpoint gerado
por essa passagem. O personagem chegou à clínica com HUD, recebeu comandos e
caminhou com o inventário fechado. As capturas foram feitas fora da janela de
medição; não houve pausa durante os comandos de movimento.

A revisão final permaneceu **8,1075181 segundos** sob comando de movimento:
2811 → 2854 apresentações registradas no intervalo, contador de erros SDK
D3D12 em zero antes, durante e depois, sem crash ou falha de pipeline.
Ao encerrar: `D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`, `SHUTDOWN=0` e
`PROCESS_EXIT_CODE=0`. Isso é validação funcional no PC, não aprovação de
desempenho, de toda a fidelidade gráfica ou de execução no Xbox/AppContainer.
O log ainda contém avisos `SanitizeCopyLayers`; eles não são erros D3D12 nem
foram considerados prova de fidelidade das cópias.

Evidências no diretório ignorado `Build/playable`:

- `installed-dump.log`: diagnóstico do dump instalado.
- `after-live.log`: falha DXIL reproduzida após finalizar o contrato.
- `clean-live.log`: execução final com camada de diagnóstico e contadores SDK.
- `gameplay-window.json`: horários, duração e contadores da janela de movimento.
- `live32.png` / `live33.png`: antes e depois do movimento, com HUD e sem menu.
- `core-build.log`, `dozen-final-barrier-build.log`, `host-build.log`: compilação.
- `package-sign.log`, `package-verify.log`: assinatura e conteúdo do bundle.

O bundle Release x64 foi recompilado, assinado e verificado contra as DLLs de
produção (`CMS_SIGNATURE=PASS`, `PAYLOAD_HASHES=PASS`), sem instalação nesta
revisão.
