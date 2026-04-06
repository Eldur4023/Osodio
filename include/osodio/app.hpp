#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "types.hpp"
#include "router.hpp"

namespace osodio {

class App {
public:
    App()  = default;
    ~App() = default;

    // Route registration — support both :param and {param} styles
    template<typename F> App& get   (std::string path, F&& h) { router_.add("GET",    std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& post  (std::string path, F&& h) { router_.add("POST",   std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& put   (std::string path, F&& h) { router_.add("PUT",    std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& patch (std::string path, F&& h) { router_.add("PATCH",  std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& del   (std::string path, F&& h) { router_.add("DELETE", std::move(path), std::forward<F>(h)); return *this; }
    template<typename F> App& any   (std::string path, F&& h) { router_.add("*",      std::move(path), std::forward<F>(h)); return *this; }

    // Middleware (applied in order for every request)
    App& use(Middleware m) { middlewares_.push_back(std::move(m)); return *this; }

    // Directorio donde se buscan los templates (default: "./templates")
    App& set_templates(std::string dir) { templates_dir_ = std::move(dir); return *this; }

    // Start listening — Flask style:
    //   app.run()               → 0.0.0.0:5000
    //   app.run(8080)           → 0.0.0.0:8080
    //   app.run("127.0.0.1", 8080)
    void run(const std::string& host = "0.0.0.0", uint16_t port = 5000);
    void run(uint16_t port) { run("0.0.0.0", port); }

private:
    Router                  router_;
    std::vector<Middleware> middlewares_;
    std::string             templates_dir_ = "./templates";
};

} // namespace osodio
