/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#include "restconf_utils.h"
#include "sysrepo-cpp/Session.hpp"

using namespace std::string_literals;
namespace ng = nghttp2::asio_http2;
namespace ng_client = ng::client;

namespace {
std::string serverAddressAndPort(const std::string& server_address, const std::string& server_port) {
    return "http://["s + server_address + "]" + ":" + server_port;
}
}

Response::Response(int statusCode, const Response::Headers& headers, const std::string& data)
    : Response(statusCode, transformHeaders(headers), data)
{
}

Response::Response(int statusCode, const ng::header_map& headers, const std::string& data)
    : statusCode(statusCode)
    , headers(headers)
    , data(data)
{
}

bool Response::equalStatusCodeAndHeaders(const Response& o) const
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

bool Response::operator==(const Response& o) const
{
    return equalStatusCodeAndHeaders(o) && data == o.data;
}

ng::header_map Response::transformHeaders(const Response::Headers& headers)
{
    ng::header_map res;
    std::transform(headers.begin(), headers.end(), std::inserter(res, res.end()), [](const auto& h) -> std::pair<std::string, ng::header_value> { return {h.first, {h.second, false}}; });
    return res;
}

Response clientRequest(const std::string& server_address,
                       const std::string& server_port,
                       const std::string& method,
                       const std::string& uri,
                       const std::string& data,
                       const std::map<std::string, std::string>& headers,
                       const boost::posix_time::time_duration timeout)
{
    boost::asio::io_service io_service;
    auto client = std::make_shared<ng_client::session>(io_service, server_address, server_port);

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

        auto req = client->submit(ec, method, serverAddressAndPort(server_address, server_port) + uri, data, reqHeaders);
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

UniqueResource manageNacm(sysrepo::Session session)
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
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='16']/module-name", "ietf-subscribed-notifications");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='16']/action", "permit");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='16']/access-operations", "exec");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='16']/rpc-name", "establish-subscription");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='17']/module-name", "ietf-subscribed-notifications");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='17']/action", "permit");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='17']/access-operations", "read");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='17']/path", "/ietf-subscribed-notifications:filters");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='99']/module-name", "*");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='anon rule']/rule[name='99']/action", "deny");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/group[.='optics']", "");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/module-name", "ietf-system");
    session.setItem("/ietf-netconf-acm:nacm/rule-list[name='dwdm rule']/rule[name='1']/action", "permit"); // overrides nacm:default-deny-* rules in ietf-system model
    session.applyChanges();
}

SSEClient::SSEClient(
    boost::asio::io_service& io,
    const std::string& server_address,
    const std::string& server_port,
    std::binary_semaphore& requestSent,
    const RestconfNotificationWatcher& eventWatcher,
    const std::string& uri,
    const std::map<std::string, std::string>& headers,
    const std::chrono::seconds silenceTimeout,
    const ReportIgnoredLines reportIgnoredLines)
    : client(std::make_shared<ng_client::session>(io, server_address, server_port))
    , t(io, std::chrono::seconds(silenceTimeout))
{
    ng::header_map reqHeaders;
    for (const auto& [name, value] : headers) {
        reqHeaders.insert({name, {value, false}});
    }

    // shutdown the client after a period of no traffic
    t.async_wait([maybeClient = std::weak_ptr<ng_client::session>{client}](const boost::system::error_code& ec) {
        if (ec == boost::asio::error::operation_aborted) {
            return;
        }
        if (auto client = maybeClient.lock()) {
            client->shutdown();
        }
    });

    client->on_connect([&, uri, reqHeaders, silenceTimeout, server_address, server_port, reportIgnoredLines](auto) {
        boost::system::error_code ec;

        auto req = client->submit(ec, "GET", serverAddressAndPort(server_address, server_port) + uri, "", reqHeaders);
        req->on_response([&, silenceTimeout, reportIgnoredLines](const ng_client::response& res) {
            requestSent.release();
            res.on_data([&, silenceTimeout, reportIgnoredLines](const uint8_t* data, std::size_t len) {
                dataBuffer.append(std::string(reinterpret_cast<const char*>(data), len));
                parseEvents(eventWatcher, reportIgnoredLines);
                t.expires_after(std::chrono::seconds(silenceTimeout));
            });
        });

        req->on_close([maybeClient = std::weak_ptr<ng_client::session>{client}](auto) {
            if (auto client = maybeClient.lock()) {
                client->shutdown();
            }
        });
    });

    client->on_error([&](const boost::system::error_code& ec) {
        throw std::runtime_error{"HTTP client error: " + ec.message()};
    });
}

void SSEClient::parseEvents(const RestconfNotificationWatcher& eventWatcher, const ReportIgnoredLines reportIgnoredLines)
{
    static const std::string dataPrefix = "data:";
    static const std::string ignorePrefix = ":";

    std::size_t pos = 0;
    constexpr auto EVENT_SEPARATOR = "\n\n"; // FIXME: Not a production-ready code; does not deal with all possible newline combinations of CR and LF

    while ((pos = dataBuffer.find(EVENT_SEPARATOR)) != std::string::npos) {
        // extract event
        auto rawEvent = dataBuffer.substr(0, pos + std::char_traits<char>::length(EVENT_SEPARATOR));
        std::istringstream stream(rawEvent);
        dataBuffer.erase(0, pos + std::char_traits<char>::length(EVENT_SEPARATOR));

        // split on newlines
        std::string line;
        std::string event;
        while (std::getline(stream, line)) {
            if (line.starts_with(ignorePrefix) && reportIgnoredLines == ReportIgnoredLines::Yes) {
                eventWatcher.commentEvent(line);
            } else if (line.starts_with(ignorePrefix)) {
                continue;
            } else if (line.starts_with(dataPrefix)) {
                event += line.substr(dataPrefix.size());
            } else if (line.empty() && !event.empty()) {
                eventWatcher.dataEvent(event);
                event.clear();
            } else if (line.empty()) {
                continue;
            } else {
                CAPTURE(rawEvent);
                FAIL("Unprefixed response");
            }
        }
    }
}
