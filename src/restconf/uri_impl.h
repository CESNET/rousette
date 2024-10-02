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

/** @brief Represents prefix type which is the part of the URI before that RESTCONF-encoded YANG path starts (e.g., /restconf/data). */
struct URIPrefix {
    /** @brief API resource type, i.e., the path segment just after /{+restconf} */
    enum class Type {
        RestconfRoot, // /{+restconf}/data (RFC 8040)
        BasicRestconfData, // /{+restconf}/data (RFC 8040)
        BasicRestconfOperations, // /{+restconf}/operations (RFC 8040)
        NMDADatastore, // /{+restconf}/ds/<datastore> (RFC 8527)
        YangLibraryVersion, // /{+restconf}/yang-library-version (RFC 8040, sec 3.3)
    };

    Type resourceType;
    boost::optional<ApiIdentifier> datastore; // /restconf/ds/ must also specify a datastore.

    URIPrefix();
    URIPrefix(Type resourceType, const boost::optional<ApiIdentifier>& datastore = boost::none);
    bool operator==(const URIPrefix&) const = default;
};

/** @brief Represents parsed URI path split into segments delimited by a `/` separator. */
struct URIPath {
    URIPrefix prefix;
    std::vector<PathSegment> segments;

    URIPath();
    URIPath(const std::vector<PathSegment>& pathSegments);
    URIPath(const URIPrefix& prefix, const std::vector<PathSegment>& pathSegments);

    bool operator==(const URIPath&) const = default;
};

/** @brief Represents parsed YANG module name and revision from YANG schema URI */
struct YangModule {
    std::string name;
    boost::optional<std::string> revision;

    bool operator==(const YangModule&) const = default;
};

std::optional<URIPath> parseUriPath(const std::string& uriPath);
std::optional<YangModule> parseModuleWithRevision(const std::string& uriPath);
std::optional<std::multimap<std::string, queryParams::QueryParamValue>> parseQueryParams(const std::string& queryString);
}
}

BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::impl::URIPrefix, resourceType, datastore);
BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::impl::URIPath, prefix, segments);
BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::impl::YangModule, name, revision);
BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::PathSegment, apiIdent, keys);
BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::PathSegment::KeyValue, prefix, keyValue);
BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::ApiIdentifier, prefix, identifier);
