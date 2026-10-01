#pragma once
#include "parser.hpp"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>

// Estado de cada nodo del DAG
enum class NodeState {
    PENDING,    // Esperando dependencias
    READY,      // Listo para ejecutar (en la ready queue)
    RUNNING,    // Siendo ejecutado actualmente
    DONE,       // Finalizado con éxito
    FAILED,     // Falló; su rama será abortada
    ABORTED     // Abortado por fallo de un predecesor
};

// Extremos de un pipe de notificación por actividad
struct ActivityPipe {
    int read_fd  = -1;   // fd que el padre usa para recibir resultado del hijo
    int write_fd = -1;   // fd que el hijo usa para enviar resultado al padre
};

// Nodo del grafo que engloba toda la información de una actividad
struct Node {
    Activity             activity;
    NodeState            state           = NodeState::PENDING;
    pid_t                pid             = -1;   // PID del proceso hijo (-1 si no lanzado)
    int                  duration_ms     = 0;    // Duración efectiva (aleatorio si era -1)
    ActivityPipe         pipe;                   // Pipe hijo→padre (resultado)
    std::vector<std::string> successors;         // IDs de nodos que dependen de éste
    int                  pending_deps    = 0;    // Cuántas dependencias faltan por completar
    std::string          completion_msg;         // Mensaje OK enviado al padre (guardado para las dependientes)
};

// Grafo DAG + Ready Queue + lógica de dependencias
class Scheduler {
public:
    // Construye el grafo a partir de las actividades parseadas.
    // Lanza std::runtime_error si hay ciclos o IDs duplicados.
    explicit Scheduler(const std::vector<Activity>& activities);

    // Devuelve verdadero si quedan nodos sin finalizar (DONE/FAILED/ABORTED).
    // Implementado con contador O(1) para evitar O(N) por iteración.
    bool has_pending() const;

    // Devuelve los nodos listos para ejecutar (en orden FIFO)
    std::vector<std::string> get_ready_ids();

    // Marca un nodo como RUNNING (después de hacer fork)
    void mark_running(const std::string& id, pid_t pid, int duration_ms,
                      int read_fd, int write_fd);

    // Procesa la finalización exitosa de un nodo:
    //   - lo marca DONE
    //   - decrementa pending_deps de sus sucesores
    //   - encola en ready_queue los sucesores que queden sin deps
    void mark_done(const std::string& id);

    // Procesa el fallo de un nodo:
    //   - lo marca FAILED
    //   - propaga ABORTED en cascada a todos sus descendientes
    void mark_failed(const std::string& id);

    // Marca un nodo como ABORTED (propagación de fallos)
    void mark_aborted(const std::string& id);

    // Devuelve referencia mutable al nodo (para leer pid/pipes/etc.)
    Node& get_node(const std::string& id);
    const Node& get_node(const std::string& id) const;

    // Devuelve todos los IDs de nodos en estado RUNNING
    std::vector<std::string> get_running_ids() const;

    // Devuelve todos los nodos
    const std::unordered_map<std::string, Node>& nodes() const { return nodes_; }

private:
    std::unordered_map<std::string, Node> nodes_;
    std::queue<std::string>               ready_queue_;
    int                                   pending_count_ = 0; // contador O(1) para has_pending()

    // Detección de ciclos (DFS sobre el DAG)
    void detect_cycles() const;
    void dfs(const std::string& id,
             std::unordered_set<std::string>& visited,
             std::unordered_set<std::string>& in_stack) const;

    // Propaga ABORTED recursivamente a descendientes
    void abort_subtree(const std::string& id);
};
