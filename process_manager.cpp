#include "process_manager.hpp"

#include <iostream>
#include <unistd.h>      // fork, pipe, read, write, close, usleep
#include <sys/wait.h>    // waitpid, WIFEXITED, WEXITSTATUS, WIFSIGNALED
#include <sys/types.h>
#include <signal.h>      // signal, SIGINT, SIGTERM, kill
#include <cstring>       // memset
#include <cstdlib>       // exit, rand, srand
#include <ctime>         // time
#include <cerrno>
#include <stdexcept>
#include <algorithm>

using namespace std;

// ─────────────────────────────────────────────
// Variable global para SIGINT (solo lectura fuera del handler)
// ─────────────────────────────────────────────
static volatile sig_atomic_t g_sigint_received = 0;

static void sigint_handler(int /*sig*/) {
    g_sigint_received = 1;
}

bool ProcessManager::sigint_received() {
    return g_sigint_received != 0;
}

// ─────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────
ProcessManager::ProcessManager(Scheduler& scheduler, int K)
    : sched_(scheduler), K_(K) {
    srand(static_cast<unsigned>(time(nullptr)));
}

// ─────────────────────────────────────────────
// Instalar manejadores de señal
// ─────────────────────────────────────────────
void ProcessManager::setup_signals() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;   // Reiniciar syscalls interrumpidas (p.ej. waitpid)
    if (sigaction(SIGINT, &sa, nullptr) == -1) {
        throw runtime_error("No se pudo instalar el handler de SIGINT");
    }
    // Ignorar SIGPIPE: si un proceso lector cierra su extremo antes de que el hijo escriba
    signal(SIGPIPE, SIG_IGN);
}

// ─────────────────────────────────────────────
// Duración efectiva
// ─────────────────────────────────────────────
int ProcessManager::effective_duration(const Activity& act) {
    if (act.duration_ms < 0) {
        // Aleatorio entre 100 y 5000 ms
        return 100 + rand() % (5000 - 100 + 1);
    }
    return act.duration_ms;
}

// ─────────────────────────────────────────────
// launch_one: crea el pipe, hace fork y ejecuta la actividad en el hijo
// ─────────────────────────────────────────────
pid_t ProcessManager::launch_one(const string& node_id) {
    Node& node = sched_.get_node(node_id);
    int dur    = effective_duration(node.activity);

    // Crear pipe padre↔hijo
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        cerr << "[ERROR] pipe() para \"" << node_id << "\": " << strerror(errno) << "\n";
        return -1;
    }
    int read_fd  = pipefd[0];   // Padre lee aquí
    int write_fd = pipefd[1];   // Hijo escribe aquí

    pid_t pid = fork();
    if (pid == -1) {
        cerr << "[ERROR] fork() para \"" << node_id << "\": " << strerror(errno) << "\n";
        close(read_fd);
        close(write_fd);
        return -1;
    }

    if (pid == 0) {
        // ── PROCESO HIJO ──────────────────────────────────────────────
        // El hijo no necesita el extremo de lectura
        close(read_fd);

        // Restaurar SIGINT al comportamiento por defecto en el hijo
        signal(SIGINT, SIG_DFL);

        // El hijo escribe su log a stderr para evitar interleaving con el stdout del padre
        cerr << "[INICIO] Actividad \"" << node.activity.name
             << "\" (ID=" << node_id << ", dur=" << dur << "ms)\n";
        cerr.flush();

        // Simular trabajo con usleep (sin busy-waiting)
        usleep(static_cast<useconds_t>(dur) * 1000);

        // Escribir mensaje de finalización al padre a través del pipe
        string msg = "OK:" + node_id + ":" + node.activity.name;
        ssize_t written = write(write_fd, msg.c_str(), msg.size());

        close(write_fd);

        if (written <= 0) {
            // Error al escribir → salida fallida
            exit(1);
        }

        cerr << "[FIN] Actividad \"" << node.activity.name
             << "\" (ID=" << node_id << ") completada.\n";
        cerr.flush();

        exit(0);
        // ── FIN PROCESO HIJO ─────────────────────────────────────────
    }

    // ── PROCESO PADRE ────────────────────────────────────────────────
    // El padre no necesita el extremo de escritura
    close(write_fd);

    // Registrar nodo como RUNNING
    sched_.mark_running(node_id, pid, dur, read_fd, /*write_fd=*/-1);
    pid_to_id_[pid] = node_id;

    cout << "[LANZADO] Actividad \"" << node.activity.name
         << "\" (ID=" << node_id << ", PID=" << pid << ", dur=" << dur << "ms)\n";
    cout.flush();

    return pid;
}

// ─────────────────────────────────────────────
// launch_ready: lanza tantos procesos como lo permita K
// ─────────────────────────────────────────────
int ProcessManager::launch_ready(const vector<string>& ready_ids, int running_count) {
    int launched = 0;
    for (const auto& id : ready_ids) {
        if (running_count + launched >= K_) {
            // Límite de concurrencia alcanzado: volver a encolar el nodo como READY
            // (se tomará en la siguiente iteración del bucle principal)
            sched_.get_node(id).state = NodeState::PENDING;
            // Trick: decrementamos pending_deps a 0 para que se re-encole al despertar
            // En realidad, simplemente lo guardamos en una cola auxiliar dentro del scheduler.
            // La forma más limpia: no sacamos el nodo de la ready_queue si no lo lanzamos.
            // Re-encolar directamente:
            // Nota: el scheduler ya sacó todos los ready de la cola. Si no los lanzamos,
            // los ponemos de vuelta como READY para que el bucle principal los reintente.
            sched_.get_node(id).state = NodeState::READY;
            // Reinyectar en la ready_queue interna del scheduler no es trivial desde aquí.
            // Lo resolvemos en main.cpp guardando los no-lanzados.
            break;
        }
        pid_t pid = launch_one(id);
        if (pid == -1) {
            // fork falló: tratar como fallo de la actividad
            cerr << "[ERROR] No se pudo lanzar la actividad \"" << id << "\"\n";
            sched_.mark_failed(id);
        } else {
            ++launched;
        }
    }
    return launched;
}

// ─────────────────────────────────────────────
// wait_for_any: espera a que cualquier hijo termine (SIN busy-waiting)
// ─────────────────────────────────────────────
WaitResult ProcessManager::wait_for_any() {
    if (pid_to_id_.empty()) return {"", false};

    int   status = 0;
    pid_t pid    = waitpid(-1, &status, 0);   // Bloquea hasta que un hijo termine

    if (pid == -1) {
        if (errno == EINTR) {
            // Interrumpido por señal (p.ej. SIGINT)
            return {"", false};
        }
        if (errno == ECHILD) {
            // No hay hijos: situación inesperada
            return {"", false};
        }
        cerr << "[ERROR] waitpid: " << strerror(errno) << "\n";
        return {"", false};
    }

    auto it = pid_to_id_.find(pid);
    if (it == pid_to_id_.end()) {
        // PID desconocido (no debería pasar)
        return {"", false};
    }

    string node_id = it->second;
    pid_to_id_.erase(it);

    // Cerrar el extremo de lectura del pipe del nodo
    Node& node = sched_.get_node(node_id);
    if (node.pipe.read_fd != -1) {
        // Leer el mensaje del hijo (hasta 256 bytes)
        char buf[256] = {};
        ssize_t n = read(node.pipe.read_fd, buf, sizeof(buf) - 1);
        close(node.pipe.read_fd);
        node.pipe.read_fd = -1;

        if (n > 0) {
            string msg(buf, static_cast<size_t>(n));
            cout << "[MENSAJE] Pipe de \"" << node_id << "\": " << msg << "\n";
            cout.flush();
        }
    }

    // Determinar éxito o fallo
    bool success = false;
    if (WIFEXITED(status)) {
        success = (WEXITSTATUS(status) == 0);
    } else if (WIFSIGNALED(status)) {
        // El hijo fue matado por señal → fallo
        success = false;
    }

    return {node_id, success};
}

// ─────────────────────────────────────────────
// kill_all_running: envía SIGTERM a todos los hijos activos
// ─────────────────────────────────────────────
void ProcessManager::kill_all_running() {
    for (auto& [pid, id] : pid_to_id_) {
        cout << "[KILL] Enviando SIGTERM a PID=" << pid
             << " (actividad \"" << id << "\")\n";
        kill(pid, SIGTERM);
    }
    // Recolectar hijos para evitar zombies
    while (!pid_to_id_.empty()) {
        int status = 0;
        pid_t pid  = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) {
            // Dar un poco de tiempo y reintentar
            usleep(10000);
            pid = waitpid(-1, &status, WNOHANG);
            if (pid <= 0) break;
        }
        pid_to_id_.erase(pid);
    }
    pid_to_id_.clear();
}
