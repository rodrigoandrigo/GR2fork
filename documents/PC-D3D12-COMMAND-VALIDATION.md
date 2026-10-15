# Falha de comandos D3D12 no PC

Os logs do aplicativo instalado recebidos em 14/10/2026 mostram Chronicles of
Teddy (CUSA03328) falhando em `ID3D12GraphicsCommandList::Close`, HRESULT
`0x80070057`, que o Dozen reportava como `ErrorOutOfHostMemory`.

A reprodução nativa com a mesma DLL confirmou duas mensagens da validação
D3D12: 869, footprint de cópia fora do buffer, e 538, uso de vertex buffer
ainda no estado de destino de cópia.

O guest usa D16 com stencil separado. Quando o host substitui D16+S8 por um
formato suportado com plano de profundidade maior, o upload direto não preserva
o tamanho nem a representação dos texels. O upload de profundidade de uma
amostra agora passa por R16 UNORM e pelo passe de conversão de profundidade
existente, com seleção de mip e camada. O stencil não é reinterpretado como
parte desses texels. Esta alteração não implementa conversão de download ou
upload de D16+S8 multiamostrado.

No Dozen, cópias entre buffers restauram o destino para acesso comum após a
escrita, com barreira de buffer no caminho enhanced e transições no caminho
legacy. Uma falha de Close mantém o HRESULT no log e não é mais classificada
automaticamente como falta de memória. Se houver camada de validação ativa,
até 32 mensagens são encaminhadas ao callback de logs.

As regras relevantes estão na [especificação de cópias Vulkan](https://docs.vulkan.org/spec/latest/chapters/copies.html)
e na [documentação de estados D3D12](https://learn.microsoft.com/en-us/windows/win32/direct3d12/using-resource-barriers-to-synchronize-resource-states-in-direct3d-12).

A espera por VBlank no UWP agora observa o encerramento da sessão e cancela a
thread guest depois de liberar o mutex. O comportamento desktop é preservado.
O rastreamento de uma parada durante a introdução identificou a thread principal
em sceKernelWaitEqueue. As esperas de equeue e event flag também observam a
parada, preservam o prazo original e saem antes de devolver execução ao guest.
Eventos já recebidos não são consultados novamente pelo predicado consumidor.
Ao repetir o boot com o mesmo save, também foi identificada uma corrida na
consulta de tamanho durante a rotação dos backups. As consultas agora usam o
mesmo mutex do worker e excluem as pastas internas de backup da contagem.

## Evidência e limites

- Antes: a falha de Close foi reproduzida no diagnóstico nativo.
- Depois: 1159 frames em 30 segundos, boot e shutdown zero, erro de sessão vazio
  e nenhuma mensagem D3D12 armazenada na fila de validação do dispositivo.
- A ativação forçada da camada D3D12 foi temporária e removida do código final.
- Na compilação sem essa ativação, a parada durante a introdução apresentou
  277 frames em 15 segundos, com boot e shutdown zero e erro vazio, reutilizando
  o save existente. PAC-MAN 256 também encerrou sem erro após 507 frames em
  25 segundos.
- A captura final desse teste ficou preta. Frames apresentados não comprovam
  chegada ao menu, gameplay, áudio correto ou execução do pacote instalado.
- NGS2 continua com funções não implementadas, incluindo VoiceGetState; seus
  avisos não foram suprimidos nem substituídos por estados de áudio inventados.
- Os quatro ciclos de pausa, retomada, término, falha guest intencional e
  recarga da DLL passaram no diagnóstico de sessão. O bundle Release foi
  assinado com a chave temporária do projeto; assinatura CMS e hashes do core,
  Dozen, SDL3, DXC, DXIL e frontend foram conferidos.
- A DLL exata do pacote apresentou 529 frames em 15 segundos em Chronicles of
  Teddy, com boot e shutdown zero e erro de sessão vazio. A captura desse teste
  também permaneceu preta; o aplicativo instalado e o gameplay não foram validados.

O homebrew [depth16-upload/main.cpp](hardware/depth16-upload/main.cpp) fornece
uma referência de leitura normalizada de 16 bits no PS4. O SDK OpenOrbis não
está configurado; ele não foi compilado ou executado no hardware nesta tarefa.
Os diagnósticos e seus logs ficam no diretório ignorado `Build`.
