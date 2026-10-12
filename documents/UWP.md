# Build UWP com DLL e Direct3D 12

O modo `SHADPS4_UWP` gera `shadPS4.dll` com uma interface C versionada.
O aplicativo XAML carrega essa DLL por `LoadPackagedLibrary`. O build desktop
continua disponível na configuração padrão.

## Dependências e compilação

- Visual Studio com C++ UWP, Windows SDK 10.0.26100.0 e clang-cl.
- CMake, Ninja e Python 3.
- SDL3 em `.../SDL3-uwp`.
- D3D12 em `externals/d3d12-native`.

Execute na raiz:

```powershell
.\scripts\build-uwp.ps1 -Configuration Release
```

Use `-PythonExecutable` se Python não estiver no PATH. `-SDL3Source` e
`-D3D12Source` substituem as pastas acima. `-CoreOnly` compila e copia a DLL,
sem compilar o aplicativo. A solução UWP também chama esse script antes do build.

O script assina o bundle com `shadPS4-UWP/shadPS4-UWP_TemporaryKey.pfx`,
verifica a assinatura CMS e exporta o certificado público `.cer` junto ao pacote.
`-CertificatePath` substitui o certificado; `-SkipSigning` gera somente o pacote
sem assinatura. A assinatura exige que o Subject corresponda ao Publisher do
manifesto. `scripts/sign-uwp.ps1 -Package <bundle>` permite assinar separadamente.

SDL3 é compilado com MSVC/C++/CX; o núcleo usa clang-cl para preservar a ABI SysV
e o assembly x64 do PS4. O modo UWP não compila o SDL de `externals/sdl3`.

O post-build copia a DLL, a import library, SDL3.dll e o header da ABI para
`shadPS4-UWP/Runtime/<Configuration>`. O aplicativo inclui shadPS4.dll e
SDL3.dll na raiz do pacote. Apenas x64 é suportado.

## D3D12 usado pelo host

O núcleo compila diretamente os componentes do projeto indicado:

- `d3d12_device_context.cpp`: dispositivo, fila DIRECT, fences e espera.
- `d3d12_resource_allocator.cpp`: recursos e orçamento.
- `d3d12_command_encoder.cpp`: transições e comandos de transferência.
- `d3d12_pipeline_cache.cpp`: descritores, root signatures e pipelines.
- `d3d12_videocore_bridge.cpp`: contrato de comandos, transferências e readback.

O frontend ImGui cria seu swapchain de composição usando o dispositivo D3D12
importado e anexa-o ao SwapChainPanel na thread UI. A DLL recebe um segundo
painel, usado pelo VideoOut guest; o ImGui pode compor um overlay transparente.
A ABI mantém também o adaptador de apresentação para outros consumidores.
Não há dependência de Dozen nem carregamento de um ICD Vulkan neste caminho.

## Limite atual

**Há um renderer guest D3D12 experimental, ainda incompleto para jogos.**
Liverpool usa um rasterizador próprio com draw/dispatch, recursos guest e
GCN→SPIR-V→HLSL→DXIL. A bridge restrita do snapshot não executa esse caminho.

O estado da migração do renderer e os caminhos ainda pendentes estão em
[D3D12-RENDERER.md](D3D12-RENDERER.md), incluindo os resultados de execução
GCN em GPU D3D12 e as limitações ainda presentes.

Consequentemente, inicializar dispositivo, apresentar o fundo e compilar a DLL
não equivale a executar um jogo PS4. O aplicativo mantém o boot desabilitado;
a ABI rejeita pedidos de boot antes de executar o guest. O núcleo Vulkan fica
compilado como parte da base desktop, mas não é usado pela apresentação UWP.

Ainda faltam estágios e estados gráficos avançados, coerência completa,
boot brokered e teardown guest. Operações não suportadas geram erro da sessão.

## Armazenamento e diagnóstico

Configuração, logs, saves e módulos usam LocalFolder/shadPS4. O FolderPicker
fornece StorageFolders à DLL para montar `/libraryN` em modo somente leitura.
O worker lê metadados e capas pela API sem converter o grant em caminho externo.
Selecionar pastas está disponível; iniciar o guest continua desabilitado.

O host salva tokens gerados pela FutureAccessList do próprio pacote e a lista
de pastas em LocalSettings. Na próxima inicialização recupera os StorageFolders;
grants revogados são ignorados. O token antigo `shadps4-game` é migrado quando
não existe uma lista. Falhas de leitura aparecem na UI; outra pasta pode ser adicionada.

## Arquivos brokered

A ABI C recebe ponteiros emprestados de IInspectable para StorageFile,
StorageFolder ou IRandomAccessStream. A DLL faz QueryInterface e mantém suas
referências; streams são clonados para ter cursor próprio. Handles nativos
de arquivos são recebidos por `shadps4_open_file_handle` e duplicados: a DLL
nunca fecha o handle original. O host pode liberar o objeto/handle após a
chamada bem-sucedida. Para handles duplicados, não faça I/O concorrente pelo
original, pois o objeto kernel e suas características de compartilhamento
permanecem os mesmos.

Os novos exports são aditivos à versão 1 da ABI:

| Operação | API |
| --- | --- |
| Montar/desmontar StorageFolder | `shadps4_mount_storage_folder`, `shadps4_unmount_storage_folder` |
| Abrir objeto/stream/handle | `shadps4_open_storage_file`, `shadps4_open_random_access_stream`, `shadps4_open_file_handle` |
| Abrir arquivo de uma montagem | `shadps4_open_mounted_file` |
| Ler/escrever/seek | `shadps4_file_read`, `shadps4_file_write`, `shadps4_file_seek` |
| Tamanho/truncate/flush/close | `shadps4_file_get_size`, `shadps4_file_set_size`, `shadps4_file_flush`, `shadps4_file_close` |
| Metadados/enumeração | `shadps4_storage_stat`, `shadps4_storage_enumerate` |
| Criar pasta/apagar/renomear | `shadps4_storage_create_directory`, `shadps4_storage_delete`, `shadps4_storage_rename` |

As operações retornam 0 ou HRESULT negativo, preservado também no callback
de erro e em get_error. Chamadas devem partir de uma thread COM MTA; STA/UI
é rejeitada com RPC_E_WRONG_THREAD antes do I/O. Tokens de arquivo são uint64
opacos: 0 é inválido, não são reutilizados após shutdown, e o token encerrado
retorna ERROR_INVALID_HANDLE. Falha na abertura deixa o token de saída em 0.
Leitura no EOF retorna sucesso com zero bytes. Leitura/escrita podem ser
parciais; observe o contador `transferred`.

Modos: READ, READ_WRITE (arquivo existente), CREATE (cria/trunca) e APPEND
(cria se necessário dentro de montagem e escreve no final). Seek usa 0 para
início, 1 para posição atual e 2 para fim. O consumidor preenche o campo size
de ShadPS4FileInfo com sizeof antes de stat. Datas são segundos Unix;
modified_seconds na enumeração é 0, consulte stat para obter a data.
Nomes na enumeração são UTF-8 e só valem durante o callback. A enumeração
entrega um snapshot fora do mutex, permitindo consultar metadados nele.

Montagens usam nomes absolutos com um componente, como `/app0` e `/media`.
Arquivos usam componentes relativos abaixo delas. Caminhos absolutos externos,
`..`, barras invertidas, componentes vazios, nomes com ponto/espaço final e
caracteres especiais Win32 são rejeitados. Não há fallback para fopen nem
StorageFile.GetFileFromPathAsync para resolver esses caminhos. O mount mantém
o StorageFolder brokered e navega pelos seus métodos Windows.Storage.

Renomear não substitui destino existente. Arquivos podem ser movidos entre
subpastas da mesma montagem; mover diretórios entre pais retorna
ERROR_NOT_SUPPORTED e mover entre montagens retorna ERROR_NOT_SAME_DEVICE.
Delete usa PermanentDelete (inclusive conteúdo de subpastas) e nunca aceita
apagar a raiz montada. Montagens somente leitura rejeitam mutações com
E_ACCESSDENIED. Acesso WRITE declarado pela API não amplia as permissões
que o Windows concedeu ao objeto/handle.

BrokeredFile/BrokeredFsBackend implementam IFile/IBackend, conectando montagens
à abertura, leitura, escrita e enumeração já existentes em MntPoints.
Não existe RootHostPath para esses objetos. Syscalls guest que ainda usam
diretamente std::filesystem para mutações/permissões continuam sem suporte
para montagens brokered; elas não recebem um caminho externo como fallback.
A execução guest permanece fora do escopo da base atual.

Unmount impede novas aberturas; tokens já abertos continuam válidos até close
ou shutdown. Shutdown fecha tokens e remove as montagens brokered. O host
deve aguardar suas tarefas antes de descarregar a DLL.

Referência: [File access permissions](https://learn.microsoft.com/en-us/windows/apps/develop/files/file-access-permissions).
FutureAccessList é privado do pacote, não uma autorização compartilhada com
outros aplicativos. Nenhuma capability broadFileSystemAccess foi adicionada.

Validação local desta camada: build Release da DLL, host e MSIX; 39 exports
públicos conferidos; flag AppContainer e hash da DLL dentro do pacote conferidos.
O probe nativo exercitou StorageFile/StorageFolder, CRUD, EOF, leitura/escrita
em blocos acima de 1 MiB, append, truncate, handles síncronos/assíncronos,
posse dos objetos, tokens após unmount, consultas dentro do callback de
enumeração, restrições de escrita, traversal e rejeição de STA. Os checks de
CPU/D3D12, callbacks de log/erro, arquivo de log e shutdown também passaram.

Esse probe executou em um processo Windows nativo, não em AppContainer.
Picker, persistência/restauração do grant no aplicativo instalado, provedores
remotos e Xbox ainda não foram exercitados. Não se presume permissão a partir
de um caminho válido nem resultado AppContainer a partir do probe nativo.

## Contrato de embedding

Todas as funções públicas de embedding são exportadas com ABI C no header
`src/uwp/shadps4_api.h`. A versão 1 aceita os novos exports de forma aditiva:
`shadps4_set_log_callback`, `shadps4_set_error_callback`,
`shadps4_flush_logs` e `shadps4_shutdown`.

O host registra os callbacks antes de inicializar a DLL. Logs UTF-8 incluem
nível (0 trace, 1 debug, 2 info, 3 warning, 4 error, 5 critical) e são também
gravados em `ApplicationData::Current->LocalFolder/shadPS4/log/shadps4.log`.
O logger UWP não registra atexit/at_quick_exit nem usa console/stderr;
falhas do próprio logger são encaminhadas ao callback. O host atual encaminha
os callbacks ao debugger; outro consumidor pode encaminhá-los à sua UI.

Configuração com `panel` e `attach_swapchain` nulos inicializa o núcleo sem
apresentação, para consumidores sem XAML. Nesse modo a validação CPU/D3D12,
armazenamento, callbacks e logs permanecem ativos; render_host_frame retorna erro.

Callbacks são síncronos na thread emissora e podem ocorrer em threads diferentes.
O texto só é válido durante a chamada: copie-o antes de postar à UI. Eles não
devem lançar exceções, bloquear, reentrar operações de ciclo de vida ou liberar
seu contexto enquanto há chamadas em andamento. `shadps4_get_error` pode ser
consultada no callback de erro. Remova callbacks apenas após encerrar as tarefas
que usam a DLL.

Inicialização roda em tarefa de background no host; apresentação e resize usam
uma thread de trabalho, que agrupa pedidos de frame/resize. A thread UI apenas
solicita trabalho e anexa o swapchain quando o callback é despachado para ela.
`shadps4_run` pertence à thread de sessão, já separada da UI, e continua
rejeitando boot enquanto faltam caminhos gráficos avançados e boot brokered completo.

O encerramento segue: solicitar stop, aguardar a sessão e a apresentação,
chamar shutdown, remover callbacks e liberar a DLL. Shutdown libera a
apresentação e drena/fecha os logs; chamadas com sessão ainda ativa retornam
erro. Assertions do núcleo UWP lançam erro para a fronteira de embedding em
vez de executar breakpoint/encerrar o processo. A recuperação de faults em
threads guest ainda depende da implementação futura de execução guest.

A interface valida tamanho/versão da configuração e verifica execução e
modificação de código x64 antes de inicializar o host. Erros D3D12 preservam
HRESULT e mensagem na ABI. O manifesto inclui `codeGeneration`.

Compilação, conteúdo do pacote, carregamento nativo, AppContainer, apresentação
e gameplay são verificações separadas. Não se presume resultado no Xbox a
partir dos resultados registrados pelo outro projeto.

Referência: [VirtualProtectFromApp e codeGeneration](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-virtualprotectfromapp).


## Frontend ImGui

O ImGui 1.89.5 WIP e seu backend DX12 foram copiados, sem modificações, de
RPCS3-UWP/Frontend/dependencies/imgui para shadPS4-UWP/third_party/imgui,
incluindo LICENSE.txt. Essa cópia pertence somente ao host; nenhuma estrutura
ImGui atravessa a ABI da DLL.

Biblioteca, configurações globais e por jogo, importação de módulos/fontes/DLCs
e logs ficam no frontend. A biblioteca aceita até 64 pastas brokered, com
habilitar/remover/atualizar e tokens persistidos na FutureAccessList.
A lista é virtualizada; busca, ordenação, leitura de PSF e decodificação WIC
acontecem no worker MTA. A última busca substitui a busca pendente.

A thread UI modifica apenas o snapshot protegido por mutex. A thread de
renderização possui o contexto ImGui. Entrada é limpa ao perder foco, sair
com o ponteiro ou abrir diálogo nativo. O desligamento interrompe filas,
aguarda o worker e libera a DLL depois do renderer.

O heap possui 65 descritores: um para fonte e 64 para capas. A expulsão LRU
espera o fence do último uso antes de reutilizar um descritor. Capas têm até
256x256 pixels; filas, logs, erros de capa e uploads são limitados. Até dois
uploads entram na lista de comandos de cada frame, sem espera global por capa.

A API de configurações usa os oito grupos reais do desktop e valida tipo,
faixa, enumeração e política antes de salvar. O inventário completo de 167
opções, com 46 editáveis e justificativas para 121 exclusões desta base atual,
está em [UWP-SETTINGS.md](UWP-SETTINGS.md). Perfis por jogo respeitam os campos
overrideable do desktop, sem alterar o singleton global.

Validação local: build Release do núcleo, host e pacote; API de configurações
com persistência, reset, perfis, isolamento e rejeição de traversal; renderer
nativo com 400 frames/capas, cache máximo de 64, lista sintética de 100 mil
itens e readback de GPU. Esses probes não executam o aplicativo empacotado.
Instalação, navegação visual completa, picker/FutureAccessList no AppContainer,
resumo da suspensão e Xbox continuam sem validação de execução.


## XAudio2 e Windows.Gaming.Input

XAudio2 é o backend de saída padrão em novas configurações UWP e aparece no
seletor Audio/audio_backend com valor 2. Configurações existentes de SDL3 ou
OpenAL são preservadas. Microfone continua usando o backend SDL de entrada;
XAudio2 não é uma API de captura. O mixer Audio3d existente entrega sua saída
à mesma camada AudioOut.

As portas XAudio2 compartilham dispositivo/mastering voice numa thread MTA
própria, que mantém COM até a destruição do dispositivo, e possuem source
voices independentes. Não se exige COM inicializado na thread guest que abre
a porta. A compilação usa WINAPI_FAMILY_APP e importa XAudio2_9 diretamente,
sem o fallback de carregamento da API desktop. São aceitos S16 e float, mono/estéreo/oito canais, com
remapeamento pelo AudioFormatInfo do núcleo e ganhos por canal mais o volume
global. Cada porta conserva oito buffers próprios até OnBufferEnd; callbacks
fazem apenas operações atômicas, sem logs, alocações ou espera. Uma fila cheia
descarta o novo bloco e registra diagnóstico limitado, sem bloquear a UI.
DestroyVoice ocorre antes de liberar os buffers. O shutdown interrompe e
aguarda as threads AudioOut antes de destruir vozes e dispositivo.

Windows.Gaming.Input alimenta tanto a navegação ImGui quanto os pads do núcleo.
Um worker MTA mantém até quatro slots estáveis por identidade do Gamepad,
detecta conexão/desconexão e publica cada pacote completo sob o mutex do
GameController. Botões A/B/X/Y correspondem a Cross/Circle/Square/Triangle,
Menu a Options e View ao clique do TouchPad. Direcional, L1/R1, L3/R3,
sticks e gatilhos são mapeados; Y é invertido para o sistema PS4. Sticks usam
0..255 com centro 128; gatilhos usam 0..255. Não são simulados sensores ou
uma superfície touch ausentes no Gamepad. Os slots usam os usuários já
configurados no desktop. Timestamps de entrada vêm do relógio monotônico da
sessão do host, sem depender de inicialização do relógio guest.

Pedidos de vibração do guest são atômicos; somente o worker WGI escreve
Gamepad.Vibration. O motor grande usa LeftMotor e o pequeno RightMotor.
Captura de UI/foco/suspensão limpa entrada antiga e vibração. A nova função
aditiva shadps4_set_host_state(active, input_captured) coordena essa captura e
StopEngine/StartEngine do XAudio2. Desconexão e encerramento também param os
motores. O worker é aguardado antes de descarregar a DLL.

Validação Release: 288 buffers silenciosos consumidos pelo XAudio2 real em
1/2/8 canais, limite de fila, pausa/retomada, rejeição de tamanho inválido e
destruição com buffers pendentes; ativação WGI real e mapeamento de 14 botões,
dois gatilhos, centros/extremos/limites dos eixos. Nenhum controle físico estava
conectado. Hotplug físico, vibração sentida, qualidade audível, execução guest,
aplicativo instalado/AppContainer e Xbox continuam sem prova de execução.
O bundle foi assinado com CN=rodri e verificado por SignTool, sem instalar o
pacote nem alterar certificados confiáveis do sistema.
