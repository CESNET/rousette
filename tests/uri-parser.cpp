/*
 * Copyright (C) 2016-2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include "restconf/uri.h"

using namespace std::string_literals;

namespace rousette::restconf {
std::ostream& operator<<(std::ostream& s, const std::optional<std::string>& x)
{
    if (!x) {
        return s << "nullopt{}";
    }
    return s << "optional{" << *x << "}";
}
std::ostream& operator<<(std::ostream& os, const std::vector<std::string>& o)
{
    os << "[";
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (it != o.begin())
            os << " , ";
        os << "'" << *it << "'";
    }

    return os << "]";
}

std::ostream& operator<<(std::ostream& os, const rousette::restconf::ApiIdentifier& o)
{
    os << "ApiIdentifier{";
    if (o.prefix) {
        os << "prefix='" << *o.prefix << "', ";
    }
    return os << "ident='" << o.identifier << "'}";
}

std::ostream& operator<<(std::ostream& os, const rousette::restconf::PathSegment& o)
{
    return os << "Segment{" << o.apiIdent << " " << "keys=" << o.keys << "}";
}

std::ostream& operator<<(std::ostream& os, const rousette::restconf::ResourcePath& o)
{
    auto segments = o.getSegments();

    os << "[";
    for (auto it = segments.begin(); it != segments.end(); ++it) {
        if (it != segments.begin())
            os << ", ";
        os << *it;
    }

    return os << "]";
}
}

namespace doctest {
template <>
struct StringMaker<std::optional<rousette::restconf::ResourcePath>> {
    static String convert(const std::optional<rousette::restconf::ResourcePath>& obj)
    {
        std::ostringstream oss;

        if (obj) {
            oss << "optional{" << obj->getSegments() << "}";
        } else {
            oss << "nullopt{}";
        }

        return oss.str().c_str();
    }
};
}

TEST_CASE("URI path parser")
{
    using rousette::restconf::ResourcePath;
    using rousette::restconf::PathSegment;

    SECTION("Valid paths")
    {
        for (const auto& [uriPath, expected] : {
                 std::pair<std::string, ResourcePath>{"/restconf/data/x333:y666", ResourcePath(std::vector<PathSegment>{
                                                                                      {{"x333"s, "y666"}, {}},
                                                                                  })},

                 {"/restconf/data/foo:bar", ResourcePath(std::vector<PathSegment>{
                                                {{"foo"s, "bar"}, {}},
                                            })},

                 {"/restconf/data/foo:bar/baz", ResourcePath({
                                                    {{"foo"s, "bar"}, {}},
                                                    {{{}, "baz"}, {}},
                                                })},

                 {"/restconf/data/foo:bar/meh:baz", ResourcePath({
                                                        {{"foo"s, "bar"}, {}},
                                                        {{"meh"s, "baz"}, {}},
                                                    })},

                 {"/restconf/data/foo:bar/yay/meh:baz", ResourcePath({
                                                            {{"foo"s, "bar"}, {}},
                                                            {{{}, "yay"}, {}},
                                                            {{"meh"s, "baz"}, {}},
                                                        })},
                 {"/restconf/data/foo:bar/Y=val", ResourcePath({
                                                      {{"foo"s, "bar"}, {}},
                                                      {{{}, "Y"}, {"val"}},
                                                  })},
                 {"/restconf/data/foo:bar/Y=val-ue", ResourcePath({
                                                         {{"foo"s, "bar"}, {}},
                                                         {{{}, "Y"}, {"val-ue"}},
                                                     })},
                 {"/restconf/data/foo:bar/p:lst=key1", ResourcePath({
                                                           {{"foo"s, "bar"}, {}},
                                                           {{"p"s, "lst"}, {"key1"}},
                                                       })},

                 {"/restconf/data/foo:bar/p:lst=key1/leaf", ResourcePath({
                                                                {{"foo"s, "bar"}, {}},
                                                                {{"p"s, "lst"}, {"key1"}},
                                                                {{{}, "leaf"}, {}},
                                                            })},

                 {"/restconf/data/foo:bar/lst=key1,", ResourcePath({
                                                          {{"foo"s, "bar"}, {}},
                                                          {{{}, "lst"}, {"key1", ""}},
                                                      })},

                 {"/restconf/data/foo:bar/lst=key1,,,", ResourcePath({
                                                            {{"foo"s, "bar"}, {}},
                                                            {{{}, "lst"}, {"key1", "", "", ""}},
                                                        })},

                 {"/restconf/data/foo:bar/lst=key1,/leaf", ResourcePath({
                                                               {{"foo"s, "bar"}, {}},
                                                               {{{}, "lst"}, {"key1", ""}},
                                                               {{{}, "leaf"}, {}},
                                                           })},

                 {"/restconf/data/foo:bar/lst=key1,key2", ResourcePath({
                                                              {{"foo"s, "bar"}, {}},
                                                              {{{}, "lst"}, {"key1", "key2"}},
                                                          })},

                 {"/restconf/data/foo:bar/lst=key1,key2/leaf", ResourcePath({
                                                                   {{"foo"s, "bar"}, {}},
                                                                   {{{}, "lst"}, {"key1", "key2"}},
                                                                   {{{}, "leaf"}, {}},
                                                               })},
                 {"/restconf/data/foo:bar/lst=key1,key2/lst2=key1/leaf", ResourcePath({
                                                                             {{"foo"s, "bar"}, {}},
                                                                             {{{}, "lst"}, {"key1", "key2"}},
                                                                             {{{}, "lst2"}, {"key1"}},
                                                                             {{{}, "leaf"}, {}},
                                                                         })},

                 {"/restconf/data/foo:bar/lst=,key2/lst2=key1/leaf", ResourcePath({
                                                                         {{"foo"s, "bar"}, {}},
                                                                         {{{}, "lst"}, {"", "key2"}},
                                                                         {{{}, "lst2"}, {"key1"}},
                                                                         {{{}, "leaf"}, {}},
                                                                     })},

                 {"/restconf/data/foo:bar/lst=,/lst2=key1/leaf", ResourcePath({
                                                                     {{"foo"s, "bar"}, {}},
                                                                     {{{}, "lst"}, {"", ""}},
                                                                     {{{}, "lst2"}, {"key1"}},
                                                                     {{{}, "leaf"}, {}},
                                                                 })},
                 {"/restconf/data/foo:bar/lst=", ResourcePath({
                                                     {{"foo"s, "bar"}, {}},
                                                     {{{}, "lst"}, {""}},
                                                 })},

                 {"/restconf/data/foo:bar/lst=/leaf", ResourcePath({
                                                          {{"foo"s, "bar"}, {}},
                                                          {{{}, "lst"}, {""}},
                                                          {{{}, "leaf"}, {}},
                                                      })},

                 {"/restconf/data/foo:bar/prefix:lst=key1/prefix:leaf", ResourcePath({
                                                                            {{"foo"s, "bar"}, {}},
                                                                            {{"prefix"s, "lst"}, {"key1"}},
                                                                            {{"prefix"s, "leaf"}, {}},
                                                                        })},

                 {"/restconf/data/foo:bar/lst=key1,,key3", ResourcePath({
                                                               {{"foo"s, "bar"}, {}},
                                                               {{{}, "lst"}, {"key1", "", "key3"}},
                                                           })},

                 {"/restconf/data/foo:bar/lst=key%2CWithCommas,,key2C", ResourcePath({
                                                                            {{"foo"s, "bar"}, {}},
                                                                            {{{}, "lst"}, {"key,WithCommas", "", "key2C"}},
                                                                        })},

                 {R"(/restconf/data/foo:bar/list1=%2C%27"%3A"%20%2F,,foo)", ResourcePath({
                                                                                {{"foo"s, "bar"}, {}},
                                                                                {{{}, "list1"}, {R"(,'":" /)", "", "foo"}},
                                                                            })},

                 {"/restconf/data/foo:bar/list1= %20,%20,foo", ResourcePath({
                                                                   {{"foo"s, "bar"}, {}},
                                                                   {{{}, "list1"}, {"  ", " ", "foo"}},
                                                               })},

                 {"/restconf/data/foo:bar/list1= %20,%20, ", ResourcePath({
                                                                 {{"foo"s, "bar"}, {}},
                                                                 {{{}, "list1"}, {"  ", " ", " "}},
                                                             })},

                 {"/restconf/data/foo:bar/list1=žluťoučkýkůň", ResourcePath({
                                                                   {{"foo"s, "bar"}, {}},
                                                                   {{{}, "list1"}, {"žluťoučkýkůň"}},
                                                               })},

                 {"/restconf/data/foo:list=A%20Z", ResourcePath({
                                                        {{"foo"s, "list"}, {"A Z"}},
                                                    })},

                 {"/restconf/data/foo:list=A%25Z", ResourcePath({
                                                        {{"foo"s, "list"}, {"A%Z"}},
                                                    })},
             }) {

            CAPTURE(uriPath);
            nghttp2::asio_http2::uri_ref uri{.path = uriPath};
            rousette::restconf::URIParser parser(uri);
            auto path = parser.getPath();
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
                 R"(/restconf/data/foo:bar/list1=%%)",
                 "/restconf/data/ foo : bar",
                 "/rest conf/data / foo:bar",
                 "/restconf/da ta/foo:bar",
                 "/restconf/data / foo:bar = key1",
                 "/restconf/data / foo:bar =key1",
                 "/restconf/ data",
                 "/restconf /data",
                 "/restconf  data",
             }) {

            CAPTURE(uriPath);
            nghttp2::asio_http2::uri_ref uri{.path = uriPath};
            rousette::restconf::URIParser parser(uri);
            REQUIRE(!parser.getPath());
        }
    }
}
