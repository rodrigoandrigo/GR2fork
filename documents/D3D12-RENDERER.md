# Renderer D3D12: estado de implementação

O backend nativo descrito abaixo é experimental e é o padrão atual do host UWP. Vulkan/Dozen permanece como alternativa; veja [DOZEN-UWP.md](DOZEN-UWP.md).

Existe um renderer guest experimental ligado ao Liverpool. O renderer completo de jogos PS4 ainda não está concluído, e o boot de jogos permanece desabilitado. Nenhum resultado abaixo comprova execução de jogos, AppContainer ou Xbox.

## Caminhos implementados

| Caminho | Implementação atual |
| --- | --- |
| Caminhos | GpuCommandSink, rasterizador D3D12 próprio e registradores Liverpool; draws diretos, indexed/instanced, expansão CPU de draws indiretos e dispatch direto/indireto |
| Shaders | Recompiler GCN existente → SPIR-V → SPIRV-Cross HLSL → DXC DXIL; VS/PS/CS, HS/DS de tessellator e GS de stream 1/2, reflexão de recursos, base vertex/instance, compute wave64 e constantes de número de workgroups |
| Buffers | Recursos raw/typed via IR, agrupamento de views sobrepostas, identidade física para aliases contíguos, snapshots CPU, upload/download e writeback nas views guest originais; flat buffer, clip planes, GDS e spill LDS |
| Imagens | Texturas 1D (lowering 2D), 2D/3D, mips e layers, views de layer único/arrays, formatos comuns e BC, samplers, tiling GPU por compute (`tiling_runtime`) com fallback CPU, MRT, profundidade e MSAA sem compressão; read address e write address de depth separados |
| Pipelines | Root signatures por reflexão; cache limitado com chave dos shaders, bindings, input layout e estados usados; rasterização, blend, máscaras, topologias básicas, viewports múltiplos (16) com `SV_ViewportArrayIndex` e scissor por viewport; rows TEXCOORD alinhados entre VS e FS com placeholders na interface HLSL; clip planes de usuário e conversão de profundidade (-W→W) nos caminhos VS, GS e tessellation (clip planes de usuário em GS e no domínio TES; -W→W no GS via conversão do Z em EmitVertex) |
| Coerência | Posse temporária de páginas CPU e de todos os aliases físicos até a fence e o writeback; cópias pelo backing participam do bloqueio, e map/unmap aguarda o lease; buffers com aliases lineares, inclusive segmentados, compartilham a alocação GPU; representações simultâneas incompatíveis continuam rejeitadas |
| VideoOut | Contrato de apresentação compartilhado com adaptador Vulkan desktop; snapshot imutável, detiling/swizzle/gamma SDR, FP16 e BT.2020/PQ via scRGB, blank/last frame, eventos existentes e swapchain do painel de jogo UWP; redimensionamento centralizado com proporção preservada, compensação de DPI e reapresentação do último frame durante pausa |
| Embedding | Exceções de GPU/VideoOut tornam-se erro da sessão; thread de apresentação MTA; UWP não cria janela SDL nem executa seu loop de eventos desktop |
| Frontend | Painéis separados de jogo e ImGui, composição com alpha premultiplicado, compensação de DPI e overlay controlado pela thread de renderização |

As cópias locais ficam em `externals/d3d12-native` e `externals/spirv-cross`. O SDL3 continua vindo da pasta UWP indicada pelo usuário. A bridge restrita copiada do projeto de origem não traduz shaders guest: o caminho de jogos usa `src/video_core/renderer_d3d12`, o recompiler existente e o SPIRV-Cross completo. Não há Dozen ou tradução Vulkan→D3D12 neste caminho de renderização.

## Limites de recursos e sincronização

- Buffers: 128 entradas e orçamento GPU de 256 MiB; view de até 16 MiB.
- Imagens: 64 entradas, orçamento GPU de 256 MiB e snapshots CPU de 256 MiB; superfície guest de até 64 MiB.
- Shaders: 512 entradas e 64 MiB; pipelines: 128 entradas e 32 MiB.
- Bindings por comando: 512 descritores de recursos e 128 samplers.
- VideoOut: frames e uploads limitados por orçamento de 256 MiB.
- Spill LDS: até 16 MiB por dispatch, separado por workgroup.
- Wave64 em graphics não é expressível no host D3D12: a largura de onda só pode
  ser forçada em compute (`[WaveSize]` é rejeitado pelo DXC 1.8 para PS/VS/HS/DS/GS
  nos modelos 6.6/6.7/6.8; probe em `Build/wavesize-graphics-probe.cpp`, log em
  `Build/wave64-graphics-probe.log`), e o runtime não expõe nem controla a largura
  escolhida para estágios gráficos (`D3D12_OPTIONS1` informa apenas o intervalo
  32..64 no PC de validação). Padrões GCN cross-lane de 64 lanes (mbcnt/ballot com
  posições wave-relativas, readlane/writelane de indexes >= 32, matemática sobre
  SubgroupSize) não podem receber semântica fiel e continuam rejeitados com erro
  explícito em dispositivos de onda variável; operações quad (2×2) não dependem da
  largura da onda e são aceitas em qualquer estágio.

O caminho atual espera a GPU e faz readback a cada draw/dispatch. A posse de páginas ordena os acessos CPU/GPU nos limites de cada comando, mas não é uma implementação de desempenho aceitável para jogos. Falta substituir essa serialização por tracking incremental de dirty pages e submissões assíncronas, preservando fences, aliases e writes CPU concorrentes. Não é permitido reutilizar o runtime após falha de gravação de comandos. Detalhes e limites em [D3D12-COHERENCE.md](D3D12-COHERENCE.md).

## Validação executada no PC

O diagnóstico nativo temporário em `Build/` liga os objetos reais de produção e executa GCN traduzido em GPU D3D12, com readback. Os quinze casos passaram:

1. `BUFFER_STORE_DWORD`: writeback `11223344`.
2. VS/PS guest e draw de triângulo: 256 pixels vermelhos.
3. CS guest `IMAGE_STORE`: 256 pixels vermelhos em imagem com pitch.
4. PS guest `IMAGE_SAMPLE_L`: 256 pixels vermelhos amostrados.
5. GDS: writeback `89abcdef`.
6. LDS de 64 KiB com spill para storage: writeback `76543210`.
7. Aliases físicos: views compartilham recurso e readback `13579bdf`.
8. Draw somente de profundidade: 256 valores escritos e input separado preservado.
9. Variante VS com inversão de Y: 256 pixels amostrados.
10. IMAGE_STORE 3D: 1.024 voxels com gradiente entre quatro fatias.
11. IMAGE_SAMPLE_L 3D: 256 pixels da fatia selecionada.
12. IMAGE_STORE 1D: 16 texels com gradiente.
13. IMAGE_SAMPLE_L 1D: 256 pixels do texel selecionado.
14. View de layer único em imagem array: 256 pixels da segunda camada.
15. View com base mip 1: 256 pixels do mip selecionado.

Release da DLL/host e geração/assinatura do pacote foram executados. O pacote inclui `dxcompiler.dll` e `dxil.dll`; a auditoria estática dessas duas DLLs contra a lista local de APIs UWP não encontrou imports proibidos. Isso não é prova de carregamento ou compilação DXIL dentro de AppContainer.

Há uma fonte de homebrew OpenOrbis em `documents/hardware/d3d12-guest/main.cpp`, com instruções no README ao lado. Ela ainda não foi compilada nem executada em PS4 por ausência do SDK local. Não há resultados de hardware que validem mudanças de emulação.

## Redimensionamento da apresentação UWP

O painel informa suas dimensões físicas e as escalas de composição pela API
`shadps4_resize_surface`; a API anterior `shadps4_resize` permanece disponível.
O presenter D3D12 mantém os buffers na resolução do frame guest e aplica
`SetMatrixTransform` para ajustar a imagem ao painel, preservando a proporção,
centralizando a área exibida e compensando o DPI. O redimensionamento reapresenta
o último frame, inclusive durante pausa, sem depender de um novo flip guest.
Frames em branco acompanham as dimensões do painel. O overlay ImGui também aplica
a transformação inversa da escala de composição.

Validação nativa no PC em 17/10/2026:

- Dez cenários com frames de 64×32 e 128×96, painéis de 1280×720, 600×900,
  1920×1080 e 777×333, escalas de composição entre 1,0 e 2,0, incluindo mudança
  apenas de DPI e escalas diferentes nos dois eixos. Proporção, centralização,
  reapresentação durante pausa e pixels do frame em cache passaram; frame em
  branco também passou. Zero erros da camada de depuração D3D12
  (`Build/native-resize-msaa-validation.log`).
- Overlay: 400 frames com redimensionamento e mudança de DPI; matriz de
  composição e readback GPU passaram
  (`Build/native-resize-imgui-validation.log`).
- Compilação Release x64 e assinatura do bundle concluídas, com
  `CMS_SIGNATURE=PASS` (`Build/native-resize-uwp-build.log`). As DLLs
  `shadPS4.dll`, `dxcompiler.dll` e `dxil.dll` do pacote coincidem com o runtime;
  o núcleo também coincide com a saída de produção
  (`Build/native-resize-package-verification.log`).

Esses diagnósticos usam composição nativa no desktop e não executam jogos.
A validação visual do redimensionamento com jogos dentro do aplicativo UWP,
em AppContainer e no Xbox permanece pendente. Os diagnósticos e logs em `Build/`
são artefatos locais temporários.

## HDR e apresentação float

O presenter nativo aceita `A16R16G16B16Float` e `A2R10G10B10Bt2020Pq`,
com pitch, detiling e snapshot do frame. FP16 é interpretado como BGRA linear
Rec.709 e convertido para RGBA FP16 na swapchain `R16G16B16A16_FLOAT`.
Valores finitos negativos e acima de 1 são preservados; NaN/infinito são
substituídos por zero. A saída usa `RGB_FULL_G10_NONE_P709` (scRGB).

BT.2020/PQ é decodificado pela curva ST.2084 e convertido para primárias
Rec.709 estendidas, com 1,0 scRGB representando 80 nits. O modo HDR do port
também seleciona essa interpretação para buffers RGB10 genéricos. FP16 mantém
sua interpretação linear; o ajuste de gamma aplica-se ao modo SDR. O branco
SDR informado pelo Windows ajusta frames FP16 SDR quando a tela está em HDR.
O compositor do Windows gerencia a saída física e a adaptação para telas SDR;
o caminho scRGB não envia metadados HDR10 fixos.

O host acompanha `DisplayInformation.AdvancedColorInfoChanged`, informa o HDR
ativo e o branco SDR pela API `shadps4_set_hdr_output` e volta a SDR se a consulta
não estiver disponível. A capacidade HDR anunciada ao guest exige apresentação
scRGB suportada, HDR ativo na tela e a opção `HdrAllowed`. Mudanças HDR/SDR ou
do branco SDR invalidam o upload do frame em cache. Formato e espaço de cor
são atualizados também quando a resolução permanece igual.

Referências: [Advanced Color e scRGB no DirectX](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range)
e [AdvancedColorInfo](https://learn.microsoft.com/en-us/uwp/api/windows.graphics.display.advancedcolorinfo).
A fonte OpenOrbis `documents/hardware/d3d12-guest/videoout-hdr.cpp` permite
comparar faixas SDR, FP16 e PQ e transições entre formatos. A interpretação
FP16 e a luminância/gamut finais ainda precisam de comparação em PS4 físico,
tela HDR, AppContainer e Xbox; compilação e readback não comprovam essas etapas.

O diagnóstico nativo temporário passou em 17/10/2026 com zero erros D3D12:
FP16 linear/tiled, pitch com padding, canais, valores negativos/acima de 1,
redimensionamento, branco SDR de 160 nits, PQ a 10000 nits, rampas e primárias
BT.2020 contra referência independente em dupla precisão, transição HDR/SDR
do mesmo frame em cache, capacidade HDR desativada em saída SDR e blank frame.
Os dez cenários anteriores de redimensionamento SDR também passaram
(`Build/native-hdr-msaa-validation.log`).
Release x64 do núcleo e do host compilou, o bundle foi assinado com
`CMS_SIGNATURE=PASS` (`Build/native-hdr-uwp-build.log`) e as DLLs empacotadas
coincidiram com o runtime e o núcleo de produção
(`Build/native-hdr-package-verification.log`). O pacote não foi instalado ou
executado nesta validação.

## MSAA

MSAA de cor/depth, máscaras, posições, leitura GCN e resolve estão implementados.
Os 32 diagnósticos (15 anteriores e 17 MSAA) passaram com zero erros de validação D3D12,
além dos casos extra de viewport espelhado (flip horizontal exato por amostra) e de
wireframe/polygon modes (contorno de quads/polygon e smoke test de wireframe, `Build/native-gcn-validation.log`).
Detalhes, limites e homebrew em [MSAA-D3D12.md](MSAA-D3D12.md).

## Logic ops em render targets

As 16 operações lógicas usam componentes inteiros de 32 bits temporários no
output merger. UNORM/SNORM, sRGB, FP16/FP32 e os formatos packed renderizáveis
são convertidos no export do pixel shader; os bits do destino são expandidos
no upload e recompostos no writeback. O shader guest é executado uma vez por
invocação, preservando a ordem das operações entre primitivas sobrepostas.

Somente os canais habilitados de cada alvo são compactados na representação
temporária. Isso permite máscaras diferentes entre MRTs, embora o D3D12 exija
logic op global sem blend independente. Slots vazios recebem descritores RTV
nulos válidos. Padding, canais desabilitados, layers e o layout guest são
preservados na recomposição; MSAA usa transferência por amostra. O caminho
temporário usa 16 bytes por texel/amostra, continua sujeito aos orçamentos do
cache e usa tiling CPU durante a expansão/recomposição.

Em 17/10/2026, 1.794 casos com GCN real passaram com zero erros D3D12:
24 formatos não inteiros e quatro casos de regressão UINT/SINT, 16 operações,
superfícies lineares/tiled, 1×/4× MSAA, MRTs nos slots 0 e 3 com máscaras
complementares, preservação de padding e duas instâncias sobrepostas em XOR
(`Build/native-logic-msaa-validation.log`). Os resultados são comparados a
uma referência CPU bitwise após quantização, incluindo valores SNORM negativos.
Também passaram MRTs com UNORM e FP16 juntos e troca RGBA/BGRA sem alteração do
snapshot guest; a chave do cache inclui o formato original e a máscara de canais.
A regressão geral GCN/MSAA terminou com `D3D12_VALIDATION_ERRORS=0`
(`Build/native-regression-msaa-validation.log`).
Release x64 compilou e o bundle foi assinado com `CMS_SIGNATURE=PASS`
(`Build/native-logic-uwp-build.log`). As DLLs empacotadas coincidiram com o
runtime e o núcleo de produção (`Build/native-logic-package-verification.log`).
O pacote não foi instalado ou executado nesta validação.

A fonte de comparação em PS4 é
[`hardware/logic-ops/main.cpp`](hardware/logic-ops/main.cpp), com instruções no
README ao lado. Ela ainda não foi compilada nem executada. A equivalência das
conversões/arredondamento e da aplicação de ROP em float/sRGB com o CB do PS4
permanece dependente dessa comparação; os casos no PC não comprovam hardware
guest, jogos, AppContainer ou Xbox. As limitações existentes de feedback entre
attachments e shaders continuam valendo.

## Trabalho pendente para jogos

- Captura de stream-out stream 0 com GS off-chip implementada: a captura é
  shader-side (`BufferType::Strmout`, SSBO guest-backed reutilizando o
  writeback do `ResourceCache` — `MarkWritten`/`PrepareDownload`/
  `CommitDownloads` escrevem na memória guest), injetada no `Info.buffers`
  pelo recompilador quando `HwGeometryRuntimeInfo.strmout_buffer_mask != 0`
  e escrita no `EmitVertex` (`EmitStreamOutCapture`): vértice k do slot =
  dwords 0-3 `Position0` xyzw + dwords 4+ `Param_i` xyzw em sequência,
  truncado a `vgt_gs_vert_itemsize[0]` dwords e à janela
  `stride*out_vert_data_size`; todos os buffers habilitados do stream 0
  recebem o mesmo conteúdo, escrito em `(offset + k) * stride * 4`.
  Como GFX6-VI não tem registradores `VGT_STRMOUT_BUFFER_BASE`, a base de
  cada buffer vem de descritores `AmdGpu::Buffer` varridos heurísticamente
  no user data do GS (ordenados por compatibilidade de `stride` com
  `VTX_STRIDE`), com dump completo (user data + candidatos + escolhas) em
  `LOG_INFO(Render_Vulkan)` na primeira configuração nova; ausência de
  candidatos é erro explícito. Unidades de `SIZE/VTX_STRIDE/OFFSET`
  tratadas como dwords (linear = dwords*4 bytes) — pressupostos
  documentados, confirmáveis/corrigíveis pelo dump/traces. FILLED_SIZE é
  atualizado após cada draw (fendente de espera por draw do backend):
  contagem estática CPU = partições da topologia × instances × GS
  invocations × nº estático de `EmitVertex` do IR (`Info::
  gs_emit_vertices`), saturada pela capacidade do buffer. Continuam erro
  explícito: GS on-chip com stream-out, streams além do 0 /
  `rast_stream != 0`, draws sem GS (rodam sem captura), emissão dinâmica
  de vértices (loops com `EmitVertex` contam como 1 nó, relatório por
  trace) e buffers SEM descritor user data do GS. Interruptores de
  validação por trace de jogo real: slot exato do descritor de cada buffer
  de stream-out e unidades de SIZE/VTX_STRIDE/OFFSET.
  Clip-plane e conversão de profundidade (-W→W) em geometry foram
  implementados: `user_clip_plane_enable` e o clip space
  (`PA_CL_CLIP_CNTL`/`MinusWToW`) seguem para o runtime do GS
  (`HwGeometryRuntimeInfo`
  `user_clip_plane_mask`/`emulate_depth_negative_one_to_one`); clip planes
  usam o mesmo lowering do VS avaliado antes de cada `EmitVertex` do copy
  shader (pula quando o GS exporta as próprias distâncias); com
  tessellation, os planos aplicam no domínio (TES compilado como
  `HwStage::Vertex`) e, com GS embutido, no próprio GS já lowerado;
  -W→W converte o Z final do GS em
  `EmitVertex` (`ConvertDepthMode`), cobrindo também tessellation+GS.
  Shading parcial de samples despacha o pixel shader por
  amostra (mais invocações que o hardware guest; divergência documentada em
  `MSAA-D3D12.md`). Geometry e tessellator (LS/HS/ES/DS) com invokes
  múltiplos, exit prim e copy shader já rodam nos modelos D3D12 nativos.
  Viewports com escala horizontal negativa (imagem espelhada) rodam via flip
  de posição no shader, com escala/origem do viewport D3D12 em valor
  absoluto; escalas zero (degeneradas) continuam erro explícito.
- EQAA verdadeiro (fragments != samples). MSAA depth/stencil sem compressão,
  alpha-to-coverage, logic ops nos formatos renderizáveis descritos acima,
  polygon bias, index swap, MRTs com gaps/subviews, exports
  misc/clip/cull (até 8 distâncias), depth bounds (trivial e via discard no
  shader) e clears explícitos de depth/stencil estão implementados e
  validados em GPU no PC (`Build/native-stencil-msaa-validation.log`); HTILE/CMASK/FMASK
  sem MSAA são tratados como superfícies planas, com risco documentado em
  superfícies fast-cleared. MSAA com FMASK roda com dados de cor sempre em
  storage MSAA plano (mesmo design do Vulkan de produção, que também não
  comprime); leituras de recursos FMASK no shader são stubadas no recompiler
  (valores constantes) e o decompress é no-op — validado em GPU com
  `IMAGE_GET_RESINFO` sobre sharp FMASK (`Build/native-gcn-validation.log`).
- Feedback SRV/UAV com `IMAGE_LOAD` foi implementado por lowering das leituras
  para UAV compartilhada quando os descritores têm o mesmo endereço base guest
  no mesmo shader. Há barreira de memória, swizzle no shader e conversão sRGB;
  mips constantes/dinâmicos usam views UAV por nível. O cache exige layouts
  compartilháveis e suporte do dispositivo ao formato de typed UAV load.
  `IMAGE_SAMPLE`/gather, aliases com outros endereços virtuais e entre estágios
  ainda não têm esse lowering. Veja [D3D12-COHERENCE.md](D3D12-COHERENCE.md).
- Views simultâneas de imagem/buffer com escrita usam lowering de `IMAGE_LOAD`/
  `IMAGE_STORE` e atômicos inteiros de 32 bits para buffer raw compartilhado,
  incluindo aliases físicos, tiling e preservação de padding. O limite é
  16 MiB por superfície e 64 MiB de tabelas por shader, com duas entradas de
  buffer por imagem. Texels de 8/16 bits (`R8`, `RG8`, `R16`) usam extração
  de bits e escrita parcial com compare-and-swap de 32 bits, preservando os
  vizinhos entre invocações. Permanecem pendentes BCn/depth,
  amostragem filtrada, atômicos sem equivalente implementado e conflitos entre
  estágios. Alternate tiling Neo usa 16 pipes, tabelas alternativas e bank
  swizzle nos caminhos CPU/GPU e nos aliases. RGB96 permanece linear: a
  referência AMD restringe formatos expandidos a layouts lineares; descritores
  RGB96 tiled são rejeitados, sem substituir o layout por uma aproximação.
- Feedback SRV/UAV ainda não
  coberto pelo lowering acima e aliases de
  layouts incompatíveis com escrita. Imagem e buffer somente de leitura podem
  compartilhar backing físico usando snapshots independentes. Views de buffers
  que admitem offsets lineares compartilham um recurso GPU, inclusive com
  endereços virtuais distintos e backing segmentado. Aliases que exigem tradução
  não linear e repetição de um segmento físico dentro do recurso ainda são
  rejeitados. A posse temporária de páginas protege os acessos CPU/GPU até a
  fence e o writeback, cobrindo todos os aliases e as cópias diretas pelo backing;
  o unmap respeita os limites do mapa físico e preserva os outros aliases. O
  tiling mantém staging, parâmetros e readback por operação e preserva padding,
  com fallback CPU quando o orçamento temporário não comporta outra operação.
  Visibilidade CPU/GPU durante a execução do próprio dispatch, dirty tracking
  incremental e submissões assíncronas continuam pendentes. A validação e a fonte
  OpenOrbis estão descritas em [D3D12-COHERENCE.md](D3D12-COHERENCE.md); isso não
  comprova coerência completa de todos os estados guest.
- Gate de cor por resultado de depth (bits
  ENABLE_COLOR_WRITES_ON_DEPTH_FAIL / DISABLE_COLOR_WRITES_ON_DEPTH_PASS) com func de depth mista é executado em
  duas passes na mesma submissão: a pass de fail usa a função de depth invertida (func→complementar) com write de
  depth zerado, ops de stencil KEEP preservando o teste, para escrever cor exatamente nos pixels que falham o teste
  guest; a pass base (func real) cobre depth write e cor nos pixels aprovados conforme os mesmos bits. O
  fragment/vertex/geometry não pode ter writes de memória (storage images/buffers, atomics, DMA), verificados no
  `draw_runtime.cpp`; caso contrário erro explícito. A classificação de fail usa o depth buffer congelado no início
  do draw (sem write de depth na pass de fail). Funções Always/Never e test de depth desativado têm dados exatos
  exatos (caminho pré-existente de máscara/desativação), e stencil front/back divergente só bloqueia quando a face
  correspondente é desenhada e o ref/máscara realmente é consumido. Workgroup start
  offsets (`COMPUTE_START_*`) entregam o id de workgroup com os offsets somados
  (`relative + start`, por eixo e apenas quando `tgid_enable` inclui o eixo).
  O valor entra no runtime info do CS — chaves de recompilação novas só quando um
  start diferente de zero aparecer (starts iguais a 0 compartilham o
  cache pré-existente). Modos de polygon com face única continuam na conversão de índices na CPU: line
  em quads/polygon/rectangles vira lista de arestas de contorno (sem diagonais) e point sem culling em triângulos
  vira lista de pontos. Modo misto (ambas as faces visíveis com fills diferentes) executa duas passes por face,
  cada uma com o cull complementar (CULL_BACK mantém as faces front, CULL_FRONT as back), o fill do modo da face
  e o stencil/ref/máscaras do config guest daquela face (`fill_pass_stencil`); as exigências de ref/máscara
  "single value" valem só quando as duas faces caem na mesma pass — com passes separadas cada face carrega o
  próprio config. Misto combinado com guest geometry, tessellation ou primitivas polygon/quad/rect siguen erro
  explícito (as passes re-executariam efeitos laterais ou conflitariam com a conversão de contorno). Point fill
  com culling é emulado por um geometry shader auxiliar gerado (`CompilePointFill`, molde do rect expansion): o
  winding projetivo (det de x/y/w) recusa as faces opostas à face da pass (tabela por `front_face`/
  FrontCounterClockwise) e emite os 3 vértices como pontos, com o stencil da face renderizada na slot front do
  D3D12 (pontos sempre usam a slot front); point fill pura sem culling permanece na conversão CPU. O gate de cor por depth (fail pass) compõe por
  pass: cada pass clona o próprio fail com func invertida, e a rotação provoking-Last aplica-se às passes
  regulares (as passes de pontos não consomem provoking vertex).
- Auditoria de cobertura de formatos, estados de rasterização e operações IR concluída. Estados: cull mode,
  front face e winding preservados (o flip Y no shader e o yscale guest negativo se cancelam, logo o winding de
  tela e o `FrontCounterClockwise` coincidem com o hardware guest), depth clip (ZclipEnable), bias de depth
  único quando front e back coincidem; bias divergente entre faces (habilitação assimétrica ou valores
  front/back diferentes) segue a mesma via de passes por face do modo misto — cada pass carrega o
  bias/clamp/slope do config da face (face desabilitada recebe zero) — e combina com os demais triggers de
  pass (a mesma restrição de tessellation/geometry/primitivas polygon se aplica); provoking vertex First é o comportamento nativo D3D12 e Last com shading flat é emulado por
  reordenação do índice convertido na CPU: cada triângulo (a,b,c) do índice convertido vira (c,a,b) — rotação
  cíclica que preserva winding, posição e interpolação, colocando o vértice guest final na slot 0 que o raster
  D3D12 usa como fonte do flat — e cada segmento de linha (a,b) vira (b,a); disparado pelo par
  `provoking_vtx_last` + `flat_shade` em algum input guest (sem flat, Last é inócuo e segue sem conversão),
  incluindo rects (o triângulo warpado do rect guest também tem o último vértice como provoker), strips/fans/
  quads/polygon já convertidos e topologias de linha; erra explícito com adjacency, geometry ou tessellation,
  onde a ordem dos vértices não é reordenável sem reescrever a fonte de vértices. Linhas largas (width ≠ 1px em
  topologias de linha ou wireframe) e line stipple (`mode_control.line_stripple_enable` + `line_stipple`
  @ 0xA283 / `line_stipple_cntl` @ 0xA209 / `line_stipple_scale` @ 0xA20A) são emuladas com um GS auxiliar de
  expansão (`ShaderCache::CompileLineExpand`, mesmo molde de point fill/rectangle): a pass vira SOLID/CULL_NONE
  com GS de input `line`/`lineadj` (topologias de linha e tessellation isoline) que emite o quad da linha
  expandida, perpendicular em NDC por metade da largura escalado pelas dims guest do viewport (assadas no
  shader, indexadas por `SV_ViewportArrayIndex` quando exportado), ou input `triangle`/`triangleadj` no
  wireframe (triângulos convertidos na CPU; cada face expande as 3 arestas com cull por determinante análogo
  ao do point fill, ambas slots de stencil idênticas); com stipple o GS parte o segmento em quad por janela
  de bit (128 janelas × 32 instâncias por segmento, além disso trunca), bit selado por
  `min(bit, repeat*16+15)` com ordem LSB/MSB de `pattern_bit_order`, padrão inteiramente 0 não desenha nada e
  0xFFFF reusa o quad único; `line_stipple_cntl` (reset/expand_full_length/fractional_accum/diamond_adjust) e
  `PA_SU_LINE_STIPPLE_VALUE/STATE` (0xC280, sem mapeamento no register file) não são consumidos. Divergências
  a validar em hardware: semântica exata da escala do padrão (interpretamos scale como pixels por bit,
  scale 0 → 1px/bit estilo GL) e do reset por segmento; janelas de bit encolhidas 0.01px nas bordas (evita
  dupla cobertura em blend); progressão do padrão em comprimento de tela (w divergente entre endpoints
  distorce a progressão); interpolação de atributos linear na tela (não perspectiva-correta) nos quads;
  cobertura por quads de triângulo (regras de pixel/blend marginais divergem da rasterização guest de
  linhas); segmentos com w ≤ 0 descartados. Índice de viewport exportado pelo shader
  (`misc` com `use_vtx_viewport_idx`) amarra os 16 viewports guest (`PA_CL_VPORT_*`) com scissors por
  índice (`viewport_scissors` intersectando o trio global), via `SV_ViewportArrayIndex` nativo do
  SPIRV-Cross; viewports além do 0 entram com sanitização (scale/offset não finito vira viewport vazio,
  depth range inválido vira [0,1]), flip Y/X e a conversão clip-space no shader (clip_disable) seguem do
  viewport 0 (orientação por viewport precisa alinhar entre as viewports guest de um draw e múltiplos
  viewports com clip_disable permanecem erro do emitter), e sem exportação o draw vincula só o viewport 0. Formatos: `ConvertFormat` cobre o leque GNM renderável/samplável
  (unorm/snorm/uint/sint/srgb 8-32bpp, 10-10-10-2, 11-11-10, 9E5, 4:4:4:4, BC1-7, depth/stencil) com erro
  explícito em formato faltante, probes prévios (BGRA SRV, cross-family, unformatted readback) validaram os
  caminhos; SRV/UAV de sRGB→UNORM e feedback SRV/UAV continuam na pendência de coerência acima. Blend: fatores,
  operações, rop3 lógico completo, dual source via fatores SRC1 e saída extra do PS. Operações IR: o backend
  SPIR-V é compartilhado com o caminho Vulkan, logo a cobertura de opcodes é a do backend completo; desvios de
  toolchain D3D12 já documentados (demote lowering para discard, stencil export via `SV_StencilRef`, fp64
  degradado, clipping de usuário em recompiler) mantêm-se.
- Validação visual e de luminância/gamut do VideoOut SDR/FP16/HDR em AppContainer,
  tela HDR e Xbox, incluindo comparação de FP16 com PS4 físico.
- Boot brokered completo, teardown dos threads guest e ciclo de execução de jogos embutidos.

Estados ainda não suportados geram erro explícito da sessão. Compilar e executar estes casos básicos não encerra o pedido de renderer completo.




