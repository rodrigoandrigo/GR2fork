# Renderer D3D12: estado de implementação

Integração de 24/10/2026: [melhorias do EXODUS](EXODUS-INTEGRATION.md).
O recompilador compartilhado recebeu correções de fluxo, PHI, barreiras de memória,
derivadas coarse e indefinidos de 64 bits. `fast_shader_math` é opcional e permanece
desligado por padrão nos perfis Vulkan e D3D12. A gravação Vulkan em outra thread e
as filas Vulkan de readback pertencem ao caminho Dozen; não são recursos do
renderer D3D12 nativo. As pendências de fidelidade abaixo continuam separadas
dessas melhorias e da validação em Xbox.

Revisão do código local em **21/10/2026**. Este documento separa implementação,
restrições funcionais, desempenho e validação. Desde 23/10/2026, Vulkan/Dozen é
o padrão do host UWP; o renderer D3D12 nativo permanece como opção experimental.
Detalhes de inicialização e encerramento em [UWP-GRAPHICS-LIFECYCLE.md](UWP-GRAPHICS-LIFECYCLE.md).

**O renderer ainda não está completo. O boot de jogos está implementado e
habilitado.** O usuário confirmou anteriormente a tela inicial no Xbox Series S;
isso não comprova gameplay prolongado, precisão gráfica ou todos os recursos.
Não permanece válida a afirmação antiga de que o boot estava desabilitado.

## Escopo da revisão

Foram confrontados os módulos de `src/video_core/renderer_d3d12`, apresentação
`src/uwp/d3d12_presenter.*`, `native_host.*`, API UWP, integração Liverpool/GNM,
memória guest, recompilador compartilhado, tiling CPU e fundação D3D12 em
`externals/d3d12-native/platform/xbox_uwp`. A revisão examinou seleção de caminhos,
condições de rejeição, fallbacks, limites, ownership, submissão e evidências locais.

Os registros de validação existentes em `Build/` foram relidos; não representam
novas execuções de jogos nesta revisão. Uma mensagem de overflow, descritor
inválido ou orçamento esgotado não foi classificada automaticamente como recurso
faltante. As pendências abaixo identificam comportamento válido ainda restrito,
aproximado ou sem comprovação suficiente. Não se afirma equivalência total ao PS4
por uma inspeção estática ou por resultados de diagnósticos. P01–P05 foram implementados
nestas atualizações, com diagnósticos locais separados
da validação de jogos e Xbox.

## Arquitetura atual

O fluxo de jogo é GNM/PM4 → Liverpool → `GpuCommandSink` → `D3D12::Rasterizer`
→ `CommandRuntime` → comandos D3D12. O boot seleciona `InitializeNativeGpu` no
registro GNM quando o backend nativo está ativo. Não existe tradução de chamadas
Vulkan para D3D12 nesse caminho.

O recompilador GCN produz IR/SPIR-V compartilhado com o backend Vulkan. VS/PS/CS
usam SPIRV-Cross → HLSL → DXC; GS e tessellation usam `CompileNativeSpirv`, via
`spirv_to_dxil`, com validação DXIL. Portanto a DLL desse compilador continua
necessária para esses estágios, embora o renderer não dependa de Dozen.

O runtime e o apresentador possuem seus próprios contextos D3D12. O VideoOut
pode receber snapshot imutável da textura residente por recurso compartilhado;
se não houver representação compatível, usa o caminho CPU. O painel de jogo e
o overlay ImGui são superfícies distintas. A swapchain do jogo ignora alpha;
o overlay usa sua composição própria.

## Caminhos implementados

“Implementado” abaixo significa presença do caminho no código, com os limites
explicitados neste documento; não significa cobertura de todos os jogos.

| Subsistema | Implementação atual | Fontes principais |
| --- | --- | --- |
| Comandos | Draw direto, indexed/instanced, base vertex/instance, lotes via ExecuteIndirect com preparação GPU e fallback CPU, dispatch direto/indireto, fill/copy e GDS. | [rasterizer](../src/video_core/renderer_d3d12/d3d12_rasterizer.cpp), [runtime](../src/video_core/renderer_d3d12/command_runtime.cpp) |
| Estágios | VS/PS/CS, GS off-chip, LS/HS/ES/DS, invocações de GS, copy shader, interfaces e root signatures por reflexão. | [shaders](../src/video_core/renderer_d3d12/shader_cache.cpp), [draw](../src/video_core/renderer_d3d12/draw_runtime.cpp) |
| Buffers | Residência GPU com dirty tracking por páginas, readback sob demanda, inputs persistentes, views raw/typed, snapshots, uploads parciais, agrupamento físico, backing segmentado/não linear e segmentos repetidos por tabelas de endereços. | [cache](../src/video_core/renderer_d3d12/resource_cache.cpp), [bindings](../src/video_core/renderer_d3d12/resource_bindings.cpp) |
| Imagens | 1D por lowering, 2D/3D, arrays/cube nas views nativas, mips/layers, formatos normalizados/inteiros/float, packed e BC; tiling CPU/GPU. | [imagens](../src/video_core/renderer_d3d12/image_cache.cpp), [tiling](../src/video_core/renderer_d3d12/tiling_runtime.cpp) |
| Aliases e feedback shader | `IMAGE_LOAD`/`STORE`, gather/query, amostragem planar/volume/cube com cadeia de mips, BC1–BC7 e storage compartilhado para aliases imagem/imagem/buffer, inclusive endereços virtuais distintos e entre estágios. | [lowering](../src/video_core/renderer_d3d12/image_buffer_alias.cpp), [preparação](../src/video_core/renderer_d3d12/shader_cache.cpp) |
| Escritas subword | Texels/componentes de 8/16 bits usam extração e compare-and-swap de 32 bits para preservar os vizinhos nas escritas compartilhadas. | [lowering](../src/video_core/renderer_d3d12/image_buffer_alias.cpp) |
| Atômicos em aliases | Add, min/max signed/unsigned, and/or/xor, exchange, compare-swap inteiro/float, inc/dec e min/max float para componente de 32 bits. Não devem voltar à lista genérica de “atômicos restantes” já resolvidos. | `Atomic` em [image_buffer_alias.cpp](../src/video_core/renderer_d3d12/image_buffer_alias.cpp) |
| Coerência | Leases CPU/GPU, resolução física, callbacks de memória, map/unmap/protect e invalidação; imagens podem continuar residentes/dirty entre comandos e sincronizar por intervalo quando a CPU ou outro uso exige. | [runtime](../src/video_core/renderer_d3d12/command_runtime.cpp), [memória](../src/core/memory.cpp) |
| Metadata e clear | CMASK misto por tile 8×8 para códigos 0/F e estados MSAA com FMASK descomprimido (1→D, 2→E, 3→F), clear tipado, swizzle/quantização e preservação de tiles/padding; modos alternate/Neo e bank swizzle nos layouts suportados. | [clear](../src/video_core/renderer_d3d12/color_clear.h), [CMASK](../src/video_core/renderer_d3d12/cmask_address.h) |
| Rasterização | Culling, winding, máscaras, depth clip/bias, até 16 viewports/scissors, flips X/Y, clip/cull distances, conversões de primitivas, provoking vertex first/last; expansões de point/line e passes por face com restrições. | [rasterizer](../src/video_core/renderer_d3d12/d3d12_rasterizer.cpp), [primitivas](../src/video_core/renderer_d3d12/primitive_conversion.h) |
| Cor/depth/stencil | MRTs com gaps/subviews, blend/dual source, MIN/MAX ZERO/ONE, blend bypass, alpha constante com consulta de capacidade, testes/clears de depth/stencil, depth bounds e gate de cor por depth com restrições. | [draw](../src/video_core/renderer_d3d12/draw_runtime.cpp), [rasterizer](../src/video_core/renderer_d3d12/d3d12_rasterizer.cpp) |
| Logic ops | 16 ROPs; alvos não inteiros são expandidos para componentes inteiros temporários e recompostos, com máscaras, padding e transferência MSAA. | [formatos](../src/video_core/renderer_d3d12/logic_op_format.h), [imagens](../src/video_core/renderer_d3d12/image_cache.cpp) |
| MSAA | Cor e depth/stencil, máscaras, alpha-to-coverage, leitura e resolve; posições guest dependem do tier do dispositivo. | [MSAA](../src/video_core/renderer_d3d12/msaa_transfer.cpp) |
| Stream-out | Captura shader-side de stream 0 com GS off-chip; buffers resolvidos por descritores do user data e filled-size calculado na CPU. É uma implementação parcial. | [draw](../src/video_core/renderer_d3d12/draw_runtime.cpp), [rasterizer](../src/video_core/renderer_d3d12/d3d12_rasterizer.cpp) |
| Memória | Meta agregada de retenção para contextos e apresentação, ajustada por RAM disponível, DXGI e limite/nível UWP; trim seguro por fences, eventos de pressão e segundo plano. | [orçamento](../src/video_core/renderer_d3d12/memory_budget.cpp), [coordenação](../src/video_core/renderer_d3d12/command_scheduler.cpp) |
| Caches | Variantes antes da tradução GCN, DXIL em memória/disco, PSO em memória e blob de driver em disco quando permitido; entradas limitadas. | [shaders](../src/video_core/renderer_d3d12/shader_cache.cpp), [persistência](../src/video_core/renderer_d3d12/persistent_cache.cpp), [PSO](../src/video_core/renderer_d3d12/pipeline_cache.cpp) |
| Transferências | Pool de upload/readback aposentado por fences, heaps persistentes por contexto, staging liberado e cache de pipelines do tiler limitado após fences. | [pool](../src/video_core/renderer_d3d12/transfer_pool.cpp), [tiling](../src/video_core/renderer_d3d12/tiling_runtime.cpp) |
| Apresentação | Snapshot residente por cópia, fallback CPU, gamma/swizzle, blank/last frame, redimensionamento/DPI/centralização e reapresentação durante pausa. | [presenter](../src/uwp/d3d12_presenter.cpp), [conversão](../src/uwp/resident_conversion.h) |
| HDR/float | FP16 linear e BT.2020/PQ → scRGB FP16; negativos/valores acima de 1 preservados quando finitos; HDR anunciado conforme saída e configuração. | [presenter](../src/uwp/d3d12_presenter.cpp) |
| FSR1 | EASU/RCAS espacial opcional SDR, FP32/`cs_6_0`, mesmos filtros para entrada CPU/residente, orçamento limitado; HDR/float e downscale conservam o caminho anterior. | [FSR](../src/video_core/renderer_d3d12/fsr_pass.cpp) |
| Diagnóstico/lifecycle | Tempos CPU e timestamps GPU opcionais, detecção de remoção durante espera, orçamento/DRED no contexto, drenagem na destruição e falha da sessão após erro de GPU. | [contexto](../externals/d3d12-native/platform/xbox_uwp/d3d12_device_context.cpp), [host](../src/uwp/native_host.cpp) |

## Limites atuais e dependências do dispositivo

| Recurso | Limite/condição do código |
| --- | --- |
| Buffers | Por contexto: 128 entradas e allocator de 256 MiB no cache shader, mais cache separado de inputs com os mesmos limites; view/grupo compartilhado até 16 MiB. |
| Imagens | 64 entradas no total assíncrono (16 por contexto); por contexto, allocator GPU de 256 MiB e contabilidade de snapshots CPU de 256 MiB; superfície guest até 64 MiB. Até 16 mips, dimensões até 16384×16384, profundidade/layers até 2048, ainda sujeitos ao formato/dispositivo/orçamento. |
| Tabelas de aliases | Imagens: até 64 MiB de endereços por shader e consumo de dois buffers por imagem; buffers: tabela de até 4 milhões de palavras e mapeamento que preserve a palavra física de 32 bits. |
| Shaders | Por cache/contexto: cache DXIL e cache de variantes limitados a 512 entradas/64 MiB cada, por suas contabilidades. |
| PSOs | Por contexto: 128 entradas e 32 MiB de chaves em memória; isso não mede o tamanho interno de todos os objetos do driver. |
| Disco | 256 MiB por diretório/categoria do cache persistente. Blobs de PSO de driver desativados em `Windows.Xbox`; DXIL e PSO em RAM continuam ativos. |
| Bindings | Quatro heaps persistentes por contexto: 512 recursos, 128 samplers, 8 RTVs e 1 DSV. Limites adicionais de buffers/constantes do recompilador também se aplicam. |
| Transferências | Por contexto: pool de 256 MiB; tiler com allocator temporário de 64 MiB e fallback CPU quando não suportado ou sem orçamento. |
| VideoOut | Contabilidade de frames de 256 MiB; allocator do apresentador de 512 MiB; FSR com allocator separado de 128 MiB e saída até 8192 por eixo, sujeita ao orçamento. A retenção desses pools participa da meta agregada de P04; os caps locais continuam como limites de segurança. |
| Indiretos/índices GPU | Até 4096 draws por lote, janela de argumentos até 16 MiB; índices expandidos até 4 milhões; scratch GPU de 32 MiB por contexto, reutilizado após sua fence. Stride e overflow validados; a view de índices usa o prefixo mapeado de `max_index_size`, incluindo capacidade zero. |
| LDS/GDS | Spill LDS até 16 MiB por dispatch, separado por workgroup; GDS de 64 KiB. |
| Shader Model | Guest requer SM6.4; SM6.6 é usado quando reportado, sem ser requisito universal. Compute de onda variável só pode forçar wave64 com SM6.6; em SM6.4, operações cross-lane exigem dispositivo fixo wave64. |
| Waves gráficas | Operações quad são aceitas; demais operações subgroup/cross-lane exigem `WaveLaneCountMin == WaveLaneCountMax == 64`. Não existe `[WaveSize]` gráfico neste caminho. |
| Formatos/MSAA | Mapeamento em `ConvertFormat` não comprova todos os usos RTV/DSV/SRV/UAV/resolve. Typed UAV load, posições de samples, número de samples e formatos continuam sujeitos às capacidades D3D12. |

As capacidades efetivamente reportadas no Xbox precisam ser confirmadas no
próprio dispositivo. Suporte a SM6.4 não comprova, isoladamente, wave64 fixo,
posições programáveis ou todos os formatos de UAV.

RGB96 tiled é rejeitado deliberadamente; os formatos expandidos usam o contrato
linear documentado na integração anterior. Não é uma tarefa concluída por
inventar um layout tiled. A referência correta para um eventual caso guest
válido deve preceder qualquer ampliação dessa restrição.

## Sincronização e residência: o que realmente mudou

`Draw` e `Dispatch` do rasterizer usam quatro contextos isolados sobre a mesma
fila direta. Cada contexto preserva allocator, lista, descritores, transferências
e lease de memória até sua fence e writeback. Duas listas prontas são submetidas
juntas; fim de packet run/submissão envia a lista restante. A CPU pode prosseguir
enquanto comandos independentes estão em voo.

Hazards usam intervalos e aliases físicos. Acesso CPU aguarda os contextos que
usam o intervalo; map/unmap/protect e sincronizações explícitas drenam os trabalhos
necessários. A reutilização de cada contexto espera sua fence. Dependências entre
contextos ainda podem materializar imagens e aguardar na CPU. GDS, flush, resolve,
clear materializado e captura residente permanecem síncronos.

Buffers guest escritos permanecem na GPU após a fence. Leitura/escrita CPU,
alias incompatível, eviction, remapeamento e flush solicitam writeback conforme
o intervalo. Watches por alias físico protegem páginas limpas contra escrita e
páginas dirty na GPU contra acesso CPU; a lease em voo continua separada dessa
posse persistente.

Um hit limpo de buffer evita releitura e comparação do backing CPU. Escritas CPU
invalidam páginas de 4 KiB e o upload lê/compara apenas os intervalos afetados,
sem copiar por cima de palavras ainda pertencentes à GPU entre esses intervalos.
Views com o mesmo layout físico reutilizam o recurso mesmo com endereços virtuais
distintos. Ampliações ou agrupamentos incompatíveis podem exigir materialização;
não há um heap único de buffers compartilhado por todos os contextos.

Inputs IA têm cache persistente próprio. Índices sem byte swap e atributos
contíguos podem usar cópia GPU de um buffer residente, sem readback intermediário;
inputs limpos são reutilizados. P03 também aplica byte swap na GPU nos draws
elegíveis; inputs não contíguos e conversões incompatíveis mantêm o fallback CPU. Aliases de imagem já representados como buffers
reutilizam o backing residente; a conversão de uma textura nativa para um layout
incompatível ainda pode exigir download/retile. Flush e consumidores CPU reais
continuam materializando dados.

Os caps dos allocators/pools continuam por contexto; P04 coordena a retenção agregada entre eles.
As 64 entradas de imagens foram repartidas em 16 por contexto. O runtime
independente, sem tracking de memória, conserva o contrato de comparação/writeback
síncrono para consumidores que não fornecem invalidações.

P03 prepara os argumentos e a contagem na GPU antes dos draws elegíveis, evitando
que escritas dos próprios draws alterem o lote capturado. `ExecuteIndirect` atualiza
os dois valores de base usados pelos SGPRs e os argumentos D3D12 de cada comando,
incluindo base vertex signed e first instance. A contagem GPU limita o lote ao
máximo PM4; registros inativos têm count/instances zero. Dispatch indireto atualiza
também as três dimensões usadas pelo shader; dimensões fora do limite D3D12
produzem trabalho zero no caminho GPU.

O lote gráfico GPU exige estágios sem escrita storage, sem GS/tessellation,
stream-out, clear ou múltiplos passes. Essas restrições preservam ordenação e
barreiras entre draws; os demais casos mantêm captura prévia e execução CPU.
Dispatch com spill LDS dimensionado pelo número de grupos mantém o fallback CPU.
Janelas de argumentos não mapeadas integralmente ou views físicas não contíguas usam
fallback, sem ampliar artificialmente o intervalo guest válido.

A expansão paralela cobre line loop/strip, triangle fan/strip/list, polygon,
quad list/strip, provoking vertex e byte swap nos casos elegíveis sem restart.
Índices 16/32 bits e sequências não indexadas são aceitos. Restart, contornos,
point fill, conversões antes de GS/tessellation e passes especiais continuam no
caminho CPU.

Isso não equivale a dizer que toda imagem é baixada a cada draw. Com tracking
ativo, a imagem mantém ownership/dirty na GPU e sincroniza sob demanda em acesso
CPU, alias incompatível, invalidação, eviction, alteração de mapeamento ou flush.
`AcquireCpuAccess`, `AcquireMemoryMappingAccess` e as watches de memória já estão
integrados. O snapshot de VideoOut compatível evita download da imagem guest,
embora ainda copie e espere a GPU. Não há scanout direto da textura mutável.

Esses mecanismos não comprovam equivalência de todas as políticas de cache e
barreiras guest. Os casos residuais de coerência estão na lista abaixo; as
fontes de referência estão na seção de evidências.

### Coordenação de memória (P04)

`MemoryBudget` limita a meta agregada de retenção a 2 GiB em primeiro plano e
64 MiB em segundo plano, reduzindo-a conforme `MemoryManager` (uso, limite e nível
do aplicativo), RAM física disponível e orçamentos DXGI local/não local. UMA não
soma nem exige um segundo orçamento não local. Falhas de consulta mantêm as
outras fontes disponíveis; um orçamento DXGI válido igual a zero é respeitado.
Há reserva de 64–256 MiB para o processo e margens próprias para RAM/dispositivo.
O excesso acima do limite também entra no déficit a recuperar.

A amostragem ocorre a cada 250 ms; os eventos UWP de aumento/redução de uso e
mudança de limite antecipam a próxima amostra. Crescimento da retenção acima da
meta antecipa a coleta, e a recuperação do orçamento cresce no máximo 16 MiB por
amostra. O host informa perda/retorno de atividade e continua atendendo pressão
no loop de segundo plano; o apresentador libera temporários FSR e mantém o frame
necessário à retomada. Os callbacks de eventos usam sinais sem acesso direto à
GPU e são revogados na destruição.

A soma inclui contextos coordenador/workers, buffers/inputs, imagens e planos
MSAA, snapshots CPU, pools de transferências, temporários de índices/tiling,
shaders/variantes e apresentação/FSR/frames/swapchain. Tamanhos de recursos usam
allocation info; PSOs opacos usam estimativa de 64 KiB por entrada e frames usam
sua reserva conservadora. Overhead interno do driver/DXC e descritores também
entra no uso real, sem estimativa exata no contador. Não é uma medição exata de
residência física nem soma
independente de RAM e VRAM em UMA; o uso real do processo/DXGI governa a pressão.

A coleta ocorre em fronteiras seguras: submete/drena os contextos e espera suas
fences antes de liberar recursos. Transferências concluídas, caches reconstruíveis
e entradas limpas são recolhidos antes de materializar escritas GPU necessárias
à remoção. Dirty não sincronizável e frames ainda referenciados permanecem vivos.
A meta é de retenção, não um limite rígido que invalida o working set de um único
comando; os caps de alocação anteriores continuam ativos. O log `D3D12 memory`
mostra uso, meta, bytes recolhidos e excesso ainda referenciado (`pinned`). Isso
não garante que todo jogo caiba no limite efetivamente concedido pelo Xbox.

### Tiling e transferências (P05)

O tiler processa cada subrecurso com mip, camada global/base da view, amostra e
profundidade próprios. Arrays parciais e volumes incluem os mips menores; planos
MSAA passam pelo import/export existente de amostras. BC1–BC7 usam coordenadas de
blocos com arredondamento das dimensões, sem decodificar/recomprimir os blocos.
As fórmulas de endereçamento e os contratos guest anteriores são preservados.

Cada dispatch cobre somente a região visível, preservando padding, camadas e
canais fora da escrita. Dispatch 2D cobre mais de 65535 grupos. Constantes root
substituem o upload de tabelas/CBVs; leitura direta do upload como SRV elimina o
buffer default e a cópia intermediária da entrada. Não há mais buffer/cópia de
zeros para texels de 8/16 bits. Os buffers de saída/entrada usam o tamanho efetivo
do footprint ou do backing guest, em vez do maior dos dois para ambos.

As trocas 4:4:4:4 e o unpack/pack raw de logic ops são feitos no mesmo shader,
respeitando máscaras inclusive parciais/vazias. O download escolhe tiling GPU
antes de alocar/copiar para o readback linear, evitando a transferência duplicada.
No caminho persistente, o dirty tracking permite ler diretamente no upload
mapeado e escrever do readback mapeado para o guest, sem snapshot CPU intermediário.
O runtime sem tracking mantém comparação/snapshot para seu contrato síncrono.
Imagens lineares no fallback também dispensam a cópia integral para `linear`.

O limite de scratch GPU continua 64 MiB por contexto e os pools continuam
vinculados a fences. Até 32768 subrecursos podem ser descritos, ainda sujeitos aos
limites de imagem, footprint e dispositivo. Depth/stencil com planos especiais,
promoção D16/S8, formatos/layouts incompatíveis e falta de scratch conservam os
caminhos CPU anteriores. A representação nativa expandida de logic ops continua
com 16 bytes por texel; P05 reduz sua formatação/cópias, sem alterar essa precisão.
Não há suporte novo para renderizar/comprimir BC por shader nem para RGB96 tiled.

O limite guest `max_index_size` delimita a view de índices sem reduzir a contagem
do draw. Leituras além da capacidade ou do prefixo com backing mapeado retornam
índice zero, também em indiretos, conversões CPU/GPU e fetch programável. A última
unidade de byte swap só é lida até o backing disponível; seu padding não amplia
a capacidade lógica. Buffer nulo mantém o draw indexado. Nos indiretos, o primeiro
índice limita a capacidade restante também no fallback CPU. O contrato dos pacotes
está descrito no [guia AMD](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/programmer-references/si_programming_guide_v2.pdf).
A validação PC está em `Build/f03-index-validation.log`.

O storage compartilhado (F07) inclui gather por componente/PCF com offset,
LOD/bias/clamp e mips, query LOD limitado/sem clamp com sampler rastreado e cube
por face/array. [BC6/BC7](../src/video_core/renderer_d3d12/bptc.h) decodifica todos
os modos na GPU, incluindo BC6 signed/unsigned e modos reservados; escritas de
blocos por buffer alias são observadas no mesmo shader. O caminho raw também
lê/escreve 565/1555/4444 UNORM/SNORM/UINT/SINT e lê RGB9E5, pares GB_GR/BG_RG,
8_24/24_8 UNORM/SNORM/UINT/SINT e X24_8_32 FLOAT/UINT. Componentes inteiros em
point/gather preservam os bits sem interpolação float. Combinações fora dessas
representações continuam rejeitadas explicitamente.

A [ISA GCN, tabela 8.17](https://gpuopen.com/wp-content/uploads/2016/08/AMD_GCN3_Instruction_Set_Architecture_rev1.1.pdf)
não permite `IMAGE_STORE` tipado em BC, RGB9E5, pares ou depth packed. A escrita
válida sobre esses backings usa views raw/buffer, preservando os bytes; não há
recompressão automática de texels. Precisão e controles secundários de amostragem
continuam em F06 e metadata comprimida de depth em F04.

O sampler manual (F08) cobre os oito modos de endereçamento, incluindo
`ClampHalfBorder`, `MirrorOnceHalfBorder` e `MirrorOnceBorder`. Mirror-once
reflete uma vez e mantém clamp/borda; point respeita a direção nos limites exatos
entre texels. Os modos sem equivalente nativo, custom border e amostragem inteira
usam lowering raw mesmo sem aliases, preservando SM 6.4. Custom border mantém os
quatro words: FLOAT sem quantização, UINT/SINT como bits inteiros e UNORM/SNORM
com clamp antes da filtragem. O caminho point evita operações float desnecessárias,
preservando inclusive zero negativo, infinito, NaN e subnormal em FLOAT32.

GS on-chip (F09) usa o contrato lógico de rings coerentes convertido em atributos
ES→GS→copy, sem exigir backing físico dos rings guest. Os modos de backing
válidos deixam de bloquear o draw; o valor reservado 2 continua rejeitado.
Point fill e linhas largas/stipple com GS e tessellation capturam a saída do
último estágio na GPU e aplicam a expansão em um draw indireto. O contador de
[stream output D3D12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/stream-output-counters)
interno fornece a contagem real da expansão, incluindo strips, cuts e emissão vazia; o guest GS/HS/DS
executa uma vez, com barreira UAV antes do PS. As interfaces vêm da reflexão
DXIL, os helpers usam cache em RAM/disco e atributos flat conservam o primeiro
vértice da primitiva. O quad de linha usa ordem de strip sem sobreposição interna;
stipple é dividido em comandos para respeitar o teto de componentes por
invocação, mantendo a janela de até 4096 bits do helper.

Entrada `RectList` com guest GS não aciona a expansão de retângulo: a topologia
é a emitida pelo GS. Os buffers transitórios pertencem ao contexto em voo e entram
no orçamento e trim por fence; PSOs e root signatures ficam retidos por lease
até a fence, inclusive após eviction do cache. A reserva conservadora da captura tem teto de 32 MiB por contexto;
se a amplificação máxima ultrapassar esse limite, o draw é rejeitado antes da
gravação. Stream-out guest completo continua em F10 e fidelidade das expansões,
provoking-last e cobertura/interpolação continuam em F15. [Homebrew OpenOrbis](hardware/geometry-expansion/README.md).

Modos polygon diferentes por face (F12) usam uma captura dos estágios guest,
seguida de comandos indiretos na ordem primitiva→face→batch, com barreiras UAV.
Isso conserva a execução única de VS/HS/DS/GS e evita agrupar todas as faces de
um lado antes das demais. Um GS de passagem conserva o reaproveitamento de
vértices no caminho sem guest GS. Máscaras de arestas/vértices removem diagonais
e pontos duplicados de quad/polygon/rect; os helpers preservam PrimitiveID e
IsFrontFace. Cada face mantém rasterizer, depth/stencil e referência próprios.
A contagem real exige readback de quatro bytes e espera de fence antes do
replay (`face_capture_wait`); portanto esse fallback ainda introduz um stall.
Os PSOs e root signatures de todas as faces ficam retidos até a fence.
[Homebrew OpenOrbis](hardware/polygon-faces/README.md).

O gate de cor por depth (F13) reutiliza essa captura e separa os resultados de
stencil/depth antes de aplicar as alterações reais do depth/stencil. Os passes
auxiliares conservam stencil e não escrevem depth; somente o passe base aplica
os efeitos do output merger. Storage/atômicos do PS usam partições disjuntas em
sample único, incluindo stencil-fail quando o contrato é late, sem repetir os
efeitos do VS. As máscaras de cor por passe entram no rastreamento de escrita.
Depth bounds usa o teste nativo do [depth armazenado](https://microsoft.github.io/DirectX-Specs/d3d/DepthBoundsTest.html),
com ou sem PS. Intervalos invertidos usam rejeição por depth sem emitir bounds
inválidos à API, preservando efeitos UAV late e suprimindo-os no contrato early.
[Homebrew OpenOrbis](hardware/depth-gate/README.md).

Stencil guest sem equivalente direto (F14) usa os testes nativos sobre uma
cópia GPU readonly do depth/stencil anterior à primitiva. Um PS auxiliar aplica
a operação de oito bits no buffer de stencil com CAS, preservando bytes vizinhos
e bits fora da write mask; a cópia de volta altera somente o plano stencil.
O shader guest executa uma vez, o depth real é preservado e as atualizações são
intercaladas por primitiva/face, inclusive no gate do F13. `test_val` e `op_val`
permanecem independentes. Os helpers mantêm os RTs com máscara de escrita zero
para conservar a extensão de rasterização original. Snapshot/buffer pertencem ao
contexto em voo, entram no orçamento/trim de `GpuCommands` e têm limite conjunto
de 32 MiB; seus PSOs/root signatures participam do lease da submissão.
[Homebrew OpenOrbis](hardware/stencil-ops/README.md).

Stream-out guest conta as emissões executadas na GPU e lê o resultado após a fence,
incluindo emissão vazia, `EMIT_CUT` e emissão em `GS_DONE`. Buffers esparsos usam seus
próprios strides; filled-size usa dwords, limita vértices completos e conserva o cursor
entre draws. Flush/sync/reset aguardam os comandos anteriores. `STRMOUT_BUFFER_UPDATE`
separa restauração de offset e salvamento de filled-size, incluindo origem em memória,
unidades em bytes e swaps. [Fonte OpenOrbis](hardware/stream-out/README.md).

Tessellation Pow2 usa o processamento Integer do hardware, preservando o
pré-processamento dos fatores feito pelo shader GCN; nenhum arredondamento extra
é aplicado às saídas ou aos buffers guest. O caminho nativo aceita point nos três
domínios, line em isolines e triangle CW/CCW em tri/quad. Topologias reservadas e
domínios incompatíveis entre HS/DS são rejeitados antes de executar os shaders.
[Fonte OpenOrbis](hardware/tessellation/README.md) e
[contrato de Pow2](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#11.7.7.1%20Pow2%20Partitioning).

## Trabalho pendente para jogos

### Correção funcional e fidelidade

A lista abaixo é a fila atual de lacunas encontradas no código. A numeração
serve para acompanhamento; não é ordem de facilidade nem garantia de que todo
jogo depende de cada item.

| ID | O que falta para cobertura completa | Evidência no código |
| --- | --- | --- |
| F01 | Feedback de attachments com escrita simultânea, preservando ordem e efeitos laterais sem replay incorreto. | `ValidateTargetRange` em [resource_bindings.cpp](../src/video_core/renderer_d3d12/resource_bindings.cpp) permite leituras simultâneas de attachments imutáveis: color com máscara zero e planos depth/stencil sem escrita/clear, inclusive aliases físicos. Usa o conteúdo sincronizado antes de um único draw; rejeita sobreposição quando shader ou attachment escreve. Escrita ordenada permanece pendente. |
| F02 | Completar fetch coerente de índices com conversão de primitivas/restart e descritores de fetch modificados durante o draw. | Fetch shaders são traduzidos inline no VS e usam storage compartilhado com buffers/imagens, sem snapshot de dados da IA; aliases físicos, formatos, offsets e divisores seguem o tradutor GCN. Índices 16/32 bits com escrita no mesmo backing usam leitura programável no VS, com swaps, base e instâncias; indiretos com escrita passam pelo fallback de argumentos. Conversão/restart nesse feedback continua rejeitado. Código e descritores ainda são resolvidos na tradução CPU. |
| F04 | CMASK além de 0/F, metadata comprimida/TC-compatible, DCC e HTILE com semântica real; FMASK com remapeamento/compressão guest em vez de valores constantes. | `MaterializeColorClearImpl` aceita 0/F sem compressão e separa clear/cor expandida de FMASK descomprimido em MSAA 2×/4×/8×, preservando seus bits; ainda rejeita EQAA, FMASK comprimido, DCC e TC-compatible quando requerem materialização. HTILE permanece sem decodificação; [resource_patching_pass.cpp](../src/shader_recompiler/ir/passes/resource_patching_pass.cpp) substitui leituras FMASK por constantes e não aceita storage FMASK. |
| F05 | EQAA verdadeiro, em que samples e fragments diferem, e equivalência da frequência parcial de shading por sample. | `MAX_ANCHOR_SAMPLES` corrigido para três bits; `PS_ITER_SAMPLE` respeitado e contagem de iterações preservada. [draw_runtime.cpp](../src/video_core/renderer_d3d12/draw_runtime.cpp) rejeita EQAA e frequência parcial antes de traduzir/executar o draw, evitando multiplicar efeitos laterais. Falta o fallback de cobertura→fragment/anchor e shading agrupado com interpolação, depth/stencil, blend e FMASK coerentes. |
| F06 | Completar amostragem de storage compartilhado: LOD implícito com cadeia de mips, filtragem entre mips, anisotropia, seleção min/mag/LOD/reduction fiel e cálculo geral de gradientes. | [Sample](../src/video_core/renderer_d3d12/image_buffer_alias.cpp) calcula derivadas implícitas no PS e gradientes 1D/2D/3D, seleciona mip point/linear, min/mag, bias/clamps e reduction; anisotropia por footprint até 16 taps e borda float tipada. Samplers entram nas dependências do cache de variantes. Ainda faltam equivalência GCN de precisão/controles secundários (aniso threshold/bias, bias secundário, trunc/ceil e seleção coarse por quad); o filtro anisotrópico 3D difere do volume nativo usado como referência no PC. [Homebrew](hardware/shared-sampling/README.md). |
| F10 | Completar stream-out sem GS, múltiplos streams/rast stream, layout/seleção exata dos descritores e compactação ordenada da emissão entre invocações. | A contagem já vem da GPU, com filled-size e eventos sincronizados; a captura de dados ainda cobre stream 0, escolhe descritores por heurística e usa índice de escrita local por invocação. |
| F11 | Esclarecer a semântica GCN dos pares de domínio/topologia fora do contrato nativo, se forem usados pelo guest. | Pow2 e os oito pares nativos estão cobertos. A especificação D3D restringe line a isolines e triangle a tri/quad. `DumpAllPointsAsInOrderLineList` no tessellator Microsoft/Mesa é identificado como visualização de depuração; o caminho isoline não define geração triangular. Isso não comprova o comportamento GCN desses pares. Sem um contrato guest demonstrado, continuam rejeitados antes da consulta aos caches DXIL, sem inventar conectividade. |
| F12 | Bias por face em point fill e linhas expandidas, calculado sobre a primitiva original. | Modos mistos com GS/tessellation e quad/polygon/rect usam captura única e replay por primitiva/face; fill e wireframe nativo preservam bias independente. Pontos, linhas largas/stipple e contornos expandidos ainda rejeitam bias não zero quando depth está ativo. O gate por depth aceita replay por face em fill/wire nativo; expansões continuam restritas em F13. |
| F13 | Completar gate com PS que escreve depth/coverage/stencil ref ou usa discard, MSAA/alpha-to-coverage, gate simultâneo a bounds/storage, feedback de depth, rasterização expandida, captura de adjacency sem GS e logic ops MRT com máscaras densas incompatíveis. | Gate com storage/atômicos em sample único usa captura dos estágios anteriores ao PS e replay por primitiva, separando stencil-fail, depth-fail e pass sem repetir escritas. Depth/stencil evoluem entre primitivas; fill/wire nativo por face compartilha essa captura. Depth bounds usa o depth armazenado e funciona sem PS; exige suporte nativo `DepthBoundsTestSupported`. As combinações restantes continuam rejeitadas explicitamente. |
| F14 | Integrar a atualização auxiliar de stencil com MSAA, alpha-to-coverage, discard/exportações do PS, rasterização expandida, adjacency sem GS e múltiplas camadas nativas. | AND/OR/XOR/NAND/NOR/XNOR, Ones e ReplaceTest/ReplaceOp independentes usam snapshot GPU do depth/stencil anterior à primitiva, testes nativos readonly e atualização atômica dos bytes pela write mask. Add/Sub clamp/wrap usam `op_val`, com caminho nativo quando ele vale 1. Refs/read/write masks diferentes por face usam captura única e replay ordenado; backface desabilitado herda corretamente a configuração frontal. O fallback cobre sample único e uma camada nativa; rejeita as combinações restantes antes de executar shaders guest. |
| F15 | Parcial: equivalência guest de provoking-last em tessellation e limites de interface; cobertura/interpolação completa de linhas/pontos, inclusive recorte de polígonos e degenerados. | Captura única com ordinal de emissão do GS, compactação adjacency CPU/GPU, seleção flat pós-captura e transformação pelo viewport selecionado implementadas. Linhas usam clipping homogêneo e interpolação smooth/noperspective ao subdividir. Limites abaixo. |
| F16 | Parcial: reset/estado/acumulacao entre segmentos e packets; metricas major/full-length, fractional/diamond; cobertura guest completa e W degenerado. | REPEAT_COUNT agora repete cada bit (valor + 1), o padrao volta ciclicamente e o recorte preserva a fase para endpoint inicial com W positivo. Batches dimensionados pelo viewport/escala substituem o corte silencioso em 4.096 janelas; limites de memoria/endereco geram erro explicito. A continuidade dos registradores SU/SC ainda nao esta implementada. |
| F17 | Parcial: ampliar o fallback de constantes RGB/alpha para MRT e slots diferentes de zero, e combinações com dual-source guest. | Um RT no slot 0 usa saída secundária do PS para alpha constante, mantendo as constantes RGB no output merger e o alpha original do fragmento. MIN/MAX RGB ficam nativos porque ignoram fatores. Configurações incompatíveis continuam rejeitadas explicitamente. |
| F18 | Parcial: FP64 por software e demais operações/modos; arredondamentos direcionados, denorm híbrido/FP16/FP64 e semântica completa de NaN; cross-lane sem wave64 garantido. | O perfil nativo preserva FP64 no IR, consulta DoublePrecisionFloatShaderOps e rejeita a ausência de suporte em vez de reduzir para FP32. Bitcasts uint2/double usam asdouble/asuint no HLSL. FP32 flush/preserve seguem ExecutionMode e opções -denorm do DXC, também nos helpers derivados. As guardas de wave64 continuam ativas. |
| F19 | Parcial: demais opcodes GCN, DPP, SDWA em caminhos de 16/64 bits, append/consume com semântica geral por wave e validação dos demais estágios até DXIL/SM6.4. | SDWA de 32 bits agora aplica seletores, extensão, modificadores e preservação do destino. ORN2/NOR/XNOR B32, CMOV B32 e SEXT I8/I16 implementados, incluindo SCC. DPP sem lowering validado é rejeitado explicitamente. Emissores unreachable isolados não medem a cobertura. |
| F20 | Parcial: demais comandos PM4 válidos, estatísticas/controles guest e fidelidade das consultas nas expansões. | Oclusão D3D12 mede samples aprovados por depth/stencil e resolve resultados após fence, também em contextos assíncronos. SET_PREDICATION aplica ZPASS/PRIMCOUNT/BOOL32/64, polaridade, continuação e espera/no-wait aos pacotes marcados. COPY_DATA gráfico implementa memória/GDS/registradores/imediato/clock, 32/64 bits e confirmação. Restam GET_LOD_STATS, seletores atômicos de COPY_DATA, geração de estatísticas PRIMCOUNT/pipeline, distribuição/controle por RB e SGPR de draw index. |
| F21 | Parcial: combinações que excedem o lowering raw, orçamento de tabelas ou dependem das semânticas pendentes de F01/F04/F05/F06/F07. | Storage MSAA escrito/atômico/feedback e views MSAA de um sample usam storage compartilhado. Aliases escritos com a mesma base também são lowerados, independentemente do formato/layout. A seleção consulta suporte real de dimensão, load/sample/gather/comparison, UAV typed load/store e sample count; views sem suporte nativo usam o lowering existente. Permanecem as guardas para operações sem representação, formatos/layouts não implementados, metadata comprimida e feedback de attachments com escrita. |
| F22 | Parcial: continuidade após perda de trabalho já submetido ou de estado GPU sem cópia CPU, e recuperação conjunta do presenter/swapchain. | Modo opcional SHADPS4_D3D12_RECOVERY=1 faz checkpoints sincronizados, desabilita batching assíncrono e permite uma recuperação por sessão na thread PM4. Recria o runtime e restaura GDS, oclusão, filled-size e registradores, recarregando recursos da memória guest. Recusa replay após submissão, escrita CPU, operação já concluída ou checkpoint inválido; erros determinísticos continuam encerrando a sessão. |

Os atômicos 32-bit listados em “Caminhos implementados”, aliases virtuais entre
estágios, segmentos físicos repetidos, CMASK 0/F e MSAA descomprimido 2×/4×/8× por tile e dirty tracking de
imagens **não estão pendentes como recursos inteiros**. F01/F02/F06/F21
identificam os casos residuais concretos. Tampouco se exige coerência definida
para data races que o próprio contrato guest não define: falta confrontar as
políticas de sincronização/cache em usos válidos com hardware.

F22 oferece recuperação conservadora, desligada por padrão. Com
`SHADPS4_D3D12_RECOVERY=1`, o renderer sincroniza os recursos com a memória guest
antes/depois das operações da thread PM4 e guarda GDS, oclusão e filled-size.
Perda do dispositivo, HRESULT `E_OUTOFMEMORY` ou `std::bad_alloc` antes de nova submissão
pode recriar dispositivo/fila, caches, allocators, listas, descritores e pipelines;
restaura os registradores gráficos/compute da chamada e tenta a operação uma vez.
Os recursos de imagem/buffer são reconstruídos sob demanda a partir da memória
guest sincronizada; os callbacks de backing, residência e orçamento são religados.
Leases CPU/mapping mantêm o runtime vivo até sua liberação. Checkpoints possuem
identidade de runtime e não são utilizáveis durante mudanças de mapeamento.

A troca não ocorre se a operação já terminou, escreveu memória guest pela CPU,
submeteu qualquer lista desde o checkpoint ou se a tentativa da sessão já foi
usada. Esses casos encerram a sessão com diagnóstico. O descarte de um runtime
falho não submete listas pendentes nem confirma readbacks de trabalho abandonado.
A recuperação automática é restrita à thread PM4 para não restaurar registradores
enquanto o parser os modifica. Outros acessos preservam o tratamento de falha.
Isso tem custo de sincronização/readback e não equivale a savestate de CPU, RAM,
filas e efeitos externos. Recuperação geral em voo e recriação conjunta da
apresentação após TDR real permanecem pendentes. O runtime antigo é liberado
antes da recriação; outros componentes que ainda retenham o device removido,
incluindo presenter/frames, podem impedir a criação do substituto e levam ao
encerramento controlado. Isso decorre do
[device por adapter de D3D12CreateDevice](https://microsoft.github.io/DirectX-Specs/d3d/IndependentDevices.html).
[Remoção programática do dispositivo](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice).

F21 encaminha storage MSAA para buffers coerentes por texel/sample/camada,
preservando os bytes vizinhos e o padding, inclusive em formatos de 8/16 bits.
A seleção de aliases não exige mais bases virtuais diferentes: views RGBA8 e
R32_UINT da mesma base compartilham as escritas. Suporte nativo é consultado no
dispositivo por uso; ausência de mapeamento DXGI ou de capacidade da view aciona
o lowering raw já implementado, sem remover as validações de operação/formato.
O backend continua em SM6.4. [Capacidades typed UAV](https://learn.microsoft.com/en-us/windows/win32/direct3d12/typed-unordered-access-view-loads).

Isso não fecha toda a F21: o caminho raw ainda limita tabelas a imagens de 16 MiB
e ao orçamento de descritores, rejeita RGB96 tiled e operações/formatos sem
packing/lowering, e não resolve as dependências de metadata ou feedback de
attachments com escrita. Os testes de uso, layout e ordem permanecem necessários
para cada combinação; a presença em `ConvertFormat` não prova suporte completo.
[OpenOrbis: views com a mesma base](hardware/image-views/main.cpp), validado
localmente apenas por sintaxe.

F20 usa consultas `D3D12_QUERY_TYPE_OCCLUSION` no raster final; captura de
geometria, helpers de stencil e passes auxiliares de depth-fail não entram no
contador. `PixelPipeStatDump`/`ZpassDone` publicam snapshots válidos, com a soma
no primeiro par e zero nos demais; `PixelPipeStatReset`/`RstPixCnt` reiniciam a
soma. A leitura aguarda fences e mantém os readbacks no pool de transferências.
O contrato de distribuição física e dos controles de contagem por RB ainda não
está reproduzido. Outros backends mantêm o fallback anterior de contador sintético.

A predicação é resolvida na CPU após sincronização, com espera cooperativa quando
faltam resultados válidos; o hint no-wait de ZPASS permite executar nesse caso.
ZPASS acumula visibilidade, enquanto PRIMCOUNT acumula ausência de overflow.
Os registradores consumidos pelo parser rápido também respeitam a predicação.
`ACQUIRE_MEM` e `PFP_SYNC_ME` sincronizam o renderer; `COND_EXEC` sincroniza antes
da leitura. A solução prioriza coerência e ainda pode introduzir stalls.
`COPY_DATA` rejeita seletores sem implementação e ranges GDS inválidos, em vez de
prosseguir com dados inventados. O clock usa o contador emulado já existente.
[OpenOrbis: oclusão/predicação](hardware/occlusion-predication/main.cpp), com
validação local sintática. Codificação consultada no
[guia de programação SI da AMD](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/programmer-references/si_programming_guide_v2.pdf)
e nos emissores PM4 de Mesa presentes no repositório.

A auditoria de F19 distingue os placeholders de SSA/registros, eliminados por passes,
dos caminhos sem tradução. Append/consume já são convertidos em atômicos por
`PatchGlobalDataShareAccess`, com reconhecimento e remoção de padrões de prefixo
em `RemoveAppendBufferLaneOffset`; isso não implementa o contrato geral de retorno
uniforme e atualização por contagem de lanes da wave. Permanecem, entre outros,
`S_BREV_B64`, variantes FP64 e instruções GWS/DS. Contar cases do frontend também
não é um oráculo: comparações e outras famílias usam dispatch compartilhado.
Fonte de verificação: [OpenOrbis SDWA/escalares](hardware/ir-gcn/main.cpp), com
validação local somente sintática.

O avanço de F15 preserva os estágios guest por captura única. O ordinal ocupa
`TEXCOORD31` no GS nativo com flat-last; 32 atributos guest exigem outro layout e
permanecem rejeitados. A reflexão conta componentes efetivos, incluindo semânticas
empacotadas de viewport/primitive ID. A transformação por viewport preserva o
contrato anterior de clip-disable; W não unitário ainda requer validação.
A ordem de tessellation, cobertura exata de bordas/caps/diamond, polígonos recortados
em point fill, tamanhos de ponto e todos os casos degenerados continuam pendentes.
F16 preserva a fase do trecho recortado quando o endpoint inicial tem W positivo;
continuidade entre primitivas/packets e os controles SU/SC ainda permanecem pendentes.
A fonte [OpenOrbis de provoking-last](hardware/flat-viewport/main.cpp) compara
rotação dos vértices e contador do GS; sua validação local é somente sintática.

### Desempenho, memória e escalabilidade

Estas tarefas não devem ser confundidas com instruções/estados guest ausentes.
ExecuteIndirect, copy queue separada e reconstrução temporal não são requisitos
isolados para correção de um renderer, embora algumas otimizações sejam
necessárias para desempenho aceitável nos jogos.

| ID | Trabalho restante | Situação atual |
| --- | --- | --- |
| P01 | Implementado: listas agrupadas, quatro contextos em voo, hazards por alias físico e leases seguros. | Reciclagem por fence; dependências compartilhadas podem esperar na CPU. Diagnósticos PC passaram; desempenho/gameplay Xbox ainda requerem validação. |
| P02 | Implementado: buffers e inputs persistentes, dirty tracking por página/intervalo e readback sob demanda. | Hits limpos evitam releitura/comparação CPU; input contíguo pode receber cópia GPU. Layouts incompatíveis, byte swap e sincronizações explícitas mantêm fallbacks. |
| P03 | Implementado nos casos elegíveis: preparação GPU, ExecuteIndirect para draw/dispatch, expansão paralela de índices e byte swap. | Contagem e constantes dos SGPRs preservadas; restart, escrituras storage entre draws e passes especiais mantêm fallback CPU. |
| P04 | Implementado: meta agregada adaptativa, pressão real do processo/dispositivo e redução em segundo plano UWP. | Recolhe caches após fences e preserva dirty por writeback; working sets e frames referenciados podem exceder a meta e são reportados. Pressão e suspensão reais no Xbox ainda exigem validação. |
| P05 | Implementado: tiling por subrecurso para arrays/views parciais, volumes, planos MSAA e BC; conversão 4:4:4:4 e empacotamento de logic ops na GPU. | Upload usa SRV sobre staging e constantes root; elimina cópia/readback duplicados e snapshots CPU intermediários no caminho persistente. Planos depth/stencil especiais e casos sem orçamento mantêm CPU; logic ops ainda usam até 16 bytes nativos por texel/amostra. |
| P06 | Profiling e otimizações implementados; ampliar cobertura de jogos/cenas e Series S. | Agregação CPU por etapa/lock/fence, hits de caches, bytes de apresentação e timestamps GPU; leitura curta de metadata e cópia residente direta quando a conversão é identidade. PAC-MAN e startup de Teddy medidos no PC; consultas CPU sem backing e reentrada durante mapeamento foram corrigidas. Decisões de novas filas/precompilação dependem de mais sessões; blobs de driver no Xbox continuam desativados. |

### Profiling e decisões de throughput (P06)

`GAME_PROFILE=1`, definido antes de carregar o core, habilita `NATIVE_PROFILE` em stderr.
Cada nome registra contagem, tempo inclusivo total/máximo, limites superiores dos percentis
p50/p95/p99 em buckets logarítmicos, amostras de pelo menos 5 ms e bytes. Os relatórios são
cumulativos, respeitam o intervalo por nome e emitem o restante no teardown normal; não
somar etapas aninhadas nem somar relatórios sucessivos. `present_interval` mede o intervalo
CPU entre apresentações, não o tempo GPU de um frame. `NATIVE_GPU` informa timestamps das
listas por contexto, inclusive o resumo final abaixo de 128 submissões. Profiling desabilitado
não consulta relógio nem acumula histogramas; não há marcador PIX no sink PM4.

As etapas distinguem tradução GCN, misses SPIR-V, DXC, criação de PSO, disco, aquisição dos
locks do runtime/scheduler/presenter, fences e preparação/conversão/apresentação. Os contadores
separam hits RAM/variantes/blob e bytes residentes, CPU, convertidos e copiados diretamente.
`SPARSE_SLOW` preserva o tamanho solicitado, em vez de imprimir zero após consumir o intervalo.
A coleta por variável de ambiente/stderr foi exercitada no harness desktop; não equivale a
uma sessão instrumentada no AppContainer ou no Xbox.

No PC com RX 6600M, PAC-MAN 256 foi medido em janelas de 25 s desde o boot. A amostra anterior
registrou 142 leituras de programa, 8.908.614 us totais (62,7 ms por leitura), máximo de
401.151 us e 18 apresentações. No binário final empacotado, com a leitura inicial reduzida
de 64 KiB para 8 bytes quando o prefixo aponta para a metadata, foram 142 leituras,
2.397.029 us (16,9 ms por leitura), máximo de 26.301 us e também 18 apresentações. A varredura
completa continua como fallback; código e metadata continuam relidos, sem cache que presuma
imutabilidade guest. Uma amostra intermediária chegou a 25 apresentações, mas as repetições
não demonstraram ganho consistente de throughput. O frame capturado ainda é o logo de startup;
esses dados não comprovam FPS, correção visual ou gameplay prolongado. Caches do driver/SO
e duração do boot não foram controlados como em um benchmark estatístico.

No binário final com cache frio de aplicação, tradução GCN consumiu 28,7 ms, DXC 221,5 ms,
criação de PSOs 1,8 ms e espera do presenter 10,6 ms em 19 apresentações incluindo a inicial
blank. Com cache quente, houve seis hits SPIR-V em disco, cinco hits de blob de PSO e nenhuma
chamada DXC; o caminho completo dos cinco misses de PSO caiu de 12,1 ms para 3,6 ms. Essa sessão
teve 15 apresentações de jogo, reforçando que eliminar compilação não resolveu o throughput.
Os locks acumularam 4,6 ms no runtime, 50,0 ms no scheduler e 17 us no presenter na sessão fria;
não são o custo dominante medido. Houve 132.710.400 bytes de captura/cópia residente e uma
preparação CPU de 8.355.840 bytes. Timestamps reportaram 1.063,8 us médios em 102 submissões;
isso é tempo de lista, não duração GPU isolada de cada cópia nem frame time completo.

Mantêm-se a fila direct e os quatro contextos: esses custos não justificam introduzir outra
fila ou compilação GCN especulativa nesta carga. Preservam-se os caches persistentes e a
especialização dependente de bindings; não se antecipam variantes que dependem de memória
guest mutável. A decisão deve ser reavaliada com V02/V06, não extrapolada para o Series S.
Blobs opcionais de PSO do driver continuam desativados no Xbox.

Para apresentação SDR RGBA8 residente, sem upscale e com gamma 1, a tabela de conversão é
verificada como identidade em todos os 256 componentes antes da cópia direta do buffer
compartilhado para o swapchain. Isso elimina textura convertida e dispatch intermediários
nesse caso; BGRA, gamma não identidade, FP16/PQ/HDR e FSR preservam a conversão. Não elimina
a captura compartilhada nem o fence que protege seu conteúdo. O diagnóstico de 72 comparações
CPU/GPU incluiu os dois casos diretos, sem erros D3D12. Não foi atribuído ganho de gameplay
a essa otimização a partir do diagnóstico.

Chronicles of Teddy foi desbloqueado após corrigir a consulta de aliases usada por acessos
CPU: uma escrita de 12 bytes na pilha da thread de áudio não pode exigir backing físico de
recurso GPU. `BackingRangesOverlap` compara somente as partes Direct/Flexible/Pooled existentes,
inclusive intervalos com lacunas, sem perder aliases físicos; a criação de recursos GPU mantém
a validação estrita. A busca na tabela de páginas também verifica seus limites antes de
indexar, mantendo o preenchimento com zeros das leituras esparsas não mapeadas.

As repetições expuseram uma reentrada adicional quando o AvPlayer solicitava mapeamentos:
um upload interno podia invalidar os buffers do contexto ou reiniciar sua lista ainda aberta.
Acesso CPU interno durante gravação, sincronização ou conclusão não reinicia esse trabalho;
consumidores CPU externos continuam passando pelos hazards/fences e pelos locks do runtime.
A marcação de escrita de render targets/depth/stencil ocorre após registrar o draw, antes de
preparar os downloads. O diagnóstico dirigido mantém um lease de mapeamento pendente durante
três draws que leem um buffer no fragment shader e confere os pixels, além dos casos de
pilha/lacunas/aliases e dos cenários P01–P04; zero erros D3D12.

Com essas correções, duas sessões desktop de 45 s, com cache frio/quente, terminaram com
`BOOT_RESULT=0`, `SHUTDOWN=0` e 15/16 apresentações. A repetição com o core do pacote final
teve 17 apresentações em 45 s, também com boot/stop/shutdown sem erro. PAC-MAN terminou a
regressão de 25 s com dez apresentações e os mesmos retornos de sucesso. A captura de Teddy
mostra o logo AKSYS Games. Isso resolve as falhas de backing/reentrada reproduzidas, sem
comprovar gameplay prolongado,
correção de todas as cenas ou hardware Xbox. A cobertura de P06 continua vinculada a V02/V06.

### Validação necessária para declarar o renderer completo

| ID | Evidência ainda necessária |
| --- | --- |
| V01 | Comparar valores, layouts, barreiras, atômicos, rasterização, metadata, blend/stencil e formatos com referências técnicas e diagnósticos no PC. |
| V02 | Cobertura de jogos reais no PC e depois Series S: cenas, gameplay prolongado, transições, efeitos, vários formatos/estágios e comparação visual com referência. Chegar à tela inicial não fecha isso. |
| V03 | Runtime UWP/AppContainer real para recursos/compiladores, resizing/DPI, HDR/SDR, apresentação, input e mudanças de estado do painel. Os diagnósticos desktop não exercitam todo esse ambiente. |
| V04 | Medir luminância/gamut/primárias em tela HDR e conferir FP16/PQ, inclusive transições e branco SDR. Readback FP16 sozinho não prova a saída física. |
| V05 | Repetir carga/descarga, stop, suspensão/retomada, detach, pressão de memória e falhas de GPU, verificando ownership/threads, estabilidade e memória no PC e Xbox. O host já tem teardown; sua cobertura real não está concluída. |
| V06 | Medir frame time, stalls, memória CPU/GPU e limites por aplicação em sessões prolongadas; ausência de retenção em 96 ciclos sintéticos não prova ausência de todo vazamento. |

A API UWP atualmente impede iniciar outro guest na mesma instância após um boot
(`guest_started`), solicitando reload da DLL. Isso é uma limitação de ciclo de
vida do host, separada da tradução gráfica. O boot brokered e o teardown não
estão ausentes: precisam de cobertura de cenários e eventual ampliação.

## Evidências já disponíveis

Esta seção reúne registros anteriores e diagnósticos executados nas atualizações
P01–P06 e F01–F14. Os diagnósticos em `Build/` são artefatos locais temporários, não uma
suíte incluída no repositório nem execução de jogos no Xbox.

| Evidência | Resultado registrado | Limite da conclusão |
| --- | --- | --- |
| `Build/f14-stencil-validation.log` | SM6.4: 1536 casos das 16 operações, três resultados, quatro write masks, quatro operandos e duas faces; 393216 comparações de bytes. Quatro draws com refs/masks diferentes, depth/stencil mutáveis, early/late e gate F13 conferem stencil/depth/cor e contadores. Extensão com RT menor, largura ímpar, D16S8/D24S8/D32S8, orçamento e oito submissões assíncronas com eviction de PSOs passaram; VS=120/PS=10240, zero erros D3D12. | Diagnóstico GPU PC com referência CPU; workers assíncronos também limitados a SM6.4. Uma camada/sample; fonte OpenOrbis conferido somente quanto à sintaxe. |
| `Build/f14-stencil-extra-validation.log` | Repetição dirigida dos casos de ordem, formatos, extensão e orçamento, stencil sem PS guest e oito evictions com a fila bloqueada por fence; zero erros D3D12. | Confirma trabalho pendente durante eviction; não mede paralelismo de contextos (pico 1). |
| `Build/f14-f13-regression-validation.log` / `Build/f14-f12-regression-validation.log` | Regressões de gate/storage/bounds e dos 34 casos F12. | Diagnósticos nativos PC; sem validação Xbox nesta atualização. |
| `Build/f13-gate-validation.log` | SM6.4: oito gates com storage/store e atômicos, stencil mutável e masks pass/fail; VS=9, PS=140 late ou 112 com stencil early. Store em imagem storage e máscaras R/B-pass/G-fail com XOR também conferidos. Dois casos point/line, bounds sem PS com intervalo válido/invertido e quatro bounds com UAV (256 invocações late, 128/0 early). Oito submissões assíncronas somam VS=72/PS=1120; zero erros D3D12. | Readbacks PC; restrições F13 acima preservadas. PS ausente também foi aceito com RTs de máscara zero. |
| `Build/f13-gs-validation.log` | Gate com GS: pixels/depth iguais à referência e contador GS=1; zero erros D3D12. | Caso sintético PC, sem validar todas as combinações de GS/tessellation com gate. |
| `Build/f13-f12-regression-validation.log` | Regressão dos 34 casos F12 de polígonos, IsFrontFace, bias e tessellation, zero erros D3D12. | Diagnóstico nativo PC; sem execução Xbox nesta atualização. |
| `Build/f12-combined-validation.log` | SM6.4: oito comparações de quad/quad strip/polygon/rect com contadores VS iguais à referência, 16 verificações IsFrontFace, quatro comparações de bias fill/wire nativo e seis de tessellation com/sem GS; zero erros D3D12. | Readbacks PC; sem bias em expansões, gameplay ou validação Xbox. |
| `Build/f22-recovery-validation.log` | Falha de gravação por alocação e RemoveDevice no renderer isolado recuperados; GDS, oclusão, filled-size, registradores e atômico preservados. Após cada recuperação: 256 pixels corretos, novo frame residente e zero erros na fila de diagnóstico do device restaurado. Leases CPU bloquearam a troca; modo padrão assíncrono também passou. Replay após submissão recusado, com atômico executado uma vez. Uma lista assíncrona pronta foi cancelada após falha de outro contexto; submissão bloqueada e nenhum replay no teardown. | PC com fault injection; prefixo SM6.4 e integração do rasterizer no modelo reportado pelo PC (6.6). Não valida TDR global, presenter/swapchain, gameplay, UWP ou Xbox. |
| `Build/f21-async-validation.log` | 55 casos em SM6.4: storage MSAA 2×/4×/8× com/sem array e base_array, linear/tiled alternativo, RGBA8/R32_UINT/R8_UINT/R16_UINT, padding e samples não escritos preservados; aliases RGBA8↔R32_UINT da mesma base em linear/tiled e RGB96 store/sample, leitura MSAA 1/16 e quatro escritas assíncronas em samples distintos; zero erros D3D12. | Diagnóstico nativo PC; não prova todas as views, metadata, large images, UWP ou Xbox. |
| `Build/f20-occlusion-validation.log` / `Build/f20-depth-occlusion-validation.log` | SM6.4: 256 samples por draw, acumulação 512, scissor 128, área vazia e rejeição depth/stencil com zero, reset e 12 draws assíncronos totalizando 3.072; zero erros D3D12. | Diagnóstico nativo PC; não cobre todas as expansões, MSAA/EQAA ou Xbox. |
| `Build/f20-pm4-validation.log` | Processador Liverpool com sink de diagnóstico: predicação ZPASS/PRIMCOUNT/BOOL32/64, inversão, continuação, no-wait, espera/retomada, registradores predicados no parser rápido, COPY_DATA 32/64 bits entre memória/GDS/registradores gráficos e compute/imediato e dump de contadores. | Valida encaminhamento/estado PM4; o sink deste diagnóstico não rasteriza. |
| `Build/f19-sdwa-validation.log`, `Build/f19-scalar-validation.log` e `Build/f19-float-validation.log` | 1.176 combinações SDWA inteiras, 1.200 casos escalares com SCC e 112 conversões SDWA para float com omod/clamp, por GCN→IR→HLSL→DXIL/SM6.4 e leitura GPU. Rejeição explícita de DPP conferida; zero erros D3D12. | Compute no PC; não constitui cobertura total de IR/GCN, estágios gráficos, Vulkan, UWP ou Xbox. |
| `Build/f18-numeric-validation.log` | Três adições GCN FP64 com resultado exato em bits, quatro testes de denorm FP32 entrada/saída e cinco valores especiais (zeros assinados, infinitos e NaN); `F18_ERRORS=0`. | PC/SM6.4 com FP64 nativo; não prova todos os opcodes, payload de NaN em operações gerais, rounding dirigido nem software FP64/cross-lane. |
| `Build/f18-gs-regression-build.log` e `Build/f18-tess-regression-build.log` | Regressões por captura/replay GS e LS/HS/DS/GS após a alteração do perfil numérico; zero erros D3D12. | Caminhos existentes, sem adicionar cobertura numérica FP64 em todos esses estágios. |
| `Build/f17-blend-extra-validation.log` | 240 casos RGB constantes/alpha e inversos, ADD/SUB/REV_SUB/MIN/MAX, máscaras RGB/alpha, saídas adicionais não ligadas e duas instâncias sobrepostas; referência CPU com tolerância de um nível UNORM e 256/512 atômicos PS; zero erros D3D12. | Fallback forçado no PC/SM6.4, um RT RGBA8 no slot 0; não cobre MRT, formatos float, MSAA ou Xbox. |
| `Build/f16-stipple-validation.log` | 16 comparacoes CPU/GPU: REPEAT_COUNT 0/1/3/255, LSB/MSB, clipping fracionario e linha visivel apos 4.096 janelas. Tres comparacoes com recorte explicito para W negativo/zero/pequeno; `F16_ERRORS=0`. | PC/SM6.4, linhas horizontais com width=2 e PS constante; nao prova reset, estado/acumulacao nem cobertura geral. |
| `Build/f15-clip-validation.log` | Cinco configuracoes do viewport 1 (incluindo clip-disable com W=1), cinco modos adjacency GPU e oito casos flat-last GS (strips/cuts x fill/point/wide/stipple); pixels e contadores iguais as referencias, `F15_ERRORS=0`. | PC/SM6.4; nao cobre tessellation provoking-last nem viewport diferente em cada primitiva. |
| `Build/f15-interpolation-validation.log` | W distinto nos endpoints, smooth/noperspective, com e sem clipping: linha continua e subdividida identicas, `max_error=0` nos quatro casos; zero erros D3D12. | Nao estabelece cobertura/interpolacao completa em todos os casos. |
| `Build/f14-f12-regression-validation.log` e `Build/f14-f13-regression-validation.log` | Reexecutados apos F15: faces/GS/tessellation, gate por depth, storage e submissao assincrona; zero erros D3D12. | Arquivos conservam o nome do runner anterior. |
| `Build/f12-face-geometry-validation.log` | 60 comparações por face com GS: point/wide/stipple, strips/cuts, emissão vazia, 2/32 invocações, adjacency, múltiplas primitivas, rings e RectList; pixels e atômicos conferidos, zero erros D3D12. | Referência GPU; permanecem as restrições F12/F15/F16 descritas acima. |
| `Build/f12-order-validation.log` | Duas comparações de três triângulos sobrepostos com orientação alternada: blend e depth preservam a ordem original; nove execuções VS e zero erros D3D12. | Referência nativa PC; não comprova ordenação de data races indefinidas pelo contrato guest. |
| `Build/f12-async-validation.log` | Oito submissões de replay por face, contador final 8 e zero erros D3D12. | Hazard no mesmo backing, pico de um contexto em voo. |
| `Build/f09-package-build.log` / `Build/f09-package-verification.log` | Core Release compilado, bundle assinado com CMS válido e payload/core de produção coincidentes. | Pacote não instalado nem executado no host UWP; sem validação Xbox desta revisão. |
| `Build/f09-production-pacman.log` / `Build/real-game-frame.png` | PAC-MAN 256 chegou à tela inicial em 1920×1080, 56 apresentações em 75 s, `BOOT_RESULT=0` e `SHUTDOWN=0`; core com hash igual ao empacotado. | Conferência visual no harness PC, sem gameplay prolongado ou execução UWP/Xbox. |
| `Build/f09-geometry-matrix-validation.log` | 33 comparações em SM6.4: point fill, wide/stipple, strips/cuts, emissão vazia, 2/32 invocações, saída de linhas, adjacency, múltiplas primitivas, rings ES→GS e entrada RectList; pixels e contador atômico conferidos, zero erros D3D12. | Referência GPU pelo caminho sem guest GS; os efeitos do GS ocorrem uma vez por invocação. Linha sólida tem verificação independente de intensidade por fragmento. |
| `Build/f09-attributes-partial-validation.log` | 12 comparações de atributos completos/parciais, interpolação e flat pelo primeiro vértice nos três modos; zero erros D3D12. | Referência GPU e verificação independente do valor flat, em saída RGBA8. |
| `Build/f09-feedback-validation.log` | Três comparações em que o PS lê o valor escrito pelo GS antes da expansão, com contador igual a 1; zero erros D3D12. | Coerência entre os dois draws no PC. |
| `Build/f09-tessellation-validation.log` | Três comparações LS/HS/DS→GS contra LS/HS/DS nos modos point/wide/stipple, com rings ES e contador do GS igual a 1; zero erros D3D12. | Diagnóstico GPU no PC, sem gameplay Xbox. |
| `Build/f09-geometry-async-validation.log` | Oito submissões assíncronas de captura/expansão, contador final 8 e zero erros D3D12. | A mesma escrita induz hazard; pico de um contexto em voo nessa amostra. |
| `Build/f09-broad-regression-validation.log` | Regressão GCN/SM6.4, aliases, feedback, atômicos, tiling, MSAA e estágios; zero erros D3D12. | Diagnóstico nativo PC. |
| `Build/f08-border-validation.log` / `Build/f08-border-boundary-validation.log` | 1536 casos em SM6.4: oito modos, point/linear, bordas fixas/customizadas, FLOAT/UNORM/SNORM/UINT/SINT, coordenadas negativas/fracionárias/limites exatos e imagens com/sem alias; zero erros D3D12. | Referência CPU por texel/tap; modos com equivalente também passam pelo sampler nativo PC. |
| `Build/f08-border-bits-validation.log` | Dois casos FLOAT32 com/sem alias preservam os bits de zero negativo, infinito, NaN com payload e subnormal no custom border point; zero erros D3D12. | Readback float32 no PC; não valida hardware Xbox. |
| `Build/f08-border-integer-bits-validation.log` | Quatro casos UINT/SINT com/sem alias preservam os 32 bits dos quatro componentes de custom border, observados por escrita em buffer após a amostragem; zero erros D3D12. | Readback de words no PC, sem conversão float no valor comparado. |
| `Build/f08-broad-regression-validation.log` | Regressão GCN/SM6.4, aliases, feedback, tiling, atômicos, MSAA e estágios; zero erros D3D12. | Diagnóstico nativo PC. |
| `Build/f08-production-pacman.log` / `Build/real-game-frame.png` | PAC-MAN 256 alcançou a tela inicial em 1920×1080, 84 apresentações em 75 s, `BOOT_RESULT=0` e `SHUTDOWN=0`; core com hash igual ao empacotado. | Conferência visual no harness PC; sem gameplay ou execução UWP/Xbox desta revisão. |
| `Build/f08-package-build.log` / `Build/f08-package-verification.log` | Core Release compilado, bundle assinado com CMS válido e payload/core de produção coincidentes. | Pacote não instalado nem executado; sem validação Xbox desta revisão. |
| `Build/f07-package-build.log` / `Build/f07-package-verification.log` | Core Release compilado, bundle assinado com CMS válido e payload/core de produção coincidentes. | Pacote não instalado nem executado; sem validação Xbox desta revisão. |
| `Build/f07-production-pacman.log` / `Build/real-game-frame.png` | PAC-MAN 256 atingiu a tela inicial com prompt de início em 1920×1080; `BOOT_RESULT=0` e `SHUTDOWN=0`, usando o mesmo hash do core empacotado. | Harness PC de produção; sem gameplay nem execução UWP/Xbox nesta revisão. |
| `Build/f07-bptc-validation.log` | 45 casos float BC6/BC7 em SM6.4, todos os modos e reservados, BC6 signed/unsigned, vários blocos/texels e bloco escrito por buffer antes da amostragem; zero erros D3D12. | Comparação com sampler nativo PC; BC6 coincide exatamente nos casos registrados. |
| `Build/f07-query-cube-validation.log` | 16 comparações float: gather com borda/swizzle e imagem gravável, LOD limitado/sem clamp e dez faces de cube array; zero erros D3D12. | Sampler nativo PC; não mede desempenho em jogos. |
| `Build/f07-formats-validation.log` / `Build/f07-numeric-validation.log` | 12 casos de packed/depth/pares e 18 casos SNORM/UINT/SINT; leitura/escrita e preservação de componentes inteiros, zero erros D3D12. | Quatro comparações com sampler nativo e 26 referências CPU de bits/conversões. Não cobre todas as combinações numéricas rejeitadas. |
| `Build/f07-gather-validation.log` | 11 verificações de LOD fracionário, point/linear, bias, clamps, offsets e PCF; zero erros D3D12. | Referência analítica de mips/valores, sem prova geral da precisão GCN. |
| `Build/f07-broad-regression-validation.log` | Regressão GCN/SM6.4, aliases, feedback, tiling, atômicos, MSAA e estágios; zero erros D3D12. | O ajuste final de swizzle/borda de gather foi verificado pelos 16 casos específicos acima. |
| `Build/f06-storage-sampling-validation.log` / `Build/f06-minmag-validation.log` | 191 casos de amostragem e cinco casos adicionais de min/mag/clamps em SM6.4; zero erros D3D12. Inclui mips, derivadas, arrays, volume, sRGB, reduction, borda float e clamp do shader. | 187 comparações com sampler nativo e quatro verificações analíticas de anisotropia 3D; estas quatro diferem do filtro nativo PC. Precisão e controles GCN continuam pendentes. |
| `Build/f06-broad-regression-validation.log` | Regressão GCN/SM6.4, aliases, feedback, tiling, atômicos e MSAA; zero erros D3D12. | Diagnóstico PC, anterior ao ajuste final de clamps coberto pela matriz de amostragem. |
| `Build/f06-production-pacman.log` / `Build/real-game-frame.png` | PAC-MAN 256 atingiu a tela inicial com prompt de início em 1920×1080; `BOOT_RESULT=0` e `SHUTDOWN=0`, usando o mesmo hash do core empacotado. | Harness PC de produção; sem gameplay, execução UWP ou Xbox nesta revisão. |
| `Build/f06-package-build.log` / `Build/f06-package-verification.log` | Core Release compilado, bundle assinado com CMS válido e payload/core de produção coincidentes. | Pacote não instalado nem executado; sem validação Xbox desta revisão. |
| `Build/p06-profile-before.log` / `Build/p06-production-cold.log` / `Build/p06-production-warm.log` | PAC-MAN 256: leitura média de programa caiu de 62,7 para 16,9 ms; cache quente com seis hits SPIR-V, cinco blobs PSO e zero chamadas DXC; boot/teardown sem erro. | Logo de startup no harness PC; apresentações oscilaram de 15 a 25 por sessão, sem ganho consistente de throughput nem prova visual/gameplay/Xbox. |
| `Build/p06-sm64-validation.log` | Regressão GCN/SM6.4, aliases, atômicos, feedback, tiling, MSAA e GS/tessellation; zero erros D3D12. | Diagnóstico PC; não substitui a cobertura de jogos/cenas/Xbox. |
| `Build/p06-final-uwp-build.log` / `Build/p06-package-verification.log` | Release x64 compilado, bundle assinado e hashes dos payloads conferidos com o core de produção. | Compilação e pacote; não significa instalação/execução UWP nem validação Xbox. |
| `Build/p06-direct-presenter-validation.log` | 72 comparações CPU/GPU, dois casos de cópia residente direta, HDR/FSR/resize e zero erros D3D12. | Diagnóstico desktop; não mede ganho de gameplay nem valida a saída física HDR/Xbox. |
| `Build/teddy-recursion-cold.log` / `Build/teddy-recursion-warm.log` | Teddy executou duas sessões de 45 s, com 15/16 apresentações e boot/stop/shutdown sem erro; captura do logo AKSYS. | Harness desktop e startup, sem aprovação de gameplay/Xbox. |
| `Build/teddy-final-production.log` / `Build/teddy-fix-pacman-regression.log` | Core do pacote final: Teddy 45 s/17 apresentações e PAC-MAN 25 s/dez apresentações, boot/stop/shutdown sem erro. | Sessões de startup no PC, sem prova de gameplay prolongado ou Xbox. |
| `Build/teddy-final-uwp-build.log` / `Build/teddy-final-package-verification.log` | Release x64 assinado; hashes de core/payloads conferidos com a produção. | O pacote não foi instalado/executado no AppContainer ou no Xbox. |
| `Build/teddy-memory-final-validation.log` | 20 verificações de aliases/lacunas/pilha/limites, GPU estrita preservada, três draws com lease de mapeamento pendente e pixels corretos; regressão P01–P04, zero erros D3D12. | Diagnóstico PC. |
| `Build/p05-tiling-validation.log` | 210 casos comparados byte a byte com a referência CPU: arrays parciais, volumes, MSAA 2/8, BC1–BC7, packed, máscaras 15/5/0, backing/padding e dispatch acima de 65535 grupos; upload persistente sem snapshot e sem readback duplicado, zero erros D3D12. | Diagnóstico nativo PC; não mede ganho de desempenho em jogos nem valida hardware Xbox. |
| `Build/p05-residency-validation.log` | Regressão com memória guest real, argumentos indiretos/índices, ownership, aliases/dirty e quatro ciclos de pressão; zero erros D3D12. | Diagnóstico PC; não comprova gameplay Xbox. |
| `Build/p05-sm64-validation.log` | Regressão GCN/SM6.4, aliases/atômicos/feedback, alternate tiling, MSAA e GS/tessellation; zero erros D3D12. | Runtime nativo PC; não comprova desempenho/gameplay Xbox. |
| `Build/p05-presenter-validation.log` | 72 comparações CPU/residente, 88 capturas sem download e recriação FSR após trim com pixels preservados; zero erros D3D12. | Diagnóstico PC; não é instalação/execução UWP nem validação de suspensão Xbox. |
| `Build/p05-uwp-build.log`, `Build/p05-package-verification.log` | Release x64 compilado, bundle com assinatura CMS válida e executável/seis DLLs iguais aos binários de produção. | Pacote não instalado/executado; Xbox não validado nesta etapa. |
| `Build/p04-validation.log` | 15 cenários de orçamento (processo/dispositivo/RAM/UMA/zero/excesso/overflow), transferência presa por fence e quatro ciclos de pressão preservando imagens/buffers dirty e reutilização; regressões P01–P03 e zero erros D3D12. | Política e pressão induzida em diagnóstico PC; não comprova notificações/suspensão reais no Xbox. |
| `Build/p04-presenter-validation.log` | Seis trims/recriações FSR com pixels preservados, 72 comparações CPU/residente e 88 capturas sem download; zero erros D3D12. | Diagnóstico PC; não é suspensão real de aplicativo UWP. |
| `Build/p04-sm64-runtime.log` | Regressão GCN/SM6.4 com oito coletas induzidas durante os comandos, aliases, atômicos, tiling, MSAA e GS/tessellation; zero erros D3D12 e exit code 0. | Diagnóstico nativo PC; sem comprovação de gameplay ou pressão real UWP/Xbox. |
| `Build/p04-uwp-build.log`, `Build/p04-package-verification.log` | Release x64 compilado, assinatura CMS válida e executável/seis DLLs conferidos contra os binários de produção. | Pacote não instalado/executado; Xbox não validado nesta etapa. |
| `Build/p03-validation.log` | 705 combinações de índices, limite de 65536 grupos, contagem zero/limitada, strides 24/28, base signed/SGPRs, dispatch indireto e argumentos residentes sem readback; zero erros D3D12. | Diagnóstico nativo PC; não comprova gameplay Xbox. |
| `Build/p03-guest-validation.log` | Caminho de produção DrawIndirect com contagem GPU e expansão de strips/provoking, mais regressão de residência; zero erros D3D12. | Não é execução de jogo. |
| `Build/p03-presenter-validation.log` | 72 comparações CPU/residente e 88 capturas sem download; FSR/resize/HDR e zero erros D3D12. | Diagnóstico nativo PC. |
| `Build/p03-sm64-validation.log` | Regressão GCN/SM6.4, aliases, atômicos, tiling, MSAA, GS/tessellation; 1529 submissões com timestamps e zero erros D3D12. | Não comprova desempenho/gameplay Xbox. |
| `Build/p03-uwp-build.log`, `Build/p03-package-verification.log` | Release x64 compilado; bundle com assinatura CMS válida e executável/DLLs conferidos contra os binários de produção. | Pacote não instalado nem executado; Xbox não validado nesta etapa. |
| `Build/p02-uwp-build.log`, `Build/p02-package-verification.log` | Release x64 compilado, bundle assinado com CMS válido e payload igual ao núcleo/DLLs de produção. | Pacote não instalado/executado; Xbox não validado nesta alteração. |
| `Build/p02-sm64-validation.log` | GCN/SM6.4, aliases físicos, atômicos, tiling, MSAA, GS/tessellation e timestamps de 1529 submissões; zero erros D3D12. | Diagnóstico nativo PC com contrato síncrono independente. |
| `Build/p02-presenter-validation.log` | 72 comparações CPU/residente e 88 capturas sem download, incluindo FSR/resize/HDR; zero erros D3D12. | Não valida apresentação/gameplay no Xbox. |
| `Build/p02-buffer-validation.log` | 32 dispatches sem readback/releitura CPU; consumidor por alias e input de índice por cópia GPU; dirty de 4 KiB e novas views preservando palavras GPU; zero erros D3D12. | Diagnóstico nativo PC; não comprova desempenho/gameplay Xbox. |
| `Build/p02-residency-validation.log` | Residência de imagens, RTV→SRV, CPU/GPU concorrentes, map/unmap/protect, eviction e destruição; zero erros D3D12. | Não comprova equivalência geral ao PS4. |
| `Build/p01-uwp-build.log`, `Build/p01-package-verification.log` | Release x64 compilado, bundle assinado com CMS válido e payload comparado ao núcleo/DLLs de produção. | Pacote não instalado nem executado; Xbox não validado nesta alteração. |
| `Build/p01-async-validation.log` | GPU bloqueada: quatro contextos, duas submissões sem espera CPU; compute/gráficos, hazards RAW/WAR, leases, reciclagem, resolve MSAA, map/unmap e destruição; zero erros D3D12. | Diagnóstico nativo PC, sem gameplay Xbox. |
| `Build/p01-sm64-validation.log` | Regressão GCN/SM6.4, aliases, atômicos, tiling, MSAA, GS/tessellation e timestamps de 1529 submissões; zero erros D3D12. | Runtime independente síncrono; não comprova gameplay Xbox. |
| `Build/p01-presenter-validation.log` | 72 comparações CPU/residente, 88 capturas sem download, FSR/resize/HDR; zero erros D3D12. | Diagnóstico nativo PC. |
| `Build/p01-residency-validation.log` | Residência, RTV→SRV sem download, aliases, CPU/GPU concorrentes, eviction, proteção e flush/destruição; zero erros D3D12. | Não comprova equivalência geral ao PS4. |
| `Build/mixed-adaptations-sm64-validation.log` | Regressão GCN/SM6.4, aliases, atômicos, tiling, MSAA, GS/tessellation; zero erros D3D12; timestamps de 1529 submissões. | Não é compatibilidade geral com jogos/PS4/Xbox. |
| Mesmo log | Quatro heaps reutilizados por 96 ciclos, staging CPU retido zero em 96 ciclos e upload parcial de quatro bytes em buffer de 1 MiB. | Evidência desses ciclos/intervalos, sem prova global de ausência de vazamentos. |
| `Build/mixed-adaptations-guest-validation.log` | MIN/MAX, alpha constante nativo/fallback, bypass, depth bounds inativo, NONE, overflow e provoking vertex first/last/restart; coerência, eviction e destruição; zero erros. | Hipóteses guest ainda exigem validação adicional no PC. |
| `Build/mixed-adaptations-presentation-validation.log` | 72 referências CPU/residente, 88 captures sem downloads, FSR/RCAS, HDR/float, tiling e resize; zero erros. | Composição nativa desktop, sem validar scanout HDR ou AppContainer/Xbox. |
| `Build/mixed-adaptations-uwp-build.log` e `Build/mixed-adaptations-package-verification.log` | Release x64, bundle assinado (`CMS_SIGNATURE=PASS`), payload e núcleo de produção coincidentes. | Pacote compilado/assinado não comprova instalação ou runtime. |
| Relato anterior do usuário | Tela inicial atingida no Xbox Series S. | Evidência de boot nesse cenário, sem validação prolongada desta revisão. |

## Critério de conclusão

Para declarar suporte gráfico completo, é necessário fechar ou delimitar por
contrato de hardware as lacunas F01–F21, validar os usos guest suportados e
executar a matriz V01–V06. Recuperação F22, desempenho de P01–P05 e throughput P06 precisam ter seu
nível de suporte explicitado para o produto. Nenhum contador isolado de casos,
nenhum build e nenhum boot substitui essa cobertura.

FSR1 é opcional; HDR filtrado por FSR, TAA/FSR2/FSR3, frame generation e uma copy
queue separada não são recursos obrigatórios da emulação PS4. `temporal_jitter.h`
e a [API de jitter](UWP-NATIVE-TEMPORAL.md) existentes não constituem um upscaler
temporal implementado.

## Melhorias derivadas da análise de pt-pc-main

A análise e a relação entre referência, mudanças e restrições estão em
[D3D12-PT-PC-ADAPTATIONS.md](D3D12-PT-PC-ADAPTATIONS.md). O runtime reutiliza
quatro heaps de descritores por contexto após fences; buffers fazem upload do intervalo
alterado quando o backing não foi remapeado. GAME_PROFILE permite medir ticks
GPU dos batches, além dos tempos CPU existentes. Esperas verificam remoção do
device periodicamente, os proprietários drenam as filas na destruição e os
relatórios de falha acrescentam orçamento DXGI e contextos DRED disponíveis.

## Referência ps4-dx12-fix-mixed-cmask

A análise da árvore local `ps4-dx12-fix-mixed-cmask/src/video_core/renderer_d3d12`
originou as adaptações de FSR1, estados de blend/depth/NONE, provoking vertex e
captura de argumentos indiretos já descritas nas tabelas acima. DMA virtual GPU,
captura geral de pontos e filas assíncronas da referência não foram portados;
suas dependências de compilador e ownership não se integram por cópia isolada.

## Inicialização e caminho Dozen em 23/10/2026

As correções de arquivos, memória e inicialização, junto com as correções de
entrada DXIL, provoking vertex e clip depth do Dozen, estão descritas em
[UWP-GAME-STARTUP.md](UWP-GAME-STARTUP.md). Os resultados desse caminho não
encerram pendências do renderer D3D12 nativo. A validação atual é no PC;
esta revisão ainda não foi executada no Xbox.

