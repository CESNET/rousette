/*
 * Copyright (C) 2016-2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <experimental/iterator>
#include <sysrepo-cpp/Connection.hpp>
#include "restconf/uri.h"
#include "restconf/uri_impl.h"
#include "tests/configure.cmake.h"

namespace rousette::restconf {

std::ostream& operator<<(std::ostream& os, const ApiIdentifier& obj)
{
    os << "ApiIdentifier{";
    os << "prefix=";
    if (obj.prefix) {
        os << "'" << *obj.prefix << "'";
    } else {
        os << "nullopt{}";
    }

    return os << "ident='" << obj.identifier << "'}";
}

std::ostream& operator<<(std::ostream& os, const PathSegment& obj)
{

    os << "Segment{" << obj.apiIdent << " "
       << "keys=";
    os << "[";
    std::copy(obj.keys.begin(), obj.keys.end(), std::experimental::make_ostream_joiner(os, ", "));
    return os << "]}";
}

std::ostream& operator<<(std::ostream& os, const std::vector<PathSegment>& v)
{
    os << "[";
    std::copy(v.begin(), v.end(), std::experimental::make_ostream_joiner(os, ", "));
    return os << "]";
}
}

namespace rousette::restconf::impl {
std::ostream& operator<<(std::ostream& os, const ResourcePath& obj)
{
    os << "[";
    std::copy(obj.segments.begin(), obj.segments.end(), std::experimental::make_ostream_joiner(os, ", "));
    return os << "]";
}
}

namespace doctest {
template <>
struct StringMaker<std::optional<std::string>> {
    static String convert(const std::optional<std::string>& obj)
    {
        std::ostringstream oss;

        if (obj) {
            oss << "optional{" << *obj << "}";
        } else {
            oss << "nullopt{}";
        }

        return oss.str().c_str();
    }
};

template <>
struct StringMaker<std::optional<rousette::restconf::impl::ResourcePath>> {
    static String convert(const std::optional<rousette::restconf::impl::ResourcePath>& obj)
    {
        std::ostringstream oss;

        if (obj) {
            oss << "optional{" << obj->segments << "}";
        } else {
            oss << "nullopt{}";
        }

        return oss.str().c_str();
    }
};
}

TEST_CASE("URI path parser")
{
    using rousette::restconf::ApiIdentifier;
    using rousette::restconf::PathSegment;
    using rousette::restconf::impl::ResourcePath;

    SECTION("Valid paths")
    {
        for (const auto& [uriPath, expected] : {
                 std::pair<std::string, ResourcePath>{"/restconf/data/x333:y666", ResourcePath({
                                                                                      {{"x333", "y666"}},
                                                                                  })},
                 {"/restconf/data/foo:bar", ResourcePath(std::vector<PathSegment>{
                                                {{"foo", "bar"}},
                                            })},
                 {"/restconf/data/foo:bar/baz", ResourcePath({
                                                    {{"foo", "bar"}},
                                                    {{"baz"}},
                                                })},
                 {"/restconf/data/foo:bar/meh:baz", ResourcePath({
                                                        {{"foo", "bar"}},
                                                        {{"meh", "baz"}},
                                                    })},
                 {"/restconf/data/foo:bar/yay/meh:baz", ResourcePath({
                                                            {{"foo", "bar"}},
                                                            {{"yay"}},
                                                            {{"meh", "baz"}},
                                                        })},
                 {"/restconf/data/foo:bar/Y=val", ResourcePath({
                                                      {{"foo", "bar"}},
                                                      {{"Y"}, {"val"}},
                                                  })},
                 {"/restconf/data/foo:bar/Y=val-ue", ResourcePath({
                                                         {{"foo", "bar"}},
                                                         {{"Y"}, {"val-ue"}},
                                                     })},
                 {"/restconf/data/foo:bar/p:lst=key1", ResourcePath({
                                                           {{"foo", "bar"}},
                                                           {{"p", "lst"}, {"key1"}},
                                                       })},

                 {"/restconf/data/foo:bar/p:lst=key1/leaf", ResourcePath({
                                                                {{"foo", "bar"}},
                                                                {{"p", "lst"}, {"key1"}},
                                                                {{"leaf"}},
                                                            })},
                 {"/restconf/data/foo:bar/lst=key1,", ResourcePath({
                                                          {{"foo", "bar"}},
                                                          {{"lst"}, {"key1", ""}},
                                                      })},
                 {"/restconf/data/foo:bar/lst=key1,,,", ResourcePath({
                                                            {{"foo", "bar"}},
                                                            {{"lst"}, {"key1", "", "", ""}},
                                                        })},
                 {"/restconf/data/foo:bar/lst=key1,/leaf", ResourcePath({
                                                               {{"foo", "bar"}},
                                                               {{"lst"}, {"key1", ""}},
                                                               {{"leaf"}},
                                                           })},
                 {"/restconf/data/foo:bar/lst=key1,key2", ResourcePath({
                                                              {{"foo", "bar"}},
                                                              {{"lst"}, {"key1", "key2"}},
                                                          })},
                 {"/restconf/data/foo:bar/lst=key1,key2/leaf", ResourcePath({
                                                                   {{"foo", "bar"}},
                                                                   {{"lst"}, {"key1", "key2"}},
                                                                   {{"leaf"}},
                                                               })},
                 {"/restconf/data/foo:bar/lst=key1,key2/lst2=key1/leaf", ResourcePath({
                                                                             {{"foo", "bar"}},
                                                                             {{"lst"}, {"key1", "key2"}},
                                                                             {{"lst2"}, {"key1"}},
                                                                             {{"leaf"}},
                                                                         })},
                 {"/restconf/data/foo:bar/lst=,key2/lst2=key1/leaf", ResourcePath({
                                                                         {{"foo", "bar"}},
                                                                         {{"lst"}, {"", "key2"}},
                                                                         {{"lst2"}, {"key1"}},
                                                                         {{"leaf"}},
                                                                     })},
                 {"/restconf/data/foo:bar/lst=,/lst2=key1/leaf", ResourcePath({
                                                                     {{"foo", "bar"}},
                                                                     {{"lst"}, {"", ""}},
                                                                     {{"lst2"}, {"key1"}},
                                                                     {{"leaf"}},
                                                                 })},
                 {"/restconf/data/foo:bar/lst=", ResourcePath({
                                                     {{"foo", "bar"}},
                                                     {{"lst"}, {""}},
                                                 })},
                 {"/restconf/data/foo:bar/lst=/leaf", ResourcePath({
                                                          {{"foo", "bar"}},
                                                          {{"lst"}, {""}},
                                                          {{"leaf"}},
                                                      })},
                 {"/restconf/data/foo:bar/prefix:lst=key1/prefix:leaf", ResourcePath({
                                                                            {{"foo", "bar"}},
                                                                            {{"prefix", "lst"}, {"key1"}},
                                                                            {{"prefix", "leaf"}},
                                                                        })},
                 {"/restconf/data/foo:bar/lst=key1,,key3", ResourcePath({
                                                               {{"foo", "bar"}},
                                                               {{"lst"}, {"key1", "", "key3"}},
                                                           })},
                 {"/restconf/data/foo:bar/lst=key%2CWithCommas,,key2C", ResourcePath({
                                                                            {{"foo", "bar"}},
                                                                            {{"lst"}, {"key,WithCommas", "", "key2C"}},
                                                                        })},
                 {R"(/restconf/data/foo:bar/list1=%2C%27"%3A"%20%2F,,foo)", ResourcePath({
                                                                                {{"foo", "bar"}},
                                                                                {{"list1"}, {R"(,'":" /)", "", "foo"}},
                                                                            })},
                 {"/restconf/data/foo:bar/list1= %20,%20,foo", ResourcePath({
                                                                   {{"foo", "bar"}},
                                                                   {{"list1"}, {"  ", " ", "foo"}},
                                                               })},
                 {"/restconf/data/foo:bar/list1= %20,%20, ", ResourcePath({
                                                                 {{"foo", "bar"}},
                                                                 {{"list1"}, {"  ", " ", " "}},
                                                             })},
                 {"/restconf/data/foo:bar/list1=žluťoučkýkůň", ResourcePath({
                                                                   {{"foo", "bar"}},
                                                                   {{"list1"}, {"žluťoučkýkůň"}},
                                                               })},
                 {"/restconf/data/foo:list=A%20Z", ResourcePath({
                                                       {{"foo", "list"}, {"A Z"}},
                                                   })},
                 {"/restconf/data/foo:list=A%25Z", ResourcePath({
                                                       {{"foo", "list"}, {"A%Z"}},
                                                   })},
                 {"/restconf/data", ResourcePath({}, {})},

                 // RFC 8527 uris
                 {"/restconf/ds/hello:world", ResourcePath(ApiIdentifier{"hello", "world"}, {})},
                 {"/restconf/ds/ietf-datastores:running/foo:bar/list1=a", ResourcePath(ApiIdentifier{"ietf-datastores", "running"}, {{{"foo", "bar"}}, {{"list1"}, {"a"}}})},
                 {"/restconf/ds/ietf-datastores:operational", ResourcePath(ApiIdentifier{"ietf-datastores", "operational"}, {})},
             }) {

            CAPTURE(uriPath);
            auto path = rousette::restconf::impl::parseUriPath(uriPath);
            REQUIRE(path == expected);
        }
    }

    SECTION("Invalid URIs")
    {
        for (const auto& uriPath : {
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
                 "/restconf/ds/ietf-datastores:operational/",
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
            for (const auto& [uriPath, expectedLyPath, expectedDatastore] : {
                     std::tuple<std::string, std::string, sysrepo::Datastore>{"/restconf/data/example:top-level-leaf", "/example:top-level-leaf", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:top-level-list=hello", "/example:top-level-list[name='hello']", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:l/list=eth0", "/example:l/list[name='eth0']", sysrepo::Datastore::Operational},
                     {R"(/restconf/data/example:l/list=et"h0)", R"(/example:l/list[name='et"h0'])", sysrepo::Datastore::Operational},
                     {R"(/restconf/data/example:l/list=et%22h0)", R"(/example:l/list[name='et"h0'])", sysrepo::Datastore::Operational},
                     {R"(/restconf/data/example:l/list=et%27h0)", R"(/example:l/list[name="et'h0"])", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:l/list=eth0/name", "/example:l/list[name='eth0']/name", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:l/list=eth0/nested=1,2,3", "/example:l/list[name='eth0']/nested[first='1'][second='2'][third='3']", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:l/list=eth0/nested=,2,3", "/example:l/list[name='eth0']/nested[first=''][second='2'][third='3']", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:l/list=eth0/nested=,2,", "/example:l/list[name='eth0']/nested[first=''][second='2'][third='']", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:l/list=eth0/choice1", "/example:l/list[name='eth0']/choice1", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:l/list=eth0/choice2", "/example:l/list[name='eth0']/choice2", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:l/list=eth0/collection=val", "/example:l/list[name='eth0']/collection[.='val']", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:l/status", "/example:l/status", sysrepo::Datastore::Operational},
                     // container example:a has a container b inserted locally and also via an augment. Check that we return the correct one
                     {"/restconf/data/example:a/b", "/example:a/b", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:a/b/c", "/example:a/b/c", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:a/b/c/enabled", "/example:a/b/c/enabled", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:a/example-augment:b", "/example:a/example-augment:b", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:a/example-augment:b/c", "/example:a/example-augment:b/c", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:a/example-augment:b/example-augment:c", "/example:a/example-augment:b/c", sysrepo::Datastore::Operational},
                     {"/restconf/data/example:a/example-augment:b/c/enabled", "/example:a/example-augment:b/c/enabled", sysrepo::Datastore::Operational},
                     // rfc 8527
                     {"/restconf/ds/ietf-datastores:running/example:l/status", "/example:l/status", sysrepo::Datastore::Running},
                     {"/restconf/ds/ietf-datastores:operational/example:l/status", "/example:l/status", sysrepo::Datastore::Operational},
                     {"/restconf/ds/ietf-datastores:startup/example:l/status", "/example:l/status", sysrepo::Datastore::Startup},
                     {"/restconf/ds/ietf-datastores:candidate/example:l/status", "/example:l/status", sysrepo::Datastore::Candidate},
                 }) {
                CAPTURE(uriPath);
                REQUIRE(rousette::restconf::impl::parseUriPath(uriPath));
                auto datastoreAndPath = rousette::restconf::asLibyangPath(ctx, uriPath);
                REQUIRE(datastoreAndPath.path == expectedLyPath);
                REQUIRE(datastoreAndPath.datastore == expectedDatastore);
            }

            for (const auto& [uriPath, expectedLyPathParent, expectedDatastore, expectedLastSegment] : {
                     std::tuple<std::string, std::string, sysrepo::Datastore, PathSegment>{"/restconf/data/example:top-level-leaf", "", sysrepo::Datastore::Operational, PathSegment({"example", "top-level-leaf"})},
                     {"/restconf/data/example:top-level-list=hello", "", sysrepo::Datastore::Operational, PathSegment({"example", "top-level-list"}, {"hello"})},
                     {"/restconf/data/example:l/list=eth0/collection=1", "/example:l/list[name='eth0']", sysrepo::Datastore::Operational, PathSegment({"example", "collection"}, {"1"})},
                     {"/restconf/data/example:l/status", "/example:l", sysrepo::Datastore::Operational, PathSegment({"example", "status"})},
                     {"/restconf/data/example:a/example-augment:b/c", "/example:a/example-augment:b", sysrepo::Datastore::Operational, PathSegment({"example-augment", "c"})},
                     {"/restconf/ds/ietf-datastores:startup/example:a/example-augment:b/c", "/example:a/example-augment:b", sysrepo::Datastore::Startup, PathSegment({"example-augment", "c"})},
                 }) {
                CAPTURE(uriPath);
                auto [datastoreAndPathParent, lastSegment] = rousette::restconf::asLibyangPathSplit(ctx, uriPath);
                REQUIRE(datastoreAndPathParent.path == expectedLyPathParent);
                REQUIRE(datastoreAndPathParent.datastore == expectedDatastore);
                REQUIRE(lastSegment == expectedLastSegment);
            }
        }

        SECTION("Contextually invalid paths")
        {
            for (const auto& uriPath : std::vector<std::string>{
                     "/restconf/data/hello:world", // nonexistent module
                     "/restconf/data/example:foo", // nonexistent top-level node
                     "/restconf/data/example-augment:b", // nonexistent top-level node
                     "/restconf/data/example:l/hello-world", // nonexistent node
                     "/restconf/data/example:f", // feature not enabled
                     "/restconf/data/example:top-level-list", // list is not a data resource
                     "/restconf/data/example:l/key-less-list", // list is not a data resource
                     "/restconf/data/example:l/list=eth0/collection", // leaf-list is not a data resource
                     "/restconf/data/example:test-rpc", // RPC is not a data resource
                     "/restconf/data/example:test-rpc/i", // RPC input node is not a data resource
                     "/restconf/data/example:test-rpc/o", // RPC output node is not a data resource
                     "/restconf/data/example:l=eth0", // node not a (leaf-)list
                     "/restconf/data/example:l/list=eth0,eth1", // wrong number of list elements
                     "/restconf/data/example:l/list=eth0/collection=br0,eth1", // wrong number of keys for a leaf-list
                     "/restconf/data/example:l/list=eth0/choose", // schema nodes should not be visible
                     "/restconf/data/example:l/list=eth0/choose/choice1", // schema nodes should not be visible
                     "/restconf/ds/hello:world/example:l", // unsupported datastore
                 }) {
                CAPTURE(uriPath);
                REQUIRE(rousette::restconf::impl::parseUriPath(uriPath));
                REQUIRE_THROWS_AS(rousette::restconf::asLibyangPath(ctx, uriPath), rousette::restconf::InvalidURIException);
            }
        }
    }
}
