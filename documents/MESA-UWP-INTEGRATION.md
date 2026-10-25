# Integração das melhorias do Mesa/Dozen

## Origem e escopo

Integração realizada em 23/10/2026 a partir da árvore local
`C:/Users/rodri/Dev1/Projetos/mesa_ai_slop_version`, branch `dozen-update`,
HEAD `82804960ac7`. A série contém 37 commits sobre a base `65b4a553b3f`.
As alterações foram adaptadas à cópia Mesa 26.2.2 em `externals/mesa-uwp`,
preservando os caminhos UWP, os estados dinâmicos existentes, os formatos,
os aliases, a robustez e as extensões adicionais dessa cópia.
A versão de todo o Mesa não foi substituída pela árvore 26.3 de desenvolvimento.

## Melhorias incorporadas

- Barreira legacy por subrecurso/layer, transições de depth/stencil, discard
  ao sair de UNDEFINED, clears de depth e stencil com layouts separados,
  stencil reference em resolves e view instance mask condicionado ao hardware.
- Stride de layers de cópias buffer/imagem calculado com `bufferImageHeight`
  e remoção dos formatos de cor não utilizados no fim da lista de attachments.
- Inline uniform blocks com armazenamento, descritores, update/copy/templates,
  propriedades e recursos anunciados; maintenance4 e consultas de requisitos
  de memória sem criar um objeto Vulkan persistente.
- Memória Vulkan com buffers coerentes marcados globalmente coerentes no DXIL,
  atômicos com atributos de efeitos colaterais e acessos coerentes de 64 bits
  preservados como loads/stores.
- Conversões e operações de subgrupo de 8/16 bits, armazenamento estreito
  em dispositivos sem operações nativas de 16 bits e correção de RANGE_BASE
  ao ampliar loads no NIR, incluindo os ajustes de alinhamento necessários
  à base local.
- Cache/feedback de pipelines, incluindo cache hit sem compilação e preservação
  de erros em lotes; remoção de SSBOs sem uso de descritor no SPIR-V/NIR.
- Fallback de blend com alpha constante, informação de formato/alinhamento,
  propriedades Vulkan e diagnóstico de mensagens D3D12.

Estados dinâmicos estendidos já possuíam implementação própria no destino.
Ela foi preservada, incluindo o gate de stencil independente. Vulkan 1.3 só é
anunciado com SM 6.6, WaveOps e suporte ao stencil independente requerido por
esse caminho local; dispositivos restantes continuam em Vulkan 1.2.
Os controles `DZN_DEBUG`, `DZN_MAX_SHADER_MODEL` e `DZN_MAX_D3D12_OPTIONS`
seguem o mecanismo de opções do Mesa. A configuração UWP não lê variáveis de
ambiente; os diagnósticos que forçam capacidades usam DLLs auxiliares locais,
sem alterar a detecção do binário de produção.

## Validação

- Dozen UWP e ShadPS4-UWP compilados em Release x64; bundle assinado com
  `CMS_SIGNATURE=PASS`. As DLLs do pacote coincidem com o runtime e com as
  saídas de produção (`uwp-build.log` e `package-verification.log`).
- Readback de inline uniform block e feedback de cache na DLL de produção:
  `PIPELINE_CACHE_FEEDBACK=PASS` e quatro DWORDs exatos.
- Readback de armazenamento e aritmética de 8/16 bits sem operações nativas
  de 16 bits, em variantes diagnósticas bindless e descriptor tables:
  `11223344,55667788,00006689,000000bc`.
- Regressão na DLL de produção: compute GCN (`11223344`), MSAA 4× e draw
  indireto (4.096 pixels vermelhos em cada diagnóstico gráfico).

Os fontes, backups, diffs de integração e logs ficam localmente em
`Build/mesa-improvements/`; não são testes adicionados à árvore do Mesa.
Os resultados são de execução nativa no PC. Não comprovam conformidade Vulkan
completa, execução de jogos, AppContainer ou Xbox. Em particular, os novos
casos de barreiras, cópias com padding, subgrupos estreitos e blend não têm
cobertura exaustiva de readback nesta integração.

## Rastreabilidade da série de origem

| Commit | Alteração na origem | Tratamento |
| --- | --- | --- |
| `c85304e1b1f` | microsoft/spirv_to_dxil: Keep dead variable removal in bindless lowering | Integrada/adaptada; comportamento existente preservado |
| `b69cc58da61` | dzn: Support VK_KHR_zero_initialize_workgroup_memory | Integrada/adaptada; comportamento existente preservado |
| `0d6787022d8` | dzn: Support VK_KHR_shader_terminate_invocation | Integrada/adaptada; comportamento existente preservado |
| `e8278ba08e7` | dzn: Support VK_EXT_shader_demote_to_helper_invocation | Integrada/adaptada; comportamento existente preservado |
| `bb96d6c8d35` | dzn: Expose extensions that are already implemented | Integrada/adaptada; comportamento existente preservado |
| `79164d1e9d5` | dzn: Support VK_KHR_format_feature_flags2 | Integrada/adaptada; comportamento existente preservado |
| `aa04da7dfcb` | dzn: Report the texel buffer alignment properties | Integrada/adaptada; comportamento existente preservado |
| `b25ab673062` | dzn: Keep VK_PIPELINE_COMPILE_REQUIRED in batch pipeline creation | Integrada/adaptada; comportamento existente preservado |
| `26a7032f418` | dzn: Support VK_EXT_pipeline_creation_cache_control | Integrada/adaptada; comportamento existente preservado |
| `e344ae79b10` | dzn: Support VK_EXT_pipeline_creation_feedback | Integrada/adaptada; comportamento existente preservado |
| `c75e1116daa` | dzn: Support VK_EXT_image_robustness | Integrada/adaptada; comportamento existente preservado |
| `5e1882916df` | dzn: Support VK_KHR_maintenance4 | Integrada/adaptada; comportamento existente preservado |
| `9dd3eb6a8b7` | microsoft/compiler: Don't declare DXIL atomics as readnone | Integrada/adaptada; comportamento existente preservado |
| `13dc477a668` | microsoft/compiler: Declare coherent storage buffers globally coherent | Integrada/adaptada; comportamento existente preservado |
| `f0772952154` | microsoft/compiler: Keep 64-bit coherent accesses as loads and stores | Integrada/adaptada; comportamento existente preservado |
| `451ddab305f` | dzn: Support VK_KHR_vulkan_memory_model | Integrada/adaptada; comportamento existente preservado |
| `5d6242f51ee` | dzn: Support VK_EXT_inline_uniform_block | Integrada/adaptada; comportamento existente preservado |
| `ce02589e032` | dzn: Discard when leaving the undefined layout in image barriers | Integrada/adaptada; comportamento existente preservado |
| `809545170ef` | dzn: Support VK_EXT_extended_dynamic_state and extended_dynamic_state2 | Implementação local existente preservada |
| `abccb5cbb43` | dzn: Report Vulkan 1.3 on shader model 6.6 devices with wave operations | Integrada/adaptada; comportamento existente preservado |
| `7d299b7b215` | microsoft/compiler: Only put the wave size into PSV0 for shader model 6.6+ | Integrada/adaptada; comportamento existente preservado |
| `199b69a24cb` | dzn: Fix legacy transition barriers for depth/stencil and layer ranges | Integrada/adaptada; comportamento existente preservado |
| `e719904a51d` | dzn: Clear depth and stencil separately when their layouts differ | Integrada/adaptada; comportamento existente preservado |
| `fa10860fa51` | dzn: Set the stencil reference on the base command list in resolves | Integrada/adaptada; comportamento existente preservado |
| `01cdeec0fae` | dzn: Emulate constant alpha blend factors without AlphaBlendFactorSupported | Integrada/adaptada; comportamento existente preservado |
| `1f8f581d435` | dzn: Add switches that hide D3D12 capabilities for testing | Integrada/adaptada; comportamento existente preservado |
| `814be07de86` | vulkan/runtime: Mark a query result only checked by an assertion | Não aplicável: vk_frame_pacer.c não existe nesta base |
| `77a0d7dbabb` | dzn: Only reset the view instance mask on devices with view instancing | Integrada/adaptada; comportamento existente preservado |
| `cc9399eb378` | dzn: Log D3D12 debug layer errors with DZN_DEBUG=d3d12 | Integrada/adaptada; comportamento existente preservado |
| `29cde45d961` | microsoft/spirv_to_dxil: Remove storage buffers without descriptor uses | Integrada/adaptada; comportamento existente preservado |
| `7a72a168248` | dzn: Use bufferImageHeight for the layer stride of buffer/image copies | Integrada/adaptada; comportamento existente preservado |
| `af770e1ea18` | dzn: Omit trailing unused color attachment formats | Integrada/adaptada; comportamento existente preservado |
| `41e22bcbf41` | nir/lower_mem_access_bit_sizes: Handle RANGE_BASE when widening loads | Integrada/adaptada; comportamento existente preservado |
| `edf08be5a68` | microsoft/compiler: Narrow with u2u in the 8-bit and 16-bit cast lowering | Integrada/adaptada; comportamento existente preservado |
| `66251b6ad2e` | microsoft/compiler: Clear support_16bit_alu without 16-bit sizes | Integrada/adaptada; comportamento existente preservado |
| `7afd6b1073c` | microsoft/compiler: Lower narrow subgroup operations to a supported size | Integrada/adaptada; comportamento existente preservado |
| `82804960ac7` | dzn: Support 8-bit and 16-bit integers and storage without native 16-bit ops | Integrada/adaptada; comportamento existente preservado |
## Correção adicional de coerência — 24/10/2026

`dxil_nir_lower_coherent_loads_and_stores` promove o recurso para acesso UAV
quando substitui uma leitura/escrita por operação atômica. A classificação de
somente leitura é removida tanto da instrução quanto das variáveis correspondentes;
`nir_chase_binding` preserva a identificação de binding e descriptor set. A correção
evita DXIL com operações atômicas sobre handles SRV, sem eliminar a coerência.

O descritor de imagem também registra o formato e os usos quando uma escrita
solicita uma view sem UAV preparado. A correção correspondente no núcleo considera
views compatíveis ao escolher os usos da imagem. Validação e evidências de
inicialização estão em [UWP-GAME-STARTUP.md](UWP-GAME-STARTUP.md).


## PerVertexKHR e cópia de imagem para buffer — 24/10/2026

A compilação DXIL de entradas por vértice usa assinatura `nointerpolation` e
índices i8 imediatos em `attributeAtVertex`; para índice dinâmico, seleciona as
leituras válidas de cada vértice, sem criar aritmética i8 rejeitada pelo validador.
A cópia de imagem para buffer prepara e encerra o acesso de escrita do destino
nas duas implementações de barreiras. A execução final no PC chegou ao
personagem controlável e passou por uma janela de movimento de 8,1 segundos,
com zero mensagens D3D12. Evidências e limites estão em
[UWP-GAME-STARTUP.md](UWP-GAME-STARTUP.md).


## Maintenance6 e profundidade irrestrita (24/10/2026)

`VK_KHR_maintenance6` foi implementada no Dozen com feature `maintenance6`,
propriedades e os quatro comandos `*2KHR` de descritores/push constants.
Bindings com estágios gráficos e compute atualizam os dois pontos de binding.
Os comandos antigos e novos de index buffer aceitam `VK_NULL_HANDLE`, sem
acessar um objeto nulo; com `nullDescriptor`, a leitura nativa retorna índice zero.
`vkBindBufferMemory2` e `vkBindImageMemory2` preenchem `VkBindMemoryStatusKHR`
por operação e continuam o lote após uma falha, retornando um dos erros do lote.
As propriedades são `maxCombinedImageSamplerDescriptorCount=1`,
`blockTexelViewCompatibleMultipleLayers=false` e
`fragmentShadingRateClampCombinerInputs=false`, de acordo com os recursos
atualmente oferecidos pelo driver.

Validação nativa no PC com a DLL UWP compilada, RX 6600M e debug D3D12:
readbacks corretos nos quatro comandos novos, bindings gráficos/compute na mesma
chamada, push descriptors diretos e por template, índices nulos de 8/16/32 bits
com `firstIndex=23` e `vertexOffset=7`, e status de memória de buffer/imagem.
Uma falha de criação D3D12 foi injetada apenas no processo de diagnóstico:
lotes de três buffers e de três imagens retornaram `VK_ERROR_OUT_OF_DEVICE_MEMORY`,
cada um com resultados individuais `[-2, 0, 0]` e três tentativas. Nenhum hook de diagnóstico faz parte
da DLL. Evidência: `Build/maintenance6/debug-validation.log`.
Essa validação não constitui execução no Xbox nem certificação de conformidade.

`VK_EXT_depth_range_unrestricted` continua pendente e não é anunciada.
A extensão exige min/max de viewport, depth bounds e clears fora de `[0,1]`.
O caminho nativo D3D12 restringe o viewport a esse intervalo e aplica clamp
à profundidade final, inclusive quando o fragment shader escreve `SV_Depth`.
Uma sonda D3D12 na RX 6600M gravou `-2` e `2` em D32_FLOAT e leu `0` e `1`
(`Build/maintenance6/native-depth.log`). Portanto, acrescentar a extensão à
lista ou remapear apenas o viewport não implementa seu contrato. Falta um
caminho alternativo de depth/stencil que preserve esses valores, seus testes,
MSAA, clears e a coerência com cópias/amostragem antes de habilitar a extensão.

Referências: [maintenance6](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance6.html),
[depth range unrestricted](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_depth_range_unrestricted.html),
[viewport D3D12](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_viewport)
e [clamp no Output Merger, seção 17.10](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#17.10%20Depth%20Clamp).


## Extensões AMD de parâmetros de vértice e imagem com LOD (24/10/2026)

- `VK_AMD_shader_explicit_vertex_parameter`: anunciada quando o dispositivo
  oferece barycentrics D3D12 e Shader Model 6.1 ou superior. O frontend SPIR-V
  habilita a instrução AMD; `ExplicitInterpAMD` usa uma assinatura sem
  interpolação e `load_input_vertex` é emitida como `AttributeAtVertex`,
  preservando valores float e os bits dos inteiros. As coordenadas AMD usam
  `(y,z)` dos pesos D3D12, mantendo o vínculo com os vértices 1 e 2.
  Centro, centroid, sample e pull model são traduzidos no NIR. Centroid/sample
  são calculados a partir dos planos de interpolação e das posições das amostras,
  sem emitir operações `eval*` sobre a semântica especial `SV_Barycentrics`.
  A avaliação dos planos é movida para a entrada do shader, e o inverso de W
  do centro é preservado também na execução por amostra.
- `VK_AMD_shader_image_load_store_lod`: cada descritor de storage image reserva
  um banco de 16 descritores, cobrindo os níveis de mip suportados pelo D3D12.
  O NIR seleciona o descritor do LOD relativo ao mip base da view. Leituras,
  escritas e consultas usam a view do nível selecionado; as operações comuns
  e os atômicos continuam selecionando o nível zero da view. Imagens de texel
  buffer não recebem esse banco. Os caminhos SRV/UAV e bindless tratam os mips,
  arrays, cubemaps e a profundidade reduzida de cada mip 3D. Updates, templates,
  cópias, push descriptors, contagens variáveis e hashes de layouts preservam
  o stride físico sem alterar as contagens Vulkan de descritores.
  Índices dinâmicos de recursos são emitidos como potencialmente não uniformes,
  inclusive quando a divergência vem do LOD em vez de uma decoração SPIR-V.
  Os limites de storage images consideram o custo físico dos bancos; o custo
  adicional é aplicado apenas nos dispositivos que habilitam essa extensão.

Validação no PC com as DLLs UWP compiladas e RX 6600M:

- Readback de triângulo com W diferentes nos vértices, parâmetros float/uint,
  todos os sete builtins AMD e MSAA 1×/4×: 4096 pixels corretos e zero erros
  do debug D3D12 (`Build/amd-extensions/vertex.log` e `vertex-msaa.log`).
- Imagens 1D, 2D, 3D, arrays, cubemap e cubemap array, mip base 1 e LODs 0/1/2
  divergentes dentro do mesmo workgroup. Leituras e cópias dos mips físicos
  conferidas para escrita/leitura e readonly, com descritores tradicionais e
  bindless; arrays de descritores indexados dinamicamente, templates e cópias
  de sets incluídos. Todos os resultados e o debug D3D12 passaram
  (`Build/amd-extensions/image-final.log`).
- Com robustness2, LODs negativos/excessivos e coordenadas fora do mip retornam
  os componentes padrão esperados. A regressão sem as novas extensões passou
  nos 81 casos de imagens de cada uma das três fases de robustness2, e nos
  seis cenários de maintenance6 (`robust2-regression.log` e
  `maintenance6-regression.log`, na mesma pasta).

- PAC-MAN 256 executado durante 25 segundos com o core UWP e o Dozen atualizado:
  `PRESENT_COUNT=1070`, `D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`,
  `SHUTDOWN=0` e saída do processo zero. Evidência:
  `Build/amd-extensions/pacman-regression.log` e `pacman-regression.png`.

Os resultados acima são sondas nativas no PC usando as DLLs UWP. Não representam
validação no Xbox nem certificação de conformidade Vulkan.

Contratos consultados: [SPIR-V AMD explicit vertex parameter](https://github.khronos.org/SPIRV-Registry/extensions/AMD/SPV_AMD_shader_explicit_vertex_parameter.html),
[AMD image load/store LOD](https://registry.khronos.org/OpenGL/extensions/AMD/AMD_shader_image_load_store_lod.txt)
e [barycentrics D3D12](https://github.com/microsoft/DirectXShaderCompiler/wiki/SV_Barycentrics).

## Maintenance8 e layout de feedback de attachments (25/10/2026)

`VK_KHR_maintenance8` e a feature `maintenance8` são anunciadas quando o
adaptador oferece Shader Model 6.7, Advanced Texture Ops e um validador DXIL
compatível. Isso permite offsets dinâmicos nativos sem alterar as derivadas
ou a escolha de LOD das amostragens. Dispositivos sem esses requisitos não
recebem a extensão. Não se presume esse suporte no Xbox.

O caminho existente de cópias preserva os bits entre formatos compatíveis de
cor e depth/stencil; as barreiras de transferência entre filas tratam
`QUEUE_FAMILY_OWNERSHIP_TRANSFER_USE_ALL_STAGES`. O compilador distingue
`OpSRem` de `OpSMod`, inclusive com operandos negativos, e os blits 3D usam
a profundidade separadamente da contagem de layers. O runtime aceita a cadeia
`VkMemoryBarrierAccessFlags3KHR`; a revisão atual não define bits de acesso
adicionais. Os merges de pipeline caches agora adquirem os dois locks em
ordem estável, evitando deadlock em merges concorrentes nos dois sentidos,
inclusive com `INTERNALLY_SYNCHRONIZED_MERGE`.

`VK_EXT_attachment_feedback_loop_layout` e sua feature estão disponíveis por
padrão após a conclusão descrita na seção final. Ela acrescenta um caminho de leitura
por SRV sobre snapshots internos dos attachments em feedback. Antes de cada
draw, as subresources lidas são copiadas com transições sincronizadas; os
RTVs/DSVs continuam escrevendo na imagem original. Isso evita vincular a
mesma subresource simultaneamente para leitura e escrita no D3D12. Cor,
profundidade, stencil e MSAA preservam o formato, as views e os planos da
imagem. Metadados por descritor identificam imagens sem reter ponteiros de
views destruídas; cópias de descritores também copiam esses metadados.
Os snapshots e slots bindless pertencem ao command buffer, são liberados
no reset e voltam ao layout inicial no encerramento para permitir replay.

A limitação original de descritores resolvidos somente durante a gravação
foi corrigida pela associação de cada draw aos seus snapshots e pela
propagação de atualizações tardias. Dispositivos com a feature de feedback
habilitada usam tabelas clássicas atualizáveis, inclusive com descriptor
indexing. Dispositivos sem essa feature preservam a seleção bindless.

Validação nativa no PC, RX 6600M, com as DLLs UWP:

- Maintenance8: offsets dinâmicos em fetch e sample, `SRem`/`SMod` negativos,
  4.000 merges concorrentes e consultas simultâneas de cache; zero falhas.
- Cópia `R32_SFLOAT → D32_SFLOAT → R32_SFLOAT`: 16 padrões preservados bit a
  bit, incluindo negativos, NaN, infinitos, subnormais e zero negativo.
- Feedback de cor, depth D32 e stencil D24/S8 nos caminhos clássico e
  bindless: 4.096 pixels por caso com resultado esperado. Cor também passa
  com MSAA 4×. Todos os casos executam novamente o mesmo command buffer.
- As sondas acima retornaram `SDK_ERRORS=0`. Evidências em
  `Build/maintenance8-feedback/{maintenance8,feedback,depth-feedback,msaa-feedback}.log`.
- Regressões de imagem com LOD, robustness2 (81 casos em três fases) e
  maintenance6 (seis cenários) passaram, com logs na mesma pasta.

PAC-MAN 256 permaneceu 25 segundos na tela inicial, com 1.191 presents,
`D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`, `SHUTDOWN=0` e saída zero
(`Build/maintenance8-feedback/pacman-regression.log` e `pacman-regression.png`).

Sondas no PC não substituem validação no Xbox ou certificação Vulkan.

Contratos: [maintenance8](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_maintenance8.html),
[attachment feedback loop](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_attachment_feedback_loop_layout.html)
e [barreiras D3D12](https://microsoft.github.io/DirectX-Specs/d3d/D3D12EnhancedBarriers.html).

O pacote Release foi recompilado com as DLLs atualizadas. A assinatura CMS e
os hashes dos arquivos incluídos são verificados por
`Build/maintenance8-feedback/sign.log` e `package-verify.log`. O pacote não
foi instalado nem validado no Xbox nesta tarefa.

## Barycentric KHR e mixed attachment samples AMD (25/10/2026)

`VK_KHR_fragment_shader_barycentric` já era exposta pelo Dozen, mas o caminho
centroid/sample/offset ainda emitia operações DXIL `eval*` sobre
`SV_Barycentrics`. Esses usos não são confiáveis no driver AMD testado.
O lowering agora calcula os pesos solicitados a partir dos planos dos pesos
no centro do pixel. As derivadas e os planos são calculados na entrada do
shader; o offset ou índice de amostra pode continuar dinâmico no ponto de
uso. Pesos com perspectiva preservam o inverso de W, incluindo execução por
amostra. `PerVertexKHR` continua usando `AttributeAtVertex`, inclusive com
índices dinâmicos e atributos inteiros. A extensão e a feature exigem
barycentrics D3D12, Shader Model 6.1 e validador DXIL compatível. A versão do
layout/cache do runtime foi incrementada para invalidar shaders anteriores.

`VK_AMD_mixed_attachment_samples` está disponível por padrão após a conclusão
descrita na seção final. Attachments de cor
com menos amostras que a rasterização recebem backing interno com a mesma
contagem do depth/stencil. Antes do draw, os valores de cor existentes são
carregados nesse backing. Depois do draw, somente as primeiras N amostras
são copiadas de volta à imagem de cor com N amostras; não é uma resolução
por média. Blits internos distinguem as contagens de origem e destino e
limitam seus índices de amostra ao intervalo da origem.

O pipeline D3D12 mantém a contagem de rasterização e o depth/stencil original,
preservando os testes e as escritas de todas as suas amostras. As cópias são
sincronizadas e os backings são retidos pelo command buffer até reset/destruição,
com restauração de layout no encerramento para permitir reenvio. O mesmo
caminho atende render passes e dynamic rendering com
`VkAttachmentSampleCountInfoAMD`, além dos draws diretos e indiretos.

As pendências de slices de imagens 3D e consultas ativas foram resolvidas
na conclusão descrita na seção final. Essas combinações passaram a executar
sem a rejeição `VK_ERROR_FEATURE_NOT_PRESENT`. O caminho acrescenta cópias e
draws internos por draw do aplicativo.

Sondas nativas no PC, RX 6600M, usando as DLLs UWP:

- Barycentric: seis cenários, combinando 1×/4× com pesos pixel, Sample e
  Centroid. Incluem perspectiva, no-perspective, `PerVertexKHR` float/integer,
  índices dinâmicos e `interpolateAtOffset/Sample/Centroid`. Cada cenário
  verificou 4.096 pixels; todos passaram com `SDK_ERRORS=0`.
- Mixed samples: cor 1×/2× com depth 4×, render pass/dynamic rendering e
  descritores clássicos/bindless. O segundo draw foi rejeitado pelo depth
  test, preservando a primeira cor. Uma sonda compute leu os quatro valores
  de profundidade (0,2; 0,3; 0,4; 0,5), inclusive as amostras sem saída de cor.
  Os oito cenários passaram, também no reenvio do command buffer, sem erros
  D3D12. As combinações com consulta ativa tiveram rejeição explícita validada.

Evidências: `Build/mixed-barycentric/bary.log`, `bary-single.log`, `mixed.log`,
`mixed-dynamic.log` e `mixed-query.log`. São validações nativas no PC, não
conformidade Vulkan, AppContainer instalado ou resultados no Xbox.

Contratos: [AMD mixed attachment samples](https://docs.vulkan.org/refpages/latest/refpages/source/VK_AMD_mixed_attachment_samples.html)
e [KHR fragment shader barycentric](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_fragment_shader_barycentric.html).

Uma sonda adicional de barycentrics em frequência de pixel com MSAA 4×
validou centroid e índice de amostra dinâmico sem depender de `SampleId`
(`bary-pixel.log`). Mixed samples também preservou o padrão inteiro
`0x80000000` em `R32_UINT`, com cor 1×/2× e depth 4× (`mixed-uint.log`).
As regressões AMD explicit vertex parameter, feedback MSAA 4× e os seis
cenários maintenance6 passaram (`*-regression.log` na mesma pasta).

PAC-MAN 256 permaneceu na tela inicial durante 25 segundos com o Dozen
atualizado: `PRESENT_COUNT=1109`, `D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`,
`SHUTDOWN=0` e saída zero. Evidências: `pacman-regression.log` e
`pacman-regression.png`. O bundle Release foi recompilado e assinado; a
assinatura CMS e a igualdade dos payloads com as DLLs de Runtime são
verificadas em `sign.log` e `package-verify.log`, na mesma pasta. Não houve
instalação do pacote nem execução no Xbox nesta tarefa.

## Extended dynamic state e shader stencil export (25/10/2026)

As duas extensões já possuíam implementação no estado local do Dozen. Esta
revisão confirma os caminhos existentes e alinha a lista de capacidades do
frontend SPIR-V compartilhado com a exportação de stencil já suportada no
NIR/DXIL. No caminho Vulkan, o runtime obtém as capacidades do dispositivo;
portanto, esse ajuste não substitui a detecção de suporte da GPU.

- `VK_EXT_extended_dynamic_state`: a extensão e a feature são anunciadas
  quando há suporte D3D12 a referências e máscaras de stencil independentes
  para front/back. Os comandos EXT e seus aliases core 1.3 atualizam cull,
  front face, topologia, viewport/scissor com contagem, stride de vértices,
  habilitação/comparação/escrita de depth, depth bounds e operações de stencil.
  Estados que exigem PSO são aplicados às variantes existentes, sem duplicar
  a implementação ou criar exceções por jogo.
- `VK_EXT_shader_stencil_export`: a exposição continua condicionada a
  `PSSpecifiedStencilRefSupported`. `FragStencilRefEXT` é emitido como
  `SV_StencilRef`; o valor do fragment shader participa da comparação e das
  operações de escrita de stencil. A capacidade `StencilExportEXT` agora
  também está declarada no frontend do compilador compartilhado.

Validação nativa no PC, RX 6600M, com as DLLs UWP:

- Referência de stencil variável por pixel e maior que 255, verificada após
  truncamento aos oito bits do attachment. Os 4.096 texels corresponderam ao
  padrão esperado, independentemente da referência fixa configurada.
- Alteração da comparação e máscara de stencil entre draws: comparação
  exata rejeita valores diferentes; máscara `0xfe` ignora apenas o bit zero.
- Oito estados/passagens verificam habilitação de stencil/depth, depth
  compare `NEVER/ALWAYS/LESS`, depth write desativado, depth bounds e cull
  front+back. A cor final foi `R=191`, `G=0` nos 4.096 pixels.
- Strides de vértices 16 e 8, com offsets distintos, aplicados antes e depois
  do bind do pipeline. Atributos de posição são realmente lidos do buffer.
- A mesma sequência passou usando os comandos core Vulkan 1.3.
- Em MSAA 4×, a leitura compute confirmou stencil por amostra `0,16,32,48`.
  A cor final e o reenvio do command buffer também passaram. Todas as sondas
  retornaram `STENCIL_EXPORT_ERRORS=0` e `SDK_ERRORS=0`.

Evidências em `Build/dynamic-stencil/`: `baseline.log`, `stride-baseline.log`,
`states.log`, `core.log` e `msaa.log`. São sondas nativas no PC; não são
certificação Vulkan ou validação no Xbox.

Referências: [extended dynamic state](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_extended_dynamic_state.html),
[shader stencil export](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_shader_stencil_export.html)
e [SPIR-V stencil export](https://github.khronos.org/SPIRV-Registry/extensions/EXT/SPV_EXT_shader_stencil_export.html).

PAC-MAN 256 permaneceu na tela inicial durante 25 segundos, com 1.176
presents, `D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`, `SHUTDOWN=0` e saída
zero (`Build/dynamic-stencil/pacman-regression.log` e `pacman-regression.png`).
O bundle Release foi recompilado e assinado. `sign.log` confirma
`CMS_SIGNATURE=PASS`, e `package-verify.log` confirma `PAYLOAD_HASHES=PASS`
para as DLLs atualizadas. Não houve instalação nem execução no Xbox.

## Views 3D e descriptor indexing (25/10/2026)

`VK_IMAGE_VIEW_TYPE_3D` é um tipo de view do Vulkan core. O Dozen já criava
SRVs/UAVs 3D; esta revisão corrige casos que falhavam quando as views eram
combinadas com slices e arrays de descritores.

- Storage images 3D permanecem em UAV também para leituras somente leitura.
  A conversão anterior para SRV perdia `FirstWSlice`/`WSize`: uma view de
  profundidade 1 iniciando no slice 1 era lida como um volume de profundidade
  2, começando no slice 0. O shader agora observa o offset e a profundidade
  efetivos da storage view. Views amostradas continuam usando SRV e ignoram
  corretamente o recorte de slices, como exigido pelo Vulkan.
- Arrays de descritores sem tamanho declarado são dimensionados conforme o
  layout no caminho clássico antes de converter seus derefs. Isso elimina
  a falha de compilação observada com um array runtime de images 3D readonly.
- Ranges de descritores parcialmente preenchidos ou atualizáveis recebem
  flags D3D12 de volatilidade. Slots não acessados podem permanecer sem
  inicialização, preservando o contrato de `PARTIALLY_BOUND`.
- `vkGetDescriptorSetLayoutSupport` não dereferencia mais a estrutura
  opcional de contagem variável quando ela está ausente. Na alocação,
  `pDescriptorCounts` passa a ser ignorado nos layouts sem binding variável,
  evitando consumo incorreto do pool e `VK_ERROR_OUT_OF_POOL_MEMORY`.
- A detecção de descriptor indexing e o uso de heaps diretamente indexados
  agora exigem SM 6.6, Resource Binding Tier 3 e validador DXIL compatível.
  Os anúncios EXT/core e buffer device address usam o mesmo critério.
  A versão do cache/layout do runtime foi incrementada para 10.

Validação nativa no PC, RX 6600M, usando as DLLs UWP:

- Seis volumes `R32_UINT`, três mips e views começando no mip 1. Leituras por
  `texelFetch`/`textureLod`, consultas de tamanho e escritas no eixo Z
  retornaram os valores e as dimensões esperados.
- Arrays runtime com índices divergentes por lane, contagem variável menor
  que o máximo e slots parcialmente preenchidos. Templates aplicados após a
  gravação e cópias de descritores antes do reenvio foram observados pela GPU.
- Um descritor não utilizado foi atualizado enquanto o envio esperava uma
  timeline semaphore; após o sinal, os resultados permaneceram corretos.
  Samplers distintos, clamp/repeat no eixo Z, também foram selecionados e
  atualizados corretamente por índices não uniformes.
- No caminho clássico, uma view readonly com slice offset 1/count 1 leu o
  segundo slice e reportou profundidade 1. Arrays runtime e slots não usados
  passaram sem erros D3D12. A alocação incluiu deliberadamente contagens
  variáveis em layouts fixos para verificar que fossem ignoradas.

Evidências em `Build/view3d-indexing/indexing.log` e `sliced.log`:
`ERRORS=0`, `SDK_ERRORS=0`. As regressões de imagem com LOD (incluindo 3D,
SRV/UAV e classic/bindless), robustness2 (81 casos em três fases) e
maintenance6 (seis cenários) também passaram, com logs na mesma pasta.

O caminho bindless e as tabelas clássicas com robustness/null descriptors
agora propagam atualizações `UPDATE_AFTER_BIND`, conforme detalhado abaixo.
A interação com snapshots de attachment feedback foi concluída na seção
final. Os resultados não constituem certificação da extensão ou validação
no Xbox.

Referências: [descriptor indexing](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_descriptor_indexing.html),
[views 3D](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewType.html),
[slices de views 3D](https://docs.vulkan.org/refpages/latest/refpages/source/VkImageViewSlicedCreateInfoEXT.html)
e [heaps dinâmicos SM 6.6](https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_DynamicResources.html).

PAC-MAN 256 permaneceu na tela inicial por 25 segundos: 1.204 presents,
`D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`, `SHUTDOWN=0` e saída zero.
Evidências em `Build/view3d-indexing/pacman-regression.log` e
`pacman-regression.png`. O bundle Release foi recompilado, assinado e
verificado: `CMS_SIGNATURE=PASS` em `sign.log` e `PAYLOAD_HASHES=PASS` em
`package-verify.log`. O pacote não foi instalado nem testado no Xbox.


## Custom border color e UPDATE_AFTER_BIND clássico (2026-10-25)

`VK_EXT_custom_border_color` usa samplers nativos D3D12 com border color
float ou inteiro. A exposição exige `ID3D12Device11`, Shader Model 6.7,
Advanced Texture Operations e um validador DXIL compatível. Os caminhos com
formato explícito e `customBorderColorWithoutFormat` estão disponíveis
somente quando essas capacidades existem.

As tabelas clássicas agora registram os intervalos atualizáveis de cada set
copiado para os heaps do command buffer. Escritas diretas, templates e
cópias propagam os descritores modificados para todas as cópias gravadas,
inclusive no reenvio do mesmo command buffer. Os intervalos SRV/UAV
alternativos e os metadados usados por robustness2/null descriptors também
são atualizados. O acesso aos heaps e aos registros é sincronizado; reset,
liberação e destruição removem os vínculos antes de liberar seus recursos.

Sets clássicos com contagem variável mantêm heaps CPU próprios com os offsets
do layout máximo, mas consomem a quota do pool pela contagem alocada. O limite
reportado considera os intervalos SRV/UAV adicionais e todas as bindings,
inclusive quando a entrada variável não é a última no array de criação.
Os handles DXIL de buffers com índice dinâmico preservam a seleção não uniforme.
A versão interna dos layouts/cache foi incrementada para 11.

Validação na Radeon RX 6600M, usando a DLL UWP e sondas locais em
`Build/classic-uab/`:

- Border colors float, uint e sint, com formato explícito e indefinido,
  troca tardia de sampler e cópia antes do reenvio. O caso float verificou
  valores negativos, maiores que um e a mistura linear na fronteira.
- Imagens 3D, mips, samplers divergentes e storage images: templates após
  gravação, cópias antes do reenvio e atualização de slot não usado enquanto
  a submissão aguardava um semáforo timeline.
- Uniform/storage buffers e uniform/storage texel buffers, com contagem fixa
  ou variável e descritores válidos ou nulos. Leituras fora dos limites,
  tamanho dos buffers, componentes ausentes e descarte de escritas inválidas
  foram conferidos por readback.
- Troca de imagem válida para nula e restauração, incluindo atualização dos
  metadados de componentes default usados por robustness2.
- Atualizações concorrentes em slots distintos e 40 ciclos por cenário de
  alocação, liberação/reset de pool e reset de command buffer invalidado.

Esses cenários terminaram com `ERRORS=0`, `SDK_ERRORS=0` e
`THREAD_AND_LIFETIME=PASS`, conforme `result.log`, `variable.log`, `null.log`,
`buffer.log`, `texel.log`, `lifetime.log` e `border-*.log`.
As regressões de LOD (SRV/UAV e classic/bindless), robustness2 (81 casos em
três fases), maintenance6 (seis cenários) e indexing bindless também passaram.
A combinação com attachment feedback foi validada separadamente na seção
seguinte.

PAC-MAN 256 permaneceu na tela inicial por 25 segundos, com 1.132 presents,
`D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`, `SHUTDOWN=0` e saída zero.
A captura `Build/classic-uab/pacman-regression.png` foi conferida visualmente.
O bundle Release foi recompilado e assinado: `CMS_SIGNATURE=PASS`,
`PAYLOAD_HASHES=PASS` e `SOURCE_WHITESPACE=PASS`. As DLLs verificadas no
pacote têm SHA-256:

- `vulkan_dzn.dll`: `7fd76a67359320962d4a4723d5c675f89bec97e637f00e0ab79b842a35990c54`.
- `spirv_to_dxil.dll`: `69a4fba9cf2c704c0fc53f19e15d1acf04c382b2f6bc9a3901be0964dca64aa4`.

Logs e evidências em `Build/classic-uab/`. O pacote não foi instalado;
a execução do jogo usou o harness do core no PC e não valida o Xbox.


## Conclusão de mixed samples e attachment feedback (2026-10-25)

`VK_AMD_mixed_attachment_samples` e `VK_EXT_attachment_feedback_loop_layout`
agora são anunciadas por padrão. Não dependem de variáveis de ambiente.
A versão interna de layouts/cache passou para 12.

Mixed samples passou a aceitar attachments que sejam slices de volumes 3D.
O backing MSAA é um array 2D; os blits convertem os slices Z do volume para
as camadas desse array e fazem o caminho inverso ao gravar as primeiras N
amostras. As barreiras de volumes 3D tratam cada mip como uma subresource,
sem converter slices Z em camadas nativas inexistentes.

As consultas ativas continuam medindo os draws do aplicativo. Cada blit
interno mede sua própria contribuição em heaps privados. Na coleta, um
shader de compute subtrai essas contribuições dos contadores de 64 bits,
com tratamento de borrow sem depender de shaderInt64. A própria redução
também é excluída de outras consultas que estejam ativas. Consultas de
oclusão sem PRECISE usam contagem precisa internamente nesse caminho.
Os recursos e heaps privados são retidos pelo command buffer e liberados
no reset/destruição. Push constants são restauradas após mudanças de root
signature feitas pelos passes internos.

Feedback passou a preparar os snapshots de todos os attachments envolvidos
antes de cada draw e guardar um escopo imutável de redirecionamento para
cada tabela gravada. Templates, escritas e cópias `UPDATE_AFTER_BIND`
aplicam esse escopo ao atualizar os heaps GPU, preservando samplers,
formatos, metadados de robustness2 e descritores nulos. Uma atualização
para uma imagem fora do escopo ou para um descritor nulo restaura o
respectivo descritor original. Reset e destruição removem os observadores
antes dos escopos e snapshots.

A feature de feedback seleciona o caminho clássico, inclusive quando
descriptor indexing é habilitado. Isso mantém as atualizações tardias no
mesmo mecanismo sincronizado usado por robustness/null descriptors e
substitui a cópia antiga de tabelas bindless congelada na gravação.
O caminho também cobre attachments em `GENERAL`, preservando seu layout
original e inicializando corretamente recursos RT/DS antes de utilizá-los.

Sondas locais em `Build/attachment-completion/`, na RX 6600M com a DLL UWP:

- Cor 1×/2×, depth 4×, consultas de oclusão precisas e não precisas:
  16.384 amostras. Estatísticas: 6 vértices, 2 primitivas, 6 invocações de VS,
  32.768 de PS e zero de CS, excluindo os passes internos.
- Cópia do resultado de oclusão para buffer enquanto a consulta de
  estatísticas permanecia ativa: 16.384 na GPU e nenhuma invocação de CS
  atribuída ao aplicativo. Os mesmos command buffers foram reenviados.
- Slice Z=1 de volume 3D com cor 1× e depth 4×, tanto em render pass quanto
  dynamic rendering. Oclusão de 16.384 e valores de depth 0,2/0,3/0,4/0,5
  preservados nas quatro amostras.
- Feedback tardio de cor, depth D32, stencil D24/S8, MSAA 4× e volume 3D:
  imagem válida, imagem nula e restauração por cópia, usando templates após
  gravação e o mesmo command buffer em todas as fases.
- Feedback tardio em dynamic rendering, com layouts de feedback e `GENERAL`.
- Mixed samples e feedback usados juntos, com cor 1×/2×, depth 4×,
  robustness2, descritores nulos e atualizações tardias nas três fases.

Os readbacks acima conferiram 4.096 pixels por fase, com `ERRORS=0` e
`SDK_ERRORS=0`. Evidências: `stats.log`, `copy-query.log`, `3d.log`,
`3d-dynamic.log`, `late-*.log` e `mixed-feedback.log`.
Os rótulos antigos `FEEDBACK_BINDLESS` das sondas indicam a configuração
solicitada de descriptor indexing; com feedback habilitado, a execução
interna agora usa tabelas clássicas.

As regressões de UPDATE_AFTER_BIND (buffers, texel buffers e ciclo de vida),
LOD de imagens, robustness2 (81 casos em três fases), maintenance6 e
indexing bindless passaram com a DLL final. Os logs estão na mesma pasta.
PAC-MAN 256 permaneceu na tela inicial durante 25 segundos, com 1.206
presents, `D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`, `SHUTDOWN=0` e saída
zero. A captura `pacman-regression.png` foi conferida visualmente.

O bundle Release foi recompilado, assinado e verificado:
`CMS_SIGNATURE=PASS`, `PAYLOAD_HASHES=PASS` e `SOURCE_WHITESPACE=PASS`.
O SHA-256 de `vulkan_dzn.dll` no pacote é
`5bc1e5fe71fd0a9df0eadaa0928ca711b89aa15dc29395b0f09786dcdf0677fd`.
A validação usou sondas GPU e o harness do core no PC. O pacote não foi
instalado nem executado no Xbox; esses resultados não constituem certificação
Vulkan nem comprovam gameplay dos títulos que usam essas extensões.

## GCN shader e StorageReadWithoutFormat (2026-10-25)

`VK_AMD_gcn_shader` passou a ser exposto pelo Dozen. `CubeFaceIndexAMD`
e `CubeFaceCoordAMD` são traduzidos para ALU DXIL, incluindo sinais,
as seis faces e prioridade dos eixos nos empates. A divisão usa o módulo
do eixo principal também na avaliação de constantes. A tradução é
compartilhada com `spirv_to_dxil.dll`.

`TimeAMD` usa o contador de ciclos DXIL. A assinatura de
`dx.op.cycleCounterLegacy` foi corrigida para retornar `dx.types.twoi32`,
sem overload. Como a metade superior retornada pelo driver testado não é
confiável, o caminho AMD estende a metade inferior com estado por invocação,
convertido para SSA depois do inlining. Cada redução observada do contador
incrementa a metade superior. Isso preserva a monotonicidade entre chamadas,
inclusive em funções e loops; não mede voltas completas que ocorram entre
duas leituras. As unidades e a comparação entre invocações ou estágios não
são garantidas pela extensão. O cache de shaders passou à revisão 15.

A emulação clássica de `StorageReadWithoutFormat` usa metadados por
descritor para reconstruir componentes ausentes e valores padrão
inteiros/flutuantes. Os metadados acompanham cópias, templates,
UPDATE_AFTER_BIND, views de imagem e texel buffers. Descritores nulos
produzem zero em todos os componentes. A normalização ocorre antes do
predicado de robustness. Imagens sem formato permanecem UAV mesmo quando
uma variável é somente leitura, preservando aliases com imagens graváveis
e leituras posteriores a `imageStore` com a barreira correspondente.
O caminho bindless mantém as conversões e os componentes da view nativa.

O suporte por formato continua condicionado às capacidades consultadas no
D3D12, incluindo UAV_TYPED_LOAD: não há fallback raw para formatos que o
dispositivo não consegue carregar. A lista Vulkan de formatos sem
qualificador não inclui R64 nem profundidade, excluídos da matriz abaixo.

Validação em GPU RX 6600M no PC, com a DLL Release:

- GCN: 64 direções, faces e empates; coordenada constante negativa;
  relógio chamado em função e repetido 32 vezes por invocação.
  `GCN_CASES=64 ERRORS=0 SDK_ERRORS=0`.
- Leitura/escrita sem formato: 47 formatos de imagem em 1D, 2D, arrays 2D
  e 3D, além de 44 formatos de texel buffer, nos caminhos clássico e
  bindless. São 1.160 fases, incluindo atualização por template, cópia de
  descritores, restauração após null, acessos fora dos limites e aliases
  coerentes. Todas retornaram `ERRORS=0 SDK_ERRORS=0`.
- Os resultados foram comparados com views amostradas independentes,
  incluindo normalização, quantização, formatos packed e componentes
  ausentes. As sondas e os logs ficam em `Build/gcn-unformatted/`.
- As regressões de UPDATE_AFTER_BIND, texel buffers, ciclo de vida,
  feedback, mixed samples 3D, estatísticas, robustness2 e LOD passaram.
  PAC-MAN 256 permaneceu aberto por 25 segundos no PC, com
  `D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`, `SHUTDOWN=0` e saída zero.

Referências: [VK_AMD_gcn_shader](https://docs.vulkan.org/refpages/latest/refpages/source/VK_AMD_gcn_shader.html),
[formatos Vulkan](https://docs.vulkan.org/spec/latest/chapters/formats.html)
e [assinaturas DXIL](https://github.com/microsoft/DirectXShaderCompiler/blob/main/lib/DXIL/DxilOperations.cpp).

O bundle Release foi recompilado e assinado, com `CMS_SIGNATURE=PASS`,
`PAYLOAD_HASHES=PASS` e `SOURCE_WHITESPACE=PASS`. SHA-256 de
`vulkan_dzn.dll`: `da662e0b68376f17160ef775a68fead5b28ae6d8919518040f7ddf8787625e5d`.
A captura `Build/gcn-unformatted/pacman-regression.png` foi conferida.
O pacote não foi instalado; a validação ocorreu no PC, sem execução no Xbox.
