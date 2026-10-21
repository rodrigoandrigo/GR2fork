# Diagnóstico de encerramento UWP

O usuário confirmou que Pac-Man inicia no PC, mas o aplicativo fecha após iniciar
o jogo no Xbox Series S. Os arquivos de log também não estavam disponíveis pelo
Device Portal. O log posterior do console identificou um bloqueio na exigência global de
Shader Model 6.6. Esse bloqueio foi corrigido com negociação SM 6.4/6.6; os logs
persistentes continuam ativos para diagnosticar qualquer falha posterior.

## Arquivos para coletar

No Device Portal, abra `LocalState/shadPS4/log` do pacote e copie:

- `uwp-session.log`: etapas do app e do core, endereço base do EXE/DLL, primeira
  operação GPU/apresentação e amostras de uso/limite de memória a cada segundo.
- `uwp-crash.log`: exceção nativa não tratada, PC/endereço acessado, contexto
  RIP/RSP, HRESULT XAML ou `std::terminate`, quando o handler é executado.
- `shadps4.log`, `shad_log.txt` e o log específico do jogo, se existir.
- `frontend-d3d12.log`, se existir.

O registro UWP é aberto antes de carregar o core. Escreve diretamente pelo Win32,
com write-through quando disponível e `FlushFileBuffers`, sem depender do logger
ou de alocação de heap no handler fatal. Os arquivos são acrescentados ao reabrir
o app. Se excederem 4 MiB, o próximo início os move para o sufixo `.previous`;
nesse caso, colete também esses arquivos.

As linhas contêm horário FILETIME e valores hexadecimais. Em eventos com
`bytes/limit`, `a` é o uso em bytes e `b` é o limite de memória do aplicativo.
Em `unhandled_seh code/pc/fault/access`, os campos `a/b/c/d` correspondem ao código,
PC e parâmetros de endereço/tipo de acesso da exceção. O endereço base do core
é registrado em `core_loaded dll_base` para permitir localizar a instrução.

O encerramento pelo sistema por falta de memória, remoção do processo e certos
fail-fasts podem não executar nenhum handler. Nesses casos, `uwp-crash.log` pode
ficar vazio: o último estágio e as amostras de `uwp-session.log` continuam sendo
os dados relevantes. Um arquivo vazio não identifica, por si só, a causa.

## Logger do core

No UWP, o arquivo recebe mensagens diretamente e executa `fflush` a cada entrada,
mesmo se a saída de console for assíncrona ou filtrar duplicatas. Erros e mensagens
críticas gravadas executam também sync. Os limites configurados permanecem ativos;
zero significa ilimitado e o contador é reiniciado ao trocar de sessão.
O mecanismo de tratamento de faults guest e a propagação dos erros permanecem
ativos: o diagnóstico não ignora exceções nem modifica o renderer por jogo.

## Validação

O diagnóstico Windows em processos separados passou encerramento forçado,
reabertura sem perder registros, 256 gravações concorrentes, exceção SEH não
tratada e `std::terminate` (`Build/uwp-persistent-log-probe-validation.log`).
O logger completo do core também passou encerramento sem shutdown, console
assíncrono com arquivo direto, preservação de oito mensagens duplicadas, limite
zero ilimitado e reinicialização do contador entre sessões
(`Build/core-log-persistence-validation.log`).

Isso valida a persistência no PC, sem comprovar execução dos handlers ou a causa
do encerramento no Xbox/AppContainer. O pacote não é instalado automaticamente.

O pacote Release x64 foi compilado e assinado. A assinatura CMS, as seis DLLs,
o EXE do host e os marcadores do diagnóstico no EXE empacotado passaram
(`Build/xbox-persistent-log-package-verification.log`). Não houve instalação
automática nem nova execução em Xbox.

## Remoção do dispositivo após o primeiro draw no Series S

Os arquivos `frontend-d3d12.log`, `shad_log.txt` e `uwp-session.log` recebidos em
19/10/2026 confirmam SM 6.4, wave64 fixa e primeira apresentação concluída.
O primeiro draw é seguido por `DXGI_ERROR_DEVICE_REMOVED` (0x887a0005), com
motivo `DXGI_ERROR_DRIVER_INTERNAL_ERROR` (0x887a0020). O DRED não forneceu
breadcrumbs nem endereço de page fault. O último consumo registrado ficou
próximo de 817 MiB, abaixo do limite informado de 5 GiB.

Foi corrigida uma lacuna na compatibilidade do bytecode: selecionar SM 6.4
agora também seleciona explicitamente o validador DXIL 1.4 no DXC e na ponte
SPIR-V/Mesa. O caminho SM 6.6 seleciona 1.6. As opções entram na chave do
cache HLSL; a identidade do core invalida os demais caches persistentes.
A criação do primeiro pipeline gráfico e sua primeira submissão agora têm
marcos separados no journal para localizar uma eventual recorrência.

Essa incompatibilidade é uma possível causa da remoção; os logs recebidos
não permitem atribuí-la conclusivamente ao compilador ou à execução GPU.
A confirmação da correção do encerramento depende de nova execução no Xbox.

Validação desta alteração: inspeção de SM e `dx.valver` de VS/PS/CS/GCN e
recarga do cache passaram (`Build/native-sm64-profile-msaa-validation.log`).
A regressão gráfica privada forçada a SM 6.4 passou com zero erros D3D12,
incluindo geometria, tesselação, MSAA, tiling, feedback e aliases
(`Build/native-sm64-regression-msaa-validation.log`). O pacote Release x64
passou compilação, assinatura CMS e comparação do core, host e seis DLLs
(`Build/dxil-compat-uwp-build.log`, `Build/dxil-compat-package-verification.log`).

## Recorrência após limitar o validador DXIL

Os novos `shad_log (1).txt` e `uwp-session (1).log` confirmam que limitar o
validador não resolveu a remoção no Series S. A criação do primeiro pipeline
termina, mas não aparece o marco de submissão do primeiro desenho. O motivo
continua sendo `DXGI_ERROR_DRIVER_INTERNAL_ERROR`.

O reset das listas gráficas e compute agora usa pipeline inicial nulo.
Uploads e transferências são preparados antes da vinculação explícita do
pipeline final, que já existe em ambos os caminhos. Assim a lista não
vincula antecipadamente um pipeline que será sobrescrito durante a preparação.

O cache persistente passa a expor sua disponibilidade. Quando não foi possível
estabelecer a identidade do driver ou criar o diretório do cache, não se chama
`ID3D12PipelineState::GetCachedBlob`: antes essa extração era feita mesmo com
o cache desabilitado. O cache em memória continua ativo. A criação do pipeline
e a extração do cache também verificam `GetDeviceRemovedReason`; o primeiro
heap RTV, heap DSV e os resets têm etapas separadas no journal.

São correções gerais de preparação e de uso de cache opcional. Os novos logs
não permitem provar qual chamada disparou a falha interna do driver; uma nova
execução no console continua necessária para confirmar o encerramento resolvido.

Validação no PC desta revisão: regressão SM 6.4 concluída com cache de PSO
desabilitado no primeiro contexto e habilitado nos dois seguintes, incluindo
GCN draw/dispatch, aliases físicos/virtuais, feedback entre estágios, atômicos,
tilings alternativos, geometria, tesselação e MSAA; zero erros D3D12
(`Build/native-sm64-regression-msaa-validation.log`). O build Release x64,
a assinatura CMS e a comparação dos payloads empacotados passaram
(`Build/pipeline-reset-uwp-build.log`, `Build/pipeline-reset-package-verification.log`).
Não houve instalação ou execução do pacote no console nesta validação.

## Remoção localizada na extração do blob do pipeline

A terceira coleta de `shad_log.txt`, `uwp-session.log` e `frontend-d3d12.log`
registra `Native D3D12 device after pipeline cache extraction`, com
`DXGI_ERROR_DRIVER_INTERNAL_ERROR` (0x887a0020). A verificação imediatamente
após criação do pipeline passou; a posterior à extração do cache detectou a
remoção. O desenho ainda não foi submetido à GPU.

No dispositivo da família `Windows.Xbox`, a persistência de blobs nativos do
driver agora fica desativada. Isso impede tanto carregar blobs existentes em
`CachedPSO` quanto chamar `GetCachedBlob`. Os objetos de pipeline continuam
reutilizados em RAM, e a persistência de shaders DXIL permanece ativa.
No PC, a persistência de pipelines continua disponível. Se a identificação
da família via WinRT falhar, apenas o cache opcional do driver é desativado.
Um marco de inicialização registra `native driver pipeline disk cache` como
`enabled` ou `disabled` para conferir o caminho selecionado no console.

A localização nos logs fundamenta evitar essa operação do driver no Xbox;
a confirmação de que elimina o encerramento ainda depende do console.

A validação direcionada no PC passou dispatch GCN, desenho com 256 pixels
vermelhos, wireframe com 16 pixels e zero erros D3D12, alternando entre cache
de blobs desativado e habilitado pela política da plataforma
(`Build/xbox-pso-cache-smoke-validation.log`). A consulta WinRT selecionou
cache habilitado no PC. Isso não executa a ramificação `Windows.Xbox` num
console; o caminho sem blobs foi exercitado explicitamente no diagnóstico.

O pacote Release x64 desta revisão passou compilação, assinatura CMS e
comparação do core, host e seis DLLs
(`Build/xbox-pso-cache-uwp-build.log`, `Build/xbox-pso-cache-package-verification.log`).
O core empacotado contém a seleção `Windows.Xbox` e o novo marco de cache.
Não houve instalação automática ou execução no Xbox.
