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
