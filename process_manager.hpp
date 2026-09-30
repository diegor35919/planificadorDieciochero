#pragma once
#include "scheduler.hpp"

#include <string>
#include <unordered_map>
#include <vector>

// Resultado devuelto por wait_for_any()
struct WaitResult {
    std::string node_id;   // ID del nodo que terminó
    bool        success;   // true → DONE, false → FAILED
};

// ProcessManager encapsula todas las syscalls POSIX:
//   fork, pipe, waitpid, kill, signal
class ProcessManager {
public:
    explicit ProcessManager(Scheduler& scheduler, int K);

    // Instala el manejador de SIGINT para inspección de la Seremi (Ctrl+C)
    void setup_signals();

    // Lanza hasta K procesos desde los nodos en estado READY.
    // Devuelve la cantidad de procesos efectivamente lanzados.
    int launch_ready(const std::vector<std::string>& ready_ids, int running_count);

    // Espera a que CUALQUIER hijo termine (sin busy-waiting).
    // Devuelve el resultado del hijo que terminó.
    // Retorna {"", false} si no hay hijos activos.
    WaitResult wait_for_any();

    // Mata todos los procesos en ejecución (usado en SIGINT y shutdown de emergencia)
    void kill_all_running();

    // Devuelve true si la señal SIGINT fue recibida
    static bool sigint_received();

private:
    Scheduler& sched_;
    int        K_;          // Límite de concurrencia

    // Mapa pid → node_id para lookup rápido en wait_for_any
    std::unordered_map<pid_t, std::string> pid_to_id_;

    // Calcula la duración efectiva: si act.duration_ms == -1 → aleatorio [100, 5000]
    static int effective_duration(const Activity& act);

    // Lanza el proceso hijo para una actividad
    // Devuelve el pid del hijo, o -1 en caso de error de fork
    pid_t launch_one(const std::string& node_id);
};
