# Substituição do gerenciamento de memória

## Backup

Snapshot anterior à migração:
`C:/Users/rodri/Dev1/Projetos/GR2fork-memory-backups/20261103-225932`.

Inclui 1.228 arquivos de core, common, renderer, integração UWP, projeto UWP e
configuração de compilação. `manifest.csv` registra caminho, tamanho e SHA-256;
os 1.228 hashes das cópias foram conferidos sem divergências. Binaries e pastas
de saída não fazem parte do snapshot. A restauração deve copiar apenas os
arquivos desejados, sem limpar o checkout.

## Estado

Migração implementada e validada no PC. A compilação UWP usa `src/core/uwp/kyty/memory_api.cpp`
e `memory_backend.cpp`, com a máquina de estados e o store do Kyty. O backend C
anterior (`guest_memory.c`, `guest_memory.inc` e `guest_swap.inc`) foi removido
após conferir suas cópias no backup.

As janelas lógicas de residência têm 64 KiB. Mapeamentos físicos e aliases
continuam aceitando 16 KiB e proteções de 4 KiB. Os offsets físicos selecionam
uma única janela de residência mesmo quando há endereços virtuais distintos.

Foram integrados commit sob demanda, sweep/segunda chance, cancelamento de
eviction por acesso, store temporário com slots e checksums, publicação de
page-in por views preenchidas previamente, thread auxiliar de faults, pager,
privatização sem quebrar aliases compartilhados, pressão e orçamento adaptativos,
pins e cópias de backing que consultam slots sem page-in desnecessário.

## Evidência atual no PC

Compilação integrada Release concluída em
`Build/kyty-memory-migration/integrated-build.log`.

Pac-Man com limite guest de 64 MiB: primeira apresentação em 3.734 ms,
5.687 frames em 100 segundos, captura da tela inicial conferida, `RUN_RESULT=0`
e `SHUTDOWN=0`. O pager registrou 21.615 expulsões, 20.449 restaurações,
46 cancelamentos e nenhum erro de I/O ou checksum. Logs e captura ficam em
`Build/kyty-memory-migration/pacman-pressure-*`.

A execução inicial sem pressão chegou à tela inicial em 3.672 ms, mas revelou
um erro de ordem de destruição no shutdown. A execução sob pressão acima já
inclui a correção desse lifetime.

Bloodborne também executou por 100 segundos: 2.729 frames, captura da tela
`PRESS THE X BUTTON` conferida, `RUN_RESULT=0` e `SHUTDOWN=0`, com nenhum erro
de pager registrado. Evidência em `Build/kyty-memory-migration/bloodborne-*`.
A integração CPU/GPU foi revisada abaixo. Nenhum desses resultados comprova
execução no Xbox.

O bootstrap UWP agora reserva faixas de VA antes da inicialização da interface,
sem carregar o core antecipadamente. A posse é transferida por uma API explícita
ao core do jogo; o address space adota essas reservas e completa as demais.
O core recebe as reservas também quando é inicializado no modo frontend e as
mantém até a construção do address space. Reservas não transferidas são
liberadas pelo seu proprietário.

Chronicles of Teddy foi executado com esse bootstrap e orçamento guest de
64 MiB: três faixas antecipadas adotadas, primeira apresentação em 12.984 ms,
5.128 frames em 100 segundos, captura da seleção de arquivo conferida e
`RUN_RESULT=0`/`SHUTDOWN=0`. Foram registradas 1.636 expulsões, 928 restaurações
e nenhum erro de pager. Evidência em `Build/kyty-memory-migration/teddy-bootstrap-*`.

A validação privada com os objetos de produção verificou aliases bidirecionais,
remoção do owner, remap, cópia direta de slots, atualização de backing evicted,
cancelamento de descarte antigo após reutilização, zeragem, 64 ciclos de slots,
proteções que devem cair no tracker e privatização com nova promoção de alias.
Foram 275 verificações aprovadas e 20 views convertidas. A validação também
incluiu cópias de aliases protegidos, corrupção deliberada de um slot e truncamento
deliberado do arquivo: a página permaneceu inacessível após a falha, os erros
`ERROR_CRC` e `ERROR_HANDLE_EOF` foram preservados e a restauração posterior
recuperou os dados originais. Evidência em
`Build/kyty-memory-migration/mapping-validation.log`. Os erros deliberadamente
injetados nessa validação não representam falhas das execuções de jogos.

Core e frontend Release compilaram em `recovery-core-build.log` e
`recovery-frontend-build.log` nesse mesmo diretório. Não foi gerado ou instalado
um pacote para essa validação.

## Contratos implementados e conferidos

- Reservas de VA e commit sob demanda com janelas de 64 KiB e proteção de 4 KiB.
- Estados swept, touched, evicting e evicted com segunda chance e cancelamento.
- Store temporário, slots reutilizáveis, integridade e publicação segura de page-in.
- Pager e executor de faults com admissão, drenagem e encerramento controlados.
- Pressão agregada, recuperação após falha e controle separado de views.
- Privatização de views quando não houver aliases físicos que a impeçam.
- Preservação do conteúdo físico entre unmap/remap e aliases simultâneos.
- Pin de stacks e leases temporárias para CPU, PM4, I/O e recursos em voo.
- Cópias de backing sem page-in desnecessário e coerência com dirty tracking GPU.
- Teardown explícito após quiescência de consumidores CPU/GPU.
- Remoção do backend anterior somente após integração de todos os consumidores.
- Compilação e validação no PC; resultado de Xbox depende de nova execução no Xbox.

A implementação lazy upstream ignora o offset físico em seu caminho privado;
copiar esse comportamento diretamente quebraria os aliases usados pelo GR2fork.
O adaptador resolve essa diferença usando janelas indexadas pelo backing físico.

## Integração CPU/GPU conferida

| Contrato do modelo Kyty | Integração no GR2fork |
| --- | --- |
| Reserva antes da interface e adoção pelo address space | `GuestMemoryBootstrap`, `GuestReservations`, `shadps4_adopt_guest_memory_reservations` e `AddressSpace::Impl` |
| Backing, regiões, proteção e fault | `GuestMemory`, `Xbox::Lazy::Regions` e `AddressSpace::HandleBackingFault`; a unidade de residência usa o offset físico para manter os aliases |
| Stacks, fibers e acesso host temporário | `PinMappings` popula e mantém pins; `AcquireHostAccess` mantém leases durante I/O, PM4 e cópias |
| Fault de tracker separado de fault de residência | Proteções guest continuam sendo respeitadas; o acesso negado cai no `PageManager`/controle de acesso GPU e não é confundido com falha de alocação |
| Cópia host e backing evicted | `CopyBacking` e `CopyPhysical` leem slots sem page-in; aliases protegidos são resolvidos fisicamente para não gerar faults recursivos |
| Estabilidade durante unmap | `GuestCopyScope`, janelas de publicação e drain das lanes mantêm a ordem antes de alterar os mapeamentos |
| Readback e dirty tracking Vulkan/Dozen | `BufferCache::DownloadMemory` publica e consome escritas após o tick; `Scheduler::DrainPendingOperations` exige fence concluído |
| Readback e recursos nativos D3D12 | `CommandRuntime::CompleteContext` espera o fence antes de completar downloads e soltar a lease; `TransferPool` só reutiliza/expulsa recursos concluídos |
| Encerramento | O pager fecha a admissão e drena seus requests; os workers/threads guest e o processador GPU são encerrados antes de destruir o backing; leases remanescentes têm deleter seguro |

Os caches e allocators de GPU continuam sendo os dos renderers do GR2fork,
conectados ao novo backing por esses contratos. A pressão agregada inclui a
memória do processo, portanto o pager não considera o guest como único consumidor.
Não há uma segunda implementação do antigo pager C em paralelo.

## Validação final de gameplay

Com três faixas antecipadas, orçamento guest de 64 MiB e limite de views reduzido
para 300 na validação privada:

| Execução | Primeira apresentação | Duração | Frames | Encerramento |
| --- | --- | --- | --- | --- |
| Pac-Man 256, Dozen | 4.719 ms | 100 s | 5.457 | `RUN_RESULT=0`, `SHUTDOWN=0` |
| Chronicles of Teddy, Dozen | 3.938 ms | 130 s | 7.403 | `RUN_RESULT=0`, `SHUTDOWN=0` |

As capturas finais foram conferidas em gameplay: o labirinto do Pac-Man e a
personagem do Teddy no quarto após selecionar o arquivo. O Pac-Man registrou
29.947 expulsões/27.057 restaurações. O Teddy registrou 3.255 expulsões,
2.246 restaurações e 2.950 privatizações. Ambos encerraram com `errors=0` e
`corrupt=0`. Logs e capturas estão em `Build/kyty-memory-migration/*-final-*`.

Essas execuções usam um host privado no PC, com o core de produção, Dozen SM6.4
e apresentação real. Não comprovam comportamento do AppContainer instalado,
de todos os jogos ou do Xbox. Não foram adicionados testes ao build do projeto.

## Conferência final

O consumidor D3D12 nativo também executou o Pac-Man por 180 segundos sob o
mesmo orçamento guest de 64 MiB: 118 frames, `RUN_RESULT=0`, `SHUTDOWN=0` e
sem erro de pager. Evidência em `pacman-native-runtime.log` e no diretório
`pacman-native-user`. Essa execução valida o caminho de consumo e encerramento
do backing; não é uma medição de desempenho representativa. A API de captura
usada pelo host privado rejeita o backend nativo, portanto essa execução não
fornece uma validação visual nativa. As capturas de gameplay conferidas acima
são do Dozen.

A última compilação do core consta em `final-core-build.log`; o frontend Release
foi recompilado com código de saída zero em `final-frontend-build.log` e
`final-frontend-exit.txt`. O core compilado, o distribuído em `Runtime/Release`
e o usado nos hosts privados possuem o mesmo SHA-256:
`353A178F25648AE0EE4DB6F5634D90A5FAF5F560B7091971CA95FD4288BF8D67`.

O backup foi reconferido ao final: os 1.228 hashes continuam iguais ao manifesto.
O CMake não inclui o backend anterior e não restam includes para seus três
arquivos removidos. As validações privadas permanecem somente em `Build`, fora
do build e dos arquivos versionados do projeto. A integração e a validação PC
da migração estão concluídas; validação de hardware Xbox continua externa a
essas evidências.

O descarte adiado usa uma lista intrusiva na metadata física, sem alocar ao
enfileirar/liberar. Novos mapeamentos cancelam descartes de gerações anteriores.
Janelas vazias são descartadas sem commit para zeragem. O store prepara a
capacidade de metadata antes de entregar um novo slot, de modo que a devolução
não precise alocar. A expulsão do pager possui rollback para exceções e falhas
de I/O. A privatização pode liberar views elegíveis de uma janela sem quebrar
as páginas que ainda possuem aliases residentes compartilhados.

## Correção da inicialização pelo frontend e velocidade

Os logs do aplicativo instalado revelaram uma lacuna da validação original:
`ShadPS4Host` era inicializado com `frontend=true`, inclusive para executar jogos,
mas a transferência das reservas estava condicionada a `frontend=false`.
O core não via as faixas reservadas e falhava ao reservar `eboot.bin` e o heap
de threads. O host privado original adotava as reservas diretamente e não
exercitava essa condição do frontend.

`ShadPS4Host::Initialize` agora transfere as reservas independentemente desse
modo. O core continua sendo seu único proprietário após a transferência.

Nas cópias físicas, o acesso já permitido pelas proteções registradas evita
alterar/restaurar proteções de página. Proteções uniformes são aplicadas em um
único trecho, e novos mapeamentos RW não repetem o processamento de restrições
que não mudaram. A restauração após sweep/eviction só muda o estado quando a
alteração de proteção realmente teve sucesso.

Uma medição privada dos objetos de produção, copiando 500 MiB em blocos de
1 MiB, registrou:

| Operação | Antes | Depois |
| --- | --- | --- |
| Leitura | 191,725 ms | 25,090 ms |
| Escrita | 316,645 ms | 21,847 ms |

Esses valores medem o caminho de backing com memória residente acessível no PC,
não FPS nem o tempo total de inicialização. As 275 verificações de aliases,
proteções e falhas de armazenamento continuaram passando. Evidência em
`Build/startup-regression-fix/copy-before.log`, `copy-after.log` e
`mapping-validation.log`; builds em `core-build.log` e `frontend-build.log`.
O pacote atualizado instalado no PC e o hardware Xbox ainda precisam de
validação própria; a compilação do frontend não equivale a essa execução.

O core corrigido executou o Pac-Man por 100 segundos no host privado: primeira
apresentação em 6.563 ms, 5.300 frames, captura de gameplay conferida,
`RUN_RESULT=0` e `SHUTDOWN=0`, com pressão guest de 64 MiB. Essa execução
confere o core e a adoção explícita, sem reproduzir a interface XAML instalada.
O defeito da guarda `frontend` foi identificado pelo log real instalado e
corrigido no frontend compilado. Não se atribui melhora do carregamento a essa
medição isolada; o ganho medido acima é o das cópias.

Logs e captura: `Build/startup-regression-fix/pacman-*`. O SHA-256 do core atual
compilado, distribuído e usado nessa execução é
`63B09D7523A485FC1E4E5042491F77D5972C57010324B5049C6CCC2DB2443E1F`;
o hash anterior neste documento pertence à versão inicial da migração.

## Correção da regressão de gameplay em jogos grandes

O caminho de cópias frequentes criava duas leases temporárias por cópia, e a
liberação procurava cada token percorrendo todos os pins temporários. Na
execução de referência do Bloodborne havia aproximadamente 18,5 mil pins ativos.
Isso multiplicava o trabalho de CPU mesmo com swap desativado no PC.

Os tokens agora usam slots reutilizáveis com geração, e a consulta de pins usa
contadores por janela em páginas esparsas. Remover um token não percorre os
outros tokens nem aloca memória. Descartes adiados são encadeados por janela
física; a liberação só examina as janelas do token liberado. A referência medida
não tinha descartes pendentes, portanto essa última mudança evita outro custo
de escala, mas não explica o ganho medido nela.

`CopyBacking` deixou de adquirir as duas leases redundantes: a cópia síncrona
do backend já mantém os locks de mapeamento/cópia e residência durante toda a
operação. Leases de I/O, PM4, stacks e recursos em voo continuam preservadas.
Isso também evita criar metadata de views apenas para esse acesso ao backing.

Comparação sequencial no host privado PC, usando o core de produção e Dozen,
com cópias idênticas do save, configuração e cache, na mesma posição e câmera
do Hunter's Dream. O save original do usuário não foi alterado. Janela de
medição: 160–190 segundos após o início, sem pedidos de captura nesse intervalo.

| Versão | Frames no intervalo | FPS médio |
| --- | --- | --- |
| Antes | 177 | 5,90 |
| Corrigida | 895 | 29,83 |

Ambas encerraram com `RUN_RESULT=0` e `SHUTDOWN=0`; as capturas de gameplay foram
conferidas. O snapshot final passou de 9.143.668 para 284.910 liberações de
leases, e de 9.874 para 40 views. Esses contadores abrangem execuções com
quantidades diferentes de frames e não são taxas por frame. Ambas registraram
zero erros e zero corrupção do store.

As 275 verificações funcionais do backend passaram. A validação dos pins
passou 962.363 verificações, com 100.000 operações aleatórias e seis pontos de
falha de alocação. Quatro leitores completaram 20.000 cópias concorrentes com
500 ciclos de expulsão/page-in, sem divergência dos dados. Esses diagnósticos
privados permanecem em `Build/gameplay-regression-fix`, fora do código
versionado. Os logs `baseline-safe-runtime.log`, `optimized-safe-runtime.log`
e `gameplay-comparison.json` registram a comparação; os subdiretórios
`*-safe-user/screenshots/presentation` contêm as capturas.

Core e frontend Release foram compilados. O SHA-256 do core compilado,
distribuído em `Runtime/Release` e usado na execução corrigida é
`4ADB6BA5057A903143E596735BDD0BAECC0D09B0FB79D8D0F009C7C731EDC846`.
Não houve instalação de pacote. O resultado de FPS é do PC no host privado;
o aplicativo instalado e o hardware Xbox ainda precisam de validação própria.

## Pressão do pager no Xbox

Os logs Xbox de 04/11/2026 mostraram 69.059 expulsões e 35.618 page-ins,
com orçamento reduzido de 5.120 para 2.560 MiB. A folga de 768 MiB e o início
da pressão a 15/16 do orçamento antecipavam a paginação. O caminho de commit
ainda usava outra folga, de 384 MiB, para expulsão preventiva.

O pager agora deriva seu orçamento de residência a cada amostra do consumo
total do aplicativo. A folga é de 220 MiB, limitada a 1/16 do limite para
processos menores: no Xbox com limite de 5.120 MiB, o alvo é 4.900 MiB.
O caminho preventivo de commit usa o mesmo alvo. Não há redução persistente
do orçamento após uma recuperação de emergência nem espera de 30 segundos
para começar a recuperá-lo. O override explícito `SHADPS4_UWP_GUEST_RAM_MB`
continua sendo respeitado.

Cada lote para de expulsar quando a residência cai até o orçamento calculado.
Candidatos antigos são restaurados sem copiar seu conteúdo se a pressão de
bytes e de views já passou. A pressão de views continua podendo privatizar
aliases elegíveis sem enviar seu conteúdo ao swap. Falhas reais de commit
continuam permitindo recuperação imediata mesmo abaixo de 4.900 MiB, pois o
limite nominal do aplicativo não garante que o sistema aceite uma alocação.
Pins, leases, verificação de conteúdo e rollback permanecem preservados.

O diagnóstico `[KytyMemory]` inclui `target_mb` para o alvo total do aplicativo;
`budget_mb` passa a mostrar o orçamento guest efetivo da amostra, considerando
o consumo das demais partes do processo. O core e o frontend Release foram
compilados; as 275 verificações do backend e 20.000 cópias concorrentes com
500 ciclos de expulsão/page-in passaram. Evidências privadas em
`Build/xbox-pager-pressure-fix`. A eficácia sob pressão no Xbox depende de
nova execução no aparelho; não houve instalação de pacote.

O core atualizado também executou o Bloodborne por 175 segundos no host
privado PC com swap habilitado e cópia isolada do save. A captura confirmou
gameplay no Hunter's Dream, com aproximadamente 30 FPS entre 155 e 165 segundos;
a execução encerrou com `RUN_RESULT=0` e `SHUTDOWN=0`. O limite de memória do PC
é maior: essa execução não reproduz a pressão de memória do Xbox.

## Diagnóstico de parada de progresso

Com o pager habilitado, uma thread independente registra `[MemoryWatch]` a
cada cinco segundos, sem adquirir os locks de memória ou de cópia. O registro
usa capacidade fixa de 128 operações; `dropped` denuncia falta de slots.
Não há gravação por acesso normal nem alteração de timeouts ou recuperação.

Operações cobertas: privatização, recuperação/sweep, fila e worker de page-in,
ciclo do pager, submissão gráfica, drenagem de cópias antes da submissão e
esperas do scheduler por gravação/submissão ou semáforo GPU. Uma operação
pendente por pelo menos cinco segundos informa nome, etapa, thread, tempo,
endereço ou tick e o erro Win32 observado. As etapas do page-in distinguem
espera pelo worker, espera pelo mutex de memória e resolução do fault. A
privatização distingue liberação da view, reserva/commit privado, cópia,
restauração de proteção e rollback do alias.

`status=last_privatization` conserva a última privatização que retornou,
inclusive seu endereço e etapa final. Esse registro é histórico, não uma
operação ainda bloqueada. `completed` conta operações instrumentadas que
retornaram; não conta frames. O diagnóstico não suspende threads nem captura
stacks e não cobre todas as esperas guest; ausência de `pending` não prova que
o jogo está avançando. `ShadPS4Host` também encaminha `[MemoryWatch]` ao
`uwp-session.log`, além do `shad_log.txt`.

O core Release compilou e as 275 verificações existentes do backend passaram.
Um probe privado confirmou o relatório de operação pendente e de conclusão,
com preservação de `GetLastError`. Evidências em
`Build/xbox-hang-diagnostics`. A causa do congelamento no Xbox permanece
pendente de uma nova captura com este diagnóstico.

## Preservação do backing compartilhado durante a execução

A captura seguinte não registrou `status=pending` nem slots perdidos. A última
privatização retornou antes da parada de progresso, e o pager continuou ativo.
Isso não comprovou a causa do congelamento. Entretanto, a revisão revelou
uma janela de concorrência real: a conversão removia a view, fazia commit RW
no endereço guest e só então copiava os dados, expondo conteúdo incompleto às
threads guest que não adquirem o mutex do backend.

O backend deixou de converter views residentes para memória privada. Mantém
o backing compartilhado e não executa sweeps apenas por exceder a contagem
de views. `view_limit` permanece no log como referência diagnóstica, sem
provocar conversão nem swap. Essa decisão substitui o comportamento de
privatização descrito nas seções históricas acima. Não há tentativa de
preencher memória privada inacessível usando APIs que exigem acesso RW ao
mesmo endereço guest.

A paginação por consumo de RAM e a recuperação após falha real de alocação
continuam ativas, assim como aliases, proteções, leases e `[MemoryWatch]`.
O contador `privatized` deve permanecer em zero nesta versão. As verificações
privadas foram ajustadas para exigir a retenção do backing ao exceder o
limite de views e conferir o conteúdo e a coerência de um novo alias.
Evidências e builds ficam em `Build/live-alias-fix`. A resolução do
congelamento precisa de nova validação no Xbox.

Core e frontend Release compilaram. Passaram as 275 verificações funcionais
ajustadas e 20.000 cópias concorrentes com 500 ciclos de expulsão/page-in,
sem erros ou corrupção. Não houve instalação de pacote.

## Reutilização de seções, page-in pendente e áudio

O retorno do swap prepara uma seção de 64 KiB por janela física, em vez de
quatro seções independentes de 16 KiB. As views guest continuam com sua
granularidade original e apontam para os offsets da seção compartilhada.
O conteúdo da janela inteira é preparado pela view scratch antes de publicar
qualquer endereço guest. Não há conversão de views vivas para memória privada.
Offsets de página com `MEM_REPLACE_PLACEHOLDER` são suportados pela
[API MapViewOfFile3](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile3).

Uma única view scratch fica disponível para reutilização; a troca de seção
desfaz a view anterior. Até oito seções vazias são reutilizadas, com retenção
máxima de 512 KiB. Uma seção só fica reutilizável depois que todas as páginas
físicas da janela deixaram de referenciá-la e suas views foram removidas.
Acima do limite diagnóstico de views, o pager pode remover mirrors canônicos
redundantes, mantendo outro owner residente e o mesmo backing. Janelas com
pins são preservadas; um acesso posterior pode reconstruir o alias.

Falhas transitórias de recursos deixam o fault pendente para nova tentativa,
com intervalo de 100 ms por pedido, sem reter os locks durante a espera e
sem impedir que outros pedidos prontos sejam processados. O slot do swap só
é liberado após um page-in bem-sucedido. CRC inválido e outros erros definitivos
continuam sendo propagados. O encerramento interrompe as novas tentativas.
Isso não garante progresso se a pressão nunca cessar: `[MemoryWatch]` mostra
o pedido pendente, e `fault_retries` identifica as tentativas.

Os relatórios acrescentam contadores de seções criadas/reutilizadas/vivas,
cache de seções, mapas/reutilizações scratch e views removidas. `failure_api`,
`failure_error`, `failure_address` e `failure_section` preservam a última
falha de alocação/mapeamento observada, inclusive quando houve recuperação;
não representam necessariamente uma falha ainda ativa.

No áudio UWP, `sceAudioOutOutput` e `sceAudioOutOutputs` preparam os samples
fora dos mutexes das portas. A cópia reconhece aliases guest e pode ler do
store sem trazer a janela para RAM. A cópia dos parâmetros da chamada com
múltiplas portas também ocorre antes dos locks. Assim, uma espera de memória
guest não impede a thread de entrega de consumir o buffer host já preparado.
`[AudioSupply]` registra lacunas de entrega maiores que 100 ms e quatro períodos
do buffer, no máximo uma vez por segundo por porta. A presença de estalos
durante o carregamento foi informada pelo usuário; a validação auditiva no
Xbox continua necessária, pois o PC não reproduz a mesma pressão.

As 275 verificações funcionais passaram. Quatro leitores completaram 20.000
cópias com 500 ciclos de paginação: uma seção foi criada e reutilizada 499
vezes, sem erros ou corrupção. A injeção de Win32 8 confirmou retenção do
slot, leitura direta por alias enquanto o fault aguardava e recuperação após
remover a falha. Outra verificação reduziu 24 views para 16, preservou os
dados e reconstruiu o acesso canônico. Diagnósticos privados e builds:
`Build/section-reuse-audio-fix`. Nenhum pacote foi instalado.

Core e frontend Release compilaram. O core final executou o Bloodborne no
host privado PC por 175 segundos com swap habilitado e save isolado. A
captura confirmou gameplay no Hunter's Dream; `RUN_RESULT=0` e `SHUTDOWN=0`.
A primeira execução permaneceu no menu e não foi usada como evidência de
gameplay. Essa validação não reproduz o limite de memória nem constitui
verificação auditiva das transições de áreas no Xbox.

## Diagnóstico dos três sintomas de recursos e áudio

`[MutexDiag]` registra a primeira criação e cada 1.024 criações de
`TimedMutex`, com totais de criação/fechamento, quantidade viva, pico e falhas.
Uma falha de `CreateMutex` preserva o Win32 imediatamente, registra commit
disponível/privado/total e até oito frames de stack com base do módulo e offset
antes do assert existente. Falhas de fechamento também são registradas.
Esses contadores cobrem `TimedMutex`, não todos os handles do processo.

`[MappingDiag]` registra falhas de alocação/mapeamento com API, erro original,
thread, endereço, handle, views, seções e snapshot de commit, no máximo uma
vez por segundo. O total de falhas continua contando as ocorrências suprimidas.
A cada cinco segundos do pager, registra deltas de criação/reutilização,
variação de views, pico amostrado de views e total de seções fechadas. Isso
permite distinguir pressão de RAM, churn e baixa reutilização. O snapshot
do sistema informa `memory_info=0` se a consulta não estiver disponível.

`[AudioQueue]` é produzido na thread de submissão, sem logging nos callbacks
realtime do XAudio2. Informa voice, buffers queued/busy/capacity,
submetidos/concluídos/rejeitados, samples reproduzidos, maior intervalo entre
tentativas de submissão, HRESULT do submit/voice e duração aproximada da fila.
Há relatório periódico a cada cinco segundos enquanto há submissões,
registro da primeira rejeição e registros limitados para fila cheia/lacunas.
`empty_observations` conta snapshots sem buffers enfileirados depois da
primeira submissão; isoladamente não comprova um underrun audível.
Os registros devem ser correlacionados com `[AudioSupply]` e `[MemoryWatch]`.

`ShadPS4Host` encaminha os quatro prefixos para `uwp-session.log`, além do
log do core. A instrumentação não altera a política de paginação, a capacidade
da fila de áudio nem o comportamento do assert. Probes privados confirmaram
o erro Win32 1450 original de uma criação de mutex, uma falha de mapeamento
Win32 8 com dados preservados no swap e uma fila XAudio2 cheia com recuperação.
Evidências em `Build/three-symptom-diagnostics`. A análise dos sintomas reais
depende da próxima execução no Xbox.

## Redução de churn de mutexes, timers e seções

`TimedMutex` reutiliza até 128 handles nativos liberados, com controle de
aquisições para não devolver um mutex ainda adquirido ao cache. As esperas
continuam usando as APIs alertáveis existentes. `[MutexDiag]` distingue os
objetos lógicos dos handles nativos criados/fechados e reutilizados. Uma falha
real de criação, sem handle elegível no cache, mantém o diagnóstico e o assert.

`AccurateSleep` mantém um timer ocioso por thread, retirando-o do cache durante
a espera. Assim, uma chamada aninhada por APC usa outro timer. A espera
interrompida cancela o timer antes de reutilizá-lo; a próxima chamada rearma
seu estado conforme o contrato de
[SetWaitableTimer](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-setwaitabletimer).
Falhas de criação/armação usam `SleepEx` e registram `[WaitTimer]`, em vez de
esperar em um handle inválido. Isso reduz o churn também na thread de entrega
de áudio, sem alterar os samples nem aumentar a fila XAudio2.

A view scratch do pager passa a ocupar um placeholder fixo de 64 KiB,
substituído com `MapViewOfFile3` e preservado entre trocas de seção. O cache de
seções vazias cobre um lote de 128 janelas, retendo no máximo 8 MiB. Isso
substitui o cache anterior de oito seções. As regras de ownership, pins,
rollback e retenção do slot no swap continuam preservadas.

Probes privados em `Build/resource-churn-fix` verificaram 100 mil objetos de
mutex com um único handle nativo, 20 mil atualizações sob contenção, timeout
e reutilização quando novas criações eram recusadas. O timer passou por
reutilização, APC aninhada, cancelamento e fallback sob falha injetada.
Três restaurações de 128 janelas criaram 128 seções e reutilizaram 256,
mantendo o endereço scratch fixo e os dados íntegros. Passaram ainda as
275 verificações do backend e a retenção/recuperação do swap com falha injetada.
O efeito sobre os estalos e o esgotamento de recursos no Xbox depende de
nova validação no aparelho.

Core e frontend Release compilaram. O core final executou o Bloodborne por
175 segundos no host privado PC, com captura de gameplay conferida e
`RUN_RESULT=0`/`SHUTDOWN=0`. Uma amostra do jogo registrou 47.104 objetos de
mutex, 3.600 handles nativos criados e 43.504 reutilizações, sem falhas de
criação/fechamento. Passaram também 20.000 cópias concorrentes com 500 ciclos
de paginação. Não houve instalação de pacote. Essa execução não mede o
efeito nas transições de área sob pressão real do Xbox.

## Diagnóstico dos semáforos e ordem de falhas

`[SemaphoreDiag]` agrega os semáforos Windows dos diferentes limites de
permissões. Registra a primeira criação e cada 1.024 criações, com objetos
criados/fechados/vivos, pico, falhas e chamadas de espera em andamento.
`timeouts` conta resultados `WAIT_TIMEOUT`, inclusive consultas sem bloqueio;
não representa sozinho um atraso do jogo.

Falhas de criação preservam imediatamente o Win32 original e registram
objeto, handle, thread, limite, contagem inicial, snapshot de commit e até
oito endereços de stack com base de módulo e offset antes do assert existente.
Falhas de fechamento, liberação e espera também são registradas. A
instrumentação mantém as esperas alertáveis e as regras de permissões,
sem adicionar cache de semáforos ou modificar a recuperação.

`[SessionFailure]` numera as chamadas de `Fail` e informa thread, estado da
sessão, pedido de parada e estado da falha de backing. A sequência reinicia
na inicialização do core. Ela ordena os relatos à API, não identifica por si
só qual erro foi fatal: erros de validação da API também passam por `Fail`.
Os estados de backing são 0=None, 1=Writing, 2=Pending e 3=Reported. A ordem
deve ser correlacionada com o primeiro `[SemaphoreDiag]`/assert e os registros
de mapeamento para investigar falhas secundárias durante o encerramento.

Os dois prefixos são encaminhados ao `uwp-session.log` e ao log do core.
O core Release compilou. O probe privado em `Build/semaphore-diagnostics`
confirmou Win32 1450 na criação, erro 298 por excesso de permissões e
funcionamento normal de wake/timeout, com contagens de criação/fechamento
consistentes. Os diagnósticos não demonstram ainda a causa no Xbox.

O probe também injetou erro 6 em espera e fechamento e confirmou os registros
e contadores correspondentes. Core e frontend Release compilaram; nenhum
pacote foi instalado.
