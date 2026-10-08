# 🐧 Pingu Player

**Reproductor de audio en línea de comandos con gestión concurrente de la lista de reproducción**
Parcial 2 — Sistemas Operativos (EAFIT) · Alternativa 2

**Autores:** Yesid Hurtado Montoya · Juan Fernando Duque

```
      .--.
     |o_o |    PINGU PLAYER
     |:_/ |    reproductor concurrente con playlist y mezclador
    //   \ \   Parcial 2 - Sistemas Operativos - EAFIT
   (|     | )
  /'\_   _/`\
  \___)=(___/
```

---

## 1. ¿Qué es?

Pingu Player es un reproductor de audio en C (POSIX/Linux) cuyo objetivo principal no es
reproducir "bonito", sino **demostrar un diseño concurrente correcto**: el motor de
reproducción está desacoplado de la lista de reproducción y de la interfaz, y todos los
componentes se comunican usando únicamente primitivas explícitas de `pthread`
(mutex, variables de condición y `pthread_rwlock`). No usamos ninguna librería de alto nivel que
abstraiga la sincronización.

Funcionalidades:

- Play, Pause, Resume, Stop, Next y Previous con respuesta inmediata.
- Lista de reproducción modificable **mientras suena la música**: agregar, eliminar (incluso la pista
  que está sonando), reordenar, vaciar y consultar.
- **Mezclador de canciones**, algoritmo Fisher–Yates, en dos formas:
  - `shuffle`: mezcla la lista una vez. Si hay música sonando, esa pista queda de primera para que no haya cortes de audio;
    si está detenido, la mezcla es total (la primera pista también es aleatoria).
  - `shuffle on|off`: **modo mezcla permanente**. Mezcla ahora, las canciones que agregues después se insertan en
    posiciones aleatorias y, al terminar la lista (con `repeat all` o al volver a empezar), se vuelve a mezclar sin repetir
    la última pista como primera. También está disponible al arrancar con `--shuffle`.
- Modos de repetición: `off`, `one`, `all`.
- Interfaz CLI con **pingüino** que cambia de cara según el estado (reproduciendo ♪, en pausa 💤, detenido).
- Formatos: WAV PCM 16 bit / 44.1 kHz (mono o estéreo) se decodifica **nativamente en C**; el resto de
  formatos (mp3, flac, ogg, m4a…) se decodifican con `ffmpeg` como proceso hijo si está instalado.

---

## 2. Arquitectura

```
                       ┌─────────────────────────────────────────┐
  teclado ───────────► │  Hilo UI / comandos (main)              │
                       │  play pause next add rm mv shuffle ...  │
                       └───────┬─────────────────────┬───────────┘
                               │ rwlock (escritor)   │ mutex + cond (estado)
                               ▼                     ▼
                       ┌──────────────┐      ┌──────────────┐
                       │  PLAYLIST    │      │   Player     │
                       │ (rwlock)     │      │ state/quit   │
                       └──────┬───────┘      └──────────────┘
                              │ rwlock (lector)
                              ▼
 archivo ─► ┌─────────────────────────┐  rb_write   ┌──────────────┐  rb_read   ┌────────────────────────┐
 de audio   │ Hilo DECODIFICADOR      │ ──────────► │ BÚFER        │ ─────────► │ Hilo SALIDA            │ ─► aplay / paplay
            │ (PRODUCTOR)             │             │ CIRCULAR     │            │ (CONSUMIDOR)           │    (tarjeta de sonido)
            └─────────────────────────┘             │ mutex + 2 cv │            └────────────────────────┘
                                                    └──────────────┘
```

| Módulo | Archivos | Responsabilidad |
|---|---|---|
| Búfer circular | `src/ringbuf.[ch]` | Cola productor/consumidor sincronizada, con "generaciones" para descartar audio viejo |
| Playlist | `src/playlist.[ch]` | Estructura compartida (Lectores–Escritores) con `pthread_rwlock` |
| Motor | `src/player.[ch]` | Hilos decodificador y de salida, máquina de estados, comandos |
| Decodificación | `src/source.[ch]`, `src/spawn.[ch]` | WAV nativo y puente a `ffmpeg` |
| Salida de audio | `src/audio_out.[ch]` | Envía PCM a `aplay`/`paplay` (o sumidero nulo para pruebas) |
| Interfaz | `src/ui.[ch]`, `src/main.c` | CLI, pingüino, interpretación de comandos |

Separación de responsabilidades: la UI **nunca** toca el audio ni el búfer directamente; solo llama a la API
de `Player`. El motor **nunca** imprime directamente; avisa a la UI por un callback.

---

## 3. Protocolo de concurrencia

### 3.1 Hilos

| Hilo | Rol | Se bloquea cuando… |
|---|---|---|
| UI (main) | Lee comandos y llama a la API del `Player` | espera una línea en `stdin` |
| Decodificador | **Productor**: lee la pista actual y escribe en el búfer | el búfer está lleno, o no hay nada que reproducir |
| Salida | **Consumidor**: saca bytes del búfer y los manda al reproductor de audio | el búfer está vacío, o está en pausa |

### 3.2 Búfer circular (productor–consumidor)

- Un `pthread_mutex_t` y dos variables de condición: `not_full` (el productor espera aquí si el búfer
  está lleno) y `not_empty` (el consumidor espera aquí si está vacío).
- **Sin espera activa:** siempre se usa `pthread_cond_wait` dentro de un `while` que reevalúa el predicado
  (protege contra despertares espurios).
- **Underrun evitable:** el decodificador lee mucho más rápido que el tiempo real, así que el búfer
  (128 KiB ≈ 0,74 s) se mantiene lleno. Se cuentan los underruns (`status`) para comprobarlo.
- **Generaciones (`gen`):** cada `rb_flush()` (Next, Prev, Stop, borrar la pista actual, `clear`) vacía
  el búfer e incrementa `gen`. `rb_write()` rechaza las escrituras de una generación vieja y el hilo de
  salida descarta cualquier trozo leído de una generación vieja. Así, tras un Next **nunca** se oye un
  retazo de la pista anterior y la respuesta es inmediata, sin tener que "esperar a que se vacíe".

### 3.3 Playlist (Lectores–Escritores)

- Un único `pthread_rwlock_t` protege el arreglo de pistas, el índice de la pista actual y el modo de repetición.
- **Lectores** (varios simultáneos): `pl_current`, `pl_peek_next`, `pl_snapshot`, `pl_count`.
- **Escritores** (exclusivos): `pl_add`, `pl_remove`, `pl_move`, `pl_shuffle`, `pl_clear`, `pl_goto`, `pl_step`, `pl_advance_from`.
- El `rwlock` se crea con `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP` para que el flujo continuo de
  lecturas del decodificador **no deje en inanición** a las modificaciones del usuario.
- **Nunca se entregan punteros internos**, solo copias (`strdup`). Por eso el decodificador puede estar
  leyendo "la pista actual" mientras el usuario la elimina: no hay accesos a memoria liberada.
- **Ids únicos por pista:** `pl_advance_from(id)` solo avanza si la pista actual sigue siendo `id`. Si
  el usuario cambió de pista justo cuando terminaba la anterior, el avance se ignora (no se salta una pista de más).

### 3.4 Estado y señalización

- `Player.mtx` + `Player.cv` protegen el estado (`STOPPED/PLAYING/PAUSED`) y la bandera de cierre.
- **Pause:** el hilo de salida se duerme en `cond_wait` mientras el estado es `PAUSED`; el decodificador
  sigue llenando el búfer hasta que se llena y se bloquea solo. **Resume** hace `broadcast` y se continúa
  exactamente donde se quedó.
- **Stop:** primero se cambia el estado a `STOPPED` y *después* se hace el flush; el decodificador toma
  la generación **dentro** del mutex mientras comprueba el estado, por lo que no puede quedar reproduciendo
  tras un Stop (se evita la carrera "decodificador arranca justo cuando se detiene").
- **Cierre ordenado:** `quit=1` + `broadcast`, `rb_close()` (despierta productor y consumidor),
  se termina el proceso `aplay` (desbloquea un `write` pendiente), `pthread_join` de ambos hilos y liberación de recursos.

### 3.5 Jerarquía de bloqueos (prevención de deadlocks)

```
Player.mtx  ──►  Playlist.rwlock          (siempre en este orden)
RingBuf.mtx = hoja (dentro de él no se toma ningún otro bloqueo)
```

Los comandos de la UI usan la playlist y el búfer **sin** sostener `Player.mtx`; solo lo toman para
cambiar el estado. Los callbacks hacia la UI nunca se invocan con un bloqueo del motor tomado. Un único
mutex serializa la impresión en pantalla para que los avisos de los hilos no se mezclen con la salida de comandos.

### 3.6 Escenarios de carrera analizados

| Escenario | Qué evita que falle |
|---|---|
| `next` mientras el decodificador está escribiendo | `gen` cambia; `rb_write` rechaza el trozo viejo |
| `rm` de la pista que suena | `pl_remove` informa `was_current`; se hace flush; el decodificador (que tenía una *copia* de la ruta) aborta y relee la pista actual |
| `rm`/`mv`/`shuffle`/`add` en modo mezcla mientras se evalúa la siguiente pista | Todo cambio ocurre bajo el `wrlock`; el decodificador siempre ve una lista consistente |
| Fin de pista justo cuando el usuario pulsa `next` | `pl_advance_from(id)` devuelve "obsoleto" y no avanza otra vez |
| `add` con la lista vacía y el decodificador dormido | `add` toma `Player.mtx` y hace `broadcast`; el decodificador revisa el predicado dentro del mutex → no hay *lost wakeup* |
| `stop` justo cuando el decodificador arranca una pista | Gen. tomada dentro del mutex (ver 3.4) |
| `shuffle` con música sonando | La pista actual pasa a la posición 0 sin flush → no hay corte de audio |
| Fin de la lista en modo mezcla | El re-mezclado ocurre dentro de `pl_advance_from` bajo el `wrlock`, junto con el avance: es una sola operación atómica |

---

## 4. Requisitos

- Ubuntu (probado con GCC 13 / Ubuntu 24.04), `gcc`, `make`.
- Para escuchar sonido: `aplay` (paquete `alsa-utils`) o `paplay` (`pulseaudio-utils`).
- Opcional, para mp3/flac/ogg/m4a: `ffmpeg`.

```bash
sudo apt update
sudo apt install -y build-essential alsa-utils ffmpeg
```

## 5. Compilar y ejecutar

```bash
make                # compila build/pingu y build/gentone
make samples        # genera 5 pistas WAV de ejemplo en samples/
./build/pingu samples/          # abre el reproductor con esa carpeta
```

Atajo: `make run` (compila, genera ejemplos y arranca con `--shuffle --play`).

Opciones de arranque:

```
./build/pingu [--backend auto|aplay|paplay|null] [--shuffle] [--repeat off|one|all] [--play] [rutas...]
```

`--backend null` ejecuta todo (hilos, búfer, comandos) con temporización real pero **sin sonido**; sirve
en máquinas virtuales sin tarjeta de sonido.

## 6. Comandos

| Comando | Acción |
|---|---|
| `play [n]` | Reproduce (desde la pista *n*) o reanuda |
| `pause` / `resume` | Pausa / reanuda |
| `stop` | Detiene |
| `next` / `prev` | Pista siguiente / anterior (circular) |
| `add <ruta…>` | Agrega archivos o carpetas (admite `*`, `~` y `"rutas con espacios"`) |
| `addir <carpeta>` | Agrega todo el audio de una carpeta |
| `ls` | Muestra la lista (► sonando, `|` en pausa, `*` seleccionada) |
| `rm <n>` | Elimina la pista *n* |
| `mv <de> <a>` | Mueve una pista de posición |
| `shuffle` | **Mezcla** la lista una vez |
| `shuffle on\|off` | Activa / desactiva el **modo mezcla permanente** |
| `clear` | Vacía la lista |
| `repeat off\|one\|all` | Modo de repetición |
| `status` | Estado, bufer, underruns, pingüino |
| `penguin` | El pingüino saluda 🐧 |
| `help` / `quit` | Ayuda / salir |

## 7. Pruebas

```bash
make check     # pruebas unitarias + estrés + prueba de extremo a extremo
make tsan      # lo mismo con ThreadSanitizer (carreras de datos)
make asan      # lo mismo con AddressSanitizer + UBSan (+ detector de fugas)
```

- `tests/test_ringbuf.c`: productor, consumidor lento y un hilo que hace `flush` al azar; verifica que
  no se pierde, duplica ni mezcla ningún byte entre generaciones.
- `tests/test_playlist.c`: pruebas deterministas de la lógica de índices y estrés con 4 escritores y 4 lectores
  simultáneos (millones de operaciones).
- `tests/smoke.sh`: lanza el reproductor completo y le manda play/pause/next/shuffle/mv/rm/clear mientras suena.

## 8. Cómo cumple la rúbrica

| Criterio | Dónde |
|---|---|
| Gestión concurrente de playlist | `src/playlist.c` (rwlock, copias, ids) · sección 3.3 |
| Búfer de audio productor–consumidor | `src/ringbuf.c` (mutex + 2 cv, sin busy-wait) · sección 3.2 |
| Control asíncrono de eventos | `src/player.c` (estado + `cond_broadcast`, flush por generación) · sección 3.4 |
| Robustez, sin deadlocks ni fugas | Jerarquía de bloqueos (3.5), cierre ordenado, `make tsan` / `make asan` |
| Diseño modular y buenas prácticas | Módulos separados (sección 2), protocolo documentado (sección 3) |

## 9. Limitaciones conocidas

- El nombre que muestra `status` es el de la pista que se está **decodificando**; como el búfer guarda
  ≈ 0,7 s por delante, el cambio de nombre se adelanta ese tiempo respecto a lo que se oye.
- La latencia de pausa/skip incluye el búfer interno de `aplay` (configurado a 100 ms) más un trozo de ~23 ms.
- Solo se decodifica nativamente WAV PCM 16 bit / 44.1 kHz; el resto depende de `ffmpeg`.
- No hay *seek* (adelantar/retroceder dentro de una pista) ni control de volumen.

## 10. Estructura del proyecto

```
pingu-player/
├── Makefile
├── README.md
├── src/        ringbuf, playlist, player, source, spawn, audio_out, ui, main
├── tools/      gentone.c   (generador de WAV de prueba)
└── tests/      test_ringbuf.c  test_playlist.c  smoke.sh
```
