# Planificador Dieciochero 🎉
**Tarea 1 — Procesos, Tuberías y Señales | Sistemas Operativos**

Simulador y planificador de actividades basado en un Grafo Acíclico Dirigido (DAG),
implementado en **C++17** usando exclusivamente llamadas al sistema POSIX:
`fork`, `pipe`, `waitpid`, `kill` y `signal/sigaction`.

---

## Compilación

```bash
make          # compila con g++ -Wall -Wextra -std=c++17
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
ID_Actividad : Nombre_Actividad : tiempo_ms : [Dep1, Dep2, ...]
```

- `ID_Actividad`: identificador único alfanumérico.
- `Nombre_Actividad`: etiqueta descriptiva.
- `tiempo_ms`: duración en ms. Si se omite o es inválido → aleatorio entre 100 y 5000 ms.
- `Dependencias`: lista de IDs separados por coma. Pueden estar vacías.
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
Lee `plan.txt` línea a línea, parsea los 4 campos separados por `:`, maneja duraciones
omitidas (`-1`) y retorna un `vector<Activity>`.

### `scheduler` — Grafo DAG + Ready Queue
- Construye el grafo de dependencias: aristas directas (deps) e inversas (sucesores).
- Detecta ciclos con DFS antes de iniciar; lanza excepción si los hay.
- Mantiene una **cola FIFO** (`ready_queue_`) con nodos listos para ejecutar.
- `mark_done(id)`: decrementa `pending_deps` de los sucesores; los que llegan a 0 se encolan.
- `mark_failed(id)`: propaga `ABORTED` en cascada a todos los descendientes del nodo fallido.

### `process_manager` — Syscalls POSIX
*(pendiente — módulo a cargo del compañero)*

### `main` — Bucle principal
*(pendiente — módulo a cargo del compañero)*

---

## Decisiones de Diseño

### Modelo de datos: `pending_deps`
Cada nodo lleva un contador de cuántas dependencias faltan por completarse.
Cuando llega a 0, el nodo pasa a READY y se encola. Esto evita recorrer
el grafo completo en cada iteración.

### Detección de ciclos previa a la ejecución
El DAG se valida con DFS antes de lanzar cualquier proceso. Si hay un ciclo,
el programa termina con un mensaje claro en lugar de colgarse indefinidamente.

### Propagación de fallos en cascada
`abort_subtree()` recorre recursivamente los sucesores de un nodo fallido
y los marca como ABORTED, aislando el error sin afectar ramas independientes.

## Process Manager y Main

Aquí agregamos la ejecución de las actividades:
- **process_manager:** Hace el trabajo de los procesos. Lanza las tareas, les asigna un tiempo al azar si no tienen, revisa los pipes para los mensajes de confirmación y ataja el Ctrl+C (la Seremi) para cancelar todo si es necesario.
- **main:** Controla el flujo. Revisa el límite K, encola las tareas que están listas y se asegura de no pasarse del límite de procesos corriendo al mismo tiempo.
