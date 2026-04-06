#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>
#include "types.hpp"

namespace osodio {

struct RouteMatch {
    bool    found   = false;
    Handler handler = nullptr;
    std::unordered_map<std::string, std::string> params;
};

class Router {
public:
    Router();
    void add(std::string method, std::string pattern, Handler handler);
    RouteMatch match(const std::string& method, const std::string& path) const;

private:
    enum class NodeType { STATIC, PARAM, WILDCARD };

    struct Node {
        std::string segment;
        NodeType    type = NodeType::STATIC;
        std::unordered_map<std::string, Handler> handlers; // method -> handler
        std::vector<std::unique_ptr<Node>> children;

        Node* find_static_child(const std::string& seg) const;
        Node* find_param_child() const;
        Node* find_wildcard_child() const;
    };

    std::unique_ptr<Node> root_;

    // Converts {id} → :id
    static std::string normalize_pattern(const std::string& p);
    
    // Helper for recursive matching
    bool match_recursive(
        const Node* node,
        const std::vector<std::string>& segments,
        size_t index,
        const std::string& method,
        std::unordered_map<std::string, std::string>& params,
        Handler& out_handler) const;
};

} // namespace osodio
