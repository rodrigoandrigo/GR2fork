# OpenOrbis: mistura de constantes RGB e alpha

Fonte GnmDriver/libkernel: 20 combinacoes de constantes RGB/alpha e inversos, com ADD/SUBTRACT/MIN/MAX/REVERSE_SUBTRACT. Compara o pixel central RGBA8 com a equacao CPU, tolerancia de um nivel UNORM. Alpha usa equacao separada. Validacao local somente sintatica; nao cobre MRT, MSAA, formatos float ou efeitos de storage.
