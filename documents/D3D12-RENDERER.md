# Renderer D3D12: estado de implementação

Revisão do código local em **21/10/2026**. Este documento separa implementação,
restrições funcionais, desempenho e validação. O renderer nativo é o padrão do
host UWP; Vulkan/Dozen continua sendo uma alternativa em [DOZEN-UWP.md](DOZEN-UWP.md).

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
| Aliases e feedback shader | `IMAGE_LOAD`/`STORE`, dimensões, amostragem parcial e storage compartilhado para aliases imagem/imagem/buffer, inclusive endereços virtuais distintos e entre estágios. | [lowering](../src/video_core/renderer_d3d12/image_buffer_alias.cpp), [preparação](../src/video_core/renderer_d3d12/shader_cache.cpp) |
| Escritas subword | Texels/componentes de 8/16 bits usam extração e compare-and-swap de 32 bits para preservar os vizinhos nas escritas compartilhadas. | [lowering](../src/video_core/renderer_d3d12/image_buffer_alias.cpp) |
| Atômicos em aliases | Add, min/max signed/unsigned, and/or/xor, exchange, compare-swap inteiro/float, inc/dec e min/max float para componente de 32 bits. Não devem voltar à lista genérica de “atômicos restantes” já resolvidos. | `Atomic` em [image_buffer_alias.cpp](../src/video_core/renderer_d3d12/image_buffer_alias.cpp) |
| Coerência | Leases CPU/GPU, resolução física, callbacks de memória, map/unmap/protect e invalidação; imagens podem continuar residentes/dirty entre comandos e sincronizar por intervalo quando a CPU ou outro uso exige. | [runtime](../src/video_core/renderer_d3d12/command_runtime.cpp), [memória](../src/core/memory.cpp) |
| Metadata e clear | CMASK misto por tile 8×8 para códigos 0/F, clear tipado, swizzle/quantização e preservação de tiles/padding; modos alternate/Neo e bank swizzle nos layouts suportados. | [clear](../src/video_core/renderer_d3d12/color_clear.h), [CMASK](../src/video_core/renderer_d3d12/cmask_address.h) |
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
| Indiretos/índices GPU | Até 4096 draws por lote, janela de argumentos até 16 MiB; índices expandidos até 4 milhões; scratch GPU de 32 MiB por contexto, reutilizado após sua fence. Stride e overflow validados; índices indiretos exigem a janela de `max_index_size` mapeada. |
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
Janelas não mapeadas integralmente ou views físicas não contíguas também usam
fallback, sem ampliar artificialmente o intervalo guest válido.

A expansão paralela cobre line loop/strip, triangle fan/strip/list, polygon,
quad list/strip, provoking vertex e byte swap nos casos elegíveis sem restart.
Índices 16/32 bits e sequências não indexadas são aceitos. Restart, contornos,
point fill, conversões antes de GS/tessellation e passes especiais continuam no
caminho CPU. Nenhuma dessas mudanças conclui o contrato de bounds guest de F03.

Isso não equivale a dizer que toda imagem é baixada a cada draw. Com tracking
ativo, a imagem mantém ownership/dirty na GPU e sincroniza sob demanda em acesso
CPU, alias incompatível, invalidação, eviction, alteração de mapeamento ou flush.
`AcquireCpuAccess`, `AcquireMemoryMappingAccess` e as watches de memória já estão
integrados. O snapshot de VideoOut compatível evita download da imagem guest,
embora ainda copie e espere a GPU. Não há scanout direto da textura mutável.

Esses mecanismos não comprovam equivalência de todas as políticas de cache e
barreiras guest. Os casos residuais de coerência estão na lista abaixo; as
fontes de comparação em hardware estão na seção de evidências.

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

## Trabalho pendente para jogos

### Correção funcional e fidelidade

A lista abaixo é a fila atual de lacunas encontradas no código. A numeração
serve para acompanhamento; não é ordem de facilidade nem garantia de que todo
jogo depende de cada item.

| ID | O que falta para cobertura completa | Evidência no código |
| --- | --- | --- |
| F01 | Feedback de attachments: render target/depth-stencil usado simultaneamente como textura/buffer em shader, preservando ordem e efeitos laterais sem replay incorreto. | `ValidateTargetRange` em [resource_bindings.cpp](../src/video_core/renderer_d3d12/resource_bindings.cpp) rejeita sobreposição. |
| F02 | Fetch coerente de vértices/índices quando o mesmo backing é escrito por shaders; incluir fetch shaders/views que ainda dependem de snapshots CPU. | `ValidateInputRange` rejeita aliases de entrada com storage escrito. |
| F03 | Aplicar o contrato guest de capacidade do index buffer, índices fora do intervalo e ranges parcialmente mapeados em todos os draws diretos/indiretos. | Liverpool preenche `max_index_size`, mas o renderer nativo monta a view pelo `request.count`; não consome esse limite. |
| F04 | CMASK além de 0/F, metadata comprimida/TC-compatible, DCC e HTILE com semântica real; FMASK com remapeamento/compressão guest em vez de valores constantes. | `MaterializeColorClearImpl` rejeita códigos/outros modos; [resource_patching_pass.cpp](../src/shader_recompiler/ir/passes/resource_patching_pass.cpp) substitui leituras FMASK por constantes e não aceita storage FMASK. |
| F05 | EQAA verdadeiro, em que samples e fragments diferem, e equivalência da frequência parcial de shading por sample. | [draw_runtime.cpp](../src/video_core/renderer_d3d12/draw_runtime.cpp) rejeita EQAA; o caminho parcial atual pode executar PS por sample, alterando o número de efeitos laterais. |
| F06 | Completar amostragem de storage compartilhado: LOD implícito com cadeia de mips, filtragem entre mips, anisotropia, seleção min/mag/LOD/reduction fiel e cálculo geral de gradientes. | `Sample` em [image_buffer_alias.cpp](../src/video_core/renderer_d3d12/image_buffer_alias.cpp) exige um nível no caso implícito; LOD explícito/gradiente escolhe mip por floor; a tabela não carrega todos os parâmetros do sampler nativo. |
| F07 | Gather/query ainda sem lowering, cube sampling no storage compartilhado, BC6/BC7 nesse lowering, escrita comprimida e demais formatos/depth não expressos pelo caminho raw. | O lowering aceita formas 1D/2D/3D/arrays, BC1–BC5 normalizado single-sample e leitura de blocos; rejeita outros casos. Não confundir com BC1–BC7/cube nas views nativas. |
| F08 | Modos de borda/mirror sem equivalente direto e precisão completa de custom border no sampler manual. | `CheckWrapMode`/`AddressMode` rejeitam modos que exigem coordenadas em shader; a borda custom do sampler raw é quantizada para RGBA8. Sampler nativo tem caminho separado. |
| F09 | GS on-chip e integração dos caminhos de expansão com guest GS/tessellation nos casos atualmente incompatíveis. | [d3d12_rasterizer.cpp](../src/video_core/renderer_d3d12/d3d12_rasterizer.cpp) rejeita on-chip, point fill com GS/tessellation e wide/stippled lines com guest GS. |
| F10 | Stream-out completo: draws sem GS, múltiplos streams/rast stream, layout/seleção exata dos descritores, emissão dinâmica, filled-size e sincronização real dos eventos de stream-out. | Captura atual cobre stream 0 off-chip; seleção de buffers é heurística por stride/user data e contagem usa nós estáticos de emissão. Liverpool sinaliza `SoVgtStreamoutFlush` imediatamente. |
| F11 | Tessellation Pow2 e combinações de domínio/topologia que exigem emulação/expansão adicional. | `CompileNativeSpirv` só mapeia Integer/FracOdd/FracEven e rejeita combinações sem equivalente. |
| F12 | Modos polygon/bias diferentes por face com geometry/tessellation e com primitivas quad/polygon/rect atualmente rejeitadas; preservar efeitos laterais ao usar múltiplos passes. | Guardas de `mixed_mode`, `bias_split` e `face_passes` no rasterizer. |
| F13 | Gate de cor por depth com shaders que escrevem storage/atomics, equivalência para depth/stencil que muda entre primitivas do mesmo draw e depth bounds em draw sem pixel shader. | `depth_gated` usa fail/base passes; `writes_memory` restringe efeitos laterais. A pass fail lê o depth anterior à pass que escreve. `DrawImpl` exige PS para o lowering de depth bounds. |
| F14 | Operações de stencil guest sem mapeamento direto (AND/OR/XOR/NAND/NOR/XNOR) e configurações de refs/máscaras incompatíveis com os valores únicos do output merger. | `StencilOp` e validações front/back em [d3d12_rasterizer.cpp](../src/video_core/renderer_d3d12/d3d12_rasterizer.cpp). |
| F15 | Provoking-last flat com adjacency/GS/tessellation; orientação/clip-disable por viewport quando diferem do viewport 0; regras precisas de cobertura/interpolação das expansões de linhas/pontos. | Guardas `remap_provoking`, orientação baseada no viewport 0 e GS auxiliares em [shader_cache.cpp](../src/video_core/renderer_d3d12/shader_cache.cpp). |
| F16 | Line stipple completo: controles de reset/estado/acumulação, perspectiva/cobertura fiel, segmentos com W problemático e padrão sem truncamento pelo limite do GS auxiliar. | `CompileLineExpand` limita janelas/instâncias e usa quads; controles adicionais de stipple não são consumidos integralmente. |
| F17 | Combinar cor constante RGB e alpha constante RGB quando o dispositivo não oferece `AlphaBlendFactorSupported`, sem alterar silenciosamente os fatores. | O fallback replica alpha nas blend constants e rejeita a combinação conflitante. |
| F18 | Fidelidade numérica GCN: FP64, modos de arredondamento/denorm/NaN e operações cross-lane nos dispositivos sem garantia wave64. | O perfil nativo não habilita float64/controles completos; [recompiler.cpp](../src/shader_recompiler/recompiler.cpp) reduz FP64 para FP32; `CompileSpirv` rejeita waves incompatíveis. |
| F19 | Operações IR/GCN ainda sem implementação no recompilador compartilhado e sua passagem completa por DXIL/SM6.4; não basta compartilhar o backend SPIR-V para afirmar cobertura total. | `EmitDataAppend`/`EmitDataConsume` em [emit_spirv_atomic.cpp](../src/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp) são unreachable; o frontend também possui variantes SDWA/DPP/opcodes sem tradução. |
| F20 | Predicação e consultas de oclusão reais, além dos comandos PM4 válidos hoje apenas avisados/stubados, como `IT_COPY_DATA` no caminho gráfico. | [liverpool.cpp](../src/video_core/amdgpu/liverpool.cpp): `IT_SET_PREDICATION` não implementado; `PixelPipeStatDump` produz contador sintético; CopyData registra warning. O SGPR de draw index (`draw_index_enable`/`draw_index_loc`) também não é propagado pela interface atual; P03 preserva os SGPRs de base vertex/instância já suportados. É uma lacuna compartilhada do processador de comandos. |
| F21 | Fechar as combinações ainda rejeitadas de views/formato/layout/alias, incluindo storage MSAA escrito e SRV/UAV que não puderem ser lowerados. | Guardas em [resource_bindings.cpp](../src/video_core/renderer_d3d12/resource_bindings.cpp) e [image_cache.cpp](../src/video_core/renderer_d3d12/image_cache.cpp). A validação deve ser por uso do formato, não somente pela presença em `ConvertFormat`. |
| F22 | Recuperação controlada após device loss/falha de gravação e restauração dos recursos/estado da sessão, se a continuidade sem reinício for exigida. | O runtime fica marcado como falho e exige recriação; diagnóstico/DRED e drenagem já existem, recuperação transparente não. |

Os atômicos 32-bit listados em “Caminhos implementados”, aliases virtuais entre
estágios, segmentos físicos repetidos, CMASK 0/F por tile e dirty tracking de
imagens **não estão pendentes como recursos inteiros**. F01/F02/F06/F07/F21
identificam os casos residuais concretos. Tampouco se exige coerência definida
para data races que o próprio contrato guest não define: falta confrontar as
políticas de sincronização/cache em usos válidos com hardware.

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
| P06 | Profiling e otimizações implementados; ampliar cobertura de jogos/cenas e Series S. | Agregação CPU por etapa/lock/fence, hits de caches, bytes de apresentação e timestamps GPU; leitura curta de metadata e cópia residente direta quando a conversão é identidade. PAC-MAN medido no PC; Teddy bloqueado por backing guest não mapeado. Decisões de novas filas/precompilação dependem de mais sessões; blobs de driver no Xbox continuam desativados. |

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

Chronicles of Teddy iniciou, mas encerrou com `Guest GPU range lacks mapped physical backing`
antes de produzir uma sessão útil. Esse resultado não conta como aprovação de compatibilidade
ou desempenho. A cobertura de P06 continua vinculada a V02/V06, inclusive gameplay e Xbox.

### Validação necessária para declarar o renderer completo

| ID | Evidência ainda necessária |
| --- | --- |
| V01 | Compilar e executar os homebrews OpenOrbis em PS4 físico; comparar valores, layouts, barreiras, atômicos, rasterização, metadata, blend/stencil e formatos com o backend. As fontes existentes não equivalem a resultados de hardware. |
| V02 | Cobertura de jogos reais no PC e depois Series S: cenas, gameplay prolongado, transições, efeitos, vários formatos/estágios e comparação visual com referência. Chegar à tela inicial não fecha isso. |
| V03 | Runtime UWP/AppContainer real para recursos/compiladores, resizing/DPI, HDR/SDR, apresentação, input e mudanças de estado do painel. Os diagnósticos desktop não exercitam todo esse ambiente. |
| V04 | Medir luminância/gamut/primárias em tela HDR e conferir FP16/PQ contra PS4, inclusive transições e branco SDR. Readback FP16 sozinho não prova a saída física. |
| V05 | Repetir carga/descarga, stop, suspensão/retomada, detach, pressão de memória e falhas de GPU, verificando ownership/threads, estabilidade e memória no PC e Xbox. O host já tem teardown; sua cobertura real não está concluída. |
| V06 | Medir frame time, stalls, memória CPU/GPU e limites por aplicação em sessões prolongadas; ausência de retenção em 96 ciclos sintéticos não prova ausência de todo vazamento. |

A API UWP atualmente impede iniciar outro guest na mesma instância após um boot
(`guest_started`), solicitando reload da DLL. Isso é uma limitação de ciclo de
vida do host, separada da tradução gráfica. O boot brokered e o teardown não
estão ausentes: precisam de cobertura de cenários e eventual ampliação.

## Evidências já disponíveis

Esta seção reúne registros anteriores e diagnósticos executados nas atualizações
P01–P06. Os diagnósticos em `Build/` são artefatos locais temporários, não uma
suíte incluída no repositório nem execução de jogos no Xbox.

| Evidência | Resultado registrado | Limite da conclusão |
| --- | --- | --- |
| `Build/p06-profile-before.log` / `Build/p06-production-cold.log` / `Build/p06-production-warm.log` | PAC-MAN 256: leitura média de programa caiu de 62,7 para 16,9 ms; cache quente com seis hits SPIR-V, cinco blobs PSO e zero chamadas DXC; boot/teardown sem erro. | Logo de startup no harness PC; apresentações oscilaram de 15 a 25 por sessão, sem ganho consistente de throughput nem prova visual/gameplay/Xbox. |
| `Build/p06-sm64-validation.log` | Regressão GCN/SM6.4, aliases, atômicos, feedback, tiling, MSAA e GS/tessellation; zero erros D3D12. | Diagnóstico PC; não substitui a cobertura de jogos/cenas/Xbox. |
| `Build/p06-final-uwp-build.log` / `Build/p06-package-verification.log` | Release x64 compilado, bundle assinado e hashes dos payloads conferidos com o core de produção. | Compilação e pacote; não significa instalação/execução UWP nem validação Xbox. |
| `Build/p06-direct-presenter-validation.log` | 72 comparações CPU/GPU, dois casos de cópia residente direta, HDR/FSR/resize e zero erros D3D12. | Diagnóstico desktop; não mede ganho de gameplay nem valida a saída física HDR/Xbox. |
| `Build/p06-teddy-profile.log` | Falha por backing físico guest não mapeado; encerramento do host concluído. | Sessão inválida para concluir desempenho/compatibilidade. |
| `Build/p05-tiling-validation.log` | 210 casos comparados byte a byte com a referência CPU: arrays parciais, volumes, MSAA 2/8, BC1–BC7, packed, máscaras 15/5/0, backing/padding e dispatch acima de 65535 grupos; upload persistente sem snapshot e sem readback duplicado, zero erros D3D12. | Diagnóstico nativo PC; não mede ganho de desempenho em jogos nem valida hardware Xbox/PS4. |
| `Build/p05-residency-validation.log` | Regressão com memória guest real, argumentos indiretos/índices, ownership, aliases/dirty e quatro ciclos de pressão; zero erros D3D12. | Diagnóstico PC; não comprova gameplay Xbox. |
| `Build/p05-sm64-validation.log` | Regressão GCN/SM6.4, aliases/atômicos/feedback, alternate tiling, MSAA e GS/tessellation; zero erros D3D12. | Runtime nativo PC; não comprova desempenho/gameplay Xbox. |
| `Build/p05-presenter-validation.log` | 72 comparações CPU/residente, 88 capturas sem download e recriação FSR após trim com pixels preservados; zero erros D3D12. | Diagnóstico PC; não é instalação/execução UWP nem validação de suspensão Xbox. |
| `Build/p05-uwp-build.log`, `Build/p05-package-verification.log` | Release x64 compilado, bundle com assinatura CMS válida e executável/seis DLLs iguais aos binários de produção. | Pacote não instalado/executado; Xbox não validado nesta etapa. |
| `Build/p04-validation.log` | 15 cenários de orçamento (processo/dispositivo/RAM/UMA/zero/excesso/overflow), transferência presa por fence e quatro ciclos de pressão preservando imagens/buffers dirty e reutilização; regressões P01–P03 e zero erros D3D12. | Política e pressão induzida em diagnóstico PC; não comprova notificações/suspensão reais no Xbox. |
| `Build/p04-presenter-validation.log` | Seis trims/recriações FSR com pixels preservados, 72 comparações CPU/residente e 88 capturas sem download; zero erros D3D12. | Diagnóstico PC; não é suspensão real de aplicativo UWP. |
| `Build/p04-sm64-runtime.log` | Regressão GCN/SM6.4 com oito coletas induzidas durante os comandos, aliases, atômicos, tiling, MSAA e GS/tessellation; zero erros D3D12 e exit code 0. | Diagnóstico nativo PC; sem comprovação de gameplay ou pressão real UWP/Xbox. |
| `Build/p04-uwp-build.log`, `Build/p04-package-verification.log` | Release x64 compilado, assinatura CMS válida e executável/seis DLLs conferidos contra os binários de produção. | Pacote não instalado/executado; Xbox não validado nesta etapa. |
| `Build/p03-validation.log` | 705 combinações de índices, limite de 65536 grupos, contagem zero/limitada, strides 24/28, base signed/SGPRs, dispatch indireto e argumentos residentes sem readback; zero erros D3D12. | Diagnóstico nativo PC; não comprova gameplay Xbox. |
| `Build/p03-guest-validation.log` | Caminho de produção DrawIndirect com contagem GPU e expansão de strips/provoking, mais regressão de residência; zero erros D3D12. | Não é execução de jogo nem PS4 físico. |
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
| `Build/mixed-adaptations-guest-validation.log` | MIN/MAX, alpha constante nativo/fallback, bypass, depth bounds inativo, NONE, overflow e provoking vertex first/last/restart; coerência, eviction e destruição; zero erros. | Hipóteses guest ainda aguardam comparação em hardware. |
| `Build/mixed-adaptations-presentation-validation.log` | 72 referências CPU/residente, 88 captures sem downloads, FSR/RCAS, HDR/float, tiling e resize; zero erros. | Composição nativa desktop, sem validar scanout HDR ou AppContainer/Xbox. |
| `Build/mixed-adaptations-uwp-build.log` e `Build/mixed-adaptations-package-verification.log` | Release x64, bundle assinado (`CMS_SIGNATURE=PASS`), payload e núcleo de produção coincidentes. | Pacote compilado/assinado não comprova instalação ou runtime. |
| Relato anterior do usuário | Tela inicial atingida no Xbox Series S. | Evidência de boot nesse cenário, sem validação prolongada desta revisão. |

As fontes de comparação ficam em [hardware/d3d12-guest](hardware/d3d12-guest/README.md),
[hardware/logic-ops](hardware/logic-ops/README.md) e
[hardware/msaa](hardware/msaa/README.md). Não foram compiladas ou
executadas em PS4 nesta revisão; o SDK local não foi identificado na integração
anterior.

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
