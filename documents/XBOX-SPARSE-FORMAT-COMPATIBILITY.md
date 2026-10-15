# Xbox: recursos esparsos e formatos de imagem

Os logs fornecidos em 14/10/2026 registram o adaptador `SraKmd_arden`, Dozen 26.2.2 e a falha `vk::Result=ErrorFeatureNotPresent` imediatamente depois da enumera√ß√£o dos formatos. Eles n√£o identificam a chamada Vulkan exata que falhou. O cache criava incondicionalmente um buffer de prova esparso nesse ponto da inicializa√ß√£o, mesmo quando o dispositivo n√£o oferece `sparseBinding`/`sparseResidencyBuffer`.

## Caminho sem buffers esparsos

O cache UWP consulta essas features e escolhe buffers device-local alocados quando n√£o est√£o dispon√≠veis. As regi√µes usam granula√ß√£o de 16 MiB e s√£o unidas quando h√° sobreposi√ß√£o ou adjac√™ncia. A migra√ß√£o copia o conte√∫do pelo runtime e mant√©m os buffers anteriores vivos at√© a conclus√£o da GPU. Os intervalos de shaders, v√©rtices, √≠ndices, indiretos e c√≥pias s√£o preparados antes dos binds para preservar aliases na mesma opera√ß√£o. Uploads, readbacks e invalida√ß√£o continuam usando o rastreamento existente.

As chamadas de cria√ß√£o do buffer de prova e dos recursos de fault processing passaram a informar o nome da opera√ß√£o nas assertions. Isso permite distinguir uma falha posterior diferente no pr√≥ximo log do Xbox. O caminho alocado pode consumir mais mem√≥ria que o esparso; ele n√£o anuncia suporte esparso inexistente.

`Build/dense-pacman-validation2.log`: a ferramenta nativa local mascarou as features esparsas via proxy de diagn√≥stico, sem alterar o driver distribu√≠do. O PAC-MAN 256 chegou a **PRESS X TO BEGIN**, apresentou 931 frames em 25 segundos e retornou `BOOT_RESULT=0`, `SHUTDOWN=0`. Captura: `Build/dense-pacman-frame.png`. As mensagens `FAULT` dessa ferramenta s√£o notifica√ß√µes de primeira chance dos page faults tratados pelo emulador; n√£o equivalem a falhas n√£o tratadas. O proxy e essa instrumenta√ß√£o ficam somente em Build e n√£o s√£o empacotados.

## AtualizaÁ„o: falha DXIL WaveSize / PSV

O log atualizado das 19:05 identifica `Failed to create fault buffer compute pipeline` e uma divergÍncia entre `WaveSize=64` no mÛdulo DXIL e o `PSVRuntimeInfo` do container. A geraÁ„o de PSV agora usa o tamanho de wave efetivamente emitido nos metadados DXIL, em vez de recalcul·-lo em outro ponto. O UUID de cache do Dozen foi atualizado para descartar blobs anteriores incompatÌveis. Falhas de validaÁ„o DXIL s„o relatadas como erro de compilaÁ„o/validaÁ„o, n„o como falta de memÛria.

`Build/xbox-wave64-validation.log`: o shader original de fault processing foi compilado com subgroup obrigatÛrio de 64 lanes e passou a validaÁ„o e o readback nativo, reconstruindo trÍs endereÁos, inclusive 34342961152. A validaÁ„o adicional de Shader Model 6.6 com Wave64 tambÈm passou pelo validador DXIL; a leitura do container confirmou `PSV_MIN=64`, `PSV_MAX=64`. Esta verificaÁ„o no PC n„o confirma execuÁ„o no Xbox.

A pedido do usu·rio, a implementaÁ„o experimental de NGS2 foi retirada do cÛdigo desta entrega. O estado anterior de ·udio foi preservado; o trabalho adiado est· somente em `Build/ngs2-deferred`.

## Formatos sem formato declarado no shader

O Dozen j√° tinha o caminho de imagens storage no NIR/DXIL, mas a convers√£o para `VkFormatProperties3` descartava as flags de 64 bits. Agora `StorageReadWithoutFormat` e `StorageWriteWithoutFormat` s√£o preenchidas separadamente, conforme `D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD`/`UAV_TYPED_STORE`, somente para formatos com storage de imagem ou texel buffer. Formatos sem esse suporte continuam sem a flag.

Foram executadas quatro variantes de readback, cada uma com 81 casos e tr√™s fases de atualiza√ß√£o/c√≥pia de descritores: float32/uint32, float16, R8 UNORM e R16 UNORM. Todas passaram, incluindo imagens sem qualifier de formato e escrita/leitura do mesmo recurso. Evid√™ncias: `Build/unformatted-images-readback.log`, `Build/unformatted-half-readback.log`, `Build/unformatted-unorm8-readback.log` e `Build/unformatted-unorm16-readback.log`.

Os avisos de formatos realmente ausentes, como certos formatos packed ou D16 com stencil, n√£o foram convertidos em capabilities falsas. O fallback anterior de upload D16 para profundidade suportada permanece. N√£o foram implementadas todas as extens√µes opcionais listadas como indispon√≠veis, nem todos os usos de cada formato anunciado no log.

A execu√ß√£o no Xbox e no aplicativo UWP instalado ainda precisa ser confirmada com o novo pacote; a valida√ß√£o acima foi feita no PC nativo.

Refer√™ncias: [Vulkan: formatos](https://docs.vulkan.org/spec/latest/chapters/formats.html), [Microsoft: UAV typed loads](https://learn.microsoft.com/en-us/windows/win32/direct3d12/typed-unordered-access-view-loads).
