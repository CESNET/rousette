/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <boost/lexical_cast.hpp>
#include "http/utils.hpp"

namespace rousette::http {

/** @short Reasonably unique, but free-form string for identifying client connections */
std::string peer_from_request(const nghttp2::asio_http2::server::request &req)
{
    std::string peer = boost::lexical_cast<std::string>(req.remote_endpoint());
    if (auto forwarded = req.header().find("forwarded"); forwarded != req.header().end()) {
        peer += '(' + forwarded->second.value + ')';
    }
    return peer;
}
}
