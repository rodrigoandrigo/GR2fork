# Memória UWP

O gerenciamento host UWP foi substituído pelo modelo de
`Test-project-arena-8ac58bdb-test-project/xbox/platform/guest_memory.inc` e
`guest_swap.inc`, incluindo `swap_codec.h` e `swap_pool_accounting.h`.
O pager anterior de seções de 16 MiB e views adaptativas foi removido do caminho
UWP. Windows desktop sem UWP e plataformas POSIX mantêm seus caminhos próprios.

## Modelo ativo

- Memória física lógica sem seção global previamente comprometida. Um único
  mapeamento usa páginas privadas de 16 KiB, comprometidas no primeiro acesso.
- Um alias físico vivo promove a página para uma seção compartilhada de 16 KiB.
  Escritas continuam visíveis por todos os aliases, inclusive pelo backing
  canônico usado pelos renderizadores. Seções executáveis são promovidas ou
  atualizadas com as permissões necessárias, preservando os dados.
- Desmapear memória direta preserva seus dados em snapshot, seção ou swap.
  Liberar a alocação física descarta o armazenamento; leases adiam esse descarte.
  Memória flexível reciclada começa zerada.
- Somente mapeamentos grandes de WC_GARLIC, sem execução, são candidatos a
  paginação. Memória flexível, CPU heaps, código, páginas compartilhadas e páginas
  fixadas não entram na retirada normal. Leases identificam as páginas físicas
  originais, mesmo quando o mapeamento virtual muda.
- Proteções de 4 KiB e a alternância escrita/execução permanecem preservadas,
  incluindo as proteções usadas pelo tracking de CPU/GPU.

## Swap e pressão

O clock de segunda chance retira páginas privadas frias em lotes de 2 MiB.
A política da referência inicia o trabalho abaixo de 640 MiB de folga no limite
real do processo e para novamente com 768 MiB. Ela substitui o limite fixo e as
inferências preventivas do pager anterior. Pedidos após falha de alocação podem
executar até três sweeps, com prazo limitado; as passagens posteriores podem
ignorar a segunda chance, mas nunca as leases.
Varreduras periódicas sem páginas recuperáveis usam intervalo progressivo de
40 a 500 ms. Crescimento de 32 MiB antecipa a próxima tentativa; uma recuperação
bem-sucedida restaura a cadência normal. Pedidos explícitos não usam esse atraso.

A criação de pipelines gráficos e compute também recupera memória guest quando
Vulkan retorna falta de memória do host ou dispositivo. Cada tentativa pede até
256 MiB ao pager, com no máximo três repetições e interrupção se nada for
recuperado. Os erros restantes continuam sendo propagados ao chamador.
Com margem inferior a 512 MiB, há uma tentativa preventiva de recuperar 128 MiB
antes da criação. Falhas `E_OUTOFMEMORY` da validação DXIL retornam
`VK_ERROR_OUT_OF_HOST_MEMORY`, em vez de erro desconhecido, mantendo a validação
ativa e permitindo que esse caminho de recuperação seja acionado.

O arquivo esparso temporário fica em `cache/swap`, com exclusão ao fechar.
Cada registro contém uma página de 16 KiB, comprimida quando útil, com marcador
para página zerada, fallback raw e verificação FNV. O teto padrão é 4 GiB e há
reserva de 384 MiB no dispositivo para saves e outros arquivos. O formato de
slots usa índices de 64 bits e limita o arquivo a 4 GiB. O pool de frames mapeados é limitado a
64 MiB e mantém seu compromisso de memória até o encerramento da sessão.
O acesso guest a dados restaurados não os fixa permanentemente nem promove sua
view para seção comprometida fora do swap. Leases adiam a retirada da view; após sua
liberação, a página volta a ser candidata e suas escritas são preservadas antes
da retirada. Código executável e a opção explícita `SHADPS4_UWP_PIN_PRIVATE`
mantêm a fixação exigida por suas políticas.

Do pager de `KytyPS5-xbox-on-upstream` foram adaptados o retorno por uma view da
página completa e a recuperação de mapeamentos separada dos bytes residentes.
Uma página de 16 KiB com proteção uniforme volta por uma única view já preenchida;
proteções distintas ou falta de suporte mantêm o caminho de subviews de 4 KiB.
Mudanças posteriores de proteção, promoção de aliases e liberação reconhecem os
dois layouts. Falhas de mapeamento tentam retirar views frias antes de paginar
outras páginas, respeitando pins e leases. Os contadores distinguem views nativas,
mapeamentos completos e recuperações de views.

As transferências do arquivo completam leituras e escritas parciais. Falhas de
quota ou recursos em transferências maiores permitem uma nova tentativa em
blocos de 4 KiB; progresso nulo é erro e o registro só é publicado após a gravação
completa. O log contabiliza essas tentativas separadamente.

Quando o armazenamento não aceita uma escrita, um pool comprimido em RAM de
192 MiB pode preservar a página. O compromisso desse pool é contado por bitmap
real e desfeito após a restauração. Se nenhum armazenamento aceitar a página,
ela permanece residente. Registros substituídos por zero e alocações físicas
liberadas também devolvem seus blobs. Escritas recusadas não consomem
permanentemente posições novas no arquivo.

`General.memory_swap_enabled` mantém a seleção do aplicativo. Os controles de
ambiente importados usam o prefixo `SHADPS4_UWP_`, em lugar de `BB_UWP_`.
Nenhuma configuração depende de um título específico.

## Integração

[AddressSpace](../src/core/uwp/address_space.inc) mantém a descoberta e reserva de
VA, arquivos mapeados e o contrato público do GR2fork. O
[gerenciador C](../src/core/uwp/guest_memory.c) substitui o backing e o pager;
[MemoryManager](../src/core/memory.cpp) continua implementando as syscalls guest,
os tipos físicos e a sincronização com os renderizadores. As capacidades guest
continuam sendo as do emulador, em vez de copiar constantes do runtime de um
único jogo da referência.

Cópias físicas leem ou escrevem o proprietário, snapshot ou registro comprimido
sem criar um alias canônico permanente. Acesso host que realmente precisa de um
ponteiro materializa e fixa suas páginas. Leases de PM4, arquivos, sockets,
readbacks e submissões GPU permanecem integradas.

Dozen/Vulkan processa liberações por fences concluídas e staging seguro antes
de recorrer à paginação guest em falhas de alocação. Sob pressão, draws e dispatches
retiram caches frios: arenas densas limpas com GPU concluída são reconstruídas
da memória guest no próximo uso, evitando manter sua cópia gráfica indefinidamente.
Buffers modificados pela GPU, alocações mantidas por CPU e recursos do tick atual
são preservados. Imagens limpas usam a retirada existente por fence; superfícies
de apresentação permanecem protegidas. A coleta não reentra nas estruturas de
um pool durante sua própria alocação e não adiciona espera global da CPU.

Uma recusa pelo orçamento estimado do heap permite a tentativa já existente sem
essa restrição antes de expulsar páginas guest, distinguindo a estimativa do heap
de uma falha real do dispositivo. `[AllocationRecovery]` registra bytes liberados,
buffers limpos retirados e imagens ainda aguardando descarte. D3D12 nativo mantém
seu caminho próprio de recuperação limitada de páginas guest.

Uma falha no handler de commit retorna o erro Win32 depois de liberar os locks,
sem propagar exceção C++ pela pilha guest e sem encerrar o processo. O mecanismo
existente de falha controlada da sessão permanece ativo. O pager tem parada,
join e limpeza de recursos antes de descarregar a DLL. Os contadores são
encaminhados ao logger do aplicativo.

## Diagnóstico da gravação Vulkan/Dozen

O diagnóstico é automático; os registros são encaminhados ao logger do aplicativo.
Não depende de reproduzir a falha no PC nem de ativar uma captura manual.

- `[VulkanFailure]`: operação, resultado, arquivo, linha e função do primeiro
  retorno Vulkan verificado que falhou. Chamadas sem rótulo usam a função chamadora.
- `[VkRecordMemory]`: fase da gravação, tick ativo, uso/limite/folga do processo,
  validade da amostra, comparação com a amostra anterior, fila de chunks,
  ticks submetidos/concluídos e orçamento/uso de cada heap, com contadores VMA.
- `[DozenRecordError]`: primeiro erro retido no command buffer, função/linha de
  origem, motivo de remoção do dispositivo, buffers internos, cópias de feedback
  e heaps de descritores. Distingue a origem do erro do `vkEndCommandBuffer` que
  eventualmente o devolve ao emulador.
- `[DozenInternalAlloc]`: função chamadora e sua linha/arquivo, command buffer,
  tamanho solicitado e tamanho do recurso, bucket upload/default, estado inicial,
  alinhamento, offset, caminho (novo, pool, upload ativo ou falha de metadata),
  resultado/HRESULT e bytes/recursos já retidos. Registra solicitações de pelo
  menos 64 MiB e qualquer falha, inclusive em solicitações pequenas. O chamador
  é capturado no ponto da chamada, antes de entrar no alocador compartilhado.
- `[DozenRecordAlloc]`: HRESULT e descrição dos recursos internos cuja criação
  falhou, além de alocações grandes; inclui bytes de alocação quando disponíveis.
- `[DozenDescriptorAlloc]`: falha de criação de heap, tipo, quantidade de
  descritores, tamanho individual e visibilidade para shaders.

As amostras normais da thread de gravação são limitadas a uma a cada 100 ms.
Crescimentos de pelo menos 128 MiB ou folga inferior a 640 MiB geram no máximo
um aviso por segundo. Uma falha força a amostra e os detalhes dos heaps.
Os diagnósticos não fazem replay de comandos nem esperas adicionais pela GPU.

A verificação com injeção privada de erro no PC registrou o primeiro erro interno
no Dozen, a falha em `vkEndCommandBuffer`, os heaps VMA e a situação da fila/fences;
a sessão retornou erro e concluiu o shutdown. A injeção fica apenas em `Build/`,
fora do código de produção. Isso verifica o diagnóstico, não reproduz a pressão
real de memória do Xbox.

A extensão de identificação do chamador foi verificada no PC: um upload real de
128 MiB registrou `caller=dzn_CmdFillBuffer`; uma criação recusada por injeção
privada registrou o chamador da injeção e `hr=0x8007000e`. O shutdown completou.
Isso não identifica retroativamente o chamador da falha anterior no Xbox;
é necessário coletar os novos registros nessa execução.

## Retenção de staging até a conclusão dos usos

Os buffers registram o último tick de uso, atualizado nas reservas, acessos de
bindings, cópias e readbacks, inclusive nos acessos sem tracking de barreiras.
`StreamBuffer::LastTick()` reflete esse uso, em vez de somente a última reserva.
A reutilização e a limpeza de buffers grandes também consideram esse tick.

`TrimRing` e `TrimLarge` retiram os buffers elegíveis do pool e transferem sua
posse para `Scheduler::DeferOperation`, vinculada ao tick atual. A destruição
acontece após a fence desse lote, cobrindo a gravação assíncrona ainda enfileirada.
Não há nova espera global nem alteração dos orçamentos ou gatilhos de pressão.

A verificação privada no PC exercitou buffers de ring/grandes ocupados, retenção
antes da fence e destruição depois dela, além de limpeza forçada por frame.
O Bloodborne de produção completou 95 segundos/2.547 frames com retorno e
shutdown normais. O Pac-Man com limpeza forçada completou 95 segundos/5.351
frames e imagem conferida, também sem erro. A continuidade no Xbox exige
validação com o novo runtime.

## Diagnóstico de vida útil dos readbacks

`[ReadbackDiag]` correlaciona `CommandRecorder::copyImageToBuffer` com o ciclo
de vida dos buffers no Dozen. Cada cópia recebe um identificador e registra
chamador/linha, imagem, destino, quantidade de regiões, offset inicial, thread
e geração do handle nas fases 0 (enfileiramento), 1 (início da gravação) e 2
(fim da gravação host; não significa conclusão da GPU).

O registro limitado mantém tamanho, recurso D3D12, memória vinculada e sequências
de criação/vínculo/destruição. A pilha de destruição acompanha os eventos mesmo
quando o endereço é reutilizado. O histórico contém os últimos 32 eventos;
o índice de buffers é fixo e pode perder entradas por colisão/reciclagem, caso
em que o destino aparece como desconhecido, sem comprovar destruição.

Um destino suspeito ou uma exceção host de acesso à memória descarrega o
histórico, o estado atual das regiões (`VirtualQuery`) e os offsets das pilhas.
O dump usa metadados separados e não dereferencia o objeto inválido; o logger
é descarregado antes de prosseguir. O rastreamento não retém recursos nem altera
a ordem das cópias. Se o lock estiver ocupado na exceção, registra `registry_busy`
em vez de aguardar esse lock.

Verificações privadas no PC confirmaram destino válido, destruído, reutilizado
e desconhecido, além do dump automático em uma exceção injetada. O Bloodborne
chegou ao menu com captura conferida e completou 95 segundos/2.562 frames,
retorno e shutdown normais. A causa específica no Xbox requer novos logs.

## Revisão das arenas densas

`[DenseArena] growth` identifica o intervalo pedido, a capacidade resultante,
arenas/bytes incorporados, ampliação da cobertura VA e tick. O resumo a cada
cinco segundos mostra capacidade atual, ocupação dos intervalos registrados em
`resident_ranges`, pedidos/reutilizações, bytes alocados cumulativamente e
cópias de migração gravadas. Lista até 16 arenas com intervalo, último uso e
estado da fence. A ocupação lógica não comprova que os dados ainda são usados
pelo jogo; `gpu_done` também não significa que uma arena pode ser descartada.

`[ResourceMemory]` separa `dense_active` de `dense_retired`, mantida até a
liberação diferida após a fence. O diagnóstico não altera expansão, alocação,
reciclagem ou sincronização das arenas. O índice de descritores foi ampliado
para 32.768 entradas, com busca limitada a 128 posições para evitar varreduras
longas. Perdas continuam explícitas. A verificação privada registrou/liberou
9.000 entradas simultâneas sem perdas e exercitou o caminho denso no PC.

## Redução de alocações no Dozen

As arenas densas arredondam a capacidade para 1 MiB ou para o tamanho do bloco,
se maior, em vez de 16 MiB. Regiões apenas adjacentes permanecem separadas;
intervalos que atravessam arenas unem as regiões sobrepostas, copiam os dados
na GPU e mantêm os buffers anteriores até a fence.

Ao expandir uma única arena, reserva-se mais 1/8 da capacidade anterior,
arredondado à página densa, com limite de 4 MiB quando o bloco não excede esse
tamanho. A reserva é limitada pelas arenas vizinhas e pelo espaço de endereços;
alocações iniciais e uniões de múltiplas arenas não recebem essa reserva.
Isso evita realocações a cada MiB durante crescimento sequencial sem voltar
a unir regiões apenas adjacentes. Uma verificação privada na GPU reproduziu
25 → 26 → 27 → 28 → 29 MiB com uma migração de 25 MiB para capacidade de 30 MiB;
o readback confirmou os dados antigos e zeros na extensão. A limitação por
uma arena vizinha também passou. A redução do pico no Xbox requer nova execução.

Push descriptors usam pools de até 16 snapshots e reaproveitam espaço de pools
compatíveis do mesmo command buffer. Cada atualização recebe um novo set;
atualizações parciais preservam os bindings anteriores. O reset continua
liberando os pools após a conclusão do uso do command buffer.

A verificação GPU no PC, com SM6.4 e barreiras legadas, preservou 80 snapshots
com dois layouts alternados usando seis pools, em duas execuções com reset.
Uma compilação privada forçou arenas densas no Pac-Man, verificou separação,
união e liberação após fence, apresentou imagem e encerrou normalmente.
Bloodborne chegou ao menu com captura conferida, completou 95 segundos e
2.503 frames, com retorno e shutdown normais. A redução do consumo e a
estabilidade sob pressão no Xbox exigem validação no dispositivo.

## Diagnóstico de retenção de recursos

Sob pressão, `[ResourceMemory]` publica a cada cinco segundos totais vivos,
criados/destruídos e pico em bytes de buffers/imagens VMA rastreados. Separa
staging ativo, staging retirado aguardando fence, streams e demais buffers.
Mostra as 16 maiores alocações com tamanho, nome, handle, tipo de memória e,
para imagens, formato/dimensões/mips/layers/samples. Os heaps VMA são amostrados
junto desses registros; falhas de gravação forçam a captura imediatamente.

`[DescriptorMemory]` informa heaps por tipo/visibilidade, origem da criação,
pools de descritores, capacidade, totais vivos/criados/destruídos e picos.
Também é descarregado na falha de criação de heap. `payload_bytes` e
`metadata_payload_bytes` representam capacidade útil e metadados conhecidos;
não medem o custo físico interno do driver D3D12. Nos pools, `slots` é a
capacidade de sets. Os registros são fixos (8.192 recursos VMA e 32.768 descritores/pools),
com perdas/limpezas não correspondidas explicitadas nos contadores.

As verificações privadas no PC confirmaram retorno dos totais após liberação,
criação/destruição de heap real e relatório automático com `E_OUTOFMEMORY`
injetado. A execução privada com amostragem forçada terminou normalmente;
a causa e retenção sob pressão real no Xbox dependem dos próximos logs.

## Diagnóstico de esperas na compilação

`[PipelineCompile]` identifica início/fim da criação de pipelines gráficos
assíncronos e compute, com hash e thread Windows. `[CompileDiag]` correlaciona
as chamadas no Dozen por identificador/pai, thread, escopo e estágio de shader.
As fases distinguem cache NIR, tradução SPIR-V/NIR, lowering, conversão NIR/DXIL,
validação DXIL, mutex/pressão do controle de concorrência, locks da biblioteca
PSO, leitura/escrita dessa biblioteca e criação do PSO D3D12.

O pager de swap consulta o diagnóstico a cada cinco segundos, independentemente
da thread que compila. Chamadas com pelo menos cinco segundos recebem tempo total,
tempo na fase atual, compilações admitidas e uso/limite/headroom do processo.
O filho mais profundo identifica a fase ativa quando os escopos são aninhados.
A conclusão lenta ou com erro registra resultado e duração. O logger descarrega
esses registros. O relatório usa até 128 entradas fixas e tenta os locks sem
aguardar; não adiciona threads nem cancela compilações. O relatório periódico
depende do pager habilitado; isso corresponde ao padrão do Xbox com swap ligado.

A verificação privada no PC distinguiu `gate_memory` de `nir_to_dxil`, restaurou
o escopo pai e esvaziou o registro após conclusão. Um atraso privado de 12 segundos
produziu o relatório periódico enquanto a compilação estava bloqueada, seguido
de conclusão. O atraso não integra o driver de produção. O timeout/abandono de
30 segundos permanece inalterado para esta investigação.

A captura por LT + RT inclui `worker.dozen.pso` nos pipelines pendentes:
API nativa, hash BLAKE3 do stream, tamanho, subobjetos por tipo D3D12,
hash/tamanho do bytecode de cada estágio e bytes dos estados fixos em hexadecimal.
Os hashes individuais de estruturas com ponteiros são locais à execução;
o hash do stream usa o conteúdo referenciado conforme o cache do Dozen.
O snapshot copia os dados antes da chamada, sem reter ponteiros do descritor,
e indica explicitamente se atingiu seu limite de 8 KiB.

`active_native_psos` e `native_concurrency_at_entry` separam chamadas D3D12
simultâneas das compilações admitidas pelo controle de memória. Cada retorno
nativo produz `[CompileDiag] native_pso_result` com ID, thread, duração e HRESULT;
retornos com erro ou duração de pelo menos 100 ms incluem também o PSO em
`native_pso_return`. O manifesto informa se a chamada ainda está ativa ou já
retornou. A coleta não serializa as chamadas D3D12 nem altera seus limites.

No Xbox e no desktop, a criação nativa de PSO não tem limite adicional de
concorrência por padrão. O limite experimental é opt-in e cobre apenas
`CreatePipelineState`/`CreateComputePipelineState`, depois da leitura do cache
e antes da gravação no cache, sem manter seus locks durante a espera.
`native_pso_slot` identifica espera por vaga; `native_queue_ms` registra essa
espera separadamente de `native_ms`, e `native_pso_limit` identifica o modo.
A tradução NIR/DXIL mantém seu controle de concorrência existente.

Para comparar no Xbox, colocar `dozen-pso-concurrency.txt` em `LocalCache`
do pacote, contendo `1` ou `4`, e reiniciar o aplicativo. O valor `0` ou a remoção do
arquivo restaura o padrão sem limite adicional. `[CompileDiag] native_pso_configuration` confirma
a configuração lida. O export opcional `mesa_uwp_set_native_pso_concurrency`
permite forçar a mesma comparação no host privado de PC; zero significa sem
limite nesse export. Essa comparação investiga contenção, sem afirmar que uma
chamada longa seja um deadlock ou que a renderização no Xbox esteja corrigida.

A captura por LT + RT também salva os PSOs pendentes em
`LocalCache/dozen-pso-captures`, com o diretório em `worker.dozen.pso_capture`.
A reconstrução em outro processo e a seleção da API alternativa no Xbox estão
descritas em [Dozen PSO replay](../tools/DozenPsoReplay/README.md).

O lowering de imagens e texturas raw está desativado por padrão no Dozen.
O export `mesa_uwp_set_raw_image_lowering` permanece disponível para habilitação
explícita antes da criação do dispositivo; o aplicativo não o reativa.

Os bindings de amostragem comprovadamente nativa são anotados antes da criação
do pipeline layout, através do export opcional
`mesa_uwp_set_native_sampled_bindings`. A classificação cobre BC, MSAA, depth
e formatos sem representação raw neste backend: R8, R32, RGBA8, RGB/RGBA32, além de R16/RGBA16 float, UNORM, UINT
e SINT. Exige um único descritor, sem escrita ou feedback. A lista acompanha
os formatos de `dzn_raw_image_format`; expandir essa representação exige revisar
a classificação. Quando habilitado, o lowering raw permanece ativo para os
demais bindings; arrays de descritores não recebem essa promessa.
A classificação participa das especializações, do cache de layouts e dos hashes
NIR/DXIL. A versão de metadata dos shaders foi incrementada para regenerar
entradas que não continham essa informação. O shader continua usando a instrução
nativa de textura, sem gerar o ramo raw que seria inativo para esses recursos.
A verificação privada no PC com o mesmo SPIR-V/SM6.4 criou ambos os pipelines e
reduziu o DXIL de 24.344 para 1.668 bytes. Isso não valida a imagem no Xbox.

## Compilação NIR sob pressão de memória

Falhas de criação/expansão da worklist de instruções invalidam o shader.
A otimização algébrica e a conversão NIR/DXIL encerram o caminho com erro,
sem escrever em ponteiro nulo nem produzir DXIL a partir da fila incompleta.
O estado de falha acompanha clones do shader; o Dozen propaga o erro Vulkan
e registra `[DozenShaderMemory]`.

No UWP fora do desktop, novas criações de pipelines e conversões DXIL aguardam
as compilações ativas quando o headroom do processo fica abaixo de 512 MiB.
O controle admite a próxima compilação quando não há outra ativa, mantém
chamadas internas reentrantes e libera a concorrência quando a pressão cessa.
Não faz espera pela GPU e não altera o orçamento de RAM/swap.

A injeção privada no PC verificou falhas na criação/expansão da fila, rejeição
na conversão DXIL e preservação do erro em clones. O controle de concorrência
foi exercitado com oito threads sob pressão simulada e sem pressão. Isso não
comprova continuidade de gameplay nem o consumo de memória no Xbox.

O Dozen foi reconstruído por completo. No harness PC, o Bloodborne chegou ao
menu (captura conferida), apresentou o primeiro frame em 8.859 ms e completou
95 segundos/2.551 frames com retorno e shutdown sem erro. Runtime atualizado;
nenhum pacote foi instalado.

## Preenchimento de buffers no Dozen

`dzn_CmdFillBuffer` usa um bloco imutável de upload de até 1 MiB e repete
cópias desse bloco até preencher o intervalo solicitado. O staging deixa de
crescer com o tamanho do destino; o alocador continua usando seus recursos de
4 MiB e mantém os blocos vinculados ao command buffer, sem sobrescrevê-los
enquanto houver comandos que os utilizem. A alteração preserva as barreiras,
os offsets de 64 bits, padrões de 32 bits e o arredondamento de `VK_WHOLE_SIZE`.
Falhas de `Map` são propagadas para o erro de gravação do command buffer.

A verificação privada no PC conferiu todos os 160 MiB após fills sobrepostos,
padrões distintos, dois command buffers gravados antes da submissão e reset/
regravação após conclusão da GPU. Passou com barreiras aprimoradas e com
SM 6.4/barreiras legadas. Não há validação em hardware Xbox nesta etapa.

## Diagnóstico de prepareFlip

Quando `PatchFlipRequest` encontra um cabeçalho inesperado, `[FlipDiag]` registra
chamador, endereço/tamanho do buffer, cabeçalho e marcador antes da espera pela
GPU, região de memória e histórico dos últimos 16 patches. A captura inclui os
últimos 128 DWORDs, as últimas 12 fronteiras de pacotes PM4 e candidatos a flip
em um snapshot limitado a 4 MiB. Leituras inacessíveis são registradas; o
capturador não força readback da GPU. O log é descarregado antes da assertiva,
que permanece ativa. Isso permite investigar alteração durante a espera,
reutilização de buffer já remendado e pacote em posição diferente.
A injeção privada de um cabeçalho incorreto no PC confirmou prewait_readable=true, `header_changed=true`, histórico e dump PM4. A falha foi registrada e o shutdown concluiu; a injeção não integra o núcleo de produção.

## Verificação local

Backup inicial com manifesto e SHA-256:
`GR2fork-memory-backups/20261102-092351`, ao lado do checkout.

- Núcleo UWP Release compilado.
- Verificação isolada: 65 casos passaram, incluindo aliases executáveis, swap,
  fallback RAM após Win32 1453, retorno seguro após Win32 1455, transferências
  concorrentes, leases, reciclagem e reinicialização.
- Integração com os objetos de produção: duas sessões consecutivas passaram,
  incluindo desmapeamento parcial, proteção do tracker, cópias desalinhadas e
  preservação de dados privados/proteções de arquivos mapeados.
- Bloodborne chegou ao menu no harness PC: primeiro frame em 8.516 ms,
  2.718 frames em 100 segundos, captura visual conferida e encerramento normal.

- Pac-Man 256 chegou ao menu no harness PC: primeiro frame em 3.844 ms,
  5.610 frames em 100 segundos, captura visual conferida e encerramento normal.

Essas verificações não demonstram pressão real de memória nem gameplay no Xbox.
Não houve instalação de pacote.

## Diagnósticos após a correção dos descritores

O rastreamento detalhado de pools/heaps de descritores foi removido, incluindo
seu registro e lock global. O símbolo exportado de relatório permanece vazio
para compatibilidade. Foram removidos os logs de cada crescimento de arena e
os contadores de pedidos/reutilizações por acesso. O resumo das arenas restantes
passa a cada 30 segundos. A coleta detalhada de memória da thread de gravação
ocorre somente em falha; os erros de alocação e diagnósticos de PSO permanecem.
Essa configuração substitui a periodicidade e o rastreamento descritos acima.

## Recuperação de staging antes de expansão

Quando o pool não encontra espaço reutilizável, executa as liberações diferidas
já elegíveis antes de alocar. Com menos de 640 MiB de margem UWP, procura um
buffer da submissão anterior, aguarda sua fence e o reutiliza. Não força flush
do tick atual. Alocações dedicadas mantidas por consumidores CPU continuam
protegidas; buffers maiores já existentes podem atender pedidos menores sob
pressão. O caminho habitual com espaço disponível não ganha essa coleta extra.
A recuperação reduz expansões evitáveis; não torna reutilizável um recurso
que pertence ao tick atual ou permanece mantido por um consumidor.

A limpeza sob pressão preserva o ring usado mais recentemente de cada tipo
por até 300 frames sem uso e até 16 MiB de alocações dedicadas recentes por
tipo, com expiração de 120 frames. As demais alocações recebem tolerância de
dois frames antes de retirada por pressão. Fences, held e destruição diferida
continuam ativos; a reserva não implica prealocação. Isso evita remover todo
o pool em um frame e precisar recriá-lo no seguinte.

## Diagnóstico de expansão sob pressão

`[StagingPressure]` é emitido antes de uma nova alocação de staging com margem
inferior a 640 MiB. Registra caminho ring/dedicated, pedido/capacidade, tipo,
flags, tick e fence conhecida. Por tipo, separa bytes retidos por CPU, do batch
atual, pendentes de submissões anteriores e já concluídos; informa tamanho
insuficiente e candidatos anteriores, além de até 12 entradas detalhadas.
`ring_reserve_failed_after_wait` identifica reserva recusada mesmo após a espera.
O resumo ResourceMemory acompanha a expansão, limitado a cada cinco segundos,
e o log é descarregado antes da alocação. Não há coleta extra em pedidos que
reutilizam espaço nem mudança de sincronização causada por este diagnóstico.
