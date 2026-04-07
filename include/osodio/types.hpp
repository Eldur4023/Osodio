#pragma once
#include <functional>

namespace osodio {

class Request;
class Response;

using Handler      = std::function<void(Request&, Response&)>;
using NextFn       = std::function<void()>;
using Middleware   = std::function<void(Request&, Response&, NextFn)>;
using DispatchFn   = std::function<void(Request&, Response&)>;
using ErrorHandler = std::function<void(int code, Request&, Response&)>;

} // namespace osodio
