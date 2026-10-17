# Memoria sob demanda no shadPS4-UWP

## Referencia analisada

`C:/Users/rodri/Dev1/Projetos/KytyPS5-xbox-on`:

- `src/kernel/memoryAddressSpace.inc`: backend lazy, reserva e compromisso no acesso, paginador opcional.
- `src/xbox/lazyRegions.h`: janelas de compromisso de 64 KiB e protecoes de 4 KiB.
- `src/common/platform/sysWindowsVirtual.cpp`: VirtualAllocFromApp/VirtualProtectFromApp e separacao de reserva e compromisso.

A adaptacao usa esse principio de alocacao sob demanda. Nao copia o backend privado nem o paginador do Kyty: o shadPS4 exige que os aliases guest e o acesso canonico compartilhem a mesma memoria fisica.

## Implementacao

Em `src/core/address_space.cpp`, somente para SHADPS4_UWP:

- A reserva inicial de 8448 MiB continua virtual; nao compromete esse volume em RAM.
- Todas as faixas fisicas usam secoes anonimas SEC_RESERVE de ate 16 MiB, criadas no primeiro acesso. Os offsets das views sao locais a cada secao, inclusive acima de 4 GiB.
- O handler compromete apenas uma janela de 64 KiB por falta de acesso, por VirtualAllocFromApp. Um bloco de 16 MiB nao implica compromisso de 16 MiB.
- Os aliases guest e canonicos apontam para a mesma secao. A granularidade de views permanece em 16 MiB para evitar multiplicar o numero de views por pagina de 64 KiB.
- Protecoes parciais de 4 KiB sao mantidas como intervalos sem sobreposicao, mesmo antes do compromisso. VirtualProtect atua somente nas paginas comprometidas; novas janelas recebem as protecoes dos aliases ao serem comprometidas. Permissoes de execucao existentes sao preservadas.
- Antes de novo compromisso, consulta-se o orcamento WinRT, limitado a 5120 MiB com margem de 256 MiB para o host. Se a consulta WinRT nao estiver disponivel, o limite do sistema ainda se aplica.
- O arquivo temporario de 4 GiB, o pool antecipado de 512 MiB e a logica de troca foram removidos. Swap esta desativado. A autorizacao para reativa-lo se necessario nao implica que esta revisao tenha um paginador ativo.
- As paginas tocadas permanecem comprometidas ate a destruicao do espaco de enderecos. Esta revisao nao implementa o descarte/paginador do Kyty; conjuntos de trabalho que excedam o limite real ainda podem esgotar a memoria.

## Validacao

Diagnosticos nativos locais, fora do pacote, em `Build/kyty-lazy-stress-final.log`:

- Compromisso de exatamente 64 KiB no primeiro acesso; a janela seguinte permanece MEM_RESERVE.
- Protecao de 4 KiB aplicada antes do primeiro acesso e alteracoes Read/None/ReadWrite.
- Aliases concorrentes sobre uma faixa virtual de 1 GiB.
- Aliases fisicos acima de 4 GiB e 7 GiB, cruzando limites de secoes.

O diagnostico usa os objetos reais de producao e metadados ELF sinteticos apenas para inicializar o espaco de enderecos. Nao adiciona exports de diagnostico ao core distribuido. A faixa virtual de 1 GiB e tocada esparsamente; esse diagnostico nao representa 1 GiB residente.

Build do core: `Build/kyty-lazy-core-final.log`.
Build e assinatura do pacote: `Build/kyty-lazy-package.log`.
Boot nativo real de PAC-MAN 256: `Build/kyty-lazy-game-final.log`.
Verificacao de conteudo do pacote: `Build/kyty-lazy-package-verification.log`.

O diagnostico nativo de PC nao substitui a execucao instalada em AppContainer nem a validacao de memoria no Xbox. Nenhuma instalacao foi feita nesta revisao.


## Orcamento das alocacoes graficas

Um novo log do Xbox chegou a compilacao dos shaders e falhou em
UniqueImage::Create com ErrorOutOfDeviceMemory. O arquivo nao continha o formato,
dimensoes ou HRESULT da alocacao, portanto nao permite atribuir a falha somente
a RAM fisica ou somente a uma descricao de recurso invalida.

No perfil SHADPS4_UWP, VMA agora recebe VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT
quando a extensao ja habilitada pelo core esta disponivel. Antes, as imagens
pediam WITHIN_BUDGET, mas o allocator nao usava o orcamento nativo da extensao.
O tamanho preferencial dos blocos grandes passa a 32 MiB, reduzindo a reserva
em blocos de 256 MiB do comportamento padrao, especialmente em pools distintos.
Requisicoes maiores continuam podendo criar blocos ou alocacoes dedicadas maiores.
Nao se desativa WITHIN_BUDGET nem se remove o limite de memoria do sistema.

Falhas de imagens registram formato, extent, mips, layers, samples, usage e flags,
bem como usage/budget e bytes reservados/utilizados por heap. Quando disponivel,
o core tambem registra AppMemoryUsage/AppMemoryUsageLimit do WinRT. O Dozen
registra HRESULT, tamanho e flags nas falhas de CreateHeap/CreateCommittedResource
e CreatePlacedResource para imagens; rejeicoes que nao sao E_OUTOFMEMORY deixam
de ser classificadas automaticamente como falta de memoria. Device removal
continua sendo reportado como VK_ERROR_DEVICE_LOST.

Evidencias: Build/image-memory-core-final.log, Build/image-memory-dozen.log,
Build/image-memory-game-final.log, Build/image-memory-package.log e
Build/image-memory-package-verification.log. O ensaio nativo de PC nao reproduz
o limite fisico do Xbox. A correcao da configuracao do allocator esta implementada;
a confirmacao de que ela resolve a alocacao no console permanece pendente.
