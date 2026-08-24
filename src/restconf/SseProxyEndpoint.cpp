/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "restconf/SseProxyEndpoint.h"

namespace rousette::restconf {

SseProxyEndpoint::Feed::Feed(SseProxyEndpoint::SourceSubscription source,
                             boost::asio::io_context& io,
                             std::shared_ptr<http::EventStream::EventSignal> events,
                             std::mutex& processEventMutex)
    : subscription(std::move(source.subscription))
    , broadcaster(io, subscription, source.encoding, std::move(events), processEventMutex)
{
}

SseProxyEndpoint::SseProxyEndpoint(std::vector<SourceSubscription> sources, boost::asio::io_context& io)
    : m_events(std::make_shared<http::EventStream::EventSignal>())
{
    // Each feed's broadcaster drains from construction and forwards notifications into the shared signal.
    // The signal has no receivers yet, which is intentional: this is a live stream, so notifications arriving
    // before any client connects are dropped rather than buffered.
    for (auto& source : sources) {
        m_feeds.emplace_back(std::move(source), io, events(), m_processEventMutex);
    }
}

SseProxyEndpoint::~SseProxyEndpoint()
{
    // Close the attached clients before the feeds (and with them m_events) go away; see termination().
    m_termination();
}
}
