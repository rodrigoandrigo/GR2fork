# ShadPS4-UWP

ShadPS4-UWP é a adaptação do emulador de PlayStation 4 shadPS4 para a Universal Windows Platform (UWP), com interface para Windows e Xbox em Modo de Desenvolvedor.

O aplicativo combina um frontend em C++/XAML e ImGui com o núcleo de emulação compilado como `shadPS4.dll`. A interface permite gerenciar a biblioteca de jogos, importar módulos do sistema e controlar a execução por controle ou teclado. O projeto integra o ciclo de vida UWP e o redimensionamento da janela à execução dos jogos.

A renderização oferece Vulkan através do Mesa Dozen, selecionado por padrão, e um backend D3D12 nativo. O Dozen traduz Vulkan para Direct3D 12; as bibliotecas gráficas são compiladas e incluídas no pacote pelo fluxo de build.

## Interface

A interface adapta o visual do shadPS4 Qt Launcher ao frontend UWP: menus no topo, barra de ferramentas com os ícones originais, biblioteca em lista ou grade, pesquisa, ajuste do tamanho das capas e painel inferior de logs redimensionável. A paleta escura e a fonte do sistema acompanham o estilo do launcher.

As configurações ficam em uma única janela com abas e perfis globais ou por jogo. Os atalhos de controles e teclado abrem essa mesma janela. As opções são obtidas da API do núcleo e cada opção aparece em uma única categoria; áudio, pastas e seleção do backend gráfico têm locais próprios. Use **Salvar** ou **Aplicar** para gravar alterações, ou **Fechar** para descartá-las.

O modo de exibição, o tamanho das capas e a disposição do painel de logs são preservados entre inicializações. O aplicativo mantém navegação por teclado e controle, além dos seletores de pastas do UWP.

No Xbox, a biblioteca e os menus solicitam o ponteiro do sistema. Ele é ocultado ao iniciar o jogo e restaurado ao voltar à interface, encerrar a sessão ou cancelar uma inicialização que falhou.

## Jogos em PKG

Use **Arquivo > Adicionar PKG…** para selecionar um pacote individual, ou adicione uma pasta que contenha arquivos `.pkg`. A autorização de acesso fica salva na biblioteca do aplicativo.

Pacotes PS4 no formato FPKG são acessados diretamente, com leitura, descriptografia e descompressão sob demanda, seguindo a implementação do Prosperity. O jogo é montado no sistema de arquivos virtual como `/app0`, sem extrair o pacote inteiro para o armazenamento do aplicativo.

A coluna **Formato** distingue **PKG** e **Pasta**; a grade também mostra essa informação. Título, serial, versão e capa são lidos do pacote. Atualizações e DLCs recebem identificação própria e não são iniciados como jogos independentes.

## Touchpad no controle Xbox

Segure **View** e mova o **analógico esquerdo** para posicionar o dedo no touchpad do PS4. O contato começa ao mover o analógico além da zona inicial ou pressioná-lo, e continua enquanto View estiver segurado, inclusive ao voltar o analógico ao centro.

Use **View + L3** para clicar no touchpad. Soltar apenas L3 libera o clique e mantém o dedo em contato; soltar View levanta o dedo e devolve o analógico à função normal do jogo. Cada jogador tem seu próprio gesto e identificador de contato.

O menu, a perda de foco e a desconexão liberam o contato. Depois disso, solte View antes de começar outro gesto. As mesmas instruções ficam na aba **Entrada**, sem criar uma configuração paralela.

## Requisitos de build

- Windows x64 e PowerShell.
- Visual Studio com toolset C++ `v145` (Visual Studio 2026), ferramentas C++ para UWP, MSBuild, Clang/LLVM e integração CMake/Ninja.
- Windows SDK **10.0.26100.0**.
- CMake **3.24 ou superior**, disponível no `PATH`.
- Git, para obter os submódulos do repositório.
- MSYS2 em `C:\msys64`, com ambiente UCRT64, Python 3, Bison e Flex. O script do Mesa utiliza `C:\msys64\usr\bin` e `C:\msys64\ucrt64\bin`.
- No Python utilizado pelo build: Meson **1.4 ou superior**, Mako, PyYAML e packaging.
- Runtime UWP **Microsoft.VCLibs.140.00 x64** instalado no Windows.
- Fontes do **SDL3-uwp**, incluindo `VisualC-WinRT\SDL-UWP.vcxproj`, em uma pasta separada.

As fontes do Mesa e do backend D3D12 ficam em `externals/mesa-uwp` e `externals/d3d12-native` neste repositório.

## Preparação

Na raiz do repositório, inicialize os submódulos:

```powershell
git submodule update --init --recursive
```

No terminal UCRT64 do MSYS2, instale as ferramentas de geração:

```bash
pacman -S --needed bison flex mingw-w64-ucrt-x86_64-python mingw-w64-ucrt-x86_64-meson mingw-w64-ucrt-x86_64-python-mako mingw-w64-ucrt-x86_64-python-yaml mingw-w64-ucrt-x86_64-python-packaging
```

Tenha disponível a árvore do SDL3-uwp e ajuste o argumento `-SDL3Source` abaixo para sua localização.

## Build completo

Execute no PowerShell, a partir da raiz do repositório:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-uwp.ps1 `
    -Configuration Release `
    -SDL3Source "C:\dev\SDL3-uwp" `
    -PythonExecutable "C:\msys64\ucrt64\bin\python.exe" `
    -Jobs 6
```

O script configura o ambiente x64 do Visual Studio e executa estas etapas:

1. Compila o SDL3 para UWP.
2. Compila o Mesa Dozen e o compilador `spirv_to_dxil`.
3. Configura e compila o núcleo `shadPS4.dll` com CMake, Ninja e clang-cl.
4. Compila o frontend UWP e gera o pacote MSIX Bundle.
5. Assina o pacote, verifica sua assinatura CMS e exporta o certificado público.

### Opções

| Parâmetro | Função |
| --- | --- |
| `-Configuration Release` | Seleciona `Release` ou `Debug`; o padrão é `Release`. |
| `-SDL3Source <pasta>` | Localização das fontes do SDL3-uwp. |
| `-D3D12Source <pasta>` | Substitui a pasta padrão `externals/d3d12-native`. |
| `-PythonExecutable <arquivo>` | Define o Python com as dependências do Mesa. |
| `-Jobs 6` | Define o paralelismo da compilação do núcleo e do Mesa. |
| `-CoreOnly` | Compila SDL3, Mesa e núcleo, sem gerar o frontend e o pacote. |
| `-SkipSigning` | Gera o aplicativo e o pacote sem executar a assinatura final. |
| `-CertificatePath <arquivo.pfx>` | Usa um certificado próprio na assinatura. |

Para gerar uma versão de depuração, utilize o mesmo comando com `-Configuration Debug`.

## Assinatura

Por padrão, o script utiliza `shadPS4-UWP/shadPS4-UWP_TemporaryKey.pfx`. Um certificado próprio deve conter a chave privada, estar dentro da validade, ser carregável com senha vazia e ter o assunto correspondente ao `Publisher` do manifesto.

Para assinar um pacote já gerado:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\sign-uwp.ps1 `
    -Package "C:\build\shadPS4-UWP_1.0.0.0_x64.msixbundle" `
    -CertificatePath "C:\certificados\shadPS4-UWP.pfx"
```

O script utiliza o SignTool do SDK 10.0.26100.0, valida a assinatura e grava `shadPS4-UWP.cer` ao lado do pacote. A conclusão bem-sucedida informa `CMS_SIGNATURE=PASS`.

## Arquivos gerados

Os caminhos abaixo são relativos à raiz do repositório:

| Caminho | Conteúdo |
| --- | --- |
| `Build/UWP-Release/` | Build CMake/Ninja do núcleo e build do SDL3. |
| `Build/Dozen-UWP/` | Build do Mesa, incluindo `vulkan_dzn.dll` e `spirv_to_dxil.dll`. |
| `shadPS4-UWP/Runtime/Release/` | Núcleo e bibliotecas de runtime utilizadas pelo frontend. |
| `shadPS4-UWP/AppPackages/` | Pacotes `.msixbundle` e certificado público exportado pela assinatura. |

Para builds de depuração, os diretórios do núcleo e do runtime utilizam `Debug` no lugar de `Release`.

## Organização

- `shadPS4-UWP/`: frontend, manifesto, recursos e projeto UWP.
- `src/`: núcleo de emulação e backends gráficos.
- `externals/mesa-uwp/`: implementação Mesa Dozen para UWP.
- `externals/d3d12-native/`: integração do backend D3D12 nativo.
- [scripts/build-uwp.ps1](../scripts/build-uwp.ps1): build completo.
- [scripts/build-dozen.ps1](../scripts/build-dozen.ps1): build das bibliotecas Mesa.
- [scripts/sign-uwp.ps1](../scripts/sign-uwp.ps1): assinatura e verificação do pacote.

## Cheats, patches e informações durante o jogo

Selecione um jogo e abra **Cheats / Patches > Gerenciar jogo selecionado…**. As abas permitem baixar arquivos dos repositórios shadPS4 e GoldHEN, consultar versões, créditos e notas, excluir arquivos e selecionar os patches. As seleções são salvas automaticamente e aplicadas na próxima inicialização. Atualizar os arquivos preserva os patches já selecionados. Durante a execução, abra o menu do emulador e use as caixas ou botões de cheats para o jogo em execução; desmarcar uma caixa aplica os bytes de desativação do arquivo.

**Configurações > Exibir contador de FPS** controla o contador no canto superior esquerdo da imagem. Ele mede os flips efetivos do jogo, com atualização a cada meio segundo, sem manter o swapchain da interface apresentando frames durante a gameplay.

O **swap de memória** fica ativado por padrão no Xbox e desativado por padrão no PC. A configuração global `General.memory_swap_enabled` preserva uma escolha explícita. O backing da RAM guest usa arquivos temporários em `cache/swap`, compartilhados pelas suas diferentes vistas de memória e removidos ao liberar os mapeamentos ou encerrar o processo. O sistema operacional administra a paginação desses arquivos.
