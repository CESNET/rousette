/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <algorithm>
#include <experimental/iterator>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include "restconf/uri.h"

namespace rousette::restconf {

namespace {
namespace x3 = boost::spirit::x3;

auto set_zero = [](auto& ctx) { _val(ctx) = 0u; };
auto add = [](auto& ctx) {
    char c = std::tolower(_attr(ctx));
    _val(ctx) = _val(ctx) * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0');
};
const auto urlEncodedChar = x3::rule<class urlEncodedChar, unsigned>{"urlEncodedChar"} = x3::lit('%')[set_zero] >> x3::xdigit[add] >> x3::xdigit[add];

const auto reservedChars = x3::lit(':') | '/' | '?' | '#' | '[' | ']' | '@' | '!' | '$' | '&' | '\'' | '(' | ')' | '*' | '+' | ',' | ',' | ';' | '=' | '%'; // reserved chars according to RFC 3986, sec. 2.2 (plus %)
const auto keyValue = x3::rule<class keyValue, std::string>{"keyValue"} = *(urlEncodedChar | (x3::char_ - reservedChars));

const auto keyList = x3::rule<class keyList, std::vector<std::string>>{"keyList"} = keyValue % ',';
const auto identifier = x3::rule<class apiIdentifier, std::string>{"identifier"} = (x3::alpha | x3::char_('_')) >> *(x3::alnum | x3::char_('_') | x3::char_('-') | x3::char_('.'));
const auto apiIdentifier = x3::rule<class identifier, ApiIdentifier>{"apiIdentifier"} = -(identifier >> ':') >> identifier;
const auto listInstance = x3::rule<class keyList, PathSegment>{"listInstance"} = apiIdentifier >> -('=' >> keyList);
const auto fullyQualifiedApiIdentifier = x3::rule<class identifier, ApiIdentifier>{"apiIdentifier"} = identifier >> ':' >> identifier;
const auto fullyQualifiedListInstance = x3::rule<class keyList, PathSegment>{"listInstance"} = fullyQualifiedApiIdentifier >> -('=' >> keyList);
const auto uriGrammar = x3::rule<class grammar, std::vector<PathSegment>>{"grammar"} = x3::lit("/") >> x3::lit("restconf") >> "/" >> x3::lit("data") >> "/"
    >> fullyQualifiedListInstance >> -("/" >> listInstance % "/"); // RFC 8040, sec 3.5.3
}

URIParser::URIParser(const nghttp2::asio_http2::uri_ref& uri)
    : m_uri(uri)
{
}

std::optional<ResourcePath> URIParser::getPath() const
{
    std::vector<PathSegment> out;
    auto iter = std::begin(m_uri.path);
    auto end = std::end(m_uri.path);

    if (!x3::parse(iter, end, uriGrammar >> x3::eoi, out)) {
        return std::nullopt;
    }

    return out;
}

ResourcePath::ResourcePath(const std::vector<PathSegment>& segments)
    : m_segments(segments)
{
}

std::vector<PathSegment> ResourcePath::getSegments() const
{
    return m_segments;
}
}
