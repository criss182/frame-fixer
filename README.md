# Frame Fixer (mod de Geode para Geometry Dash 2.2074)

Graba y reproduce macros de clicks por frame, con un offset global ajustable.

## Compilar
1. Instala Geode CLI y el SDK, y define la variable de entorno `GEODE_SDK`.
2. En esta carpeta:
   ```
   geode build
   ```
3. Se genera un archivo `.geode`; ponlo en la carpeta `geode/mods` de tu juego (o usa `geode package install`).

## Usar
- **Grabar:** activa "Grabar macro", juega y completa el nivel (o sal). Se guarda en la carpeta de guardado del mod (dentro de la carpeta `geode`, en la carpeta del mod; busca `macro.txt`).
- **Reproducir:** activa "Reproducir macro". Se cargan los clicks al entrar al nivel y los clicks manuales quedan bloqueados.
- **Corregir:** si tus clicks salen un poco antes o después, ajusta "Offset de frames" (por ejemplo -1 o +2) y vuelve a probar en practice mode.

## Formato de la macro
Una línea por evento: `frame down player`. Mira `macro-ejemplo.txt`.

## Notas
- Los frames son pasos de física del juego, no los FPS de la pantalla.
- Cambia `tuusuario` en `mod.json` por tu nombre.
- Úsalo para practicar. No envíes completados hechos con macros a leaderboards ni listas de demons.
