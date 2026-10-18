# Coerência de memória no D3D12 nativo

O caminho nativo usa posse temporária de páginas durante cada comando. Essa
implementação é conservadora: mantém a espera da GPU e o readback por
draw/dispatch/resolve. Ela não implementa submissões assíncronas.

## Posse CPU/GPU

Antes de ler os recursos de um comando, o runtime reúne buffers, imagens,
vertex/index buffers, render targets, depth/stencil e cores de borda customizadas.
Os endereços separados de leitura e escrita de depth/stencil também entram nessa
lista. Descritores de vertex buffers são obtidos antes da proteção das páginas.

`MemoryManager::AcquireGpuAccess` resolve esses intervalos no backing físico e
encontra todos os mapeamentos virtuais correspondentes. As páginas CPU de todos
esses aliases ficam temporariamente sem acesso. A exceção de acesso aguarda a
liberação da posse; não remove a proteção para deixar uma escrita CPU atravessar
um comando GPU em andamento. `CopySparseMemory` e `TryWriteBacking`, que acessam
diretamente o backing, participam do mesmo bloqueio.

O thread GPU continua acessando o backing sob o lease de memória. A posse só é
liberada depois da fence e do writeback de buffers, imagens e stencil. A liberação
restaura as permissões CPU anteriores e acorda os threads bloqueados. O guard RAII
também restaura as permissões quando o comando termina por exceção. O lease
impede alterações de map/unmap enquanto os ponteiros físicos estão em uso.

O handler de posse é consultado antes dos handlers de cache dos backends e é
desvinculado do gerenciador de memória na destruição. Cópias CPU sem uma posse
GPU ativa continuam podendo executar em paralelo. No unmap, faixas virtuais
contíguas podem atravessar vários intervalos do mapa físico; cada intervalo é
atualizado separadamente e permanece mapeado enquanto existir outro alias.

Essa política ordena também os acessos à memória sem relação de dependência que
compartilham uma página CPU com um recurso GPU. O custo de serialização é maior
que o de um tracker de páginas com downloads assíncronos. Os snapshots continuam
sendo comparados no upload; uma escrita CPU posterior à liberação é observada
no próximo comando, inclusive quando feita por outro alias.

## Aliases de buffers

O cache compara os segmentos físicos dos descritores e calcula seus offsets
relativos. Views que admitem uma representação linear compartilham um recurso
D3D12, mesmo quando seus endereços virtuais diferem ou o backing é segmentado.
O upload reúne os segmentos pelos endereços guest originais. O writeback usa os
offsets de cada view escrita e os respectivos destinos guest.

Uma representação que exija colocar duas posições físicas distintas no mesmo
offset do recurso D3D12 não é aceita. A repetição de um segmento físico dentro
de uma única view também continua sem suporte: ela exige tradução não linear
dos endereços de acesso do shader.

## Imagens e tiling

Cada operação de tiling/detiling mantém seus próprios parâmetros, uploads,
scratch buffers e readback até a conclusão da submissão. O tamanho do input é
definido antes de gravar sua cópia; ele não é realocado enquanto essa cópia está
pendente. Cada imagem recebe o readback da sua própria operação.

O tiling de volta à memória guest parte do snapshot original e atualiza somente
os texels da extensão visível. Padding, gaps entre layouts e bytes fora da
superfície permanecem preservados a partir do destino de escrita, inclusive
quando a leitura de depth/stencil usa outro endereço. O caminho promovido de
depth 16-bit inicializa também os texels fora da view com os dados do destino.
Quando o orçamento temporário de tiling GPU
não comporta outra operação, o comando usa a conversão CPU existente.

## Imagem e buffer com escrita simultânea

O compilador D3D12 detecta sobreposição entre imagens e buffers guest do mesmo
shader usando a resolução de aliases do runtime, inclusive endereços virtuais
distintos. As operações `IMAGE_LOAD`/`IMAGE_STORE` das imagens afetadas passam
a acessar o mesmo buffer raw GPU das views de buffer. Não há textura separada
nem cópia intermediária entre a escrita e a leitura. Imagens sobrepostas são
incluídas no mesmo lowering para não deixar uma view com snapshot desatualizado.

Uma tabela de offsets por texel é gerada com as funções existentes do tiler CPU
e carregada como buffer somente de leitura. Ela preserva o endereçamento linear,
microtiled/macrotiled, mips, layers, volume e amostras. A conversão de formato
reutiliza o lowering de buffers tipados. Swizzle, conversões numéricas e sRGB
permanecem no caminho de imagem. Barreiras de memória e descritores coerentes
preservam a ordem dos acessos por aliases; a sincronização entre invocações
continua sendo responsabilidade do shader guest.

As views raw respeitam alinhamento D3D12 de 16 bytes, com o restante do offset
nos parâmetros do shader. O writeback usa os intervalos guest das views escritas;
padding e bytes vizinhos permanecem no mesmo backing. `IMAGE_GET_RESINFO` usa as
dimensões da view. Atômicos inteiros de 32 bits com operação raw equivalente
também acessam esse backing compartilhado.

Os texels de 8/16 bits (`R8`, `RG8`, `R16`) são extraídos de palavras raw
de 32 bits. As escritas usam um loop de compare-and-swap sobre a palavra,
atualizando apenas os bits do texel. As operações byte/short das views de
buffer usam o mesmo caminho, incluindo o offset em bytes do descritor; assim,
invocações que escrevem vizinhos não sobrescrevem os demais texels. O perfil
D3D12 mantém os intermediários subword em 32 bits com truncamento/extensão
explícitos, sem exigir storage ou aritmética nativa de 8 bits no HLSL.

O caminho cobre os formatos suportados de 8/16/32/64/128 bits por texel e RGB de
96 bits linear. O limite de cada superfície é 16 MiB, as tabelas somam até
64 MiB por shader, e cada imagem usa duas entradas do orçamento de buffers.
BCn, depth, amostragem
filtrada/gather, atômicos sem equivalente raw implementado e conflitos entre
estágios ainda são rejeitados. Isso não representa coerência completa de todos
os layouts e operações possíveis.

## Limites que continuam explícitos

`IMAGE_LOAD` de uma imagem escrita pelo mesmo shader pode usar dois descritores
com o mesmo endereço base guest. O recompiler D3D12 detecta o alias e emite as
leituras como storage image/UAV, com swizzle no shader, conversão sRGB antes
do swizzle e barreira de memória de imagem antes de cada leitura por alias.
A escrita e a leitura usam a mesma textura nativa; não há cópia SRV intermediária.
O leitor continua marcado como somente leitura para o writeback. Views com mip
constante ou dinâmico usam os descritores UAV de cada nível.

Esse caminho cobre layouts que o cache consegue compartilhar e formatos com
suporte a typed UAV loads no dispositivo. A detecção no recompiler usa o endereço
base virtual do descritor e os recursos do mesmo shader: aliases por outros
endereços virtuais e entre estágios ainda podem ser rejeitados. A barreira
preserva a ordem de acessos da invocação; comunicação entre invocações continua
dependendo da sincronização do programa guest.

Posse CPU/GPU não torna duas representações GPU independentes coerentes dentro
de uma mesma invocação de shader. Feedback com `IMAGE_SAMPLE`/gather, feedback de attachment,
imagem/buffer fora do lowering acima e imagens sobrepostas com layouts
incompatíveis sem buffer compartilhado ainda precisam de um backing comum ou de um
lowering específico das operações de imagem. Esses estados continuam sendo
rejeitados; não são anunciados como coerência completa.

A visibilidade CPU/GPU é estabelecida nos limites de cada comando. Um shader
que espere uma escrita CPU durante a própria execução do dispatch não é coberto
por esse modelo de snapshots e readback: isso exige memória compartilhada viva
ou outro modelo de execução. A posse de páginas não substitui essa capacidade.

Comparações no PS4 estão na fonte OpenOrbis
[d3d12-guest/main.cpp](hardware/d3d12-guest/main.cpp). Não há resultado de PS4,
AppContainer ou Xbox para estas mudanças.

## Diagnóstico no PC

`Build/native-image-buffer-validation.log` verifica escrita da imagem lida pelo
buffer e escrita do buffer lida pela imagem, com igualdade byte a byte do restante
da alocação. Os casos cobrem o mesmo endereço e aliases físicos, linear,
microtiled e macrotiled real, atômicos `IMAGE_ATOMIC_ADD`/`BUFFER_ATOMIC_ADD`,
acessos fora dos limites, mip 1, camada 2 de array, volume 3D, imagem 1D,
amostra 3 de MSAA e conversão RGBA8 UNORM/sRGB. A regressão GCN completa terminou
com `D3D12_VALIDATION_ERRORS=0`. A fonte OpenOrbis inclui `IMAGE_BUFFER_WRITES`
para comparação em hardware; ainda não foi compilada nem executada no PS4.

O diagnóstico temporário `Build/native-feedback-validation.log` verifica
feedback entre dois descritores no mesmo dispatch: 256 texels UNORM com swizzle,
256 texels sRGB com decode antes do swizzle, cópia `R32_UINT` de `12345678`,
constante de swizzle inteira `00000001` e
seleção de mip constante/dinâmica. A regressão GCN nativa inclui os casos
existentes de draw, amostragem, depth e MSAA; o resultado foi zero erros de
validação D3D12. Isso não valida feedback filtrado nem execução em UWP/PS4/Xbox.

O diagnóstico temporário em `Build/native-coherence-msaa-validation.log` cobre
leitura CPU, escrita CPU raw, escrita pelo backing e unmap concorrentes com um
dispatch GCN real. A leitura deve observar `11223344` depois da fence; as
palavras CPU vizinhas devem preservar `aabbccdd` e `55667788`. Um segundo dispatch
escreve por um buffer segmentado, lê por outro descritor sobreposto e verifica
`13579bdf` nos dois segmentos.

O caso com duas imagens tiled verifica 128 texels e 128 posições de padding.
Outro caso submete três imagens maiores e excede o orçamento de tiling GPU: uma
usa tiling GPU e duas usam fallback CPU, verificando 3.142.656 texels e 3.072
posições de padding. O diagnóstico também verifica a restauração das permissões
por exceção e o término do unmap depois da liberação da posse.

O caso de depth com endereços separados verifica 256 valores visíveis gravados,
768 posições de padding preservadas no destino e 1.024 valores intactos na
origem. A execução terminou com `D3D12_ERRORS=0`. As regressões existentes em
`Build/native-coherence-gcn-regression.log` e
`Build/native-coherence-stencil-regression.log` também terminaram sem erros de
validação. Stencil MSAA 16× foi ignorado porque o formato não é suportado no
dispositivo de validação; isso não é resultado positivo para esse caso.

As cópias temporárias do construtor de `MemoryManager` e do setup de
`AddressSpace` inicializam o SDK em `0x05000000`, pois o diagnóstico não carrega
um ELF para preencher `ElfInfo`. A lógica de posse, cache, runtime, shaders e
operações de mapa é a de produção; esses construtores de diagnóstico não são
incluídos na DLL entregue. Isso valida os casos exercitados no PC, não o boot
de jogos ou o ambiente AppContainer.

O build completo Release x64 e a assinatura do MSIXBundle passaram
(`Build/native-coherence-uwp-build.log`, `CMS_SIGNATURE=PASS`). Os hashes de
`shadPS4.dll`, `dxcompiler.dll` e `dxil.dll` dentro do pacote correspondem aos do
runtime; a DLL principal corresponde também à saída de produção do CMake
(`Build/native-coherence-package-verification.log`). O pacote não foi instalado
nem executado.

### Validação de aliases com texels de 8/16 bits

O diagnóstico nativo temporário passou 30 casos de visibilidade imagem/buffer
com `R8_UINT`, `RG8_UINT` e `R16_UINT`, cinco modos de tiling e aliases físicos.
Nove configurações adicionais passaram 16 repetições com 64 invocações cada,
escrevendo texels e elementos byte/short adjacentes no mesmo backing: 9.216
invocações, com comparação integral da alocação e preservação de padding.
Dez casos verificaram conversões UNORM, SNORM, SINT e FP16, leitura fora dos
limites retornando zero e escrita fora dos limites preservando a alocação.
Outros dez verificaram mip, array com base layer, volume, 1D e MSAA 4x para
`R8_UINT`/`R16_UINT`. O diagnóstico também executou as regressões GCN/MSAA,
terminando com `D3D12_VALIDATION_ERRORS=0` e validação GPU habilitada no PC.

O Release x64 do ShadPS4-UWP foi recompilado e o bundle assinado, com
`CMS_SIGNATURE=PASS`. O core do pacote corresponde ao build de produção e às
DLLs de runtime verificadas (`Build/native-subword-package-verification.log`);
o readback final está em `Build/native-subword-msaa-validation.log`.
O pacote não foi instalado nem executado.

O homebrew OpenOrbis inclui comparação bidirecional `R8_UINT`/`R16_UINT`,
com offsets dentro da palavra e verificação dos bytes vizinhos. Essa extensão
não foi compilada nem executada em PS4. Os resultados nativos não comprovam
jogos, AppContainer ou Xbox.

### Alternate tiling Neo e RGB96

Alternate tiling usa `GetAltPipeConfig`, `GetAltBankHeight`, `GetAltNumBanks`
e `GetAltMacrotileAspect` no tiler CPU e no compute shader D3D12. O cálculo de
pipe inclui os quatro bits de `P16_32x32_8x16`; mips que caem para microtiling
continuam usando o layout correspondente. Os modos PRT 2D/3D mantêm os bits de
coordenadas acima do macro tile; somente os modos PRT sem rotação limitam essas
coordenadas ao macro tile.

Em descritores de imagem e color buffer Neo, os bits de bank swizzle são
extraídos da base codificada e removidos do endereço guest antes da resolução
de aliases. O cache D3D12 inclui a seleção de alternate tiling na identidade da
imagem, mesmo quando os tamanhos e o bank swizzle coincidem com os normais.
O lowering imagem/buffer aceita esse layout para os formatos já suportados.
Compressão, depth e conflitos entre estágios conservam as restrições anteriores.

RGB96 tiled não foi habilitado. O AddrLib da AMD, incluído em
`externals/mesa-uwp/src/amd/addrlib/src/core/addrlib1.cpp`, exige `IsLinear`
quando o formato usa `ADDR_EXPANDED`, como `ADDR_FMT_32_32_32` definido em
`addrelemlib.cpp`. Não há nesse contrato um layout tiled RGB96 para implementar
como suporte nativo. O caminho linear continua disponível; a tabela de aliases,
o tiler CPU e o cache D3D12 rejeitam explicitamente RGB96 tiled, evitando
usar o endereçamento de 64/128 bits ou copiar texels através dos bits de pipe/bank.

O homebrew OpenOrbis inclui duas variantes Neo `R32_UINT` com
`Display2DThin`, bancos 0/1 e comparação de todos os 64 KiB da alocação. Essa
extensão ainda não foi compilada nem executada em PS4 por ausência do SDK.
