---
name: handoff
description: Cierre de sesión del fork Mi-Kha — analiza los cambios hechos DURANTE ESTA SESIÓN de chat (no el git diff) y documenta en ARCHITECTURE.md los que lo ameriten, integrados a la estructura existente. Invocar al final de cada sesión de trabajo.
---

# /handoff — documentación de cierre de sesión

## Fuente de verdad: la sesión, NO el diff

Enumera los cambios repasando **esta conversación** (incluyendo su resumen si el contexto fue
compactado): qué archivos editaste tú, qué fixes aplicaste, qué se descubrió, qué se revirtió, qué
decisiones se tomaron y por qué.

⚠️ **PROHIBIDO derivar la lista de cambios de `git diff`/`git status`**: el working tree contiene
cambios del usuario ajenos a la sesión, y cambios de sesiones anteriores aún sin commitear. Git solo
puede usarse para *confirmar el detalle* de un cambio que YA sabes que hiciste (releer una función
que editaste), nunca para descubrir qué cambió.

## Qué merece entrar a ARCHITECTURE.md

Para cada cambio de tu lista, evalúa si una sesión futura lo necesita. Amerita documentarse:

- Fixes con causa raíz no obvia (síntoma → causa → fix → invariante a no romper).
- Cambios de **contratos o semántica** (formatos, layouts, orden de llamadas, defines de build).
- Descubrimientos: trampas nuevas (→ §7.4), herramientas/técnicas de diagnóstico nuevas (→ §7.3),
  hechos del entorno del usuario (hardware, Steam, sistema).
- Cambios de estado del proyecto: trabajo pausado/retomado, bugs pendientes con su plan (→ §6.x y §8).
- Correcciones al propio doc: si la sesión demostró que algo documentado es FALSO, corrígelo —
  un doc que miente es peor que uno incompleto.

NO amerita: experimentos descartados sin lección, instrumentación temporal, detalles que el código
ya expresa, cambios del usuario que tú no hiciste.

## Cómo documentar

1. **Lee ARCHITECTURE.md completo antes de editar** — necesitas su estructura y su nivel de detalle
   para juzgar la relevancia *relativa* de lo nuevo.
2. **Integra, no apiles**: cada entrada va en la sección que le corresponde (arquitectura §2-5,
   fixes §6, metodología/trampas §7, estado §8), al nivel de detalle de sus vecinas. El archivo es
   un **mapa curado, no una bitácora**: cada línea nueva compite por la atención de la siguiente
   sesión. Si algo nuevo deja obsoleta una entrada vieja, actualízala o elimínala en el mismo acto.
3. Mantén el estilo del doc: español, técnico en inglés, el "porqué" siempre presente, fechas
   absolutas (YYYY-MM-DD).
4. Si la sesión dejó trabajo a medias, actualiza (o crea) la sección de estado pausado con el punto
   exacto de reanudación: qué se sabe, qué sospechas siguen vivas, cuál era el siguiente paso.
5. Actualiza la tabla de estado (§8) si cambió.

## Cierre

- Verifica que no quedó instrumentación temporal tuya en el código (`grep -rn "TRACE" Kore/` sobre
  los backends tocados) ni archivos basura fuera del scratchpad.
- Reporta al usuario: qué documentaste (y dónde), qué decidiste NO documentar y por qué, y cualquier
  pendiente que la próxima sesión heredará.
- Si también mantienes memoria persistente propia, actualízala — pero ARCHITECTURE.md es la fuente
  de verdad compartida; la memoria solo apunta hacia él.
