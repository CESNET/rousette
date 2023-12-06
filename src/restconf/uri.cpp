/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include "restconf/Exceptions.h"
#include "restconf/uri.h"
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
const auto uriPrefix = x3::rule<class uriPrefix, URIPrefix>{"uriPrefix"} = (x3::lit("data") >> x3::attr(URIPrefix::Type::BasicRestconfData) >> x3::attr(boost::none)) | (x3::lit("ds") >> x3::attr(URIPrefix::Type::NMDADatastore) >> "/" >> fullyQualifiedApiIdentifier);
const auto uriPath = x3::rule<class uriPath, std::vector<PathSegment>>{"uriPath"} = -x3::lit("/") >> -(fullyQualifiedListInstance >> -("/" >> listInstance % "/")); // RFC 8040, sec 3.5.3
const auto uriGrammar = x3::rule<class grammar, URI>{"grammar"} = x3::lit("/") >> x3::lit("restconf") >> "/" >> uriPrefix >> uriPath;
}

std::optional<URI> parseUriPath(const std::string& uriPath)
{
    URI out;
    auto iter = std::begin(uriPath);
    auto end = std::end(uriPath);

    if (!x3::parse(iter, end, uriGrammar >> x3::eoi, out)) {
        return std::nullopt;
    }

    return out;
}

URIPrefix::URIPrefix()
    : resourceType(URIPrefix::Type::BasicRestconfData)
{
}

URIPrefix::URIPrefix(URIPrefix::Type resourceType, const boost::optional<ApiIdentifier>& datastore)
    : resourceType(resourceType)
    , datastore(datastore)
{
}

URI::URI() = default;

URI::URI(const URIPrefix& prefix, const std::vector<PathSegment>& segments)
    : prefix(prefix)
    , segments(segments)
{
}

URI::URI(const std::vector<PathSegment>& segments)
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
std::optional<sysrepo::Datastore> datastoreFromApiIdentifier(const boost::optional<ApiIdentifier>& datastore)
{
    if (!datastore) {
        return std::nullopt;
    }

    if (*datastore->prefix == "ietf-datastores") {
        if (datastore->identifier == "running") {
            return sysrepo::Datastore::Running;
        } else if (datastore->identifier == "operational") {
            return sysrepo::Datastore::Operational;
        } else if (datastore->identifier == "candidate") {
            return sysrepo::Datastore::Candidate;
        } else if (datastore->identifier == "startup") {
            return sysrepo::Datastore::Startup;
        } else if (datastore->identifier == "factory-default") {
            return sysrepo::Datastore::FactoryDefault;
        }
    }

    throw InvalidURIException("Unsupported datastore " + *datastore->prefix + ":" + datastore->identifier);
}
}

DatastoreAndPath::DatastoreAndPath(const boost::optional<ApiIdentifier>& datastore, const std::string& path)
    : datastore(datastoreFromApiIdentifier(datastore))
    , path(path)
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

/** @brief Returns true if any parent of this schema node is a RPC/action node. This means node is an input or output node of RPC/action. */
bool insideRPC(libyang::SchemaNode node)
{
    auto n = node.parent();

    while (n) {
        if (n->nodeType() == libyang::NodeType::RPC || n->nodeType() == libyang::NodeType::Action) {
            return true;
        }

        n = n->parent();
    }

    return false;
}

/** @brief checks if provided schema node is valid for this HTTP method */
bool isValidDataResource(libyang::SchemaNode node)
{
    if (insideRPC(node)) {
        return false;
    }

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

std::pair<std::optional<libyang::SchemaNode>, std::string> asLibyangPath(const libyang::Context& ctx, const std::vector<PathSegment>::const_iterator& begin, const std::vector<PathSegment>::const_iterator& end)
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
    }

    return {currentNode, res};
}
}

/** @brief Transforms URI path (i.e., data resource identifier) into a path that is understood by libyang and a datastore (RFC 8527)
 *
 * @throws InvalidURIException When the path is contextually invalid
 * @throws InvalidURIException When URI cannot be parsed
 * @throws InvalidURIException When unable to properly escape YANG list key value (i.e., the list value contains both single and double quotes).
 * @throws InvalidURIException When datastore is not implemented
 * @return DatastoreAndPath object containing a sysrepo datastore and a libyang path as a string
 */
DatastoreAndPath asLibyangPath(const libyang::Context& ctx, const std::string& httpMethod, const std::string& uriPath)
{
    if (httpMethod != "GET" && httpMethod != "PUT") {
        throw ErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
    }

    auto uri = impl::parseUriPath(uriPath);
    if (!uri) {
        throw InvalidURIException("Syntax error");
    }
    if (httpMethod == "GET" && uri->segments.empty()) {
        return {uri->prefix.datastore, "/*"};
    } else if (uri->segments.empty()) {
        throw InvalidURIException("Invalid URI for PUT request");
    }

    auto [schemaNode, lyPath] = asLibyangPath(ctx, uri->segments.begin(), uri->segments.end());

    if (!isValidDataResource(*schemaNode) && (schemaNode->nodeType() == libyang::NodeType::RPC || schemaNode->nodeType() == libyang::NodeType::Action)) {
        throw ErrorResponse(405, "protocol", "operation-not-supported", "'"s + schemaNode->path() + "' is not a data resource", std::nullopt);
    } else if (!isValidDataResource(*schemaNode)) {
        throw InvalidURIException("'"s + schemaNode->path() + "' is not a data resource");
    }

    return {uri->prefix.datastore, lyPath};
}

/** @brief Transforms URI path into a libyang path to the parent node (or empty if this path was a root node) and ApiIdentifier describing the last path segment.
 * This is useful for the PUT method where we have to start editing the tree in the parent node.
 *
 * @throws InvalidURIException When the path is contextually invalid
 * @throws InvalidURIException When URI cannot be parsed
 * @throws InvalidURIException When unable to properly escape YANG list key value (i.e., the list value contains both single and double quotes).
 * @throws InvalidURIException When datastore is not implemented
 * @return Pair of a libyang path to the parent as a string and PathSegment instance describing the last path segment node
 */
std::pair<std::string, PathSegment> asLibyangPathSplit(const libyang::Context& ctx, const std::string& uriPath)
{
    auto uri = impl::parseUriPath(uriPath);
    if (!uri) {
        throw InvalidURIException("Syntax error");
    }
    if (uri->segments.empty()) {
        throw InvalidURIException("Cannot split the datastore resource URI");
    }


    auto lastSegment = uri->segments.back();
    auto [schemaNodeParent, parentLyPath] = asLibyangPath(ctx, uri->segments.begin(), uri->segments.end() - 1);

    // we know that the path is valid so we can get last segment module from ly
    if (!lastSegment.apiIdent.prefix) {
        auto [schemaNode, fullLyPath] = asLibyangPath(ctx, uri->segments.begin(), uri->segments.end());
        lastSegment.apiIdent.prefix = std::string(schemaNode->module().name());
    }

    return {parentLyPath, lastSegment};
}

}
