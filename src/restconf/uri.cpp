/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <libyang-cpp/Enum.hpp>
#include "restconf/Exceptions.h"
#include "restconf/uri.h"
#include "restconf/uri_impl.h"

using namespace std::string_literals;

namespace rousette::restconf {
namespace impl {

namespace {
namespace x3 = boost::spirit::x3;

// clang-format off

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
const auto uriPrefix = x3::rule<class uriPrefix, URIPrefix>{"uriPrefix"} =
    (x3::lit("data") >> x3::attr(URIPrefix::Type::BasicRestconfData) >> x3::attr(boost::none)) |
    (x3::lit("ds") >> x3::attr(URIPrefix::Type::NMDADatastore) >> "/" >> fullyQualifiedApiIdentifier) |
    (x3::lit("operations") >> x3::attr(URIPrefix::Type::BasicRestconfOperations) >> x3::attr(boost::none));
const auto uriPrefixYangLibraryVersion = x3::rule<class uriPrefixYangLibraryVersion, URIPrefix>{"uriPrefixYangLibraryVersion"} =
    (x3::lit("yang-library-version") >> x3::attr(URIPrefix::Type::YangLibraryVersion) >> x3::attr(boost::none));
const auto uriPath = x3::rule<class uriPath, std::vector<PathSegment>>{"uriPath"} = -x3::lit("/") >> -(fullyQualifiedListInstance >> -("/" >> listInstance % "/")); // RFC 8040, sec 3.5.3
const auto uriGrammar = x3::rule<class grammar, URI>{"grammar"} = x3::lit("/") >> x3::lit("restconf") >> "/" >>
    ((uriPrefix >> uriPath) |
     (uriPrefixYangLibraryVersion >> -x3::lit("/") >> x3::attr(std::vector<PathSegment>{} /* there is no path segment in this URI, this is just a dummy to return correct type */)));

// clang-format on
}

namespace {
namespace x3 = boost::spirit::x3;

// clang-format off

const auto moduleName = x3::rule<class apiIdentifier, std::string>{"moduleName"} = (x3::alpha | x3::char_('_')) >> *(x3::alnum | x3::char_('_') | x3::char_('-') | x3::char_('.'));
const auto revision = x3::rule<class revision, std::string>{"revision"} = x3::repeat(4, x3::inf)[x3::digit] >> x3::char_("-") >> x3::repeat(2)[x3::digit] >> x3::char_("-") >> x3::repeat(2)[x3::digit];
const auto yangSchemaUriGrammar = x3::rule<class grammar, impl::YangModule>{"yangSchemaUriGrammar"} = x3::lit("/") >> x3::lit("yang") >> "/" >> moduleName >> -(x3::lit("@") >> revision >> -x3::lit(".yang"));

// clang-format on
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

std::optional<impl::YangModule> parseModuleWithRevision(const std::string& uriPath)
{
    impl::YangModule parsed;
    auto iter = std::begin(uriPath);
    auto end = std::end(uriPath);

    if (!x3::parse(iter, end, yangSchemaUriGrammar >> x3::eoi, parsed)) {
        return std::nullopt;
    }

    return parsed;
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

    throw ErrorResponse(400, "application", "operation-failed", "Unsupported datastore " + *datastore->prefix + ":" + datastore->identifier);
}
}

RestconfRequest::RestconfRequest(Type type, const boost::optional<ApiIdentifier>& datastore, const std::string& path)
    : type(type)
    , datastore(datastoreFromApiIdentifier(datastore))
    , path(path)
{
}

namespace {
std::optional<libyang::SchemaNode> findChildSchemaNode(const libyang::SchemaNode& node, const ApiIdentifier& childIdentifier)
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
std::string maybeQualified(const libyang::SchemaNode& currentNode)
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
 *  @throws ErrorResponse if both single and double quotes used in the input
 * */
std::string escapeListKey(const std::string& str)
{
    auto singleQuotes = str.find('\'') != std::string::npos;
    auto doubleQuotes = str.find('\"') != std::string::npos;

    if (singleQuotes && doubleQuotes) {
        throw ErrorResponse(400, "application", "operation-failed", "Encountered mixed single and double quotes in XPath. Can't properly escape.");
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

/** @brief checks if provided schema node is valid for this HTTP method */
void checkValidDataResource(const std::optional<libyang::SchemaNode>& node, const impl::URIPrefix& prefix)
{
    if (!node) {
        throw ErrorResponse(400, "application", "operation-failed", "'/' is not a data resource");
    }

    if (prefix.resourceType != impl::URIPrefix::Type::BasicRestconfData && prefix.resourceType != impl::URIPrefix::Type::NMDADatastore) {
        throw ErrorResponse(400, "application", "operation-failed", "GET method must be used with a data resource or a complete datastore resource");
    }

    switch (node->nodeType()) {
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
        return;
    case libyang::NodeType::RPC:
    case libyang::NodeType::Action:
        throw ErrorResponse(405, "protocol", "operation-not-supported", "'"s + node->path() + "' is an RPC/Action node");
    default:
        throw ErrorResponse(400, "protocol", "operation-failed", "'"s + node->path() + "' is not a data resource");
    }
}

/** @brief checks if provided schema node is valid for POST resource */
void checkValidPostResource(const std::optional<libyang::SchemaNode>& node, const impl::URIPrefix& prefix)
{
    if (!node) {
        if (prefix.resourceType == impl::URIPrefix::Type::BasicRestconfOperations) {
            throw ErrorResponse(400, "protocol", "operation-failed", "'/' is not an operation resource");
        } else {
            throw ErrorResponse(405, "application", "operation-not-supported", "POST method for a complete-datastore resource is not yet implemented");
        }
    }

    if (node->nodeType() != libyang::NodeType::RPC && node->nodeType() != libyang::NodeType::Action) {
        throw ErrorResponse(405, "application", "operation-not-supported", "POST method for a data resource is not yet implemented");
    }

    if (node->nodeType() == libyang::NodeType::RPC && prefix.resourceType != impl::URIPrefix::Type::BasicRestconfOperations) {
        throw ErrorResponse(400, "protocol", "operation-failed", "RPC '"s + node->path() + "' must be requested using operation prefix");
    }

    if (node->nodeType() == libyang::NodeType::Action && prefix.resourceType != impl::URIPrefix::Type::BasicRestconfData) {
        throw ErrorResponse(400, "protocol", "operation-failed", "Action '"s + node->path() + "' must be requested using data prefix");
    }
}

/** @brief Validates whether SchemaNode is valid for this HTTP method and prefix. If not, throws with ErrorResponse.
 *
 * @throw ErrorResponse If node is invalid for this httpMethod and URI prefix */
void validateRequestSchemaNode(const std::optional<libyang::SchemaNode>& node, const std::string& httpMethod, const impl::URIPrefix& prefix)
{
    if (httpMethod == "GET" || httpMethod == "PUT") {
        checkValidDataResource(node, prefix);
    } else {
        checkValidPostResource(node, prefix);
    }
}

/** @brief Wrapper for a libyang path and a corresponding SchemaNode. SchemaNode is nullopt for datastore resource */
struct SchemaNodeAndPath {
    std::string dataPath;
    std::optional<libyang::SchemaNode> schemaNode;
};

/** @brief Translates PathSegment sequence to a path understood by libyang
 * @return libyang path to a data node
 * @throws ErrorResponse On invalid URI which can mean that, e.g, a node is not found, wrong number of list keys provided, list key could not be properly escaped.
 */
SchemaNodeAndPath asLibyangPath(const libyang::Context& ctx, const std::vector<PathSegment>::const_iterator& begin, const std::vector<PathSegment>::const_iterator& end)
{
    std::optional<libyang::SchemaNode> currentNode;
    std::string res;

    for (auto it = begin; it != end; ++it) {
        if (auto prevNode = currentNode) {
            if (!(currentNode = findChildSchemaNode(*currentNode, it->apiIdent))) {
                throw ErrorResponse(400, "application", "operation-failed", "Node '" + apiIdentName(it->apiIdent) + "' is not a child of '" + prevNode->path() + "'");
            }
        } else { // we are starting at root (no parent)
            try {
                currentNode = ctx.findPath("/" + *it->apiIdent.prefix + ":" + it->apiIdent.identifier);
            } catch (const libyang::Error& e) {
                throw ErrorResponse(400, "application", "operation-failed", e.what());
            }
        }

        res += "/" + maybeQualified(*currentNode);

        if (currentNode->nodeType() == libyang::NodeType::List) {
            const auto& listKeys = currentNode->asList().keys();

            if (listKeys.size() == 0) {
                throw ErrorResponse(400, "application", "operation-failed", "List '" + currentNode->path() + "' has no keys. It can not be accessed directly");
            } else if (it->keys.size() != listKeys.size()) {
                throw ErrorResponse(400, "application", "operation-failed", "List '" + currentNode->path() + "' requires " + std::to_string(listKeys.size()) + " keys");
            }

            // FIXME: use std::views::zip in C++23
            auto itKeyValue = it->keys.begin();
            for (auto itKeyName = listKeys.begin(); itKeyName != listKeys.end(); ++itKeyName, ++itKeyValue) {
                res += '[' + std::string{itKeyName->name()} + "=" + escapeListKey(*itKeyValue) + ']';
            }
        } else if (currentNode->nodeType() == libyang::NodeType::Leaflist) {
            if (it->keys.size() != 1) {
                throw ErrorResponse(400, "application", "operation-failed", "Leaf-list '" + currentNode->path() + "' requires exactly one key");
            }

            res += "[.=" + escapeListKey(it->keys.front()) + ']';
        } else if (it->keys.size() > 0) {
            throw ErrorResponse(400, "application", "operation-failed", "No keys allowed for node '" + currentNode->path() + "'");
        }

        if (std::next(it) != end && (currentNode->nodeType() == libyang::NodeType::RPC || currentNode->nodeType() == libyang::NodeType::Action)) {
            throw ErrorResponse(400, "application", "operation-failed", "'"s + currentNode->path() + "' is an RPC/Action node, any child of it can't be requested", std::nullopt);
        }
    }
    return {res, currentNode};
}
}

/** @brief Parse requested URL as a RESTCONF requested
 *
 * The URI path (i.e., a resource identifier) will be parsed into an action that is supposed to be performed,
 * the target datastore, and a libyang path over which the operation will be performed.
 *
 * @throws ErrorResponse when the URI cannot be parsed or the URI is invalid for this HTTP method
 */
RestconfRequest asRestconfRequest(const libyang::Context& ctx, const std::string& httpMethod, const std::string& uriPath)
{
    if (httpMethod != "GET" && httpMethod != "PUT" && httpMethod != "POST") {
        throw ErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
    }

    auto uri = impl::parseUriPath(uriPath);
    if (!uri) {
        throw ErrorResponse(400, "application", "operation-failed", "Syntax error");
    }

    if (uri->prefix.resourceType == impl::URIPrefix::Type::YangLibraryVersion) {
        if (httpMethod == "GET") {
            return {RestconfRequest::Type::YangLibraryVersion, boost::none, ""s};
        } else {
            throw ErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
        }
    }

    if (httpMethod == "GET" && uri->segments.empty()) {
        return {RestconfRequest::Type::GetData, uri->prefix.datastore, "/*"};
    } else if (httpMethod == "PUT" && uri->segments.empty()) {
        return {RestconfRequest::Type::CreateOrReplaceThisNode, uri->prefix.datastore, "/"};
    }

    auto [lyPath, schemaNode] = asLibyangPath(ctx, uri->segments.begin(), uri->segments.end());
    validateRequestSchemaNode(schemaNode, httpMethod, uri->prefix);
    if (httpMethod == "GET") {
        return {RestconfRequest::Type::GetData, uri->prefix.datastore, lyPath};
    } else if (httpMethod == "PUT") {
        return {RestconfRequest::Type::CreateOrReplaceThisNode, uri->prefix.datastore, lyPath};
    } else {
        return {RestconfRequest::Type::Execute, uri->prefix.datastore, lyPath};
    }
}

/** @brief Transforms URI path into a libyang path to the parent node (or empty if this path was a root node) and PathSegment describing the last path segment.
 * This is useful for the PUT method where we have to start editing the tree in the parent node.
 *
 * @throws ErrorResponse On invalid URI
 * @return Pair of a libyang path to the parent as a string and a PathSegment instance describing the last path segment node
 */
std::pair<std::string, PathSegment> asLibyangPathSplit(const libyang::Context& ctx, const std::string& uriPath)
{
    auto uri = impl::parseUriPath(uriPath);
    if (!uri) {
        throw ErrorResponse(400, "application", "operation-failed", "Syntax error");
    }
    if (uri->segments.empty()) {
        throw ErrorResponse(400, "application", "operation-failed", "Cannot split the datastore resource URI");
    }


    auto lastSegment = uri->segments.back();
    auto [parentLyPath, schemaNodeParent] = asLibyangPath(ctx, uri->segments.begin(), uri->segments.end() - 1);

    // we know that the path is valid so we can get last segment module from the returned SchemaNode
    if (!lastSegment.apiIdent.prefix) {
        auto [fullLyPath, schemaNode] = asLibyangPath(ctx, uri->segments.begin(), uri->segments.end());
        lastSegment.apiIdent.prefix = std::string(schemaNode->module().name());
    }

    return {parentLyPath, lastSegment};
}

std::optional<libyang::Module> asYangModule(const libyang::Context& ctx, const std::string& uriPath)
{
    if (auto parsedModule = impl::parseModuleWithRevision(uriPath)) {
        // Converting between boost::optional and std::optional is not trivial
        if (parsedModule->revision) {
            return ctx.getModule(parsedModule->name, *parsedModule->revision);
        } else {
            return ctx.getModule(parsedModule->name, std::nullopt);
        }
    }
    return std::nullopt;
}
}
