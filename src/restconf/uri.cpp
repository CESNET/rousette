/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <boost/fusion/include/std_pair.hpp>
#include <experimental/iterator>
#include <libyang-cpp/Enum.hpp>
#include <map>
#include <string>
#include "restconf/Exceptions.h"
#include "restconf/uri.h"
#include "restconf/uri_impl.h"
#include "restconf/utils/yang.h"

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

const auto uriPath = x3::rule<class uriPath, std::vector<PathSegment>>{"uriPath"} = -x3::lit("/") >> -(fullyQualifiedListInstance >> -("/" >> listInstance % "/")); // RFC 8040, sec 3.5.3
const auto nmdaDatastore = x3::rule<class nmdaDatastore, URIPrefix>{"nmdaDatastore"} = x3::attr(URIPrefix::Type::NMDADatastore) >> "/" >> fullyQualifiedApiIdentifier;
const auto resources = x3::rule<class resources, URIPath>{"resources"} =
    (x3::lit("/data") >> x3::attr(URIPrefix{URIPrefix::Type::BasicRestconfData}) >> uriPath) |
    (x3::lit("/ds") >> nmdaDatastore >> uriPath) |
    (x3::lit("/operations") >> x3::attr(URIPrefix{URIPrefix::Type::BasicRestconfOperations}) >> uriPath) |
    (x3::lit("/yang-library-version") >> x3::attr(URIPrefix{URIPrefix::Type::YangLibraryVersion}) >> x3::attr(std::vector<PathSegment>{} /* no path segment in this resource, just a dummy to return correct type */) >> -x3::lit("/"));
const auto uriGrammar = x3::rule<class grammar, URIPath>{"grammar"} = x3::lit("/restconf") >> resources;

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

namespace {
namespace x3 = boost::spirit::x3;


// clang-format off

auto validDepthValues = [](auto& ctx) {
    _val(ctx) = _attr(ctx); // it seems that this must be present, otherwise the _val(ctx) is default-constructed?
    _pass(ctx) = _attr(ctx) > 0 && _attr(ctx) < 65536;
};

struct withDefaultsTable : x3::symbols<queryParams::QueryParamValue> {
    withDefaultsTable()
    {
        add
            ("trim", queryParams::withDefaults::Trim{})
            ("explicit", queryParams::withDefaults::Explicit{})
            ("report-all", queryParams::withDefaults::ReportAll{})
            ("report-all-tagged", queryParams::withDefaults::ReportAllTagged{});
    }
} const withDefaultsParam;

struct contentTable : x3::symbols<queryParams::QueryParamValue> {
    contentTable()
    {
        add
            ("all", queryParams::content::AllNodes{})
            ("nonconfig", queryParams::content::OnlyNonConfigNodes{})
            ("config", queryParams::content::OnlyConfigNodes{});
    }
} const contentParam;

struct insertTable: x3::symbols<queryParams::QueryParamValue> {
    insertTable()
    {
    add
        ("first", queryParams::insert::First{})
        ("last", queryParams::insert::Last{})
        ("after", queryParams::insert::After{})
        ("before", queryParams::insert::Before{});
    }
} const insertParam;

// early sanity check, this timestamp will be parsed by libyang::fromYangTimeFormat anyways
const auto dateAndTime = x3::rule<class dateAndTime, std::string>{"dateAndTime"} =
    x3::repeat(4)[x3::digit] >> x3::char_('-') >> x3::repeat(2)[x3::digit] >> x3::char_('-') >> x3::repeat(2)[x3::digit] >> x3::char_('T') >>
    x3::repeat(2)[x3::digit] >> x3::char_(':') >> x3::repeat(2)[x3::digit] >> x3::char_(':') >> x3::repeat(2)[x3::digit] >> -(x3::char_('.') >> +x3::digit) >>
    (x3::char_('Z') | (-(x3::char_('+')|x3::char_('-')) >> x3::repeat(2)[x3::digit] >> x3::char_(':') >> x3::repeat(2)[x3::digit]));
const auto filter = x3::rule<class filter, std::string>{"filter"} = +(urlEncodedChar | (x3::char_ - '&'));
const auto depthParam = x3::rule<class depthParam, queryParams::QueryParamValue>{"depthParam"} = x3::uint_[validDepthValues] | (x3::string("unbounded") >> x3::attr(queryParams::UnboundedDepth{}));
const auto queryParamPair = x3::rule<class queryParamPair, std::pair<std::string, queryParams::QueryParamValue>>{"queryParamPair"} =
        (x3::string("depth") >> "=" >> depthParam) |
        (x3::string("with-defaults") >> "=" >> withDefaultsParam) |
        (x3::string("content") >> "=" >> contentParam) |
        (x3::string("insert") >> "=" >> insertParam) |
        (x3::string("point") >> "=" >> uriPath) |
        (x3::string("filter") >> "=" >> filter) |
        (x3::string("start-time") >> "=" >> dateAndTime) |
        (x3::string("stop-time") >> "=" >> dateAndTime);

const auto queryParamGrammar = x3::rule<class grammar, queryParams::QueryParams>{"queryParamGrammar"} = queryParamPair % "&" | x3::eps;

// clang-format on
}

std::optional<URIPath> parseUriPath(const std::string& uriPath)
{
    URIPath out;
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

std::optional<queryParams::QueryParams> parseQueryParams(const std::string& queryString)
{
    std::optional<queryParams::QueryParams> ret;

    if (!x3::parse(std::begin(queryString), std::end(queryString), queryParamGrammar >> x3::eoi, ret)) {
        return std::nullopt;
    }

    return ret;
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

URIPath::URIPath() = default;

URIPath::URIPath(const URIPrefix& prefix, const std::vector<PathSegment>& segments)
    : prefix(prefix)
    , segments(segments)
{
}

URIPath::URIPath(const std::vector<PathSegment>& segments)
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

std::optional<std::variant<libyang::Module, libyang::SubmoduleParsed>> getModuleOrSubmodule(const libyang::Context& ctx, const std::string& name, const std::optional<std::string>& revision)
{
    if (auto mod = ctx.getModule(name, revision)) {
        return *mod;
    }
    if (auto mod = ctx.getSubmodule(name, revision)) {
        return *mod;
    }
    return std::nullopt;
}
}

RestconfRequest::RestconfRequest(Type type, const boost::optional<ApiIdentifier>& datastore, const std::string& path, const queryParams::QueryParams& queryParams)
    : type(type)
    , datastore(datastoreFromApiIdentifier(datastore))
    , path(path)
    , queryParams(queryParams)
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

/** @brief Validates whether SchemaNode is valid for this HTTP method and prefix
 *
 * @throw ErrorResponse If node is invalid for this httpMethod and URI prefix */
void validateMethodForNode(const std::string& httpMethod, const impl::URIPrefix& prefix, const std::optional<libyang::SchemaNode>& node)
{
    if (httpMethod == "OPTIONS") {
        // no check is needed; path validation happens via the request handler by trying all other methods
    } else if (!node) {
        // no data path provided
        switch (prefix.resourceType) {
        case impl::URIPrefix::Type::BasicRestconfOperations:
            // FIXME: implement this, https://datatracker.ietf.org/doc/html/rfc8040#section-3.3.2
            throw ErrorResponse(400, "protocol", "operation-failed", "'/' is not an operation resource");
            break;
        case impl::URIPrefix::Type::BasicRestconfData:
        case impl::URIPrefix::Type::NMDADatastore:
            if (httpMethod == "DELETE") {
                throw ErrorResponse(400, "application", "operation-failed", "'/' is not a data resource");
            }
            break;
        case impl::URIPrefix::Type::YangLibraryVersion:
            if (httpMethod != "GET" && httpMethod != "HEAD") {
                throw ErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
            }
            break;
        }
    } else if (httpMethod == "POST") {
        switch (node->nodeType()) {
        case libyang::NodeType::RPC:
            if (prefix.resourceType != impl::URIPrefix::Type::BasicRestconfOperations) {
                throw ErrorResponse(400, "protocol", "operation-failed", "RPC '"s + node->path() + "' must be requested using operation prefix");
            }
            break;
        case libyang::NodeType::Action:
            if (prefix.resourceType != impl::URIPrefix::Type::BasicRestconfData) {
                throw ErrorResponse(400, "protocol", "operation-failed", "Action '"s + node->path() + "' must be requested using data prefix");
            }
            break;
        default:
            checkValidDataResource(node, prefix);
        }
    } else {
        checkValidDataResource(node, prefix);
    }
}

void validateQueryParameters(const std::multimap<std::string, queryParams::QueryParamValue>& params_, const std::string& httpMethod)
{
    std::map<std::string, queryParams::QueryParamValue> params;
    for (const auto& [k, v] : params_) {
        auto [it, inserted] = params.emplace(k, v);
        if (!inserted) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '" + k + "' already specified");
        }
    }

    for (const auto& param : {"depth", "with-defaults", "content"}) {
        if (auto it = params.find(param); it != params.end() && httpMethod != "GET" && httpMethod != "HEAD") {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '"s + param + "' can be used only with GET and HEAD methods");
        }
    }

    for (const auto& param : {"insert", "point"}) {
        if (auto it = params.find(param); it != params.end() && httpMethod != "POST" && httpMethod != "PUT") {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '"s + param + "' can be used only with POST and PUT methods");
        }
    }

    for (const auto& param : {"filter", "start-time", "stop-time"}) {
        if (auto it = params.find(param); it != params.end()) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '"s + param + "' can be used only with streams");
        }
    }

    {
        auto itInsert = params.find("insert");
        auto itPoint = params.find("point");
        auto hasInsertParamBeforeOrAfter = itInsert != params.end() && (std::holds_alternative<queryParams::insert::After>(itInsert->second) || std::holds_alternative<queryParams::insert::Before>(itInsert->second));
        auto hasPointParam = itPoint != params.end();

        if (hasPointParam != hasInsertParamBeforeOrAfter) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter 'point' must always come with parameter 'insert' set to 'before' or 'after'");
        }
    }
}

void validateQueryParametersForStream(const std::multimap<std::string, queryParams::QueryParamValue>& params_)
{
    std::map<std::string, queryParams::QueryParamValue> params;
    for (const auto& [k, v] : params_) {
        auto [it, inserted] = params.emplace(k, v);
        if (!inserted) {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '" + k + "' already specified");
        }

        if (k != "filter" && k != "start-time" && k != "stop-time") {
            throw ErrorResponse(400, "protocol", "invalid-value", "Query parameter '" + k + "' can't be used with streams");
        }
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

            try {
                res += listKeyPredicate(listKeys, it->keys);
            } catch (const std::invalid_argument& e) {
                throw ErrorResponse(400, "application", "operation-failed", e.what());
            }
        } else if (currentNode->nodeType() == libyang::NodeType::Leaflist) {
            if (it->keys.size() != 1) {
                throw ErrorResponse(400, "application", "operation-failed", "Leaf-list '" + currentNode->path() + "' requires exactly one key");
            }

            try {
                res += "[.=" + escapeListKey(it->keys.front()) + ']';
            } catch (const std::invalid_argument& e) {
                throw ErrorResponse(400, "application", "operation-failed", e.what());
            }
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

/** @brief Returns a schema node corresponding to the parsed RESTCONF URI */
std::optional<libyang::SchemaNode> asLibyangSchemaNode(const libyang::Context& ctx, const std::vector<PathSegment>& pathSegments)
{
    return asLibyangPath(ctx, pathSegments.begin(), pathSegments.end()).schemaNode;
}

std::vector<PathSegment> asPathSegments(const std::string& uriPath)
{
    auto uri = impl::parseUriPath(uriPath);
    if (!uri) {
        throw ErrorResponse(400, "application", "operation-failed", "Syntax error");
    }

    return uri->segments;
}

/** @brief Parse requested URL as a RESTCONF requested
 *
 * The URI path (i.e., a resource identifier) will be parsed into an action that is supposed to be performed,
 * the target datastore, and a libyang path over which the operation will be performed.
 *
 * @throws ErrorResponse when the URI cannot be parsed or the URI is invalid for this HTTP method
 */
RestconfRequest asRestconfRequest(const libyang::Context& ctx, const std::string& httpMethod, const std::string& uriPath, const std::string& uriQueryString)
{
    if (httpMethod != "GET" && httpMethod != "PUT" && httpMethod != "POST" && httpMethod != "DELETE" && httpMethod != "HEAD" && httpMethod != "OPTIONS" && httpMethod != "PATCH") {
        throw ErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
    }

    auto uri = impl::parseUriPath(uriPath);
    if (!uri) {
        throw ErrorResponse(400, "application", "operation-failed", "Syntax error");
    }

    auto queryParameters = impl::parseQueryParams(uriQueryString);
    if (!queryParameters) {
        throw ErrorResponse(400, "protocol", "invalid-value", "Query parameters syntax error");
    }
    validateQueryParameters(*queryParameters, httpMethod);

    auto [lyPath, schemaNode] = asLibyangPath(ctx, uri->segments.begin(), uri->segments.end());
    validateMethodForNode(httpMethod, uri->prefix, schemaNode);

    auto path = uri->segments.empty() ? "/" : lyPath;
    RestconfRequest::Type type;

    if (httpMethod == "OPTIONS") {
        return {RestconfRequest::Type::OptionsQuery, boost::none, ""s, {}};
    } else if (uri->prefix.resourceType == impl::URIPrefix::Type::YangLibraryVersion) {
        return {RestconfRequest::Type::YangLibraryVersion, boost::none, ""s, *queryParameters};
    } else if ((httpMethod == "GET" || httpMethod == "HEAD")) {
        type = RestconfRequest::Type::GetData;
        path = uri->segments.empty() ? "/*" : path;
    } else if (httpMethod == "PUT") {
        type = RestconfRequest::Type::CreateOrReplaceThisNode;
    } else if (httpMethod == "DELETE" && schemaNode) {
        type = RestconfRequest::Type::DeleteNode;
    } else if (httpMethod == "POST" && schemaNode && (schemaNode->nodeType() == libyang::NodeType::Action || schemaNode->nodeType() == libyang::NodeType::RPC)) {
        type = RestconfRequest::Type::Execute;
    } else if (httpMethod == "POST" && (uri->prefix.resourceType == impl::URIPrefix::Type::BasicRestconfData || uri->prefix.resourceType == impl::URIPrefix::Type::NMDADatastore)) {
        type = RestconfRequest::Type::CreateChildren;
    } else if (httpMethod == "PATCH") {
        type = RestconfRequest::Type::MergeData;
    } else {
        throw std::logic_error("Unhandled request "s + httpMethod + " " + uriPath);
    }

    return {type, uri->prefix.datastore, path, *queryParameters};
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

std::optional<std::variant<libyang::Module, libyang::SubmoduleParsed>> asYangModule(const libyang::Context& ctx, const std::string& uriPath)
{
    if (auto parsedModule = impl::parseModuleWithRevision(uriPath)) {
        // Converting between boost::optional and std::optional is not trivial
        if (parsedModule->revision) {
            return getModuleOrSubmodule(ctx, parsedModule->name, *parsedModule->revision);
        } else {
            return getModuleOrSubmodule(ctx, parsedModule->name, std::nullopt);
        }
    }
    return std::nullopt;
}

RestconfStreamRequest asRestconfStreamRequest(const std::string& httpMethod, const std::string& uriPath, const std::string& uriQueryString)
{
    static const auto netconfStreamRoot = "/streams/NETCONF/";
    RestconfStreamRequest::Type type;

    if (httpMethod != "GET" && httpMethod != "HEAD") {
        throw ErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
    }

    if (uriPath == netconfStreamRoot + "XML"s) {
        type = RestconfStreamRequest::Type::NetconfNotificationXML;
    } else if (uriPath == netconfStreamRoot + "JSON"s) {
        type = RestconfStreamRequest::Type::NetconfNotificationJSON;
    } else {
        throw ErrorResponse(404, "application", "invalid-value", "Invalid stream");
    }

    auto queryParameters = impl::parseQueryParams(uriQueryString);
    if (!queryParameters) {
        throw ErrorResponse(400, "protocol", "invalid-value", "Query parameters syntax error");
    }

    validateQueryParametersForStream(*queryParameters);

    return {type, *queryParameters};
}

/** @brief Returns a set of allowed HTTP methods for given URI. Usable for the 'allow' header */
std::set<std::string> allowedHttpMethodsForUri(const libyang::Context& ctx, const std::string& uriPath)
{
    std::set<std::string> allowedHttpMethods;

    for (const auto& httpMethod : {"GET", "PUT", "POST", "DELETE", "HEAD", "PATCH"}) {
        try {
            asRestconfRequest(ctx, httpMethod, uriPath, "");
            allowedHttpMethods.insert(httpMethod);
        } catch (const ErrorResponse&) {
            // httpMethod is not allowed for this uri path
        }
    }

    if (!allowedHttpMethods.empty()) {
        allowedHttpMethods.insert("OPTIONS");
    }

    return allowedHttpMethods;
}
}
