# Memória UWP

O gerenciamento host UWP foi substituído pelo modelo de
`Test-project-arena-8ac58bdb-test-project/xbox/platform/guest_memory.inc` e
`guest_swap.inc`, incluindo `swap_codec.h` e `swap_pool_accounting.h`.
O pager anterior de seções de 16 MiB e views adaptativas foi removido do caminho
UWP. Windows desktop sem UWP e plataformas POSIX mantêm seus caminhos próprios.

## Modelo ativo

- Memória física lógica sem seção global previamente comprometida. Um único
  mapeamento usa páginas privadas de 16 KiB, comprometidas no primeiro acesso.
- Um alias físico vivo promove a página para uma seção compartilhada de 16 KiB.
  Escritas continuam visíveis por todos os aliases, inclusive pelo backing
  canônico usado pelos renderizadores. Seções executáveis são promovidas ou
  atualizadas com as permissões necessárias, preservando os dados.
- Desmapear memória direta preserva seus dados em snapshot, seção ou swap.
  Liberar a alocação física descarta o armazenamento; leases adiam esse descarte.
  Memória flexível reciclada começa zerada.
- Somente mapeamentos grandes de WC_GARLIC, sem execução, são candidatos a
  paginação. Memória flexível, CPU heaps, código, páginas compartilhadas e páginas
  fixadas não entram na retirada normal. Leases identificam as páginas físicas
  originais, mesmo quando o mapeamento virtual muda.
- Proteções de 4 KiB e a alternância escrita/execução permanecem preservadas,
  incluindo as proteções usadas pelo tracking de CPU/GPU.

## Swap e pressão

O clock de segunda chance retira páginas privadas frias em lotes de 2 MiB.
A política da referência inicia o trabalho abaixo de 640 MiB de folga no limite
real do processo e para novamente com 768 MiB. Ela substitui o limite fixo e as
inferências preventivas do pager anterior. Pedidos após falha de alocação podem
executar até três sweeps, com prazo limitado; as passagens posteriores podem
ignorar a segunda chance, mas nunca as leases.

O arquivo esparso temporário fica em `cache/swap`, com exclusão ao fechar.
Cada registro contém uma página de 16 KiB, comprimida quando útil, com marcador
para página zerada, fallback raw e verificação FNV. O teto padrão é 2 GiB e há
reserva de 384 MiB no dispositivo para saves e outros arquivos. O formato de
slots também limita o arquivo a 2 GiB. O pool de frames mapeados é limitado a
64 MiB e mantém seu compromisso de memória até o encerramento da sessão.

Quando o armazenamento não aceita uma escrita, um pool comprimido em RAM de
192 MiB pode preservar a página. O compromisso desse pool é contado por bitmap
real e desfeito após a restauração. Se nenhum armazenamento aceitar a página,
ela permanece residente. Registros substituídos por zero e alocações físicas
liberadas também devolvem seus blobs. Escritas recusadas não consomem
permanentemente posições novas no arquivo.

`General.memory_swap_enabled` mantém a seleção do aplicativo. Os controles de
ambiente importados usam o prefixo `SHADPS4_UWP_`, em lugar de `BB_UWP_`.
Nenhuma configuração depende de um título específico.

## Integração

[AddressSpace](../src/core/uwp/address_space.inc) mantém a descoberta e reserva de
VA, arquivos mapeados e o contrato público do GR2fork. O
[gerenciador C](../src/core/uwp/guest_memory.c) substitui o backing e o pager;
[MemoryManager](../src/core/memory.cpp) continua implementando as syscalls guest,
os tipos físicos e a sincronização com os renderizadores. As capacidades guest
continuam sendo as do emulador, em vez de copiar constantes do runtime de um
único jogo da referência.

Cópias físicas leem ou escrevem o proprietário, snapshot ou registro comprimido
sem criar um alias canônico permanente. Acesso host que realmente precisa de um
ponteiro materializa e fixa suas páginas. Leases de PM4, arquivos, sockets,
readbacks e submissões GPU permanecem integradas.

Dozen/Vulkan e D3D12 nativo pedem recuperação limitada de páginas guest após
falhas de alocação. Staging concluído pela fence pode ser liberado antecipadamente
sob pressão; imagens consideram também o limite do processo, preservando o
writeback e os retiros por fence existentes. A tradução de módulos de shader
pode abrir espaço antes de compilar. As arenas densas e o descarte diferido do
GR2fork continuam usando seu scheduler; a fila de preparação específica do
runtime de referência não foi introduzida no emulador.

Uma falha no handler de commit retorna o erro Win32 depois de liberar os locks,
sem propagar exceção C++ pela pilha guest e sem encerrar o processo. O mecanismo
existente de falha controlada da sessão permanece ativo. O pager tem parada,
join e limpeza de recursos antes de descarregar a DLL. Os contadores são
encaminhados ao logger do aplicativo.

## Verificação local

Backup inicial com manifesto e SHA-256:
`GR2fork-memory-backups/20261102-092351`, ao lado do checkout.

- Núcleo UWP Release compilado.
- Verificação isolada: 65 casos passaram, incluindo aliases executáveis, swap,
  fallback RAM após Win32 1453, retorno seguro após Win32 1455, transferências
  concorrentes, leases, reciclagem e reinicialização.
- Integração com os objetos de produção: duas sessões consecutivas passaram,
  incluindo desmapeamento parcial, proteção do tracker, cópias desalinhadas e
  preservação de dados privados/proteções de arquivos mapeados.
- Bloodborne chegou ao menu no harness PC: primeiro frame em 8.516 ms,
  2.718 frames em 100 segundos, captura visual conferida e encerramento normal.

- Pac-Man 256 chegou ao menu no harness PC: primeiro frame em 3.844 ms,
  5.610 frames em 100 segundos, captura visual conferida e encerramento normal.

Essas verificações não demonstram pressão real de memória nem gameplay no Xbox.
Não houve instalação de pacote.
