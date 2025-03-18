/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#pragma once
#include <boost/optional.hpp>
#include <boost/variant.hpp>
#include <libyang-cpp/Module.hpp>
#include <libyang-cpp/SchemaNode.hpp>
#include <map>
#include <optional>
#include <set>
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
    std::string name() const;
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
struct UnboundedDepth {
    bool operator==(const UnboundedDepth&) const = default;
};

namespace withDefaults {
struct Trim {
    bool operator==(const Trim&) const = default;
};
struct Explicit {
    bool operator==(const Explicit&) const = default;
};
struct ReportAll {
    bool operator==(const ReportAll&) const = default;
};
struct ReportAllTagged {
    bool operator==(const ReportAllTagged&) const = default;
};
}

namespace content {
struct AllNodes {
    bool operator==(const AllNodes&) const = default;
};
struct OnlyConfigNodes {
    bool operator==(const OnlyConfigNodes&) const = default;
};
struct OnlyNonConfigNodes {
    bool operator==(const OnlyNonConfigNodes&) const = default;
};
}

namespace insert {
struct First {
    bool operator==(const First&) const = default;
};
struct Last {
    bool operator==(const Last&) const = default;
};
struct Before {
    bool operator==(const Before&) const = default;
};
struct After {
    bool operator==(const After&) const = default;
};

using PointParsed = std::vector<PathSegment>;
}

namespace fields {
struct ParenExpr;
struct SemiExpr;
struct SlashExpr;

using Expr = boost::variant<boost::recursive_wrapper<SlashExpr>, boost::recursive_wrapper<ParenExpr>, boost::recursive_wrapper<SemiExpr>>;

struct ParenExpr {
    Expr lhs;
    boost::optional<Expr> rhs;

    ParenExpr() = default;
    ParenExpr(const Expr& lhs, const Expr& rhs) : ParenExpr(lhs, boost::optional<Expr>(rhs)) {}
    ParenExpr(const Expr& lhs, const boost::optional<Expr>& rhs = boost::none)
        : lhs(lhs)
        , rhs(rhs)
    {
    }

    bool operator==(const ParenExpr&) const = default;
};
struct SemiExpr {
    Expr lhs;
    boost::optional<Expr> rhs;

    SemiExpr() = default;
    SemiExpr(const Expr& lhs, const Expr& rhs) : SemiExpr(lhs, boost::optional<Expr>(rhs)) {}
    SemiExpr(const Expr& lhs, const boost::optional<Expr>& rhs = boost::none)
        : lhs(lhs)
        , rhs(rhs)
    {
    }

    bool operator==(const SemiExpr&) const = default;
};
struct SlashExpr {
    ApiIdentifier lhs;
    boost::optional<Expr> rhs;

    SlashExpr() = default;
    SlashExpr(const ApiIdentifier& lhs, const Expr& rhs) : SlashExpr(lhs, boost::optional<Expr>(rhs)) {}
    SlashExpr(const ApiIdentifier& lhs, const boost::optional<Expr>& rhs = boost::none)
        : lhs(lhs)
        , rhs(rhs)
    {
    }

    bool operator==(const SlashExpr&) const = default;
};
}

using QueryParamValue = std::variant<
    UnboundedDepth,
    unsigned int,
    std::string,
    withDefaults::Trim,
    withDefaults::Explicit,
    withDefaults::ReportAll,
    withDefaults::ReportAllTagged,
    content::AllNodes,
    content::OnlyNonConfigNodes,
    content::OnlyConfigNodes,
    insert::First,
    insert::Last,
    insert::Before,
    insert::After,
    insert::PointParsed,
    fields::Expr>;
using QueryParams = std::multimap<std::string, QueryParamValue>;
}

/** @brief Specifies request type and target as determined from URI */
struct RestconfRequest {
    enum class Type {
        GetData, ///< GET on a data resource or a complete-datastore resource
        CreateOrReplaceThisNode, ///< PUT on a data resource
        DeleteNode, ///< DELETE on a data resource
        Execute, ///< POST on a operation resource (Execute an RPC or an action)
        ExecuteInternal, ///< POST on a operation resource (RPC or action) that is not sent to sysrepo but handled in roustte directly
        CreateChildren, ///< POST on a data resource
        YangLibraryVersion, ///< Report ietf-yang-library version
        OptionsQuery, ///< Request for allowed HTTP methods for a path
        MergeData, ///< PATCH on a data resource or a complete-datastore resource
        RestconfRoot, ///< GET on restconf API root
        ListRPC, ///< GET on operation resource
    };

    Type type;
    std::optional<sysrepo::Datastore> datastore;
    std::string path;
    queryParams::QueryParams queryParams;

    RestconfRequest(Type type, const boost::optional<ApiIdentifier>& datastore, const std::string& path, const queryParams::QueryParams& queryParams);
};

struct RestconfStreamRequest {
    struct NetconfStream {
        libyang::DataFormat encoding;

        NetconfStream();
        NetconfStream(const libyang::DataFormat& encoding);
    } type;
    queryParams::QueryParams queryParams;
};

RestconfRequest asRestconfRequest(const libyang::Context& ctx, const std::string& httpMethod, const std::string& uriPath, const std::string& uriQueryString = "");
std::optional<libyang::SchemaNode> asLibyangSchemaNode(const libyang::Context& ctx, const std::vector<PathSegment>& pathSegments);
std::pair<std::string, PathSegment> asLibyangPathSplit(const libyang::Context& ctx, const std::string& uriPath);
std::vector<PathSegment> asPathSegments(const std::string& uriPath);
std::optional<std::variant<libyang::Module, libyang::SubmoduleParsed>> asYangModule(const libyang::Context& ctx, const std::string& uriPath);
RestconfStreamRequest asRestconfStreamRequest(const std::string& httpMethod, const std::string& uriPath, const std::string& uriQueryString);
std::set<std::string> allowedHttpMethodsForUri(const libyang::Context& ctx, const std::string& uriPath);

std::string fieldsToXPath(const libyang::Context& ctx, const std::string& prefix, const queryParams::fields::Expr& expr);
}
