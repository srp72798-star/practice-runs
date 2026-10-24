#pragma once

// Servidor HTTP local del AI Level Builder.
// Este archivo NO incluye Geode: el servidor (cpp-httplib) vive en su propia
// unidad de compilacion para no mezclar cabeceras de Windows con las de Geode.

#include <functional>
#include <string>

namespace ai {
    struct Response {
        int status = 200;
        std::string body;  // JSON
    };

    // Se llama desde el hilo del servidor. Nunca debe tocar el juego
    // directamente: tiene que pasar la operacion al hilo principal.
    using Handler = std::function<Response(
        std::string const& tool,
        std::string const& body,
        std::string const& idempotencyKey
    )>;

    // Escucha solo en 127.0.0.1. Todas las peticiones necesitan
    // "Authorization: Bearer <token>".
    bool startServer(int port, std::string token, Handler handler, std::string& error);
}
