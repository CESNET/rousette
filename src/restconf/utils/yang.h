/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <chrono>
#include <libyang-cpp/DataNode.hpp>
#include <optional>
#include <sysrepo-cpp/Subscription.hpp>

namespace libyang {
class Leaf;
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

/** @brief Parses the YANG date-and-time leaf at @p path, if present. */
std::optional<sysrepo::NotificationTimeStamp> optionalTime(const libyang::DataNode& node, const std::string& path);

/** @brief Reads interval from the YANG node and converts it to std::milliseconds.
 *
 *  @tparam SourceRatio Ratio of the interval from the YANG node (e.g. centiseconds, seconds, ...)
 * */
template <class SourceRatio>
std::optional<std::chrono::milliseconds> createInterval(const libyang::DataNode& node, const std::string& path)
{
    if (auto intervalNode = node.findPath(path)) {
        auto value = std::get<uint32_t>(intervalNode->asTerm().value());
        std::chrono::duration<std::chrono::milliseconds::rep, SourceRatio> duration(value);
        return std::chrono::duration_cast<std::chrono::milliseconds>(duration);
    }

    return std::nullopt;
}
}
