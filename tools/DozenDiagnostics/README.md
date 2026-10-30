# Dozen Diagnostics para Xbox UWP

Aplicativo independente do emulador e de jogos, com identidade de pacote própria.
Ao abrir, executa a bateria automaticamente e exibe uma mensagem ao terminar.
O relatório é gravado e descarregado para disco a cada linha:

`LocalState\dozen-diagnostics.log`

No Device Portal, selecione o armazenamento do aplicativo **Dozen Diagnostics**,
abra `LocalState` e baixe esse arquivo. Uma execução interrompida preserva o último
`CASE` iniciado. Uma nova abertura substitui o relatório anterior.

## Bateria de imagens

22 formatos normalizados, inteiros, float e packed. Para cada formato:

- Leitura de textura por SRV (`sample`).
- Leitura storage com formato declarado (`read`).
- Leitura storage sem formato (`unformatted`).
- Read-modify-write na mesma imagem (`modify`).
- Escrita UAV seguida de leitura SRV, com transição e dependência (`write_sample`).

São 110 combinações. Cada fase executada verifica 256 texels de quatro componentes,
incluindo os componentes implícitos de formatos R/RG/RGB. Há valores inteiros maiores
que um, negativos em formatos assinados e um padrão espacial na escrita. O buffer
de resultado começa com uma sentinela e é invalidado após a fence antes de ler.

`PASS` significa valores corretos devolvidos pela GPU. `FAIL` registra o primeiro
pixel/componente divergente. `ERROR` significa erro de API ou de infraestrutura.
`UNSUPPORTED` indica ausência do contrato necessário e **não** conta como aprovação.
A execução para em device loss ou timeout de GPU. O resumo `END` informa cada total;
`FATAL` ou ausência de `END` significa execução incompleta.

A aplicação consulta o suporte antes de executar. Não anuncia capacidades ausentes,
não força operações Vulkan inválidas e não implementa novos fallbacks no driver.
São usados shaders SPIR-V pré-compilados e o Dozen distribuído com o projeto.

Ainda não cobertos: filtros, mips, arrays, MSAA, compressão BC e emulações gerais de extensões opcionais. Esta bateria
não comprova a correção visual completa do renderer.

## Compilação

Na raiz do repositório:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-dozen-diagnostics.ps1
```

Requer o runtime Release já compilado em `shadPS4-UWP/Runtime/Release`, Visual Studio,
SDK Windows 10.0.26100.0, glslangValidator e Python. Os caminhos de Python e glslang
podem ser passados ao script. Ele cria e assina:

`Build/DozenDiagnostics/DozenDiagnostics_1.0.18.0_x64.msix`

O script não instala nem executa o pacote. O pacote usa o framework
`Microsoft.VCLibs.140.00`, como o emulador. A identidade separada permite manter
os dois aplicativos instalados sem compartilhar configurações ou dados.

O executável `DozenDiagnostics-PC.exe` reutiliza exatamente a bateria Vulkan.
Serve para conferir o próprio verificador; seus resultados não validam o Xbox.


## Leituras sem formato (schema 4)

O contrato D3D12 de TypedUAVLoadAdditionalFormats garante um grupo de 15 formatos
quando essa capacidade é verdadeira. A consulta individual FORMAT_SUPPORT é
necessária para os formatos opcionais; não deve esconder os integrantes do grupo
garantido. O Dozen agora combina as duas fontes de capacidade, exigindo também
suporte à criação da UAV do formato.

Fonte: https://learn.microsoft.com/en-us/windows/win32/direct3d12/typed-unordered-access-view-loads

Dos 19 casos sem suporte no relatório do Xbox, nove pertencem ao grupo garantido:
R8_UNORM, R8_UINT, R8_SINT, R16_SFLOAT, R8G8B8A8_UNORM, R8G8B8A8_UINT,
R8G8B8A8_SINT, R16G16B16A16_SFLOAT e R32G32B32A32_SFLOAT.

Os dez opcionais continuam dependendo de consulta positiva ou de uma futura
emulação: R8_SNORM, R8G8_UNORM, R8G8_SNORM, R16_UNORM, R16G16_SFLOAT,
R8G8B8A8_SNORM, R16G16B16A16_UNORM, R32G32_SFLOAT, A2B10G10R10_UNORM_PACK32
e B10G11R11_UFLOAT_PACK32. Não foi implementado um conversor geral de imagens.

O caso unformatted agora executa duas fases no mesmo recurso: leitura inicial e
leitura/escrita/releitura com padrão espacial, conferindo 2048 componentes. Também
verifica os componentes implícitos de formatos R/RG/RGB. A contagem permanece 182
casos; as fases não são contadas como aprovações independentes.

O driver e o diagnóstico compilaram; o pacote foi assinado e seus hashes foram
conferidos. No PC, os 182 casos passaram. Um controle negativo aplicado
somente à segunda fase foi detectado no pixel 256 e retornou falha. O Xbox confirmou os nove casos: 172 PASS, 0 FAIL, 10 sem suporte e 0 erros de API,
incluindo leitura/escrita/releitura sem formato. Os dez opcionais não foram habilitados.


## Conversão por aliases UAV inteiras (schema 5)

O pacote 1.0.4.0 mantém os 182 casos Vulkan e acrescenta quatro probes nativos
D3D12/SM6.4. Eles validam o candidato a fallback para R8_SNORM, RGBA8_SNORM,
R16_UNORM e RGBA16_UNORM: a mesma textura typeless recebe duas UAVs da mesma
família, uma inteira para leitura e outra normalizada para escrita. A conversão
faz extensão de sinal e clamp SNORM ou divisão UNORM. Não há snapshot, cópia entre
as vistas nem SRV em estado incompatível com UAV.

Cada probe inicializa uma textura 16x16, lê pela vista inteira e converte, escreve
pela vista normalizada e relê a alteração depois de uma barreira UAV. O verificador
compara 2048 componentes, incluindo valores intermediários e negativos. A seleção
D3D12 exige correspondência com o LUID do adaptador Vulkan. As consultas nativas
dos dez formatos opcionais também são registradas.

Os resultados são separados em NATIVE_ALIAS_PASS/FAIL/ERROR/UNSUPPORTED e
NATIVE_ALIAS_END. END inclui native_alias_ok; um probe que falha faz o executável
retornar erro. A execução aguarda fence antes de ler ou destruir recursos. Em
falha após submissão, os recursos em voo são retidos até o processo encerrar.

O schema 5 ainda não integra o fallback aos descritores e à tradução NIR do Dozen,
nem habilita os dez formatos na consulta Vulkan. A integração precisa tratar
arrays/cópias/atualizações de descritores e aliases de shader sem perder coerência.
O resultado da execução Xbox deste pacote deve confirmar a representação antes
de usá-la como fallback de produção. Os seis demais formatos seguem pendentes.


Validação local do schema 5: 182 PASS Vulkan e 4 PASS NATIVE_ALIAS no PC.
Um controle negativo, alterando somente a saída da segunda leitura do alias
R8_SNORM em uma pasta separada, falhou no pixel 256 e retornou código 1. O pacote
normal preserva os shaders aprovados. Isso verifica o diagnóstico; não comprova
execução nem suporte dos fallbacks no Xbox.


## Fallback integrado no Dozen (schema 6)

O Xbox confirmou os quatro probes nativos do schema 5: NATIVE_ALIAS_END
passed=4 failed=0 unsupported=0. A bateria Vulkan permaneceu em 172 PASS,
0 FAIL e 10 sem suporte. Esse resultado valida a representação, não a integração.

O pacote 1.0.5.0 integra ao driver a leitura sem formato por uma UAV inteira
auxiliar para R8_SNORM, RGBA8_SNORM, R16_UNORM e RGBA16_UNORM. O lowering NIR
seleciona a conversão pelo metadata do descritor; escritas continuam na UAV
normalizada do mesmo recurso. O caminho exige TypedUAVLoadAdditionalFormats,
UAV inteira da família garantida e suporte à escrita normalizada. Não promove
StorageReadWithoutFormat para texel buffers nem para os seis formatos restantes.
O fallback fica no caminho de tabelas de descritores; dispositivos com suporte
bindless conservam a consulta nativa de capacidades.

A tabela auxiliar é incluída na assinatura raiz, alocação de pools, cópia de
descritores, snapshots/atualizações em voo e descritores por mip. Recursos storage
das famílias envolvidas usam representação typeless. O cache do driver passou a
22. O número de descritores auxiliares entra nos limites e na consulta de suporte
a layouts; somente dispositivos que precisam do fallback reservam a nova tabela.

O schema 6 acrescenta quatro casos Vulkan com arrays de duas vistas do mesmo
recurso, descritores copiados, valores fracionários, leitura pelo alias 1, escrita
pelo alias 0 e releitura. São 186 casos Vulkan e quatro probes nativos separados.
O verificador aplica a quantização SNORM/UNORM esperada nas leituras fracionárias.

A build UWP ignora variáveis de ambiente: as execuções PC anteriores não
comprovavam o limite SM6.4 apenas por definir DZN_MAX_SHADER_MODEL. Agora o
diagnóstico aplica explicitamente mesa_uwp_set_shader_model_limit(0x64) antes de
criar a instância. A aplicação normal não chama essa API e mantém o limite nativo.
O log registra o limite pedido e o modelo efetivo, o fallback ativo e o binding tier.

Validação PC do pacote atual: shader_model=0x64, integer_image_loads=1,
186 PASS Vulkan, 0 FAIL, 0 sem suporte, 0 erros de API e quatro probes nativos
aprovados. O controle negativo alterando somente a releitura do array R8_SNORM
foi detectado no pixel 256 e retornou código 1. A integração ainda aguarda execução
no Xbox; nela, o alvo desta etapa é recuperar quatro dos dez casos, mantendo seis
sem suporte. Não há confirmação de correção visual geral de jogos.


## Ordenação entre dispatches em UAV (schema 7)

O Xbox executou o schema 6 com 177 PASS, 3 FAIL, 6 sem suporte e 0 erros de API.
Os quatro formatos recuperados e os quatro casos de arrays/descritores copiados
passaram. R8_UNORM, R16_SFLOAT e RGBA32_SFLOAT falharam na leitura inicial sem
formato: pixel 128, componente 0, esperado 1, recebido 0. Os probes nativos
continuaram em quatro aprovações.

Foi identificada uma dependência ausente no caminho de barreiras legadas: uma
barreira Vulkan de imagem com os estados D3D12 antes/depois iguais a UAV perdia
seu efeito ao descartar a transição sem mudança de estado. Isso permitia
sobreposição entre leitura e escrita de dispatches. O driver agora emite uma
barreira UAV para o recurso indicado nessas dependências explícitas, sem espera
CPU ou barreira global. O resultado Xbox é compatível com esse problema; a
confirmação da correção no console exige nova execução.

O pacote 1.0.6.0 mantém 186 casos Vulkan e quatro probes nativos. O diagnóstico
agora também força explicitamente as barreiras legadas antes da criação da
instância, para exercitar no PC esse caminho. O log informa enhanced_barriers=0.
A aplicação normal não chama essa configuração e continua selecionando o caminho
suportado pelo dispositivo. Os seis formatos sem fallback continuam pendentes.


Validação local do schema 7: shader_model=0x64, integer_image_loads=1 e
enhanced_barriers=0 confirmados pelo driver; 186 PASS Vulkan, 0 FAIL, 0 erros de
API e quatro probes nativos aprovados. Pacote compilado, assinado e conteúdo
conferido contra o driver recém-compilado. Ainda sem confirmação Xbox do schema 7.


## Leitura float por UAV inteira (schema 8)

O Xbox executou o schema 7 com 176 PASS, 4 FAIL e 6 sem suporte. A barreira UAV
adicionada não eliminou as divergências. R8_UNORM, R16_SFLOAT, R32_SFLOAT e
RGBA32_SFLOAT falharam na fase inicial sem formato; os quatro fallbacks e seus
casos de arrays/cópia continuaram aprovados. A hipótese de sobreposição entre
dispatches não foi suficiente para explicar/corrigir esse resultado.

O pacote 1.0.7.0 amplia a representação inteira auxiliar para R8/RGBA8_UNORM,
R16/RGBA16_SFLOAT e R32/RGBA32_SFLOAT. As famílias inteiras correspondentes estão
no grupo D3D12 garantido. O metadata seleciona divisão UNORM8/16, extensão/clamp
SNORM8, decodificação float16 ou reinterpretação dos bits float32. As escritas
continuam na vista original do mesmo recurso. O uso dessa representação é geral
por formato/capacidade; não depende de título nem de shader específico. Os seis
formatos opcionais restantes não foram habilitados. Cache do driver: versão 23.

A bateria passa a 192 casos Vulkan, com seis casos adicionais de arrays/copias e
valores fracionários para as representações ampliadas. Float16/32 incluem leitura
negativa exata (-0.5), escrita pelo alias 0 e releitura pelo alias 1. Os quatro
probes nativos permanecem separados.

No PC, o driver confirmou shader_model=0x64, integer_image_loads=1 e
enhanced_barriers=0: 192 PASS Vulkan e quatro probes nativos aprovados, sem erros
de API. O pacote foi compilado e assinado. O Xbox confirmou o schema 8: 186 PASS Vulkan, 0 FAIL, 6 sem suporte,
0 erros de API e quatro probes nativos aprovados. As quatro divergências
anteriores foram eliminadas nessa bateria. Permanecem sem suporte as leituras
sem formato de R8G8_UNORM, R8G8_SNORM, R16G16_SFLOAT, R32G32_SFLOAT,
A2B10G10R10_UNORM_PACK32 e B10G11R11_UFLOAT_PACK32.


## Ponte de conversão raw para os seis formatos restantes (schema 9)

O pacote 1.0.8.0 preserva o driver validado no schema 8 e acrescenta seis probes
nativos D3D12/SM6.4: RG8_UNORM, RG8_SNORM, RG16_SFLOAT, RG32_SFLOAT,
RGB10A2_UNORM e R11G11B10_FLOAT. O suporte Vulkan anunciado permanece inalterado.
A ausência de vistas inteiras compatíveis garantidas impede estender diretamente
o fallback anterior a esses seis formatos no Xbox.

Cada probe executa na GPU: upload da textura, cópia para buffer com footprint
nativo, leitura/decodificação raw, permutação e codificação dos canais, releitura,
cópia de retorno à textura e readback dos bits. Compara 2048 componentes e os bits
de 256 texels por formato. As escritas RG8 usam compare-exchange para preservar
o texel vizinho na mesma palavra de 32 bits. Há casos SNORM -128, frações,
float16/32 negativos, zero com sinal, subnormais, infinito e NaN. R11G11B10 usa
arredondamento direto para a precisão destino, sem dupla conversão via float16.
Payloads NaN de float16/32 podem variar; as demais diferenças de bits são falhas.

Os resultados ficam separados em NATIVE_RAW_PASS/FAIL/ERROR e NATIVE_RAW_END;
END inclui native_raw_ok. Falha retorna código 1. Recursos em voo são retidos
até encerrar o processo se a espera de fence falhar. O adaptador é selecionado
pelo mesmo LUID usado pelo Dozen. Não são executadas leituras de textura sem
suporte D3D12: as leituras/escritas do shader usam buffer raw.

Validação PC: 192 PASS Vulkan, quatro probes de aliases e seis probes raw
aprovados, com SM6.4 e barreiras legadas explícitos. Um controle negativo alterou
somente os bits depois da releitura de RGB10A2, mantendo os valores retornados
pelo shader corretos. A verificação da cópia de retorno à textura detectou a
corrupção no pixel 0 e retornou código 1. Pacote compilado e assinado; shaders
e executáveis conferidos contra o conteúdo empacotado.

Essa ponte ainda é um protótipo no diagnóstico, não um fallback Vulkan integrado.
Continuam pendentes a representação persistente e o tracking de autoridade no
Dozen, o lowering de todos os acessos tipados/sem formato, aliases entre bindings
e estágios, a integração com amostragem/attachments e a sincronização de
transferências, mips, camadas e views. Copiar antes/depois de um dispatch,
isoladamente, não resolve leitura de textura e escrita storage compartilhadas
dentro do mesmo draw. O Xbox confirmou o schema 9: 186 PASS Vulkan, 0 FAIL, 6 sem suporte e
0 erros de API, com quatro probes de aliases e seis probes raw aprovados.
As quatro fases dos seis probes raw passaram, incluindo a conferência dos bits
de retorno à textura. Isso valida a ponte de conversão dessa bateria; os seis
casos Vulkan permanecem sem suporte até a integração coerente ao driver.


## Representação persistente no driver (schema 10)

O pacote 1.0.9.0 integra uma ponte explícita à imagem Vulkan do Dozen. O driver
aloca sob demanda um VkBuffer/VkDeviceMemory local ao dispositivo, mantém seus
footprints por subrecurso e destrói o backing auxiliar junto da imagem. A alocação
usa a infraestrutura normal de memória do driver. O diagnóstico recebe um handle
emprestado; não deve destruí-lo nem liberar sua memória separadamente.

O estado de autoridade é registrado por mip/camada em cada command buffer:
textura, buffer raw ou ambos sincronizados. Uma preparação copia apenas os
subrecursos cuja textura é autoritativa; releituras limpas reutilizam o buffer.
Escritas raw declaradas são materializadas antes das dependências de imagem e
memória global. Escritas pendentes também são materializadas em EndCommandBuffer.
O buffer volta a COMMON no fim da lista e o tracking de gravação é descartado no
reset/destruição do command buffer. Isso evita tratar gravação de comandos como
se fosse conclusão da GPU. Entre listas a textura permanece a representação
canônica; o backing raw persiste, com atualização conservadora no primeiro uso.

A interface interna está em dzn_raw_image.h:
- mesa_uwp_get_raw_image devolve o buffer, offset, row pitch e contadores.
- mesa_uwp_cmd_prepare_raw_image prepara acesso raw e declara se haverá escrita.

Essa interface exige imagem já vinculada, optimal, color, sem MSAA/sparse/protected,
com uso storage, um dos seis formatos e command buffer primário fora de rendering.
Os subrecursos preparados precisam estar em GENERAL. O acesso à imagem/backing
deve ser exclusivo e corretamente sincronizado pelo chamador; a interface não
adiciona sincronização entre filas nem resolve aliases físicos entre imagens.
O handle emprestado só é válido durante a vida da imagem; a aplicação deve concluir
o uso GPU antes de destruí-la, como exige o ciclo de vida normal de recursos Vulkan.

O novo grupo PERSISTENT_RAW usa shaders Vulkan/GLSL e essa implementação do driver,
não recursos D3D12 externos ao Dozen. Há doze casos: seis formatos no subrecurso
base e seis em mip 1/camada 1. Cada caso faz duas atualizações com dados diferentes,
usa o mesmo buffer em três submissões, confere leitura/escrita/releitura e
amostragem da textura após materialização. Uma escrita é deixada pendente até o
fim da command list e verificada por amostragem na submissão seguinte.
Os contadores verificam duas atualizações, duas materializações e duas reutilizações
limpas no caso base; com quatro subrecursos são oito operações de cada tipo.
São conferidos 6144 componentes por caso base e 1536 no mip selecionado.

O PC passou os doze casos, além dos 192 casos Vulkan anteriores, quatro probes
nativos de aliases e seis probes raw. Um controle negativo, em executável separado,
omitindo a declaração de escrita raw, foi detectado nos doze casos e retornou erro.
O Xbox confirmou o schema 10: PERSISTENT_RAW_END passed=12 failed=0,
186 PASS Vulkan, 0 FAIL, 6 sem suporte, 0 erros de API, quatro probes de aliases
e seis probes raw aprovados. Todos os casos mantiveram o mesmo buffer em três
submissões. Os contadores foram 2/2/2 no caso base e 8/8/8 em mip 1/camada 1,
conforme esperado. Isso valida a ponte explícita dessa bateria no console.

A integração é parcial e explícita. O lowering automático de operações image/texture
para esse backing, todos os aliases entre bindings/estágios e a mistura simultânea
com attachments ainda não foram implementados. Os seis formatos continuam sem
StorageReadWithoutFormat anunciado no Xbox. Aprovar PERSISTENT_RAW não comprova
suporte Vulkan geral aos seis formatos nem correção visual de jogos.


## Backing canônico para aliases físicos compatíveis (schema 11)

O pacote 1.0.10.0 fecha um pré-requisito da coerência entre imagens distintas:
backings raw são compartilhados por imagens com VK_IMAGE_CREATE_ALIAS_BIT,
vinculadas ao mesmo VkDeviceMemory/offset, com descrições nativas equivalentes
(dimensões, mips, samples, layout, flags, alinhamento e família typeless).
Offsets distintos, sobreposição parcial e layouts incompatíveis não são unificados.

O backing tem contagem de referências independente da imagem que o criou. O
registro é protegido pelo lock recursivo do dispositivo; memória, footprints e
buffer só são liberados quando o último alias é destruído. Autoridade e estado
raw são identificados pelo backing compartilhado, e cada subrecurso conserva o
alias nativo e o layout usados na materialização. Ler pelo segundo alias não
recopia uma textura antiga sobre escritas raw pendentes do primeiro.

A bateria PERSISTENT_RAW passa a 18 casos. Os seis adicionais vinculam duas
imagens à mesma memória, verificam o mesmo buffer raw, escrevem por uma e leem
pela outra antes da materialização. Depois da amostragem de conferência, destroem
a imagem que criou o backing e realizam outra submissão lendo pelo alias
sobrevivente. São quatro submissões no caso físico. Os seis casos passaram no PC;
os doze casos anteriores e as baterias Vulkan/nativas também passaram.

Esta etapa não implementa o lowering automático. Continuam pendentes a seleção
dos descritores efetivamente acessados, o desvio de todos os acessos image/texture,
a coerência com amostragem/attachments no mesmo comando e aliases de layouts
incompatíveis. Os seis formatos continuam sem StorageReadWithoutFormat no Xbox.
O Xbox aprovou o schema 11: 18 casos persistentes, quatro aliases nativos,
seis conversões raw, 186 PASS Vulkan, zero falhas/erros de API e seis casos
sem suporte. Aprovação da ponte explícita não equivale à habilitação geral dos formatos.

## Consumidores nativos da ponte (schema 12)

O pacote 1.0.11.0 trata acessos nativos a subrecursos que já participam da ponte.
Antes de cópias, blits, resolves, clears e uso como color attachment, materializa
escritas raw pendentes. Escritas nativas invalidam a cópia raw correspondente,
preservando os demais mips/layers. O início de rendering considera layerCount e
viewMask; não varre descritores potencialmente inativos. Não acrescenta waits CPU.

Seis casos native_consumers verificam clear em GENERAL, upload em GENERAL e
clear de attachment, seguidos por leitura raw e amostragem nativa. Incluem cache
raw anterior à escrita nativa e escrita raw pendente antes do clear. Cada caso
realiza seis submissões e espera oito cópias textura->raw, três raw->textura e
duas reutilizações limpas. No PC passaram os 24 casos da ponte e a bateria
anterior com SM6.4 e barreiras legadas explícitas. Como controle negativo,
o executável novo com o driver do pacote 1.0.9.0 falhou nos seis novos casos
logo após o clear, enquanto os 18 casos anteriores passaram. A revisão final
corrigida passou os 24 casos, com 192 PASS Vulkan, zero falhas/erros de API.
O Xbox confirmou o schema 12: 24 casos persistentes, 186 PASS Vulkan, zero
falhas/erros de API e os mesmos seis casos sem suporte.

A ponte continua explícita. Este pacote não implementa lowering automático NIR,
amostragem raw no mesmo comando que escreve storage, merge simultâneo de
attachment/storage ou coerência entre layouts físicos incompatíveis. Os seis
formatos continuam desabilitados para StorageReadWithoutFormat no Xbox.


## Lowering automático experimental (schema 13)

O pacote 1.0.12.0 ativa `mesa_uwp_set_raw_image_lowering(1)` apenas no diagnóstico,
antes da instância Vulkan. O padrão do emulador continua desativado. O perfil
força SM6.4 e barreiras legadas e anuncia leitura sem formato para os seis formatos
em tiling optimal; não promove storage texel buffers nem linear tiling.

O compilador NIR desvia imageLoad/imageStore, com ou sem formato explícito, para
um UAV raw compartilhado pelos aliases compatíveis. Metadados por descritor
informam formato, offset, pitches e dimensões do mip/layer da view. Os conversores
cobrem RG8 UNORM/SNORM, RG16F, RG32F, RGB10A2 UNORM e R11G11B10F. Stores RG8 usam
CAS de 32 bits para preservar o texel vizinho. R11G11B10F usa arredondamento direto
para o formato final. Barreiras de imagem também abrangem os acessos SSBO gerados.

A preparação do draw/dispatch usa os bindings de recursos do pipeline; não chama
APIs explícitas de preparação no aplicativo. A autoridade raw é materializada nas
barreiras/consumidores nativos existentes e no fim da lista. Descritores copiados,
mips, layers e o cache serializado de pipelines conservam seus metadados. O ABI
de cache foi atualizado para a versão 25 e distingue o perfil experimental.

No PC passaram 198 casos Vulkan (incluindo os seis casos sem formato e seis
arrays com aliases copiados), os 24 casos da ponte e 18 casos automáticos com
valores fracionários/especiais, mip1/layer1, clear/upload/attachment, amostragem
posterior e conferência de bytes nativos. Os 18 pipelines foram recriados de
cache serializado com cache hit.
`AUTOMATIC_RAW` exige cópias reais e registra `explicit_prepare=0`; não basta a
carga tipada nativa funcionar no PC. O executável desktop também passou com
`--native`, que desativa o lowering e mantém o perfil normal.

O Xbox confirmou o schema 13: 198 PASS Vulkan, 24 casos persistentes e 18
casos automáticos, zero falhas, sem suporte ou erros de API. Isso valida o perfil
experimental dessa revisão, sem habilitar o emulador.

## Storage, sampler e attachment simultâneos (schema 14)

O pacote 1.0.13.0 mantém SM6.4, barreiras legadas e ativação experimental somente
no diagnóstico. O sampler 2D/2D-array pode ler o mesmo backing raw usado por
imageLoad/imageStore no mesmo draw/dispatch. Descritores combinados, separados e
samplers imutáveis conservam os metadados de formato, swizzle, mip, layer, filtro,
endereçamento, LOD e borda. O ABI de cache passa a 28. Leituras por sampler não
são classificadas como escritas.

O lowering inclui texelFetch, consultas de tamanho/níveis/LOD, amostragem com LOD
explícito, gradientes e LOD implícito em fragment shader, gather, seleção min/mag,
filtragem entre mips, redução min/max e anisotropia. A implementação segue as
[regras de amostragem Vulkan](https://docs.vulkan.org/spec/latest/chapters/textures.html).
As verificações deste pacote cobrem texelFetch e textureLod em centros de texels;
não constituem validação exaustiva desses filtros e modos.

Cada store marca na GPU os segmentos de 16 bits alterados, inclusive stores que
mantêm o valor anterior. Depois do draw, um compute interno combina as escritas
raw com o attachment nativo, preserva os segmentos não escritos e limpa a máscara.
A fusão não repete shaders do aplicativo, não compara pixels na CPU e usa buffers
internos com o ciclo de vida da command list. Cópias e compute auxiliares respeitam
as dependências GPU e ficam fora da predicação e das consultas do aplicativo.

A bateria SIMULTANEOUS tem cinco variantes por formato: storage/attachment,
sampler/attachment, os três juntos, sampler separado e sampler imutável.
Cada variante executa dois draws consecutivos, confere os bytes finais de todos
os texels e exige exatamente 224 efeitos laterais de fragment shader. Os stores
atingem regiões distintas da escrita do attachment, com fronteira entre texels
RG8 no mesmo DWORD; não depende de corridas Vulkan indefinidas sobre o mesmo texel.
Os 18 casos AUTOMATIC_IMAGE também conferem texelFetch e textureLod imediatamente
após imageStore e memoryBarrierImage no mesmo dispatch, antes de qualquer cópia.

Ainda exigem implementação/validação adicional: amostragem raw 1D/3D/cube,
views especiais de 3D, input attachments, endereços raw acima de 4 GiB, aliases
parciais ou de layouts incompatíveis, command buffers secundários e leituras
concorrentes em filas distintas. A ponte explícita não marca stores SSBO externos
na máscara e mantém seu contrato sequencial. O perfil de produção permanece
desativado. O Xbox confirmou o schema 14 final: 30/30 casos simultâneos, 224 efeitos
laterais por caso e 228 PASS Vulkan, sem falhas ou erros de API.

Validação no PC da revisão final: 228 PASS Vulkan, 30/30 SIMULTANEOUS,
24/24 PERSISTENT_RAW, 18/18 AUTOMATIC_IMAGE, quatro aliases nativos e seis
conversões raw; zero falhas, casos sem suporte ou erros de API. O controle
`--native` também passou (198 PASS Vulkan e 24/24 persistentes). Os 11 SPIR-V
novos/alterados passaram no spirv-val para Vulkan 1.2. O pacote foi assinado com
CMS_SIGNATURE=PASS; DLLs e executável dentro do MSIX foram comparados com o
payload validado. SHA-256 do pacote:
`576880ae352c8325250e5431769c21c70c218b72fb7d82e0b981f833696281cd`.

## Dimensões e aliases incompatíveis (schema 15)

O pacote 1.0.14.0 amplia o sampler raw para 1D, 1D-array, 3D, cube e cube-array,
com seleção de mip/min/mag, coordenadas e offsets dimensionais, interpolação
espacial e entre mips, gradientes, consultas de dimensões/níveis/LOD e gather de
cube. Cubes usam projeção por face, derivadas da projeção e remapeamento das
amostras nas costuras; o canto combina os três texels incidentes. O código de
conversão é compartilhado pelos seis formatos. O ABI de cache passa a 29.

Views storage/sampler dos formatos raw suportados usam os mesmos bytes mesmo
quando a família DXGI ou o cast de tipos não é compatível com o recurso nativo.
Os descritores nativos desse caminho são nulos, evitando criar views D3D12
inválidas; os metadados e o UAV raw continuam apontando para o backing real.
A materialização conserva os bits no formato original da imagem.

Imagens físicas incompatíveis não compartilham o backing canônico. A memória
vinculada registra seu range real; antes de ativar/escrever um alias sobreposto,
o driver conclui as materializações pendentes e invalida as representações raw
que perderam validade. A ativação por UNDEFINED inclui a barreira de aliasing
nas listas legadas. Isso cobre sobreposição total e parcial sem acrescentar
waits CPU. Não se exige preservação de conteúdo entre imagens incompatíveis:
o aplicativo deve reativar e inicializar o recurso conforme o
[contrato Vulkan de aliases](https://docs.vulkan.org/spec/latest/chapters/resources.html#resources-memory-aliasing).

O diagnóstico acrescenta 30 casos dimensionais (cinco tipos por formato),
12 casos de reativação de aliases incompatíveis (ranges iguais e parcialmente
sobrepostos) e cinco pares de views mutáveis storage/sampler. Os casos verificam
os valores devolvidos pela GPU, mips, array/cube index, texelFetch, interpolação
1D/3D, gradientes com LOD não zero, gather e bordas/cantos de cube. Os aliases
são alternados duas vezes no mesmo command buffer e o resultado nativo final é
conferido após a fence. O relatório separa DIMENSION_END (30),
INCOMPATIBLE_ALIAS_END (12) e INCOMPATIBLE_VIEW_END (5).

Continuam fora desta validação as views especiais/slices de 3D, input attachments,
endereços raw acima de 4 GiB, formatos externos aos codecs raw e execução em
command buffers secundários ou filas concorrentes. O perfil de produção permanece
desativado. A aprovação do schema 14 no Xbox não valida automaticamente o schema 15.

Na revisão final passaram no PC os 30 casos dimensionais, 12 aliases físicos
incompatíveis e cinco pares de views mutáveis: 275 PASS Vulkan, zero falhas,
casos sem suporte ou erros de API, além de 24/24 persistentes e 18/18 automáticos.
O perfil de controle --native passou com 198 PASS Vulkan e 24/24 persistentes.
Os 30 novos SPIR-V passaram no spirv-val para Vulkan 1.2. O MSIX foi assinado
(CMS_SIGNATURE=PASS) e seus executável, DLLs e shaders foram comparados com o
payload validado. SHA-256:
`f72712cf9625e135a286d10e3e784457b4494b8d86868acad56ec1d4c264e7ea`.
O Xbox confirmou o schema 15: 30/30 casos dimensionais, 12/12 aliases incompatíveis,
5/5 pares de views e 275 PASS Vulkan, sem falhas, casos sem suporte ou erros de API.

## Perfil de produção (schema 16)

O lowering raw agora é habilitado por padrão na DLL do Dozen. Os critérios de
capacidade continuam valendo: cargas inteiras disponíveis e caminho sem suporte
a bindless. A escolha efetiva ocorre antes da inicialização dos shaders meta,
que só são criados nos dispositivos que usarão esse caminho. Dispositivos fora
desses critérios conservam suas capacidades e representações nativas.

O diagnóstico 1.0.15.0 consulta mesa_uwp_get_raw_image_lowering antes da instância
e exige production_default=1. A execução normal não chama a API de ativação:
registra explicit_raw_override=0 e exercita o padrão distribuído no emulador.
--native continua sendo um controle explícito de desativação. O perfil de
medição do diagnóstico mantém SM6.4 e barreiras legadas.

A ativação não equivale a validação visual de jogos. Os limites de cobertura
descritos no schema 15 continuam aplicáveis.

Validação do padrão de produção no PC: production_default=1,
explicit_raw_override=0, 275 PASS Vulkan, zero falhas, casos sem suporte ou erros
de API. O controle --native passou com 198 PASS e explicit_raw_override=1.
O emulador Release e o diagnóstico foram compilados e assinados
(CMS_SIGNATURE=PASS); a DLL do driver dentro dos dois pacotes coincide com o build.
Os executáveis do pacote do emulador também foram comparados com seus outputs.
SHA-256 do bundle Release:
`8e5dc80f1bccb8039523ea1883791ad9dc81b64015b28922d29ff4aae6293140`.
SHA-256 do diagnóstico 1.0.15.0:
`70d94e027c091959b73c8639ed588d73802b85eea877160655a0357c694e939e`.
Nenhum pacote foi instalado. O schema 16 permite conferir o padrão no Xbox.

## Ativação de imagens dedicadas (schema 17)

A inicialização do emulador com o perfil de produção expôs uma barreira de aliasing
emitida para recursos committed. A reprodução no host UWP no PC, com SM6.4 e
validação D3D12, registrou ResourceBarrier, mensagem 528: committed resource não
pode usar barreira de aliasing. Close retornava 0x80070057 e Vulkan ErrorUnknown.
O controle com raw desativado inicializava normalmente.

A ativação agora exige memória com heap e ausência de dedicated_res antes de
emitir a barreira legada de aliasing. A invalidação das representações raw e as
barreiras de layout normais permanecem. O padrão de produção continua ativo.
O mesmo host corrigido passou inicialização, oito presents, resize e shutdown
com a validação D3D12 habilitada. Não houve instalação de pacote nesse processo.

O diagnóstico 1.0.16.0 acrescenta DEDICATED_IMAGE: seis imagens 3D com alocação
explicitamente dedicada, storage/sampler raw, mips, filtragem e conferência de
valores. A bateria de aliases em heaps continua sendo executada. O schema 17
mantém production_default=1 e explicit_raw_override=0.

A revisão corrigida passou no PC: DEDICATED_IMAGE_END 6/0 e END 281/0,
unsupported=0, api_errors=0, production_default=1 e explicit_raw_override=0.
O controle --native passou com 198/0. Os pacotes do emulador e diagnóstico foram
assinados (CMS_SIGNATURE=PASS); seis DLLs e o EXE do bundle do emulador foram
comparados com o runtime, e ambos os pacotes contêm a mesma DLL corrigida.
SHA-256 do bundle:
`1e69f2823046657550b6483df6e8af9234276884aa8fd2944fd02ad4bdcc38ff`.
SHA-256 do diagnóstico 1.0.16.0:
`8bbf77b17127d66cf70d53078c196246900ef464ed4b77ca6306664b29bbb4a1`.
A execução corrigida no Xbox ainda depende do novo pacote; a reprodução e a
validação descritas acima foram feitas no PC.

## Onze leituras storage sem formato (schema 18)

O pacote 1.0.17.0 acrescenta codecs raw para R16_SNORM, R8G8_UINT/SINT,
R16G16_UNORM/SNORM/UINT/SINT, A2B10G10R10_UINT_PACK32, R32G32_UINT/SINT e
R16G16B16A16_SNORM. O backing, as transições, a máscara de escrita e a coerência
com consumidores nativos continuam compartilhados com o caminho anterior.
A ativação de produção permanece habilitada.

Os metadados passam a informar explicitamente o tamanho do texel (2, 4 ou 8
bytes); o ABI de cache passa a 30. Loads/stores escolhem os codecs de acordo com
a classe numérica do shader. Inteiros mantêm os bits e o sinal, normalizados
mantêm sua conversão numérica, e stores de 16 bits usam CAS para preservar o
texel adjacente. TexelFetch inteiro conserva componentes implícitos inteiros.
O suporte adicional anunciado é para storage images em tiling optimal; linear
tiling e storage texel buffers continuam dependendo de suas capacidades nativas.

A bateria básica passa de 22 para 33 formatos. Cada formato novo verifica SRV,
leitura tipada, leitura sem formato com read-modify-write, escrita/releitura e
storage seguido de texelFetch. Os onze também verificam arrays de descritores
copiados com aliases. As cargas sem formato dos sete formatos inteiros são
comparadas no shader contra padrões exatos; incluem 0x80000011 nos formatos de
32 bits, antes da conversão do resultado para float. Não basta a comparação
float aceitar valores que perderam bits baixos.

Uma verificação adicional de textureLod inteiro encontrou a limitação de
sampleLevel inteiro no backend DXIL/SM6.4, também presente no controle --native.
Ela não é anunciada como implementada nesta etapa: amostragem inteira filtrada
permanece no caminho anterior. O lowering novo de sampler inteiro cobre
texelFetch e consultas de tamanho/níveis, além do acesso storage solicitado.
As demais limitações dos schemas anteriores continuam aplicáveis.

A entrega final passou no PC com 347 PASS Vulkan, zero falhas, casos sem suporte
ou erros de API. Os onze novos formatos responderam por 55 casos básicos e 11
casos de aliases copiados; os 55 acessos storage/aliases exigiram estatísticas
reais do backing raw. Houve 14 verificações de leitura inteira exata (sete
formatos, sem/com aliases). O controle --native passou com 264 PASS. Os 88 novos
SPIR-V foram validados para Vulkan 1.2. A inicialização do host passou com a
validação D3D12 ligada, incluindo apresentação, resize e shutdown.

O Release do emulador e o diagnóstico foram assinados (CMS_SIGNATURE=PASS), e os
payloads foram comparados com o build: seis DLLs e EXE no emulador, driver, EXE
e shaders novos no diagnóstico. SHA-256 do bundle:
`002bd22282f8a7bb29b41eeb2a92354eadb0d797e60f60c273d7a54a6eff2cec`.
SHA-256 do diagnóstico 1.0.17.0:
`93bd611f3c9d1e1c5a8e5890430234f4908f25eb29e827784f03ce7dc3a6abd9`.
Nenhum pacote foi instalado. O suporte adicional ainda aguarda a execução do
schema 18 no Xbox; os demais formatos ausentes não foram habilitados nesta etapa.

## SSBO explícito e amostragem somente leitura (schema 19)

O log do Xbox reportou Non-UAV destination to atomic intrinsic ao criar o pipeline
de pós-processamento. A análise nir_opt_access não contabilizava load_ssbo,
store_ssbo, ssbo_atomic e ssbo_atomic_swap, inseridos antes de explicit-I/O pelo
lowering raw. Depois da conversão de cargas coerentes em atômicos, o otimizador
podia inferir readonly e emitir o recurso como SRV. A análise agora contabiliza
esses acessos; atômicos contam como leitura e escrita e preservam a classe UAV.
O ABI de cache passa a 31. O perfil de produção permanece ativo.

Seis casos simultaneous_sampler_readonly_attachment usam fragment shader com
sampler e saída de cor, sem storage/SSBO do aplicativo, com LOD implícito. A
imagem começa com cor fracionária conhecida, e dois draws escrevem o complemento
do canal vermelho, conferido nos bytes nativos; o contador de efeitos laterais
SSBO deve permanecer zero. O driver anterior reproduziu exatamente Non-UAV no
primeiro caso novo. A versão corrigida passou os seis e os 347 anteriores:
END 353/0, unsupported=0, api_errors=0; SIMULTANEOUS_END 36/0. O controle --native
passou com 264/0. O SPIR-V novo passou no spirv-val para Vulkan 1.2.

Os pacotes Release e diagnóstico 1.0.18.0 foram assinados (CMS_SIGNATURE=PASS),
e seus payloads foram conferidos com o build. SHA-256 do bundle:
`97daf483d4b58b70a8c3d2b657b610b57c1b0b6dd19d3df6767dd88197f66195`.
SHA-256 do diagnóstico:
`eaae0e5f6e907b372b9f0cd7fb9ce592cd4a15f54897ddf1e17ae86888b13dae`.
A correção ainda aguarda execução no Xbox. Os logs de capacidade sem avisos para
os onze formatos novos não substituem a conferência dos valores na GPU.
