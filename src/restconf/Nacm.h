/*
 * Copyright (C) 2023 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#pragma once
#include <sysrepo-cpp/Connection.hpp>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo-cpp/Subscription.hpp>
#include <thread>

namespace rousette::restconf {

/** @brief Class managing NACM in sysrepo. Responsible for any NACM operations and anonymous access authorization.
 *
 * Instantiating this class initializes NACM in sysrepo. Upon deleting, NACM is properly destroyed.
 */
class Nacm {
public:
    Nacm(sysrepo::Connection conn);
    bool authorize(sysrepo::Session session, const std::string& user);

private:
    sysrepo::Session m_srSession;
    sysrepo::Subscription m_srSub;
    std::atomic<bool> m_anonymousEnabled;
};

}
