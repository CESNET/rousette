/*
 * Copyright (C) 2026 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#pragma once

#include <chrono>
#include <libyang-cpp/DataNode.hpp>
#include <optional>
#include <string>
#include <sysrepo-cpp/Enum.hpp>

namespace rousette::restconf {

/** @brief Datastores for which sysrepo maintains the last-modification timestamps */
constexpr bool hasLastModified(sysrepo::Datastore datastore)
{
    return datastore == sysrepo::Datastore::Running || datastore == sysrepo::Datastore::Startup || datastore == sysrepo::Datastore::Candidate || datastore == sysrepo::Datastore::FactoryDefault;
}

std::optional<std::chrono::system_clock::time_point> takeLastModified(libyang::DataNode& tree);
std::string httpDate(const std::chrono::system_clock::time_point& time);
}
