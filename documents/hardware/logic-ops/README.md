# Logic ops em render targets: comparação OpenOrbis

Use `main.cpp` como programa separado de um template OpenOrbis e ligue
`libkernel` e `libSceGnmDriver`. A fonte submete GCN VS/PS e registradores CB,
executando as 16 ROPs em RGBA8 UNORM/SNORM/sRGB e RGBA16/RGBA32 float.
O pixel shader exporta (0,5; 0,25; 0,75; 1), o destino começa com bytes 0x35,
as máscaras alternam RGBA e RB, e XOR usa duas instâncias sobrepostas.

Compare os valores `raw` e `predicted_bitwise` com o renderer nativo. O resultado
previsto aplica a ROP aos bits após quantização; ele é uma hipótese a confrontar
com PS4 físico, especialmente para float, sRGB e arredondamento. Divergências
exigem investigar conversão/ROP do CB, e não ajustar valores por jogo ou shader.
A fonte mantém memória e comandos vivos enquanto aguarda a fence e lê o centro
somente após flush de cor e conclusão. Ela não cobre MSAA, MRT ou tiling.

Não foi compilada nem executada: o SDK OpenOrbis não está disponível neste
ambiente. Os readbacks D3D12 no PC validam a implementação contra o modelo
bitwise descrito, sem substituir a comparação em PS4.
