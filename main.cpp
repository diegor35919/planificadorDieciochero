#include "parser.hpp"
#include "scheduler.hpp"
#include "process_manager.hpp"

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

using namespace std;

// ─────────────────────────────────────────────
// Uso: ./planificador plan.txt K
// ─────────────────────────────────────────────
static void usage(const char* prog) {
    cerr << "Uso: " << prog << " <plan.txt> <K>\n"
         << "  plan.txt  - Archivo de planificación de actividades\n"
         << "  K         - Límite de concurrencia (número máximo de procesos simultáneos)\n";
}

int main(int argc, char* argv[]) {
    // ── 1. Validar argumentos ───────────────────────────────────────────
    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }

    string plan_file = argv[1];
    int K = 0;
    try {
        K = stoi(argv[2]);
        if (K <= 0) throw invalid_argument("K debe ser positivo");
    } catch (const exception& e) {
        cerr << "[ERROR] Argumento K inválido: " << e.what() << "\n";
        usage(argv[0]);
        return 1;
    }

    // ── 2. Parsear plan.txt ─────────────────────────────────────────────
    vector<Activity> activities;
    try {
        activities = parse_plan(plan_file);
        cout << "[PARSER] " << activities.size() << " actividades leídas de \""
             << plan_file << "\"\n";
    } catch (const exception& e) {
        cerr << "[ERROR] Parseo: " << e.what() << "\n";
        return 1;
    }

    // ── 3. Construir el DAG ─────────────────────────────────────────────
    Scheduler sched(activities);
    cout << "[SCHEDULER] DAG construido. K=" << K << "\n";

    // ── 4. Inicializar el Process Manager ───────────────────────────────
    ProcessManager pm(sched, K);
    try {
        pm.setup_signals();
    } catch (const exception& e) {
        cerr << "[ERROR] Señales: " << e.what() << "\n";
        return 1;
    }

    // ── 5. Bucle principal ──────────────────────────────────────────────
    //
    // Invariante del bucle:
    //   running_count = número de hijos activos en este momento
    //
    // Lógica:
    //   a) Obtener nodos READY y lanzarlos respetando el límite K.
    //   b) Si hay procesos corriendo, esperar a que uno termine (waitpid bloqueante).
    //   c) Actualizar el scheduler con el resultado (DONE o FAILED).
    //   d) Los nuevos READY generados por mark_done() se tomarán en la próxima iteración.
    //   e) Si no hay procesos corriendo ni nodos pendientes → terminamos.
    //   f) Si SIGINT fue recibido → matar todo y salir.

    int running_count = 0;
    // Buffer para nodos READY que no pudieron ser lanzados por límite K
    vector<string> deferred_ready;

    cout << "\n══════════════════════════════════════\n"
         << "   INICIO DE LA PLANIFICACIÓN\n"
         << "══════════════════════════════════════\n\n";

    while (sched.has_pending() || running_count > 0) {
        // Verificar SIGINT (Ctrl+C → Inspección de la Seremi)
        if (ProcessManager::sigint_received()) {
            cout << "\n[SIGINT] ¡Inspección de la Seremi! Abortando todas las actividades...\n";
            pm.kill_all_running();
            cout << "[SIGINT] Todas las actividades han sido detenidas.\n";
            return 130;  // Código de salida estándar para Ctrl+C
        }

        // a) Recopilar nodos READY: diferidos + nuevos del scheduler
        vector<string> ready = move(deferred_ready);
        deferred_ready.clear();

        vector<string> new_ready = sched.get_ready_ids();
        ready.insert(ready.end(), new_ready.begin(), new_ready.end());

        // b) Lanzar hasta K procesos
        for (const auto& id : ready) {
            if (ProcessManager::sigint_received()) break;

            if (running_count >= K) {
                // Límite alcanzado: diferir
                deferred_ready.push_back(id);
                sched.get_node(id).state = NodeState::READY;
                continue;
            }

            // Verificar que el nodo no haya sido abortado mientras esperaba
            NodeState st = sched.get_node(id).state;
            if (st == NodeState::ABORTED || st == NodeState::FAILED) {
                cout << "[SKIP] Actividad \"" << id << "\" omitida (abortada/fallida).\n";
                continue;
            }

            // Intentar lanzar
            // Preparamos una lista de un elemento y llamamos launch_ready
            vector<string> single = {id};
            int launched = pm.launch_ready(single, running_count);
            if (launched > 0) {
                running_count += launched;
            } else {
                // Si el nodo sigue en READY (diferido por EMFILE), volver a encolar
                NodeState st2 = sched.get_node(id).state;
                if (st2 == NodeState::READY) {
                    deferred_ready.push_back(id);
                }
                // Si está en FAILED ya fue marcado por launch_ready
            }
        }

        // c) Si no hay hijos corriendo y no hay nodos pendientes → done
        if (running_count == 0) {
            if (!sched.has_pending()) break;
            // Si hay deferred pero no podemos lanzar (todos abortados), salir
            bool all_done = true;
            for (const auto& id : deferred_ready) {
                NodeState st = sched.get_node(id).state;
                if (st != NodeState::ABORTED && st != NodeState::FAILED) {
                    all_done = false;
                    break;
                }
            }
            if (all_done) break;
            // Caso raro: hay nodos diferidos pero K=0 (ya validado), no debería pasar
            continue;
        }

        // d) Esperar a que un hijo termine (SIN busy-waiting)
        WaitResult result = pm.wait_for_any();

        if (result.node_id.empty()) {
            // Interrumpido por señal o error
            continue;
        }

        --running_count;

        if (result.success) {
            cout << "[OK] Actividad \"" << result.node_id << "\" completada con éxito.\n";
            sched.mark_done(result.node_id);
        } else {
            cerr << "[FALLO] Actividad \"" << result.node_id
                 << "\" falló. Abortando su rama...\n";
            sched.mark_failed(result.node_id);
        }
    }

    // Verificar SIGINT una última vez
    if (ProcessManager::sigint_received()) {
        pm.kill_all_running();
        return 130;
    }

    // ── 6. Resumen final ────────────────────────────────────────────────
    cout << "\n══════════════════════════════════════\n"
         << "   RESUMEN DE LA PLANIFICACIÓN\n"
         << "══════════════════════════════════════\n";

    int done = 0, failed = 0, aborted = 0;
    for (const auto& [id, node] : sched.nodes()) {
        string status_str;
        switch (node.state) {
            case NodeState::DONE:    ++done;    status_str = "✓ COMPLETADA"; break;
            case NodeState::FAILED:  ++failed;  status_str = "✗ FALLIDA";   break;
            case NodeState::ABORTED: ++aborted; status_str = "⊘ ABORTADA";  break;
            default:                            status_str = "? DESCONOCIDA"; break;
        }
        cout << "  [" << status_str << "] " << node.activity.name
             << " (ID=" << id << ")\n";
    }

    cout << "\nTotal: " << done    << " completadas, "
                        << failed  << " fallidas, "
                        << aborted << " abortadas.\n";

    cout << "\n══════════════════════════════════════\n";

    return (failed + aborted > 0) ? 1 : 0;
}
