#include "parser.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cerrno>
#include <cstdlib>

using namespace std;

// Elimina espacios en los extremos de una cadena
static string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Divide una cadena por delimitador CONSERVANDO campos vacíos.
// Necesario para parsear líneas como "1 : a : : dep1" donde la duración está vacía.
static vector<string> split_keep_empty(const string& s, char delim) {
    vector<string> tokens;
    stringstream ss(s);
    string token;
    while (getline(ss, token, delim)) {
        tokens.push_back(trim(token));  // push siempre, incluso si queda vacío
    }
    return tokens;
}

// Divide por delimitador descartando tokens vacíos (para listas de deps por coma)
static vector<string> split_skip_empty(const string& s, char delim) {
    vector<string> tokens;
    stringstream ss(s);
    string token;
    while (getline(ss, token, delim)) {
        string t = trim(token);
        if (!t.empty()) tokens.push_back(t);
    }
    return tokens;
}

vector<Activity> parse_plan(const string& filepath) {
    ifstream file(filepath);
    if (!file.is_open()) {
        throw runtime_error("No se pudo abrir el archivo: " + filepath);
    }

    vector<Activity> activities;
    string line;
    int lineno = 0;

    while (getline(file, line)) {
        ++lineno;
        line = trim(line);

        // Ignorar líneas vacías y comentarios
        if (line.empty() || line[0] == '#') continue;

        // Formato esperado: ID : Nombre : tiempo_ms : [dep1, dep2, ...]
        // Usamos split_keep_empty para preservar el campo de duración aunque esté vacío.
        // Ejemplo válido: "1 : a : : dep1"  →  ["1", "a", "", "dep1"]
        vector<string> fields = split_keep_empty(line, ':');

        // Se esperan al menos 3 campos (ID, Nombre, tiempo); deps es opcional
        if (fields.size() < 3) {
            throw runtime_error("Formato inválido en línea " + to_string(lineno)
                                + ": \"" + line + "\"");
        }

        Activity act;
        act.id   = fields[0];
        act.name = fields[1];

        if (act.id.empty()) {
            throw runtime_error("ID vacío en línea " + to_string(lineno));
        }
        if (act.name.empty()) {
            throw runtime_error("Nombre vacío en línea " + to_string(lineno));
        }

        // Parsear duración: campo vacío → aleatorio (-1); no-número → aleatorio
        const string& dur_str = fields[2];
        if (dur_str.empty()) {
            act.duration_ms = -1;  // aleatorio
        } else {
            // Usar strtol para detectar errores de parseo robustamente
            char* end_ptr = nullptr;
            errno = 0;
            long val = strtol(dur_str.c_str(), &end_ptr, 10);
            // Válido solo si consumió todos los caracteres y no hubo overflow
            if (end_ptr == dur_str.c_str() || *end_ptr != '\0' || errno != 0 || val < 0) {
                act.duration_ms = -1;  // valor inválido → aleatorio
            } else {
                act.duration_ms = static_cast<int>(val);
            }
        }

        // Parsear dependencias: campo 4 opcional, puede contener "[dep1, dep2]" o "dep1, dep2"
        if (fields.size() >= 4 && !fields[3].empty()) {
            string dep_str = fields[3];
            // Quitar corchetes opcionales: [dep1, dep2] → dep1, dep2
            if (!dep_str.empty() && dep_str.front() == '[') dep_str.erase(dep_str.begin());
            if (!dep_str.empty() && dep_str.back()  == ']') dep_str.pop_back();
            act.deps = split_skip_empty(dep_str, ',');
        }

        activities.push_back(act);
    }

    if (activities.empty()) {
        throw runtime_error("El archivo de plan está vacío o no contiene actividades válidas.");
    }

    return activities;
}
