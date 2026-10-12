# Renderer D3D12: estado de implementação

Existe um renderer guest experimental ligado ao Liverpool. O renderer completo de jogos PS4 ainda não está concluído, e o boot de jogos permanece desabilitado. Nenhum resultado abaixo comprova execução de jogos, AppContainer ou Xbox.

## Caminhos implementados

| Caminho | Implementação atual |
| --- | --- |
| Comandos | GpuCommandSink, rasterizador D3D12 próprio e registradores Liverpool; draws diretos, indexed/instanced, expansão CPU de draws indiretos e dispatch direto/indireto |
| Shaders | Recompiler GCN existente → SPIR-V → SPIRV-Cross HLSL → DXC DXIL; VS/PS/CS, reflexão de recursos, base vertex/instance, compute wave64 e constantes de número de workgroups |
| Buffers | Recursos raw/typed via IR, agrupamento de views sobrepostas, identidade física para aliases contíguos, snapshots CPU, upload/download e writeback nas views guest originais; flat buffer, clip planes, GDS e spill LDS |
| Imagens | Texturas 1D/2D/3D, mips e layers, views de layer único/arrays, formatos comuns e BC, samplers, CPU tiling/detiling, MRT e profundidade simples; read address e write address de depth separados |
| Pipelines | Root signatures por reflexão; cache limitado com chave dos shaders, bindings, input layout e estados usados; rasterização, blend, máscaras, topologias básicas, viewport e scissor |
| Coerência | Upload antes do comando, download após conclusão da fence e antes do retorno; lease de memória guest protege map/unmap; aliases incompatíveis dentro do mesmo comando são rejeitados |
| VideoOut | Contrato de apresentação compartilhado com adaptador Vulkan desktop; frame D3D12 com snapshot imutável, detiling/swizzle/gamma SDR, blank/last frame, eventos existentes e swapchain do painel de jogo UWP |
| Embedding | Exceções de GPU/VideoOut tornam-se erro da sessão; thread de apresentação MTA; UWP não cria janela SDL nem executa seu loop de eventos desktop |
| Frontend | Painéis separados de jogo e ImGui, composição com alpha premultiplicado e overlay controlado pela thread de renderização |

As cópias locais ficam em `externals/d3d12-native` e `externals/spirv-cross`. O SDL3 continua vindo da pasta UWP indicada pelo usuário. A bridge restrita copiada do projeto de origem não traduz shaders guest: o caminho de jogos usa `src/video_core/renderer_d3d12`, o recompiler existente e o SPIRV-Cross completo. Não há Dozen ou tradução Vulkan→D3D12 neste caminho de renderização.

## Limites de recursos e sincronização

- Buffers: 128 entradas e orçamento GPU de 256 MiB; view de até 16 MiB.
- Imagens: 64 entradas, orçamento GPU de 256 MiB e snapshots CPU de 256 MiB; superfície guest de até 64 MiB.
- Shaders: 512 entradas e 64 MiB; pipelines: 128 entradas e 32 MiB.
- Bindings por comando: 512 descritores de recursos e 128 samplers.
- VideoOut: frames e uploads limitados por orçamento de 256 MiB.
- Spill LDS: até 16 MiB por dispatch, separado por workgroup.

O caminho atual espera a GPU e faz readback a cada draw/dispatch. Isso favorece coerência e diagnóstico, mas não é uma implementação de desempenho aceitável para jogos. Falta substituir essa serialização por tracking de páginas e submissões assíncronas, preservando fences, aliases e writes CPU concorrentes. Não é permitido reutilizar o runtime após falha de gravação de comandos.

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

## Trabalho pendente para jogos

- Geometry/tessellation, wave64 em graphics e primitivas que exigem emulação.
- MSAA/resolve, stencil, depth bounds, HTILE/CMASK/FMASK e caminhos de compressão/fast clear.
- Views simultâneas de imagem/buffer, feedback SRV/UAV, aliases de layouts incompatíveis, backing físico segmentado e dirty tracking completo.
- Cobertura completa de formatos, estados de rasterização e operações IR.
- HDR e apresentação float; validação visual do VideoOut em AppContainer.
- Boot brokered completo, teardown dos threads guest e ciclo de execução de jogos embutidos.
- Comparações OpenOrbis/PS4 para tiling, shaders, offsets de todos os buffers, memória compartilhada, draws e fences; depois gameplay no PC e Xbox.

Estados ainda não suportados geram erro explícito da sessão. Compilar e executar estes casos básicos não encerra o pedido de renderer completo.



