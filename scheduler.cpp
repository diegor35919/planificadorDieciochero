#include "scheduler.hpp"

#include <stdexcept>
#include <algorithm>
#include <iostream>

using namespace std;

// ─────────────────────────────────────────────
// Constructor: construye el grafo y la ready queue inicial
// ─────────────────────────────────────────────
Scheduler::Scheduler(const vector<Activity>& activities) {
    // 1) Crear nodos
    for (const auto& act : activities) {
        if (nodes_.count(act.id)) {
            throw runtime_error("ID duplicado en el plan: \"" + act.id + "\"");
        }
        Node node;
        node.activity     = act;
        node.state        = NodeState::PENDING;
        node.pending_deps = static_cast<int>(act.deps.size());
        nodes_[act.id]    = node;
    }

    // 2) Construir lista de sucesores (aristas inversas) y verificar que los deps existan
    for (auto& [id, node] : nodes_) {
        for (const auto& dep_id : node.activity.deps) {
            if (!nodes_.count(dep_id)) {
                throw runtime_error("La actividad \"" + id
                                    + "\" depende de \"" + dep_id
                                    + "\" que no existe en el plan.");
            }
            nodes_[dep_id].successors.push_back(id);
        }
    }

    // 3) Detectar ciclos
    detect_cycles();

    // 4) Cargar ready queue inicial (nodos sin dependencias)
    for (const auto& [id, node] : nodes_) {
        if (node.pending_deps == 0) {
            ready_queue_.push(id);
            nodes_[id].state = NodeState::READY;
        }
    }
}

// ─────────────────────────────────────────────
// Detección de ciclos con DFS
// ─────────────────────────────────────────────
void Scheduler::detect_cycles() const {
    unordered_set<string> visited;
    unordered_set<string> in_stack;
    for (const auto& [id, _] : nodes_) {
        if (!visited.count(id)) {
            dfs(id, visited, in_stack);
        }
    }
}

void Scheduler::dfs(const string& id,
                    unordered_set<string>& visited,
                    unordered_set<string>& in_stack) const {
    visited.insert(id);
    in_stack.insert(id);

    const Node& node = nodes_.at(id);
    for (const auto& succ : node.successors) {
        if (!visited.count(succ)) {
            dfs(succ, visited, in_stack);
        } else if (in_stack.count(succ)) {
            throw runtime_error("Ciclo detectado en el DAG involucrando: \""
                                + id + "\" → \"" + succ + "\"");
        }
    }
    in_stack.erase(id);
}

// ─────────────────────────────────────────────
// ¿Quedan nodos sin terminar?
// ─────────────────────────────────────────────
bool Scheduler::has_pending() const {
    for (const auto& [id, node] : nodes_) {
        if (node.state != NodeState::DONE &&
            node.state != NodeState::FAILED &&
            node.state != NodeState::ABORTED) {
            return true;
        }
    }
    return false;
}

// ─────────────────────────────────────────────
// Devuelve IDs listos en orden FIFO y los saca de la cola
// ─────────────────────────────────────────────
vector<string> Scheduler::get_ready_ids() {
    vector<string> ready;
    while (!ready_queue_.empty()) {
        ready.push_back(ready_queue_.front());
        ready_queue_.pop();
    }
    return ready;
}

// ─────────────────────────────────────────────
// Marcar nodo como RUNNING
// ─────────────────────────────────────────────
void Scheduler::mark_running(const string& id, pid_t pid, int duration_ms,
                              int read_fd, int write_fd) {
    auto& node          = nodes_.at(id);
    node.state          = NodeState::RUNNING;
    node.pid            = pid;
    node.duration_ms    = duration_ms;
    node.pipe.read_fd   = read_fd;
    node.pipe.write_fd  = write_fd;
}

// ─────────────────────────────────────────────
// Marcar nodo como DONE y propagar a sucesores
// ─────────────────────────────────────────────
void Scheduler::mark_done(const string& id) {
    auto& node = nodes_.at(id);
    node.state = NodeState::DONE;

    // Decrementar dependencias de sucesores
    for (const auto& succ_id : node.successors) {
        auto& succ = nodes_.at(succ_id);
        // Solo procesar si el sucesor no ha sido abortado
        if (succ.state == NodeState::ABORTED || succ.state == NodeState::FAILED) continue;

        --succ.pending_deps;
        if (succ.pending_deps == 0) {
            succ.state = NodeState::READY;
            ready_queue_.push(succ_id);
        }
    }
}

// ─────────────────────────────────────────────
// Marcar nodo como FAILED y propagar ABORTED
// ─────────────────────────────────────────────
void Scheduler::mark_failed(const string& id) {
    nodes_.at(id).state = NodeState::FAILED;
    // Propagar aborto a toda la sub-rama que dependía de este nodo
    for (const auto& succ_id : nodes_.at(id).successors) {
        abort_subtree(succ_id);
    }
}

void Scheduler::mark_aborted(const string& id) {
    nodes_.at(id).state = NodeState::ABORTED;
}

void Scheduler::abort_subtree(const string& id) {
    auto& node = nodes_.at(id);
    if (node.state == NodeState::ABORTED || node.state == NodeState::FAILED) return;
    node.state = NodeState::ABORTED;
    cerr << "[SCHEDULER] Actividad \"" << id << "\" abortada por fallo de dependencia.\n";
    for (const auto& succ_id : node.successors) {
        abort_subtree(succ_id);
    }
}

// ─────────────────────────────────────────────
// Accesores
// ─────────────────────────────────────────────
Node& Scheduler::get_node(const string& id) {
    return nodes_.at(id);
}

const Node& Scheduler::get_node(const string& id) const {
    return nodes_.at(id);
}

vector<string> Scheduler::get_running_ids() const {
    vector<string> running;
    for (const auto& [id, node] : nodes_) {
        if (node.state == NodeState::RUNNING) {
            running.push_back(id);
        }
    }
    return running;
}
