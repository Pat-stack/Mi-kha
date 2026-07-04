# ARCHITECTURE.md — Fork Mi-Kha (Kha + Kore absorbido)

> **Para quién es este documento**: una sesión nueva de un agente (o un humano) que necesita trabajar
> en este fork — diagnosticar bugs gráficos, aplicar parches al engine, o continuar trabajo pausado —
> sin el contexto de las sesiones anteriores. Léelo completo antes de tocar código. La sección
> "Metodología" es tan importante como la de arquitectura: documenta *cómo* se encontraron los bugs,
> no solo cuáles eran.
>
> **Ciclo de vida**: este doc se carga con **`/contexto`** al inicio de cada sesión y se actualiza
> con **`/handoff`** al final (analiza los cambios de la sesión — nunca el git diff — y documenta
> los que lo ameriten). Si trabajas sin esos comandos, cumple sus contratos a mano:
> `.claude/skills/{contexto,handoff}/SKILL.md`.

---

## 1. Qué es este repo y cuál es su relación con el juego

- **Este repo** (`~/Mi-Kha/Mi-kha`) es un **fork personal de largo plazo de [Kha](https://github.com/Kode/Kha)**,
  el framework multimedia de Haxe. Estrategia del fork: **absorber los submódulos como archivos
  regulares** en vez de forkear cada uno. `Kore/` (el runtime C, upstream lo llama Kinc/Kore) fue
  absorbido desde `Kode/Kore` rama `v2`, commit `2c6315d0`, en el commit local `0cdbecde`.
  Los `Tools/` (khamake, binarios de haxe/krafix por plataforma) y `Backends/Kore-hxcpp/khacpp`
  siguen siendo submódulos.
- **El juego** que consume este fork es **Vanguard Valiants**, en `~/stuff`. Es un juego 2D
  (con una escena 3D: el mapa del mundo) construido casi todo sobre **G2** con pipelines/shaders
  custom, más instanced rendering en la escena del mapa.
- **Cómo se compila el juego**: con la extensión Kha de VSCode. La ruta del SDK viene de
  `kha.khaPath` en `~/.config/Code/User/settings.json` y debe apuntar a `/home/patricio/Mi-Kha/Mi-kha`.
  ⚠️ **Gotcha histórico**: si esa ruta está mal, la extensión cae *en silencio* a otro checkout
  (p. ej. `~/Kha/Kha`, el upstream sin fixes). **Siempre verifica qué Kha usó un build** con
  `strings <binario> | grep <string de un fix tuyo>` o revisando rutas en
  `~/stuff/build/linux-build/Vanguard-Valiants/CMakeLists.txt`.
- **Target de producción actual: OpenGL con GLX en Linux** (ver §5). El backend Vulkan está en
  pausa con un bug conocido pendiente (ver §6.3).

## 2. Mapa del repo

```
Mi-Kha/Mi-kha/
├── Sources/                  # Kha en Haxe (API de alto nivel del framework)
│   ├── kha/graphics2/        # G2: API 2D inmediata (lo que más usa el juego)
│   ├── kha/graphics4/        # G4: API "moderna" (pipelines, buffers) + Graphics2.hx (impl de G2 sobre G4)
│   └── Shaders/              # painter-image/colored/text/video .vert/.frag (shaders internos de G2)
├── Backends/
│   └── Kore-hxcpp/           # Puente Haxe→C: kha.kore.* llama kinc_* vía @:functionCode
│       ├── main.cpp          # kickstart, "Starting application" = entra a kinc_start()
│       └── kha/graphics4/    # PipelineState.hx, Graphics.hx, etc. (¡setTexture(null) es NO-OP!)
├── Kore/                     # El runtime C (Kinc). TODO el trabajo de backend gráfico vive aquí
│   ├── kfile.js              # Config de build por plataforma/API: DEFINES viven aquí (KINC_GLX, etc.)
│   ├── Sources/kinc/         # Core agnóstico (graphics4/pipeline.c.h con defaults, vertexstructure.h…)
│   └── Backends/
│       ├── Graphics4/OpenGL/     # Backend GL directo (glue EGL/GLX incluido en OpenGL.c.h)
│       ├── Graphics4/G4onG5/     # Capa que implementa G4 sobre G5 (Vulkan/D3D12/Metal)
│       ├── Graphics5/Vulkan/     # Backend Vulkan (el más inmaduro; recibió la mayoría de los fixes)
│       └── System/Linux/         # Ventanas/input: x11/ y wayland/, dispatch runtime por procs
└── Tools/                    # khamake + binarios (haxe, krafix el compilador de shaders)
```

**Convención de compilación crítica**: cada backend es **una sola unidad de traducción**. Los `*.c.h`
se `#include`-an desde un único `*unit.c` (p. ej. `vulkanunit.c`, `linuxunit.c`, `g4ong5unit.c`,
`openglunit.c`). Consecuencias: los `static` son de facto globales del backend, el orden de includes
importa, y para verificar sintaxis compilas el `*unit.c`, no el `.c.h` suelto (ver §7.2).

## 3. La pila gráfica, de arriba a abajo

### 3.1 G2 (kha.graphics2) — donde vive el 90% del juego

`Sources/kha/graphics4/Graphics2.hx` implementa G2 sobre G4 con **tres painters batcheados**, cada
uno con su vertex buffer propio y su **estructura de vértices FIJA**:

| Painter | Estructura (nombre → tipo) | Stride |
|---|---|---|
| ImageShaderPainter | `vertexPosition` F32_3X, `vertexUV` F32_2X, `vertexColor` **UInt8_4X_Normalized** | **24** |
| ColoredShaderPainter | `vertexPosition` F32_3X, `vertexColor` UInt8_4X_Normalized | 16 |
| TextShaderPainter | `vertexPosition` F32_3X, `vertexUV` F32_2X, `vertexColor` **Float32_4X** | **36** |

**El contrato clave de G2 con pipelines custom** (`g2.pipeline = miPipeline`):
- El mismo pipeline se asigna a LOS TRES painters (`setPipeline`, Graphics2.hx ~línea 1101).
- Cada painter dibuja con SU buffer. El inputLayout del pipeline custom **debe describir el buffer
  del painter que lo va a usar** (para imágenes: la estructura de 24 bytes con esos nombres exactos;
  usar `Graphics2.createImageVertexStructure()`).
- En **GL esto es indulgente**: los atributos se toman del *buffer* al bindearlo, y
  `glBindAttribLocation` con un nombre inexistente es no-op. En **Vulkan el vertex fetch se hornea
  del inputLayout del pipeline** → cualquier discrepancia de stride/nombres = basura o crash.
  *Regla general: GL es la semántica de referencia; los demás backends deben emularla.*
- `SimplePipelineCache` hace `getTextureUnit("tex")` para todo pipeline custom; si el fragment shader
  no tiene un sampler llamado `tex`, la unit regresa con stages = -1 (no lanza excepción en hxcpp).
- ⚠️ `kha.kore.graphics4.Graphics.setTexture(unit, null)` es **NO-OP** (Graphics.hx:222-226): G2
  "des-bindea" texturas pero en realidad nunca se des-bindean. Las texturas persisten como en GL.

### 3.2 G4 → Kinc: el puente hxcpp

`Backends/Kore-hxcpp/kha/graphics4/*.hx` traduce llamadas Haxe a `kinc_g4_*` con `@:functionCode`.
`PipelineState.compile()` copia el estado Haxe al `kinc_g4_pipeline_t` y llama
`kinc_g4_pipeline_compile`. Los shaders llegan como blobs SPIR-V/GLSL ya compilados por krafix.

### 3.3 Los dos caminos de G4 en Linux

**Camino A — OpenGL directo** (`Backends/Graphics4/OpenGL/`): `OpenGL.c.h` (~1300 líneas) implementa
G4 con GL. El glue de contexto es por plataforma con `#ifdef`: `KINC_WINDOWS` (wgl), `KINC_EGL`,
y **`KINC_GLX`** (añadido en este fork, ver §5). En Linux los símbolos GL vienen de linkear `libGL`
(glew.c está excluido en kfile.js para Linux).

**Camino B — G4onG5** (`Backends/Graphics4/G4onG5/G4.c.h`): implementa G4 grabando en UN
`kinc_g5_command_list_t`. Detalles que hay que saber para no romperlo:

- **`current_state`**: espejo CPU de todo el estado (pipeline, buffers, texturas+units, viewport,
  scissor). Existe porque el command list se corta y hay que re-establecer estado. Se resetea en
  `kinc_g4_begin` (cada frame).
- **Constant buffers**: uno grande de `4096 × 100` slots; cada draw usa un slot (`startDraw` fija
  offsets dinámicos, `endDraw` avanza). Al agotarse los 100 (o con `waitAfterNextDraw`):
  **flush intra-frame** = end + execute + WAIT + begin + re-establecer TODO el estado (líneas
  ~188-263). Este flush es el único punto upstream que re-aplicaba texturas.
- **Vertex buffers dinámicos**: se crean ×500 copias; cada `lock` rota `_currentIndex` (offset =
  index × count al bindear). Buffers estáticos: cada lock fuerza `waitAfterNextDraw` (flush+wait).
- **Frame**: `kinc_g4_begin` → end+execute del command list "entre frames" → flip de
  `framebuffers[2]` → `kinc_g5_begin` (acquire) → begin → `endDraw(false)` (prepara slot 0).
  `kinc_g4_end` → end + execute + present + begin (para trabajo entre frames).
- **Serialización total CPU/GPU**: `kinc_g5_command_list_begin` espera el fence de la ejecución
  anterior. No hay frames en vuelo. (Techo de rendimiento estructural de este camino; también
  simplifica el razonamiento: al grabar el frame N+1, el N ya terminó en GPU.)

### 3.4 Backend Vulkan (Graphics5/Vulkan) — estado y contratos

Un solo archivo por tema (`Vulkan.c.h` init/swapchain/render passes, `pipeline.c.h` pipelines y
descriptors, `commandlist.c.h`, `rendertarget.c.h`, `texture.c.h`, …), todo en `vulkanunit.c`.

**Modelo de descriptors** (no es el típico de Vulkan, memorízalo):
- UN solo `VkDescriptorSetLayout` global de 18 bindings: `0`/`1` = STORAGE_BUFFER_DYNAMIC (uniforms
  de vertex/fragment con offsets dinámicos por draw), `2..17` = COMBINED_IMAGE_SAMPLER.
- **Convención de texture units**: binding SPIR-V `N` ⇒ unit/slot `N-2` (`get_texture_unit` regresa
  `number - 2`; los arrays globales `vulkanTextures[16]`/`vulkanRenderTargets[16]`/`vulkanSamplers[16]`
  se indexan por slot).
- Los sets se **cachean** por un id = `1 | (texture_count << 1) | (uniform << 8)` (`calc_descriptor_id`).
  Nota: el id NO distingue *qué* slots — solo cuántos. `reuse_descriptor_sets()` (al presentar)
  los marca reutilizables.
- Los writes de descriptors deben ser **compactos y con dstBinding real** (slots no contiguos
  existen: p. ej. shader con 2 samplers usa slots 0 y 1 = bindings 2 y 3).

**Parsing de SPIR-V en runtime** (`parse_shader` en pipeline.c.h): extrae por NOMBRE los locations
de inputs (OpName + OpDecorate Location), bindings de samplers y offsets del uniform buffer global
`_k_global_uniform_buffer_type`. `find_number` devuelve `(uint32_t)-1` si el nombre no existe —
**todo consumidor debe manejar ese -1** (fuente de dos crashes históricos, ver §6.1).

**Semántica de render targets tras los fixes de esta sesión** (NO revertir sin entender §6.2):
- Los RT de color viven **permanentemente en `VK_IMAGE_LAYOUT_GENERAL`** con `loadOp = LOAD`
  (passes single-target en Vulkan.c.h, MRT en commandlist.c.h, transición inicial en
  rendertarget.c.h). Esto emula la persistencia de contenidos de GL que G2 asume, y coincide con el
  `imageLayout = GENERAL` que los descriptors declaran al muestrear. Costo: sin DCC en AMD. Correcto > rápido.
- El pass del framebuffer (swapchain) también es `loadOp = LOAD`: hay un render pass vacío grabado
  entre `kinc_g4_end` y `kinc_g4_begin` que con DONT_CARE tenía licencia de corromper la imagen
  todavía en pantalla (parpadeo).
- `endPass()` en commandlist.c.h **borra** `vulkanTextures[]/vulkanRenderTargets[]` en cada cambio
  de render target; la contraparte `reapply_textures()` en G4.c.h los re-aplica desde `current_state`
  tras cada `set_render_targets`/`restore_render_target`.

### 3.5 krafix (compilador de shaders) — sus bugs conocidos

krafix (binario precompilado en `Tools/linux_x64`, fuera del alcance del fork) compila GLSL → SPIR-V
para Vulkan / GLSL para GL. Rarezas VERIFICADAS de su SPIR-V:

1. **Locations de vertex inputs asignados alfabéticamente contando cada input como 1 slot.**
   Un `mat4` consume 4 locations por spec, así que los inputs siguientes COLISIONAN con sus columnas
   (SPIR-V inválido). Workaround en el engine: `fix_mat4_input_locations()` (§6.1.3).
2. **Bindings de samplers también alfabéticos** por shader (p. ej. `col_map`=2, `tex`=3). Por eso el
   slot de una textura DEPENDE de los nombres de los demás samplers del shader.
3. `BufferBlock` sin decoraciones `Offset` cuando el uniform buffer contiene struct-arrays
   (VUID-08737, 9 errores de validación). RADV lo tolera. Sin síntoma conocido; pendiente reportar.
4. `bool` dentro del uniform buffer (inválido para spirv-val, tolerado por RADV).

### 3.6 Sistema Linux: x11/wayland, EGL/GLX

- `linuxunit.c` → `kinc_linux_init_procs()`: intenta Wayland, luego X11; llena la tabla `procs`
  (funcs.h). Con `KINC_NO_WAYLAND` solo compila/usa X11 (bajo sesiones Wayland corre vía XWayland,
  que es como Steam lanza juegos de todos modos).
- **Xlib se carga con dlopen** (tabla `xlib.` en x11.h, poblada con `LOAD_FUN` en x11/system.c.h).
  Si necesitas una función X nueva: añádela al struct en x11.h Y al listado LOAD_FUN. (Los símbolos
  `glX*` en cambio se linkean directo de libGL.)
- El backend instala un **X error handler no-fatal** (loguea y sigue) — los X errors no matan el
  proceso, pero ojo: pueden ser señal de bugs.
- **EGL vs GLX**: el glue EGL vive dentro de OpenGL.c.h (`kinc_egl_*`) y pide al windowing solo
  `kinc_egl_get_display/get_native_window`. El glue GLX de este fork vive en
  `x11/glcontext.c.h` (§5) con hooks `kinc_glx_*` declarados en OpenGL.c.h.

## 4. Shaders del juego y del framework

- Los shaders compilados del juego quedan en `~/stuff/build/linux-resources/*.{vert,frag}.spirv`
  (target Vulkan) o GLSL (target GL). Los fuente en `~/stuff/Shaders/*.glsl`.
- Inspección: `spirv-dis <archivo> | grep -E "OpName|Location|Binding"` — primera herramienta ante
  cualquier problema de atributos/samplers.
- El juego usa `Project.compilarShader(frag)`: pipelines custom para G2 = `painter_image_vert` +
  frag custom, dibujados por el image painter. Su inputLayout DEBE ser
  `Graphics2.createImageVertexStructure()` para el target Vulkan (en GL cualquier cosa "funciona",
  ver §3.1). *Nota: actualmente el juego declara una estructura manual stride-36 con nombres viejos
  (`texPosition`) — funciona en GL, y fue REVERTIDO a propósito a ese estado; si se retoma Vulkan,
  ese cambio del juego hay que rehacerlo (§6.3).*

## 5. Backend GLX (nuevo en este fork) — por qué y cómo

**Motivación**: el Steam Overlay y RenderDoc en Linux se enganchan a `glXSwapBuffers` (y
`vkQueuePresentKHR`). Desktop-GL sobre **EGL es invisible para ambos** — esa era la razón original
de intentar Vulkan. Restaurar GLX resolvió el objetivo real con el renderer ya probado.

**Implementación** (upstream eliminó GLX hace años; esto es re-implementación propia del fork):
- `Kore/Backends/System/Linux/Sources/kinc/backend/x11/glcontext.c.h` (nuevo): fbconfig 24/8 con
  fallback a 16, contexto vía `glXCreateContextAttribsARB` probando versiones 4.6→2.1 (con error
  handler X silenciado durante el probing — versiones no soportadas generan X errors), fallback a
  `glXCreateContext` legacy, vsync con `glXSwapIntervalEXT`.
- La ventana X11 se crea con **el visual del fbconfig** (hook en `x11/window.c.h` bajo `KINC_GLX`).
- Hooks en OpenGL.c.h paralelos a los de EGL: `kinc_glx_init/init_window/destroy_window/destroy/`
  `make_current/swap_buffers`.
- `Kore/kfile.js` (sección Linux OpenGL): define `KINC_GLX` + `KINC_NO_WAYLAND` (antes `KINC_EGL` + lib EGL).
- **Anti-sabotaje de GPU**: al inicio de `kinc_glx_choose_visual()` se hace `unsetenv("DRI_PRIME")`
  (con override `KINC_KEEP_DRI_PRIME=1`). Razón: el cliente Steam inyecta `DRI_PRIME=pci-…` en
  sistemas multi-GPU y puede elegir un GPU que NO maneja el display → cada frame viaja por una
  copia PRIME cross-GPU → **tearing que ningún vsync arregla** + bajón de fps. Para GL siempre
  queremos el GPU del display (= default de Mesa). También se loguea `GL_RENDERER` al arrancar —
  ante cualquier reporte gráfico raro, esa línea dice en qué GPU se está renderizando de verdad.

## 6. Fixes de esta sesión (2026-07-01 → 07-03), con su porqué

> Todos verificados en runtime salvo donde se indique. Los del backend Vulkan permanecen en el fork
> aunque el target activo sea GL — son correcciones de bugs reales de upstream.

### 6.1 Crashes del backend Vulkan (pipeline.c.h)

1. **Segfault en `vkCreateGraphicsPipelines` (RADV)** — atributo de vértice cuyo nombre no existe en
   el SPIR-V → `find_number` = -1 → `vi_attrs[].location = 0xFFFFFFFF` → RADV indexa fuera de rango.
   *Fix*: si el location no existe, **saltar el atributo pero seguir avanzando `offset` y `stride`**
   (el dato sigue físicamente en el buffer; los demás offsets deben quedar alineados), y asignar
   `vertexAttributeDescriptionCount = attr` DESPUÉS del loop con el conteo real. Emite warning con
   el nombre. Legal por spec: el pipeline puede omitir atributos que el shader no consume.
2. **SIGFPE en `vkCreateGraphicsPipelines`** — el switch de formatos no tenía caso
   `KINC_G4_VERTEX_DATA_F32_4X4` (mat4, usado por instancing) → format basura → división por
   block-size 0 en RADV. *Fix*: expandir mat4 en **4 atributos vec4 en locations consecutivos**
   (offsets +0/16/32/48); el pre-conteo del array cuenta 4 slots por mat4.
3. **SPIR-V inválido de krafix con mat4** (§3.5.1). *Fix*: `fix_mat4_input_locations()` — si el
   inputLayout trae mat4, re-asigna los locations de los inputs (orden de krafix preservado,
   reservando 4 slots por mat4) y **parchea las OpDecorate en el SPIR-V in-place** (`impl.source`
   es copia privada del shader — shader.c.h la malloc-ea; el parche es idempotente). Actualiza
   `vertexLocations` con set_number para que los vi_attrs coincidan. Validado con spirv-dis/spirv-val
   contra el shader real del juego.
4. **Escrituras OOB `vulkanTextures[-1]`** — `set_texture` y variantes escribían con unit -1 cuando
   el sampler no existe. *Fix*: guard `>= 0` en `set_texture`, `set_texture_from_render_target(_depth)`
   y `set_image_texture` (espejo del que ya tenía `set_sampler`).

### 6.2 Render roto en Vulkan (semántica de passes y descriptors)

5. **RTs negros / triángulo fantasma / parpadeo** — los render passes usaban
   `loadOp DONT_CARE` + `initialLayout UNDEFINED` (descarta contenidos en cada `setRenderTarget`;
   G2 compone capas re-entrando a los RTs) y el pass del framebuffer podía corromper la imagen
   presentada. *Fix*: la semántica descrita en §3.4 (GENERAL + LOAD en todo).
6. **Clear de depth inválido** — el juego hace `clear(…, 10.0)`; GL clampea, Vulkan no (VUID-00022,
   774 errores; en RADV = depth indefinido = fichas parpadeando). *Fix*: clamp [0,1] en
   `kinc_g5_command_list_clear`, igual que `glClearDepth`.
7. **Descriptors "never updated" (VUID-08114, 6.5k errores)** — tres defectos combinados: writes
   con count denso sobre slots no contiguos, gate `if (vulkanTextures[0])`, y el wipe de `endPass`
   sin re-aplicación fuera del flush de constant buffers. *Fix*: writes compactos con dstBinding
   real (en `getDescriptorSet`, `update_textures` y gemelas compute) + `reapply_textures()` en
   G4.c.h tras cada cambio de render target.

### 6.3 Estado del backend Vulkan al pausar (SI SE RETOMA, EMPEZAR AQUÍ)

- **Bug pendiente**: VUID-08114 residual (~1-2 draws/frame) en **binding 3** — draws del composite
  fullscreen (`renderFinal.frag`: samplers `col_map`=binding 2, `tex`=binding 3) donde el slot 1
  queda sin escribir. Síntoma: pantalla parpadeando negro↔gris. La investigación quedó en poner
  trazas en `kinc_g4_set_texture` / `endPass` / `startDraw` (ya retiradas) para ver la secuencia
  exacta de bindings alrededor de ese draw. Sospechas vivas: interacción del wipe de endPass con
  el orden `setTexture(col_map)` → `g2.begin()` (switch de RT) → draw, o el cache de descriptor
  sets reutilizando un set con binding sin escribir (el id no distingue slots, §3.4).
- **Fichas oscuras** en la escena del mapa (probablemente relacionado al anterior o a samplers).
- Requiere REHACER el cambio del juego: inputLayout de los pipelines G2 custom →
  `Graphics2.createImageVertexStructure()` (en `Project.hx` `compilarShader` y `Mundo.hx`
  `pipeline_add`; fue revertido al pausar Vulkan porque en GL no hace falta).
- El clear de depth 10.0 del juego (`Mapa_escena.hx:1302,1527`) quedó SIN tocar a propósito
  (el clamp del engine lo cubre).

### 6.4 GLX y GPU (ver §5)

8. Backend GLX completo + visual de ventana + `XSync` añadido a la tabla xlib.
9. `unsetenv("DRI_PRIME")` + log de `GL_RENDERER`.

## 7. METODOLOGÍA — cómo trabajar en este fork con eficacia

> Esta sección existe porque los bugs de esta sesión NO se resolvieron leyendo código y teorizando:
> se resolvieron con evidencia dura en cada paso. Sigue el mismo protocolo.

### 7.1 Reglas de oro

1. **Evidencia antes que teoría.** Cada hipótesis debe pagar con una verificación barata antes de
   escribir código. Los errores de esta sesión (exonerar al overlay de Steam prematuramente, un
   offset mal leído en gdb) vinieron de saltarse esto; los aciertos (DRI_PRIME, krafix, DONT_CARE)
   vinieron de instrumentar y mirar.
2. **GL es el contrato de referencia.** Si algo funciona en GL y falla en Vulkan, casi siempre el
   backend Vulkan viola una garantía implícita de GL (persistencia de contenidos, clamps, binding
   por nombre, tolerancia a atributos sin consumir). Arregla el backend para honrar el contrato;
   no fuerces al juego a adaptarse — G2 dicta estructuras y el juego no puede esquivarlas.
3. **`assert()` es no-op en release.** Kinc está plagado de `assert(!err)`. Un build release cruza
   los checks y crashea lejos de la causa. No confíes en que "si llegó aquí, era válido".
4. **Un fix por síntoma verificado, y re-corre.** Esta sesión fue una cebolla: 6+ bugs apilados,
   cada uno tapando al siguiente. Arregla, recompila, observa el síntoma NUEVO, repite. No intentes
   arreglar la cebolla entera de una teoría.
5. **Reporta con honestidad**: si un hallazgo previo tuyo resulta falso (pasó con un offset de gdb),
   corrígelo explícitamente ante el usuario.

### 7.2 Iteración rápida (no le pidas al usuario recompilar para probar ideas)

```bash
# Recompilar SOLO el runtime Kore del fork (segundos) usando el proyecto ninja ya generado:
cd ~/stuff/build/linux-build/release && ninja
cp Vanguard-Valiants ../../linux/           # el dir de deploy con los assets
cd ~/stuff/build/linux && ./Vanguard-Valiants   # correr (abre ventana en el display del usuario)
```
- El ninja compila los `*unit.c` directamente desde `~/Mi-Kha/Mi-kha/Kore/...` — tus ediciones al
  fork entran sin regenerar nada. (Cambios a `kfile.js` o listas de archivos SÍ requieren regenerar
  con khamake — eso hazlo/pídelo explícitamente.)
- Chequeo de sintaxis sin build completo (ajusta defines al backend):
```bash
F=~/Mi-Kha/Mi-kha/Kore
gcc -fsyntax-only -I$F/Sources -I$F/Backends/System/Linux/Sources -I$F/Backends/System/POSIX/Sources \
  -I$F/Backends/Graphics5/Vulkan/Sources -I$F/Backends/Graphics4/G4onG5/Sources \
  -DKINC_VULKAN -DKINC_G5 -DKINC_G4 -DKINC_G4ONG5 \
  $F/Backends/Graphics5/Vulkan/Sources/kinc/backend/graphics5/vulkanunit.c
# GL/GLX: -DKINC_OPENGL -DKINC_GLX -DKINC_NO_WAYLAND + openglunit.c / linuxunit.c
```
- Si añades instrumentación temporal (kinc_log "TRACE ..."), **retírala toda al terminar** y
  verifica con `grep -rn TRACE` que no quedó nada.

### 7.3 Caja de herramientas de diagnóstico (todas probadas en esta máquina)

- **gdb batch** para crashes (sin símbolos del binario igual sirve):
  `gdb -batch -ex run -ex bt -ex "info registers" ./Vanguard-Valiants`. Para inspeccionar argumentos
  de llamadas Vulkan: breakpoint en el símbolo (`break vkCreateGraphicsPipelines`), leer registros
  (rdi,rsi,rdx,rcx = args 1-4 en SysV) y desreferenciar structs por offset. ⚠️ Calcula offsets con
  cuidado y VERIFICA dos veces — un offset mal leído produjo un falso hallazgo en esta sesión.
- **Validation layers de Vulkan SIN sudo**:
  `apt-get download vulkan-validationlayers && dpkg -x *.deb extracted`, luego
  `VK_LAYER_PATH=extracted/usr/share/vulkan/explicit_layer.d LD_LIBRARY_PATH=extracted/usr/lib/x86_64-linux-gnu VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./juego`.
  Resume con `grep -o "VUID-[A-Za-z-]*" log | sort | uniq -c | sort -rn`. Un VUID = un contrato roto;
  léelo completo, dice exactamente qué objeto y por qué.
- **spirv-dis / spirv-val** sobre `~/stuff/build/linux-resources/*.spirv` para cualquier duda de
  locations/bindings/nombres. Compara SIEMPRE lo que el shader declara contra lo que el engine asume.
- **Proceso vivo**: `/proc/PID/environ` (tr '\0' '\n') y `/proc/PID/maps` responden "¿qué ambiente
  le puso Steam?" y "¿qué .so cargó de verdad?". Clave para el caso DRI_PRIME.
- **GPU y presentación**: `xrandr --listproviders` (quién maneja el display),
  `glxinfo | grep renderer` con/sin variables de ambiente (prueba A/B), `DRI_PRIME`, `vblank_mode`
  (Mesa; 3 = fuerza vsync — si el tearing sobrevive a vblank_mode=3, el problema es un path de
  COPIA, no de sincronización: los page-flips no pueden tearear, las copias sí).
- **Arqueología de upstream**: `git clone --filter=blob:none` de Kode/Kore; para buscar en historia
  usa `git rev-list -1 --before=FECHA HEAD` + `git ls-tree`/`git show COMMIT:ruta` (⚠️ `git log -S`
  en un clone sin blobs descarga uno por uno — no lo hagas).
- **¿Qué Kha usó este binario?**: `strings` buscando un mensaje de log tuyo (ver §1).

### 7.4 Trampas conocidas (no re-aprenderlas por las malas)

- Desactivar el overlay de Steam **no quita** el `LD_PRELOAD` de gameoverlayrenderer.so.
- El Ubuntu 24.04 del usuario tiene user namespaces restringidos (AppArmor) → pressure-vessel/bwrap
  de Steam falla ("bwrap: setting up uid map: Permission denied"). Pendiente del lado del usuario.
- La máquina es multi-GPU: **Intel Haswell (00:02.0) + RX 480 (01:00.0, maneja el display)**.
  Cualquier rareza gráfica: primero confirma en qué GPU está corriendo (log de GL_RENDERER).
- kfile.js corre wayland-scanner al generar; el `waylandunit.c` generado compila aunque uses
  `KINC_NO_WAYLAND` (es solo código de protocolo, peso muerto).
- Estilo del código Kinc: C, tabs, comentarios en inglés y solo para constraints que el código no
  puede expresar. Respeta el idioma del archivo.

## 8. Estado actual y trabajo futuro razonable

| Área | Estado |
|---|---|
| OpenGL/GLX Linux | ✅ Producción. Overlay Steam + RenderDoc funcionando. |
| Vulkan Linux | ⏸️ Pausado. Funcional hasta el composite; bug pendiente en §6.3. |
| krafix | Bugs documentados en §3.5; fix real requiere recompilar el binario o reportar a Kode. |
| Upstream | Los fixes de Vulkan (§6.1-6.2) son candidatos a PR a Kode/Kore si algún día interesa. |

**Posibles siguientes tareas** (por valor/costo): retomar el bug 08114 de Vulkan con el plan de
§6.3; `_NET_WM_BYPASS_COMPOSITOR` como blindaje extra de presentación; log de renderer equivalente
para el path Vulkan; reportar los bugs de krafix a upstream.
