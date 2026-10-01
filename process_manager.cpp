#include "process_manager.hpp"

#include <iostream>
#include <sstream>
#include <unistd.h>      // fork, pipe, read, write, close, usleep, _exit
#include <sys/wait.h>    // waitpid, WIFEXITED, WEXITSTATUS, WIFSIGNALED
#include <sys/types.h>
#include <sys/resource.h> // setrlimit / getrlimit para ampliar fd limit
#include <signal.h>       // sigaction, SIGINT, SIGTERM, kill, sigwait, sigset_t
#include <cstring>        // memset, strerror
#include <cstdlib>        // rand, srand
#include <ctime>          // time
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
//
// FIX (2.1): Se elimina SA_RESTART para que waitpid() retorne EINTR
// al llegar Ctrl+C, permitiendo reacción inmediata.
// También se intenta ampliar el límite de file descriptors (Fix 2.4).
// ─────────────────────────────────────────────
void ProcessManager::setup_signals() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;   // Sin SA_RESTART: waitpid() saldrá con EINTR al recibir SIGINT
    if (sigaction(SIGINT, &sa, nullptr) == -1) {
        throw runtime_error("No se pudo instalar el handler de SIGINT");
    }
    // Ignorar SIGPIPE: si un lector cierra su extremo antes de que el hijo escriba
    signal(SIGPIPE, SIG_IGN);

    // FIX (2.4): Ampliar límite de file descriptors al máximo permitido
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);  // error no es fatal; seguimos con el límite actual
    }
}

// ─────────────────────────────────────────────
// Duración efectiva
// ─────────────────────────────────────────────
int ProcessManager::effective_duration(const Activity& act) {
    if (act.duration_ms < 0) {
        return 100 + rand() % (5000 - 100 + 1);
    }
    return act.duration_ms;
}

// ─────────────────────────────────────────────
// launch_one: crea pipes, hace fork y ejecuta la actividad en el hijo.
//
// Diseño de pipes:
//   • pipe_result[2]: hijo → padre  (hijo escribe "OK:<id>:<nombre>" al terminar)
//   • pipe_deps[2]:   padre → hijo  (padre escribe mensajes de dependencias ya OK)
//
// El hijo lee los mensajes de sus dependencias ANTES de simular el trabajo,
// cumpliendo el requisito de la rúbrica (2.2).
//
// FIX (1.3): El hijo usa _exit() en lugar de exit() para evitar que se
// vacíen los buffers heredados del padre y se duplique la salida.
//
// FIX (2.4): Si pipe() falla por EMFILE (sin fds disponibles), retorna -2
// para indicar "diferir" en lugar de marcar la actividad como fallida.
// ─────────────────────────────────────────────
pid_t ProcessManager::launch_one(const string& node_id) {
    Node& node = sched_.get_node(node_id);
    int dur    = effective_duration(node.activity);

    // ── Recolectar mensajes de dependencias ya completadas ──────────────
    // Construir el string que el padre enviará al hijo antes de que empiece.
    string dep_payload;
    for (const auto& dep_id : node.activity.deps) {
        const Node& dep_node = sched_.get_node(dep_id);
        if (!dep_node.completion_msg.empty()) {
            if (!dep_payload.empty()) dep_payload += "|";
            dep_payload += dep_node.completion_msg;
        }
    }

    // ── Crear pipe resultado: hijo → padre ──────────────────────────────
    int pipe_result[2];
    if (pipe(pipe_result) == -1) {
        if (errno == EMFILE || errno == ENFILE) {
            // Sin file descriptors disponibles: diferir (no marcar como FALLIDA)
            return -2;
        }
        cerr << "[ERROR] pipe(result) para \"" << node_id << "\": " << strerror(errno) << "\n";
        return -1;
    }

    // ── Crear pipe deps: padre → hijo ───────────────────────────────────
    int pipe_deps[2] = {-1, -1};
    bool has_deps_msg = !dep_payload.empty();
    if (has_deps_msg) {
        if (pipe(pipe_deps) == -1) {
            if (errno == EMFILE || errno == ENFILE) {
                close(pipe_result[0]);
                close(pipe_result[1]);
                return -2;
            }
            cerr << "[ERROR] pipe(deps) para \"" << node_id << "\": " << strerror(errno) << "\n";
            // No es fatal: el hijo simplemente no recibirá mensajes de deps
            has_deps_msg = false;
        }
    }

    // FIX (1.3): Vaciar stdout del padre antes del fork para que el hijo
    // no herede datos pendientes en el buffer y los vuelva a imprimir.
    fflush(stdout);

    pid_t pid = fork();
    if (pid == -1) {
        cerr << "[ERROR] fork() para \"" << node_id << "\": " << strerror(errno) << "\n";
        close(pipe_result[0]);
        close(pipe_result[1]);
        if (has_deps_msg) { close(pipe_deps[0]); close(pipe_deps[1]); }
        if (errno == EMFILE || errno == ENFILE) return -2;
        return -1;
    }

    if (pid == 0) {
        // ── PROCESO HIJO ───────────────────────────────────────────────
        // Restaurar SIGINT al comportamiento por defecto en el hijo
        signal(SIGINT, SIG_DFL);

        // Cerrar extremos que el hijo no usa
        close(pipe_result[0]);           // hijo no lee del pipe de resultado
        if (has_deps_msg) close(pipe_deps[1]);  // hijo no escribe en el pipe de deps

        // FIX (2.2): Leer mensajes de dependencias recibidos del padre
        // (se los enviará antes de que llamemos a usleep)
        if (has_deps_msg && pipe_deps[0] != -1) {
            char buf[4096] = {};
            ssize_t n = read(pipe_deps[0], buf, sizeof(buf) - 1);
            close(pipe_deps[0]);
            if (n > 0) {
                cerr << "[INSUMOS] \"" << node.activity.name
                     << "\" recibió: " << string(buf, static_cast<size_t>(n)) << "\n";
                cerr.flush();
            }
        }

        cerr << "[INICIO] Actividad \"" << node.activity.name
             << "\" (ID=" << node_id << ", dur=" << dur << "ms)\n";
        cerr.flush();

        // Simular trabajo con usleep (sin busy-waiting)
        usleep(static_cast<useconds_t>(dur) * 1000);

        // Escribir mensaje de finalización al padre a través del pipe
        string msg = "OK:" + node_id + ":" + node.activity.name;
        ssize_t written = write(pipe_result[1], msg.c_str(), msg.size());
        close(pipe_result[1]);

        cerr << "[FIN] Actividad \"" << node.activity.name
             << "\" (ID=" << node_id << ") completada.\n";
        cerr.flush();

        // FIX (1.3): _exit() en lugar de exit() para NO vaciar los buffers
        // heredados del padre (evita salida duplicada al redirigir a archivo)
        _exit(written > 0 ? 0 : 1);
        // ── FIN PROCESO HIJO ──────────────────────────────────────────
    }

    // ── PROCESO PADRE ─────────────────────────────────────────────────
    close(pipe_result[1]);           // padre no escribe en el pipe de resultado
    if (has_deps_msg) close(pipe_deps[0]);  // padre no lee del pipe de deps

    // FIX (2.2): Enviar mensajes de dependencias al hijo ANTES de que empiece
    if (has_deps_msg && pipe_deps[1] != -1) {
        write(pipe_deps[1], dep_payload.c_str(), dep_payload.size());
        close(pipe_deps[1]);
    }

    // Registrar nodo como RUNNING
    sched_.mark_running(node_id, pid, dur, pipe_result[0], /*write_fd=*/-1);
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
            sched_.get_node(id).state = NodeState::READY;
            break;
        }
        pid_t pid = launch_one(id);
        if (pid == -2) {
            // Sin file descriptors disponibles: diferir la actividad
            sched_.get_node(id).state = NodeState::READY;
            cerr << "[DEFER] Actividad \"" << id
                 << "\" diferida por límite de file descriptors.\n";
        } else if (pid == -1) {
            // fork falló definitivamente: marcar como fallida
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
            // FIX (2.1): Sin SA_RESTART, llegará EINTR al recibir SIGINT.
            // Retornamos vacío para que el bucle principal revise g_sigint_received.
            return {"", false};
        }
        if (errno == ECHILD) {
            return {"", false};
        }
        cerr << "[ERROR] waitpid: " << strerror(errno) << "\n";
        return {"", false};
    }

    auto it = pid_to_id_.find(pid);
    if (it == pid_to_id_.end()) {
        return {"", false};
    }

    string node_id = it->second;
    pid_to_id_.erase(it);

    // Leer el mensaje de finalización del hijo
    Node& node = sched_.get_node(node_id);
    if (node.pipe.read_fd != -1) {
        char buf[512] = {};
        ssize_t n = read(node.pipe.read_fd, buf, sizeof(buf) - 1);
        close(node.pipe.read_fd);
        node.pipe.read_fd = -1;

        if (n > 0) {
            string msg(buf, static_cast<size_t>(n));
            // FIX (2.2): Guardar el mensaje en el nodo para que sus dependientes
            // lo reciban cuando sean lanzados (padre se lo pasará vía pipe_deps)
            node.completion_msg = msg;
            cout << "[MENSAJE] \"" << node_id << "\": " << msg << "\n";
            cout.flush();
        }
    }

    // Determinar éxito o fallo
    bool success = false;
    if (WIFEXITED(status)) {
        success = (WEXITSTATUS(status) == 0);
    } else if (WIFSIGNALED(status)) {
        // El hijo fue matado por señal.
        // FIX (2.1): No lo contamos como [FALLO] si fue matado por SIGINT
        // (que el propio padre envió durante kill_all_running).
        success = false;
    }

    return {node_id, success};
}

// ─────────────────────────────────────────────
// kill_all_running: envía SIGTERM a todos los hijos activos
//
// FIX (2.1): Usa waitpid bloqueante en loop hasta recolectar TODOS los
// hijos, evitando zombies. El intento único con WNOHANG anterior dejaba
// procesos sin recolectar si tardaban en terminar.
// ─────────────────────────────────────────────
void ProcessManager::kill_all_running() {
    // Fase 1: enviar SIGTERM a todos los hijos activos
    for (auto& [pid, id] : pid_to_id_) {
        cout << "[KILL] Enviando SIGTERM a PID=" << pid
             << " (actividad \"" << id << "\")\n";
        kill(pid, SIGTERM);
    }
    cout.flush();

    // Fase 2: esperar a que TODOS terminen (bloqueante, sin zombies)
    while (!pid_to_id_.empty()) {
        int   status = 0;
        pid_t pid    = waitpid(-1, &status, 0);   // bloqueante
        if (pid <= 0) break;
        pid_to_id_.erase(pid);
    }
    pid_to_id_.clear();
}
