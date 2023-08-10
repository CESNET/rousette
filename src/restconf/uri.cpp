/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <algorithm>
#include <experimental/iterator>
#include <iostream>
#include <libyang-cpp/SchemaNode.hpp>
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

/* reserved characters according to RFC 3986, sec. 2.2 with '%' added. The '%' character is not specified as reserved but it effectively is because
 * "Percent sign serves as the indicator for percent-encoded octets, it must be percent-encoded (...)" [RFC 3986, sec. 2.4]. */
const auto reservedChars = x3::lit(':') | '/' | '?' | '#' | '[' | ']' | '@' | '!' | '$' | '&' | '\'' | '(' | ')' | '*' | '+' | ',' | ',' | ';' | '=' | '%';
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

namespace {
std::optional<libyang::SchemaNode> findChildSchemaNode(libyang::SchemaNode node, const ApiIdentifier& childIdentifier)
{
    for (const auto& child : node.childInstantiables()) {
        if (child.name() == childIdentifier.identifier && (!childIdentifier.prefix || std::string{child.module().name()} == childIdentifier.prefix)) {
            return child;
        }
    }

    return std::nullopt;
}

/** @brief Returns fully qualified name of the nodeType
 *
 * @return string in the form <module>:<nodeName> if the parent module does not exist or is different from module of @p node else return only name of @p node.
 */
std::string canonicalName(libyang::SchemaNode currentNode)
{
    using namespace std::string_literals;

    std::string res;

    if (!currentNode.parent() || currentNode.parent()->module().name() != currentNode.module().name()) {
        res += std::string{currentNode.module().name()} + ":";
    }

    return res + std::string{currentNode.name()};
}

/** @brief Escapes key with the other type of quotes than found in the string.
 *
 *  @throws std::invalid_argument if both single and double quotes used in the input
 * */
std::string escapeListKey(const std::string& str)
{
    auto singleQuotes = str.find('\'') != std::string::npos;
    auto doubleQuotes = str.find('\"') != std::string::npos;

    if (singleQuotes && doubleQuotes) {
        throw std::invalid_argument("Encountered mixed single and double quotes in XPath; can't properly escape.");
    } else if (singleQuotes) {
        return '\"' + str + '\"';
    } else {
        return '\'' + str + '\'';
    }
}
}

std::optional<ResourcePath> parseUriPath(const std::string& uriPath)
{
    std::vector<PathSegment> out;
    auto iter = std::begin(uriPath);
    auto end = std::end(uriPath);

    if (!x3::parse(iter, end, uriGrammar >> x3::eoi, out)) {
        return std::nullopt;
    }

    return out;
}

ApiIdentifier::ApiIdentifier() = default;

ApiIdentifier::ApiIdentifier(const std::string& prefix, const std::string& identifier)
    : prefix(prefix)
    , identifier(identifier)
{
}

ApiIdentifier::ApiIdentifier(const std::string& identifier)
    : prefix(boost::none)
    , identifier(identifier)
{
}

PathSegment::PathSegment() = default;

PathSegment::PathSegment(const ApiIdentifier& apiIdent, const std::vector<std::string>& keys)
    : apiIdent(apiIdent)
    , keys(keys)
{
}

ResourcePath::ResourcePath(const std::vector<PathSegment>& segments)
    : m_segments(segments)
{
}

std::vector<PathSegment> ResourcePath::getSegments() const
{
    return m_segments;
}

std::string ResourcePath::asLibyangPath(const libyang::Context& ctx) const
{
    std::optional<libyang::SchemaNode> currentNode;
    std::string res;

    for (auto it = m_segments.begin(); it != m_segments.end(); ++it) {
        auto prev = currentNode;
        currentNode = currentNode ? findChildSchemaNode(*currentNode, it->apiIdent) : ctx.findPath("/" + *it->apiIdent.prefix + ":" + it->apiIdent.identifier);
        if (!currentNode) {
            throw std::runtime_error("Node path not found, prev=" + prev->path());
        }

        res += "/" + canonicalName(*currentNode);

        if (currentNode->nodeType() == libyang::NodeType::List) {
            const auto& listKeys = currentNode->asList().keys();

            if (it->keys.size() == listKeys.size()) {
                // FIXME: use std::views::zip in C++23
                auto itKeyValue = it->keys.begin();
                for (auto itKeyName = listKeys.begin(); itKeyName != listKeys.end(); ++itKeyName, ++itKeyValue) {
                    res += '[' + std::string{itKeyName->name()} + "=" + escapeListKey(*itKeyValue) + ']';
                }
            } else if (it->keys.size() > 0 && it->keys.size() != listKeys.size()) {
                throw std::runtime_error("List keys count mismatch");
            }
        } else if (currentNode->nodeType() == libyang::NodeType::Leaflist) {
            if (it->keys.size() == 1) {
                res += "[.=" + escapeListKey(it->keys.front()) + ']';
            } else if (it->keys.size() > 1) {
                throw std::runtime_error("Leaflist keys count mismatch");
            }
        } else if (it->keys.size() > 0) {
            throw std::runtime_error("Current node is neither list nor leaflist but keys were specified");
        }
    }

    return res;
}
}
