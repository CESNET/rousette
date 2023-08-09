/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <boost/fusion/adapted/struct/adapt_struct.hpp>
#include <boost/spirit/home/x3.hpp>
#include <nghttp2/asio_http2.h>
#include <string>
#include <sysrepo-cpp/Session.hpp>

namespace rousette::restconf {

struct ApiIdentifier {
    boost::optional<std::string> prefix;
    std::string identifier;

    bool operator==(const ApiIdentifier&) const = default;
};

struct PathSegment {
    ApiIdentifier apiIdent;
    std::vector<std::string> keys;

    bool operator==(const PathSegment&) const = default;
};

class ResourcePath {
public:
    ResourcePath(const std::vector<PathSegment>& pathSegments);

    bool operator==(const ResourcePath&) const = default;
    std::vector<PathSegment> getSegments() const;

private:
    std::vector<PathSegment> m_segments;
};

class URIParser {
public:
    URIParser(const nghttp2::asio_http2::uri_ref& uri);
    std::optional<ResourcePath> getPath() const;

private:
    nghttp2::asio_http2::uri_ref m_uri;
};

}

BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::PathSegment, apiIdent, keys);
BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::ApiIdentifier, prefix, identifier);
