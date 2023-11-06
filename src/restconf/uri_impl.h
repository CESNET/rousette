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

/** @brief Represents parsed URI path split into segments delimited by a `/` separator. */
struct ResourcePath {
    boost::optional<ApiIdentifier> datastore;
    std::vector<PathSegment> segments;

    ResourcePath();
    ResourcePath(const std::vector<PathSegment>& pathSegments);
    ResourcePath(const boost::optional<ApiIdentifier>& datastore, const std::vector<PathSegment>& pathSegments);

    bool operator==(const ResourcePath&) const = default;
};

std::optional<ResourcePath> parseUriPath(const std::string& uriPath);
}
}

BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::impl::ResourcePath, datastore, segments);
BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::PathSegment, apiIdent, keys);
BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::ApiIdentifier, prefix, identifier);
