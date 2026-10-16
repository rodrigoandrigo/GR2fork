# Tessellation e geometria no D3D12 nativo

## Implementacao

O rasterizador identifica LS/HS/VS e LS/HS/ES/GS. LS fornece os vertices de
controle; HS e TES sao traduzidos de GCN pelo recompilador compartilhado.
O HS e preparado primeiro para obter os strides e o numero de pontos de
controle. Em seguida, as duas etapas SPIR-V sao ligadas pelo compilador NIR
para que as assinaturas de controle e constantes de patch coincidam.
O runtime instala HS/DS/GS, recursos e constantes na PSO D3D12. Nenhum
dispositivo Vulkan e criado por esse caminho.

O compilador `spirv_to_dxil.dll` acompanha o pacote e e carregado apenas
quando necessario. O core verifica o tamanho da estrutura de configuracao
exportado pela DLL antes de chama-la. O DXIL gerado passa pelo validador
Microsoft. Recursos readonly declarados como storage buffers conservam
o binding UAV usado pelo runtime nativo; descriptor sets viram os register
spaces da etapa. Storage images conservam sua classe UAV.

O modulo Mesa aceita dominio, espacamento, orientacao, point mode e contagens
de controle provenientes do pipeline. Essas opcoes sao opt-in; as chamadas
existentes de Dozen continuam usando sua configuracao anterior. As chaves
de cache incluem os dois modulos SPIR-V e os parametros de tessellation.

Rect lists usam um GS gerado a partir da interface real do VS. A deteccao do
canto e a interpolacao do quarto vertice seguem a implementacao compartilhada
de rect lists. Atributos flat conservam o valor do vertice provocador; o GS
emite um strip de quatro vertices. O shader gerado e armazenado no cache.

Tambem foram corrigidos o execution mode de saida GS em pontos e a contagem
de vertices de entrada nos strips com adjacencia. Fans e line loops podem
ser convertidos antes do GS sem manter um tipo de entrada inexistente em D3D12.

## Evidencias de GPU no PC

- `Build/native-tess-peer-gpu.log`: hull/domain triangular, PSO e readback.
- `Build/native-quad-gpu.log`: dominio quad, fractional even e readback.
- `Build/native-combined-final-gpu.log`: tessellation seguida de GS.
- `Build/native-rect-gpu.log`: cobertura de rects nas seis ordens de entrada.
- `Build/native-point-nir-gpu.log`: GS de saida em pontos e readback.
- `Build/native-quad-fragment-gpu.log`: quad broadcast em GPU com faixa de
  32 a 64 lanes, readback correto e rejeicao de ballot dependente de wave64.

Os casos aprovados registram D3D12_ERRORS=0. Sao diagnosticos sinteticos;
nao sao uma execucao completa de jogos GCN, hardware PS4 ou validacao Xbox.

## Limites mantidos

Wave64 geral em graphics com largura variavel continua pendente. As operacoes
de quad em fragmento sao independentes da largura da wave; ballot/shuffle
entre 64 lanes nao sao. Duas waves de 32 lanes nao compartilham registradores
nem uma barreira que permita implementar diretamente essa comunicacao no
pipeline grafico atual. O atributo WaveSize de SM 6.6 e definido para compute:
https://microsoft.github.io/DirectX-Specs/d3d/HLSL_SM_6_6_WaveSize.html

Falta uma execucao alternativa que preserve o grupo guest de 64 lanes e o
contrato de rasterizacao/derivadas. Nao se remove a verificacao nem se apresenta
o suporte a quad como implementacao completa de wave64.

Permanecem rejeitados: partitioning Pow2, topologia de linhas de tessellation
fora de isolines, wireframe de rects/quads/polygon, polygon mode point,
geometry on-chip, stream output e conversoes adicionais de clip/depth no GS.
Interfaces e recursos complexos de shaders guest ainda precisam de cobertura
de runtime. O backend continua experimental e a selecao padrao permanece Dozen.

## Build e pacote

O build Release completo foi concluido em
`Build/native-tess-final-package-refresh.log`. O pacote inclui
`spirv_to_dxil.dll`; os sete binarios conferidos com os arquivos do build
registram `PAYLOAD_HASHES=PASS` em
`Build/native-tess-final-package-verification.log`.

A assinatura foi repetida apos liberar o arquivo do pacote e registra
`CMS_SIGNATURE=PASS` em `Build/native-tess-final-signature.log`, usando a
chave temporaria do projeto. O smoke test do backend nativo registra
`INIT=0`, `SMOKE_SHUTDOWN=0` e `PROCESS_EXIT_CODE=0` em
`Build/native-tess-final-smoke.log`. O pacote nao foi instalado nesta validacao.

A regressao desktop do caminho padrao Vulkan/Dozen, em
`Build/native-tess-dozen-regression-90s.log`, chegou ao menu de Pac-Man 256:
3585 frames apresentados, `D3D_INFO_QUEUE_MESSAGES=0`, `BOOT_RESULT=0`,
`SHUTDOWN=0` e `PROCESS_EXIT_CODE=0`. O readback
`Build/real-game-frame.png` foi inspecionado. A primeira janela de 45 segundos
terminou durante o carregamento, com dois frames; a janela maior confirmou
o progresso. Esse resultado valida o caminho padrao no probe desktop,
nao o gameplay nativo D3D12 nem o AppContainer/Xbox. A comparacao em hardware
PS4 com homebrew OpenOrbis continua pendente.
