/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#pragma once
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include <thread>

namespace rousette::restconf {

class Nacm {
public:
    Nacm(sysrepo::Connection conn);
    bool anonymousEnabled() const;
    std::string anonymousUser() const;

private:
    sysrepo::Session m_srSession;
    sysrepo::Subscription m_srSub;
    std::atomic<bool> m_anonymousEnabled;
};

}
