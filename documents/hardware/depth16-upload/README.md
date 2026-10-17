# Referência de normalização de 16 bits

Compile `main.cpp` como homebrew OpenOrbis, ligando GnmDriver e libkernel.
O programa escreve valores de 16 bits em uma textura linear UNORM e executa
um shader GCN que lê o texel e grava seu resultado float em memória acessível
pela CPU. Compara os extremos e os valores próximos de meia escala com
`value / 65535`, tolerância de 0,000001.

Execute no PS4 e guarde as seis linhas `UNORM16` e `UNORM16_READ_RESULT`.
Esses resultados são a referência numérica para a conversão de profundidade
D16 quando o host usa outro formato. O programa não testa stencil, escrita
por depth test, cópias Vulkan ou a implementação D3D12 do host.

O SDK OpenOrbis não está configurado neste ambiente. Este homebrew não foi
compilado nem executado no PS4; não constitui evidência de aprovação no hardware.
