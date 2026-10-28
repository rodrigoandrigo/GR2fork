# Comparação de semântica de arquivos em hardware

Adicione `main.c` a um projeto console OpenOrbis, usando o startup, as bibliotecas
POSIX e o empacotamento do próprio toolchain. Execute o mesmo pacote no PS4 e no
emulador. Não há dependência de um jogo comercial.

O argumento opcional seleciona a raiz gravável; o padrão é `/data`. O programa
exige que `shadps4-exodus-hardware` ainda não exista e só remove os próprios arquivos.

Os resultados verificam identidade de inode entre `stat`, `fstat` e caminhos com
`./`; tamanho após escrita; truncamento negativo; locks compartilhados/exclusivos,
downgrade e liberação ao fechar; remoção de diretório não vazio; criação com pai
ausente; e leitura/escrita de `/dev/null`. Cada grupo imprime `PASS` ou `FAIL`.
`unavailable` indica que a área não pôde ser preparada.

Esta fonte é um teste de hardware, conforme o AGENTS.md. O SDK OpenOrbis e um PS4
não estavam disponíveis nesta integração; a fonte não foi compilada nem executada
em hardware. Ela não valida shaders, rede, serviços de música ou o desempenho da GPU.
