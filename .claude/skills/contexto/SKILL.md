---
name: contexto
description: Carga el contexto del fork Mi-Kha al inicio de la sesión leyendo ARCHITECTURE.md. Invocar al comenzar cualquier sesión de trabajo sobre Kha/Kore o el juego.
---

# /contexto — carga de contexto de sesión

Ejecuta estos pasos, en orden, antes de cualquier otro trabajo:

1. **Lee COMPLETO** `ARCHITECTURE.md` en la raíz del repo (`/home/patricio/Mi-Kha/Mi-kha/ARCHITECTURE.md`).
   No lo hojees: las secciones de contratos (§3) y metodología (§7) son las que evitan repetir
   errores ya pagados. Si el archivo no existe, dilo de inmediato — es una anomalía.

2. **Verificación de frescura** (barata, sin profundizar):
   - `git -C /home/patricio/Mi-Kha/Mi-kha log --oneline -5` y `git status --short` — para saber si
     hay trabajo no documentado posterior a la última actualización del doc.
   - Si ves commits o cambios grandes que ARCHITECTURE.md no refleja, adviértelo al usuario: el doc
     puede estar desactualizado y un `/handoff` quedó pendiente.

3. **Resume al usuario en máximo 5 líneas**: target de producción actual, trabajo pausado y su
   estado, bugs/pendientes conocidos, y cualquier alerta de frescura del paso 2. Nada más — no
   propongas trabajo, no expliques la arquitectura; el resumen es una confirmación de que el
   contexto está cargado.

4. A partir de aquí, **aplica la metodología de §7 del doc por defecto** en todo el trabajo de la
   sesión (evidencia antes que teoría, iteración con ninja, verificación de qué Kha usó cada build,
   limpieza de instrumentación temporal).
