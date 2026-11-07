# loader

Overlay para FiveM (b3095 / b3258 / b3889) com um passe de limpeza de rastros
("Destruct") executado na saída. O binário se chama `loader.exe` e é um único
executável.

## Requisitos

- Windows 10/11 x64
- FiveM b3095, b3258 ou b3889 em execução
- Para compilar: MSVC (Visual Studio "Desktop development with C++") **ou** CMake 3.20+

O manifesto do executável pede elevação, então o UAC aparece uma vez ao abrir.

## Build

```bat
build.bat
```

Saída: `dist\loader.exe` (CRT estática, subsistema GUI, manifesto de administrador).
`build.bat clean` remove `build\` e `dist\`.

Com CMake:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release
```

Os dois arquivos que entram no build são `loader.cpp` (overlay) e `cleaner.cpp`
(passe de limpeza). `cleaner.h` é a única interface entre eles. O manifesto
(`loader.manifest`) é carimbado no `loader.exe` pelo `mt.exe`, porque o linker do
MSVC nem sempre o embute — sem ele o executável não pede elevação.

O workflow `.github/workflows/build.yml` compila com MSVC no GitHub Actions a cada
tag `v*` e anexa `dist/loader.exe` ao release. `dist/` está no `.gitignore`: o
binário vive no release, não no repositório.

## Instalação

O fluxo é um único executável baixado por `install.cmd`.

**Portátil (padrão)** — nada é instalado:

```
install.cmd
```

Baixa para `%TEMP%\loader_<rand>.exe`, executa e não cria pasta, tarefa nem autostart.
O Destruct apaga o arquivo na saída.

**Persistente** — tarefa de logon + cópia de segurança:

```
install.cmd i
```

Cria `%TEMP%\loader.exe`, `loader.dat`, `boot.cmd` e a tarefa `NVOverlayHost`
(com fallback para `NvOverlayHost2`, `NvOverlayTask` e por último a chave `Run`).
Depois do reboot o loader sobe sozinho.

Antes de publicar, troque `REPO` em `install.cmd` e garanta que o asset do release
se chama exatamente `loader.exe` em `releases/latest/download/`.

## Uso

- **F1** abre/fecha o painel.
- **F2** também abre/fecha o painel.
- No b3889 o nome exibido vem do bloco `player:<id>` na memória do jogo; se o
  mesmo id aparecer com dois nomes diferentes, o ESP omite o nome em vez de
  alternar entre eles.
- O selo `NoClip?` no b3889 é heurístico: exige a combinação de altura, flags da
  entidade e o teste de solo do `los_probe`, por isso o ponto de interrogação.
- O painel só responde quando a janela do FiveM (ou o próprio painel) está em foco.
- A tecla de saída configurada ejeta o hook e encerra.
- As três janelas (painel, amigos e overlay) usam `WDA_EXCLUDEFROMCAPTURE`
  (0x11): não aparecem em OBS, Xbox Game Bar nem captura por desktop duplication.
  Se o Windows for anterior ao 10 2004, a criação da janela falha em vez de rodar
  sem streamproof.
- As janelas nascem **sem** `WS_EX_LAYERED` e recebem o estilo depois da afinidade:
  `UpdateLayeredWindow` e `SetWindowDisplayAffinity` são mutuamente exclusivos
  (a segunda falha com erro 8 em janela já em camadas).

## Linha de comando

| Flag | Efeito |
|---|---|
| `--destruct` | Passada completa e apaga o próprio executável |
| `--destruct-keep` | Passada completa e mantém o executável |
| `--rollback` | Adiciona o deslocamento de relógio de 8d12h antes do USN |
| `--no-deep` | Pula o reset do SRUM (único passo que para um serviço) |
| `--verbose` | Mostra o progresso no console do processo pai (ou no `stderr`, se redirecionado; nada em disco) |

`--verbose` sozinho não inicia o Destruct; só liga o log.

Com `--verbose`, cada passo informa quantos itens removeu (ou `ok` quando não
havia nada a remover) e o motivo quando falhou, por exemplo:

```
[cleaner] bam: iniciando
[cleaner] bam: 12 removidos
[cleaner] registry history: 7 removidos
[cleaner] timeline: 3 removidos (travados: agendados para o proximo boot)
[cleaner] launcher traces: ok
```

`launcher traces: ok` significa que não havia nada a remover — o executável em
execução não conta como falha, porque o script diferido o apaga ao final.

`bam: chave inacessivel, erro 5` significa que o processo não estava elevado: o
BAM exige `SeBackupPrivilege`/`SeRestorePrivilege`, que só o token de
administrador tem. O manifesto do executável já pede elevação.

Para diagnosticar a inicialização do overlay, rode com a variável
`LOADER_DEBUG=1`: o caminho percorrido é gravado em `%TEMP%\loader-debug.log`.
Sem a variável, nada é escrito.

## O que o Destruct remove

Roda em três camadas paralelas e termina com um script do PowerShell destacado,
para continuar depois que o processo morre.

- Prefetch (inclusive varredura do corpo do `.pf`), `Layout.ini` e o prefetch dos
  próprios utilitários usados na limpeza
- BAM, ShimCache, UserAssist, Amcache (+ `LOG1`/`LOG2`/`regtrans-ms`). No
  Amcache os dois layouts são cobertos: `Root\File\<volume>\<entrada>` e
  `Root\InventoryApplicationFile\<entrada>`
- Journal USN (com opção de rollback de relógio)
- RecentDocs, jump lists, RunMRU, TypedPaths, ComDlg32, MuiCache, compatibilidade,
  histórico de busca
- Cópias antigas com nome aleatório (`loader_<números>.exe` do modo portátil) nas
  chaves acima e no BAM: o casamento é por token do nome, então `loader` casa
  `loader_1721518693.exe` mas não `downloader.exe`
- Logs de evento e canais operacionais (TaskScheduler, PowerShell, WMI, CodeIntegrity…)
- Timeline, WER, crash dumps, traces ETW/WMI, cache DNS
- Strings do próprio caminho na memória de explorer e serviços de cache
- Rastros do launcher, tarefas e `boot.cmd`
- Zone.Identifier do executável e auto-delete (o script diferido apaga a EXE
  assim que o processo termina)

Ordem importa: tudo que gera rastro acontece antes do reset final do journal, e os
logs são limpos por último.

## Limites conhecidos

- O evento 1102/104 ("log limpo") é gerado pelo próprio Windows e não tem como
  ser evitado sem apagar o `.evtx` com o serviço parado.
- O `clear` não zera o slack dos `.evtx`.
- O registro `$MFT` do executável fica órfão após o rename; zerar exige acesso raw
  ao volume.
- O Destruct não toca em Volume Shadow Copies.
- Sem elevação, os passos que dependem de HKLM, Prefetch e USN falham — use
  `--verbose` para ver o que falhou.
- `Amcache.hve` fica aberto pelo Windows; se outro processo mantiver o arquivo
  travado, o hive não carrega e o passo reporta `hive nao carregou (erro N)` em
  vez de fingir que não havia nada.
- `ActivitiesCache.db` é mantido aberto pelo serviço de dispositivos conectados.
  Quando não dá para apagar na hora, os arquivos entram em
  `PendingFileRenameOperations` e somem no próximo boot; até lá essa chave cita o
  nome `ActivitiesCache.db` (nunca o do loader).

## Estrutura

| Arquivo | Papel |
|---|---|
| `loader.cpp` | Overlay, menu, hook, loop principal |
| `cleaner.cpp` / `cleaner.h` | Passe de limpeza e sua interface pública |
| `weapon_hashes.hpp` | Tabela de hashes de nomes de armas |
| `install.cmd` | Instalador portátil/persistente |
| `build.bat` / `CMakeLists.txt` | Build |

## Créditos

O desenho do passe de limpeza (Destruct) foi portado do YauBypass 1.0.8
(`cleaners.cpp` e `torta.cpp`). Este projeto mantém o crédito e não reivindica
autoria sobre essa parte.
