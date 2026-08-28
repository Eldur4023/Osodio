#include <odio/db.hpp>

namespace odio {

// ─── DbPool ──────────────────────────────────────────────────────────────────

DbPool::~DbPool() { stop(); }

void DbPool::start(size_t workers) {
    if (!threads_.empty()) return;
    workers_.assign(workers, 0);

    for (size_t i = 0; i < workers; ++i) {
        threads_.emplace_back([this, i] {
            for (;;) {
                std::function<void(size_t)> job;
                {
                    std::unique_lock<std::mutex> lock(mutex_);
                    cv_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                    if (stopping_ && jobs_.empty()) return;
                    job = std::move(jobs_.front());
                    jobs_.pop();
                }
                // Un trabajo que lanza no puede llevarse por delante el worker:
                // sin conexion viva, el modulo dejaria de responder para todos.
                try { job(i); } catch (...) {}
            }
        });
    }
}

void DbPool::submit(std::function<void(size_t)> job) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        jobs_.push(std::move(job));
    }
    cv_.notify_one();
}

void DbPool::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
}

// ─── DbRegistry ──────────────────────────────────────────────────────────────

// Cada driver compilado se declara aqui.  Las funciones existen solo si su
// opcion de cmake esta activada.
#ifdef OSODIO_SQLITE
std::unique_ptr<DbDriver> make_sqlite_driver();
#endif
#ifdef OSODIO_POSTGRES
std::unique_ptr<DbDriver> make_postgres_driver();
#endif
#ifdef OSODIO_MYSQL
std::unique_ptr<DbDriver> make_mysql_driver();
#endif

DbRegistry::DbRegistry() {
#ifdef OSODIO_SQLITE
    { Slot s; s.driver = make_sqlite_driver();   slots_["sqlite"]   = std::move(s); }
#endif
#ifdef OSODIO_POSTGRES
    { Slot s; s.driver = make_postgres_driver(); slots_["postgres"] = std::move(s); }
#endif
#ifdef OSODIO_MYSQL
    { Slot s; s.driver = make_mysql_driver();    slots_["mysql"]    = std::move(s); }
#endif
}

DbRegistry& DbRegistry::instance() {
    static DbRegistry r;
    return r;
}

std::vector<std::string> DbRegistry::available() const {
    std::vector<std::string> out;
    for (const auto& [name, _] : slots_) out.push_back(name);
    return out;
}

bool DbRegistry::has(const std::string& name) const {
    return slots_.count(name) > 0;
}

bool DbRegistry::activate(const std::string& name,
                          const std::map<std::string, std::string>& options,
                          std::string& error) {
    auto it = slots_.find(name);
    if (it == slots_.end()) {
        error = "el modulo '" + name + "' no esta compilado en este binario";
        return false;
    }
    Slot& slot = it->second;
    if (slot.activated) return true;

    if (!slot.driver->configure(options, error)) return false;

    slot.pool = std::make_unique<DbPool>();
    slot.pool->start(slot.driver->pool_size());
    slot.activated = true;
    return true;
}

DbDriver* DbRegistry::active(const std::string& name) const {
    auto it = slots_.find(name);
    if (it == slots_.end() || !it->second.activated) return nullptr;
    return it->second.driver.get();
}

DbPool* DbRegistry::pool(const std::string& name) const {
    auto it = slots_.find(name);
    if (it == slots_.end() || !it->second.activated) return nullptr;
    return it->second.pool.get();
}

void DbRegistry::shutdown() {
    for (auto& [_, slot] : slots_)
        if (slot.pool) slot.pool->stop();
}

} // namespace odio
