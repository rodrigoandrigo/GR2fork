# Homebrew OpenOrbis para FIFO

`main.cpp` envia 240 frames por `sceVideoOutSubmitFlip`, com três buffers lineares
e IDs sequenciais em `flipArg`. As 24 faixas no topo codificam o ID em binário,
com o bit menos significativo à esquerda. O buffer só é reutilizado depois de
uma apresentação posterior à sua última utilização.

Compile como aplicação OpenOrbis, vinculando `libkernel` e `libSceVideoOut`.

O resultado esperado é `COMPLETE flips=240 arg=239 pending=0`. Esse resultado valida o fluxo de flips da aplicação, mas não comprova scanout físico. Para verificar a tela, capture a saída HDMI e decodifique os IDs das faixas: todos devem aparecer em ordem. Repetições por refresh são permitidas; inversões e IDs ausentes não são.

Não existe pausa fixa entre submissões: a espera serve para reutilizar os
buffers e aguardar a conclusão. A taxa física da tela continua aplicável.
