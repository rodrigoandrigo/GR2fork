# Observação de LWFS em hardware

Adicione `main.c` a um projeto console do toolchain OpenOrbis e vincule
`libkernel`. As assinaturas usadas estão no
[header público de OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/libkernel.h).

Execute numa área gravável do título. O padrão é `/download0`; um argumento
pode selecionar outra área já montada. O programa cria exclusivamente
`shadps4-lwfs-probe.bin`, recusa sobrescrever um arquivo existente e remove o
arquivo ao concluir. `UNAVAILABLE` indica que a abertura não foi possível.

Compare a saída entre PS4 e emulador: retorno, tamanho e posição antes/depois
de alocar, alocar um comprimento menor, alocar zero, seek e escrita LWFS. O
readback usa a interface POSIX para verificar se a escrita atingiu o arquivo.
O retorno zero do programa indica apenas conclusão da coleta, não equivalência.

Este programa coleta a semântica que falta documentar; não presume que alocação
LWFS tenha o mesmo efeito de `ftruncate`. Fonte fornecida, não executada em PS4
nesta revisão.
