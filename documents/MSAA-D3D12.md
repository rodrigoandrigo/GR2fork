# MSAA no renderer D3D12

O backend nativo descrito abaixo foi preservado como trabalho experimental. O caminho padrão UWP passou a Vulkan/Dozen; veja [DOZEN-UWP.md](DOZEN-UWP.md).

O caminho guest implementa MSAA de cor e profundidade sem compressão. O draw
usa a contagem dos registradores Liverpool, PSO e attachments compatíveis,
máscaras dos quatro pixels do padrão 2×2 e posições programáveis. A frequência
do pixel shader pode ser por pixel ou por todos os samples. Configurações
que o dispositivo não suporta geram erro da sessão.

## Recursos e coerência

- Texturas 2D/array multisample, com 2, 4, 8 ou 16 samples, conforme suporte
  de formato/contagem anunciado pelo dispositivo. Não há mips ou UAVs MSAA.
- O upload usa uma textura auxiliar simples com uma camada por sample e
  draws de transferência em frequência de sample. O readback exporta cada
  sample por shader para essa textura auxiliar, antes da cópia para CPU.
- Nas famílias compatíveis, views UINT preservam os bits dos samples de cor
  durante a transferência. BGRA8 usa UNORM, e R11G11B10 usa conversão float.
  D16/D32 usam upload por SV_Depth e leitura do depth multisample.
- CPU tiling/detiling percorre cada sample: cor usa planos dentro do microtile;
  depth usa samples intercalados por pixel, seguindo as funções existentes
  de endereçamento AMD. Padding e camadas fora da view são preservados.
- O orçamento de imagens inclui textura MSAA, textura auxiliar e buffers de
  upload/readback. A implementação continua síncrona por comando.

## Shaders e resolve

GCN IMAGE_LOAD pode selecionar sample e camada em SRVs Texture2DMS/Array.
O emissor SPIR-V inclui ImageMSArray e usa o operando Sample, sem Lod, para
arrays MSAA. O índice de sample é reduzido pela contagem da imagem.

Um wrapper genérico do estágio de fragmento aplica as máscaras guest por
pixel em SV_Coverage, preservando exportações de coverage do shader. Quando
DB_EQAA pede frequência completa, SV_SampleIndex força a execução por sample.
Draws sem pixel shader usam um shader interno somente de coverage.

COLOR_CONTROL Resolve usa MRT0 como origem multisample e MRT1 como destino
simples, incluindo os slices das views. Os recursos native são separados por
layout/contagem, permitindo que os ranges guest do resolve se sobreponham.
ResolveSubresource é usado somente em formatos que anunciam suporte a resolve.
VideoOut continua recebendo a superfície simples após o resolve feito pelo guest.

As posições respeitam o [contrato de SetSamplePositions da Microsoft](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12graphicscommandlist1-setsamplepositions).
Tier 1 aceita um padrão por pixel; Tier 2 permite o padrão 2×2 quando o total
de posições cabe em 16. Um padrão 2×2 idêntico é reduzido a um padrão único.
As posições também são restauradas nas transições de profundidade.

## Evidência no PC

O diagnóstico temporário em Build liga os objetos reais do núcleo. Para a
validação, o device context foi instrumentado com callback ID3D12InfoQueue1,
debug layer e GPU-based validation; essa instrumentação não pertence ao produto.
Os quinze casos anteriores e dezessete casos MSAA passaram, totalizando 32:

| Caso | Resultado |
| --- | --- |
| Cor 2×/4×/8× com máscaras diferentes em cada pixel do padrão 2×2 | 512/1.024/2.048 samples corretos; samples não cobertos e padding preservados |
| Resolve de cor 2×/4×/8× | Média esperada no destino simples |
| Depth-only 2×/4×/8× com read/write separados | 512/1.024/2.048 samples corretos |
| IMAGE_LOAD GCN 2×/4×/8× | 256 pixels do sample selecionado em cada caso |
| IMAGE_LOAD em array 4× | 256 pixels da camada/sample selecionados |
| Cor microtiled 4× e posições programáveis | 1.024 samples corretos contra endereço Morton independente |
| Depth 4× com gradiente e posições não padrão | 1.024 profundidades corretas |
| Atomics GCN em pixel/sample shading 4× | 256/1.024 invocações |
| Validação D3D12 | Zero erros; sem avisos na última execução |

Também foi corrigida uma incompatibilidade anterior das views 1D: agora o
backend usa texturas 2D com altura 1, conforme o lowering do recompiler.
Readbacks sem debug não haviam detectado a incompatibilidade dos descritores.

## Limites

MSAA comprimido com FMASK/CMASK/HTILE, EQAA com quantidades diferentes de
samples/fragments e frequência parcial de shading ainda não estão
implementados. Essas configurações são rejeitadas no caminho correspondente.
Depth/stencil MSAA sem compressão está implementado com transfers por plano e
coberto por `Build/native-stencil-msaa-validation.log`; alpha-to-mask usa
`AlphaToCoverageEnable`. O stencil MSAA é lido no canal G (`.y`) do typeless
R32G8X24; cada plano possui heap de descritores shader-visible próprio, pois
draws da mesma lista enxergam o último conteúdo do heap. Logic ops exigem
render target inteiro: neste PC, criar PSO com XOR em UNORM derruba o
processo, então formatos não inteiros são rejeitados com erro explícito.
Resolve de profundidade e formatos sem suporte nativo a resolve não são
anunciados como implementados. A contagem 8× foi validada em roundtrip
depth/stencil; 16× tem checagem de capacidade e é pulada quando o dispositivo
não anuncia quality levels para D32FS8, como neste ambiente.

A fonte [OpenOrbis de leitura MSAA](hardware/msaa/main.cpp) permite comparar
samples, arrays, wrap e microtiling em PS4. Não foi compilada ou executada por
ausência do SDK local. Não há evidência de AppContainer, PS4, Xbox ou gameplay;
o renderer completo e o boot de jogos continuam pendentes.
