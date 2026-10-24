# Jogos PKG no ShadPS4-UWP

O frontend reconhece arquivos `.pkg` nas pastas da biblioteca e também permite selecionar um pacote por **Arquivo > Adicionar PKG…**. Arquivos individuais usam `StorageFile` e FutureAccessList; pastas continuam usando o acesso brokered existente. O mesmo cadastro de fontes restaura ambos os tipos ao reabrir o aplicativo.

O leitor foi adaptado de `prosperity/delta/formats/pkg_filesystem.cc`, preservando o caminho FPKG: recuperação de EKPFS com o conjunto público de chaves de pacotes fake, AES-XTS do PFS e descompressão PFSC por bloco. Usa a biblioteca LibreSSL e o zlib já presentes no projeto. Não são adicionadas chaves de pacotes de varejo.

`PkgBackend` implementa a interface de arquivos existente, com leitura aleatória, seek, stat, enumeração de diretórios, conteúdo somente leitura e mmap por cópia. O pacote permanece aberto por uma referência compartilhada enquanto houver arquivos em uso. A montagem de boot utiliza `/app0` e `/hostapp`; não extrai o jogo inteiro. Metadados e ícones externos podem ser lidos mesmo quando o conteúdo interno não é inicializável.

A interface exibe o formato na tabela e na grade, permite pesquisar e ordenar por formato e mostra o motivo quando um pacote não pode ser iniciado. A configuração por jogo continua identificada pelo serial e compartilha a mesma API das versões em pasta. Pacotes identificados como atualização ou DLC não são tratados como executáveis independentes.

## Validação no PC

Comparação de leituras do PKG com as cópias extraídas dos mesmos títulos, incluindo início, fim e offsets distribuídos em cada arquivo:

| Jogo | Arquivos | Leituras | Bytes comparados |
| --- | ---: | ---: | ---: |
| PAC-MAN 256 | 55 | 275 | 12.899.802 |
| Chronicles of Teddy | 1.381 | 6.905 | 160.669.600 |
| Minecraft Preview | 18.848 | 94.235 | 588.512.799 |

As comparações passaram sem diferenças nos intervalos lidos. São verificações por amostragem de conteúdo, não hashes completos de todos os arquivos.

Execução direta dos mesmos PKGs no harness desktop com Dozen:

| Jogo | Rota de acesso | Duração | Apresentações |
| --- | --- | ---: | ---: |
| PAC-MAN 256 | Arquivo dentro de pasta autorizada | 30 s | 864 |
| Chronicles of Teddy | `StorageFile` individual | 45 s | 2.502 |
| Minecraft Preview | Arquivo dentro de pasta autorizada | 75 s | 1.874 |

Nos três casos: montagem inicializável, `BOOT_RESULT=0`, `SHUTDOWN=0` e `D3D_INFO_QUEUE_MESSAGES=0`. As capturas confirmam apresentação de imagem; não representam validação completa de gameplay. O seletor e a restauração da FutureAccessList foram compilados, mas não exercitados em aplicativo instalado nesta revisão.

O bundle Release foi assinado com `CMS_SIGNATURE=PASS`, e seus arquivos foram comparados com o runtime e o executável produzidos pelo build. O núcleo empacotado tem SHA-256 `02cdab2cef84344ed13e1a6ee126d2e1120fe26ecc0b53b401d9ef6fa09d2d56`, igual ao núcleo usado nas execuções acima.

Evidências locais ficam em `Build/pkg-*-compare.log`, `Build/pkg-*-boot.log`, `Build/pkg-frontend-build.log` e `Build/pkg-core-build.log`. Boot e apresentação em harness desktop, validação do pacote e execução instalada no Xbox são verificações distintas.
