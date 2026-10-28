# Comparação do touchpad em hardware PS4

Este homebrew registra os relatórios `scePadReadState` para comparar um DualShock 4 físico com o touchpad emulado pelo controle Xbox no ShadPS4-UWP.

Adicione `main.c` a um projeto C do OpenOrbis e vincule as bibliotecas `ScePad`, `SceUserService`, `kernel` e `c`. Use o fluxo habitual do SDK para gerar o SELF e o pacote homebrew. O SDK OpenOrbis não está instalado neste ambiente; não foi gerado ou executado um binário de hardware nesta revisão.

O programa coleta aproximadamente 120 segundos, grava `/data/shadps4-touchpad.csv` e informa coordenadas fora da resolução anunciada. Um caminho alternativo pode ser fornecido como primeiro argumento.

Faça a mesma sequência nos dois ambientes:

1. Controle em repouso, sem tocar no touchpad.
2. Toque sem clicar, mova o dedo até os cantos e volte ao centro mantendo contato.
3. Clique durante o contato e depois solte apenas o clique.
4. Levante o dedo; comece outro contato e confira a mudança de identificador.
5. Abra o menu, suspenda ou desconecte durante um contato e confira a liberação.

No Xbox, segure **View** para usar o analógico esquerdo como dedo. O contato começa quando o analógico sai da zona de início ou quando L3 é pressionado; persiste até soltar View. **View + L3** representa o clique. Depois de abrir o menu ou perder o controle, solte View antes de iniciar outro gesto.

Compare presença de contato, clique independente, limites das coordenadas e continuidade dos identificadores. `touch_aux` registra o campo bruto adicional do SDK, cujo significado depende da versão do firmware; não é usado como critério automático.

Definições da API conferidas nos headers oficiais [Pad.h](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/Pad.h) e [_types/pad.h](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/_types/pad.h).
