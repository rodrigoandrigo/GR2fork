# Robustness2 no Dozen UWP

O shadPS4 exige `robustBufferAccess2`, `robustImageAccess2` e `nullDescriptor`.
A exceção UWP que aceitava apenas `robustImageAccess` foi removida. O driver
expõe as features de robustness2 quando a extensão está disponível, com root
signature 1.1 ou superior.

## Implementação

O lowering NIR verifica coordenadas contra as dimensões da view, incluindo
layers, LOD relativo à view e índice de sample. Leituras fora dos limites usam
zero para componentes presentes e os valores de substituição dos componentes
ausentes. Stores são ignorados e atomics retornam zero sem alterar a imagem.
A distinção entre R/RG e RGBA é exigida pela [especificação Vulkan](https://docs.vulkan.org/guide/latest/robustness.html).

Os heaps de descritores conservam uma máscara de substituição por slot,
derivada do formato real da image view ou buffer view e de seu component
mapping. Descritores nulos têm máscara zero. O dado acompanha escritas, cópias
e templates; não depende do formato declarado no shader.

Ao gravar comandos, a máscara é copiada para um buffer de upload com a mesma
vida útil do command buffer. Um SRV na tabela de descritores existente dá ao
shader acesso à máscara. Isso não aumenta o custo máximo em DWORDs da root
signature nem altera as root constants usadas pelos comandos indiretos.

A tradução dos bindings conserva o offset de cada recurso, incluindo arrays e
prefixos de outros descriptor sets. O hash do cache NIR incorpora esses offsets.
A versão do layout do pipeline cache foi incrementada para invalidar entradas
anteriores. Dispositivos que habilitam robustez de imagens usam descriptor tables
com limites, em vez do caminho bindless.

## Validação de 13/10/2026

Diagnósticos nativos executaram o driver UWP em D3D12 na AMD Radeon RX 6600M:

- 81 casos em três fases, com 324 componentes conferidos por fase, sem divergências.
- Formatos float R32/RG32/RGBA32 e uint R32/RGBA32, inclusive image format Unknown
  em imagens readonly e de leitura/escrita.
- Coordenadas negativas e grandes, limites de views, mipmaps restritos, layers,
  imagens 1D/2D/3D, sampled MSAA 4× e texel buffers.
- Stores e atomics fora dos limites, preservação do conteúdo válido e controles
  com stores e atomics dentro dos limites.
- Arrays de descritores com formatos diferentes e mudanças por escrita, cópia e
  template. O mesmo shader foi reutilizado em pipeline cache com prefixos de
  descriptor sets diferentes e retorno ao layout anterior.
- 21 verificações de descritores nulos com robustImageAccess2 habilitado,
  incluindo o alpha de imagens, texturas e texel buffers.
- Fragment shader com fetch, image load e descritor nulo: 4.096 pixels corretos.

Os fontes dos diagnósticos ficam no diretório ignorado Build, sem adicionar uma
suíte de testes ao repositório. Logs: `Build/robust2-*-validation.log`.

O build Release do núcleo e do frontend passou. A inicialização real do renderer
na DLL passou com a exigência estrita das três features, seguida de apresentação
FIFO, resize e shutdown. As DLLs dentro do bundle correspondem por hash ao
runtime; o bundle foi assinado com o PFX do projeto e a assinatura CMS foi
verificada. Logs adicionais: `Build/robust2-final-package.log`,
`Build/robust2-package-validation.log` e `Build/robust2-host-validation.log`.

Os resultados comprovam os casos executados no PC. Não substituem Vulkan CTS,
execução em AppContainer, gameplay, scanout físico ou testes no Xbox. A integração
posterior do boot brokered e da parada guest está descrita em [BOOT-UWP.md](BOOT-UWP.md).
