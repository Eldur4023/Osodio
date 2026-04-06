#include <osodio/osodio.hpp>
#include <iostream>
#include <cassert>

using namespace osodio;

namespace osodio {
struct User {
    std::string name;
    int age;
};
OSODIO_SCHEMA(User, name, age);
OSODIO_VALIDATE(User,
    check(age, min(18), max(99)),
    check(name, len_min(3))
);
}

void test_validation_success() {
    User u = {"Antigravity", 25};
    assert(has_validate<User>::value);
    
    // Manual validation call
    validate(u); 
    std::cout << "Validation success test passed!" << std::endl;
}

void test_validation_failure() {
    User u = {"A", 15};
    try {
        validate(u);
        assert(false && "Should have thrown ValidationError");
    } catch (const ValidationError& e) {
        assert(e.messages.size() == 2);
        std::cout << "Validation failure test passed! Errors: " << e.messages.size() << std::endl;
    }
}

void test_handler_interception() {
    bool called = false;
    auto lambda = [&](Body<User> body) {
        called = true;
    };

    Request req;
    req.body = R"({"name": "A", "age": 15})"; // Invalid
    Response res;

    HandlerTraits<decltype(lambda)>::call(lambda, req, res);
    
    assert(!called);
    assert(res.status_code() == 400);
    std::cout << "Handler interception test passed!" << std::endl;
}

int main() {
    try {
        test_validation_success();
        test_validation_failure();
        test_handler_interception();
        std::cout << "All validation tests passed!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
