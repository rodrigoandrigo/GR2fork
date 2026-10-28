# Diagnóstico de mapeamento de memória e transições da interface

A revisão `memory_rootcause_v1` acrescenta telemetria para investigar a recusa
de mapeamento seguida da falha em `XboxUI.Api.dll`. Não altera os limites ou
as decisões de recuperação de memória.

## Coleta no Xbox

Reproduza a inicialização que falhou e preserve os quatro arquivos da mesma
execução: `shad_log.txt`, `shadps4.log`, `uwp-session.log` e `uwp-crash.log`.
Eles continuam em `LocalState/shadPS4/log`. O início da sessão identifica
`diagnostics_revision memory_rootcause_v1`; os registros têm horário, thread
e número de sequência.

## Mapeamentos

As linhas `[MemDiag] failure=...` distinguem:

- `swap_file_create`: criação do arquivo de swap.
- `ram_section_create`: criação da seção; o tamanho informado é o da seção,
  que pode ser 16 MiB mesmo quando o caller deseja uma view de 64 KiB.
- `guest_alias_map_from_app` e `guest_alias_map_native`: criação do alias,
  distinguindo o caminho FromApp do caminho com permissão executável.
- `backing_view_map`: criação da view canônica do backing.
- `backing_placeholder_split`: divisão do placeholder.
- `store_view_map`: acesso temporário ao armazenamento.

Cada recusa inclui o erro Win32 capturado antes dos diagnósticos, endereço
virtual e físico, tamanho, offset, proteção, handle, flags e o estado do
endereço retornado por `VirtualQuery`. As primeiras 32 recusas são detalhadas;
depois, uma a cada 64. As primeiras oito também incluem snapshots completos.
Uma falha terminal sempre emite o snapshot, mesmo após atingir esse limite.

Os snapshots registram views, seções, arquivos, regiões fixadas e executáveis,
orçamento do pager, consumo/limite UWP e disponibilidade de memória do sistema.
Bytes de aliases e backing são informados separadamente; somá-los não mede
RAM física residente. `system_ok` e `system_win32` indicam se a consulta do
sistema pôde ser realizada. Erro Win32 8 sozinho não identifica qual recurso
foi esgotado.

`recovery` registra candidatos, seções protegidas que não puderam ser retiradas,
quantidade liberada, falhas de I/O, primeiro erro Win32 e duração da tentativa.

## Paginador

Uma amostra a cada cinco segundos informa uso, limite, cobertura física,
outros consumidores estimados, orçamento, número de views e decisão de pressão.

- `pager_lock_busy`: a tentativa não conseguiu obter o bloqueio.
- `pager_budget_failed`: a consulta WinRT falhou; inclui HRESULT e contador.
- `tick`, `lock_skips` e `budget_errors`: contadores cumulativos.
- `evictions`, `second_chances`, `emergency` e `store_copies`: ações realizadas.

As mensagens `[MemDiag]` também são encaminhadas ao diário persistente
`uwp-session.log`. O erro original de mapeamento é preservado após a coleta.

## Interface e exceção nativa

O diário marca entrada e saída de recarga, shutdown, descarregamento da DLL,
inicialização, associação do swapchain, integração com SDL e operações de
habilitação/foco da página do ponteiro.

O crash log registra as últimas etapas da interface e do núcleo, todos os
registradores gerais, 32 bytes a partir de RIP e até 32 frames da pilha original
da exceção. Os frames incluem PC, base do módulo, RVA e caminho quando disponível.
Os words dos bytes de instrução são gravados em ordem little-endian.
A caminhada é interrompida quando não pode continuar com segurança.

A última etapa registrada localiza uma operação em andamento ou concluída;
ela não prova, por si só, que essa operação causou a falha. A correlação deve
usar thread, sequência, pilha e módulo. Os diagnósticos não suprimem a exceção.

## Verificação da revisão

Núcleo e frontend UWP Release compilados; pacote assinado com assinatura CMS e
hashes do payload conferidos, sem instalação. O verificador nativo existente
capturou pilha e bytes de instrução de uma exceção controlada, preservou o
diário após término forçado e manteve os 256 registros concorrentes.
As verificações existentes de mapeamento passaram, incluindo recusas injetadas
com erro Win32 8. O boot do Bloodborne no PC e o descarregamento/recarga do
núcleo passaram com as amostras `[MemDiag]` ativas. A próxima ocorrência no Xbox
é necessária para identificar a causa específica dessa plataforma.

O mapa do núcleo, o PDB da interface e os binários correspondentes foram
preservados em `Build/rootcause-diagnostics-symbols`, com manifesto SHA-256.

## Correções da revisão v2

Os logs do Xbox mostraram recusas em `guest_alias_map_from_app`, com seções
criadas, placeholders reservados e consultas de orçamento bem-sucedidas.
Assim, a recuperação agora pode mapear a mesma seção com `MapViewOfFile3`,
mantendo endereço, flags e proteção de dados. Uma recuperação bem-sucedida
seleciona esse caminho para as próximas views de dados; se ele falhar, o
caminho FromApp continua disponível. O erro primário é preservado quando
ambas as tentativas falham. A política vale também para o backing e para
views temporárias do armazenamento, sem criar cópias privadas dos aliases.

Na janela mínima, falhas de recursos ainda permitem até oito rodadas de
recuperação com espera limitada entre tentativas. O início de novas rodadas
é limitado a 250 ms; operações de I/O já em andamento podem levar mais tempo.
Os eventos `native_map_recovered`, `native_map_retry_failed` e
`guest_map_recovered` permitem conferir o resultado no Xbox.

A pilha nativa foi simbolizada com o SDL correspondente ao pacote anterior:
`WINTRT_InitialiseInputPaneEvents`, `SDL_winrtkeyboard.cpp:142`, durante a
reinscrição de `InputPane.Showing`. A implementação não guardava os tokens
dessas inscrições para removê-las antes de descarregar a DLL.

O SDL externo em `C:/Users/rodri/Dev1/Projetos/SDL3-uwp` agora guarda e remove
os tokens de `InputPane` e dos eventos do painel XAML. A associação repetida
ao mesmo painel é idempotente. O host chama o novo export
`SDL_WinRTDetachXAMLPanel` na thread da interface, finalizando o SDL antes de
descarregar o núcleo. Se a remoção falhar, o núcleo é retido e a recarga é
interrompida. O pacote exige o SDL atualizado junto do núcleo.

O identificador da sessão desta versão é `memory_rootcause_v2`. A confirmação
do comportamento corrigido no Xbox depende da execução desse pacote no console.

Na revisão v2, núcleo, SDL e frontend UWP Release compilaram. Passaram os
verificadores nativos existentes com swap ligado/desligado, recusas de
mapeamento, cota de 32 views e cota de 16 seções. O Bloodborne completou 30
ciclos do harness no PC, apresentou 830 frames, encerrou normalmente e permitiu
descarregar/recarregar o núcleo, com zero mensagens na fila de diagnóstico D3D.
Essa execução usa um painel nativo do harness; não exercita os eventos reais
de `InputPane`/XAML do Xbox.

A assinatura CMS e os hashes do núcleo e do SDL dentro do pacote passaram na
conferência. Não houve instalação. Os binários, mapa do núcleo e PDBs do host
e do SDL desta revisão estão em `Build/rootcause-fix-v2-symbols`.

## Correções da revisão v3

O log v2 avançou além da falha anterior, mas terminou no processador de comandos
da GPU ao ler `0x6000d70d7000`. Os bytes registrados em RIP correspondem a
`vmovdqu (%r11), %ymm5` no código XXH3 do cache de texturas. O cálculo de hash
usava diretamente os ponteiros de `BackingPages`, sem impedir a retirada da view
durante a leitura. A comparação de backing usava o mesmo padrão desprotegido.

No UWP, esses dois caminhos agora copiam cada trecho para um snapshot local com
`AddressSpace::CopyBacking`, que mantém o bloqueio/pin e usa o armazenamento
compartilhado quando a view está ausente. Hash e comparação operam sobre o
snapshot. Permanecem as regras anteriores de zeros para os buracos do hash e
de ignorar buracos na comparação. O estado XXH3 é liberado também se a cópia
falhar por exceção.

Os logs também mostraram centenas de novas tentativas de views de 1 MiB após
recusas repetidas. Uma recusa de recursos de uma view grande passa a selecionar
64 KiB para as novas views da sessão. A tentativa inicial grande cede primeiro
ao fallback pequeno, antes de retirar outros aliases e seções. As proteções e
o compartilhamento físico permanecem os mesmos. O evento
`mapping_window_reduced` identifica essa adaptação.

A verificação com 32 views encontrou outro bloqueio: fixar uma pilha também
impedia retirar todos os outros aliases de sua RAM, até consumir a cota inteira.
Agora o pin virtual protege a view efetivamente usada; o pin físico continua
impedindo fechar a seção ou retirar o backing necessário. Outros aliases podem
ser retirados e restaurados sem alterar a view fixada nem os dados compartilhados.

O identificador desta revisão é `memory_rootcause_v3`. As correções de remoção
dos eventos SDL/XAML da v2 permanecem no pacote.

## Revisão v4: tela preta e pressão de mapeamentos

Os logs v3 do Xbox mostram execução de draws e compilação de pipelines sem
falha fatal registrada, mas com 192 recusas de mapeamento e recuperação
repetida de seções. Eles não comprovam que os frames chegaram ao painel.
No harness nativo do PC, o mesmo núcleo apresentou a tela de título do
Bloodborne; isso não valida a composição XAML do Xbox.

A recuperação de recursos agora tenta liberar aliases e seções antes de
reduzir a janela grande. Uma cota de seções/views não deve selecionar
permanentemente 64 KiB quando a mesma janela grande pode ser restaurada.
`CopyBacking` mantém views canônicas reutilizáveis, protegidas pelo mesmo
bloqueio e pin, antes de recorrer às views temporárias. Isso evita criar e
destruir um mapeamento para cada trecho do hash de textura.

A camada XAML do launcher recebe opacidade zero durante a execução sem menu
e volta a ser visível no menu/biblioteca. O contador de FPS continua na camada
separada. A mudança independe da composição alfa do swapchain do launcher.
O evento `frontend_overlay_visible` registra a transição, e `[UWP present]`
registra a primeira apresentação de jogo e depois cada 300 apresentações,
com dimensões da origem e da saída.

O identificador desta revisão é `memory_rootcause_v4`. A confirmação da
correção da tela preta no Xbox depende da execução no console.

O usuário confirmou FPS variável, áudio e controle ativos durante a tela preta.
Esse comportamento reforça a investigação da composição das camadas, sem
comprovar por si só o conteúdo do framebuffer no console.

Na v4, núcleo e frontend Release compilaram. Os verificadores nativos existentes
passaram com swap ligado/desligado, janela física de 64 KiB, cota de 32 views
com pilha preservada e cota de 16 seções com reabertura dos mesmos dados.
O Bloodborne apresentou 1.029 frames em 30 ciclos do harness no PC, com captura
da tela de título, zero mensagens na fila de diagnóstico D3D e encerramento/
descarregamento/recarga bem-sucedidos. Essa captura não valida o painel XAML
do Xbox. Assinatura CMS e hashes dos payloads do pacote passaram; não houve
instalação. Os símbolos correspondentes estão em `Build/rootcause-v4-symbols`.
