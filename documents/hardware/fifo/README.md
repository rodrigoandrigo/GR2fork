# Homebrew OpenOrbis para FIFO

`main.cpp` envia 240 frames por `sceVideoOutSubmitFlip`, com três buffers lineares
e IDs sequenciais em `flipArg`. As 24 faixas no topo codificam o ID em binário,
com o bit menos significativo à esquerda. O buffer só é reutilizado depois de
uma apresentação posterior à sua última utilização.

Compile como aplicação OpenOrbis, vinculando `libkernel` e `libSceVideoOut`.
O SDK OpenOrbis não está instalado neste ambiente; este fonte não foi compilado
nem executado em PS4.

O resultado esperado é `COMPLETE flips=240 arg=239 pending=0`. Esse resultado
valida o fluxo de flips da aplicação, mas não comprova scanout físico. Para
verificar a tela, capture a saída HDMI e decodifique os IDs das faixas: todos
devem aparecer em ordem. Repetições por refresh são permitidas; inversões e
IDs ausentes não são. Registre modelo do PS4, firmware, refresh e método de
captura, e repita a comparação no host PC e no Xbox quando o boot guest estiver
disponível.

Não existe pausa fixa entre submissões: a espera serve para reutilizar os
buffers e aguardar a conclusão. A taxa física da tela continua aplicável.
