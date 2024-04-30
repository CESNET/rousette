/*
 * Copyright (C) 2016-2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <experimental/iterator>
#include <string>
#include <sysrepo-cpp/Connection.hpp>
#include "restconf/Exceptions.h"
#include "restconf/uri.h"
#include "restconf/uri_impl.h"
#include "tests/configure.cmake.h"
#include "tests/pretty_printers.h"

using namespace std::string_literals;

namespace {
std::string serializeErrorResponse(int code, const std::string& errorType, const std::string& errorTag, const std::string& errorMessage)
{
    return "("s + std::to_string(code) + ", \"" + errorType + "\", \"" + errorTag + "\", \"" + errorMessage + "\")";
}
}

namespace rousette::restconf {

REGISTER_EXCEPTION_TRANSLATOR(const ErrorResponse& e)
{
    return serializeErrorResponse(e.code, e.errorType, e.errorTag, e.errorMessage).c_str();
}

}

namespace doctest {

template <>
struct StringMaker<std::optional<rousette::restconf::impl::YangModule>> {
    static String convert(const std::optional<rousette::restconf::impl::YangModule>& obj)
    {
        std::ostringstream oss;

        if (obj) {
            oss << "YangModule{" << obj->name << ", " << (obj->revision ? obj->revision.value() : "nullopt{}") << "}";
        } else {
            oss << "nullopt{}";
        }

        return oss.str().c_str();
    }
};
}

TEST_CASE("URI path parser")
{
    using rousette::restconf::PathSegment;
    using rousette::restconf::RestconfRequest;
    using rousette::restconf::impl::URIPath;
    using rousette::restconf::impl::URIPrefix;

    SECTION("Valid paths")
    {
        for (const auto& [uriPath, expected] : {
                 std::pair<std::string, URIPath>{"/restconf/data/x333:y666", {{
                                                                                 {{"x333", "y666"}},
                                                                             }}},
                 {"/restconf/data/foo:bar", {{
                                                {{"foo", "bar"}},
                                            }}},
                 {"/restconf/data/foo:bar/baz", {{
                                                    {{"foo", "bar"}},
                                                    {{"baz"}},
                                                }}},
                 {"/restconf/data/foo:bar/meh:baz", {{
                                                        {{"foo", "bar"}},
                                                        {{"meh", "baz"}},
                                                    }}},
                 {"/restconf/data/foo:bar/yay/meh:baz", {{
                                                            {{"foo", "bar"}},
                                                            {{"yay"}},
                                                            {{"meh", "baz"}},
                                                        }}},
                 {"/restconf/data/foo:bar/Y=val", {{
                                                      {{"foo", "bar"}},
                                                      {{"Y"}, {"val"}},
                                                  }}},
                 {"/restconf/data/foo:bar/Y=val-ue", {{
                                                         {{"foo", "bar"}},
                                                         {{"Y"}, {"val-ue"}},
                                                     }}},
                 {"/restconf/data/foo:bar/p:lst=key1", {{
                                                           {{"foo", "bar"}},
                                                           {{"p", "lst"}, {"key1"}},
                                                       }}},

                 {"/restconf/data/foo:bar/p:lst=key1/leaf", {{
                                                                {{"foo", "bar"}},
                                                                {{"p", "lst"}, {"key1"}},
                                                                {{"leaf"}},
                                                            }}},
                 {"/restconf/data/foo:bar/lst=key1,", {{
                                                          {{"foo", "bar"}},
                                                          {{"lst"}, {"key1", ""}},
                                                      }}},
                 {"/restconf/data/foo:bar/lst=key1,,,", {{
                                                            {{"foo", "bar"}},
                                                            {{"lst"}, {"key1", "", "", ""}},
                                                        }}},
                 {"/restconf/data/foo:bar/lst=key1,/leaf", {{
                                                               {{"foo", "bar"}},
                                                               {{"lst"}, {"key1", ""}},
                                                               {{"leaf"}},
                                                           }}},
                 {"/restconf/data/foo:bar/lst=key1,key2", {{
                                                              {{"foo", "bar"}},
                                                              {{"lst"}, {"key1", "key2"}},
                                                          }}},
                 {"/restconf/data/foo:bar/lst=key1,key2/leaf", {{
                                                                   {{"foo", "bar"}},
                                                                   {{"lst"}, {"key1", "key2"}},
                                                                   {{"leaf"}},
                                                               }}},
                 {"/restconf/data/foo:bar/lst=key1,key2/lst2=key1/leaf", {{
                                                                             {{"foo", "bar"}},
                                                                             {{"lst"}, {"key1", "key2"}},
                                                                             {{"lst2"}, {"key1"}},
                                                                             {{"leaf"}},
                                                                         }}},
                 {"/restconf/data/foo:bar/lst=,key2/lst2=key1/leaf", {{
                                                                         {{"foo", "bar"}},
                                                                         {{"lst"}, {"", "key2"}},
                                                                         {{"lst2"}, {"key1"}},
                                                                         {{"leaf"}},
                                                                     }}},
                 {"/restconf/data/foo:bar/lst=,/lst2=key1/leaf", {{
                                                                     {{"foo", "bar"}},
                                                                     {{"lst"}, {"", ""}},
                                                                     {{"lst2"}, {"key1"}},
                                                                     {{"leaf"}},
                                                                 }}},
                 {"/restconf/data/foo:bar/lst=", {{
                                                     {{"foo", "bar"}},
                                                     {{"lst"}, {""}},
                                                 }}},
                 {"/restconf/data/foo:bar/lst=/leaf", {{
                                                          {{"foo", "bar"}},
                                                          {{"lst"}, {""}},
                                                          {{"leaf"}},
                                                      }}},
                 {"/restconf/data/foo:bar/prefix:lst=key1/prefix:leaf", {{
                                                                            {{"foo", "bar"}},
                                                                            {{"prefix", "lst"}, {"key1"}},
                                                                            {{"prefix", "leaf"}},
                                                                        }}},
                 {"/restconf/data/foo:bar/lst=key1,,key3", {{
                                                               {{"foo", "bar"}},
                                                               {{"lst"}, {"key1", "", "key3"}},
                                                           }}},
                 {"/restconf/data/foo:bar/lst=key%2CWithCommas,,key2C", {{
                                                                            {{"foo", "bar"}},
                                                                            {{"lst"}, {"key,WithCommas", "", "key2C"}},
                                                                        }}},
                 {R"(/restconf/data/foo:bar/list1=%2C%27"%3A"%20%2F,,foo)", {{
                                                                                {{"foo", "bar"}},
                                                                                {{"list1"}, {R"(,'":" /)", "", "foo"}},
                                                                            }}},
                 {"/restconf/data/foo:bar/list1= %20,%20,foo", {{
                                                                   {{"foo", "bar"}},
                                                                   {{"list1"}, {"  ", " ", "foo"}},
                                                               }}},
                 {"/restconf/data/foo:bar/list1= %20,%20, ", {{
                                                                 {{"foo", "bar"}},
                                                                 {{"list1"}, {"  ", " ", " "}},
                                                             }}},
                 {"/restconf/data/foo:bar/list1=žluťoučkýkůň", {{
                                                                   {{"foo", "bar"}},
                                                                   {{"list1"}, {"žluťoučkýkůň"}},
                                                               }}},
                 {"/restconf/data/foo:list=A%20Z", {{
                                                       {{"foo", "list"}, {"A Z"}},
                                                   }}},
                 {"/restconf/data/foo:list=A%25Z", {{
                                                       {{"foo", "list"}, {"A%Z"}},
                                                   }}},
                 {"/restconf/data", {{}, {}}},
                 {"/restconf/data/", {{}, {}}},

                 // RFC 8527 uris
                 {"/restconf/ds/hello:world", {{URIPrefix::Type::NMDADatastore, {{"hello", "world"}}}, {}}},
                 {"/restconf/ds/ietf-datastores:running/foo:bar/list1=a", {{URIPrefix::Type::NMDADatastore, {{"ietf-datastores", "running"}}}, {{{"foo", "bar"}}, {{"list1"}, {"a"}}}}},
                 {"/restconf/ds/ietf-datastores:operational", {{URIPrefix::Type::NMDADatastore, {{"ietf-datastores", "operational"}}}, {}}},
                 {"/restconf/ds/ietf-datastores:operational/", {{URIPrefix::Type::NMDADatastore, {{"ietf-datastores", "operational"}}}, {}}},
                 // RPCs and actions
                 {"/restconf/operations/example:rpc-test", {{URIPrefix::Type::BasicRestconfOperations, boost::none}, {{{"example", "rpc-test"}}}}},
                 {"/restconf/data/example:tlc/list=hello-world/example-action", {{
                                                                                    {{"example", "tlc"}},
                                                                                    {{"list"}, {"hello-world"}},
                                                                                    {{"example-action"}},
                                                                                }}},

                 {"/restconf/yang-library-version", {{URIPrefix::Type::YangLibraryVersion, boost::none}, {}}},
                 {"/restconf/yang-library-version/", {{URIPrefix::Type::YangLibraryVersion, boost::none}, {}}},
             }) {
            CAPTURE(uriPath);
            auto path = rousette::restconf::impl::parseUriPath(uriPath);
            REQUIRE(path == expected);
        }
    }

    SECTION("Invalid URIs")
    {
        for (const std::string uriPath : {
                 "/restconf/foo",
                 "/restconf/foo/foo:bar",
                 "/restconf/data/foo",
                 "/restconf/data/foo:",
                 "/restconf/data/:bar",
                 "/restconf/data/333:666",
                 "/restconf/data/foo:bar/lst==",
                 "/restconf/data/foo:bar/lst==key",
                 "/restconf/data/foo:bar/=key",
                 "/restconf/data/foo:bar/lst=key1,,,,=",
                 "/restconf/data/foo:bar/X=Y=instance-value",
                 "/restconf/data/foo:bar/:baz",
                 "/restconf/data/foo:list=A%xyZ",
                 "/restconf/data/foo:list=A%0zZ",
                 "/restconf/data/foo:list=A%%1Z",
                 "/restconf/data/foo:list=A%%25Z",
                 "/restconf/data/foo:list=A%2",
                 "/restconf/data/foo:list=A%2,",
                 "/restconf/data/foo:bar/list1=%%",
                 "/restconf/data/foo:bar/",
                 "/restconf/data/ foo : bar",
                 "/rest conf/data / foo:bar",
                 "/restconf/da ta/foo:bar",
                 "/restconf/data / foo:bar = key1",
                 "/restconf/data / foo:bar =key1",
                 "/restconf/ data",
                 "/restconf /data",
                 "/restconf  data",

                 "/restconf/ds",
                 "/restconf/ds/operational",
                 "/restconf/ds/ietf-datastores",
                 "/restconf/ds/ietf-datastores:",
                 "/restconf/ds/ietf-datastores:operational/foo:bar/",

                 "/restconf/yang-library",
                 "/restconf/yang-library-version/foo:list",
             }) {

            CAPTURE(uriPath);
            REQUIRE(!rousette::restconf::impl::parseUriPath(uriPath));
        }
    }

    SECTION("Translation to libyang path")
    {
        auto ctx = libyang::Context{std::filesystem::path{CMAKE_CURRENT_SOURCE_DIR} / "tests" / "yang"};
        ctx.loadModule("example", std::nullopt, {"f1"});
        ctx.loadModule("example-augment");

        SECTION("Contextually valid paths")
        {
            SECTION("GET, PUT, DELETE, POST (data)")
            {
                for (const auto& [httpMethod, expectedRequestType] : {
                         std::pair<std::string, RestconfRequest::Type>{"GET", RestconfRequest::Type::GetData},
                         {"PUT", RestconfRequest::Type::CreateOrReplaceThisNode},
                         {"DELETE", RestconfRequest::Type::DeleteNode},
                         {"POST", RestconfRequest::Type::CreateChildren},
                     }) {
                    for (const auto& [uriPath, expectedLyPath, expectedDatastore] : {
                             std::tuple<std::string, std::string, std::optional<sysrepo::Datastore>>{"/restconf/data/example:top-level-leaf", "/example:top-level-leaf", std::nullopt},
                             {"/restconf/data/example:top-level-list=hello", "/example:top-level-list[name='hello']", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0", "/example:tlc/list[name='eth0']", std::nullopt},
                             {R"(/restconf/data/example:tlc/list=et"h0)", R"(/example:tlc/list[name='et"h0'])", std::nullopt},
                             {R"(/restconf/data/example:tlc/list=et%22h0)", R"(/example:tlc/list[name='et"h0'])", std::nullopt},
                             {R"(/restconf/data/example:tlc/list=et%27h0)", R"(/example:tlc/list[name="et'h0"])", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/name", "/example:tlc/list[name='eth0']/name", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/nested=1,2,3", "/example:tlc/list[name='eth0']/nested[first='1'][second='2'][third='3']", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/nested=,2,3", "/example:tlc/list[name='eth0']/nested[first=''][second='2'][third='3']", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/nested=,2,", "/example:tlc/list[name='eth0']/nested[first=''][second='2'][third='']", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/choice1", "/example:tlc/list[name='eth0']/choice1", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/choice2", "/example:tlc/list[name='eth0']/choice2", std::nullopt},
                             {"/restconf/data/example:tlc/list=eth0/collection=val", "/example:tlc/list[name='eth0']/collection[.='val']", std::nullopt},
                             {"/restconf/data/example:tlc/status", "/example:tlc/status", std::nullopt},
                             // container example:a has a container b inserted locally and also via an augment. Check that we return the correct one
                             {"/restconf/data/example:a/b", "/example:a/b", std::nullopt},
                             {"/restconf/data/example:a/b/c", "/example:a/b/c", std::nullopt},
                             {"/restconf/data/example:a/b/c/enabled", "/example:a/b/c/enabled", std::nullopt},
                             {"/restconf/data/example:a/example-augment:b", "/example:a/example-augment:b", std::nullopt},
                             {"/restconf/data/example:a/example-augment:b/c", "/example:a/example-augment:b/c", std::nullopt},
                             {"/restconf/data/example:a/example-augment:b/example-augment:c", "/example:a/example-augment:b/c", std::nullopt},
                             {"/restconf/data/example:a/example-augment:b/c/enabled", "/example:a/example-augment:b/c/enabled", std::nullopt},
                             // rfc 8527
                             {"/restconf/ds/ietf-datastores:running/example:tlc/status", "/example:tlc/status", sysrepo::Datastore::Running},
                             {"/restconf/ds/ietf-datastores:operational/example:tlc/status", "/example:tlc/status", sysrepo::Datastore::Operational},
                             {"/restconf/ds/ietf-datastores:startup/example:tlc/status", "/example:tlc/status", sysrepo::Datastore::Startup},
                             {"/restconf/ds/ietf-datastores:candidate/example:tlc/status", "/example:tlc/status", sysrepo::Datastore::Candidate},
                             {"/restconf/ds/ietf-datastores:factory-default/example:tlc/status", "/example:tlc/status", sysrepo::Datastore::FactoryDefault},
                         }) {
                        CAPTURE(httpMethod);
                        CAPTURE(expectedRequestType);
                        CAPTURE(uriPath);
                        REQUIRE(rousette::restconf::impl::parseUriPath(uriPath));
                        auto [requestType, datastore, path, queryParams] = rousette::restconf::asRestconfRequest(ctx, httpMethod, uriPath);
                        REQUIRE(requestType == expectedRequestType);
                        REQUIRE(path == expectedLyPath);
                        REQUIRE(datastore == expectedDatastore);
                        REQUIRE(queryParams.empty());
                    }

                    for (const auto& [uriPath, expectedLyPathParent, expectedLastSegment] : {
                             std::tuple<std::string, std::string, PathSegment>{"/restconf/data/example:top-level-leaf", "", {{"example", "top-level-leaf"}}},
                             {"/restconf/data/example:top-level-list=hello", "", {{"example", "top-level-list"}, {"hello"}}},
                             {"/restconf/data/example:tlc/list=eth0/collection=1", "/example:tlc/list[name='eth0']", {{"example", "collection"}, {"1"}}},
                             {"/restconf/data/example:tlc/status", "/example:tlc", {{"example", "status"}}},
                             {"/restconf/data/example:a/example-augment:b/c", "/example:a/example-augment:b", {{"example-augment", "c"}}},
                             {"/restconf/ds/ietf-datastores:startup/example:a/example-augment:b/c", "/example:a/example-augment:b", {{"example-augment", "c"}}},
                         }) {
                        CAPTURE(httpMethod);
                        CAPTURE(expectedRequestType);
                        CAPTURE(uriPath);
                        auto [parentPath, lastSegment] = rousette::restconf::asLibyangPathSplit(ctx, uriPath);
                        REQUIRE(parentPath == expectedLyPathParent);
                        REQUIRE(lastSegment == expectedLastSegment);
                    }
                }

                SECTION("Datastore resource")
                {
                    std::string uriPath;
                    std::optional<sysrepo::Datastore> expectedDatastore;

                    SECTION("/restconf/data")
                    {
                        uriPath = "/restconf/data";
                    }
                    SECTION("/restconf/ds/")
                    {
                        uriPath = "/restconf/ds/ietf-datastores:running";
                        expectedDatastore = sysrepo::Datastore::Running;
                    }

                    {
                        auto [requestType, datastore, path, queryParams] = rousette::restconf::asRestconfRequest(ctx, "GET", uriPath);
                        REQUIRE(requestType == RestconfRequest::Type::GetData);
                        REQUIRE(path == "/*");
                        REQUIRE(datastore == expectedDatastore);
                        REQUIRE(queryParams.empty());
                    }
                    {
                        auto [requestType, datastore, path, queryParams] = rousette::restconf::asRestconfRequest(ctx, "PUT", uriPath);
                        REQUIRE(requestType == RestconfRequest::Type::CreateOrReplaceThisNode);
                        REQUIRE(path == "/");
                        REQUIRE(datastore == expectedDatastore);
                        REQUIRE(queryParams.empty());
                    }
                    {
                        auto [requestType, datastore, path, queryParams] = rousette::restconf::asRestconfRequest(ctx, "POST", uriPath);
                        REQUIRE(requestType == RestconfRequest::Type::CreateChildren);
                        REQUIRE(path == "/");
                        REQUIRE(datastore == expectedDatastore);
                        REQUIRE(queryParams.empty());
                    }
                }
            }

            SECTION("POST (RPC)")
            {
                std::string uri;
                std::string expectedPath;

                SECTION("RPC")
                {
                    uri = "/restconf/operations/example:test-rpc";
                    expectedPath = "/example:test-rpc";
                }
                SECTION("Action")
                {
                    uri = "/restconf/data/example:tlc/list=hello-world/example-action";
                    expectedPath = "/example:tlc/list[name='hello-world']/example-action";
                }

                CAPTURE(uri);
                auto [action, datastore, path, queryParams] = rousette::restconf::asRestconfRequest(ctx, "POST", uri);
                REQUIRE(path == expectedPath);
                REQUIRE(datastore == std::nullopt);
                REQUIRE(action == RestconfRequest::Type::Execute);
                REQUIRE(queryParams.empty());
            }
        }

        SECTION("Contextually invalid paths")
        {
            int expectedCode;
            std::string expectedErrorType;
            std::string expectedErrorTag;
            std::string expectedErrorMessage;
            std::string uriPath;

            SECTION("GET, PUT, DELETE, POST (data)")
            {
                expectedCode = 400;
                expectedErrorType = "application";
                expectedErrorTag = "operation-failed";

                SECTION("Unparseable URI")
                {
                    uriPath = "/restconf/data///!/@akjsaosdasdlasd";
                    expectedErrorMessage = "Syntax error";
                }

                SECTION("Nonexistent modules and nodes")
                {
                    SECTION("Nonexistent module")
                    {
                        uriPath = "/restconf/data/hello:world";
                        expectedErrorMessage = "Couldn't find schema node: /hello:world";
                    }
                    SECTION("Nonexistent top-level node")
                    {
                        uriPath = "/restconf/data/example:foo";
                        expectedErrorMessage = "Couldn't find schema node: /example:foo";
                    }
                    SECTION("Augment top-level node can't be top-level node")
                    {
                        uriPath = "/restconf/data/example-augment:b";
                        expectedErrorMessage = "Couldn't find schema node: /example-augment:b";
                    }
                    SECTION("Nonexistent node")
                    {
                        uriPath = "/restconf/data/example:tlc/hello-world";
                        expectedErrorMessage = "Node 'hello-world' is not a child of '/example:tlc'";
                    }
                    SECTION("Feature not enabled")
                    {
                        uriPath = "/restconf/data/example:f";
                        expectedErrorMessage = "Couldn't find schema node: /example:f";
                    }
                    SECTION("Schema node")
                    {
                        uriPath = "/restconf/data/example:tlc/list=eth0/choose";
                        expectedErrorMessage = "Node 'choose' is not a child of '/example:tlc/list'";
                    }
                    SECTION("Schema node")
                    {
                        uriPath = "/restconf/data/example:tlc/list=eth0/choose/choice1";
                        expectedErrorMessage = "Node 'choose' is not a child of '/example:tlc/list'";
                    }
                }

                SECTION("Invalid data resources")
                {
                    SECTION("top-level list node")
                    {
                        uriPath = "/restconf/data/example:top-level-list";
                        expectedErrorMessage = "List '/example:top-level-list' requires 1 keys";
                    }
                    SECTION("List node")
                    {
                        uriPath = "/restconf/data/example:tlc/key-less-list";
                        expectedErrorMessage = "List '/example:tlc/key-less-list' has no keys. It can not be accessed directly";
                    }
                    SECTION("leaf-list node")
                    {
                        uriPath = "/restconf/data/example:tlc/list=eth0/collection";
                        expectedErrorMessage = "Leaf-list '/example:tlc/list/collection' requires exactly one key";
                    }
                    SECTION("RPC input node")
                    {
                        uriPath = "/restconf/data/example:test-rpc/i";
                        expectedErrorMessage = "'/example:test-rpc' is an RPC/Action node, any child of it can't be requested";
                    }
                    SECTION("RPC output node")
                    {
                        uriPath = "/restconf/data/example:test-rpc/o";
                        expectedErrorMessage = "'/example:test-rpc' is an RPC/Action node, any child of it can't be requested";
                    }
                }

                SECTION("(Leaf-)list key handling")
                {
                    SECTION("Not a list")
                    {
                        uriPath = "/restconf/data/example:tlc=eth0";
                        expectedErrorMessage = "No keys allowed for node '/example:tlc'";
                    }
                    SECTION("Wrong number of keys in a list")
                    {
                        uriPath = "/restconf/data/example:tlc/list=eth0,eth1";
                        expectedErrorMessage = "List '/example:tlc/list' requires 1 keys";
                    }
                    SECTION("Wrong number of keys in a leaf-list")
                    {
                        uriPath = "/restconf/data/example:tlc/list=eth0/collection=br0,eth1";
                        expectedErrorMessage = "Leaf-list '/example:tlc/list/collection' requires exactly one key";
                    }
                }

                SECTION("Unsupported datastore")
                {
                    uriPath = "/restconf/ds/hello:world/example:tlc";
                    expectedErrorMessage = "Unsupported datastore hello:world";
                }

                for (const auto& httpMethod : {"GET"s, "PUT"s, "DELETE"s, "POST"s}) {
                    CAPTURE(httpMethod);
                    REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, httpMethod, uriPath),
                                           serializeErrorResponse(expectedCode, expectedErrorType, expectedErrorTag, expectedErrorMessage).c_str(),
                                           rousette::restconf::ErrorResponse);
                }
            }

            SECTION("GET, PUT, DELETE with RPC nodes")
            {
                expectedCode = 405;
                expectedErrorType = "protocol";
                expectedErrorTag = "operation-not-supported";

                SECTION("RPC node")
                {
                    uriPath = "/restconf/data/example:test-rpc";
                    expectedErrorMessage = "'/example:test-rpc' is an RPC/Action node";
                }
                SECTION("Action node")
                {
                    uriPath = "/restconf/data/example:tlc/list=eth0/example-action";
                    expectedErrorMessage = "'/example:tlc/list/example-action' is an RPC/Action node";
                }

                for (const auto& httpMethod : {"GET"s, "PUT"s, "DELETE"s}) {
                    CAPTURE(httpMethod);
                    REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, httpMethod, uriPath),
                                           serializeErrorResponse(expectedCode, expectedErrorType, expectedErrorTag, expectedErrorMessage).c_str(),
                                           rousette::restconf::ErrorResponse);
                }
            }

            SECTION("POST (operation)")
            {
                expectedCode = 400;
                expectedErrorType = "protocol";
                expectedErrorTag = "operation-failed";

                SECTION("RPC node missing")
                {
                    uriPath = "/restconf/operations";
                    expectedErrorMessage = "'/' is not an operation resource";
                }
                SECTION("RPC must be invoked via /restconf/operations")
                {
                    uriPath = "/restconf/data/example:test-rpc";
                    expectedErrorMessage = "RPC '/example:test-rpc' must be requested using operation prefix";
                }
                SECTION("actions must be invoked via /restconf/data")
                {
                    uriPath = "/restconf/operations/example:tlc/list=eth0/example-action";
                    expectedErrorMessage = "Action '/example:tlc/list/example-action' must be requested using data prefix";
                }

                SECTION("RPC and action input/output nodes")
                {
                    expectedErrorType = "application";
                    SECTION("RPC")
                    {
                        expectedErrorMessage = "'/example:test-rpc' is an RPC/Action node, any child of it can't be requested";

                        SECTION("Input node")
                        {
                            uriPath = "/restconf/operations/example:test-rpc/i";
                        }
                        SECTION("Output node")
                        {
                            uriPath = "/restconf/operations/example:test-rpc/o";
                        }
                    }
                    SECTION("Action")
                    {
                        expectedErrorMessage = "'/example:tlc/list/example-action' is an RPC/Action node, any child of it can't be requested";

                        SECTION("Input node")
                        {
                            uriPath = "/restconf/data/example:tlc/list=eth0/example-action/i";
                        }
                        SECTION("Output node")
                        {
                            uriPath = "/restconf/data/example:tlc/list=eth0/example-action/o";
                        }
                    }
                }

                REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, "POST", uriPath),
                                       serializeErrorResponse(expectedCode, expectedErrorType, expectedErrorTag, expectedErrorMessage).c_str(),
                                       rousette::restconf::ErrorResponse);
            }

            SECTION("yang-library-version")
            {
                for (const std::string httpMethod : {"PUT", "POST", "PATCH", "DELETE"}) {
                    CAPTURE(httpMethod);
                    REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, httpMethod, "/restconf/yang-library-version"),
                                           serializeErrorResponse(405, "application", "operation-not-supported", "Method not allowed.").c_str(),
                                           rousette::restconf::ErrorResponse);
                }
            }

            SECTION("Unsupported HTTP methods")
            {
                auto exc = serializeErrorResponse(405, "application", "operation-not-supported", "Method not allowed.");
                REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, "HEAD", "/restconf/data/example:top-level-leaf"), exc.c_str(), rousette::restconf::ErrorResponse);
                REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, "OPTIONS", "/restconf/data/example:top-level-leaf"), exc.c_str(), rousette::restconf::ErrorResponse);
                REQUIRE_THROWS_WITH_AS(rousette::restconf::asRestconfRequest(ctx, "PATCH", "/restconf/data"), exc.c_str(), rousette::restconf::ErrorResponse);
            }
        }
    }

    SECTION("YANG schema uri paths")
    {
        SECTION("Parser")
        {
            for (const auto& [uriPath, expected] : {
                     std::pair<std::string, rousette::restconf::impl::YangModule>{"/yang/module_mod", {"module_mod", boost::none}},
                     {"/yang/module_mod", {"module_mod", boost::none}},
                     {"/yang/_mo1-dule.yang", {"_mo1-dule.yang", boost::none}},
                     {"/yang/yang.yang", {"yang.yang", boost::none}},
                     {"/yang/yang.yang@2024-02-28.yang", {"yang.yang", "2024-02-28"s}},
                     {"/yang/mod123@2020-02-21", {"mod123", "2020-02-21"s}},
                     {"/yang/mod123@66666-12-31", {"mod123", "66666-12-31"s}},
                     {"/yang/ietf-system@2014-01-06.yang", {"ietf-system", "2014-01-06"s}},
                 }) {
                CAPTURE(uriPath);
                REQUIRE(rousette::restconf::impl::parseModuleWithRevision(uriPath) == expected);
            }

            for (const std::string uriPath : {
                     "/yang",
                     "/yang/",
                     "/yang/module@a",
                     "/yang/.yang",
                     "/yang/1.yang",
                     "/yang/module@aaaa-bb-cc",
                     "yang/module@2024-02-27", /* intentional missing leading slash */
                     "/yang/module@1234-123-12",
                     "/yang/module@1234-12",
                     "/yang/module@123-12-12",
                     "/yang/module@1234",
                     "/yang/@2020-02-02",
                     "/yang/@1234",
                 }) {
                CAPTURE(uriPath);
                REQUIRE(!rousette::restconf::impl::parseModuleWithRevision(uriPath));
            }
        }

        SECTION("Get modules")
        {
            auto ctx = libyang::Context{std::filesystem::path{CMAKE_CURRENT_SOURCE_DIR} / "tests" / "yang"};
            auto mod = ctx.loadModule("example", std::nullopt, {"f1"});
            ctx.loadModule("ietf-netconf-acm", "2018-02-14");
            ctx.loadModule("root-mod", std::nullopt);

            auto modName = [](auto&& mod) { return std::visit([](auto&& arg) { return std::string{arg.name()}; }, mod); };

            SECTION("Module without revision")
            {
                SECTION("Revision in URI")
                {
                    REQUIRE(!rousette::restconf::asYangModule(ctx, "/yang/example@2020-02-02"));
                }

                SECTION("No revision in URI")
                {
                    std::string uri;
                    std::string expectedModuleName;

                    SECTION("Module")
                    {
                        uri = "/yang/example";
                        expectedModuleName = "example";
                    }

                    SECTION("Module with imports and submodules")
                    {
                        uri = "/yang/root-mod";
                        expectedModuleName = "root-mod";
                    }

                    SECTION("Submodule")
                    {
                        uri = "/yang/root-submod";
                        expectedModuleName = "root-submod";
                    }

                    SECTION("Imported module")
                    {
                        uri = "/yang/imp-mod";
                        expectedModuleName = "imp-mod";
                    }

                    SECTION("Imported submodule")
                    {
                        uri = "/yang/imp-submod";
                        expectedModuleName = "imp-submod";
                    }

                    auto mod = rousette::restconf::asYangModule(ctx, uri);
                    REQUIRE(mod);
                    REQUIRE(modName(*mod) == expectedModuleName);
                }
            }

            SECTION("Module with revision")
            {
                SECTION("Correct revision in URI")
                {
                    auto mod = rousette::restconf::asYangModule(ctx, "/yang/ietf-netconf-acm@2018-02-14");
                    REQUIRE(mod);
                    REQUIRE(modName(*mod) == "ietf-netconf-acm");
                }

                SECTION("Incorrect revision in URI")
                {
                    REQUIRE(!rousette::restconf::asYangModule(ctx, "/yang/ietf-netconf-acm@2020-02-02"));
                }

                SECTION("No revision in URI")
                {
                    REQUIRE(!rousette::restconf::asYangModule(ctx, "/yang/ietf-netconf-acm"));
                }
            }
        }
    }

    SECTION("query params")
    {
        using rousette::restconf::asRestconfRequest;
        using rousette::restconf::RestconfRequest;
        using rousette::restconf::impl::parseQueryParams;
        using namespace rousette::restconf::queryParams;

        SECTION("Parsing")
        {
            REQUIRE(parseQueryParams("") == QueryParams{});
            REQUIRE(parseQueryParams("depth=65535") == QueryParams{{"depth", 65535u}});
            REQUIRE(parseQueryParams("depth=unbounded") == QueryParams{{"depth", UnboundedDepth{}}});
            REQUIRE(parseQueryParams("depth=1&depth=unbounded") == QueryParams{{"depth", 1u}, {"depth", UnboundedDepth{}}});
            REQUIRE(parseQueryParams("depth=unbounded&depth=123") == QueryParams{{"depth", UnboundedDepth{}}, {"depth", 123u}});
            REQUIRE(parseQueryParams("a=b") == std::nullopt);
            REQUIRE(parseQueryParams("Depth=1") == std::nullopt);
            REQUIRE(parseQueryParams("depth=-1") == std::nullopt);
            REQUIRE(parseQueryParams("depth=0") == std::nullopt);
            REQUIRE(parseQueryParams("depth=65536") == std::nullopt);
            REQUIRE(parseQueryParams("depth=") == std::nullopt);
            REQUIRE(parseQueryParams("depth=foo") == std::nullopt);
            REQUIRE(parseQueryParams("=") == std::nullopt);
            REQUIRE(parseQueryParams("&") == std::nullopt);
            REQUIRE(parseQueryParams("depth=1&") == std::nullopt);
            REQUIRE(parseQueryParams("a&b=a") == std::nullopt);
            REQUIRE(parseQueryParams("with-defaults=report-all") == QueryParams{{"with-defaults", withDefaults::ReportAll{}}});
            REQUIRE(parseQueryParams("with-defaults=trim") == QueryParams{{"with-defaults", withDefaults::Trim{}}});
            REQUIRE(parseQueryParams("with-defaults=explicit") == QueryParams{{"with-defaults", withDefaults::Explicit{}}});
            REQUIRE(parseQueryParams("with-defaults=report-all-tagged") == QueryParams{{"with-defaults", withDefaults::ReportAllTagged{}}});
            REQUIRE(parseQueryParams("depth=3&with-defaults=report-all") == QueryParams{{"depth", 3u}, {"with-defaults", withDefaults::ReportAll{}}});
            REQUIRE(parseQueryParams("with-defaults=") == std::nullopt);
            REQUIRE(parseQueryParams("with-defaults=report") == std::nullopt);
            REQUIRE(parseQueryParams("with_defaults=ahoj") == std::nullopt);
            REQUIRE(parseQueryParams("with-defaults=report_all") == std::nullopt);
            REQUIRE(parseQueryParams("with-defaults=depth=3") == std::nullopt);
            REQUIRE(parseQueryParams("with-defaults=&depth=3") == std::nullopt);
            REQUIRE(parseQueryParams("with-defaults=trim;depth=3") == std::nullopt);
            REQUIRE(parseQueryParams("with-defaults=trim=depth=3") == std::nullopt);
            REQUIRE(parseQueryParams("content=all&content=nonconfig&content=config") == QueryParams{{"content", content::AllNodes{}}, {"content", content::OnlyNonConfigNodes{}}, {"content", content::OnlyConfigNodes{}}});
            REQUIRE(parseQueryParams("content=ahoj") == std::nullopt);
            REQUIRE(parseQueryParams("insert=first") == QueryParams{{"insert", insert::First{}}});
            REQUIRE(parseQueryParams("insert=last") == QueryParams{{"insert", insert::Last{}}});
            REQUIRE(parseQueryParams("insert=foo") == std::nullopt);
            REQUIRE(parseQueryParams("depth=4&insert=last&with-defaults=trim") == QueryParams{{"depth", 4u}, {"insert", insert::Last{}}, {"with-defaults", withDefaults::Trim{}}});
            REQUIRE(parseQueryParams("insert=before") == QueryParams{{"insert", insert::Before{}}});
            REQUIRE(parseQueryParams("insert=after") == QueryParams{{"insert", insert::After{}}});
            REQUIRE(parseQueryParams("insert=uwu") == std::nullopt);
        }

        SECTION("Full requests with validation")
        {
            auto ctx = libyang::Context{std::filesystem::path{CMAKE_CURRENT_SOURCE_DIR} / "tests" / "yang"};
            ctx.loadModule("example", std::nullopt, {"f1"});
            ctx.loadModule("example-augment");

            SECTION("Depth")
            {
                auto r1 = asRestconfRequest(ctx, "GET", "/restconf/data/example:tlc", "depth=unbounded");
                REQUIRE(r1.queryParams == QueryParams({{"depth", UnboundedDepth{}}}));

                auto r2 = asRestconfRequest(ctx, "GET", "/restconf/data/example:tlc", "depth=11111");
                REQUIRE(r2.queryParams == QueryParams({{"depth", 11111u}}));

                REQUIRE_THROWS_WITH_AS(asRestconfRequest(ctx, "POST", "/restconf/data/example:tlc", "depth=1&depth=2"),
                                       serializeErrorResponse(400, "protocol", "invalid-value", "Query parameter 'depth' already specified").c_str(),
                                       rousette::restconf::ErrorResponse);

                REQUIRE_THROWS_WITH_AS(asRestconfRequest(ctx, "POST", "/restconf/data/example:tlc", "depth=1"),
                                       serializeErrorResponse(400, "protocol", "invalid-value", "Query parameter 'depth' can be used only with GET and HEAD methods").c_str(),
                                       rousette::restconf::ErrorResponse);
            }

            SECTION("with-default")
            {
                auto resp = asRestconfRequest(ctx, "GET", "/restconf/data/example:tlc", "with-defaults=report-all");
                REQUIRE(resp.queryParams == QueryParams({{"with-defaults", withDefaults::ReportAll{}}}));

                REQUIRE_THROWS_WITH_AS(asRestconfRequest(ctx, "POST", "/restconf/data/example:tlc", "with-defaults=report-all"),
                                       serializeErrorResponse(400, "protocol", "invalid-value", "Query parameter 'with-defaults' can be used only with GET and HEAD methods").c_str(),
                                       rousette::restconf::ErrorResponse);
            }

            SECTION("content")
            {
                auto resp = asRestconfRequest(ctx, "GET", "/restconf/data/example:tlc", "content=nonconfig");
                REQUIRE(resp.queryParams == QueryParams({{"content", content::OnlyNonConfigNodes{}}}));

                REQUIRE_THROWS_WITH_AS(asRestconfRequest(ctx, "POST", "/restconf/data/example:tlc", "content=config"),
                                       serializeErrorResponse(400, "protocol", "invalid-value", "Query parameter 'content' can be used only with GET and HEAD methods").c_str(),
                                       rousette::restconf::ErrorResponse);
            }

            SECTION("insert first/last")
            {
                auto resp = asRestconfRequest(ctx, "PUT", "/restconf/data/example:tlc", "insert=first");
                REQUIRE(resp.queryParams == QueryParams({{"insert", insert::First{}}}));

                resp = asRestconfRequest(ctx, "POST", "/restconf/data/example:tlc", "insert=last");
                REQUIRE(resp.queryParams == QueryParams({{"insert", insert::Last{}}}));

                REQUIRE_THROWS_WITH_AS(asRestconfRequest(ctx, "GET", "/restconf/data/example:tlc", "insert=first"),
                                       serializeErrorResponse(400, "protocol", "invalid-value", "Query parameter 'insert' can be used only with POST and PUT methods").c_str(),
                                       rousette::restconf::ErrorResponse);
            }

            SECTION("insert before/after")
            {
                auto resp = asRestconfRequest(ctx, "PUT", "/restconf/data/example:tlc", "insert=before&point=/example:ordered-lists/lst=key");
                REQUIRE(resp.queryParams == QueryParams({
                            {"insert", insert::Before{}},
                            {"point", insert::PointParsed({
                                          {{"example", "ordered-lists"}, {}},
                                          {{"lst"}, {"key"}},
                                      })},
                        }));

                resp = asRestconfRequest(ctx, "POST", "/restconf/data/example:tlc", "point=/example:ordered-lists/ll=key&insert=after");
                REQUIRE(resp.queryParams == QueryParams({
                            {"point", insert::PointParsed({
                                          {{"example", "ordered-lists"}, {}},
                                          {{"ll"}, {"key"}},
                                      })},
                            {"insert", insert::After{}},
                        }));

                REQUIRE_THROWS_WITH_AS(asRestconfRequest(ctx, "POST", "/restconf/data/example:ordered-lists", "insert=after"),
                                       serializeErrorResponse(400, "protocol", "invalid-value", "Query parameter 'point' must always come with parameter 'insert' set to 'before' or 'after'").c_str(),
                                       rousette::restconf::ErrorResponse);
                REQUIRE_THROWS_WITH_AS(asRestconfRequest(ctx, "POST", "/restconf/data/example:ordered-lists", "point=/example:ordered-lists/ll=key"),
                                       serializeErrorResponse(400, "protocol", "invalid-value", "Query parameter 'point' must always come with parameter 'insert' set to 'before' or 'after'").c_str(),
                                       rousette::restconf::ErrorResponse);
                REQUIRE_THROWS_WITH_AS(asRestconfRequest(ctx, "POST", "/restconf/data/example:ordered-lists", "insert=after&point=/invalid:path"),
                                       serializeErrorResponse(400, "application", "operation-failed", "Couldn't find schema node: /invalid:path").c_str(),
                                       rousette::restconf::ErrorResponse);
            }

            REQUIRE_THROWS_WITH_AS(asRestconfRequest(ctx, "GET", "/restconf/data/example:tlc", "hello=world"),
                                   serializeErrorResponse(400, "protocol", "invalid-value", "Query parameters syntax error").c_str(),
                                   rousette::restconf::ErrorResponse);
        }
    }
}
