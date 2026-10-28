# Conversão de formatos no Dozen

## Inventário do log de Xbox recebido em 2026-10-27

Dispositivo informado: `Microsoft Direct3D12 (SraKmd_arden)`, Dozen 26.2.2.
A lista abaixo registra capacidades ausentes consultadas na inicialização; essas
mensagens, isoladamente, não demonstram que um jogo tenha usado cada operação.

| Formato Vulkan | Capacidades ausentes no log |
| --- | --- |
| `R8Unorm` | `StorageReadWithoutFormat` |
| `R8Snorm` | `StorageReadWithoutFormat` |
| `R8Uint` | `StorageReadWithoutFormat` |
| `R8Sint` | `StorageReadWithoutFormat` |
| `R8Srgb` | `SampledImage | ColorAttachment | TransferSrc | TransferDst` |
| `R16Unorm` | `StorageReadWithoutFormat` |
| `R16Snorm` | `StorageReadWithoutFormat` |
| `R16Uint` | `StorageReadWithoutFormat` |
| `R16Sint` | `StorageReadWithoutFormat` |
| `R16Sfloat` | `StorageReadWithoutFormat` |
| `R8G8Unorm` | `StorageReadWithoutFormat` |
| `R8G8Snorm` | `StorageReadWithoutFormat` |
| `R8G8Uint` | `StorageReadWithoutFormat` |
| `R8G8Sint` | `StorageReadWithoutFormat` |
| `R8G8Srgb` | `SampledImage | ColorAttachment | TransferSrc | TransferDst` |
| `R16G16Unorm` | `StorageReadWithoutFormat` |
| `R16G16Snorm` | `StorageReadWithoutFormat` |
| `R16G16Uint` | `StorageReadWithoutFormat` |
| `R16G16Sint` | `StorageReadWithoutFormat` |
| `R16G16Sfloat` | `StorageReadWithoutFormat` |
| `B10G11R11UfloatPack32` | `StorageReadWithoutFormat` |
| `A2B10G10R10UnormPack32` | `StorageReadWithoutFormat` |
| `A2B10G10R10SnormPack32` | `SampledImage | StorageImage | ColorAttachment | StorageReadWithoutFormat | StorageWriteWithoutFormat` |
| `A2B10G10R10UintPack32` | `StorageReadWithoutFormat` |
| `A2B10G10R10SintPack32` | `SampledImage | StorageImage | VertexBuffer | ColorAttachment | TransferSrc | TransferDst | StorageReadWithoutFormat | StorageWriteWithoutFormat` |
| `R8G8B8A8Unorm` | `StorageReadWithoutFormat` |
| `R8G8B8A8Snorm` | `StorageReadWithoutFormat` |
| `R8G8B8A8Uint` | `StorageReadWithoutFormat` |
| `R8G8B8A8Sint` | `StorageReadWithoutFormat` |
| `R32G32Uint` | `StorageReadWithoutFormat` |
| `R32G32Sint` | `StorageReadWithoutFormat` |
| `R32G32Sfloat` | `StorageReadWithoutFormat` |
| `R16G16B16A16Unorm` | `StorageReadWithoutFormat` |
| `R16G16B16A16Snorm` | `StorageReadWithoutFormat` |
| `R16G16B16A16Uint` | `StorageReadWithoutFormat` |
| `R16G16B16A16Sint` | `StorageReadWithoutFormat` |
| `R16G16B16A16Sfloat` | `StorageReadWithoutFormat` |
| `R32G32B32A32Uint` | `StorageReadWithoutFormat` |
| `R32G32B32A32Sint` | `StorageReadWithoutFormat` |
| `R32G32B32A32Sfloat` | `StorageReadWithoutFormat` |
| `R5G6B5UnormPack16` | `StorageImage | StorageReadWithoutFormat | StorageWriteWithoutFormat` |
| `A1R5G5B5UnormPack16` | `StorageImage | StorageReadWithoutFormat | StorageWriteWithoutFormat` |
| `R5G5B5A1UnormPack16` | `SampledImage | StorageImage | ColorAttachment | TransferSrc | TransferDst | StorageReadWithoutFormat | StorageWriteWithoutFormat` |
| `B4G4R4A4UnormPack16` | `StorageImage | ColorAttachment | StorageReadWithoutFormat | StorageWriteWithoutFormat` |
| `D16UnormS8Uint` | `DepthStencilAttachment` |

## Implementação verificada nesta etapa

- Conversão de entrada de vértices `A2B10G10R10_SINT_PACK32` através de
  `R32_UINT`, com extração e extensão de sinal independentes de RGB e alfa.
- O mesmo conversor atende `A2R10G10B10_SINT_PACK32` e
  `A2R10G10B10_UINT_PACK32`, preservando a ordem de componentes.
- Conversão SNORM 10:10:10:2 limita o resultado inferior a -1, incluindo
  RGB = -512 e alfa = -2. Dividir apenas pelo máximo positivo produzia
  valores inferiores a -1.
- Versão do cache do driver incrementada para descartar shaders anteriores.

O Dozen foi compilado em Release. Cinco execuções nativas no PC, com leitura
do render target de 64 x 64, verificaram os três formatos inteiros e as duas
ordens SNORM. Todas produziram 4096 pixels com o resultado esperado.
Os probes de diagnóstico permanecem em `Build/format-conversion/`, fora das
fontes distribuídas. Isso não constitui validação no Xbox.

## Conversão de imagens ainda pendente

A conversão de vértices não implementa as capacidades de imagem ausentes.
O suporte anunciado para essas operações permanece condicionado à implementação
real e às capacidades do D3D12.

- StorageReadWithoutFormat exige uma representação legível nos dispositivos
  sem UAV_TYPED_LOAD, com conversão de componentes e coerência com escritas
  UAV. Trocar a leitura por SRV sem tratar aliases de leitura/escrita não
  resolve esse contrato.
- R8/RG8 sRGB exigem conversão com filtragem no espaço linear, codificação nas
  escritas de render target e conversão nas transferências.
- Formatos compactados sem representação nativa exigem conversão nas cópias,
  amostragem, armazenamento e attachments, preservando os bits visíveis ao
  aplicativo, os mips, as camadas e as vistas compatíveis.
- D16UnormS8Uint exige conversão por aspecto para uma representação de
  profundidade/stencil suportada, incluindo upload, readback e precisão D16.

Não foi implementado nesta etapa um conversor completo para todas as imagens
listadas no log.
