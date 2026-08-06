/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include "restconf/SseProxyEndpoint.h"

namespace rousette::restconf {

SseProxyEndpoint::Feed::Feed(SseProxyEndpoint::SourceSubscription source, boost::asio::io_context& io, std::shared_ptr<http::EventStream::EventSignal> events)
    : subscription(std::move(source.subscription))
    , broadcaster(io, subscription, source.encoding, std::move(events), nullptr)
{
}

SseProxyEndpoint::SseProxyEndpoint(std::vector<SourceSubscription> sources, boost::asio::io_context& io)
    : m_events(std::make_shared<http::EventStream::EventSignal>())
{
    for (auto& source : sources) {
        m_feeds.emplace_back(std::move(source), io, events());
    }
}
}
