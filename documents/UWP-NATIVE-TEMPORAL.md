# API de jitter temporal do D3D12 nativo

Esta referência descreve o contrato de configuração e leitura de jitter. O
estado do renderer, a alocação de recursos e as pendências ficam em
[D3D12-RENDERER.md](D3D12-RENDERER.md).

Jitter permanece desativado até que um consumidor temporal configure seu alvo.
O FSR1 espacial existente não usa esse mecanismo. A API não implementa
reconstrução temporal, motion vectors ou separação de HUD.

## Jitter

TemporalJitter usa Halton (bases 2 e 3), centrado em meio pixel. A amostra e a
mesma em todos os draws elegiveis de um frame. A fase so avanca ao preparar um
novo frame VideoOut que utilizou jitter; apresentar/reutilizar o ultimo frame
nao avanca a sequencia. Frames sem draw elegivel invalidam o historico.

A aplicacao ocorre no viewport do primeiro alvo de cor, somente quando endereco,
origem zero e dimensoes correspondem exatamente a configuracao. O deslocamento
na rasterizacao equivale ao deslocamento de clip XY apos a divisao por W;
os dados guest e as constantes de projecao nao sao reescritos. O scissor permanece
fixo. O integrador deve selecionar um alvo de cor da cena anterior ao HUD e
tratar os passes de profundidade/motion vectors antes de ativar reconstrucao.
Nao ha inferencia automatica de cameras ou classificacao por jogo/shader.

O offset em pixels usa X para direita e Y para baixo. NDC usa
(2*x/renderWidth, -2*y/renderHeight). pixels, previous_pixels, ndc, frame, phase,
phases, generation, reset e applied ficam disponiveis. applied=false significa
que nao existe amostra de rasterizacao valida para aquele frame. Troca de
configuracao, nova sessao e ResizeBuffers invalidam o historico. Alteracoes do
viewport fora das dimensoes selecionadas nao recebem jitter.

Com phases=0, a contagem e floor(8*(outputWidth/renderWidth)^2), limitada a 1024.
Um consumidor XeSS pode fornecer sua propria contagem; nao ha exigencia de que
os dois SDKs usem a mesma politica. O reset inicia novamente na primeira fase e
iguala previous_pixels a pixels para nao introduzir velocidade artificial.

As convencoes seguem as referencias oficiais:
- https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-temporal/
- https://www.intel.com/content/www/us/en/developer/articles/technical/xess-sr-developer-guide.html

## API da DLL

shadps4_configure_native_temporal_jitter recebe ShadPS4TemporalJitterConfig:
size=sizeof(config), target_address, render_width, render_height, output_width,
output_height e phases. target_address=0 desativa a funcao. A configuracao
publica e aceita em READY/STOPPED, antes da sessao, evitando mudancas no meio
de um lote guest. O renderer/consumidor nativo pode reconfigurar TemporalJitter
diretamente em uma fronteira de frame sincronizada ao integrar resolucao dinamica.

shadps4_get_native_temporal_jitter recebe um ShadPS4TemporalJitterSample com
size=sizeof(sample) e completed_frame=0 para a amostra corrente ou 1 para a
ultima amostra preparada. Um reset invalida a ultima amostra ate outro frame
ser preparado. Cada frame nativo tambem armazena sua propria copia da amostra,
para que uma fila de apresentacao nao associe o jitter de outro frame a ele.

Nao ativar jitter em producao sem o passe de reconstrucao temporal: jitter
isolado desloca a amostragem, mas nao produz antialiasing temporal por si so.

