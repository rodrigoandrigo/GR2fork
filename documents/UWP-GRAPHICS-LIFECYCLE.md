# Inicialização e encerramento gráfico UWP

Atualização de 23/10/2026, após relato de falha ao iniciar novamente no Xbox Series S.

## Padrão de renderização

Vulkan/Dozen é o padrão do host e da API embutida. Na primeira abertura desta
versão, a preferência antiga é migrada para `dozen`, com o marcador
`shadps4.graphics.dozen_default.v1`. Depois disso, a seleção explícita de
`d3d12` continua disponível e é respeitada. Uma falha ao persistir a migração
não impede usar Dozen na sessão atual.

## Correção de encerramento

Um diagnóstico no PC reproduziu uma referência de apresentação retida:
`SURFACE_STILL_ATTACHED_AFTER_SHUTDOWN=FAIL`. O host Dozen destruía os objetos
Vulkan sem remover a swapchain anexada ao painel. O painel e a referência de
composição do host podiam manter o dispositivo e a fila D3D12 vivos.

O encerramento agora para os produtores, libera VideoOut, rasterizador,
presenter e bootstrap, e chama o callback com swapchain nula antes de remover
os callbacks e descarregar o driver. A operação é idempotente e também cobre
falhas durante a inicialização. Uma falha de detach é reportada pelo shutdown,
permitindo nova tentativa em vez de confirmar uma liberação incompleta.

O destrutor do host mantém um apartamento COM válido durante a limpeza. O
worker da interface encerra seu MTA antes que a última referência do host seja
destruída; a limpeza não pode depender desse apartamento já encerrado.

## Validação no PC

- Antes da correção: apresentação e resize passaram, mas a superfície continuou
  anexada após o shutdown (`Build/graphics-relaunch/before.log`).
- Depois: oito ciclos consecutivos de inicialização, apresentação, resize e
  shutdown, com `SURFACE_DETACHED=PASS` e `SHUTDOWN=0` em cada ciclo
  (`Build/graphics-relaunch/after.log`).
- Mais oito ciclos com falha de attachment injetada antes de cada inicialização
  válida. Rollback sem superfície retida, nova tentativa e encerramento passaram
  (`Build/graphics-relaunch/retry-validation.log`).
- Núcleo e host Release x64 compilados, bundle assinado (`CMS_SIGNATURE=PASS`)
  e payload do executável/DLLs conferido contra as saídas de produção
  (`Build/graphics-relaunch/final-host-build.log` e `package-verification.log`).

Os diagnósticos usam o núcleo de produção em execução nativa no PC e um callback
de attachment controlado. Não exercitam o painel XAML no Xbox nem comprovam que
o erro relatado no Series S tenha exatamente a mesma causa. A nova versão ainda
precisa de abertura, fechamento e segunda abertura no console.

## Menu do Xbox e perda do dispositivo

O usuário confirmou Pac-Man jogável no Series S, mas relatou falha ao abrir o
menu do Xbox para fechar o aplicativo e na abertura seguinte. Os logs
`frontend-d3d12.log` e `frontend-d3d12 (1).log` registram, nessa ordem:

- Fence D3D12 com `0x887A0007` e o mesmo motivo de perda do dispositivo.
- Mensagens 527 de estado anterior incorreto em barreiras, incluindo buffers
  em vertex/constant e shader resource tratados como COMMON.
- Mensagens 834 de falha de memória do kernel. O uso local informado era
  660.082.688 bytes, com orçamento de 3.986.266.522 bytes; esse registro sozinho
  não comprova esgotamento do orçamento de memória de vídeo.
- Na nova abertura, `Dozen did not expose a D3D12 adapter`.

O estado ativo do host agora pausa as threads convidadas também no Dozen.
A pausa efetiva combina pausa manual e inatividade; recuperar o foco não
desfaz uma pausa manual. A transição de inicialização para convidado pronto
aplica a pausa sob o mesmo mutex, evitando uma retomada seguida de pausa
atrasada. A interface também acompanha `CoreWindow.VisibilityChanged`.

No Dozen, cada command buffer passa por uma chamada separada a
`ExecuteCommandLists`. A implementação depende dessa fronteira tanto para o
decay de buffers para COMMON quanto para a ordenação entre listas em eventos.
Agrupar as listas em uma única chamada não garante essas propriedades.
Isso não corrige por si só possíveis barreiras inválidas dentro de uma lista.
A enumeração de adaptadores também preserva o resultado Vulkan de falha,
distinguindo-o de uma enumeração bem-sucedida sem adaptadores.

A confirmação de que essas mudanças resolvem o reset e a segunda abertura
continua dependendo de repetir a sequência no Series S. Não há recuperação
garantida de um dispositivo que já sofreu reset do driver.

Validação desta revisão no PC:

- Convidado sintético: pausa por inatividade, retomada automática e preservação
  de pausa manual passaram. Quatro ciclos de execução/encerramento/descarregamento
  da DLL passaram, incluindo saída normal e falha convidada intencional
  (`Build/graphics-relaunch/menu-pause-validation.log`).
- Oito ciclos de inicialização, rollback de attachment com falha injetada,
  apresentação, resize e shutdown passaram sem superfície retida
  (`Build/graphics-relaunch/menu-relaunch-validation.log`).
- Desenho GCN com MSAA 4x no Dozen atualizado: 4.096 pixels vermelhos esperados
  (`Build/graphics-relaunch/menu-msaa-validation.log`).
- Release x64 compilado, bundle assinado com `CMS_SIGNATURE=PASS` e payload
  do host, núcleo e Mesa conferido contra produção
  (`Build/graphics-relaunch/xbox-menu-final-build.log` e
  `Build/graphics-relaunch/menu-package-verification.log`).
