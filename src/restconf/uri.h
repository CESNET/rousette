/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#pragma once
#include <boost/optional.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <sysrepo-cpp/Enum.hpp>

namespace libyang {
class Context;
}

namespace rousette::restconf {
/** @brief Identifier referring to a YANG node name with an optional prefix.
 *
 * Corresponds to api-identifier rule in the grammar for data resource idenfitifer (https://datatracker.ietf.org/doc/html/rfc8040#section-3.5.3.1).
 */
struct ApiIdentifier {
    boost::optional<std::string> prefix;
    std::string identifier;

    ApiIdentifier();
    ApiIdentifier(const std::string& prefix, const std::string& identifier);
    ApiIdentifier(const std::string& identifier);

    bool operator==(const ApiIdentifier&) const = default;
};

/** @brief Represents "one-level" of the RESTCONF URI path, i.e., an ApiIdentifier that can be followed by list keys or leaf-list value.
 *
 * Corresponds to list-instance rule in the grammar for data resource idenfitifer (https://datatracker.ietf.org/doc/html/rfc8040#section-3.5.3.1).
 */
struct PathSegment {
    ApiIdentifier apiIdent;
    std::vector<std::string> keys;

    PathSegment();
    PathSegment(const ApiIdentifier& apiIdent, const std::vector<std::string>& keys = {});

    bool operator==(const PathSegment&) const = default;
};

/** @brief Specifies request type and target as determined from URI */
struct RestconfRequest {
    enum class Type {
        GetData, ///< GET on a data resource or a complete-datastore resource
        CreateOrReplaceThisNode, ///< PUT on a data resource
    };

    Type type;
    std::optional<sysrepo::Datastore> datastore;
    std::string path;

    RestconfRequest(Type type, const boost::optional<ApiIdentifier>& datastore, const std::string& path);
};

RestconfRequest asRestconfRequest(const libyang::Context& ctx, const std::string& httpMethod, const std::string& uriPath);
std::pair<std::string, PathSegment> asLibyangPathSplit(const libyang::Context& ctx, const std::string& uriPath);
}
