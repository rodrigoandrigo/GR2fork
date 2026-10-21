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

As duas variantes `IMAGE_LOAD_FEEDBACK virtual_alias=1` usam o segundo
mapeamento virtual do mesmo direct memory como descritor de leitura, enquanto
o descritor de escrita continua no primeiro mapeamento. Ambas devem imprimir
`PASS`, com `writer=12345678` e `reader=12345678` ou `reader=00000001` na
variante de swizzle constante. O shader grava pelo primeiro endereço, lê pelo
segundo e grava o valor observado no texel seguinte do primeiro. Os quatro
casos de feedback usam endereços separados para código e metadados. A extensão
com aliases virtuais distintos aguarda compilação com o SDK e execução em PS4.

O grupo `IMAGE_BUFFER_ATOMIC` submete 47 casos em ambas as ordens (94
submissões): operação
pela imagem seguida de `BUFFER_ATOMIC_ADD(0)`, ou operação pelo buffer seguida
de `IMAGE_ATOMIC_ADD(0)`. Confere o valor anterior com GLC, a observação do
resultado pela outra view, a palavra final e a preservação do vizinho. Inclui
SUB, INC/DEC nos limites, FMIN/FMAX e FCMPSWAP com sinais, zeros, infinidades,
NaNs e subnormais, além de exchange, compare-swap inteiro e operações lógicas.
Cada submissão usa código e metadados em um endereço próprio para não depender
da reutilização do cache de instruções. Registre também os casos divergentes,
especialmente o tratamento de NaN e denormais, para comparação com a ISA.
Esta extensão não foi compilada: o SDK OpenOrbis não está disponível neste
ambiente. A execução e a comparação em PS4 continuam pendentes.

O grupo `STAGE_SHARING_VS_PS` submete dois draws com um triângulo cobrindo
16x16 pixels. O vertex shader grava `0.5f` em três texels `R32_FLOAT`; o pixel
shader lê o primeiro texel, grava o resultado em uma região separada da mesma
superfície e exporta vermelho para um render target RGBA8. A variante
`virtual_alias=1` usa o segundo endereço virtual no pixel shader. Os casos
esperam `colored=256 padding=PASS PASS`, 259 texels com bits `3f000000` no
backing compartilhado e preservação de todos os demais texels/padding. O target
usa pitch de 64 pixels e exige `ff000080` na área 16x16. O EOP estabelece a
fence de leitura de volta. Registre os resultados e eventuais timeouts; esse
novo grupo também aguarda compilação com o SDK OpenOrbis e execução em PS4.

O grupo `NONLINEAR_PHYSICAL_ALIAS` mapeia duas páginas virtuais de 64 KiB em
ordem física invertida ou repete a mesma página física nas duas posições. Uma
escrita pelo descritor segmentado é seguida de incremento atômico pelo alias
linear e nova leitura pelo descritor segmentado. As variantes `repeated=0/1`
esperam `old=7 observed=8 physical=8 padding=PASS PASS`, preservando todos os
outros bytes das duas páginas físicas. Os mapeamentos são removidos após a
fence. O grupo ainda aguarda compilação com o SDK OpenOrbis e execução em PS4.

O grupo `IMAGE_RESIDENCY_CPU_GPU` executa duas submissões sobre a mesma imagem.
A primeira escreve na GPU e confere o resultado pela CPU. Em seguida a CPU grava
`87654321` no texel inicial e `aabbccdd` no padding do pitch; a segunda submissão
lê o texel e o grava no próximo texel. São exercitados o endereço original e um
alias virtual. O resultado esperado é
`IMAGE_RESIDENCY_CPU_GPU phases=2 virtual_alias=0/1 padding=PASS PASS`.
Cada fase aguarda seu EOP antes da inspeção CPU. O grupo ainda aguarda
compilação com o SDK OpenOrbis e execução em PS4.

O grupo `CMASK_MIXED` configura uma superfície RGBA8 linear com pitch 64,
extensão desenhada 16x16, CMASK de 2 KiB e primeiro nibble zero; os demais
nibbles ficam F. Um draw com `CB_COLOR_CONTROL.MODE=EliminateFastClear` deve
materializar `ff000080` somente no tile 8x8 inicial. O resultado esperado é
`clear_pixels=64 metadata=ff padding=PASS PASS`, preservando os demais pixels
e os outros bytes de CMASK. Os shaders e a metadata têm endereços próprios,
e o backing só é verificado depois do EOP. O caso aguarda compilação com o
SDK OpenOrbis e execução em PS4; registre modelo, firmware e saída completa.

O grupo `TYPED_CLEARS` repete a eliminação de fast clear com 12 variantes:
R8/R16/RG16/RGBA8 SINT, FP16 com infinidades e payloads NaN, FP32 com zero
negativo, RGBA32 UINT, swizzle RGBA8 e packed 565/1555/4444. Cada linha
`CMASK_MIXED format=... number=... swap=...` deve informar
`clear_pixels=64 metadata=ff padding=PASS PASS`; o resumo esperado é
`TYPED_CLEARS cases=12 PASS`. A comparação confere os bytes armazenados,
incluindo os bits de NaN e zero negativo. Este grupo ainda não foi compilado
por falta do SDK OpenOrbis nem executado em PS4; os resultados em hardware
continuam pendentes.

Em seguida registra o segundo alias como superfície VideoOut linear e mostra
por 300 flips um gradiente vermelho horizontal, verde vertical e quadriculado
azul de 32 pixels. Registre `CPU_ALIAS`, `GPU_ALIAS`, a fence,
`VIDEOOUT_CPU_HASH`, `VIDEOOUT_API`, modelo do PS4 e firmware; capture a imagem
para comparação. Sucesso da API e hash CPU não confirmam a imagem apresentada.

Este caso não cobre todos os 56 offsets do push buffer, tiling, GDS/LDS,
MSAA, compressão, geometry/tessellation ou jogos. Essas comparações em
hardware continuam necessárias antes de aceitar o renderer como completo.

Os casos `STAGE_SHARING_VS_PS blend_case=1..8` exercitam MIN/MAX em
RGBA8 UNORM com todas as combinações de fatores ZERO/ONE. Cada canal é
comparado com MIN/MAX dos operandos multiplicados pelos fatores; o padding
deve permanecer intacto. Esses casos registram a hipótese adotada pelo
renderer de referência e precisam ser comparados com um PS4 físico. A fonte
OpenOrbis foi ampliada, mas o SDK não está instalado para compilá-la aqui.

Os casos 9 e 10 usam fator de alpha constante de 0,5, respectivamente com
blend ativo e com `CB_COLOR_INFO.BLEND_BYPASS`. O caso 11 usa primitiva NONE
e exige preservação da imagem e ausência das escritas de VS/PS. O caso 12
habilita depth bounds fora do Z desenhado, sem depth/stencil attachment ativo,
e espera o mesmo resultado do draw normal. Registre também os valores obtidos
quando divergirem das hipóteses da referência.

Os casos 13/14 desenham um quad como TRIANGLE_STRIP com atributo flat
proporcional ao vertex ID (0, 0,25, 0,5 e 0,75), alternando o primeiro/último
provoking vertex. Esperam níveis 0/64 e 128/191 nos dois triângulos. Os pixels
sobre a diagonal são excluídos da comparação para não impor uma regra de
cobertura para a aresta compartilhada; os demais pixels e o padding são
verificados. Essas expectativas também aguardam comparação no PS4 físico.

`COMMAND_DEPENDENCIES` submete oito dispatches independentes e uma cópia por
alias físico após `ACQUIRE_MEM`, com uma fence final. Todos os destinos devem
conter `11223344`, incluindo `alias_copy`. Compara dependências e preservação
da memória submetida; não mede paralelismo da implementação D3D12. Este caso
ainda não foi compilado nem executado por ausência do SDK OpenOrbis.

`PERSISTENT_BUFFER_CPU_ALIAS` continua o caso de dependências: após a fence,
a CPU altera a palavra vizinha pelo segundo alias e outro dispatch a lê.
Espera-se `neighbor=aabbccdd original=11223344`. Compare esses valores no
PS4 físico; o caso não mede readbacks ou uploads internos do emulador.
Esta extensão ainda não foi compilada nem executada por ausência do SDK.

`indirect.cpp` é um homebrew separado para o P03. Ele compara hashes de draws
diretos com `DRAW_INDIRECT_COUNT_MULTI` e `DRAW_INDEX_INDIRECT_COUNT_MULTI`,
com strides de 24/28 bytes, contagem 0/1/2 e SGPRs de base/instância usados na
posição dos vértices. Compara triangle list/fan/strip e quad list/strip, incluindo
o padding do alvo. As linhas `INDIRECT_SGPRS` devem apresentar hashes iguais.
Use-o como `main.cpp` em outro projeto OpenOrbis e ligue libkernel e
libSceGnmDriver. Registre modelo/firmware e todos os hashes para comparar o
hardware com o emulador. Não foi compilado nem executado: SDK ausente neste
ambiente. Esses hashes não medem desempenho nem comprovam a ausência de
readbacks no emulador.

O caso `COMMAND_DEPENDENCIES` também lê duas vezes os mesmos 12 bytes de
`/app0/eboot.bin` para buffers locais da pilha após submeter comandos GPU.
`CPU_STACK_FILE_COPY bytes=12 result=PASS` exige leituras completas e idênticas;
a conclusão dos comandos e os aliases físicos continuam sendo verificados.
Esse acesso CPU não deve exigir que a pilha seja um recurso GPU com backing
Direct/Flexible/Pooled. Execute a fonte em PS4 físico e no emulador e registre
as duas saídas. Essa extensão ainda não foi compilada nem executada em PS4.

`READONLY_ATTACHMENT_FEEDBACK` usa um segundo color attachment com máscara de
escrita zero e lê seu backing como textura `R32_FLOAT` no pixel shader. O primeiro
attachment recebe vermelho 0,5; o backing lido e seu padding devem permanecer
inalterados. Repete com alias virtual distinto. O draw é submetido uma vez e não
escreve no attachment compartilhado. Essa extensão ainda não foi compilada nem
executada em PS4; não valida feedback com escrita nem depth/stencil.

`COHERENT_VERTEX_FETCH` grava três posições no vertex shader e as lê pelo fetch
shader na mesma invocação, após `s_waitcnt`. Repete a leitura por alias virtual
para o mesmo backing físico. Espera um quadrado verde de 16×16, as três posições
no buffer e padding preservado. `COHERENT_INDEX_FETCH` usa índices 32 bits e um
storage buffer sobre o mesmo backing; o VS grava a quarta palavra e incrementa
um contador na quinta. Espera o mesmo quadrado, `12345678` na quarta palavra e
contador 3. Repete com índice em alias físico. Os índices usados não são escritos
simultaneamente: o caso evita uma corrida sem resultado definido e observa
coerência do backing compartilhado e ausência de replay. As duas extensões ainda
não foram compiladas nem executadas em PS4 por ausência do SDK OpenOrbis.

`BOUNDED_INDEX_FETCH` mantém `INDEX_COUNT=3` e varia `MAX_SIZE` entre 0, 1, 2 e 3
no pacote `DRAW_INDEX_2`, usando um alias físico para os índices. Somente capacidade
3 deve produzir o quadrado; nos demais casos os índices ausentes são zero e a
primitiva é degenerada. O contador do VS é observado entre 1 e 3, sem presumir
reutilização exata pelo vertex cache. Não submete memória GPU desmapeada ao PS4.
Esta extensão ainda não foi compilada nem executada em hardware.

O grupo `CMASK_MSAA` acrescenta superfícies 2×/4×/8× com FMASK em memória,
CMASK inicialmente expandido D/E/F e um tile em clear 1/2/3. Após eliminar o
clear, conta os texels materializados e verifica a preservação dos outros
texels e dos estados FMASK. É uma referência para comparação em PS4, ainda
não compilada com OpenOrbis nem executada em hardware; não valida DCC, HTILE,
TC-compatible ou leitura de FMASK por shaders.
