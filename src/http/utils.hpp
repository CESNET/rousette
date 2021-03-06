/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <nghttp2/asio_http2_server.h>

namespace rousette::http {

std::string peer_from_request(const nghttp2::asio_http2::server::request &req);
}
