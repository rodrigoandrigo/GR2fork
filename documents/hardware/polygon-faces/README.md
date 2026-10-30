# OpenOrbis: modos polygon por face

Fonte para template OpenOrbis com GnmDriver/libkernel. Configura ES→GS→copy→PS,
com modos front/back point/fill, line/point e fill/line. O quarto caso desativa
as duas faces. Cada draw deve manter exatamente um incremento atômico no GS,
inclusive quando nenhuma face rasteriza. `FACE_CASE` registra hash da superfície
RGBA8 16×16, contador e resultado da submissão; `FACE_RESULT` compara os modos
de backing on-chip/off-chip. O caso com ambas as faces desativadas deve manter
a superfície zerada.

O fonte cobre seleção por face e efeitos anteriores à rasterização. Não cobre
bias, tessellation, ordem de efeitos do PS ou precisão completa de cobertura.
Nesta atualização foi conferida a sintaxe com headers stub; não foi compilado
um ELF OpenOrbis nem obtido resultado de execução desse fonte.
