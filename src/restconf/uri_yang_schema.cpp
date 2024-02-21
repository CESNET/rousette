/*
 * Copyright (C) 2024 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Tomáš Pecka <tomas.pecka@cesnet.cz>
 */

#include <boost/spirit/home/x3.hpp>
#include <libyang-cpp/Context.hpp>
#include <libyang-cpp/Module.hpp>
#include "uri_yang_schema.h"

using namespace std::string_literals;

namespace rousette::restconf {

namespace {

namespace x3 = boost::spirit::x3;

// clang-format off

const auto moduleName = x3::rule<class apiIdentifier, std::string>{"moduleName"} = (x3::alpha | x3::char_('_')) >> *(x3::alnum | x3::char_('_') | x3::char_('-') | x3::char_('.'));
const auto revision = x3::rule<class revision, std::string>{"revision"} = x3::repeat(4, x3::inf)[x3::digit] >> x3::char_("-") >> x3::repeat(2)[x3::digit] >> x3::char_("-") >> x3::repeat(2)[x3::digit];
const auto uriGrammar = x3::rule<class grammar, impl::YangModule>{"uriGrammar"} = x3::lit("/") >> x3::lit("yang") >> "/" >> moduleName >> -(x3::lit("@") >> revision);

// clang-format on
}

namespace impl {
std::optional<impl::YangModule> parseModuleWithRevision(const std::string& uriPath)
{
    impl::YangModule parsed;
    auto iter = std::begin(uriPath);
    auto end = std::end(uriPath);

    if (!x3::parse(iter, end, uriGrammar >> x3::eoi, parsed)) {
        return std::nullopt;
    }

    return parsed;
}
}

std::optional<libyang::Module> asYangModule(const libyang::Context& ctx, const std::string& uriPath)
{
    if (auto parsedModule = impl::parseModuleWithRevision(uriPath)) {
        // Converting between boost::optional and std::optional is not trivial
        if (parsedModule->revision) {
            return ctx.getModule(parsedModule->name, *parsedModule->revision);
        } else {
            return ctx.getModule(parsedModule->name, std::nullopt);
        }
    }
    return std::nullopt;
}
}
