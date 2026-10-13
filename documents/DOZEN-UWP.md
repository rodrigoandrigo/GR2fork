# shadPS4 Vulkan → Dozen → D3D12 → shadPS4-UWP

O build UWP usa o renderer Vulkan e o Dozen da cópia em `externals/mesa-uwp`.
O renderer D3D12 experimental anterior permanece no fonte, mas deixou de ser
compilado no núcleo UWP. O frontend ImGui continua usando seu dispositivo D3D12
e um painel separado para o overlay.

## Build e carregamento

`scripts/build-uwp.ps1` compila Dozen com Meson, MSVC, `_XBOX_UWP`,
`WINAPI_FAMILY_APP` e CRT Store, sem adicionar uma suíte de testes ao projeto.
SDL3 continua vindo de `C:\Users\rodri\Dev1\Projetos\SDL3-uwp`.
O Dozen é compilado em Release também quando o host usa Debug.

O núcleo carrega `vulkan_dzn.dll` do pacote e inicializa Vulkan-Hpp diretamente
com `vk_icdGetInstanceProcAddr`. Não depende de `vulkan-1.dll`, registro de ICD
nem variáveis de ambiente. `dxil.dll` fornece validação/assinatura de DXIL;
`dxcompiler.dll` é incluída para diagnósticos. O pacote é assinado com o PFX
configurado em `shadPS4-UWP`.

Em diagnósticos fora de um pacote, o carregamento fica restrito às DLLs ao lado
do núcleo e ao D3D12 do diretório de sistema. Essa alternativa só é usada quando
o processo não possui identidade de pacote.

## Adaptação do renderer

A definição Win32 de Vulkan-Hpp é compartilhada por todos os arquivos do núcleo
UWP, mantendo o mesmo layout da tabela de dispatch. A checagem de ambiente usa
um dispatcher local e funciona antes da inicialização e durante a apresentação.


- Vulkan 1.2 e SPIR-V 1.5 no UWP, com extensões para dynamic rendering,
  synchronization2, subgroup size control e estados dinâmicos.
- Descritores nulos materializados no Dozen, incluindo os caminhos de atualização
  normal, templates e buffers dinâmicos. O modo de descritores com limites evita
  o caminho bindless quando `nullDescriptor` está habilitado.
- Buffers esparsos usam recursos D3D12 reservados e tiles de 64 KiB. Bind,
  rebinding, aliases e unbind passam pela fila comum do Vulkan, preservando
  semáforos timeline e fences. O suporte depende de Tiled Resources Tier 2 e
  dos limites de endereço do dispositivo; imagens esparsas não são anunciadas.
- O cache de buffers consulta requisitos criando um buffer temporário, sem
  exigir `VK_KHR_maintenance4`.
- O runtime Dozen mantém os dados usados pelos comandos indiretos em root
  constants e coloca o array de tamanhos dos viewports em um CBV de upload
  separado. Isso evita ultrapassar o limite de 64 DWORDs da root signature.
  A versão de layout invalida os caches anteriores.
- O painel do host substitui a janela SDL. O renderer guest apresenta diretamente
  ao swapchain do Dozen, sem criar um segundo contexto ImGui dentro da DLL.

## Ordem dos frames

O host seleciona exclusivamente FIFO. O Dozen usa `FLIP_SEQUENTIAL` e
`Present1` com intervalo 1, sem mailbox, tearing ou limitador extra de FPS.
A tela mantém seu limite físico de refresh e aplica backpressure.

VideoOut possui um consumidor FIFO separado do temporizador de vblank emulado.
Com `flip_rate=0`, os frames são processados assim que chegam. Uma cadência
explicitamente solicitada pelo guest continua sendo respeitada. Recriações de
swapchain retêm o frame pendente para nova tentativa; frames novos só voltam
à fila livre depois da submissão, com fence protegendo sua reutilização.

## Validação e limites

Os diagnósticos nativos de PC exercitam shaders GCN traduzidos pelo recompiler
do projeto e executados através de Vulkan/Dozen/D3D12. Eles cobrem draw e
dispatch diretos e indiretos, MSAA 4× com resolve, descritores nulos e buffers
esparsos. O diagnóstico da DLL cobre contadores de apresentação consecutivos,
resize FIFO e shutdown. Os logs estão em `Build/dozen-*-validation.log`.

A validação final de 12/10/2026 passou com as DLLs de Runtime/Release que foram
conferidas por hash dentro do bundle assinado. Foram confirmados nove presents
consecutivos, resize para 96×64 com FLIP_SEQUENTIAL, shutdown, unload e checagem
do ambiente durante a apresentação. A ABI conserva seus 39 exports públicos;
a configuração expõe 135 opções e preserva 32 excluídas. A assinatura CMS e o
signer do PFX foram conferidos em `Build/dozen-signature-validation.log`.

Essas evidências não equivalem a gameplay, scanout físico, execução em
AppContainer ou Xbox. O pacote não é instalado pelo build.

O boot guest público usa os mounts brokered e está ligado ao frontend.
[BOOT-UWP.md](BOOT-UWP.md) descreve a parada, a recarga do núcleo e a validação
nativa do ciclo. Essas alterações não equivalem a suporte completo a jogos PS4.

O caminho UWP exige `robustImageAccess2` e `nullDescriptor`, assim como o
renderer desktop. O Dozen verifica os limites de views, mipmaps, layers e
samples e usa metadados por descritor para substituir os componentes ausentes
com o valor exigido pelo formato, inclusive quando o shader declara formato
Unknown. [ROBUSTNESS2-DOZEN.md](ROBUSTNESS2-DOZEN.md) registra a implementação e
a validação GPU. As features de robustness2 seguem a disponibilidade da
extensão e de root signature 1.1.

Acesso direto por BDA/DMA fica desativado neste perfil. Validação Vulkan desktop
e RenderDoc não fazem parte do pacote. `largePoints`, imagens esparsas e outras
capacidades ausentes continuam limitadas pelo Dozen.

O fonte OpenOrbis em [hardware/fifo](hardware/fifo/README.md) permite comparar
IDs de frames com captura física. Ainda não foi compilado nem executado em PS4.
