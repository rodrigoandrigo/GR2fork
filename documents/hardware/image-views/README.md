# OpenOrbis: views de imagem com a mesma base

Fonte GnmDriver/libkernel: escreve RGBA8, le o mesmo backing como R32_UINT e grava o valor em outro texel, com wait entre operacoes. Compara todos os texels e padding com a referencia; detecta views incoerentes e escrita fora do destino. Validacao local somente sintatica; nao cobre storage MSAA ou todas as combinacoes de formato/layout.
