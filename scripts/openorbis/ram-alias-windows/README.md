# RAM compartilhada e limites de visões

Adicione `main.c` a um projeto console OpenOrbis com libkernel. Execute o mesmo
homebrew no PS4 e no emulador. A fonte aloca 16 MiB de memória direta, cria dois
aliases, verifica dados a cada 64 KiB, altera a proteção de uma subfaixa e remapeia
uma janela de 64 KiB no endereço original. O resultado esperado é
`RAM_ALIAS_WINDOWS result=PASS`.

A etapa de I/O grava um trecho de um alias em
`/data/ram-alias-windows.bin`, zera a RAM pelo outro alias, aguarda 1,5 segundo
e lê o arquivo diretamente na memória compartilhada. Ambos os aliases devem
observar o padrão original. O arquivo temporário é removido ao concluir.

Também verifica a transição de uma visão de dados para leitura/execução, executa
uma função que retorna 42 e retorna a visão para leitura/escrita. O conteúdo
precisa continuar compartilhado entre os aliases após ambas as transições.

Uma etapa adicional cria 64 aliases da mesma RAM e confirma a visibilidade das
escritas ao atravessar várias visões. No PC, a recuperação de pressão também é
verificada com uma cota artificial de visões simultâneas e com pilhas protegidas
contra remoção. A RAM física permanece compartilhada quando uma visão é retirada
e reconstruída.

Pilhas de pthreads, sinais e fibers, TLS/TCB e metadados das threads são mantidos
fora da recuperação de visões. O teste no PC também descarrega o núcleo e
recria a apresentação depois de encerrar o Bloodborne.

A recuperação de swap fecha seções inativas, preserva os arquivos abertos e
recria os mapeamentos no próximo acesso. A verificação no PC força uma cota de
16 seções durante o acesso a 1 GiB e compara os valores entre aliases após cada
recriação, incluindo proteção de páginas, execução e limpeza dos arquivos.

O homebrew também copia 16 KiB no interior de uma seção. A verificação nativa
força a recusa de visões físicas maiores que 64 KiB e repete a cópia no offset
do log, confirmando o fallback e o deslocamento correto dentro do arquivo.

O teste compara a semântica visível ao jogo; não reproduz o limite de recursos do
Windows/Xbox. A recuperação de `MapViewOfFile3` também foi exercitada separadamente
no PC com recusas de mapeamento injetadas. O SDK OpenOrbis e um PS4 não estão
disponíveis nesta máquina; este homebrew não foi compilado nem executado no PS4.
