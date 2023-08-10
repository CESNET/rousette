/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#pragma once
#include <boost/fusion/adapted/struct/adapt_struct.hpp>
#include <boost/spirit/home/x3.hpp>
#include <string>
#include <sysrepo-cpp/Session.hpp>
#include "restconf/uri.h"

namespace rousette::restconf {

namespace impl {

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

/** @brief Represents parsed URI path split into segments delimited by a `/` separator. */
struct ResourcePath {
    std::vector<PathSegment> segments;

    ResourcePath(const std::vector<PathSegment>& pathSegments);

    bool operator==(const ResourcePath&) const = default;
};

std::optional<ResourcePath> parseUriPath(const std::string& uriPath);
}
}

BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::impl::PathSegment, apiIdent, keys);
BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::impl::ApiIdentifier, prefix, identifier);
