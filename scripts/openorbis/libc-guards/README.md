# Guardas de inicialização C++

Homebrew para comparação em PS4 com OpenOrbis. Adicione `main.c` a um projeto
console do toolchain, mantenha seu startup e empacotamento usuais e vincule
`libkernel`. A saída usa o stdout do projeto.

O programa resolve as três funções da biblioteca de sistema, verifica o byte
de conclusão, a repetição após abort e a publicação da inicialização para oito
threads concorrentes. A saída esperada contém
`LIBC_GUARDS constructors=1 value=12345678 result=PASS` e código de saída zero.

O mesmo ELF pode comparar o comportamento do HLE no emulador sem selecionar
jogos ou shaders específicos. O caminho do módulo é o da biblioteca presente
no próprio console; este projeto não distribui firmware.

Fonte fornecida para validação em hardware. Não executada em PS4 nesta revisão.
Contrato de referência: [Itanium C++ ABI, One-time Construction API](https://itanium-cxx-abi.github.io/cxx-abi/abi.html#once-ctor).
