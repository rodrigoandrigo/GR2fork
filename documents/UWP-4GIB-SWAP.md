# Historico do swap de 4 GiB

Este mecanismo foi substituido em 15/10/2026 pela memoria sob demanda descrita em [UWP-DEMAND-MEMORY.md](UWP-DEMAND-MEMORY.md). O swap esta desativado na implementacao atual; as secoes abaixo documentam a implementacao anterior e suas validacoes.

# Swap de 4 GiB no shadPS4-UWP

## Analise de xbox-swap-master

Referencia local: `C:/Users/rodri/Dev1/Projetos/xbox-swap-master`.

O projeto usa o1heap, reserva virtual, um arquivo e faltas de acesso para alternar paginas entre RAM e disco, com limite residente de 512 MiB. A implementacao nao foi copiada: `frealloc` pode ler alem da alocacao original, erros de I/O sao ignorados e a exclusao mutua do handler nao impede escritas concorrentes na pagina antes de ela ser retirada do acesso. Os aliases fisicos do PS4 tambem exigem tratamento proprio.

## Implementacao anterior

A implementacao reside em `src/core/address_space.cpp`, sob `SHADPS4_UWP`.

- A reserva inicial de 8448 MiB e virtual. Nao exige esse tamanho em RAM residente.
- O arquivo temporario esparso `cache/swap-4GiB-<pid>.tmp` tem exatamente 4.294.967.296 bytes logicos, verificados na inicializacao. O armazenamento fisico cresce conforme os blocos sao escritos.
- Os primeiros 4096 MiB da memoria fisica emulada usam esse swap. Apenas 512 MiB desse intervalo permanecem residentes no pool de RAM compartilhada, com 32 frames de 16 MiB.
- Nao sao criadas views do arquivo para os aliases guest. ReadFile/WriteFile transferem os dados entre arquivo e pool residente. As views guest e canonicas apontam para a mesma secao anonima de RAM.
- Ao reutilizar um frame, todos os aliases guest e a view canonica correspondentes sao desmapeados antes da gravacao no arquivo. Novas faltas aguardam a exclusao mutua; o buffer privado de I/O permanece acessivel. Isso evita copiar uma pagina enquanto outra thread ainda pode modifica-la.
- As protecoes usadas pelo rastreamento da GPU sao preservadas e restauradas ao recarregar o alias. A politica de substituicao usa o ultimo carregamento/mapeamento de cada frame.
- O restante da memoria fisica usa secoes anonimas independentes de ate 16 MiB, criadas sob demanda com SEC_COMMIT. Cada view usa um offset local nessa secao; nao existe uma secao anonima global de 8 GiB no UWP. Antes de comprometer RAM adicional, consulta-se o orcamento WinRT, limitado a 5120 MiB, mantendo uma margem de 256 MiB. Se essa consulta nao estiver disponivel, a alocacao continua sujeita aos limites do sistema.
- Views de dados usam MapViewOfFile3FromApp com PAGE_READWRITE. Aliases executaveis em RAM usam a API de mapeamento que preserva as permissoes de execucao. A copia canonica de dados nao precisa ser executavel.
- Views, secoes e handles sao liberados ao destruir o espaco de enderecos. O arquivo usa FILE_FLAG_DELETE_ON_CLOSE.

Este mecanismo pagina memoria fisica emulada. Nao substitui globalmente malloc/new e nao transforma o limite de 5120 MiB em 9216 MiB de RAM residente. Recursos e caches D3D12 precisam continuar cabendo no orcamento. I/O pode reduzir o desempenho.

## Motivo da revisao de 15/10/2026

O log mais recente avancou pelo mapeamento logico dos 2 GiB iniciais, mas falhou ao ativar uma view do arquivo em MapRegion. A revisao anterior ainda dependia de mapeamentos do arquivo no Xbox. A implementacao atual usa I/O explicito para o swap e apenas RAM compartilhada para os aliases.

## Validacao

O diagnostico nativo em Build/ religa os objetos de producao em uma DLL privada com um ponto de entrada de diagnostico; ele nao e incluido no pacote. O harness inicializa metadados ELF sinteticos, sem executar um jogo, para isolar o espaco de enderecos. Escreve padroes em 1 GiB, excedendo o pool de 512 MiB, e verifica as releituras pelo alias original, por um segundo alias e pela copia canonica, em ordens opostas. Houve passagem do teste sequencial e do teste com leitores concorrentes.

Logs: `Build/swap-production-stress.log`, `Build/swap-production-concurrent-stress.log`, `Build/swap-explicit-game-verified.log`, `Build/swap-explicit-package.log`. O teste de PAC-MAN 256 no PC terminou com BOOT_RESULT=0 e SHUTDOWN=0.

O teste nativo nao comprova compatibilidade com o AppContainer do Xbox. A validacao no console, incluindo I/O e pressao de memoria, permanece pendente.

O teste com escritores e leitores concorrentes tambem passou (`Build/swap-production-readwrite-stress.log`). PAC-MAN 256 apresentou 660 frames em 25 segundos e encerrou sem erro. O pacote Release foi assinado e os hashes do payload passaram (`Build/swap-explicit-package-verification.log`).

## RAM acima de 4 GiB

O novo log passou pelo swap, mas falhou na view canonica da RAM fisica em 0x100000000. A secao RAM global foi substituida por secoes pequenas, com offsets locais, incluindo mapeamentos desalinhados em relacao ao bloco de 16 MiB. O diagnostico de producao confirmou coerencia em 4 GiB + 2 MiB, atravessando limites de blocos, e em 0x1f0000000. Tambem repetiu a gravacao e leitura concorrente do swap de 1 GiB. Log: `Build/ram-pages-production-stress.log`. Confirmacao no Xbox pendente.

O tratamento das faltas de memoria tambem impede cancelamento assincrono enquanto mantem as travas do paginador. Na validacao final, PAC-MAN 256 apresentou 851 frames em 25 segundos, com BOOT_RESULT=0, SHUTDOWN=0 e erro vazio (`Build/ram-pages-final-game.log`).
