# OpenOrbis: oclusao, predicacao e COPY_DATA

Fonte GnmDriver/libkernel: snapshots de ZPASS ao redor de um triangulo de 256 pixels, soma dos pares de contadores, predicacao normal/invertida e COPY_DATA imediato de 64/32 bits. A copia condicionada visivel deve ocorrer e a invertida deve permanecer zerada. Validacao local sintatica; nao cobre PRIMCOUNT, no-wait, EQAA ou distribuicao por render backend.
