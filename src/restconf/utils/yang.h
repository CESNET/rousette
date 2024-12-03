/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <chrono>
#include <sysrepo-cpp/Subscription.hpp>

namespace libyang {
class Leaf;
class DataNode;
class Context;
enum class DataFormat;
}

namespace rousette::restconf {

std::string escapeListKey(const std::string& str);
std::string listKeyPredicate(const std::vector<libyang::Leaf>& listKeyLeafs, const std::vector<std::string>& keyValues);
std::string leaflistKeyPredicate(const std::string& keyValue);
bool isUserOrderedList(const libyang::DataNode& node);
bool isKeyNode(const libyang::DataNode& maybeList, const libyang::DataNode& node);
std::string as_restconf_notification(const libyang::Context& ctx, libyang::DataFormat dataFormat, libyang::DataNode notification, const sysrepo::NotificationTimeStamp& time);
}
