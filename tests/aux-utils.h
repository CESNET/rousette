/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
 */

#pragma once
#include "trompeloeil_doctest.h"
#include <latch>
#include <nghttp2/asio_http2_client.h>
#include "restconf_utils.h"

namespace sysrepo {
class Session;
}

namespace ng = nghttp2::asio_http2;

static const auto SERVER_ADDRESS = "::1";

#define AUTH_DWDM {"authorization", "Basic ZHdkbTpEV0RN"}
#define AUTH_NORULES {"authorization", "Basic bm9ydWxlczplbXB0eQ=="}
#define AUTH_ROOT {"authorization", "Basic cm9vdDpzZWtyaXQ="}
#define AUTH_WRONG_PASSWORD {"authorization", "Basic ZHdkbTpGQUlM"}

#define FORWARDED {"forward", "proto=http;host=example.net"}

#define CONTENT_TYPE_JSON {"content-type", "application/yang-data+json"}
#define CONTENT_TYPE_XML {"content-type", "application/yang-data+xml"}

#define CONTENT_TYPE_YANG_PATCH_JSON {"content-type", "application/yang-patch+json"}
#define CONTENT_TYPE_YANG_PATCH_XML {"content-type", "application/yang-patch+xml"}

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

const ng::header_map eventStreamHeaders{
    {"access-control-allow-origin", {"*", false}},
    {"content-type", {"text/event-stream", false}},
};

#define ACCESS_CONTROL_ALLOW_ORIGIN {"access-control-allow-origin", "*"}
#define ACCEPT_PATCH {"accept-patch", "application/yang-data+json, application/yang-data+xml, application/yang-patch+xml, application/yang-patch+json"}

Response get(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "GET", uri, "", headers, timeout);
}

Response options(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "OPTIONS", uri, "", headers, timeout);
}

Response head(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "HEAD", uri, "", headers, timeout);
}

Response put(auto xpath, const std::map<std::string, std::string>& headers, const std::string& data, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "PUT", xpath, data, headers, timeout);
}

Response post(auto xpath, const std::map<std::string, std::string>& headers, const std::string& data, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "POST", xpath, data, headers, timeout);
}

Response patch(auto uri, const std::map<std::string, std::string>& headers, const std::string& data, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "PATCH", uri, data, headers, timeout);
}

Response httpDelete(auto uri, const std::map<std::string, std::string>& headers, const boost::posix_time::time_duration timeout = CLIENT_TIMEOUT)
{
    return clientRequest(SERVER_ADDRESS, SERVER_PORT, "DELETE", uri, "", headers, timeout);
}
