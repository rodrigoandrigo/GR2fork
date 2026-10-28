# Criação de arquivos pelo convidado

Adicione `main.c` a um projeto console OpenOrbis com o startup e o
empacotamento do toolchain. O programa usa a interface POSIX da libc.

O destino padrão é `/data`. Um argumento pode selecionar outra área gravável
disponível no título, como `/download0` ou `/download1`. A ausência da área é
reportada como `UNAVAILABLE`, separadamente de uma falha nas operações.

O programa cria e remove somente `shadps4-create-probe.bin` na área selecionada.
Verifica criação, escrita, seek, leitura, exclusividade, truncamento e criação
com acesso somente de leitura. Também verifica extensão com zeros e redução
por `ftruncate`, preservando a posição do descritor. O resultado esperado numa área gravável é
`FILESYSTEM_CREATE root=/data result=PASS`, com retorno zero.

Fonte fornecida para comparação em PS4 e no emulador. Não executada em PS4
nesta revisão.
