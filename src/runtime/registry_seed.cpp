// Fabric Topology - vendor-neutral topology governance runtime.
// Copyright 2026 Summon Software Labs.
// Distributed under the Apache License, Version 2.0.

#include "fabric_topology/runtime/registry_seed.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fabric_topology::runtime {

namespace {

[[nodiscard]] Status make_status(Outcome outcome, const char* code) {
    Status status;
    status.set_outcome(outcome).set_code(code);
    return status;
}

[[nodiscard]] bool record_less(const EntityRecord& a, const EntityRecord& b) {
    if (a.entity_id != b.entity_id) {
        return a.entity_id < b.entity_id;
    }
    return a.generation < b.generation;
}

}  // namespace

Status write_registry_seed(const std::string& path, const std::vector<EntityRecord>& records) {
    std::vector<EntityRecord> sorted = records;
    std::sort(sorted.begin(), sorted.end(), record_less);

    std::ostringstream out;
    out << "# Fabric Registry seed consumed by the Fabric Topology tools.\n";
    out << "# entity <identifier> <CLASS> <generation> [superseded-by <identifier>] [retired]\n";
    for (const EntityRecord& record : sorted) {
        out << "entity " << record.entity_id << ' ' << to_string(record.entity_class) << ' '
            << record.generation.to_string();
        if (record.superseded && !record.superseded_by.empty()) {
            out << " superseded-by " << record.superseded_by;
        }
        if (record.retired) {
            out << " retired";
        }
        out << '\n';
    }

    const std::filesystem::path target(path);
    const std::filesystem::path temporary = target.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) {
            Status status = make_status(Outcome::PersistenceIoFailure, "registry_seed.open_failed");
            status.with("path", temporary.string());
            return status;
        }
        const std::string text = out.str();
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.flush();
        if (!stream) {
            Status status = make_status(Outcome::PersistenceIoFailure, "registry_seed.write_failed");
            status.with("path", temporary.string());
            return status;
        }
    }
    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        Status status = make_status(Outcome::PersistenceIoFailure, "registry_seed.rename_failed");
        status.with("path", path);
        status.with("detail", error.message());
        return status;
    }
    Status status = make_status(Outcome::Ok, "registry_seed.written");
    status.with("path", path);
    status.with("records", std::to_string(sorted.size()));
    return status;
}

Status load_registry_seed(const std::string& path, IEntityDirectory& directory) {
    InMemoryEntityDirectory* target = dynamic_cast<InMemoryEntityDirectory*>(&directory);
    if (target == nullptr) {
        return make_status(Outcome::UnsupportedOperation, "registry_seed.directory_not_writable");
    }

    std::ifstream stream(path);
    if (!stream) {
        Status status = make_status(Outcome::PersistenceIoFailure, "registry_seed.open_failed");
        status.with("path", path);
        return status;
    }

    std::size_t loaded = 0;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        ++line_number;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        if (start >= line.size() || line[start] == '#') {
            continue;
        }

        std::istringstream fields(line.substr(start));
        std::string keyword;
        EntityRecord record;
        std::string entity_class_text;
        std::string generation_text;
        fields >> keyword >> record.entity_id >> entity_class_text >> generation_text;
        if (keyword != "entity" || !fields) {
            Status status = make_status(Outcome::MalformedRequest, "registry_seed.malformed_line");
            status.with("path", path);
            status.with("line", std::to_string(line_number));
            return status;
        }
        const auto entity_class = entity_class_from_string(entity_class_text);
        if (!entity_class.has_value()) {
            Status status = make_status(Outcome::MalformedRequest, "registry_seed.unknown_class");
            status.with("line", std::to_string(line_number));
            status.with("class", entity_class_text);
            return status;
        }
        record.entity_class = *entity_class;
        try {
            record.generation = EntityGeneration{static_cast<std::uint64_t>(std::stoull(generation_text))};
        } catch (const std::exception&) {
            Status status = make_status(Outcome::MalformedRequest, "registry_seed.bad_generation");
            status.with("line", std::to_string(line_number));
            return status;
        }

        std::string trailer;
        while (fields >> trailer) {
            if (trailer == "retired") {
                record.retired = true;
                continue;
            }
            if (trailer == "superseded-by") {
                std::string successor;
                if (!(fields >> successor)) {
                    Status status = make_status(Outcome::MalformedRequest,
                                                "registry_seed.missing_successor");
                    status.with("line", std::to_string(line_number));
                    return status;
                }
                record.superseded = true;
                record.superseded_by = successor;
                continue;
            }
            Status status = make_status(Outcome::MalformedRequest, "registry_seed.unknown_field");
            status.with("line", std::to_string(line_number));
            status.with("field", trailer);
            return status;
        }

        Status added = target->add(record);
        if (added.outcome() != Outcome::Committed) {
            added.with("line", std::to_string(line_number));
            return added;
        }
        ++loaded;
    }

    Status status = make_status(Outcome::Ok, "registry_seed.loaded");
    status.with("path", path);
    status.with("records", std::to_string(loaded));
    return status;
}

}  // namespace fabric_topology::runtime
