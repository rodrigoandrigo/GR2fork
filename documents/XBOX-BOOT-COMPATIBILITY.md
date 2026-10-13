# Inicialização Vulkan no Xbox

O núcleo UWP aceita a ausência de `VK_EXT_extended_dynamic_state`. Nesse caso,
viewport e scissor usam os comandos Vulkan básicos, com contagem na pipeline.
Os estados de profundidade, operações de stencil, culling, orientação das faces
e strides dos vértices também participam da chave da pipeline. Blits de MSAA
seguem a mesma seleção de comandos. O cache serializado usa a versão 4 da chave
para impedir a leitura de chaves com o layout anterior.

O Dozen mantém suas capacidades reais: a extensão continua condicionada a
`IndependentFrontAndBackStencilRefMaskSupported`. Esta adaptação não habilita
capacidades falsas nem remove os requisitos de robustness2. As limitações do
driver para stencil independente permanecem separadas desta inicialização.

Uma assertion da DLL agora retorna arquivo, linha, função e mensagem ao host.
A descarga de logs UWP espera a fila assíncrona antes de descarregar o arquivo,
preservando o diagnóstico que antes podia ficar somente no buffer.

O log `shadps4-XBOX.log` recebido tinha 4.096 bytes e terminava no meio da lista
de extensões, sem a assertion. A ausência de extended dynamic state reproduz
no PC a mesma mensagem genérica com a DLL anterior. Essa reprodução identifica
um bloqueio corrigível, mas não prova que seja o único bloqueio do Xbox.

## Validação da correção

- Release do núcleo e frontend compilados; bundle assinado com o certificado
  temporário do projeto, com verificação CMS aprovada.
- DLL anterior: falha ao remover somente extended dynamic state da enumeração.
- DLL corrigida: quatro ciclos nativos de boot, parada, recarga e falha guest
  passaram nessa condição; os mesmos quatro ciclos passaram no caminho normal.
- Remover robustness2 continua causando erro obrigatório, com localização e
  motivo no retorno ao host e no arquivo de log.
- Os hashes das cinco DLLs no pacote correspondem aos arquivos de Runtime/Release.

Os ciclos usam pequenos executáveis ELF de diagnóstico em um processo nativo
no PC. Não validam a interface instalada, renderização de jogos, AppContainer
ou execução física no Xbox. Os resultados estão em
`Build/xbox-fallback-validation.log` e `Build/xbox-assert-final-build.log`.
