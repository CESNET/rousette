/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include "trompeloeil_doctest.h"
#include <iostream>
#include <nghttp2/asio_http2_client.h>
#include <optional>
#include <sstream>
#include <sysrepo-cpp/Session.hpp>
#include "tests/UniqueResource.h"

using namespace std::string_literals;
namespace ng = nghttp2::asio_http2;
namespace ng_client = ng::client;

struct Response {
    int statusCode;
    ng::header_map headers;
    std::string data;

    Response(int statusCode, const std::multimap<std::string, std::string>& headers, const std::string& data)
        : Response(statusCode, transformHeaders(headers), data)
    {
    }

    Response(int statusCode, const ng::header_map& headers, const std::string& data)
        : statusCode(statusCode)
        , headers(headers)
        , data(data)
    {
    }

    bool equalStatusCodeAndHeaders(const Response& o) const
    {
        // Skipping 'date' header. Its value will not be reproducible in simple tests
        ng::header_map myHeaders(headers);
        ng::header_map otherHeaders(o.headers);
        myHeaders.erase("date");
        otherHeaders.erase("date");

        return statusCode == o.statusCode && std::equal(myHeaders.begin(), myHeaders.end(), otherHeaders.begin(), otherHeaders.end(), [](const auto& a, const auto& b) {
                   return a.first == b.first && a.second.value == b.second.value; // Skipping 'sensitive' field from ng::header_value which does not seem important for us.
               });
    }

    bool operator==(const Response& o) const
    {
        return equalStatusCodeAndHeaders(o) && data == o.data;
    }

    static ng::header_map transformHeaders(const std::multimap<std::string, std::string>& headers) {
        ng::header_map res;
        std::transform(headers.begin(), headers.end(), std::inserter(res, res.end()), [](const auto& h) -> std::pair<std::string, ng::header_value> { return {h.first, {h.second, false}}; });
        return res;
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

#define FORWARDED {"forward", "proto=http;host=example.net"}

#define CONTENT_TYPE_JSON {"content-type", "application/yang-data+json"}
#define CONTENT_TYPE_XML {"content-type", "application/yang-data+xml"}

#define YANG_ROOT "/yang"
#define RESTCONF_ROOT "/restconf"
#define RESTCONF_DATA_ROOT RESTCONF_ROOT "/data"
#define RESTCONF_OPER_ROOT RESTCONF_ROOT "/operations"
#define RESTCONF_ROOT_DS(NAME) RESTCONF_ROOT "/ds/ietf-datastores:" NAME

const ng::header_map jsonHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"application/yang-data+json", false}},
};

const ng::header_map xmlHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"application/yang-data+xml", false}},
};

const ng::header_map noContentTypeHeaders{
    {"access-control-allow-origin", {"*", false}},
};

const ng::header_map yangHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"application/yang", false}},
};

const ng::header_map plaintextHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"text/plain", false}},
};

const ng::header_map eventStreamHeaders {
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"text/event-stream", false}},
};

#define ACCESS_CONTROL_ALLOW_ORIGIN {"access-control-allow-origin", "*"}
#define ACCEPT_PATCH {"accept-patch", "application/yang-data+json;charset=utf-8,application/yang-data+xml;charset=utf-8"}

Response clientRequest(auto method,
        auto uri,
        const std::string& data,
        const std::map<std::string, std::string>& headers,
        // this is a test, and the server is expected to reply "soon"
        const boost::posix_time::time_duration timeout=boost::posix_time::seconds(3))
{
    boost::asio::io_service io_service;
    auto client = std::make_shared<ng_client::session>(io_service, SERVER_ADDRESS, SERVER_PORT);

    client->read_timeout(timeout);

    std::ostringstream oss;
    ng::header_map resHeaders;
    int statusCode;

    client->on_connect([&](auto) {
        boost::system::error_code ec;

        ng::header_map reqHeaders;
        for (const auto& [name, value] : headers) {
            reqHeaders.insert({name, {value, false}});
        }

        auto req = client->submit(ec, method, SERVER_ADDRESS_AND_PORT + uri, data, reqHeaders);
        req->on_response([&](const ng_client::response& res) {
            res.on_data([&oss](const uint8_t* data, std::size_t len) {
                oss.write(reinterpret_cast<const char*>(data), len);
            });
            statusCode = res.status_code();
            resHeaders = res.header();
        });
        req->on_close([maybeClient = std::weak_ptr<ng_client::session>{client}](auto) {
            if (auto client = maybeClient.lock()) {
                client->shutdown();
            }
        });
    });
    client->on_error([](const boost::system::error_code& ec) {
        throw std::runtime_error{"HTTP client error: " + ec.message()};
    });
    io_service.run();

    return {statusCode, resHeaders, oss.str()};
}

Response get(auto uri, const std::map<std::string, std::string>& headers)
{
    return clientRequest("GET", uri, "", headers);
}

Response options(auto uri, const std::map<std::string, std::string>& headers)
{
    return clientRequest("OPTIONS", uri, "", headers);
}

Response head(auto uri, const std::map<std::string, std::string>& headers)
{
    return clientRequest("HEAD", uri, "", headers);
}

Response put(auto xpath, const std::string& data, const std::map<std::string, std::string>& headers)
{
    return clientRequest("PUT", xpath, data, headers);
}

Response post(auto xpath, const std::string& data, const std::map<std::string, std::string>& headers)
{
    return clientRequest("POST", xpath, data, headers);
}

Response patch(auto uri, const std::string& data, const std::map<std::string, std::string>& headers)
{
    return clientRequest("PATCH", uri, data, headers);
}

Response httpDelete(auto uri, const std::map<std::string, std::string>& headers)
{
    return clientRequest("DELETE", uri, "", headers);
}

auto manageNacm(sysrepo::Session session)
{
    return make_unique_resource(
            [session]() mutable {
                session.switchDatastore(sysrepo::Datastore::Running);
                session.copyConfig(sysrepo::Datastore::Startup, "ietf-netconf-acm");
            },
            [session]() mutable {
                session.switchDatastore(sysrepo::Datastore::Running);

                /* cleanup running DS of ietf-netconf-acm module
                   because it contains XPaths to other modules that we
                   can't uninstall because the running DS content would be invalid
                 */
                session.copyConfig(sysrepo::Datastore::Startup, "ietf-netconf-acm");
            });
}

void setupRealNacm(sysrepo::Session session)
{
    session.switchDatastore(sysrepo::Datastore::Running);
    session.setItem("/ietf-netconf-acm:nacm/enable-external-groups", "false");
    session.setItem("/ietf-netconf-acm:nacm/groups/group[name='optics']/user-name[.='dwdm']", "");
    session.setItem("/ietf-netconf-acm:nacm/groups/group[name='yangnobody']/user-name[.='yangnobody']", "");
    session.setItem("/ietf-netconf-acm:nacm/groups/group[name='norules']/user-name[.='norules']", "");

    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/group[.='yangnobody']", "");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='10']/module-name", "ietf-system");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='10']/action", "permit");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='10']/access-operations", "read");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='10']/path", "/ietf-system:system/contact");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='11']/module-name", "ietf-system");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='11']/action", "permit");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='11']/access-operations", "read");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='11']/path", "/ietf-system:system/hostname");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='12']/module-name", "ietf-system");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='12']/action", "permit");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='12']/access-operations", "read");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='12']/path", "/ietf-system:system/location");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='13']/module-name", "example");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='13']/action", "permit");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='13']/access-operations", "read");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='14']/module-name", "ietf-restconf-monitoring");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='14']/action", "permit");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='14']/access-operations", "read");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='15']/module-name", "example-delete");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='15']/action", "permit");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='15']/access-operations", "read");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='15']/path", "/example-delete:immutable");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='99']/module-name", "*");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='99']/action", "deny");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/group[.='optics']", "");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/module-name", "ietf-system");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/action", "permit"); // overrides nacm:default-deny-* rules in ietf-system model
    session.applyChanges();
}
