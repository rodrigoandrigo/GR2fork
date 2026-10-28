# Gerenciamento de memória UWP baseado no Kyty

A referência é a cópia local de `KytyPS5-xbox-on-upstream`, cujo funcionamento
no Xbox com jogos de PS5 foi confirmado pelo usuário. A integração preserva
o modelo de memória física do shadPS4 e seus aliases; não substitui RAM
compartilhada por cópias privadas.

## Correspondência dos mecanismos

| Kyty | Adaptação no shadPS4 |
| --- | --- |
| Janelas de 64 KiB | Cópias divididas nas fronteiras de 64 KiB; acesso temporário ao armazenamento e fallback de views persistentes de 1 MiB para 64 KiB. |
| Arquivo de paginação | Arquivos temporários por seção de 16 MiB, já existentes, preservados durante a retirada e recriação dos mapeamentos. |
| Page-in antes de expor o endereço | Buffers usados pelo host são materializados antes da chamada ao sistema. Acesso ao armazenamento por aliases temporários não depende de uma falta de página dentro de `memcpy`. |
| Pin de buffers de I/O | Leases RAII em arquivos e sockets, com preparação das páginas e liberação inclusive durante exceções. |
| Pins de pilhas e threads | A view usada e sua seção física ficam protegidas; outros aliases podem ser retirados e restaurados. Pilhas de pthreads, sinais, fibers e TLS permanecem protegidos. |
| Bloqueio das cópias | As cópias do backing mantêm o bloqueio do espaço de endereços e pins das duas pontas durante cada janela. |
| Leitura direta do swap | Uma janela ausente do backing pode ser lida ou escrita por um alias temporário do mesmo arquivo, sem repopular o endereço canônico. |
| Orçamento dinâmico | Consulta de uso/limite UWP a cada 100 ms, descontando outros consumidores e evitando contar aliases físicos repetidos. Margem de 768 MiB e recuperação de emergência abaixo de 384 MiB livres. |
| Adaptação após falta de recursos | Reutilização de views frias e fechamento de seções sem usuários; emergências reduzem o orçamento em 256 MiB, com recuperação gradual. |
| Segunda chance | Regiões frias perdem temporariamente o acesso; um novo acesso restaura as proteções originais e cancela a retirada. A retirada normal exige uma segunda passagem sem acesso. |
| Controle de quantidade de views | Retirada preventiva acima de 6.000 views residentes. O número é uma política conservadora, não uma afirmação sobre o limite de todos os Xbox. |
| Encerramento do paginador | Solicitação de parada, despertar e `join` ao parar o jogo e antes de descarregar a DLL; o início de uma sessão reativa o trabalhador quando necessário. |
| Contadores | Page-ins, retiradas, segundas chances, recuperações de emergência e cópias diretas do armazenamento no log de encerramento. |

## Diferenças necessárias

No Kyty, a transição de memória privada para slots exige cópia, checksum e
detecção de escritas concorrentes durante a cópia. No shadPS4, a página continua
pertencendo à mesma seção de arquivo através de todos os aliases. A retirada
remove views, preservando o armazenamento compartilhado. Não há um snapshot
privado que possa divergir das escritas do guest. Antes de fechar uma seção
inativa, o arquivo é sincronizado; uma falha impede seu fechamento nesse caminho.

As proteções de páginas continuam sendo preservadas e restauradas pelos caminhos
existentes, inclusive as proteções usadas pela coerência CPU/GPU. A recuperação
não promove páginas protegidas para escrita indiscriminadamente. Regiões
executáveis e faixas fixadas não são escolhidas para retirada preventiva.

O contador de bytes mapeados representa cobertura física de views, não uma
medição individual do working set de cada página. O consumo real usado para
detectar pressão vem de `Windows.System.MemoryManager`.

## Verificação

O homebrew OpenOrbis em `scripts/openorbis/ram-alias-windows` verifica aliases,
proteções, execução e persistência dos dados através de I/O e de um intervalo
ocioso. Ele compara semântica com o PS4; o limite de views e a pressão do
AppContainer precisam ser verificados separadamente no Windows/Xbox.

Build do núcleo, execução no PC, execução do homebrew no PS4 e validação no Xbox
são evidências distintas. O funcionamento do Kyty no Xbox não substitui a
validação desta adaptação.

Verificação local desta integração:

- Núcleo UWP Release compilado; uma segunda chamada ao build não encontrou trabalho pendente.
- Verificadores nativos existentes passaram com swap ligado e desligado, aliases concorrentes
  cobrindo 1 GiB, proteções de 4 KiB e memória acima de 4/7 GiB.
- Cotas artificiais de 32 views e 16 seções passaram, incluindo fallback físico de 64 KiB,
  preservação de regiões fixadas e recriação das seções com o conteúdo intacto.
- Bloodborne completou 90 ciclos do harness no PC, com 3.682 apresentações, encerramento normal
  e zero mensagens na fila de diagnóstico D3D.
- Na versão final, outra sessão de 30 ciclos realizou 56.697 cópias diretas do armazenamento;
  parada, descarregamento da DLL, reinicialização e novo encerramento passaram.
- Pacote Release assinado: assinatura CMS e hashes do payload conferidos. Sem instalação.

A política preventiva de segunda chance ainda precisa ser exercitada sob pressão real no Xbox.
O homebrew ampliado não foi executado no PS4. Esses resultados de boot não comprovam gameplay.
