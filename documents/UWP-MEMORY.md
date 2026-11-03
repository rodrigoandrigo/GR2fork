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

O arquivo esparso temporário fica em `cache/swap`, com exclusão ao fechar.
Cada registro contém uma página de 16 KiB, comprimida quando útil, com marcador
para página zerada, fallback raw e verificação FNV. O teto padrão é 2 GiB e há
reserva de 384 MiB no dispositivo para saves e outros arquivos. O formato de
slots também limita o arquivo a 2 GiB. O pool de frames mapeados é limitado a
64 MiB e mantém seu compromisso de memória até o encerramento da sessão.

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

Dozen/Vulkan e D3D12 nativo pedem recuperação limitada de páginas guest após
falhas de alocação. Staging concluído pela fence pode ser liberado antecipadamente
sob pressão; imagens consideram também o limite do processo, preservando o
writeback e os retiros por fence existentes. A tradução de módulos de shader
pode abrir espaço antes de compilar. As arenas densas e o descarte diferido do
GR2fork continuam usando seu scheduler; a fila de preparação específica do
runtime de referência não foi introduzida no emulador.

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

Os bindings de amostragem comprovadamente nativa são anotados antes da criação
do pipeline layout, através do export opcional
`mesa_uwp_set_native_sampled_bindings`. A classificação cobre BC, MSAA, depth
e formatos sem representação raw neste backend: R8, R32, RGBA8, RGB/RGBA32, além de R16/RGBA16 float, UNORM, UINT
e SINT. Exige um único descritor, sem escrita ou feedback. A lista acompanha
os formatos de `dzn_raw_image_format`; expandir essa representação exige revisar
a classificação. O lowering raw permanece ativo para os demais bindings; arrays de descritores não recebem essa promessa.
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




