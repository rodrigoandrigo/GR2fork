# Renderer D3D12: estado de implementação

O renderer completo de jogos PS4 **não está implementado**. O boot continua
desabilitado. Não se presume suporte a jogos por compilar a DLL, apresentar
ImGui, copiar buffers ou executar shaders autorais do projeto de origem.

## Integração feita

- Snapshot local em `externals/d3d12-native`, com licença e SHA256SUMS.
- Device context, allocator, encoder, pipeline cache e VideoCore bridge
  compilados diretamente dessa cópia. O frontend usa o mesmo snapshot local.
- Liverpool recebe `VideoCore::GpuCommandSink`, sem incluir o rasterizador
  Vulkan. O Vulkan implementa o contrato e conserva os hooks de packet run,
  fences, markers e offsets SGPR dos draws indiretos deste fork.
- A validação de ambiente da DLL exercita fill e download/readback D3D12
  pela mesma interface de comandos.

## Caminhos de jogos ainda necessários

| Caminho | Situação atual | Trabalho necessário |
| --- | --- | --- |
| PM4/Liverpool | Contrato de comandos independente do Vulkan | Criar e ligar o rasterizador D3D12 com registradores graphics/compute e reportar erros de submissão |
| Memória guest | MemoryManager ainda aponta para Vulkan::Rasterizer | Adaptar map/unmap/register/invalidate, dirty tracking, aliases e download antes de sinalizar fences |
| Buffers | Bridge nativa faz transferências de buffers registrados | Cache ligado à MMU, GDS, buffers raw/typed, uploads/downloads e retirement por fence |
| Imagens | TextureCache e detiling ainda ligados ao Vulkan | Formatos PS4, mips/layers, render targets, depth/stencil, views, samplers, MSAA/resolve e coerência com buffers |
| Shaders GCN | Recompiler atual emite SPIR-V para Vulkan | Perfil D3D12, conversão completa para HLSL/DXIL, reflexão e bindings coerentes com os recursos guest |
| Tradutor copiado | VS/PS/compute restritos, sem recursos gerais de jogos | Suportar recursos, builtins, estágios e operações reais; validar divergências entre SPIR-V e DXIL |
| Pipelines | PSO nativo inicial cobre triângulo e um color target | Blend, múltiplos targets, depth/stencil, rasterização, topologia, input layout, MSAA e chave completa do cache |
| Draw/dispatch | Bridge copiada retorna E_NOTIMPL | Direct/indirect draws e dispatches, indexed/instanced e parâmetros SGPR |
| VideoOut | Driver usa Vulkan::Presenter e Vulkan::Frame | Apresentação D3D12 de superfícies guest, flip labels/events, blank/last frame e suspensão |
| Validação | Build e diagnóstico nativo da fundação | Homebrew OpenOrbis em PS4, comparação de resultados, guest real e execução no AppContainer/Xbox |

O limite é explícito no código de origem: `D3D12VideoCoreBridge::Draw`,
`DrawIndirect`, `DispatchDirect`, `DispatchIndirect`, `ReadDataFromGds` e
`ProcessDownloadImages` rejeitam operações ainda não implementadas.
`spirv_hlsl_bridge.cpp` rejeita texturas/buffers e recursos fora do seu contrato
restrito. Essas rejeições não foram substituídas por no-ops ou draws fictícios.

## Compilação

`scripts/build-uwp.ps1` usa a cópia local por padrão, preservando o SDL3 externo
selecionado pelo usuário. `-D3D12Source` é um override explícito compartilhado
pelo núcleo e pelo host. O pacote segue assinado pelo certificado UWP fornecido.

O progresso descrito aqui é preparação da integração. Não fecha o pedido de
renderer completo nem demonstra execução gráfica de jogos.
