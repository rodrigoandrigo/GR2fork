# Rastreamento do Mesa-UWP

O código de `externals/mesa-uwp` é incorporado diretamente ao Git do GR2fork,
sem submódulo. A versão declarada em `VERSION` é 26.2.2. A primeira inclusão
registra o estado local atual, incluindo as adaptações UWP e Dozen existentes;
não representa uma cópia upstream sem modificações nem recupera o histórico
das alterações anteriores.

Depois dessa inclusão, alterações nos fontes aparecem normalmente em
`git status --short -- externals/mesa-uwp` e podem ser revisadas com
`git diff -- externals/mesa-uwp`. Alterações preparadas para commit podem ser
revisadas com `git diff --cached -- externals/mesa-uwp`.

Diretórios de compilação e instalação, caches Python e dependências baixadas
em `subprojects` continuam ignorados. Os scripts de `bin` e os arquivos de
`build-support` são fontes do Mesa, portanto são incluídos apesar das regras
gerais de artefatos do repositório principal.

Os arquivos `scripts/build-dozen.ps1` e `scripts/mesa-uwp-x64.ini` registram a
configuração de compilação usada pelo GR2fork. Alterações nesses arquivos
devem acompanhar os fontes quando afetarem a integração.
