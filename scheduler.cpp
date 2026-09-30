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
