#include "parser.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <algorithm>

using namespace std;

// Elimina espacios en los extremos de una cadena
static string trim(const string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Divide una cadena por un delimitador y devuelve los tokens recortados
static vector<string> split(const string& s, char delim) {
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
        // El campo de dependencias puede estar vacío
        vector<string> fields = split(line, ':');

        // Se esperan al menos 3 campos (ID, Nombre, tiempo); deps es opcional
        if (fields.size() < 3) {
            throw runtime_error("Formato inválido en línea " + to_string(lineno)
                                + ": \"" + line + "\"");
        }

        Activity act;
        act.id   = trim(fields[0]);
        act.name = trim(fields[1]);

        // Parsear duración: si está vacío o no es número válido → -1 (aleatorio)
        string dur_str = trim(fields[2]);
        if (dur_str.empty()) {
            act.duration_ms = -1;
        } else {
            try {
                act.duration_ms = stoi(dur_str);
                if (act.duration_ms < 0) act.duration_ms = -1;
            } catch (...) {
                act.duration_ms = -1;
            }
        }

        // Parsear dependencias (campo 4, opcional)
        if (fields.size() >= 4) {
            string dep_str = trim(fields[3]);
            if (!dep_str.empty()) {
                act.deps = split(dep_str, ',');
            }
        }

        activities.push_back(act);
    }

    if (activities.empty()) {
        throw runtime_error("El archivo de plan está vacío o no contiene actividades válidas.");
    }

    return activities;
}
