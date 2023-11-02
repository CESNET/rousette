/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <iostream>
#include <nghttp2/asio_http2_client.h>
#include <optional>
#include <sstream>

using namespace std::string_literals;
namespace ng = nghttp2::asio_http2;
namespace ng_client = ng::client;

struct Response {
    int statusCode;
    ng::header_map headers;
    std::string data;

    bool operator==(const Response& o) const
    {
        // Skipping 'date' header. Its value will not be reproducible in simple tests
        ng::header_map myHeaders(headers);
        ng::header_map otherHeaders(o.headers);
        myHeaders.erase("date");
        otherHeaders.erase("date");

        return statusCode == o.statusCode && data == o.data && std::equal(myHeaders.begin(), myHeaders.end(), otherHeaders.begin(), otherHeaders.end(), [](const auto& a, const auto& b) {
                   return a.first == b.first && a.second.value == b.second.value; // Skipping 'sensitive' field from ng::header_value which does not seem important for us.
               });
    }
};

namespace doctest {

template <>
struct StringMaker<ng::header_map> {
    static String convert(const ng::header_map& m)
    {
        std::ostringstream oss;
        oss << "{\n";
        for (const auto& [k, v] : m) {
            oss << "\t"
                << "{\"" << k << "\", "
                << "{\"" << v.value << "\", " << std::boolalpha << v.sensitive << "}},\n";
        }
        oss << "}";
        return oss.str().c_str();
    }
};

template <>
struct StringMaker<Response> {
    static String convert(const Response& o)
    {
        std::ostringstream oss;

        oss << "{"
            << std::to_string(o.statusCode) << ", "
            << StringMaker<decltype(o.headers)>::convert(o.headers) << ",\n"
            << "\"" << o.data << "\",\n"
            << "}";

        return oss.str().c_str();
    }
};
}

static const auto SERVER_ADDRESS = "::1";
static const auto SERVER_ADDRESS_AND_PORT = "http://["s + SERVER_ADDRESS + "]" + ":" + SERVER_PORT;

#define AUTH_DWDM {"authorization", "Basic ZHdkbTpEV0RN"}
#define AUTH_NORULES {"authorization", "Basic bm9ydWxlczplbXB0eQ=="}
#define AUTH_ROOT {"authorization", "Basic cm9vdDpzZWtyaXQ="}

const ng::header_map jsonHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"application/yang-data+json", false}},
};

const ng::header_map xmlHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"application/yang-data+xml", false}},
};

Response clientRequest(auto method, auto xpath, const std::map<std::string, std::string>& headers)
{
    boost::asio::io_service io_service;
    ng_client::session client(io_service, SERVER_ADDRESS, SERVER_PORT);

    // this is a test, and the server is expected to reply "soon"
    client.read_timeout(boost::posix_time::seconds(3));

    std::ostringstream oss;
    ng::header_map resHeaders;
    int statusCode;
    std::optional<std::string> clientError;

    client.on_connect([&](auto) {
        boost::system::error_code ec;

        ng::header_map reqHeaders;
        for (const auto& [name, value] : headers) {
            reqHeaders.insert({name, {value, false}});
        }

        auto req = client.submit(ec, method, SERVER_ADDRESS_AND_PORT + "/restconf/data"s + xpath, reqHeaders);
        req->on_response([&](const ng_client::response& res) {
            res.on_data([&oss](const uint8_t* data, std::size_t len) {
                oss.write(reinterpret_cast<const char*>(data), len);
            });
            statusCode = res.status_code();
            resHeaders = res.header();
        });
        req->on_close([&client](auto) {
            client.shutdown();
        });
    });
    client.on_error([&clientError](const boost::system::error_code& ec) {
        clientError = ec.message();
    });
    io_service.run();

    if (clientError) {
        FAIL("HTTP client error: ", *clientError);
    }

    return {statusCode, resHeaders, oss.str()};
}

Response get(auto xpath, const std::map<std::string, std::string>& headers)
{
    return clientRequest("GET", xpath, headers);
}
