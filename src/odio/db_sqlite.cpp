#include <string_view>
#include <odio/db.hpp>

#include <sqlite3.h>

#include <cstring>

namespace odio {

namespace {

// Driver de SQLite.
//
// Cada worker abre su propia conexion al mismo fichero.  SQLite serializa las
// escrituras internamente, asi que varias conexiones concurrentes son seguras;
// lo que se activa es WAL, que permite leer mientras otro escribe en vez de
// bloquear a todo el mundo.
class SqliteDriver : public DbDriver {
public:
    const char* name() const override { return "sqlite"; }

    bool configure(const std::map<std::string, std::string>& options,
                   std::string& error) override {
        auto it = options.find("file");
        if (it == options.end() || it->second.empty()) {
            error = "sqlite: falta 'file' en el bloque de configuracion";
            return false;
        }
        file_ = it->second;

        auto p = options.find("pool");
        if (p != options.end()) {
            long n = std::strtol(p->second.c_str(), nullptr, 10);
            if (n < 1 || n > 64) {
                error = "sqlite: 'pool' tiene que estar entre 1 y 64";
                return false;
            }
            set_pool_size(static_cast<size_t>(n));
        }

        auto t = options.find("timeout_ms");
        if (t != options.end()) busy_timeout_ = std::atoi(t->second.c_str());

        conns_.assign(pool_size(), nullptr);
        return true;
    }

    bool open(size_t worker, std::string& error) override {
        if (worker >= conns_.size()) { error = "sqlite: worker fuera de rango"; return false; }
        if (conns_[worker]) return true;

        sqlite3* db = nullptr;
        int rc = sqlite3_open_v2(file_.c_str(), &db,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
        if (rc != SQLITE_OK) {
            error = std::string("sqlite: no se puede abrir '") + file_ + "': " +
                    (db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
            if (db) sqlite3_close(db);
            return false;
        }

        // WAL: lecturas concurrentes con una escritura en curso.  Sin esto, con
        // varios workers cualquier escritura bloquearia todas las lecturas.
        char* msg = nullptr;
        sqlite3_exec(db, "PRAGMA journal_mode=WAL", nullptr, nullptr, &msg);
        if (msg) sqlite3_free(msg);
        sqlite3_exec(db, "PRAGMA foreign_keys=ON", nullptr, nullptr, &msg);
        if (msg) sqlite3_free(msg);

        // Espera en vez de fallar cuando otra conexion tiene el fichero.
        sqlite3_busy_timeout(db, busy_timeout_);

        conns_[worker] = db;
        return true;
    }

    bool query(size_t worker, const std::string& sql, const std::vector<Value>& args,
               Value& out, std::string& error) override {
        sqlite3_stmt* stmt = nullptr;
        if (!prepare(worker, sql, args, &stmt, error)) return false;

        Value::List rows;
        int cols = sqlite3_column_count(stmt);

        for (;;) {
            int rc = sqlite3_step(stmt);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) {
                error = std::string("sqlite: ") + sqlite3_errmsg(conns_[worker]);
                sqlite3_finalize(stmt);
                return false;
            }
            Value::Dict row;
            // El numero de columnas se sabe: sin esto el diccionario crecia a
            // saltos y eran cinco realojos por fila.
            row.reservar(static_cast<size_t>(cols));
            for (int i = 0; i < cols; ++i) {
                const char* col = sqlite3_column_name(stmt, i);
                // Sin el ternario: mezclarlo con std::to_string obligaba a
                // construir un std::string temporal en CADA columna de CADA fila
                // solo para volver a leerlo como string_view.
                if (col) row[std::string_view(col)]  = column_value(stmt, i);
                else     row[std::to_string(i)]      = column_value(stmt, i);
            }
            rows.push_back(Value::dict(std::move(row)));
        }

        sqlite3_finalize(stmt);
        out = Value::list(std::move(rows));
        return true;
    }

    bool exec(size_t worker, const std::string& sql, const std::vector<Value>& args,
              long long& affected, std::string& error) override {
        sqlite3_stmt* stmt = nullptr;
        if (!prepare(worker, sql, args, &stmt, error)) return false;

        int rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
            error = std::string("sqlite: ") + sqlite3_errmsg(conns_[worker]);
            sqlite3_finalize(stmt);
            return false;
        }
        sqlite3_finalize(stmt);
        affected = sqlite3_changes(conns_[worker]);
        return true;
    }

    bool last_insert_id(size_t worker, long long& id, std::string& error) override {
        if (worker >= conns_.size() || !conns_[worker]) {
            error = "sqlite: sin conexion";
            return false;
        }
        id = sqlite3_last_insert_rowid(conns_[worker]);
        return true;
    }

    ~SqliteDriver() override {
        for (auto* db : conns_) if (db) sqlite3_close(db);
    }

private:
    std::string           file_;
    int                   busy_timeout_ = 5000;
    std::vector<sqlite3*> conns_;

    // Los parametros van SIEMPRE por bind, nunca concatenados: es lo que hace
    // imposible la inyeccion de SQL desde Odio.
    bool prepare(size_t worker, const std::string& sql, const std::vector<Value>& args,
                 sqlite3_stmt** out, std::string& error) {
        sqlite3* db = conns_[worker];
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, out, nullptr) != SQLITE_OK) {
            error = std::string("sqlite: ") + sqlite3_errmsg(db);
            return false;
        }

        int expected = sqlite3_bind_parameter_count(*out);
        if (expected != static_cast<int>(args.size())) {
            error = "sqlite: la consulta tiene " + std::to_string(expected) +
                    " parametro(s) y se pasaron " + std::to_string(args.size());
            sqlite3_finalize(*out);
            *out = nullptr;
            return false;
        }

        for (size_t i = 0; i < args.size(); ++i) {
            const Value& v = args[i];
            int idx = static_cast<int>(i) + 1;
            int rc;
            if      (v.is_null())  rc = sqlite3_bind_null(*out, idx);
            else if (v.is_bool())  rc = sqlite3_bind_int(*out, idx, v.as_bool() ? 1 : 0);
            else if (v.is_int())   rc = sqlite3_bind_int64(*out, idx, v.as_int());
            else if (v.is_float()) rc = sqlite3_bind_double(*out, idx, v.as_float());
            else {
                std::string s = v.to_string();
                rc = sqlite3_bind_text(*out, idx, s.c_str(),
                                       static_cast<int>(s.size()), SQLITE_TRANSIENT);
            }
            if (rc != SQLITE_OK) {
                error = std::string("sqlite: al enlazar el parametro ") +
                        std::to_string(idx) + ": " + sqlite3_errmsg(db);
                sqlite3_finalize(*out);
                *out = nullptr;
                return false;
            }
        }
        return true;
    }

    static Value column_value(sqlite3_stmt* stmt, int i) {
        switch (sqlite3_column_type(stmt, i)) {
            case SQLITE_NULL:    return Value::null();
            case SQLITE_INTEGER: return Value::integer(sqlite3_column_int64(stmt, i));
            case SQLITE_FLOAT:   return Value::real(sqlite3_column_double(stmt, i));
            default: {
                const auto* txt = sqlite3_column_text(stmt, i);
                int         len = sqlite3_column_bytes(stmt, i);
                return Value::str(txt ? std::string(reinterpret_cast<const char*>(txt),
                                                    static_cast<size_t>(len))
                                      : std::string());
            }
        }
    }
};

} // namespace

std::unique_ptr<DbDriver> make_sqlite_driver() {
    return std::make_unique<SqliteDriver>();
}

} // namespace odio
