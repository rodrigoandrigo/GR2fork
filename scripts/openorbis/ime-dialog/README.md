# Comparação do diálogo de texto no PS4

Adicione `main.c` a um projeto OpenOrbis com apresentação gráfica e atualização
de vídeo durante a coleta. Vincule `SceImeDialog`, `SceUserService`, `kernel` e `c`.
O diálogo usa buffers UTF-16 explícitos para não depender do tamanho de `wchar_t`
do compilador. O SDK não está instalado neste ambiente; esta fonte não foi
compilada nem executada no PS4.

Execute a mesma sequência no PS4 e no ShadPS4-UWP:

1. Substitua `Seed` por um nome e confirme.
2. Altere o texto e cancele; confira se `Seed` foi preservado no buffer.
3. Digite texto acentuado e, se aceito pelo teclado, um caractere fora do BMP.
   Tente ultrapassar o limite de 16 unidades UTF-16 antes de confirmar.

Compare `/data/ime-dialog.txt`: status final, resultado, terminação NUL,
conteúdo UTF-16 e os valores de guarda `12345678/5678`. A coleta aguarda até
cinco minutos por diálogo e registra timeout se precisar abortar.

Assinaturas e estruturas conferidas nos headers oficiais
[ImeDialog.h](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/ImeDialog.h)
e [_types/ime_dialog.h](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/_types/ime_dialog.h).
