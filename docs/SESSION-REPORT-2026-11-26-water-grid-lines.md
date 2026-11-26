# SESSION REPORT — 2026-11-26 — “grid lines” na água (Fallout 3)

Relato do usuário que abriu a investigação:

> “Two is there is a strange grid line on water sources.”

Reproduzido no port de macOS em água rasa (Fallout 3), com o mesmo padrão das capturas do
relator: linhas finas em grade, formando um “X”/cruzamentos, visíveis na superfície da água e
que parecem “congelar” a ondulação quando a água as encontra. Num cruzamento, a água parece
dividida em quatro “piscinas” separadas.

Este documento registra **tudo** que foi tentado, na ordem, incluindo o que foi descartado,
o que quebrou, como foi revertido e o que ainda pode/ precisa ser tentado. O objetivo é
permitir retomar sem repetir caminhos já refutados.

---

## 1. Ambiente e artefatos

| Item | Valor |
|---|---|
| Máquina | MacBook Air M1 (`MacBookAir10,1`), sem notch |
| Jogo | Fallout 3 (Steam), `Fallout3.exe` SHA-256 `c3f97c2255fa…` |
| Port | `Fallout-ElderScrolls-macOS-Port`, branch `main` |
| Cache de tradução usado | `~/Library/Caches/fnv-macos/translate-c3f97c2255fa/` (`--keep-cache`) |
| App instalado | `~/Applications/Fallout 3.app` |
| Log do jogo | `~/Library/Logs/Fallout3/fallout3.log` |
| INIs do jogo | `~/Library/Application Support/Fallout3/UserDocuments/My Games/Fallout3/` |
| Capturas do relator | anexos da conversa (2 imagens, água rasa + cruzamento) |
| Vídeos de análise | `/tmp/water-grid.mov` (gravado com `FNV_RECORD`; a 2ª gravação sobrescreveu a 1ª) |
| Dumps de shader | `/tmp/shaders-dump/` (1532 arquivos, 766 shaders) e `/tmp/shaders-dump2/` |
| Builds intermediários | `/tmp/rebuild-f3/`, `/tmp/rebuild-f3b/`, `/tmp/rebuild-f3t/` |
| Backups do executável | `~/Applications/Fallout 3.app/Contents/MacOS/Fallout3.bak-before-shaderdump` |
| Helper de INI | `~/Library/Caches/fnv-macos/water-test.sh` (liga/desliga features de água) |

---

## 2. Pesquisa inicial (relatos no Windows)

Antes de investigar o port, foi feita pesquisa sobre o mesmo sintoma no jogo original. Resumo:

- **Water displacement**: relatos no Steam/ENB dizem que `bUseWaterDisplacements=1` causa
  “grid lines” na água e recomendam `bUseWaterDisplacements=0` (guia do ENB e vários presets).
  - <https://steamcommunity.com/app/22370/discussions/0/1738884444451857392>
  - <https://www.nexusmods.com/fallout3/images/38609>
  - <https://archive.org/details/fallout-3-imperfect-wasteland-preset-enb.-7z>
- **Refrações/reflexões**: o mesmo comentário do Nexus cita “water refractions” e recomenda
  desligar todos os efeitos e religar um a um para achar o culpado.
- **Z-fighting nas bordas da água**: mod “Water edges fix” (Nexus 23311) existe só para isso e
  recomenda `fNearDistance=9` no `Fallout.ini`.
  - <https://www.nexusmods.com/fallout3/mods/23311>
- **Gaps no mesh de água**: GECK wiki relata erros no mesh de água (polígonos faltando) em
  worldspaces com default water height abaixo de 10500.
  - <https://geck.uesp.net/wiki/Special:MobileDiff/1729>
- **Depth fog**: relato russo cita “Depth Fog” causando artefato na água em SLI/Crossfire.
  - <https://www.playground.ru/fallout_3/forum/tehnicheskie_problemy_v_10-1445315>

Conclusão da pesquisa: havia várias causas plausíveis **no próprio Windows**, então o primeiro
teste deveria ser A/B via configurações — sem tocar em código.

---

## 3. Configuração encontrada e presets do port

O `FalloutPrefs.ini` estava no preset **Ultra** de água:

```ini
[water]
bForceHighDetailReflections=1
bUseWaterDisplacements=1
bAutoWaterSilhouetteReflections=0
iWaterBlurAmount=4
iWaterReflectWidth=1024
bUseWaterReflectionBlur=1
bUseWaterRefractions=1
bUseWaterReflections=1
bUseWaterDepth=1
iWaterReflectHeight=1024
```

No `FALLOUT.INI` havia uma **segunda cópia** das chaves:

```ini
[Water]
bUseWaterShader=1
bUseWaterReflections=1
bUseWaterRefractions=1
bUseWaterDepth=1
bUseWaterHiRes=0
bUseWaterDisplacements=1
bUseWaterLOD=1
...
```

No port (`macos/OptionsFallout.m`):

- O preset de qualidade **só escreve** o bloco `[water]` do `FalloutPrefs.ini` quando
  `opts.preset > 0` (guarda em `OptionsFallout.m:744`); com **Keep** o arquivo não é tocado.
- O `FALLOUT.INI` recebe apenas `[Controls]`/`[Display]`; **água nunca é escrita nele**.
- Presets de água: Low desliga refractions/reflections/displacements/depth (256); Medium/High
  ligam tudo (512); Ultra liga tudo com 1024, blur 4 e `bForceHighDetailReflections=1`.

---

## 4. Testes de configuração (A/B) — todos refutados

| # | Teste | Como | Resultado |
|---|---|---|---|
| 1 | Water Detail = **Low (Off)** via launcher | preset grava `[water]` do Prefs com tudo 0 | Linhas “sumiram” (depois reinterpretado: só ficaram menos visíveis) |
| 2 | `bUseWaterDisplacements=0` só no `FalloutPrefs.ini` | `sed` na linha 48 | Linhas continuaram |
| 3 | `displacement=0` também no `FALLOUT.INI` | `sed` na linha 410 (a 1ª tentativa esqueceu este arquivo) | Linhas continuaram |
| 4 | `refractions=0` nos dois INIs | `sed` linhas 53/404 | Continuaram |
| 5 | `reflections=0` nos dois INIs | `sed` linhas 54/402 | Continuaram |
| 6 | `depth=0` nos dois INIs | `sed` linhas 55/406 | **“Sumiram”** naquele momento |
| 7 | Restaurar tudo e deixar **só depth=0** | backups `.bak-gridtest` | Linhas **voltaram** |
| 8 | **Tudo em 0** (0 0 0 0) | helper `water-test.sh` | Linhas **continuam**, só mais discretas |
| 9 | fNearDistance 5 → 9 (z-fighting) | editar `FALLOUT.INI:261` | **Nenhuma mudança**; revertido para 5 |

Conclusões:

- Nenhuma combinação de settings de água elimina o artefato; elas só mudam a **visibilidade**
  (água mais “simples” disfarça as linhas).
- O “sumiu” do teste 6 foi **ilusão**: no teste 8, com tudo desligado, as linhas continuam.
- Portanto o problema **não é** displacement/refrações/reflexões/depth, nem z-fighting de
  near-plane, nem presets de água. **Ultra não está descartado por si.**

Ferramentas deixadas:

```bash
~/Library/Caches/fnv-macos/water-test.sh 0 0 0 0   # liga/desliga as 4 features nos 2 INIs
```

Backups: `FalloutPrefs.ini.bak-gridtest`, `FALLOUT.INI.bak-gridtest`,
`FalloutPrefs.ini.bak-near5`, `FALLOUT.INI.bak-near5`.

---

## 5. Análise de imagem (capturas + vídeos)

### 5.1 Capturas

As imagens do relator e as do port mostram linhas finas, claras, formando uma grade alinhada
ao mundo, com um cruzamento em “X”. Ampliações (crops 2–3×) mostram um padrão pontilhado ao
longo da diagonal, compatível com borda/emenda, não com linha desenhada.

### 5.2 Vídeo 1 (37 s) — inconclusivo

- Gravado com `FNV_RECORD=/tmp/water-grid.mov`, 1920×1080, ~18,4 fps.
- Segmentos com câmera parada (bússola/HUD imóveis) eram só a tela “PLEASE STAND BY” e o menu.
- No gameplay a câmera/arma se moviam o tempo todo → mapas de variância dominados por movimento
  de câmera. Nada conclusivo.

### 5.3 Vídeo 2 (58 s) — decisivo

- Nova gravação no mesmo caminho (sobrescreveu a 1ª), 1058 frames.
- Correlação de fase por FFT entre frames com a câmera parada deu shift **(0,0)** → câmera
  realmente imóvel.
- Análise (frames reduzidos a 480×270 cinza, numpy):

  - A **água em volta anima** (diff temporal mostra mudança de textura/specular).
  - As faixas do “X” aparecem **escuras no diff** → **não mudam**: são estáticas.
  - Mapas de std/alta-frequência mostram as faixas como bandas de baixa variação.

- Conclusão: o artefato é uma **faixa estática na superfície da água** (a ondulação para ali),
  não uma linha desenhada e não z-fighting (que cintilaria com a câmera).

### 5.4 Método de análise (reprodutível)

```bash
ffmpeg -i /tmp/water-grid.mov -vf "scale=480:270,format=gray" -f rawvideo /tmp/water-analysis2/gray.raw
# numpy: reshape (N,270,480); diff entre frames; std temporal; fase por FFT (phase correlation)
```

---

## 6. Análise dos shaders do próprio jogo

### 6.1 Extração dos shader packages

Os shaders do Fallout 3 ficam em `Data/Shaders/shaderpackage0NN.sdp` (dentro do app, em
`Contents/Resources/GameData/Data/Shaders`). Os `.sdp` contêm bytecode D3D9 + CTAB (nomes de
constantes). Foi possível listar as constantes e, depois, com o dump do próprio port,
mapear nomes → registradores.

### 6.2 Constantes do shader de água (extraídas do CTAB)

**Vertex shader (`WATER*.vso`, vs_1_1):**

| Constante | Reg set | Reg | Count |
|---|---|---|---|
| ModelViewProj | 2 (float) | 0 | 4 |
| WorldMat | 2 | 4 | 3 |
| TexScale | 2 | 8 | 1 |
| QPosAdjust | 2 | 9 | 1 |
| ObjectUV | 2 | 10 | 1 |

**Pixel shader (`WATER*.pso`, ps_2_0/ps_3_0):**

| Constante | Registrador |
|---|---|
| EyePos | c1 |
| ShallowColor | c2 |
| DeepColor | c3 |
| ReflectionColor | c4 |
| FresnelRI | c5 |
| BlendRadius | c6 |
| VarAmounts | c8 |
| FogParam | c9 |
| FogColor | c10 |
| DepthFalloff | c11 |
| SunDir | c12 |
| SunColor | c13 |

Texturas: **RefractionMap = s1, NoiseMap = s2, DisplacementMap = s3, DepthMap = s4**.

### 6.3 MSL gerado pelo port (dump)

O port foi instrumentado com `FNV_DUMP_SHADERS=<dir>` para gravar bytecode + MSL de cada
shader traduzido (766 shaders). Trechos relevantes do VS de água:

```c
// (MSL gerado; nomes de registradores r0/r1, constantes cf[N])
r1.x = dot(WorldMat.row0, v0);   // posição em world space
r1.y = dot(WorldMat.row1, v0);
r1.zw = r1.xy + QPosAdjust.xy;
vout.S5_0.xy = r1.xy;            // world XY

r0.x = sign(ObjectUV.x);         // escolhe entre UV do mesh e world-space
r0.y = CF7.y / TexScale.x;
r0.z = 1 / TexScale.x;
r0.zw = r0.z * r1.zw;                                     // world/scale (com QPosAdjust)
r1.xy = vin.v2 * r0.y - r0.zw;
vout.S5_7.xy = r0.x * r1.xy + r0.zw;   // UV do NoiseMap (S5_7)
```

E no PS:

```hlsl
r0 = t2.sample(s2, pin.S5_7.xy);      // NoiseMap
r1 = t4.sample(s4, r1.xy / r1.w);     // DepthMap
r4 = t3.sample(s3, pin.S5_6.xy);      // DisplacementMap
r1 = t1.sample(s1, r5.xy / r5.w);     // RefractionMap
```

**Descoberta central:** não existe constante de **tempo** em nenhum dos dois shaders. A UV do
NoiseMap é uma combinação estática de `WorldMat`, `QPosAdjust`, `TexScale`, `ObjectUV` e das
UVs do mesh. Logo, a animação da água **tem que vir de atualização por frame** dessas
constantes (provavelmente `ObjectUV`) e/ou de transform de textura do D3D9.

---

## 7. Hipótese “texture transform” — implementada e REFUTADA

### 7.1 Implementação tentada

Baseado na ausência de constante de tempo e no fato de o port guardar mas nunca aplicar
`D3DTS_TEXTURE0..3` / `D3DTSS_TEXTURETRANSFORMFLAGS`:

- `sm2msl.c`: helper `d3d_tex(txr, txf, stage, uv)` aplicado nos samples; buffers 4/5 na
  assinatura do pixel shader.
- `d3d9_metal.m`: por draw, subir matrizes por stage (`transform[16..19]` transpostas) e flags
  (`tss[stage][24]`), em `buffer(4)`/`buffer(5)`.

### 7.2 Resultado

Rebuild + instalação + execução no save da poça:

- **Slowdown forte** por alguns instantes e depois **a água sumiu**.
- Em outro save, performance de ~1 frame a cada 10 s.
- Saves na poça sem água, inclusive thumbnails.

### 7.3 Causa do desastre (bug da implementação, não do port)

No helper emitido, a primeira linha era:

```c
if (txf[0][s] == 0) return uv;
```

com `txf` contendo apenas **8 ints** (counts/proj só para stages 0–3). Para samplers
`s >= 4` — e a água amostra **DepthMap em s4** — a leitura saía do buffer e pegava lixo.
Com lixo != 0, a transform era aplicada com matrizes fora de faixa (`txr[s*4+…]`), gerando
UVs inválidas/NaN → água invisível e custo absurdo de GPU.

**Lição:** qualquer indexação por sampler precisa limitar `s < 4` (ou definir counts para os
16 samplers) antes de decidir aplicar transform.

### 7.4 Reversão

- Binário original restaurado de
  `~/Applications/Fallout 3.app/Contents/MacOS/Fallout3.bak-before-shaderdump`, re-assinado
  ad-hoc, jogo relançado; água/performance confirmadas normais pelo usuário.
- Mudanças da tentativa: `git stash@{0}` —
  “texture-transform experiment (broke water; keep dump idea)”.

---

## 8. Instrumentação de trace — provou que o engine NÃO usa TTFF

### 8.1 O que foi instrumentado (somente leitura, env-gated)

`FNV_TRACE_TEX=1`:

- `d3d9.c` em `SetTextureStageState(…, 24, v)`: loga quando `v != 0` (TTFF ativo), com
  `count`, `proj` e `TEXCOORDINDEX`.
- `d3d9.c` em `SetTransform(D3DTS_TEXTURE0..3, M)`: loga a matriz quando o TTFF da stage está
  ativo.
- `d3d9_metal.m` no draw: se alguma stage 0–3 tem TTFF, loga uma vez por combinação
  (vs, ps, mask) com flags, `TEXCOORDINDEX` e tamanho das texturas ligadas.

### 8.2 Execução

- Rebuild só com log (`/tmp/rebuild-f3t`), instalado, jogo aberto com
  `FNV_SKIP_LAUNCHER=1 FNV_TRACE_TEX=1`.
- Usuário carregou o save e ficou no ponto de água por 5–10 s; saiu com Cmd+Q.
- Log: **zero linhas `[tex]`**.
- Verificação do mecanismo de env: `ps eww -p <pid>` mostrou a variável
  `FNV_DUMP_SHADERS=/tmp/envcheck3` numa execução com `open --env`, e — numa verificação
  dedicada — mostrou **`FNV_TRACE_TEX=1`** no processo do jogo. O binário de trace também
  contém as strings (`strings | grep 'TTFF stage'`).
- Logo, a ausência de `[tex]` no log da passada na água **não é falta de env**: o engine
  simplesmente não ativou `TEXTURETRANSFORMFLAGS` nem `SetTransform(D3DTS_TEXTURE*)` naquela
  cena.

### 8.3 Conclusão

O engine **não habilita `TEXTURETRANSFORMFLAGS` nem usa `SetTransform(D3DTS_TEXTURE*)`**
nessa cena. A hipótese de texture transform está **refutada**; a animação da água vem das
constantes por draw/frame (`ObjectUV`/`TexScale`/`QPosAdjust`/`WorldMat`), que o port já
entrega (a água ondula).

---

## 9. Onde o diagnóstico está agora

Fatos estabelecidos:

1. Não é ajuste de qualidade de água (todas as combinações testadas; settings só mudam a
   visibilidade).
2. Não é z-fighting de near plane (`fNearDistance` sem efeito).
3. Não é textura-transform do D3D9 (engine não usa TTFF na cena).
4. A água **anima** no port; as faixas do “X” são **estáticas** (vídeo com câmera parada).
5. O artefato tem cara de **emenda/geometria do plano de água** (bordas de mesh/UV), não de
   estado de animação.

Hipóteses remanescentes (a testar):

- **Gaps/emendas no mesh do plano de água** (polígonos faltando, bordas duplicadas, vértices
  deslocados) — documentado no GECK wiki para certos worldspaces, mas também possível por
  erro de geometria gerada em runtime.
- **Descontinuidade de UV/atributos** nas bordas do mesh, que congela a amostragem (mip/edge)
  naquela faixa.
- **Particularidade do local/worldspace** (faixas só em certas poças/cells).
- **Regressão específica da recompilação** no caminho que gera o mesh de água (posições/
  índices/UVs calculados pelo código do jogo).

---

## 10. O que ainda pode ser tentado (ordem sugerida)

1. **Identificar os draws de água em runtime e registrar a estrutura do mesh**
   - Já temos os hashes dos shaders de água nos dumps (`vs_757c07f1`, `vs_b2475655`,
     `vs_f80ca834`; PS `ps_1e128d8d`, `ps_3f9f0297`, …).
   - Instrumentar (env-gated, sem alterar render): quando o VS/PS em uso casar com esses
     hashes, logar `vs`, `ps`, topologia, `num_vertices`, `count` de índices, `base_vertex`,
     `min_index` e texturas ligadas. Isso mostra se as faixas coincidem com **limites entre
     draws** (mesh separado por cell/water body) ou com **buracos dentro do mesmo mesh**.
2. **Despejar vértices/índices de um plano de água no ponto do “X”** e verificar no host:
   - vértices duplicados com posições ligeiramente diferentes nas bordas (fresta);
   - triângulos de borda ausentes/degenerados;
   - UVs descontínuas na emenda.
3. **Teste “sem mip”** (diagnóstico já embutido no build de dump): `FNV_NO_MIPS=1` força
   `MTLSamplerMipFilterNotMipmapped`. Se as faixas sumirem, é band de mip/UV (e a correção é
   continuidade de UV, não geometria). Precisa do build com dump (hoje só no stash).
4. **Comparar com Windows** no mesmo local (pedir screenshot ao relator ou usar vídeo de FO3).
   Se o artefato existir no Windows, é comportamento do engine/dados; se não, é do port.
5. **Testar outros corpos d’água** (outras poças, rio, lago) para ver se é universal ou
   específico de cell/worldspace.
6. **Testar o port de Fallout: New Vegas** numa água (mesmo backend/engine, exe diferente).
   Se o FNV também mostrar, é sistêmico do backend; se não, é específico do FO3.
7. **Investigar o caminho de geração do mesh de água no código recompilado** (se os gaps
   coincidirem com bordas de cell): procurar em `translate-c3f97c2255fa/generated` as funções
   de water plane / geometry generation e comparar com o esperado (posições, índices,
   arredondamento FPU).
8. **Rever o culling/clipping de água** (por exemplo, se bordas de mesh são cortadas por
   scissor/viewport ou por depth clip).

---

## 11. Receita para retomar (comandos)

### Restaurar o jogo ao normal

```bash
APP="$HOME/Applications/Fallout 3.app"
cp -p "$APP/Contents/MacOS/Fallout3.bak-before-shaderdump" "$APP/Contents/MacOS/Fallout3"
codesign --force --deep --sign - "$APP"
```

### Rebuild com instrumentação (cache existente)

```bash
cd ~/git/Fallout-ElderScrolls-macOS-Port
python3 tools/build.py game --game fallout3 \
  --generated "$HOME/Library/Caches/fnv-macos/translate-c3f97c2255fa/generated" \
  --exe "$HOME/Applications/Fallout 3.app/Contents/Resources/GameData/Fallout3.exe" \
  --codec-prefix "$HOME/Library/Caches/fnv-macos/codecs/prefix" \
  --unicorn-prefix /opt/homebrew/opt/unicorn \
  --cache "$HOME/Library/Caches/fnv-macos/objects-fallout3-c3f97c2255fa" \
  --out /tmp/rebuild --app-name "Fallout 3" --no-call-trace
cp -p "/tmp/rebuild/Fallout 3.app/Contents/MacOS/Fallout3" "$APP/Contents/MacOS/Fallout3"
codesign --force --deep --sign - "$APP"
```

### Lançar direto (sem launcher) com envs

```bash
open --env FNV_SKIP_LAUNCHER=1 --env FNV_DUMP_SHADERS=/tmp/shaders-dump3 -a "$APP"
# conferir env do processo:
ps eww -p "$(pgrep -x Fallout3)" | tr ' ' '\n' | grep FNV_
```

### Diagnósticos disponíveis no build de dump (stash)

- `FNV_DUMP_SHADERS=<dir>`: grava bytecode+MSL por shader (`ps_/vs_<hash>_m<major>_c<maxconst>_s<samplers>.{bin,msl}`).
- `FNV_NO_MIPS=1`: desliga mipmapping em todos os samplers (teste de band de mip).
- `FNV_TRACE_TEX=1`: log de TTFF/matrizes/draws (presente no working tree atual; inerte sem env).

### Estado do git ao encerrar

- Working tree: `d3d9.c` e `d3d9_metal.m` modificados com o **trace** (`FNV_TRACE_TEX`), sem
  efeito sem env.
- `git stash@{0}`: tentativa de texture transform (quebrada — **não reaplicar sem corrigir o
  bounds de sampler**).
- Report e mudanças **não commitados**.

---

## 12. Lições / armadilhas

- **Não confiar em “sumiu” visual**: água mais simples esconde as linhas. A/B precisa de
  câmera parada e cena idêntica.
- **Há dois INIs**: o port escreve `[water]` só no `FalloutPrefs.ini`; o `FALLOUT.INI` tem
  cópia das chaves e não é atualizado. Testar sempre nos dois.
- **Validar env**: usar `ps eww -p <pid>` — o `open --env` funciona, mas é fácil achar que um
  log vazio é resultado quando a env não chegou.
- **Dump do MSL é o caminho mais rápido** para entender um shader: `FNV_DUMP_SHADERS` +
  CTAB dos `.sdp` dá nomes de constantes e registradores.
- **Bounds**: indexar `txf[0][s]` com `s` até 15, mas só enviar 4 stages, quebra tudo
  silenciosamente (foi a causa da água invisible e do 1 fps).
- **Hipóteses precisam de teste que não altere o render** antes de virar código: o trace
  somente-leitura refutou a hipótese em uma rodada.

---

## 13. Referências cruzadas

- Shaders do jogo: `Data/Shaders/shaderpackage0NN.sdp` (dentro do app).
- Tradutor do port: `upstream/recomp/host/sm2msl.c` (D3D9 → MSL).
- Backend: `upstream/recomp/host/d3d9_metal.m`; shim: `upstream/recomp/host/d3d9.c`.
- Opções de água do port: `macos/OptionsFallout.m` (~865–910, presets; guarda em 744).
- Documentação de worldspaces/water mesh: <https://geck.uesp.net/wiki/Special:MobileDiff/1729>
- Mod “Water edges fix”: <https://www.nexusmods.com/fallout3/mods/23311>
- Thread ENB/displacement: <https://steamcommunity.com/app/22370/discussions/0/1738884444451857392>

---

*Relatório gerado em 2026-11-26 após encerramento da sessão de investigação. Nenhuma alteração
de renderização foi deixada ativa; o jogo está com o binário original e as mudanças estão no
working tree/stash conforme a seção 11.*

---

## 14. Resolução

### Causa raiz confirmada nos shaders dumpados e no backend Metal

Cruzando os 766 shaders dumpados em `/tmp/shaders-dump/` com os arquivos `.sdp` de Fallout 3:

1. **Pipeline de animação do `NoiseMap` (`s2`)**:
   - `ISNOISESCROLLANDBLEND` (`vs_7e32fb56_m1_c2_s00000000.msl` + `ps_a4ad2246_m2_c5_s00000001.msl`) faz scroll e mistura 3 camadas da textura de ruído em um RenderTarget `256×256` (`fTexScroll0/1/2`, `fTexScale`, `fAmplitude`).
   - `ISNOISENORMALMAP` (`vs_7e32fb56_m1_c2_s00000000.msl` + `ps_c78e98d0_m2_c3_s00000001.msl`) executa um filtro Sobel 3×3 (`±1/256`) sobre esse RT para gerar o `NoiseMap` (`s2`) tileável consumido por todos os shaders `WATER*.pso` (incluindo `WATER015.pso` quando todos os toggles de água estão desligados).
2. **Sinal invertido de `halfpixel` em `upstream/recomp/host/d3d9_metal.m`**:
   - `vs_7e32fb56` (assim como `vs_032d6f34` em `ISWATERDISPLACEMENT*` e outros 76 shaders `IS*`) já aplica o deslocamento D3D9 via `geometryOffset * (-2, +2)` = `(-1/W, +1/H)`.
   - Para compensar os centros de pixel inteiros do D3D9 em Metal (`+0.5 px`), o backend precisa somar `(+1/W, -1/H)`. Em `d3d9_metal.m`, isso estava restrito a `#ifdef FNV_GAME_SKYRIM`, enquanto o ramo `#else` (Fallout 3 e Fallout: New Vegas) aplicava o sinal invertido `(-1/W, +1/H)`, dobrando o erro para um pixel inteiro `(-2/W, +2/H)`.
   - Com esse deslocamento errado, a última coluna (`x = W-1`) e a última linha (`y = H-1`) nunca eram rasterizadas no passe de heightmap nem no passe Sobel, produzindo uma emenda estática de 3 texels nas bordas de `NoiseMap` que, repetida em *world space* (`WRAP`), formava o "X" congelado na água.

### Correção e verificação

- Teste A/B com `FNV_HALFPIXEL_FIX=1` confirmou o desaparecimento completo das linhas em grade na água.
- Removidas as instrumentações temporárias (`FNV_TRACE_TEX` em `d3d9.c`/`d3d9_metal.m` e `FNV_HALFPIXEL_FIX`) e unificado `vsys` em `upstream/recomp/host/d3d9_metal.m` para `{1.0f / W, -1.0f / H, 0, 0}` em todos os jogos.
- Validado com `tests/movie_draw_test.py` e rebuild/instalação limpa de `~/Applications/Fallout 3.app`.
