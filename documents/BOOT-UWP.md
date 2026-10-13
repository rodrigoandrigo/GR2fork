# Boot brokered e sessões UWP

`shadps4_run` é uma chamada bloqueante para um worker MTA. Seu argumento é um
caminho virtual de uma pasta previamente montada pela API, por exemplo
`/library0/titulo/eboot.bin`. A DLL obtém a subpasta pelo StorageFolder autorizado
e monta o mesmo backend em `/app0` e `/hostapp`, ambos somente leitura quando o
grant original é somente leitura. Não remonta o caminho físico externo.
Metadados, ELF, módulos e dados em `/app0` usam a abstração de arquivos existente.
Os arquivos privados de configuração, saves e logs continuam no diretório do host.

O frontend coloca o boot na thread de sessão, preserva o render ImGui e acompanha
o estado da DLL. Pausar, continuar e parar ficam disponíveis no menu. Alterações
de configurações, importações e mudanças nos mounts ficam bloqueadas durante a
execução. A conclusão e seus erros chegam à fila de atualizações do frontend.

A parada impede novas chamadas HLE do guest, retoma threads suspensas, acorda
os pontos de cancelamento e envia interrupções para threads executando código
guest. Não encerra arbitrariamente uma thread dentro de código host. Aguarda os
handles nativos antes de liberar os objetos de pthreads. A espera pelas threads
guest tem prazo de dez segundos; falha preserva a DLL e seus recursos, e o
shutdown pode repetir a tentativa. VideoOut, áudio, AJM, Vdecsw, NP, serviço do
kernel, cache e gamepads são encerrados antes de liberar o módulo.

Cada carregamento da DLL admite uma sessão. Após a conclusão, o frontend para o
worker de arquivos, descarrega realmente a DLL, carrega uma instância nova,
resolve novamente os exports e restaura os mounts a partir dos StorageFolders
retidos pelo host. Reutiliza o processo e os painéis, sem reutilizar singletons
guest. Uma referência restante ao módulo impede iniciar outra sessão; o host
preserva sua referência e apresenta erro. Chamadores externos devem seguir a
mesma sequência: aguardar `shadps4_run`, fazer shutdown, remover callbacks,
descarregar, carregar, inicializar e remontar as pastas.

Os limites da stack guest no TEB do Windows permanecem válidos durante a
troca de stack. Isso permite que erros C++/WinRT tratados pelo loader e pelo HLE
sejam capturados sem encerrar o processo. O callback de término e `exit` guest
solicitam o fim da sessão; não encerram o aplicativo UWP.

## Validação

O diagnóstico nativo no PC carrega a DLL dinamicamente e usa StorageFolder de
uma pasta de fixtures. Um ELF mínimo incrementa um contador em memória guest,
permitindo verificar execução, pausa e retomada. Foram verificados a resolução
da subpasta, os aliases `/app0`/`/hostapp`, parada após retomada, parada enquanto
pausado, saída pelo callback de término, falha guest por instrução inválida e
recarga da DLL no mesmo processo. A pasta do ELF contém caracteres Unicode.
O diagnóstico confirma o descarregamento consultando o loader após FreeLibrary.
Os programas e logs ficam no diretório ignorado `Build`.

O homebrew [embedding/main.cpp](hardware/embedding/main.cpp) fornece os casos
OpenOrbis de leitura, pthreads e término normal, além da variante de parada pelo
host. O SDK OpenOrbis não está instalado; esse homebrew não foi compilado ou
executado no PS4.

Build e diagnósticos nativos não comprovam boot de um jogo completo, comportamento
do frontend instalado, permissões do AppContainer, apresentação física ou Xbox.
Essas validações permanecem separadas. O pacote não é instalado pelo build.
