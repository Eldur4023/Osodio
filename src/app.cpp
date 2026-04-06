#include "../include/osodio/app.hpp"
#include "../include/osodio/request.hpp"
#include "../include/osodio/response.hpp"

#include <osodio/core/event_loop.hpp>
#include "core/tcp_server.hpp"

#include <csignal>
#include <iostream>
#include <memory>

namespace osodio {

namespace {
    core::EventLoop* g_loop = nullptr;
    void signal_handler(int) {
        std::cout << "\nShutting down..." << std::endl;
        if (g_loop) g_loop->stop();
    }
}

void App::run(const std::string& host, uint16_t port) {
    std::signal(SIGPIPE, SIG_IGN);

    core::EventLoop loop;
    g_loop = &loop;
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Construimos la función de dispatch que:
    //  1. Configura el templates_dir en la Response
    //  2. Ejecuta la cadena de middlewares
    //  3. Al final de la cadena, busca la ruta en el router
    DispatchFn dispatch = [this, &loop](Request& req, Response& res) {
        req.loop = &loop;
        res.set_templates_dir(templates_dir_);

        // call_next(i) ejecuta el middleware i, o el router si ya no quedan
        auto call_next = std::make_shared<std::function<void(size_t)>>();
        *call_next = [this, &req, &res, call_next](size_t i) {
            if (i < middlewares_.size()) {
                middlewares_[i](req, res, [call_next, i]() {
                    (*call_next)(i + 1);
                });
            } else {
                auto match = router_.match(req.method, req.path);
                if (match.found) {
                    req.params = std::move(match.params);
                    match.handler(req, res);
                } else {
                    res.status(404).json({{"error", "Not Found"}, {"path", req.path}});
                }
            }
        };
        (*call_next)(0);
    };

    core::TcpServer server(host, port, loop, std::move(dispatch));

    std::cout << " * Osodio running on http://" << host << ":" << port
              << "  (Press CTRL+C to quit)\n";

    loop.run();
    g_loop = nullptr;
}

} // namespace osodio
