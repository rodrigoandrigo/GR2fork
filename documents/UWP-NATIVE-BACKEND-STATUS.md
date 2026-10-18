# Renderer D3D12 nativo: integracao e pendencias

## Ligado nesta etapa

- API shadps4_set_graphics_backend/shadps4_get_graphics_backend. A selecao so e
  aceita antes da inicializacao e e serializada com initialize/shutdown.
- NativeHost cria a superficie, WindowSDL embutida e D3D12Presenter sem carregar
  o driver Dozen. GnmDriver cria Liverpool e entrega a ele D3D12::Rasterizer.
  O rasterizer registra o sink de memoria; o VideoOut recebe o sink nativo.
- Frontend ImGui: Backend grafico, com D3D12 nativo como padrao e identificado
  como experimental. A escolha global e salva em LocalSettings, chave
  shadps4.graphics.backend (dozen/d3d12), e aplicada recarregando o nucleo.
  Uma preferencia Dozen ja salva continua sendo respeitada.
- Inicializacao/desligamento usam exclusao mutua para impedir troca concorrente
  de backend. O worker WGI para antes da destruicao do host grafico.
- A superficie externa e desligada explicitamente antes da destruicao da
  swapchain/contexto. Se o callback falhar, shutdown retorna erro e preserva
  os recursos para permitir nova tentativa, sem descarregar a DLL.
- A fila nativa e drenada ao desligar a superficie. O fence de renderizacao
  sinalizado antes de Present nao cobre trabalho de apresentacao enfileirado
  depois dele; a drenagem final ocorre depois das chamadas de apresentacao.
- VideoOut aplica espera quando o budget de frames preparados esta cheio.
  Em execucao normal nao descarta um frame para abrir espaco. A espera pode
  ser cancelada durante encerramento solicitado; a fase de jitter nao avanca
  quando se reutiliza o frame anterior.

## Resultado real do boot nativo

O boot PC com PAC-MAN 256 (CUSA05026) pelo backend nativo chega ao menu
inicial, com `BOOT_RESULT=0`, erro de sessão vazio e `SHUTDOWN=0`: 75 frames
apresentados em 90 segundos e captura `Build/real-game-frame.png` com a tela
de título (logo 3D e `PRESS X TO BEGIN`) íntegra. O segundo título local,
Chronicles of Teddy (Harmony of Exidus), também boota com `BOOT_RESULT=0` e
75 frames em 60 segundos, splash AKSYS Games íntegra na captura. O bloqueio
anterior em `stencil_enable=true` foi resolvido com o transporte dual-plane;
na sequência foram necessários footprint sem `ALLOW_DEPTH_STENCIL` em planos
R8, layout de mip por plano no detiling de stencil, mapeamento B4G4R4A4 com
remap de swizzle `[1,0,3,2]` nos SRVs e alinhamento de rows TEXCOORD entre a
interface de saída do VS e de entrada do FS no HLSL gerado (placeholders
`shadRowN` preenchem índices ausentes para que os rowID casem na criação do
PSO; diagnóstico de erros D3D12 inclui as mensagens da info queue do debug
layer). Gameplay além do menu/splash, áudio e outros títulos continuam
pendentes de validação.

## Proximos blocos de implementacao

1. Stencil guest: operacoes front/back, comparacao, mascaras e referencias;
   transporte independente dos planos depth/stencil entre memoria guest e
   recursos D3D12; preservacao no writeback. Implementado neste incremento
   com recurso typeless, copias por plano, `OMSetStencilRef`, flags
   read-only do DSV e promocao D16S8 para D32FS8; MSAA com stencil,
   mascaras/refs front/back divergentes e ops bitwise seguem rejeitados.
2. Estados ainda rejeitados: operacoes logicas fora
   do modo Normal/Disable ou em targets não inteiros, bias de poligonos fora
   do mapeamento DepthBias/Slope, destinos esparsos com mais de 8 MRTs, mais
   de 8 distâncias clip/cull e topologias que precisam de emulacao (polygon
   mode point, modos front/back divergentes). Implementados neste incremento:
   logic ops via LogicOpEnable, polygon bias, index byte swap por CPU,
   MRTs com gaps e BaseSlice, exports misc/clip/cull, depth bounds ([0,1]
   como passthrough e demais via discard no fragment shader com root
   constants b4), modos EliminateFastClear/FmaskDecompress como no-op e
   MSAA depth/stencil 2×/4×/8× com transfers por plano (16× pulado por falta
   de quality levels no dispositivo).
3. Exports misc/clip/cull do VS implementados via MapOutputs (paridade com o
   Vulkan) e clears explícitos de depth/stencil via ClearDepthStencilView.
   Modos de geometria on-chip, stream output e conversoes de clip/depth no GS
   seguem rejeitados. Hull/domain e rect lists possuem agora os caminhos
   descritos em UWP-NATIVE-TESSELLATION.md.
   Operacoes de quad em fragmento funcionam em dispositivos de largura variavel;
   ballot, shuffle e demais trocas que dependem de 64 lanes continuam exigindo
   WaveLaneCountMin == WaveLaneCountMax == 64. Compute tem um caminho distinto.
4. Recursos: coerencia com remapeamentos/descontinuidades de backing, aliases/formatos
   e views ainda rejeitados pelo cache. Metadata HTILE/CMASK/FMASK sem MSAA
   passa a ser ignorada (superfície tratada como plana); MSAA com FMASK roda
   com dados de cor em storage plano e leituras FMASK stubadas no recompiler,
   e fast clears via metadata sem flag explícita têm risco
   documentado de leitura stale. Footprints por plano usam flags neutras e o
   detiling de stencil usa layout de mip por plano. B4G4R4A4 é mapeado com
   remap de swizzle `[1,0,3,2]` nos SRVs, validado por amostragem em GPU.
5. MSAA: EQAA verdadeiro (fragments diferente de samples) continua rejeitado.
   Parcial sample shading despacha o pixel shader por amostra (superset do
   hardware guest, divergência documentada em MSAA-D3D12.md). MSAA
   depth/stencil sem compressão, alpha-to-mask e logic ops estão implementados
   e validados em GPU no PC. Formatos não inteiros usam representação UINT
   temporária, conversão de exports e recomposição do backing guest; máscaras
   por MRT e transferências por amostra são preservadas. A comparação de ROP
   e arredondamento com PS4 físico continua pendente; veja D3D12-RENDERER.md.
6. VideoOut permanece com snapshot CPU e upload; falta compartilhamento direto
   de recursos/fences entre renderer e apresentacao para reduzir essas copias.
7. FSR/XeSS: jitter e metadados existem, mas falta selecao de entradas da cena,
   motion vectors, depth, separacao do HUD, dispatch e historico temporal.
8. Validacao de runtime AppContainer e Xbox do backend nativo. Compilacao,
   probes sinteticos e boot Vulkan/Dozen nao substituem essa validacao.

Nao houve instalacao automatica do pacote nem alteracao de backend padrao.

## Geometry e conversao de primitivas

O rasterizador nativo conecta ES como vertex shader, GS como geometry shader,
o shader de copia VS e os registradores do anel ESGS/GSVS ao recompilador
compartilhado. O runtime compila GS, inclui seus recursos na assinatura raiz
e inclui seu bytecode na PSO e na chave do cache. Invocacoes GS usam
SV_GSInstanceID e o atributo instance, respeitando o limite D3D12.

Triangle fan, polygon, quad list, quad strip e line loop possuem conversao de
indices, inclusive draws sem indices e argumentos indiretos ja expandidos.
Restart divide segmentos antes da conversao e reinicia a paridade dos strips.
Topologias de adjacencia possuem mapeamento direto. Conversoes sao limitadas
a 16 MiB de indices de saida; nao ha alocacao ilimitada.

Continuam rejeitados explicitamente: geometry on-chip, stream output,
conversao de clip/depth no GS, quads/polygon antes de GS e wireframe de
quads/polygon/rect lists que exigem preservar as arestas externas. Polygon
mode point ainda precisa de geracao de vertices/arestas em shader. Fans e
line loops antes de GS sao normalizados para listas de triangulos/linhas.
GS e hull/domain sao compilados agora por SPIR-V -> NIR -> DXIL, com
validacao DXIL; o caminho HLSL permanece para VS/PS e o GS gerado para rects.

Build/native-geometry-gpu.log registra readback de rasterizacao com VS/GS/PS
SPIR-V -> HLSL -> DXIL. Build/native-geometry-instanced-gpu.log registra o
caminho com invocacoes GS e atributo de entrada. Sao diagnosticos sinteticos
de GPU no PC; nao comprovam um pipeline GCN GS completo, homebrew em hardware
PS4 ou gameplay no Xbox. A limitacao anterior de blocos builtin no tradutor
HLSL motivou a migracao do GS para NIR; ainda nao se declara cobertura
irrestrita de shaders GCN ou todas as combinacoes de interfaces.

Build/native-geometry-final-gpu.log registra GEOMETRY_SPIRV_DXIL_GPU_READBACK=PASS,
PRIMITIVE_CONVERSION=PASS e D3D12_ERRORS=0. Build/native-geometry-smoke.log
registra INIT=0, SMOKE_SHUTDOWN=0 e PROCESS_EXIT_CODE=0. O pacote foi compilado
e assinado em Build/native-geometry-package.log; a verificacao de arquivos
incluidos em Build/native-geometry-package-verification.log passou.

## Validacao desta etapa

Build/native-backend-drain-smoke.log registra INIT=0, SMOKE_SHUTDOWN=0 e
PROCESS_EXIT_CODE=0 com o backend nativo selecionado. A versao anterior podia
terminar o processo ao liberar a fila ainda envolvida na apresentacao;
a drenagem posterior aos Present e o detach corrigem esse ciclo observado.

Build/native-backend-game-final.log registra a falha explicita de stencil,
D3D_INFO_QUEUE_MESSAGES=0, BOOT_RESULT=-1 e SHUTDOWN=0. PROCESS_EXIT_CODE=4 e o
codigo deliberado do probe para boot nao concluido; nao indica sucesso no jogo.
Nao houve assertion de GPU nem encerramento abrupto nesse resultado final.

A exigencia de manter recursos vivos enquanto a GPU os utiliza e descrita em:
https://github.com/microsoft/DirectX-Graphics-Samples/issues/261

O pacote final e registrado em Build/native-backend-package-final.log e os
hashes em Build/native-backend-package-verification.log. Os testes nativos de
staging e rasterizacao estao registrados separadamente; o jogo real pelo
backend nativo permanece bloqueado no stencil descrito acima.

Build/native-backend-dozen-regression.log confirma BOOT_RESULT=0, SHUTDOWN=0,
PROCESS_EXIT_CODE=0 e nenhuma mensagem de erro D3D12 no caminho padrao.
Build/native-backend-package-verification.log confirma PAYLOAD_HASHES=PASS.

A repeticao mais longa em Build/native-backend-dozen-final.log apresentou 2049
frames em 45 segundos de execucao, capturou a tela inicial do jogo e terminou
com BOOT_RESULT=0, SHUTDOWN=0 e PROCESS_EXIT_CODE=0. Esta captura e do caminho
Vulkan/Dozen; nao e evidencia de gameplay pelo renderer nativo.

## Tiling de imagens por compute

src/video_core/renderer_d3d12/tiling_runtime.cpp porta o TileManager do
caminho Vulkan como shader compute D3D12. Uma fonte HLSL unica e compilada
por variante via DXC (CompileHlslRaw) com defines de spec
BITS_PER_PIXEL, NUM_SAMPLES, micro/macro tile, pipe config e banco;
SRV, UAV e CBV sao root descriptors sem heaps, e o cache de pipeline usa
hash das constantes de spec. O decode (Detile) copia os bytes tiled guest
para staging, passa pelo compute e reescreve a imagem com os footprints
D3D12 da propria imagem, eliminando ConvertTiling e CopyRows do upload.
O encode (Tile) roda na preparacao de downloads com a imagem em
COPY_SOURCE e grava um readback proprio; o commit copia para image.guest
e escreve o guest, com a cauda fora dos texels zerada como no caminho CPU.

O caminho GPU exige superficie tiled, samples 1, uma layer, ate 16 mips,
bits 8/16/32/64/128 e pitch*height igual aos texels por mip; o restante
segue pelo caminho CPU inalterado, incluindo stencil e MSAA. Buffers UAV
do alocador passaram a aceitar flags explicitas (ALLOW_UNORDERED_ACCESS
default none).

Validacao: PAC-MAN 256 BOOT_RESULT=0, SHUTDOWN=0 e debug layer sem erros
no novo caminho (resta apenas a mensagem pre-existente 1361 de sampler);
presents subiram de ~0.83/s para ~1.28/s, e os samples lentos de
upload_read/upload_copy/color_upload sairam do caminho critico.
Chronicles of Teddy BOOT_RESULT=0, SHUTDOWN=0 com ~3.9 presents/s.
O custo restante por draw e o wait de submissao GPU no DrawImpl
(picos de 16 a 148 ms periodicamente), ainda em diagnostico.

## Engine NGS2 integrado

O conjunto staged em Build/ngs2-deferred foi integrado em
src/core/libraries/ngs2: parser RIFF e WaveformInfo, PCM 8 a 64 bits e
ATRAC9 via LibAtrac9, grafo de racks/portes/matrices, ciclo
play/stop/pause/resume com estados reais de voz, System/Rack/Voice no
registry de handles e render por grain em SystemRender. Os type headers
existentes (ngs2.h, sampler.h, mastering.h e demais modulos) foram
mantidos; os arquivos novos sao ngs2_runtime.h (declaracoes Engine::*)
e ngs2_runtime_types.h (tipos de runtime e flags de estado de voz).
VoiceGetState devolve agora o estado real da voz em vez de deixar o
spin-poll do jogo sem resposta. O modulo bibliotecario libatrac9 ja
estava linkado no alvo UWP.

Um detalhe de build que custou diagnosticos: Copy-Item preserva o mtime
da origem, e o ninja considerou os .obj antigos como atualizados e pulou
a recompilacao do modulo apos a copia dos fontes staged; o binario
linkou uma implementacao anterior. Atualizar o LastWriteTime dos fontes
de src/core/libraries/ngs2 antes do cmake --build resolve. O probe
nativo Build/ngs2-probe.cpp chama os simbolos Engine::* fora de linha a
partir dos RVAs de Build/real-game-core.map apos recompile completo e
valida o ciclo inteiro na ABI nativa.

Validacao: o harness Build/compile-ngs2-probe.ps1 passa completo
(RESET_OPTION preenche defaults, SystemCreate/RackCreate/RackGetVoiceHandle
com handles distintos, PCM_MATRIX_RENDER e PAUSE_RESUME_COMPLETION e
ATRAC9_PARSE_DECODE_RENDER PASS com energia 0.77 no AT9 do proprio
Teddy, invalidacao de handles e shutdown verdes). Nos boots reais,
Chronicles of Teddy BOOT_RESULT=0, SHUTDOWN=0 sem erros D3D12, com o
log Lib.Ngs2 reduzido de 4661 para 8 linhas todas de CreateRack do
proprio engine (racks 0x1000 x4, 0x2000 x2, 0x3000, 0x4002), sem spam
de VoiceGetState nem erros de VoiceControl; os racks conferem com o
layout exigido pelo jogo. PAC-MAN 256 continua BOOT_RESULT=0,
SHUTDOWN=0.

Restante (gap conhecido herdado): 530 mensagens sce_scream_api.cpp(1368)
de Invalid parameter em 75s, o middleware CRI Scream do Teddy configura
o rack sampler mas ainda nao envia blocos de audio a ele (ver
Build/ngs2-deferred/documents/NGS2-UWP-IMPLEMENTATION.md). O caminho
sonoro so avanca alem disso com o fluxo de blocos do Scream.
