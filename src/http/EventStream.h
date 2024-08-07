/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#pragma once

#include <boost/signals2.hpp>
#include <list>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>

namespace nghttp2::asio_http2::server {
class request;
class response;
}

/** @short HTTP bits */
namespace rousette::http {

/** @short Event delivery via text/event-stream

Recieve data from an EventSignal, and deliver them to an HTTP client via a text/event-stream streamed response.
*/
class EventStream : public std::enable_shared_from_this<EventStream> {
public:
    using EventSignal = boost::signals2::signal<void(const std::string& message)>;
    using Termination = boost::signals2::signal<void()>;

    EventStream(const nghttp2::asio_http2::server::request& req, const nghttp2::asio_http2::server::response& res, Termination& terminate, EventSignal& signal, const std::optional<std::string>& initialEvent = std::nullopt);
    void activate();

private:
    const nghttp2::asio_http2::server::response& res;
    enum State {
        HasEvents,
        WaitingForEvents,
        WantToClose,
        Closed,
    };

    State state = WaitingForEvents;
    std::list<std::string> queue;
    mutable std::mutex mtx; // for `state` and `queue`
    boost::signals2::scoped_connection eventSub, terminateSub;
    const std::string peer;

    size_t send_chunk(uint8_t* destination, std::size_t len, uint32_t* data_flags);
    ssize_t process(uint8_t* destination, std::size_t len, uint32_t* data_flags);
    void enqueue(const std::string& what);
};
}
