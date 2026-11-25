#include "fixcore/dictionary.hpp"

#include <cctype>
#include <set>

#include "fixcore/order.hpp"

namespace fixcore {

const MessageFieldRule* MessageDef::rule(int tag) const {
    for (const auto& r : fields)
        if (r.tag == tag) return &r;
    return nullptr;
}

const FieldDef* Dictionary::field(int tag) const {
    for (const auto& f : fields)
        if (f.tag == tag) return &f;
    return nullptr;
}

const FieldDef* Dictionary::field(std::string_view name) const {
    for (const auto& f : fields)
        if (f.name == name) return &f;
    return nullptr;
}

const MessageDef* Dictionary::message(std::string_view msg_type) const {
    for (const auto& m : messages)
        if (m.msg_type == msg_type) return &m;
    return nullptr;
}

bool value_matches_type(FieldType type, std::string_view v) {
    auto all_digits = [](std::string_view s) {
        if (s.empty()) return false;
        for (char c : s)
            if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        return true;
    };
    switch (type) {
        case FieldType::Int: case FieldType::SeqNum: case FieldType::NumInGroup: case FieldType::Length:
            return all_digits(v[0] == '-' ? v.substr(1) : v);
        case FieldType::Qty: case FieldType::Price: case FieldType::PriceOffset: case FieldType::Amt:
        case FieldType::Float: case FieldType::Percentage:
            return is_decimal(v, true);
        case FieldType::Char:
            return v.size() == 1;
        case FieldType::Boolean:
            return v == "Y" || v == "N";
        case FieldType::UtcTimestamp:
            // YYYYMMDD-HH:MM:SS[.sss...]
            return v.size() >= 17 && all_digits(v.substr(0, 8)) && v[8] == '-' && v[11] == ':' && v[14] == ':';
        case FieldType::UtcDateOnly: case FieldType::LocalMktDate:
            return v.size() == 8 && all_digits(v);
        default:
            return !v.empty();
    }
}

std::vector<std::string> Dictionary::validate(const Message& msg) const {
    std::vector<std::string> errors;
    const MessageDef* def = message(msg.msg_type());
    if (!def) {
        errors.push_back("message type " + msg.msg_type() + " is not defined for this venue");
        return errors;
    }
    std::set<int> seen;
    for (const auto& f : msg.body()) {
        seen.insert(f.tag);
        const FieldDef* fd = field(f.tag);
        if (!def->rule(f.tag)) {
            errors.push_back(std::string(def->name) + " does not allow tag " + std::to_string(f.tag) +
                             (fd ? " (" + std::string(fd->name) + ")" : ""));
            continue;
        }
        if (!fd) continue;
        if (!fd->values.empty()) {
            bool ok = false;
            for (const auto& ev : fd->values) ok |= ev.value == f.value;
            if (!ok) errors.push_back(std::string(fd->name) + "=" + f.value + " is not a valid value");
        } else if (!value_matches_type(fd->type, f.value)) {
            errors.push_back(std::string(fd->name) + "=" + f.value + " has the wrong format");
        }
    }
    for (const auto& r : def->fields) {
        bool required = r.required;
        for (const auto& c : r.required_when) {
            if (auto v = msg.get(c.tag))
                for (auto cv : c.values) required |= (cv == *v);
        }
        if (required && !seen.count(r.tag)) {
            const FieldDef* fd = field(r.tag);
            errors.push_back(std::string(def->name) + " is missing required " +
                             (fd ? std::string(fd->name) : std::string()) + "(" + std::to_string(r.tag) + ")");
        }
    }
    return errors;
}

}  // namespace fixcore
