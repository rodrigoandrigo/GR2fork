# OpenOrbis: particionamento e domínios de tessellation

Fonte para um template OpenOrbis com GnmDriver e libkernel. Configura LS→HS→DS→PS, grava fatores explícitos e compara Integer com Pow2 nos oito pares de domínio/topologia aceitos pelo caminho nativo. Um contador atômico no DS e o hash da superfície RGBA8 registram a equivalência sem depender de uma contagem fixa de invocações do driver. Inclui fatores fracionários deliberadamente, para detectar arredondamento adicional introduzido pelo emulador.

A fonte foi preparada sem execução. A checagem de sintaxe usa declarações substitutas do SDK, não produz um binário OpenOrbis. Não estabelece semântica GCN para pares fora do contrato D3D. Na [especificação, seção 11.7.15](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm), line é válido somente em isolines e triangle somente em tri/quad. No tessellator Microsoft/Mesa, `DumpAllPointsAsInOrderLineList` é uma visualização de depuração, não uma definição de conectividade guest; o gerador de isolines só distingue point de line.

O processamento de hardware de Pow2 segue Integer, preservando o pré-processamento feito pelo shader: [especificação Direct3D, 11.7.7.1](https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm#11.7.7.1%20Pow2%20Partitioning).
