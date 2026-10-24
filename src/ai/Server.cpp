// Servidor HTTP local (cpp-httplib). Sin cabeceras de Geode a proposito.

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00  // Windows 10, lo exige cpp-httplib
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Server.hpp"

#include <httplib.h>

#include <memory>
#include <thread>

namespace ai {
    namespace {
        // El servidor vive hasta que se cierra el juego. No se destruye a
        // proposito: destruir un hilo activo al salir del proceso lo cerraria
        // de golpe.
        httplib::Server* g_server = nullptr;

        bool sameSecret(std::string const& a, std::string const& b) {
            if (a.size() != b.size()) return false;
            unsigned char diff = 0;
            for (size_t i = 0; i < a.size(); i++) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
            return diff == 0;
        }

        void sendError(httplib::Response& res, int status, char const* code, char const* message) {
            res.status = status;
            res.set_content(
                std::string(R"({"ok":false,"error":")") + code + R"(","message":")" + message + R"("})",
                "application/json"
            );
        }
    }

    bool startServer(int port, std::string token, Handler handler, std::string& error) {
        if (g_server) return true;
        if (token.size() < 32) {
            error = "token demasiado corto";
            return false;
        }

        auto server = new httplib::Server();
        server->set_payload_max_length(4 * 1024 * 1024);  // 4 MB por peticion
        server->set_read_timeout(10, 0);
        server->set_write_timeout(10, 0);
        server->new_task_queue = [] { return new httplib::ThreadPool(2); };

        server->Post(R"(/api/([a-z_]+))", [token, handler](httplib::Request const& req, httplib::Response& res) {
            // Solo conexiones desde este mismo PC
            if (req.remote_addr != "127.0.0.1") {
                return sendError(res, 403, "forbidden", "Solo se aceptan conexiones locales");
            }
            // Un navegador manda Origin: se rechaza para evitar que una web
            // pueda hablar con el mod
            if (req.has_header("Origin")) {
                return sendError(res, 403, "forbidden", "Peticiones de navegador no permitidas");
            }
            if (!sameSecret(req.get_header_value("Authorization"), "Bearer " + token)) {
                return sendError(res, 401, "unauthorized", "Token incorrecto");
            }

            auto result = handler(req.matches[1], req.body, req.get_header_value("Idempotency-Key"));
            res.status = result.status;
            res.set_content(result.body, "application/json");
        });

        if (!server->bind_to_port("127.0.0.1", port)) {
            error = "No se pudo abrir el puerto " + std::to_string(port) + " (puede estar en uso)";
            delete server;
            return false;
        }

        g_server = server;
        std::thread([] { g_server->listen_after_bind(); }).detach();
        return true;
    }
}
