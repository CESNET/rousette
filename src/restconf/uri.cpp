/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include "restconf/uri_impl.h"

using namespace std::string_literals;

namespace rousette::restconf {
namespace impl {

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
const auto uriGrammar = x3::rule<class grammar, std::vector<PathSegment>>{"grammar"} = x3::lit("/") >> x3::lit("restconf") >> "/" >> x3::lit("data") >>
    -x3::lit("/") >> -(fullyQualifiedListInstance >> -("/" >> listInstance % "/")); // RFC 8040, sec 3.5.3
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

ResourcePath::ResourcePath(const std::vector<PathSegment>& segments)
    : segments(segments)
{
}
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

namespace {
std::optional<libyang::SchemaNode> findChildSchemaNode(libyang::SchemaNode node, const ApiIdentifier& childIdentifier)
{
    for (const auto& child : node.childInstantiables()) {
        if (child.name() == childIdentifier.identifier) {
            // If the prefix is not specified then we must ensure that child's module is the same as the node's module so that we don't accidentally return a child that was inserted here via an augment
            if (
                (!childIdentifier.prefix && std::string{node.module().name()} == std::string{child.module().name()}) || (childIdentifier.prefix && std::string{child.module().name()} == *childIdentifier.prefix)) {
                return child;
            }
        }
    }

    return std::nullopt;
}

/** @brief Construct a fully qualified name of the node if needed
 *
 * @return string in the form <module>:<nodeName> if the parent module does not exist or is different from module of @p node else return only name of @p node.
 */
std::string maybeQualified(libyang::SchemaNode currentNode)
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
 *  @throws InvalidURIException if both single and double quotes used in the input
 * */
std::string escapeListKey(const std::string& str)
{
    auto singleQuotes = str.find('\'') != std::string::npos;
    auto doubleQuotes = str.find('\"') != std::string::npos;

    if (singleQuotes && doubleQuotes) {
        throw InvalidURIException("Encountered mixed single and double quotes in XPath. Can't properly escape.");
    } else if (singleQuotes) {
        return '\"' + str + '\"';
    } else {
        return '\'' + str + '\'';
    }
}

std::string apiIdentName(const ApiIdentifier& apiIdent)
{
    if (!apiIdent.prefix) {
        return apiIdent.identifier;
    }
    return *apiIdent.prefix + ":" + apiIdent.identifier;
}

bool isValidDataResource(libyang::SchemaNode node)
{
    switch (node.nodeType()) {
    case libyang::NodeType::Container:
    case libyang::NodeType::Leaf:
    case libyang::NodeType::AnyXML:
    case libyang::NodeType::AnyData:
    /* querying the actual (leaf-)list node is not a valid data resource, only (leaf-)list entries are.
     * Yet we consider this as a valid resource here. If this function is called we already checked if the keys are specified in the caller.
     * If they were correctly specified, then we are querying instance. If not, then the code already throwed.
     */
    case libyang::NodeType::Leaflist:
    case libyang::NodeType::List:
        return true;
    default:
        return false;
    }
}

std::string asLibyangPath(const libyang::Context& ctx, const std::vector<PathSegment>::const_iterator& begin, const std::vector<PathSegment>::const_iterator& end)
{
    std::optional<libyang::SchemaNode> currentNode;
    std::string res;

    for (auto it = begin; it != end; ++it) {
        if (auto prevNode = currentNode) {
            if (!(currentNode = findChildSchemaNode(*currentNode, it->apiIdent))) {
                throw InvalidURIException("Node '" + apiIdentName(it->apiIdent) + "' is not a child of '" + prevNode->path() + "'");
            }
        } else { // we are starting at root (no parent)
            try {
                currentNode = ctx.findPath("/" + *it->apiIdent.prefix + ":" + it->apiIdent.identifier);
            } catch (const libyang::Error& e) {
                throw InvalidURIException(""s + e.what());
            }
        }

        res += "/" + maybeQualified(*currentNode);

        if (currentNode->nodeType() == libyang::NodeType::List) {
            const auto& listKeys = currentNode->asList().keys();

            if (listKeys.size() == 0) {
                throw InvalidURIException("List '" + currentNode->path() + "' has no keys. It can not be accessed directly");
            } else if (it->keys.size() != listKeys.size()) {
                throw InvalidURIException("List '" + currentNode->path() + "' requires " + std::to_string(listKeys.size()) + " keys");
            }

            // FIXME: use std::views::zip in C++23
            auto itKeyValue = it->keys.begin();
            for (auto itKeyName = listKeys.begin(); itKeyName != listKeys.end(); ++itKeyName, ++itKeyValue) {
                res += '[' + std::string{itKeyName->name()} + "=" + escapeListKey(*itKeyValue) + ']';
            }
        } else if (currentNode->nodeType() == libyang::NodeType::Leaflist) {
            if (it->keys.size() != 1) {
                throw InvalidURIException("Leaf-list '" + currentNode->path() + "' requires exactly one key");
            }

            res += "[.=" + escapeListKey(it->keys.front()) + ']';
        } else if (it->keys.size() > 0) {
            throw InvalidURIException("No keys allowed for node '" + currentNode->path() + "'");
        }

        if (!isValidDataResource(*currentNode)) {
            throw InvalidURIException("'"s + currentNode->path() + "' is not a data resource");
        }
    }
    return res;
}
}

/** @brief Transforms URI path (i.e., data resource identifier) into a path that is understood by libyang
 *
 * @throws InvalidURIException When the path is contextually invalid
 * @throws InvalidURIException When URI cannot be parsed
 * @throws InvalidURIException When unable to properly escape YANG list key value (i.e., the list value contains both single and double quotes).
 * @return libyang path as a string
 */
std::string asLibyangPath(const libyang::Context& ctx, const std::string& uriPath)
{
    auto resourcePath = impl::parseUriPath(uriPath);
    if (!resourcePath) {
        throw InvalidURIException("Syntax error");
    }
    if (resourcePath->segments.empty()) {
        return "/*";
    }
    return asLibyangPath(ctx, resourcePath->segments.begin(), resourcePath->segments.end());
}

/** @brief Transforms URI path (i.e., data resource identifier) into a path that is understood by libyang and
 * returns a path to the parent node (or empty if this path was a root node) and ApiIdentifier describing the last path segment.
 * This is useful for the PUT method where we have to start editing the tree in the parent node.
 *
 * @throws InvalidURIException When the path is contextually invalid
 * @throws InvalidURIException When URI cannot be parsed
 * @throws InvalidURIException When unable to properly escape YANG list key value (i.e., the list value contains both single and double quotes).
 * @return libyang path to the parent as a string and an PathSegment instance describing the last path segment node
 */
std::pair<std::string, PathSegment> asLibyangPathSplit(const libyang::Context& ctx, const std::string& uriPath)
{
    auto resourcePath = impl::parseUriPath(uriPath);
    if (!resourcePath) {
        throw InvalidURIException("Syntax error");
    }
    if (resourcePath->segments.empty()) {
        throw InvalidURIException("Cannot split the datastore resource URI");
    }


    auto lastSegment = resourcePath->segments.back();
    auto parentPath = asLibyangPath(ctx, resourcePath->segments.begin(), resourcePath->segments.end() - 1);

    // we know that the path is valid so we can get last segment module from ly
    if (!lastSegment.apiIdent.prefix) {
        auto fullPath = asLibyangPath(ctx, resourcePath->segments.begin(), resourcePath->segments.end());
        auto lastNode = ctx.findPath(fullPath);
        lastSegment.apiIdent.prefix = std::string(lastNode.module().name());
    }

    return {parentPath, lastSegment};
}

}
