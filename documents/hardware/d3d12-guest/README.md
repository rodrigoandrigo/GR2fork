# Homebrew OpenOrbis: memória guest e VideoOut

`main.cpp` é uma fonte de homebrew para comparar PS4 físico e o caminho D3D12.
Não pertence ao build do emulador nem a uma suíte de testes automatizados.

`videoout-hdr.cpp` é um programa separado para comparação de apresentação SDR,
FP16 e BT.2020/PQ. Use-o como `main.cpp` em outro projeto OpenOrbis, ligando
`libkernel` e `libSceVideoOut`. Ele alterna SDR → FP16 → PQ → FP16 → SDR,
mantendo cada estágio por 600 flips concluídos. As oito faixas PQ representam
0, 10, 80, 100, 203, 400, 1000 e 10000 nits. As faixas FP16 representam valores
lineares 0, 0,125, 1, 1,25, 2,5, 5, 12,5 e 125; a metade inferior separa os
canais para observar sua ordem. Registre os retornos de registro/flip e compare
as faixas e transições com captura e medição em PS4 físico e no emulador, usando
a mesma configuração de saída. A fonte não configura o modo global HDR do
console; registre também esse modo e a capacidade da tela. Um retorno positivo
da API não comprova luminância, gamut ou equivalência de FP16 com o PS4.
Essa fonte ainda não foi compilada nem executada em hardware por ausência do SDK.

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

O descritor desse dispatch cobre 16 bytes, embora o shader escreva apenas a
primeira palavra. Uma thread CPU escreve continuamente `aabbccdd` e `55667788`
nas duas palavras seguintes pelo segundo alias enquanto o comando é submetido.
Depois da fence e do join, `CPU_GPU_NEIGHBORS` deve preservar essas palavras.
Esse caso observa a preservação de bytes vizinhos; ele não exige que uma CPU
leia um valor definido do mesmo endereço escrito pela GPU sem sincronização.
As declarações de `scePthreadCreate`/`scePthreadJoin` vêm do
[libkernel.h do OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/libkernel.h).

O caso `SEGMENTED_IMAGE_BUFFER_ALIAS` lê o mesmo dado físico como imagem
`R32_UINT` e buffer raw, usando dois endereços virtuais distintos. Um dispatch
grava os dois resultados em um buffer de oito bytes que cruza uma fronteira de
64 KiB. As duas páginas desse destino apontam para regiões físicas separadas
por 1 MiB. O resultado esperado é `11223344` em ambas as palavras, tanto no
destino virtual quanto nos aliases físicos originais. O caso usa PM4/GCN
diretamente e `MAP_FIXED` apenas dentro da região previamente mapeada pelo
próprio programa. Registre a linha completa, modelo e firmware do PS4.
Essa extensão ainda não foi compilada nem executada em hardware.

`IMAGE_LOAD_FEEDBACK` escreve `12345678` em um texel `R32_UINT`, espera a
operação de imagem, lê esse texel por um segundo descritor com o mesmo endereço
e grava o valor no texel seguinte. Ambos devem conter `12345678` após a fence.
O dispatch tem uma invocação, evitando dependência de sincronização entre
workgroups. Uma segunda execução usa swizzle constante `1` no leitor; o segundo
texel deve conter o inteiro `00000001`. Registre a linha completa para comparar
com o lowering UAV do D3D12.
Essa extensão também não foi compilada nem executada em PS4.

`IMAGE_BUFFER_WRITES` usa uma imagem `R32_UINT` e uma view de buffer sobre a
mesma palavra. Uma invocação grava `12345678` pela imagem e lê pelo buffer;
depois grava `87654321` pelo buffer e lê pela imagem. O resultado esperado é
`image_to_buffer=12345678 buffer_to_image=87654321 source=87654321` após a
fence. O shader usa GLC e espera as operações de memória antes das leituras.
As variantes `format=1 bits=8` (`R8_UINT`) e `format=2 bits=16`
(`R16_UINT`) escrevem `78` pela imagem e `21` pelo buffer. Usam respectivamente
o byte 3 e o halfword 1 da palavra inicial `deadbeef`; os resultados esperados
são `source=21adbeef` e `source=0021beef`, com os demais bytes preservados.
Cada variante deve retornar `image_to_buffer=00000078 buffer_to_image=00000021`.
Essas extensões ainda não foram compiladas nem executadas em PS4.

As variantes `alt=1 bank=0/1` usam `R32_UINT`, `Display2DThin` e o bit
`ALT_TILE_MODE` do descritor, com base codificada separando os bits de banco do
endereço físico da superfície. O texel `(0,0)` corresponde ao offset 0 no banco
0 e ao offset 4096 no banco 1; cada caso exige
`image_to_buffer=12345678 buffer_to_image=87654321 source=87654321`
e verifica que todas as outras palavras dos 64 KiB permanecem `deadbeef`.
Execute num PS4 Pro com o bit `support_neo_mode` (23) do atributo do aplicativo
habilitado no PARAM.SFO e confirme `sceKernelIsNeoMode()`. Fora de Neo, essas
variantes imprimem `SKIP`; isso não conta como validação. Não há submissão de
RGB96 tiled: a referência AddrLib restringe esse formato a layout linear.

Em seguida registra o segundo alias como superfície VideoOut linear e mostra
por 300 flips um gradiente vermelho horizontal, verde vertical e quadriculado
azul de 32 pixels. Registre `CPU_ALIAS`, `GPU_ALIAS`, a fence,
`VIDEOOUT_CPU_HASH`, `VIDEOOUT_API`, modelo do PS4 e firmware; capture a imagem
para comparação. Sucesso da API e hash CPU não confirmam a imagem apresentada.

Este caso não cobre todos os 56 offsets do push buffer, tiling, GDS/LDS,
draws, MSAA, compressão, estágios avançados ou jogos. Essas comparações em
hardware continuam necessárias antes de aceitar o renderer como completo.
