# Homebrew OpenOrbis: leitura MSAA e array

Use `main.cpp` num projeto do [OpenOrbis](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) com `libkernel`, `libSceGnmDriver` e o runtime C/C++ do template. Preserve os materiais de pacote do template. Esta fonte fica fora do build do emulador.

O caso preenche uma textura 8×8 RGBA8 UNORM com quatro samples, usando o layout
microtiled Thin1DThin e Morton explícito. Cada sample contém uma cor binária
distinta. Um shader GCN `IMAGE_LOAD` lê cada sample em `(3,5)` e grava quatro
componentes float32 por `BUFFER_STORE_DWORDX4`. O caso array usa duas camadas,
com cores complementares na segunda. Há também leitura do índice 7 para
comparar o comportamento de wrap para sample 3.

Os componentes esperados são `00000000` ou `3f800000`. Compare esses mesmos casos com o backend D3D12 antes de aceitar o lowering de MSAA/array como compatível com hardware. Se a fence demorar, o programa mantém os recursos submetidos vivos.

O caso cobre endereçamento microtiled, seleção de sample/camada e leitura GCN.
Não comprova rasterização, posições programáveis, resolve, HTILE/FMASK/CMASK,
EQAA, alpha-to-mask, performance, apresentação ou execução de jogos.
