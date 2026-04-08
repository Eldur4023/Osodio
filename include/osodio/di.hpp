#pragma once
#include <memory>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <any>

// ─── ServiceContainer ─────────────────────────────────────────────────────────
// Thread-safe after construction: all registrations must happen before app.run().
// Resolution is lock-free (read-only unordered_map + shared_ptr copies).
//
// Supports two lifetimes:
//   singleton  — one instance shared across all requests
//               app.provide(std::make_shared<Database>(conn_str));
//
//   transient  — factory called once per Inject<T> resolution
//               app.provide<Logger>([]{ return std::make_shared<Logger>(); });

namespace osodio {

class ServiceContainer {
public:
    // Register a singleton — the same shared_ptr is returned on every resolve<T>().
    template<typename T>
    void singleton(std::shared_ptr<T> instance) {
        singletons_[typeid(T)] = std::move(instance);
    }

    // Register a transient factory — called on every resolve<T>().
    template<typename T, typename F>
    void transient(F&& factory) {
        factories_[typeid(T)] = [f = std::forward<F>(factory)]() -> std::any {
            return std::shared_ptr<T>(f());
        };
    }

    // Resolve T: singletons take priority over factories.
    // Returns nullptr if T is not registered.
    template<typename T>
    std::shared_ptr<T> resolve() const {
        auto sit = singletons_.find(typeid(T));
        if (sit != singletons_.end())
            return std::any_cast<std::shared_ptr<T>>(sit->second);

        auto fit = factories_.find(typeid(T));
        if (fit != factories_.end())
            return std::any_cast<std::shared_ptr<T>>(fit->second());

        return nullptr;
    }

    bool has(const std::type_info& ti) const {
        return singletons_.count(ti) || factories_.count(ti);
    }

private:
    std::unordered_map<std::type_index, std::any>                  singletons_;
    std::unordered_map<std::type_index, std::function<std::any()>> factories_;
};

// ─── Inject<T> ────────────────────────────────────────────────────────────────
// Handler parameter type that resolves T from the service container.
//
//   app.get("/report", [](Inject<Database> db, Inject<Logger> log) -> nlohmann::json {
//       log->info("GET /report");
//       return db->query("SELECT ...");
//   });

template<typename T>
struct Inject {
    std::shared_ptr<T> ptr;

    Inject() = default;
    explicit Inject(std::shared_ptr<T> p) : ptr(std::move(p)) {}

    T* operator->() const { return ptr.get(); }
    T& operator*()  const { return *ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

} // namespace osodio
