#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include "request.hpp"

namespace osodio {

// ─── MultipartPart ────────────────────────────────────────────────────────────
//
// One part of a multipart/form-data body.
//
//   for (auto& part : *parts) {
//       if (!part.filename.empty()) {
//           // file upload: part.name, part.filename, part.content_type, part.body
//       } else {
//           // regular field: part.name, part.body
//       }
//   }

struct MultipartPart {
    std::string name;            // Content-Disposition: form-data; name="..."
    std::string filename;        // present only for file-input fields
    std::string content_type;    // Content-Type of this part (may be empty)
    std::string body;            // raw bytes of this part

    // All part headers, lowercase-keyed.
    std::unordered_map<std::string, std::string> headers;
};

// ─── parse_multipart() ───────────────────────────────────────────────────────
//
// Parses a multipart/form-data request body into individual parts.
// Returns nullopt if:
//   • Content-Type is not multipart/form-data
//   • The boundary is missing or the body is malformed
//
//   auto parts = osodio::parse_multipart(req);
//   if (!parts) { ... handle error ... }
//   for (auto& p : *parts) { ... }

inline std::optional<std::vector<MultipartPart>>
parse_multipart(const Request& req) {
    // ── 1. Extract boundary from Content-Type ─────────────────────────────────
    auto ct_opt = req.header("content-type");
    if (!ct_opt) return std::nullopt;
    const std::string& ct = *ct_opt;

    if (ct.rfind("multipart/form-data", 0) != 0) return std::nullopt;

    auto bpos = ct.find("boundary=");
    if (bpos == std::string::npos) return std::nullopt;
    bpos += 9;
    // Skip optional leading whitespace
    while (bpos < ct.size() && ct[bpos] == ' ') ++bpos;
    bool quoted = bpos < ct.size() && ct[bpos] == '"';
    if (quoted) ++bpos;
    std::string boundary_val;
    while (bpos < ct.size()) {
        char c = ct[bpos++];
        if (quoted  && c == '"')                            break;
        if (!quoted && (c == ';' || c == ' ' || c == '\r')) break;
        boundary_val += c;
    }
    if (boundary_val.empty()) return std::nullopt;

    // RFC 2046 §5.1.1: delimiter = "--" + boundary
    const std::string delim     = "--" + boundary_val;
    const std::string final_delim = delim + "--";
    const std::string& body     = req.body;

    // ── 2. Find the first delimiter ──────────────────────────────────────────
    size_t pos = body.find(delim);
    if (pos == std::string::npos) return std::nullopt;
    pos += delim.size();
    if (pos < body.size() && body.substr(pos, 2) == "\r\n") pos += 2;

    std::vector<MultipartPart> parts;

    // ── 3. Parse each part ───────────────────────────────────────────────────
    while (pos < body.size()) {
        // Find the end of this part (next delimiter, possibly with leading CRLF)
        const std::string next_delim = "\r\n" + delim;
        size_t end = body.find(next_delim, pos);
        if (end == std::string::npos) break;

        std::string_view part_view(body.data() + pos, end - pos);

        // Split part into headers and body at the first blank line
        auto blank = part_view.find("\r\n\r\n");
        if (blank == std::string_view::npos) break;

        std::string_view hdr_block  = part_view.substr(0, blank);
        std::string_view part_body  = part_view.substr(blank + 4);

        MultipartPart part;
        part.body = std::string(part_body);

        // ── Parse part headers ─────────────────────────────────────────────
        std::string hdr_str(hdr_block);
        std::istringstream hss(hdr_str);
        std::string hline;
        while (std::getline(hss, hline)) {
            if (!hline.empty() && hline.back() == '\r') hline.pop_back();
            if (hline.empty()) continue;
            auto colon = hline.find(':');
            if (colon == std::string::npos) continue;
            std::string key = hline.substr(0, colon);
            std::string val = (colon + 2 <= hline.size()) ? hline.substr(colon + 2) : "";
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);

            if (key == "content-disposition") {
                // Extract name="..." and optional filename="..."
                auto extract = [&](const std::string& attr) -> std::string {
                    std::string needle = attr + "=\"";
                    auto p = val.find(needle);
                    if (p == std::string::npos) {
                        // Also try unquoted: attr=value
                        needle = attr + "=";
                        p = val.find(needle);
                        if (p == std::string::npos) return "";
                        p += needle.size();
                        auto q = val.find_first_of("; \r\n", p);
                        return val.substr(p, q == std::string::npos ? std::string::npos : q - p);
                    }
                    p += needle.size();
                    auto q = val.find('"', p);
                    return q == std::string::npos ? "" : val.substr(p, q - p);
                };
                part.name     = extract("name");
                part.filename = extract("filename");
            } else if (key == "content-type") {
                part.content_type = val;
            }
            part.headers[std::move(key)] = std::move(val);
        }

        parts.push_back(std::move(part));

        // Advance past the delimiter
        pos = end + next_delim.size();
        // Check for final boundary ("--") or next part ("\r\n")
        if (pos + 2 <= body.size() && body.substr(pos, 2) == "--") break;
        if (pos + 2 <= body.size() && body.substr(pos, 2) == "\r\n") pos += 2;
    }

    return parts;
}

} // namespace osodio
