# OpenOrbis: frequência de shading e EQAA

Use `main.cpp` num template OpenOrbis, vinculando GnmDriver e libkernel.
A execução percorre cobertura 2×/4×/8×, fragments de 1 até a cobertura,
iterações de 1 até os fragments, `PA_SC_MODE_CNTL_1.PS_ITER_SAMPLE` ligado e
desligado e máscaras de cobertura. O PS incrementa um contador por invocação.
O log `EQAA` registra os valores medidos; não impõe uma associação presumida
entre samples e fragments. Não valida os valores de cor, depth/stencil,
resolve, posições de interpolação nem o conteúdo comprimido de FMASK.

Registre modelo do PS4, firmware e saída completa. O SDK OpenOrbis não está
instalado neste ambiente: esta fonte não foi compilada nem executada em PS4.
