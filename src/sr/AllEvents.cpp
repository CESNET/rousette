/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <memory>
#include <set>
#include <spdlog/spdlog.h>
#include <libyang-cpp/Utils.hpp>
#include <sysrepo-cpp/utils/exception.hpp>
#include <sysrepo.h>
#include "sr/AllEvents.h"

namespace {
bool isEmptyOperationAndOrigin(const libyang::Meta& meta)
{
    const auto mod = meta.module().name();
    if (mod == "sysrepo" && meta.name() == "operation" && meta.valueStr() == "none") {
        return true;
    }
    if (mod == "ietf-origin" && meta.name() == "origin" && meta.valueStr() == "ietf-origin:unknown") {
        return true;
    }
    return false;
}
}

namespace rousette::sr {

AllEvents::AllEvents(sysrepo::Session session, const WithAttributes attrBehavior)
    : sub(std::nullopt)
    , attrBehavior(attrBehavior)
{
    session.switchDatastore(sysrepo::Datastore::Operational);
    for (const auto& mod : session.getContext().modules()) {
        if (mod.name() == "sysrepo") {
            // this one is magic, subscriptions would cause a SR_ERR_INTERNAL
            continue;
        }
        try {
            sysrepo::ModuleChangeCb cb = [this](const auto sess, auto, auto name, auto, auto, auto) {
                return onChange(sess, std::string{name});
            };
            sub = session.onModuleChange(std::string{mod.name()}, cb, std::nullopt, 0, sysrepo::SubscribeOptions::DoneOnly | sysrepo::SubscribeOptions::Passive);
            spdlog::debug("Listening for module {}", mod.name());
        } catch (sysrepo::ErrorWithCode& e) {
            if (e.code() == sysrepo::ErrorCode::NotFound) {
                // nothing to listen for, just ignore this
                continue;
            }
            throw;
        }
    }
}

sysrepo::ErrorCode AllEvents::onChange(sysrepo::Session session, const std::string& module)
{
    assert(session.activeDatastore() == sysrepo::Datastore::Operational);
    spdlog::trace("change: {}", module);

    auto changes = session.getChanges("/" + module + ":*//.");

    // FIXME: the list of changes is not complete, see https://github.com/sysrepo/sysrepo/issues/2352

    std::set<libyang::DataNode, libyang::PointerCompare> seen;
    for (const auto& srChange : changes) {
        auto node = srChange.node;
        while (node.parent()) {
            node = *node.parent();
        }
        if (!seen.insert(node).second) {
            // The get_change_tree_next() iterates over changed items, which means that we should print them
            // starting at their individual roots. There could be many changes below a common root, which is
            // why this cache is needed.
            continue;
        }

        auto copy = node.duplicate(libyang::DuplicationOptions::Recursive);
        for (const auto& elem : copy.childrenDfs()) {
            auto meta = elem.meta();
            if (meta.empty()) {
                continue;
            }

            switch (attrBehavior) {
            case WithAttributes::All:
                break;
            case WithAttributes::RemoveEmptyOperationAndOrigin:
                {
                    for (auto attr = meta.begin(); attr != meta.end(); /* nothing */) {
                        spdlog::trace(" XPath {} attr {}:{}: {}",
                                std::string{elem.path()},
                                attr->module().name(), attr->name(), attr->valueStr());
                        if (isEmptyOperationAndOrigin(*attr)) {
                            attr = meta.erase(attr);
                        } else {
                            attr = attr++;
                        }
                    }
                }
                break;
            case WithAttributes::None:
                // This is actively misleading; we're stripping out even bits such as "removed".
                auto it = meta.begin();
                while (it != meta.end()) {
                    it = meta.erase(it);
                }
                break;
            }
        };
        auto json = *copy.printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings);
        spdlog::info("JSON: {}", json);
        spdlog::warn("FULL JSON: {}",
                *session.getData('/' + module + ":*")->printStr(libyang::DataFormat::JSON, libyang::PrintFlags::WithSiblings));
        change(module, json);
    }

    return sysrepo::ErrorCode::Ok;
}
}
