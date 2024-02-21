/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#pragma once
#include <boost/fusion/adapted/struct/adapt_struct.hpp>
#include <boost/optional.hpp>
#include <optional>
#include <string>
#include <sysrepo-cpp/Enum.hpp>

namespace libyang {
class Context;
class Module;
}

namespace rousette::restconf {
namespace impl {

struct YangModule {
    std::string name;
    boost::optional<std::string> revision;

    bool operator==(const YangModule&) const = default;
};

std::optional<YangModule> parseModuleWithRevision(const std::string& uriPath);
}

std::optional<libyang::Module> asYangModule(const libyang::Context& ctx, const std::string& uriPath);
}

BOOST_FUSION_ADAPT_STRUCT(rousette::restconf::impl::YangModule, name, revision);
