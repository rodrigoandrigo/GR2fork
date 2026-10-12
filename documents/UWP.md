# Build UWP com DLL e Direct3D 12

O modo `SHADPS4_UWP` gera `shadPS4.dll` com uma interface C versionada.
O aplicativo XAML carrega essa DLL por `LoadPackagedLibrary`. O build desktop
continua disponível na configuração padrão.

## Dependências e compilação

- Visual Studio com C++ UWP, Windows SDK 10.0.26100.0 e clang-cl.
- CMake, Ninja e Python 3.
- SDL3 em `...\SDL3-uwp`.
- D3D12 em `...\xbox-series-d3d12-main`.

Execute na raiz:

```powershell
.\scripts\build-uwp.ps1 -Configuration Release
```

Use `-PythonExecutable` se Python não estiver no PATH. `-SDL3Source` e
`-D3D12Source` substituem as pastas acima. `-CoreOnly` compila e copia a DLL,
sem compilar o aplicativo. A solução UWP também chama esse script antes do build.

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

Um adaptador de apresentação cria o swapchain de composição e usa o callback
da ABI para anexá-lo ao SwapChainPanel na thread UI. Os frames de fundo são
gravados e submetidos na DLL usando os componentes D3D12 importados.
Não há dependência de Dozen nem carregamento de um ICD Vulkan neste caminho.

## Limite atual

**Ainda não há renderer PS4 D3D12 integrado.** O projeto D3D12 fornecido também
declara essa pendência: seu consumidor inicial rejeita draw e dispatch guest,
e a conversão de shaders cobre contratos restritos de validação.

Consequentemente, inicializar dispositivo, apresentar o fundo e compilar a DLL
não equivale a executar um jogo PS4. O aplicativo mantém o boot desabilitado;
a ABI rejeita pedidos de boot antes de executar o guest. O núcleo Vulkan fica
compilado como parte da base desktop, mas não é usado pela apresentação UWP.

O restante exige ligar Liverpool/PM4, recursos guest, caches, shaders,
pipelines e apresentação de videoout ao renderer nativo. Não são anunciadas
capacidades nem simulados draws para contornar essa ausência.

## Armazenamento e diagnóstico

Configuração, logs, saves e módulos usam LocalFolder/shadPS4. O FolderPicker
fornece um StorageFolder à DLL para montar `/app0` em modo somente leitura.
O host lê metadados e o início do eboot pela API, sem copiar a árvore para
LocalCache e sem converter o grant em um caminho externo. Selecionar a pasta
está disponível; iniciar o guest continua desabilitado.

O host salva a seleção sob o token `shadps4-game` no FutureAccessList do
próprio pacote e recupera o StorageFolder na próxima inicialização. Remoção
do arquivo, revogação do acesso e falhas de restauração aparecem na UI;
o usuário pode selecionar outra pasta.

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

Validação local desta camada: build Release da DLL, host e MSIX; 32 exports
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
rejeitando boot enquanto falta o renderer guest.

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
