/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#pragma once
#include <boost/optional.hpp>
#include <libyang-cpp/Module.hpp>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <sysrepo-cpp/Enum.hpp>
#include <variant>

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

/** @brief Helper namespace containing named query parameter values so we do not pollute parent namespace */
namespace queryParams {
    struct UnboundedDepth{
        bool operator==(const UnboundedDepth&) const { return true; }
    };

    using QueryParamValue = std::variant<UnboundedDepth, unsigned int>;
    using QueryParams = std::multimap<std::string, QueryParamValue>;
}

/** @brief Specifies request type and target as determined from URI */
struct RestconfRequest {
    enum class Type {
        GetData, ///< GET on a data resource or a complete-datastore resource
        CreateOrReplaceThisNode, ///< PUT on a data resource
        DeleteNode, ///< DELETE on a data resource
        Execute, ///< POST on a operation resource (Execute an RPC or an action)
        CreateChildren, ///< POST on a data resource
        YangLibraryVersion ///< Report ietf-yang-library version
    };

    Type type;
    std::optional<sysrepo::Datastore> datastore;
    std::string path;
    queryParams::QueryParams queryParams;

    RestconfRequest(Type type, const boost::optional<ApiIdentifier>& datastore, const std::string& path, const queryParams::QueryParams& queryParams);
};

RestconfRequest asRestconfRequest(const libyang::Context& ctx, const std::string& httpMethod, const std::string& uriPath, const std::string& uriQueryString = "");
std::pair<std::string, PathSegment> asLibyangPathSplit(const libyang::Context& ctx, const std::string& uriPath);
std::optional<std::variant<libyang::Module, libyang::SubmoduleParsed>> asYangModule(const libyang::Context& ctx, const std::string& uriPath);
}
