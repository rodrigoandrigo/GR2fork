# Contextos de sinais aninhados

Adicione `main.c` a um projeto console OpenOrbis com as bibliotecas POSIX do SDK.
Execute o mesmo homebrew no PS4 e no emulador. O programa dispara `SIGILL` com
`ud2`; o handler entrega `SIGUSR1` e depois altera RAX e RIP do contexto externo.
O resultado esperado é `NESTED_SIGNALS outer=10000 inner=10000 result=PASS`.

O contexto do sinal interno não pode substituir o destino da restauração do sinal
externo. Uma falha pode repetir a instrução inválida ou perder o valor de RAX.
A fonte é um teste de hardware conforme AGENTS.md. O SDK OpenOrbis e um PS4 não
estão disponíveis nesta máquina; a fonte não foi compilada nem validada no PS4.
