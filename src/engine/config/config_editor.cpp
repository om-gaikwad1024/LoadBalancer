#include "config/config_editor.h"

#include <windows.h>

#include <fstream>

namespace lb::admin {

namespace {

bool is_string(const Document& v, std::string_view s) {
    return v.is_string() && v.get_ref<const std::string&>() == s;
}

Document* find_group(Document& doc, std::string_view name) {
    auto it = doc.find("groups");
    if (it == doc.end() || !it->is_array()) return nullptr;
    for (auto& g : *it) {
        if (g.is_object() && g.contains("name") && is_string(g["name"], name)) return &g;
    }
    return nullptr;
}

// The backend object with this id, and the array holding it.
Document* find_backend(Document& doc, std::string_view id, Document** list = nullptr) {
    auto it = doc.find("groups");
    if (it == doc.end() || !it->is_array()) return nullptr;
    for (auto& g : *it) {
        if (!g.is_object() || !g.contains("backends") || !g["backends"].is_array()) continue;
        for (auto& b : g["backends"]) {
            if (b.is_object() && b.contains("id") && is_string(b["id"], id)) {
                if (list != nullptr) *list = &g["backends"];
                return &b;
            }
        }
    }
    return nullptr;
}

const char* type_name(RouteRule::Type type) {
    switch (type) {
        case RouteRule::Type::PathPrefix: return "path_prefix";
        case RouteRule::Type::PathGlob: return "path_glob";
        case RouteRule::Type::Header: return "header";
        case RouteRule::Type::Cookie: return "cookie";
    }
    return "path_prefix";
}

}  // namespace

std::string add_backend(Document& doc, const BackendFields& b) {
    Document* group = find_group(doc, b.group);
    if (group == nullptr) return "no group \"" + b.group + "\"";
    if (!group->contains("backends") || !(*group)["backends"].is_array()) return "group \"" + b.group + "\" has no backend list";
    (*group)["backends"].push_back(
        Document{{"id", b.id}, {"address", b.address}, {"port", b.port}, {"weight", b.weight}, {"drain", "keep"}});
    return {};
}

std::string update_backend(Document& doc, const BackendFields& b) {
    Document* backend = find_backend(doc, b.id);
    if (backend == nullptr) return "no backend \"" + b.id + "\"";
    (*backend)["address"] = b.address;
    (*backend)["port"] = b.port;
    (*backend)["weight"] = b.weight;
    return {};
}

std::string remove_backend(Document& doc, std::string_view id) {
    Document* list = nullptr;
    if (find_backend(doc, id, &list) == nullptr) return "no backend \"" + std::string(id) + "\"";
    for (std::size_t i = 0; i < list->size(); ++i) {
        if (is_string((*list)[i]["id"], id)) {
            list->erase(i);
            break;
        }
    }
    return {};
}

std::string set_backend_drain(Document& doc, std::string_view id, std::string_view directive) {
    Document* backend = find_backend(doc, id);
    if (backend == nullptr) return "no backend \"" + std::string(id) + "\"";
    (*backend)["drain"] = directive;
    return {};
}

std::string set_routing(Document& doc, const RoutingConfig& routing) {
    Document rules = Document::array();
    for (const auto& r : routing.rules) {
        rules.push_back(Document{{"id", r.id},
                                 {"type", type_name(r.type)},
                                 {"field", r.field.empty() ? Document(nullptr) : Document(r.field)},
                                 {"value", r.value ? Document(*r.value) : Document(nullptr)},
                                 {"group", r.group}});
    }
    doc["routing"] = Document{{"default_group", routing.default_group}, {"rules", rules}};
    return {};
}

bool write_file_atomically(const std::filesystem::path& path, std::string_view text, std::string* error) {
    std::filesystem::path temp = path;
    temp += L".lb-saving";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) {
            *error = "cannot write a temporary file next to the config file";
            std::error_code ec;
            std::filesystem::remove(temp, ec);
            return false;
        }
    }
    if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *error = "cannot replace the config file (Windows error " + std::to_string(::GetLastError()) + ")";
        std::error_code ec;
        std::filesystem::remove(temp, ec);
        return false;
    }
    return true;
}

}  // namespace lb::admin
