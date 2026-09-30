#pragma once
#include <string>
#include <vector>

// Representa una actividad leída desde plan.txt
struct Activity {
    std::string id;                     // ID único alfanumérico
    std::string name;                   // Nombre descriptivo
    int duration_ms;                    // Duración en milisegundos (-1 si no especificada → aleatoria)
    std::vector<std::string> deps;      // IDs de dependencias
};

// Lee y parsea plan.txt.
// Lanza std::runtime_error si el archivo no puede abrirse o contiene errores de formato.
std::vector<Activity> parse_plan(const std::string& filepath);
