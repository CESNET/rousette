/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 *
 */

#include <sysrepo-cpp/Session.hpp>

namespace rousette::restconf {

bool dataExists(sysrepo::Session session, const std::string& path)
{
    if (auto data = session.getData(path)) {
        if (data->findPath(path)) {
            return true;
        }
    }
    return false;
}
}
