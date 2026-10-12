# Homebrew OpenOrbis: memória guest e VideoOut

`main.cpp` é uma fonte de homebrew para comparar PS4 físico e o caminho D3D12.
Não pertence ao build do emulador nem a uma suíte de testes automatizados.

Use um projeto criado com o toolchain oficial
[OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain), substitua o
`main.cpp` e ligue `libkernel`, `libSceGnmDriver` e `libSceVideoOut`, além do
runtime C/C++ usado pelo template. Preserve os módulos e materiais de pacote
do template. O SDK OpenOrbis não está instalado neste ambiente: esta fonte
ainda não foi compilada, empacotada ou executada em PS4.

O programa mapeia a mesma alocação física em dois endereços, verifica acesso
CPU pelos aliases e submete um dispatch PM4 com GCN `BUFFER_STORE_DWORD`.
Depois da conclusão e flush do GPU, ambos os aliases devem conter
`11223344`. A memória submetida permanece viva se a fence demorar.

Em seguida registra o segundo alias como superfície VideoOut linear e mostra
por 300 flips um gradiente vermelho horizontal, verde vertical e quadriculado
azul de 32 pixels. Registre `CPU_ALIAS`, `GPU_ALIAS`, a fence,
`VIDEOOUT_CPU_HASH`, `VIDEOOUT_API`, modelo do PS4 e firmware; capture a imagem
para comparação. Sucesso da API e hash CPU não confirmam a imagem apresentada.

Este caso não cobre todos os 56 offsets do push buffer, tiling, GDS/LDS,
draws, MSAA, compressão, estágios avançados ou jogos. Essas comparações em
hardware continuam necessárias antes de aceitar o renderer como completo.
