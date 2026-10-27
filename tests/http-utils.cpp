/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <experimental/iterator>
#include "http/utils.hpp"
#include "tests/pretty_printers.h"

using namespace std::string_literals;

namespace doctest {

template <>
struct StringMaker<rousette::http::EntityTag> {
    static String convert(const rousette::http::EntityTag& obj)
    {
        return ("EntityTag{"s + (obj.weak ? "weak" : "strong") + ", " + obj.opaqueTag + "}").c_str();
    }
};

template <>
struct StringMaker<rousette::http::ProtoAndHost> {
    static String convert(const rousette::http::ProtoAndHost& obj)
    {
        return ("ProtoAndHost{"s + obj.proto.value_or("nullopt") + ", " + obj.host.value_or("nullopt") + "}").c_str();
    }
};
}

TEST_CASE("Accept header")
{
    for (const auto& [input, expected] : {
             std::pair<std::string, std::vector<std::string>>{"*/*", {"*/*"}},
             {"text/*", {"text/*"}},
             {"text/plain", {"text/plain"}},
             {"teXt/PLaIn", {"text/plain"}},
             {"application/yang-data+json", {"application/yang-data+json"}},
             {"application/yang-data+xml+json", {}},
             {"application/yang-data+xml;q=0.5", {"application/yang-data+xml"}},
             {"application/yang-data+xml;q=0.52", {"application/yang-data+xml"}},
             {"application/yang-data+xml;q=1", {"application/yang-data+xml"}},
             {"*/*", {"*/*"}},
             {"*/haha", {}},
             {"*/*;q=1", {"*/*"}},
             {"* /*;q=1", {}},
             {"text/html, application/json", {"text/html", "application/json"}},
             {" text/html;q=0.8 , application/json;q=0.5", {"text/html", "application/json"}},
             {"invalidtype", {}},
             {"invalid//type", {}},
             {"invalid+type", {}},
             {"invalid / type", {}},
             {"", {}},
             {"text/*, text/plain, text/plain;format=flowed, */*", {"text/plain", "text/plain", "text/*", "*/*"}},
             {"application/vnd.example.v2+xml", {"application/vnd.example.v2+xml"}},
             {"text/*, application/json", {"application/json", "text/*"}},
             {"text/html; charset=utf-8", {"text/html"}},
             {"text/html; Charset=utf-8", {"text/html"}},
             {"application/json; q=0.8, text/plain; charset=utf-8", {"text/plain", "application/json"}},
             {"application/*json", {}},
             {"text/html; charset=", {}},
             {R"(text/html; charset="utf-8")", {"text/html"}},
             {"text/html; charset=utf-8, application/json; q=0.7", {"text/html", "application/json"}},
             {"text/html; q=0.9, text/plain; q=0.5", {"text/html", "text/plain"}},
             {"application/*; q=0.5, text/*; charset=utf-8", {"text/*", "application/*"}},
             {"application/xml; charset, q=0.7", {}},
             {"*/*;q=0.1, application/yang-data+xml;q=0.5,text/*;q=0.6", {"text/*", "application/yang-data+xml", "*/*"}},
             {"audio/*; q=0.2, audio/basic", {"audio/basic", "audio/*"}},
             {"text/plain; q=0.5, text/html,   text/x-dvi; q=0.8, text/x-c", {"text/html", "text/x-c", "text/x-dvi", "text/plain"}},
             {"audio/*; q=q, audio/basic", {}},
             {"application/xml; q=0.8, text/plain;q=0.9", {"text/plain", "application/xml"}},
             {"application/xml; q=0.8;q=1, text/plain;charset=utf-8;q=0.9", {"text/plain", "application/xml"}},
             {"  application/xml; q=0.8, text/plain   ;   charset=utf-8; q=0.9  ", {"text/plain", "application/xml"}},
             {"text/plain;q=1" + std::string(500, '0'), {}},
             {"text/plain;Q=1", {"text/plain"}},
         }) {
        CAPTURE(input);
        REQUIRE(rousette::http::parseAcceptHeader(input) == expected);
    };
}

TEST_CASE("Get Host from forwarded")
{
    using rousette::http::ProtoAndHost;
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;proto=https;by=203.0.113.43") == ProtoAndHost{"https", std::nullopt});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;proto=http;host=example.net;by=203.0.113.43") == ProtoAndHost{"http"s, "example.net"s});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;proto=https;by=203.0.113.43;host=example.net") == ProtoAndHost{"https"s, "example.net"s});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;by=203.0.113.43;host=192.0.2.1") == ProtoAndHost{std::nullopt, "192.0.2.1"s});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;proto=http;by=203.0.113.43;host=\"::1\"") == ProtoAndHost{"http", "::1"s});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;proto=https;by=203.0.113.43;host=\"::1\", proto=http;host=192.0.2.1;for=192.0.2.61") == ProtoAndHost{"https"s, "::1"s});
    REQUIRE(rousette::http::parseForwardedHeader("for=192.0.2.60;by=203.0.113.43;host=[2001:db8::666]") == ProtoAndHost{std::nullopt, "[2001:db8::666]"s});
    REQUIRE(rousette::http::parseForwardedHeader("host=proto=https") == ProtoAndHost{});
    REQUIRE(rousette::http::parseForwardedHeader("") == ProtoAndHost{});
}

TEST_CASE("HTTP Dates")
{
    using rousette::http::parseHttpDate;
    using namespace std::chrono;

    const auto expected = sys_days{1994y / November / 6} + 8h + 49min + 37s;

    REQUIRE(rousette::http::httpDate(expected) == "Sun, 06 Nov 1994 08:49:37 GMT");

    REQUIRE(parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT") == expected);
    REQUIRE(parseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT") == expected);
    REQUIRE(parseHttpDate("Sun Nov  6 08:49:37 1994") == expected);

    REQUIRE(parseHttpDate("") == std::nullopt);
    REQUIRE(parseHttpDate("garbage") == std::nullopt);
    REQUIRE(parseHttpDate("Sun, 06 Nov 1994 08:49:37") == std::nullopt);
    REQUIRE(parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT trailing") == std::nullopt);
    REQUIRE(parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT, Sun, 06 Nov 1994 08:49:37 GMT") == std::nullopt);
    REQUIRE(parseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT; Sun, 06 Nov 1994 08:49:37 GMT") == std::nullopt);
    REQUIRE(parseHttpDate("Sun, 06 Nov 1994 08:49:60 GMT") == std::nullopt);
    REQUIRE(parseHttpDate("Sun, 06 Nov 1994 08:49") == std::nullopt);
    REQUIRE(parseHttpDate("Sun, 31 Feb 1994 08:49:37 GMT") == std::nullopt);
    REQUIRE(parseHttpDate("Sun Nov  6 08:49:37") == std::nullopt);
    REQUIRE(parseHttpDate("1994-11-06T08:49:37Z") == std::nullopt);
}

TEST_CASE("Entity tags")
{
    using rousette::http::EntityTag;
    using rousette::http::parseEntityTags;
    using Tags = std::vector<EntityTag>;

    REQUIRE(parseEntityTags(R"("xyzzy")") == Tags{{false, R"("xyzzy")"}});
    REQUIRE(parseEntityTags(R"(W/"xyzzy")") == Tags{{true, R"("xyzzy")"}});
    REQUIRE(parseEntityTags(R"("")") == Tags{{false, R"("")"}});
    REQUIRE(parseEntityTags(R"("xyzzy", "r2d2xxxx", "c3piozzzz")") == Tags{{false, R"("xyzzy")"}, {false, R"("r2d2xxxx")"}, {false, R"("c3piozzzz")"}});
    REQUIRE(parseEntityTags(R"("xyzzy",W/"r2d2xxxx")") == Tags{{false, R"("xyzzy")"}, {true, R"("r2d2xxxx")"}});
    REQUIRE(parseEntityTags(R"("a,b", "c")") == Tags{{false, R"("a,b")"}, {false, R"("c")"}});
    REQUIRE(parseEntityTags("\"\x80\xff\"") == Tags{{false, "\"\x80\xff\""}});

    // RFC 9110, sec. 5.6.1.2: empty list elements
    REQUIRE(parseEntityTags(R"(, "a" ,, "b", )") == Tags{{false, R"("a")"}, {false, R"("b")"}});

    REQUIRE(parseEntityTags("") == std::nullopt);
    REQUIRE(parseEntityTags(",") == std::nullopt);
    REQUIRE(parseEntityTags("*") == std::nullopt);
    REQUIRE(parseEntityTags("xyzzy") == std::nullopt);
    REQUIRE(parseEntityTags(R"("xyzzy)") == std::nullopt);
    REQUIRE(parseEntityTags(R"(w/"xyzzy")") == std::nullopt);
    REQUIRE(parseEntityTags(R"(W/ "xyzzy")") == std::nullopt);
    REQUIRE(parseEntityTags(R"("xy zzy")") == std::nullopt);
    REQUIRE(parseEntityTags(R"("xy"zzy")") == std::nullopt);
    REQUIRE(parseEntityTags(R"("a" "b")") == std::nullopt);
    REQUIRE(parseEntityTags(R"("a"; "b")") == std::nullopt);

    // RFC 9110, sec. 8.8.3.2
    REQUIRE(rousette::http::weakMatch({true, R"("1")"}, {true, R"("1")"}));
    REQUIRE(!rousette::http::weakMatch({true, R"("1")"}, {true, R"("2")"}));
    REQUIRE(rousette::http::weakMatch({true, R"("1")"}, {false, R"("1")"}));
    REQUIRE(rousette::http::weakMatch({false, R"("1")"}, {false, R"("1")"}));
}
