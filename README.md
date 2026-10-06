# Planificador Dieciochero
**Tarea 1 — Procesos, Tuberías y Señales | Sistemas Operativos**

Simulador y planificador de actividades basado en un Grafo Acíclico Dirigido (DAG),
implementado en **C++17** usando exclusivamente llamadas al sistema POSIX:
`fork`, `pipe`, `waitpid`, `kill` y `signal/sigaction`.

---

## Compilación

```bash
make          # compila con g++ -Wall -Wextra -std=c++17 -lpthread
make clean    # elimina binarios y objetos
make run      # compila y ejecuta con plan.txt K=3
```

## Ejecución

```bash
./planificador <plan.txt> <K>
```

- `plan.txt` — archivo de planificación (ver formato abajo)
- `K` — límite de concurrencia (máximo de procesos simultáneos)

**Ejemplo:**
```bash
./planificador plan.txt 3
```

---

## Formato de `plan.txt`

```
ID_Actividad : Nombre_Actividad : tiempo_ms : Dep1, Dep2, ...
```

- `ID_Actividad`: identificador único alfanumérico.
- `Nombre_Actividad`: etiqueta descriptiva.
- `tiempo_ms`: duración en ms. Si se omite o es inválido → aleatorio entre 100 y 5000 ms.
- `Dependencias`: lista de IDs separados por coma. Pueden estar vacías o con `[]`.
- Las líneas que comienzan por `#` son comentarios y se ignoran.

**Ejemplo:**
```
1 : prender_carbon  : 500  :
2 : comprar_carne   : 1200 :
3 : comprar_pan     : 300  :
4 : asar_longaniza  : 800  : 1, 2
5 : armar_choripan  : 250  : 3, 4
6 : servir_mesa     : 100  : 5
```

**Duración aleatoria (campo vacío):**
```
1 : a : :
2 : b : : 1
```
Ambas líneas son válidas: `a` tiene duración aleatoria sin dependencias; `b` tiene duración aleatoria y espera a `a`.

---

## Arquitectura Modular

```
planificadorDieciochero/
├── parser.hpp / parser.cpp          → Lectura y parseo de plan.txt
├── scheduler.hpp / scheduler.cpp    → DAG, Ready Queue y lógica de dependencias
├── process_manager.hpp / .cpp       → Syscalls POSIX (fork, pipe, waitpid, kill, signal)
├── main.cpp                         → Bucle principal que integra todo
├── plan.txt                         → Ejemplo de plan
└── Makefile
```

### `parser` — Lectura de archivos
Lee `plan.txt` línea a línea. Separa los campos por `:` **preservando campos vacíos**,
lo que permite duraciones omitidas (`1 : a : : dep`). Valida la duración con `strtol`
para detectar valores no numéricos con precisión. Retorna un `vector<Activity>`.

### `scheduler` — Grafo DAG + Ready Queue
- Construye el grafo de dependencias: aristas directas (deps) e inversas (sucesores).
- Detecta ciclos con DFS antes de iniciar; lanza excepción si los hay.
- Mantiene una **cola FIFO** (`ready_queue_`) con nodos listos para ejecutar.
- `mark_done(id)`: decrementa `pending_deps` de los sucesores; los que llegan a 0 se encolan.
- `mark_failed(id)`: propaga `ABORTED` en cascada a todos los descendientes del nodo fallido.
- `has_pending()`: O(1) mediante contador `pending_count_` (evita recorrer todos los nodos).
- Cada nodo almacena `completion_msg`: el mensaje `OK:<id>:<nombre>` que se enviará a sus dependientes.

### `process_manager` — Syscalls POSIX

Encapsula `fork`, `pipe`, `waitpid`, `kill` y `sigaction`.

**Diseño de pipes (criterio 2.2):**
Cada actividad usa **dos pipes**:
1. `pipe_result` (hijo → padre): el hijo escribe `"OK:<id>:<nombre>"` al terminar.
2. `pipe_deps` (padre → hijo): el padre escribe los mensajes `OK` de todas las dependencias
   ya completadas, concatenados por `|`. El hijo los lee **antes de empezar** (`usleep`),
   registrándolos con `[INSUMOS]`.

Así cuando `asar_longaniza` arranca, ya sabe que `prender_carbon` y `comprar_carne` terminaron OK.

**Señales (criterio 2.1):**
- `SIGINT` (Ctrl+C) activa `g_sigint_received`. Sin `SA_RESTART`, `waitpid()` retorna
  `EINTR` inmediatamente y el bucle principal detecta la señal y mata todo.
- `kill_all_running()` envía `SIGTERM` a cada hijo y luego espera con `waitpid` bloqueante
  en loop hasta recolectar **todos** (sin zombies).

**Estrés / file descriptors (criterio 2.4):**
- En `setup_signals()` se llama a `setrlimit(RLIMIT_NOFILE)` para ampliar el límite al máximo.
- Si `pipe()` o `fork()` fallan con `EMFILE`, `launch_one` retorna `-2` (diferir) en vez de
  `-1` (fallo). El bucle principal re-encola el nodo como diferido para reintentarlo.

**Salida duplicada (criterio 1.3):**
- El padre llama a `fflush(stdout)` antes de cada `fork()`.
- El hijo usa `_exit()` en lugar de `exit()` para no vaciar los buffers heredados.

### `main` — Bucle principal
Controla el flujo con invariante `running_count`:
- Mezcla nodos diferidos y nuevos `READY` en cada iteración.
- Respeta el límite `K` de concurrencia.
- Espera con `waitpid` bloqueante (sin busy-waiting) a que termine un hijo.
- Ante `SIGINT`, mata todos los hijos y retorna código 130.

---

## Decisiones de Diseño

### Modelo de datos: `pending_deps` + `pending_count_`
Cada nodo lleva un contador de dependencias pendientes. Cuando llega a 0, pasa a READY.
`pending_count_` es un contador global de nodos no terminados; permite `has_pending()` en O(1),
evitando el O(N²) que resultaría de recorrer todos los nodos en cada vuelta del bucle principal.

### Detección de ciclos previa a la ejecución
El DAG se valida con DFS antes de lanzar cualquier proceso. Si hay un ciclo,
el programa termina con un mensaje claro en lugar de colgarse indefinidamente.

### Propagación de fallos en cascada
`abort_subtree()` recorre recursivamente los sucesores de un nodo fallido
y los marca como ABORTED, aislando el error sin afectar ramas independientes.

### Sin busy-waiting
El padre solo llama a `waitpid(-1, &status, 0)` (bloqueante) cuando hay hijos activos.
No hay ningún `sleep()` ni polling en el bucle principal.
