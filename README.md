# Osodio Web Framework

Osodio is a high-performance, ergonomic C++20 web framework inspired by FastAPI and Express. It combines the speed of native C++ with modern developer experience.

## ✨ Core Features

- **Radix Tree Router**: O(k) route matching with support for parameters (`:id`), wildcards (`*`), and normalization.
- **OSODIO_SCHEMA**: Declarative macro for automatic JSON serialization and deserialization.
- **FastAPI-style Handlers**: Automatic type deduction and dependency injection in lambda handlers.
- **Zero-Boilerplate**: Focus on your logic, let Osodio handle the HTTP plumbing.
- **Asynchronous Foundation**: Built on a non-blocking TCP server and event loop.

---

## 🚀 Getting Started

### Define your Models
Use `OSODIO_SCHEMA` to make any struct JSON-ready:

```cpp
struct User {
    std::string id;
    std::string name;
    int age;
};

// Generates to_json and from_json automatically
OSODIO_SCHEMA(User, id, name, age);
```

### Add Validation Rules
Osodio allows you to enforce constraints on your data using `OSODIO_VALIDATE`:

```cpp
struct CreateUserBody {
    std::string name;
    int age;
};
OSODIO_SCHEMA(CreateUserBody, name, age);

// Define validation rules
OSODIO_VALIDATE(CreateUserBody,
    check(name, len_min(3)),
    check(age, min(18), max(99))
);
```

If a request fails validation, the server automatically returns a `400 Bad Request` with a list of error messages.

---

## 🛠️ API Reference

### Routing
- `app.get(path, handler)`
- `app.post(path, handler)`
- `app.put(path, handler)`
- `app.del(path, handler)`
- `app.any(path, handler)`

### Handler Arguments
- `PathParam<T, "name">`: Extracts a parameter from the URL and converts it to `T`.
- `Body<T>`: Parses the request body as JSON into type `T`.
- `Request&`: Access the raw request object.
- `Response&`: Access the raw response object.

### Response Building
If your handler takes `Response& res`, you can use the builder API:
- `res.status(code)`: Set HTTP status (e.g., 201, 404).
- `res.json(obj)`: Send a JSON response (requires `OSODIO_SCHEMA` or `nlohmann::json`).
- `res.html("file.html")`: Render a template from `./templates`.
- `res.text("plain text")`: Send a plain text response.

---

## 🏗️ Building the Project

Osodio uses CMake 3.20+ and requires a C++20 compliant compiler (GCC 11+, Clang 13+).

```bash
mkdir build && cd build
cmake ..
make
./example
```

---

## 📜 Development Plan
Osodio is evolving. Current goals include:
- [x] Radix Tree Router (O(k) scaling)
- [x] OSODIO_SCHEMA (JSON Automation)
- [x] Type Deduction in Handlers
- [ ] Declarative Validators (min, max, email)
- [ ] C++20 Coroutines (Task<T> and co_await)
- [ ] HTTPS/TLS 1.3 support
