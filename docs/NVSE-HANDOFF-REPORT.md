# Relatório de Handoff Técnico — Suporte Opt-In ao xNVSE 6.4.9 + Plugins (Fallout: New Vegas)

**Data:** 2026-11-25  
**Branch:** `feat/nvse` (derivada de `main` no commit `8e4e376`, **não** mergeada em `main`)  
**Remote `origin`:** `https://github.com/pedroh77/Fallout-ElderScrolls-macOS-Port.git`  
**Remote `upstream`:** `git@github.com:arjuk95-dot/FNV-iOS-Shim.git`  
**Status:** Pausado intencionalmente pelo usuário nesta branch antes de investigar o travamento em **Load Save / New Game**.

---

## 1. Resumo Executivo (Onde Estamos)

1. **Menu Principal com 6 plugins funcionando e estável:**
   - Os 6 plugins instalados pelo usuário (`EngineOptimizations.dll`, `jip_nvse.dll`, `johnnyguitar.dll`, `nvse_stewie_tweaks.dll`, `NVTF.dll`, `ShowOffNVSE.dll`) carregam juntos e chegam ao **Menu Principal em `40.6s – 42.1s` a `60.0 fps`** (baseline sem plugins: `16.8s` a `60.1 fps`), com **12/12** execuções headless bem-sucedidas (zero deadlocks, traps ou `TerminateProcess`).
   - Anteriormente, o conjunto levava `~195s+`, sofria deadlocks no spinlock `0x10076d58` do `jip_nvse.dll` (`0x10005540`) e corrupção de refcounts (`TRAP` / `TerminateProcess(0xc0000417)`) causada por execuções não-atômicas de instruções `LOCK` dentro do Unicorn.
2. **Problema em aberto reportado pelo usuário (NÃO investigado ainda):**
   - No app real (`~/Applications/Fallout New Vegas.app`), com xNVSE + os 6 plugins ativos:
     - O jogo chega ao Menu Principal (após aparecer o Ranger e passar alguns segundos, o cursor responde e o menu aparece).
     - Porém, **ao carregar um save ou iniciar `New Game`**, durante a tela de loading **o jogo congela no meio do fade das frases da tela** (elas param de apagar/aparecer no meio da transição), **enquanto a música continua tocando**.
   - **Vanilla (com xNVSE desligado na janela de Options):** o cursor responde antes, cliques já pulam da segunda tela (`Bethesda`) direto para o Ranger e Menu Principal, e o jogo carrega normalmente (verificado brevemente pelo usuário por ~10s, sem quebras aparentes no Vanilla).
3. **Decisão atual:** manter `main` intacto, documentar todo o estado e contexto técnico aqui, e commitar + dar push apenas na branch `feat/nvse`.

---

## 2. Contexto do Ambiente do Usuário (Que Não Está Apenas no Código)

### 2.1 App Instalado e Plugins do Usuário
- App compilado e assinado ad-hoc em: `~/Applications/Fallout New Vegas.app`
- Runtime do xNVSE 6.4.9 + Microsoft x86 C/C++ runtime instalados em:
  `~/Applications/Fallout New Vegas.app/Contents/Resources/GameData/NVSE/runtime/`
  (`nvse_1_4.dll`, `ucrtbase.dll`, `vcruntime140.dll`, `msvcp140*.dll`, `concrt140.dll`, `vcamp140.dll`, `vccorlib140.dll`, `vcomp140.dll`).
- Pasta persistente de plugins do usuário (mapeada para `Data\NVSE\Plugins` via `FNV_NVSE_PLUGINS_DIR`):
  `~/Library/Application Support/FalloutNV/NVSE/Plugins/`
  Arquivos atualmente presentes na máquina do usuário:
  - `EngineOptimizations.dll` (`InlineVanillaFunctions.dll` — sem `.ini`, `NVSEPlugin_Load` apenas retorna `1`; `nvse_stewie_tweaks.dll` checa sua presença para ativar `[Inlines]`)
  - `jip_nvse.dll` + `jip_nvse.pdb`
  - `johnnyguitar.dll` + `JohnnyGuitar.ini` + `johnnyguitar.pdb`
  - `nvse_stewie_tweaks.dll` + `nvse_stewie_tweaks.ini` + `nvse_stewie_tweaks.pdb`
  - `NVTF.dll` + `NVTF.pdb`
  - `ShowOffNVSE.dll` + `ShowOffNVSE.ini` + `ShowOffNVSE.pdb`
  - `Stewie Tweaks Readme.txt`, pastas `Tweaks/` e `textinput/`
- Diretório real de saves/INIs do usuário (**NUNCA** apontar testes automatizados para cá; usar sempre `FNV_USER_DIR=/tmp/...`):
  `~/Library/Application Support/FalloutNV/UserDocuments`

### 2.2 Caches Locais de Compilação Incremental (Máquina Atual)
A tradução do `FalloutNV.exe` com as alterações novas de `tools/translate/emit.py` **já está cacheada** neste Mac:
- Generated C: `~/Library/Caches/fnv-macos/translate-89e7020487e8/generated`
- Cache de objetos: `~/Library/Caches/fnv-macos/objects-fnv-89e7020487e8`
- Codecs: `~/Library/Caches/fnv-macos/codecs/prefix`
- Unicorn: `/opt/homebrew/opt/unicorn`

**Rebuild incremental (~3 segundos quando apenas arquivos `upstream/recomp/host/*.c` mudam):**
```bash
rm -rf /tmp/fnv-rebuild && mkdir -p /tmp/fnv-rebuild && \
python3 tools/build.py game --game fnv \
  --generated "$HOME/Library/Caches/fnv-macos/translate-89e7020487e8/generated" \
  --exe "$HOME/Applications/Fallout New Vegas.app/Contents/Resources/GameData/FalloutNV.exe" \
  --codec-prefix "$HOME/Library/Caches/fnv-macos/codecs/prefix" \
  --unicorn-prefix /opt/homebrew/opt/unicorn \
  --cache "$HOME/Library/Caches/fnv-macos/objects-fnv-89e7020487e8" \
  --out /tmp/fnv-rebuild --app-name "Fallout New Vegas" --jobs 8 --no-call-trace && \
cp -f "/tmp/fnv-rebuild/Fallout New Vegas.app/Contents/MacOS/FalloutNewVegas" \
  "$HOME/Applications/Fallout New Vegas.app/Contents/MacOS/FalloutNewVegas" && \
rm -rf /tmp/fnv-rebuild && codesign --force --deep --sign - "$HOME/Applications/Fallout New Vegas.app"
```
> **Nota importante para outro ambiente:** se os caches `translate-*` não existirem ou se `tools/translate/emit.py` for alterado, é necessário re-traduzir o executável (`./quick-install.sh --game fnv --with-nvse --keep-cache`).

---

## 3. Método e Mudanças Implementadas (Marcos 1 a 4 + Correções de Concorrência/Performance)

### Marco 1 — Carregador PE + Shims Win32 Multi-Módulo
- **`upstream/recomp/host/pe_loader.c` & `pe_loader.h`:** parser/loader PE32 em memória convidada (`MEM`), resolução de imports contra a tabela de shims e exports de DLLs já carregadas (incluindo forwarders, relocações base, suporte a PDB/export directory e busca em diretórios de runtime/`NVSE/Plugins`).
- **`upstream/recomp/host/shims_imagehlp.c`:** `MapAndLoad`, `UnMapAndLoad`, `ImageRvaToVa`, `ImageDirectoryEntryToData` usados pelo `PluginChecker` do NVSE.
- **TLS estático multi-módulo (`upstream/recomp/host/shims_kernel32.c`, `host.h`):** registro de templates TLS de DLLs e alocação dinâmica por slot em `teb + 0x1000` para todas as threads vivas (`MAX_TLS_MODULES = 32`, `MAX_LIVE_TEBS = 256`).
- **Expansão de Shims Win32:**
  - `shims_kernel32.c`: `LoadLibraryExA`, `GetModuleHandleExA`, `DisableThreadLibraryCalls`, `RtlAllocateHeap`/`Free`/`ReAllocate`/`Size`, `HeapCompact`/`Validate`/`QueryInformation`/`Walk`, `VirtualProtect_`, `FlushInstructionCache_`, `VirtualQuery_`, `GetTickCount64_`, `GetCurrentProcessorNumber_`, `FlushProcessWriteBuffers_`, etc.
  - `shims_sync.c`: `SRWLock` (`Initialize/Acquire/Release/TryAcquire` Shared & Exclusive), `ConditionVariable` (`Initialize/Wake/WakeAll/SleepConditionVariableCS/SRW`), `InitOnceExecuteOnce`, `RtlInitialize/Enter/Leave/DeleteCriticalSection`, `SList` (`RtlInitializeSListHead`, `InterlockedPushEntrySList`, `InterlockedFlushSList`), `Threadpool` (`Work`/`Timer`/`Wait`), `WaitOnAddress` / `WakeByAddressSingle` / `WakeByAddressAll`, `CreateEventExW`, `CreateSemaphoreExW`, `FreeLibraryAndExitThread`.
  - `shims_files.c`: redirecionamento automático de `Data\NVSE\Plugins\*` quando `FNV_NVSE_PLUGINS_DIR` está definido (`match_nvse_plugins_rel`), além de `CreateFile2`, `CreateFileMappingA/W`, `MapViewOfFile`, `UnmapViewOfFile`, `GetFileInformationByHandleEx`, `SetFileInformationByHandle`, `FindFirstFileW`, `FindFirstFileExA`, `GetPrivateProfileSectionA`, `GetPrivateProfileSectionNamesA`, `CreateDirectoryA/W`, `DeleteFileW`, `MoveFileExA`, `CopyFileW`, etc.
  - `shims_text.c`, `shims_win.c`, `d3dx9.c`: `CompareStringEx`, `IsValidLocaleName`, `EnumSystemLocalesEx`, `GetLocaleInfoEx`, `GetDateFormatW`, `GetTimeFormatW`, `GetSystemTimePreciseAsFileTime`, `GetNativeSystemInfo`, `GetKeyboardState`, `ToUnicode`/`ToUnicodeEx`, `OpenClipboard`/`GetClipboardData`/`SetClipboardData`, `D3DXCreate*TextureFromFileInMemoryEx`.
- **Teste unitário:** `tests/pe_loader_test.c` & `tests/pe_loader_test.py`.

### Marco 2 — Desotimização Granular, Sites Dinâmicos de 3 Estados e Boot Hook
- **`tools/translate/emit.py` & `upstream/recomp/rt_dispatch.c` & `upstream/recomp/rt.h`:**
  - Emite tabelas `rt_fn_group`, `rt_fn_patched`, `rt_fn_ranges`, `rt_fn_range_max_span`, `rt_dyn_sites`, `rt_dyn_site_count`.
  - Funções multi-entrada compartilham um único corpo `static void b_<entry>(CPU *c, int ent, uint8_t _ps)` e expõem wrappers `g_<addr>`.
  - Estado de grupo `rt_group_state(gid)` (`rt_fn_patched[gid]`):
    - `0` = vanilla nativo ARM64;
    - `1` = desotimizado (entra em `rt_fallback` / Unicorn);
    - `2` = função nativa modificada apenas em sites dinâmicos suportados (`CALL rel32`, `PUSH imm8`, `PUSH imm32`, ou substituições in-place de 5 bytes preservando flags validadas por `rt_is_valid_call5_bytes` — mantém a função inteira em ARM64 nativo e despacha apenas o site alterado via `rt_call_rel32` ou empurra o novo operando via `rt_push_imm*`). Isso manteve nativas funções enormes como `g_602190` (`TESObjectREFR::LoadForm`, 240 KB, modificada em `0x602b02` por `B8 0B 00 00 00`).
- **Snapshot Diff na `.text` (`upstream/recomp/host/shims_kernel32.c`):**
  - Mantém `orig_text` e `synced_text` em `VirtualProtect_` / `FlushInstructionCache_`. Quando um plugin desbloqueia toda a `.text` (`sz > 0x10000`, ex.: `VirtualProtect(0x401000, 0xd8104c, ...)`), apenas bytes realmente alterados são desotimizados (`sync_text_patches_locked`), evitando derrubar todos os 86.062 grupos para Unicorn.
  - Em chamadas `VirtualProtect_` menores (`sz <= 0x10000`), `classify_written_span_locked` é sempre chamada mesmo antes de restaurar permissões antigas (pois `VirtualProtect_` retorna `old_p = 0x40` RWX).
- **Boot Hook (`upstream/recomp/host/host.c`):**
  - `host_arm_nvse_boot()` verifica a chamada `CALL WinMain` em `0x00ECC46B` (`0x0086a850`) e arma `rt_set_boot_hook(winmain, nvse_on_winmain_entry)` para injetar `nvse_1_4.dll` na entrada real do `WinMain` (quando `--no-nvse` / `FNV_NVSE=0` não desabilitar).
- **Teste unitário completo:** `tests/patch_deopt_test.py` (cobrindo 12 suítes de testes: desotimização simples, multi-entrada, split-chunk, boot hook, sites dinâmicos `CALL5`/`PUSH2`/`PUSH5`, substituições 5B preservando flags e upgrade `2->1`, fast-leaf e atomicidade de bailout, além de estresse Unicorn multi-thread).

### Marco 3 & Marco 4 — Instalador Opt-In e Interface macOS
- **`quick-install.sh`:** flag `--with-nvse` (exclusiva para `--game fnv`) faz download e verificação SHA-256 do xNVSE 6.4.9 oficial e do runtime oficial Microsoft x86 VCLibs/UCRT em tempo de build (zero binários proprietários no git).
- **`macos/OptionsFallout.m` & `macos/FNVApp.m` & `README.md`:** detecção de instalação do NVSE, checkbox **Enable xNVSE** (`FNV_NVSE=0|1`), botão **Open Plugins…** e auto-configuração de `FNV_NVSE_PLUGINS_DIR` apontando para `~/Library/Application Support/FalloutNV/NVSE/Plugins`.

### Otimização & Correção de Concorrência no `fallback.c`
1. **De 1.356 hooks por gap para 1 único hook `.text`:** eliminou overhead de lista de hooks no Unicorn (`~195s -> ~105s`).
2. **Interpretador atômico rápido para funções folha (`try_fast_leaf` em `fallback.c`):** executa funções curtas hookadas (`FAST_LEAF_MAX_STEPS = 48`, `FAST_LEAF_MAX_WRITES = 8`, decode completo ModRM/SIB, flags exatas, XMM shadow `XORPS`/`PXOR`/`MOVQ`, tail-jumps/calls nativos com checkpoint de `cur_ip_io`, e prefixo `LOCK` atômico) diretamente em C sem Unicorn. Baixou `all6` para `~41s–43s`.
3. **Correção do Deadlock e Corrupção de Refcount Multi-Thread em Unicorn:**
   - **Causa raiz provada:** cada thread cria seu próprio `uc_engine`, e o Unicorn 2 roda QEMU TCG com `parallel_cpus = false`. O QEMU reduz `LOCK CMPXCHG`, `LOCK XADD`, `LOCK INC`, `LOCK DEC` e `XCHG r/m, r` para load+store **não-atômicos**. Sob contenção (como o spinlock `0x10076d58` em `0x10005540` de `jip_nvse.dll`, onde outra thread solta o lock com store simples `and [0x10076d58], 0`), a thread perdedora no Unicorn sobrescreve o `0` com o valor travado antigo e entra em deadlock infinito. Da mesma forma, antes de `try_fast_leaf` tratar `0xF0`, funções `NiRefObject` inlined pelo `nvse_stewie_tweaks` corrompiam refcounts (`TRAP` / `TerminateProcess(0xc0000417)`).
   - **Implementação final em `fallback.c`:**
     - Substituiu a tentativa anterior de `UC_HOOK_BLOCK` universal (que causava +15–22s de regressão em `nvse_stewie_tweaks.dll` por impedir TB-chaining e chamar `_tlv_get_addr` no loop de descompressão chamado por `b_4740a0`) por **`ThreadUC *` (zero TLS nos callbacks)** + **`UC_HOOK_EDGE_GENERATED` (`on_edge`)** + varredura do bloco inicial (`tuc_ensure_entry_atomics`) + **`UC_HOOK_CODE` apenas nos endereços exatos** que contêm `LOCK` ou `XCHG [mem]`.
     - Quando atingidos, são executados por `exec_atomic_insn()` via `__atomic_*(__ATOMIC_SEQ_CST)` reais do ARM64 sob `rt_lock()`/`rt_unlock()` (mantendo 100% de coerência tanto com o código C recompilado `tu_*.c` que usa `rt_lock()`, quanto com `shims_sync.c` `Interlocked*` e stores normais de memória).

### Abordagens já testadas e DESCARTADAS (Não Repetir!)
- **Não usar `BounceStat` nem shadow-diff de FPU/XMM:** causou ~40% de regressão e `__report_gsfailure`.
- **Não usar `UC_HOOK_BLOCK` cobrindo DLLs inteiras (`0x00010000..0xFFFFFFFF`):** destrói TB chaining no QEMU e custa +15–22s no loop de descompressão do Stewie Tweaks (`all6` sobe para `55–61s`).
- **Não tentar apenas `rt_lock(); uc_emu_start(..., count=1); rt_unlock();` no Unicorn:** não resolve contra threads fazendo unlock com `mov`/`and [lock], 0` (ainda ocorre race no load-store não-atômico do QEMU).

---

## 4. Resultados Medidos (Build Atual da Branch `feat/nvse`)

### 4.1 Testes Unitários e Host Checks (Todos `exit 0`)
- `python3 tests/patch_deopt_test.py` (ambos modos: `try_fast_leaf` ativo e `FNV_NO_FAST_LEAF=1`) — **PASS**
- `python3 tests/pe_loader_test.py` — **PASS**
- `python3 tests/file_shims_case_test.py` — **PASS**
- `python3 tests/sync_shims_test.py` — **PASS**
- `python3 tests/virtual_alloc_test.py` — **PASS**
- `bash tests/run.sh` — **PASS**
- `python3 tools/build.py host-check --game fnv --unicorn-prefix /opt/homebrew/opt/unicorn` — **PASS**
- `python3 tools/build.py host-check --game fnv --no-unicorn` — **PASS**
- `python3 tools/build.py host-check --game fallout3 --unicorn-prefix /opt/homebrew/opt/unicorn` — **PASS**
- `python3 tools/build.py host-check --game skyrim --unicorn-prefix /opt/homebrew/opt/unicorn` — **PASS**

### 4.2 Benchmarks Headless até o Menu Principal (12/12 sem erros/traps/deadlocks)
Medidos com `FNV_HEADLESS=1 FNV_STDERR=1 FNV_USER_DIR=/tmp/fnv-prof/user` (critério de Menu Principal = primeiro log `[metal]` com `>= 10 draws/frame`):

| Perfil | Grupos / Dyn | NVSE Carregado | 1º Input (`ui_menu_cancel`) | Menu Principal | FPS |
|---|---:|---:|---:|---:|---:|
| `none` (0 plugins) | 42 / 34 | `2.5s` | `10.8s` | **`16.8s`** | `60.1 fps` |
| `jip_1` | 190 / 90 | `1.1s` | `11.6s` | **`23.7s`** | `60.1 fps` |
| `jip_2` | 190 / 90 | `1.2s` | `15.2s` | **`21.7s`** | `60.1 fps` |
| `jip_3` | 190 / 90 | `1.0s` | `11.6s` | **`23.6s`** | `60.0 fps` |
| `no_eo_no_jip` | 279 / 233 | `2.9s` | `23.2s` | **`31.6s`** | `60.1 fps` |
| `no_eo` (5 plugins) | 470 / 250 | `3.3s` | `25.2s` | **`31.9s`** | `59.8 fps` |
| `no_jip` (5 plugins) | 699 / 460 | `3.0s` | `36.5s` | **`40.5s`** | `59.9 fps` |
| `all6_1` | 887 / 469 | `4.1s` | `37.4s` | **`41.4s`** | `60.0 fps` |
| `all6_2` | 887 / 469 | `4.2s` | `38.1s` | **`42.1s`** | `60.0 fps` |
| `all6_3` | 887 / 469 | `4.1s` | `37.8s` | **`41.8s`** | `59.9 fps` |
| `all6_4` | 887 / 469 | `3.2s` | `36.6s` | **`40.6s`** | `60.0 fps` |
| `all6_5` | 887 / 469 | `3.3s` | `36.5s` | **`40.6s`** | `60.0 fps` |

> **Observação sobre `EngineOptimizations.dll`:** comparando `no_eo` (`31.9s`) vs `all6` (`40.6s–42.1s`), `EngineOptimizations.dll` adiciona ~`9s` ao boot porque faz o `nvse_stewie_tweaks.dll` substituir ~462 funções folha vanilla por código x86 em `.text`. Em ARM64, isso não traz ganho porque essas funções já eram ARM64 `-O2`.

### 4.3 Teste Interativo Real do Usuário (2026-11-25)
- **Vanilla (`Enable xNVSE` OFF):** cursor responde rápido, clique pula a introdução direto a partir da 2ª tela (`Bethesda`) para o Ranger e Menu Principal; jogo carrega normalmente (teste breve de ~10s, sem falhas visíveis).
- **Com xNVSE + 6 plugins ativos:**
  - Chega ao Menu Principal logo após a tela do Ranger, com controle de cursor funcionando.
  - **Falha pendente:** ao iniciar **New Game** ou **Load Save**, durante a tela de loading **o jogo congela** (as dicas de texto travam no meio do fade-in/fade-out), enquanto a música continua tocando. O usuário confirmou que aguardou e o comportamento bloqueia o gameplay com os 6 mods por enquanto.

---

## 5. Próximos Passos Recomendados para o Próximo Agente

1. **Reproduzir e diagnosticar o freeze de New Game / Load Save:**
   - Rodar com `FNV_STDERR=1` + `FNV_TRACE_SYNC=1` (e opcionalmente `FNV_FALLBACK_LOG=/tmp/fb.log`) enquanto dispara `New Game` ou carrega um save via `FNV_INPUT_SCRIPT` (ou manualmente na UI) usando `FNV_USER_DIR` isolado (cópia do save real em `/tmp`).
   - Quando congelar na tela de loading, capturar `sample <PID> 3` + `spindump` ou anexar `lldb -p <PID>` (`thread backtrace all`) para identificar:
     - Se há threads presas em Unicorn (`uc_emu_start` / algum loop em DLL ou função hookada de carregamento de célula/save/BSA/NVSE);
     - Se há deadlock entre shims de sincronização (`shims_sync.c`: `SRWLock`, `WaitOnAddress`, `CriticalSection`, `ConditionVariable`) ou `pe_loader`/file shims (`CreateFileMapping`/`MapViewOfFile` usados pelo `jip_nvse`/`nvse_stewie_tweaks`/cosave `.nvse`);
     - Se algum dos 6 plugins isoladamente já passa pelo loading ou qual módulo introduz o travamento (testar em modo binário: apenas NVSE sem plugins -> cada plugin isolado -> combinações).
2. **Armadilhas conhecidas deste ambiente:**
   - **Nunca** execute `pkill -f "FalloutNewVegas"` em comando zsh que contenha a string `"FalloutNewVegas"` (mata o próprio subshell). Use `pkill -x FalloutNewVegas`.
   - Em execuções headless sem TTY, passe `FNV_STDERR=1`, caso contrário `FNVApp.m` redireciona `stderr` para `~/Library/Logs/FalloutNV/fnv.log`.
   - `FNV_QUIT_AFTER` só conta após frames Metal (`Present()`). Se travar sem renderizar frames, combine com timeout no shell/Python.
   - Sempre preserve `~/Library/Application Support/FalloutNV/UserDocuments` usando `FNV_USER_DIR=/tmp/...` em testes automatizados.
