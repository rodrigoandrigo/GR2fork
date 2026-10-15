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

## Reserva física no UWP

Os logs `shad_log.txt` e `shadps4 (1).log` recebidos em 14/10/2026 reconhecem
PAC-MAN 256, mas o primeiro falha ao criar o backing físico de 8448 MiB com
`SEC_COMMIT`: o Xbox informa que o arquivo de paginação é insuficiente.
No UWP, o backing agora usa `SEC_RESERVE`; o mapeamento guest compromete apenas
o intervalo físico necessário, mantendo o mesmo objeto compartilhado e seus
aliases. O caminho desktop conserva sua política anterior.

Segundo a [documentação do Windows](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createfilemappinga),
páginas de uma seção `SEC_RESERVE` podem ser comprometidas com VirtualAlloc,
mas não descomprometidas individualmente. O backing permanece vivo até encerrar
a sessão; essa mudança não aumenta o orçamento de memória disponível no Xbox.

Uma falha durante a construção libera as reservas e handles já criados.
Consultas `storage_stat` a arquivos inexistentes retornam o HRESULT ao chamador
sem disparar o callback de erro da sessão; permissões e outros erros continuam
reportados. Isso evita tratar a procura opcional de metadados como falha de boot.

O segundo log também contém falha de semáforo. O Dozen antes convertia qualquer
falha de CreateFence em `ErrorOutOfDeviceMemory`. Agora registra o HRESULT,
flags e motivo de remoção do dispositivo, distinguindo device lost. O log antigo
não permite determinar a causa D3D12 dessa falha; sua resolução física precisa
ser confirmada com o novo pacote no Xbox.

O diagnóstico nativo no PC com o backing sob demanda chegou à tela de título,
apresentou 704 frames em 25 segundos e encerrou com boot e shutdown retornando
zero, sem erro de sessão. Um teste adicional de 35 segundos apresentou 1317
frames e também encerrou sem erro. VirtualQueryEx confirmou 4273,609 MiB
comprometidos e 4174,391 MiB reservados no backing durante o jogo.
O core registra o uso e o limite de memória do aplicativo antes do boot.
Não é validação de orçamento ou execução no Xbox.

O bundle Release foi assinado com a chave temporária do projeto e a assinatura
CMS passou. Os hashes do core, Dozen, SDL3, DXC, DXIL e frontend dentro do pacote
correspondem aos artefatos Release. A DLL exata desse pacote apresentou 782
frames em 25 segundos no diagnóstico nativo, com boot e shutdown zero e erro
vazio. O AppContainer instalado e o Xbox ainda precisam de confirmação.
