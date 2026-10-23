# Boot e encerramento

Compile `main.cpp` como homebrew OpenOrbis, ligando libkernel e pthreads. A variante normal lê `/app0/eboot.bin`, executa duas threads, aguarda suas saídas e retorna zero.

Compile também com `-DEMBEDDING_MANUAL_STOP`. Essa variante mantém as threads
ativas. No host UWP, escolha sua pasta, inicie, pause, continue e pare pelo
frontend. O aplicativo deve continuar respondendo e permitir iniciar a variante
normal após recarregar a DLL e restaurar os grants. A leitura externa deve usar
StorageFolder; não conceda acesso por um caminho físico alternativo.

O botão Parar e a recarga da DLL pertencem ao host de embedding. A validação nativa usa um ELF mínimo independente e não substitui execução no AppContainer.
