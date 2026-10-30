# OpenOrbis: line stipple

Fonte GnmDriver/libkernel para registrar hashes e contador do GS em combinacoes de REPEAT_COUNT, PA_SU_LINE_STIPPLE_CNTL e seeds de PA_SU_LINE_STIPPLE_VALUE/PA_SC_LINE_STIPPLE_STATE. Usa wireframe com stipple. STIPPLE_SUBMISSIONS confere submissao e uma execucao do GS; os hashes permitem comparar configuracoes, mas nao sao um oraculo de fidelidade visual.

Validacao local somente sintatica. Ainda faltam um comparador de continuidade entre packets e referencias independentes de cobertura.

Contrato de REPEAT_COUNT (valor de registrador + 1): [CIK 3D Registers, AMD](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/programmer-references/CIK_3D_registers_v2.pdf).
