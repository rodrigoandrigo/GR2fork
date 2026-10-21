# Aproveitamento do renderer D3D12 de pt-pc-main

Referência local analisada em 20/10/2026:
`C:/Users/rodri/Dev1/Projetos/pt-pc-main/src/engine/render/d3d12` (31 arquivos).
O projeto de referência declara licença MIT em sua raiz. As mudanças no shadPS4
aplicam os padrões abaixo às abstrações existentes do backend nativo.

## Implementado

| Referência | Aplicação no renderer nativo |
| --- | --- |
| `backend/d3d12_util.cpp`, `d3d12_internal.h`: heaps persistentes de descritores | Quatro heaps por CommandRuntime: 512 CBV/SRV/UAV, 128 samplers, oito RTV e um DSV. ResourceBindings toma emprestados os heaps após conferir a fence; os índices usados continuam determinados pelos shaders guest. Não se cria mais um par de heaps por draw/dispatch nem heaps RTV/DSV por draw. |
| `backend/d3d12_device_impl.cpp`, `d3d12_command_list.cpp`: intervalo CPU sujo de buffers | Comparação com o snapshot encontra o primeiro e último byte alterados. Map/Unmap e CopyBufferRegion cobrem somente esse intervalo. Primeira carga, tamanho diferente e mudança de mapeamento preservam a cópia integral. UploadStatistics informa bytes, cargas integrais e parciais. |
| Pools de queries e frequência da fila em `d3d12_device_impl.cpp` | Com GAME_PROFILE, queries timestamp cercam o batch GPU e são resolvidas para readback. O resultado só é lido depois da fence; GpuTimings informa frequência, ticks e número de submissões. Sem suporte, o recurso opcional é desativado. Sem GAME_PROFILE, não são alocadas queries/readback. |
| `present/d3d12_device.cpp`: espera e remoção do dispositivo | Esperas pela fence verificam conclusão e remoção a cada 500 ms. O intervalo é de consulta, não um limite que cancela trabalho válido. Um evento antigo não é tomado como prova de conclusão de uma fence nova. |
| `Device::Destroy`, `DeviceImpl::WaitIdle` e destrutores dos recursos de upscale | CommandRuntime e D3D12Presenter drenam suas filas antes da destruição dos membros. O device context também tenta concluir a fila antes de fechar seu evento. Falha de dispositivo não escapa do destrutor. |
| `Device::LogDred` e orçamento em `d3d12_device_impl.cpp` | Falhas de memória/remoção incluem uso, orçamento e reserva dos segmentos DXGI disponíveis; DRED inclui até oito contextos por breadcrumb. Interfaces ausentes não passam a ser requisito do Xbox. |

Os tempos GPU medem o batch inteiro, incluindo uploads, renderização e cópias de
readback gravadas nele. Não substituem FPS, latência de apresentação ou medição
em gameplay. A comparação de bytes continua lendo o backing CPU: esta mudança
reduz a transferência GPU, sem prometer eliminar as leituras CPU.

A gravação e o writeback permanecem síncronos. A reutilização dos descritores
foi adaptada a esse contrato: cada runtime usa slots fixos e só os sobrescreve
após a fence. O caminho standalone de ResourceBindings continua podendo possuir
seus próprios heaps. O orçamento e o pool de transferências existente são mantidos.

## Funcionalidade equivalente já existente

| Família da referência | Situação local |
| --- | --- |
| Formatos tipados, depth typeless/SRV, transições e UAV barriers em util/command_list | ConvertFormat e os caches nativos já abrangem formatos guest e estados. Não se troca o conjunto guest pelo conjunto menor do renderer de jogo. |
| Staging, readback com row pitch e recursos retidos por fence | TransferPool, ResourceCache, ImageCache e tiling já usam fences, footprints e preservação de padding; as validações continuam cobrindo esses caminhos. |
| Cache de PSOs e root signatures, validação das interfaces de shader | Cache nativo e reflexão/remapeamento dos shaders já existem. A root signature varia com os shaders guest; a assinatura global fixa do jogo não é um contrato adequado para eles. |
| HDR, color spaces e apresentação por cópia | VideoOut nativo já converte SDR/FP16/PQ para a saída apropriada, usa scRGB na swapchain FP16, verifica suporte ao color space e faz captura residente compartilhada. |
| Recursos/fences compartilhados e identificação por LUID | Captura residente e abertura de handle já existem. DeviceContext aceita adaptador explícito. A interoperabilidade Vulkan/D3D12 de `vk_d3d12_share.cpp` não é necessária para executar o caminho D3D12 nativo. |
| Alocação com orçamento | O allocator local limita committed resources pela informação de alocação D3D12, e os caches/pools impõem limites adicionais. D3D12MA é uma dependência externa da referência; sua substituição requer comparar fragmentação e reservas no Xbox antes de alterar esse contrato de orçamento. |

## Componentes que exigem dados ou contratos do renderer de jogo

- `d3d12_raytracing.cpp` constrói BLAS/TLAS a partir da geometria e instâncias da
  cena. A execução GCN/PM4 não fornece automaticamente essa cena de alto nível.
- `d3d12_upscale.cpp`, `fsr_ray_regeneration.cpp`, `d3d12_neural.cpp` e
  `d3d12_neural_codec.h` dependem dos contratos de depth, motion vectors,
  materiais, exposição, câmera e histórico do jogo, além dos respectivos SDKs.
- `d3d12_fsr_fg.cpp`, `d3d12_frame_generation.cpp`, `sl_dlssg_presenter.cpp` e
  `xefg_presenter.cpp` usam depth/motion, HUD separado e parâmetros de câmera.
  Suas swapchains têm HWND e proprietários externos. A janela UWP usa composição.
- `agility_exports.h`, carregamento desktop e `reshade_native.h` pertencem ao
  bootstrap/integração desktop. A solução local mantém APIs UWP e SM 6.4 no Xbox.
- Frames concorrentes e liberação assíncrona da referência dependem de ausência
  das obrigações de readback e leases guest atuais. O pool de transfers já retém
  recursos até fences, mas o writeback não pode ser adiado sem ajustar coerência.

Esses componentes não são anunciados como capacidades do emulador. Identificar
buffers de câmera/movimento por título ou shader violaria a regra contra hacks.
A restrição comprovada dos blobs de PSO do driver no Xbox permanece: cache RAM
ativo, DXIL persistente, sem GetCachedBlob/CachedPSO em Windows.Xbox.

## Validação

- Diagnóstico privado no PC: buffer de 1 MiB com alteração de quatro bytes
  transfere quatro bytes, preserva os demais dados e força nova carga integral
  quando o mapeamento muda. Duas cargas integrais e duas parciais totalizaram
  2.997.154 bytes de cópia GPU nos quatro casos.
- Os quatro heaps mantiveram a identidade em 96 ciclos de desenho. As cópias CPU
  das imagens residentes continuaram sem capacidade retida ao final dos ciclos.
- Regressão SM 6.4 com GPU-based validation: feedback, aliases físicos/virtuais,
  atômicos, subwords, tiling, depth, geometria, tesselação e MSAA passaram com
  zero erros D3D12. Queries resolveram 1.529 submissões, frequência 100 MHz.
- O caminho sem GAME_PROFILE também passou dispatch, uploads parciais, desenhos,
  wireframe e reutilização dos heaps, com zero erros D3D12.
- Apresentação residente: 72 referências CPU (seis formatos, três gammas, SDR/HDR,
  duas disposições), 79 capturas, zero downloads e zero erros D3D12; inclui resize
  e troca de formato, com destruição dos recursos ao final.
- Remoção sintética do device durante espera por fence pendente: falha detectada
  em 111 ms, orçamento DXGI registrado e mensagens de erro preservadas.

Evidências privadas em Build:
`pt-d3d12-adaptations-gpu-validation.log`,
`pt-d3d12-adaptations-default-profile-validation.log`,
`pt-d3d12-resident-presentation-validation.log`,
`pt-d3d12-removal-wait-validation.log`.

Essas evidências não medem ganho de FPS nem validam o novo código no Xbox. A
chegada à tela inicial no Series S foi confirmada pelo usuário numa revisão
anterior; gameplay prolongado continua sendo uma validação separada.

O core e o host UWP compilaram em Release x64. O bundle passou assinatura CMS,
comparação das seis DLLs e do EXE com os payloads de build, marcadores de
diagnóstico e correspondência com o core de produção
(`Build/pt-d3d12-adaptations-uwp-build.log`,
`Build/pt-d3d12-adaptations-package-verification.log`). O pacote não foi instalado.
