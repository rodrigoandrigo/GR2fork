# OpenOrbis: amostragem com storage compartilhado

Use `main.cpp` num template OpenOrbis, vinculando GnmDriver e libkernel.
O programa desenha uma superfície 16×16 e compara o hash de RGBA entre a
amostragem simples e a mesma amostragem com um buffer gravável sobre o backing
da imagem. A escrita fica no padding, preservando os texels usados na leitura.

Os casos cobrem LOD explícito/fracionário, gradientes 2D, LOD implícito no PS,
bias e clamps, min/mag, mip point/linear, reduction min/max e anisotropia
2×/4×/8×/16×, clamps de LOD fornecidos pelo shader e argumentos `_CD`
uniformes no quad. A textura usa Thin1DThin, pitch 16, quatro mips e cores por nível.
`SAMPLING` registra RGBA e hash para comparação com o renderer. Registre modelo,
firmware e saída completa do PS4. Este homebrew não substitui a validação de
precisão, fronteiras, arrays/3D ou todos os controles específicos de sampler GCN.

O SDK OpenOrbis não está instalado neste ambiente; a fonte não foi compilada
nem executada em PS4.

Referências de contratos: [ISA GCN da AMD](https://gpuopen.com/wp-content/uploads/2016/08/AMD_GCN3_Instruction_Set_Architecture_rev1.1.pdf)
e [SampleGrad com clamp mínimo de LOD](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/t3d-samplegrad-s-float-float-float-int-float-).
