# Diagnostico de remocao do dispositivo D3D12

O HRESULT decimal 2289696773 corresponde a DXGI_ERROR_DEVICE_REMOVED (0x887A0005).
O log do Xbox de 15/10/2026 termina durante a inicializacao do jogo, sem causa
D3D12 interna. Esse codigo nao permite concluir se houve timeout, comando invalido,
falha de acesso da GPU ou falta de memoria. A causa no console continua pendente.

A revisao habilita DRED antes da criacao dos dispositivos, quando a plataforma
oferece a interface. Falhas de sincronizacao e chamadas do frontend registram a
operacao, o HRESULT e GetDeviceRemovedReason. Quando disponiveis, os dados DRED
incluem endereco virtual da falha, alocacoes relacionadas e ate 32 listas com a
ultima operacao concluida e a proxima operacao. A indisponibilidade das interfaces
fica explicita; nao se exige a instalacao da camada de debug no Xbox.

A mensagem completa e gravada com flush em
LocalState/shadPS4/log/frontend-d3d12.log antes de atualizar a tela de erro.
Esse arquivo e separado do log assincrono do core para sobreviver a uma falha
do frontend sem misturar escritas concorrentes no mesmo arquivo. Filas e listas
do frontend recebem nomes para identifica-las nos breadcrumbs.

Validacao nativa em Build/device-removal-diagnostic.log: remocao forcada do device
por ID3D12Device5::RemoveDevice, captura do HRESULT, motivo interno e consultas DRED.
Esse diagnostico nao reproduz a causa da remocao do Xbox. A validacao normal do
frontend com debug layer/GBV e readback fica em Build/device-removed-frontend-final.log.
Build e assinatura: Build/device-removal-package.log.
Conteudo do pacote: Build/device-removal-package-verification.log.

Esta e uma revisao de diagnostico, nao uma confirmacao de que a causa da remocao
foi corrigida. E necessario reproduzir no console e recolher frontend-d3d12.log.


## Revisao apos o diagnostico do Xbox

O arquivo recebido identifica Frontend swapchain Present com HRESULT 0x887A0005
e motivo interno 0x887A0001 (DXGI_ERROR_INVALID_CALL). A interface DRED existe,
mas ambas as consultas retornam 0x887A0004 (DXGI_ERROR_UNSUPPORTED). Portanto nao
ha breadcrumb ou endereco de falha de GPU disponivel nesse log.

As swapchains de composicao do frontend e do apresentador D3D12 nativo usavam
DXGI_SWAP_EFFECT_FLIP_DISCARD. Foram alteradas para DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL,
conforme o contrato documentado para CreateSwapChainForComposition:
https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcomposition

O WSI Win32 do Dozen ja usa FLIP_SEQUENTIAL e nao foi alterado. Os intervalos de
Present e a fila FIFO dos frames guest tambem nao foram alterados.

A validacao nativa usa uma swapchain de composicao ligada a um visual DirectComposition
em janela oculta de diagnostico, com debug layer/GBV, 400 apresentacoes, 400 uploads
de capas e duas mudancas de resolucao. O resultado e verificado por readback;
nao comprova scanout fisico nem compatibilidade com o compositor do Xbox.
Log: Build/composition-sequential-probe.log.
Boot nativo: Build/composition-sequential-game.log.
Pacote e assinatura: Build/composition-sequential-package.log.
Conteudo: Build/composition-sequential-package-verification.log.

A violacao do contrato da swapchain foi corrigida. A confirmacao de que essa
alteracao resolve a remocao no Xbox permanece pendente de reproducao no console.


## Alvos de renderizacao ausentes e InfoQueue

O segundo log do console detectou GetCompletedValue com motivo 0x887A0001 e
confirmou novamente consultas DRED nao suportadas. A deteccao pela fence nao
identifica qual fila ou comando causou a remocao, nem prova uma falha da fence.

A validacao D3D12 do core no PC encontrou a mensagem 925: NumRenderTargets=1
com todos os formatos RTV desconhecidos. O Dozen passou a retirar somente
as entradas finais VK_FORMAT_UNDEFINED do PSO. Slots internos nao sao
renumerados, preservando as localizacoes dos outputs de fragmento. A vinculacao
OMSetRenderTargets tambem retira as entradas finais sem imageView, preservando
a lista original de attachments para clears e resolves. O identificador do
cache foi atualizado de 5 para 6 para invalidar estados anteriores.

O perfil UWP agora tenta habilitar a camada de debug D3D12 disponivel no factory
do Dozen; ausencia da interface nao impede a inicializacao. No frontend,
EnableDiagnostics e configurado uma unica vez antes de criar dispositivos,
evita reativar a camada em recargas e tenta habilita-la quando disponivel.
O relatorio persistente inclui agora disponibilidade da InfoQueue e ate 32
mensagens recentes, alem das consultas DRED existentes.

Evidencias locais:
- Build/invalid-call-real-debug-layer.log: boot com uma mensagem D3D12 (925).
- Build/unused-rtv-game-debug.log: boot com D3D_INFO_QUEUE_MESSAGES=0,
  BOOT_RESULT=0 e SHUTDOWN=0 apos a correcao.
- Build/invalid-call-frontend-composition.log: 400 frames em composicao,
  400 uploads, duas mudancas de resolucao, cache de 64 capas e readback correto.
- Build/invalid-call-infoqueue-removal.log: remocao forcada confirma que a
  coleta InfoQueue/DRED continua funcionando.
- Build/unused-rtv-package.log e Build/unused-rtv-package-verification.log:
  build, assinatura e hashes do pacote.

A configuracao inconsistente de render targets foi corrigida e o aviso desapareceu
no PC. Isso ainda nao comprova a causa da remocao especifica do Xbox; o teste no
console continua necessario. Em caso de repeticao, recolher tanto frontend-d3d12.log
quanto shad_log.txt da mesma execucao.


## Preservacao de diagnosticos longos

O arquivo recebido em 15/10/2026 tinha 554 bytes e terminava em `D3D12 id=1328 seve`,
antes de completar as seis mensagens da InfoQueue. Os avisos 1328 visiveis informam
que InitialState GENERIC_READ e ignorado para buffers criados em COMMON; isoladamente
nao identificam a causa da remocao.

CheckResult agora transporta seu diagnostico agregado como std::runtime_error UTF-8,
preservando o texto ate ReportFailure e o arquivo de log, em vez de depender do texto
restrito de winrt::hresult_error. O HRESULT e o motivo interno permanecem no texto.
Os consumidores do frontend e do core ja capturam std::exception. As mensagens de
corruption/error sao ordenadas antes das warnings. Os limites existentes de 32
mensagens recentes e 64 KiB por mensagem continuam explicitos no coletor.

Build/full-device-diagnostic.log registra a remocao forcada, duas mensagens
sinteticas longas e verificacao de ambos os marcadores finais e da prioridade do
erro sobre o aviso: LONG_DIAGNOSTIC_AND_ERROR_PRIORITY=PASS LENGTH=2060.
Build/full-device-diagnostic-frontend.log verifica a composicao normal com readback.
Build/full-device-diagnostic-package.log e Build/full-device-diagnostic-package-verification.log
registram build, assinatura e payload do pacote.

Esta revisao corrige a perda do diagnostico. Nao confirma a resolucao da remocao do
Xbox: e necessario recolher frontend-d3d12.log completo da nova execucao para ler
as mensagens que estavam ausentes.


## Correcao da view RGBA/BGRA

O log completo recebido identifica a primeira operacao ilegal: CreateShaderResourceView
com B8G8R8A8_UNORM_SRGB (0x5b) sobre R8G8B8A8_TYPELESS (0x1b). A mensagem 232
confirma a remocao por INVALID_CALL; a 646 registra um handle RTV invalido apos ela.
Os avisos 1328 sobre InitialState nao sao tratados como causa desta falha.

Em dzn_image_view_prepare_srv_desc, views UNORM/sRGB que cruzam as familias
RGBA8 e BGRA8 usam agora um formato SRV da familia real do recurso e remapeiam
os componentes de memoria 0 e 2. A conversao sRGB e mantida e o remapeamento e
composto com VkComponentMapping, preservando ZERO/ONE e swizzles personalizados.
A mudanca se aplica a SRVs; nao anuncia conversao arbitraria de RTVs, UAVs ou
casts inteiros nao representaveis no D3D12. O identificador de cache passa a 7.

A criacao de RTVs tambem verifica a alocacao da entrada e do heap antes de usar
heap/slot. Em caso de erro, a gravacao recebe o VkResult, nao cria uma view com
handle nao inicializado e os caminhos de clear/rendering interrompem a gravacao.
O RTV nulo recebe a mesma protecao.

Build/bgra-srv-strict-readback.log registra oito combinacoes em cada um de dois
modos: driver de producao normal e DLL privada de diagnostico com
RelaxedFormatCastingSupported desativado. A DLL privada religa os objetos reais
com um helper fora do pacote; confirma que as imagens foram criadas como
R8G8B8A8_TYPELESS ou B8G8R8A8_TYPELESS. O probe verifica na GPU as duas direcoes,
UNORM/sRGB e swizzles personalizados, comparando com valores de referencia
calculados independentemente, inclusive a curva sRGB. Nenhum helper de diagnostico
foi adicionado ao driver distribuido.

Build/bgra-srv-game-final.log registra boot nativo real com debug D3D12.
Build/bgra-srv-package.log e Build/bgra-srv-package-verification.log registram
build, assinatura e hashes do pacote final. A operacao ilegal registrada foi
corrigida; a confirmacao em hardware Xbox continua pendente.

## Imagem compactada de 16 bits e uso opcional de storage

O log seguinte falhou em B4G4R4A4UnormPack16, 640x539, usage 0x1f,
com apenas 1478262784 bytes usados pelo app. Nao era esgotamento dos 5 GiB.
Sem RelaxedFormatCastingSupported, o D3D12 retorna tamanho de recurso invalido
para essa imagem com ALLOW_UNORDERED_ACCESS. Sem storage ela aloca normalmente.

O core UWP agora acrescenta storage opcional a imagens de cor nao comprimidas
somente quando o formato suporta storage. A consulta EXTENDED_USAGE do Dozen
so consulta outra familia DXGI quando ha RelaxedFormatCastingSupported;
sem esse recurso, exige a mesma familia typeless. Assim nao anuncia UAV para
uma familia que o recurso D3D12 nao permite acessar dessa forma.

Build/packed-image-after.log verifica alocacao e bind da imagem com casting
restrito e rejeicao de storage incompatível pela consulta de formato.
Build/packed-image-package-final.log registra o pacote reconstruido.
A validacao nativa no PC nao substitui a confirmacao no Xbox.

## Detiler Thin1DThin de 8/16 bits

O log de 15/10/2026 registra falha de CreateComputePipelineState (0x80070057)
no detiler Thin1DThin_16. O shader usava arrays de uint8_t/uint16_t nos SSBOs.
Agora as variantes de 8/16 bits leem palavras de 32 bits e extraem o pixel;
as escritas usam atomicAnd/atomicOr com mascaras disjuntas, preservando pixels
vizinhos escritos por outras invocacoes. Os enderecos e o layout guest permanecem
iguais. Nao ha selecao por jogo ou shader guest.

Build/packed-detiler-capabilities.log confirma apenas Shader nos capabilities
SPIR-V, sem Int8/Int16 ou armazenamento estreito. Build/packed-detiler-readback.log
registra roundtrip na GPU para micro/macro 8,16,32,64,128 e micro 96 bits;
macro 96 verifica somente criacao de pipeline, pois o probe nao tem uma
configuracao macro valida para esse tamanho.

O frontend tambem registrou ErrorInitializationFailed na swapchain, sem HRESULT.
O WSI agora registra o HRESULT separadamente na criacao, QueryInterface e
anexacao UWP, liberando a interface temporaria mesmo quando QueryInterface falha.
Nao ha evidencia suficiente nesse log para afirmar a causa da falha da swapchain.

## BC1 com mipmaps no Xbox

O novo log chega ao carregamento de texturas BC1 de 32x32 com seis mipmaps.
A alocacao falhava com storage opcional apesar de a consulta de formato rejeitar
esse uso. A verificacao de storage no core UWP passa a cobrir imagens comprimidas,
preservando a politica desktop. No Dozen, os flags auxiliares de render target/UAV
para TRANSFER_DST sao escolhidos pelas capacidades do formato original, sem
EXTENDED_USAGE: capacidades de uma view de outra familia nao autorizam esses
flags no recurso comprimido nativo.

Build/bc-image-probe.log registra consulta, alocacao e bind da imagem exata do
log, tanto no driver normal quanto com RelaxedFormatCastingSupported desativado.
No modo restrito, storage e rejeitado pela consulta e a imagem sem esse uso
aloca 65536 bytes. Build/bc-image-package.log registra o pacote assinado.
