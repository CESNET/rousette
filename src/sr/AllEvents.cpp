/*
 * Copyright (C) 2016-2021 CESNET, https://photonics.cesnet.cz/
 *
 * Written by Jan Kundrát <jan.kundrat@cesnet.cz>
 *
*/

#include <memory>
#include <set>
#include <spdlog/spdlog.h>
#include <sysrepo-cpp/Session.hpp>
#include <sysrepo.h>
#include "sr/AllEvents.h"

using namespace std::literals;

namespace {
auto removeOneAttribute(std::shared_ptr<sysrepo::Session> session, struct lyd_attr *attr)
{
    const auto nextAttr = attr->next;
    lyd_free_attr(session->get_context()->swig_ctx(), attr->parent, attr, 0);
    return nextAttr;
}

bool isEmptyOperationAndOrigin(struct lyd_attr *attr)
{
    const auto mod = attr->annotation->module->name;
    if (mod == "sysrepo"sv && attr->name == "operation"sv && attr->value_str == "none"sv) {
        return true;
    }
    if (mod == "ietf-origin"sv && attr->name == "origin"sv && attr->value_str == "ietf-origin:unknown"sv) {
        return true;
    }
    return false;
}
}

namespace rousette::sr {

AllEvents::AllEvents(std::shared_ptr<sysrepo::Session> session, const WithAttributes attrBehavior)
    : sub(std::make_shared<sysrepo::Subscribe>(session))
    , attrBehavior(attrBehavior)
{
    session->session_switch_ds(SR_DS_OPERATIONAL);
    for (const auto& mod : session->get_context()->get_module_iter()) {
        if (mod->name() == "sysrepo"sv) {
            // this one is magic, subscriptions would cause a SR_ERR_INTERNAL
            continue;
        }
        try {
            sub->module_change_subscribe(mod->name(), [this](const auto sess, const auto name, const auto, const auto, const auto) {
                        return onChange(sess, name);
                    }, nullptr, 0, SR_SUBSCR_CTX_REUSE | SR_SUBSCR_DONE_ONLY | SR_SUBSCR_PASSIVE);
            spdlog::debug("Listening for module {}", mod->name());
        } catch (sysrepo::sysrepo_exception& e) {
            if (e.error_code() == SR_ERR_NOT_FOUND) {
                // nothing to listen for, just ignore this
                continue;
            }
            throw;
        }
    }
}

int AllEvents::onChange(std::shared_ptr<sysrepo::Session> session, const std::string& module)
{
    assert(session->session_get_ds() == SR_DS_OPERATIONAL);
    spdlog::trace("change: {}", module);

    // Unfortunately, the C++ API doesn't give us direct access to the sr_change_iter_t struct,
    // and even if it did, there struct itself it not public, it is only described in the private
    // common.h header. This means that we have to go via the get_change_tree_next() API.
    auto changes = session->get_changes_iter(("/" + module + ":*//.").c_str());

    // FIXME: the list of changes is not complete, see https://github.com/sysrepo/sysrepo/issues/2352

    std::set<lyd_node*> seen;
    while (auto x = session->get_change_tree_next(changes)) {
        auto node = x->node();
        while (node->parent()) {
            node = node->parent();
        }
        if (!seen.insert(node->C_lyd_node()).second) {
            // The get_change_tree_next() iterates over changed items, which means that we should print them
            // starting at their individual roots. There could be many changes below a common root, which is
            // why this cache is needed.
            continue;
        }

        auto copy = node->dup(1);
        struct lyd_node *elem = nullptr, *next = nullptr;
        LY_TREE_DFS_BEGIN(copy->C_lyd_node(), next, elem) {
            if (elem->attr) {
                switch (attrBehavior) {
                case WithAttributes::All:
                    break;
                case WithAttributes::RemoveEmptyOperationAndOrigin:
                    {
                        for (auto attr = elem->attr; attr; /* nothing */) {
                            /* spdlog::trace(" XPath {} attr {}:{}: {}", */
                            /*         std::unique_ptr<char, decltype(std::free) *>{lyd_path(elem), std::free}.get(), */
                            /*         attr->annotation->module->name, attr->name, attr->value_str); */
                            if (isEmptyOperationAndOrigin(attr)) {
                                attr = removeOneAttribute(session, attr);
                            } else {
                                attr = attr->next;
                            }
                        }
                    }
                    break;
                case WithAttributes::None:
                    // This is actively misleading; we're stripping out even bits such as "removed".
                    lyd_free_attr(session->get_context()->swig_ctx(), elem, elem->attr, 1);
                    break;
                }
            }
            LY_TREE_DFS_END(copy->C_lyd_node(), next, elem)
        };
        auto json = copy->print_mem(LYD_JSON, 0);
        spdlog::info("JSON: {}", json);
        spdlog::warn("FULL JSON: {}", session->get_data(('/' + module + ":*").c_str())->print_mem(LYD_JSON, 0));
        change(module, json);
    }

    return SR_ERR_OK;
}
}
