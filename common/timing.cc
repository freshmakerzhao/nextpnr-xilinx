/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  David Shah <david@symbioticeda.com>
 *  Copyright (C) 2018  Eddie Hung <eddieh@ece.ubc.ca>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "timing.h"
#include <algorithm>
#include <boost/range/adaptor/reversed.hpp>
#include <deque>
#include <map>
#include <unordered_map>
#include <utility>
#include "log.h"
#include "util.h"

#include "json11.hpp"
#include <fstream>

namespace std {

template <> struct hash<NEXTPNR_NAMESPACE_PREFIX ClockEvent>
{
    std::size_t operator()(const NEXTPNR_NAMESPACE_PREFIX ClockEvent &obj) const noexcept
    {
        std::size_t seed = 0;
        boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX IdString>()(obj.clock));
        boost::hash_combine(seed, hash<int>()(int(obj.edge)));
        return seed;
    }
};

template <> struct hash<NEXTPNR_NAMESPACE_PREFIX ClockPair>
{
    std::size_t operator()(const NEXTPNR_NAMESPACE_PREFIX ClockPair &obj) const noexcept
    {
        std::size_t seed = 0;
        boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX ClockEvent>()(obj.start));
        boost::hash_combine(seed, hash<NEXTPNR_NAMESPACE_PREFIX ClockEvent>()(obj.start));
        return seed;
    }
};

} // namespace std
NEXTPNR_NAMESPACE_BEGIN

typedef std::vector<const PortRef *> PortRefVector;
typedef std::map<int, unsigned> DelayFrequency;


typedef std::unordered_map<ClockPair, CriticalPath> CriticalPathMap;
// typedef std::unordered_map<IdString, NetCriticalityInfo> NetCriticalityMap;

/*
struct Timing
{
    Context *ctx;
    bool net_delays;
    bool update;
    delay_t min_slack;
    CriticalPathMap *crit_path;
    DelayFrequency *slack_histogram;
    NetCriticalityMap *net_crit;
    IdString async_clock;

    struct TimingData
    {
        TimingData() : max_arrival(), max_path_length(), min_remaining_budget() {}
        TimingData(delay_t max_arrival) : max_arrival(max_arrival), max_path_length(), min_remaining_budget() {}
        delay_t max_arrival;
        unsigned max_path_length = 0;
        delay_t min_remaining_budget;
        bool false_startpoint = false;
        std::vector<delay_t> min_required;
        std::unordered_map<ClockEvent, delay_t> arrival_time;
    };

    Timing(Context *ctx, bool net_delays, bool update, CriticalPathMap *crit_path = nullptr,
           DelayFrequency *slack_histogram = nullptr, NetCriticalityMap *net_crit = nullptr)
            : ctx(ctx), net_delays(net_delays), update(update), min_slack(1.0e12 / ctx->setting<float>("target_freq")),
              crit_path(crit_path), slack_histogram(slack_histogram), net_crit(net_crit),
              async_clock(ctx->id("$async$"))
    {
    }

    delay_t walk_paths()
    {
        const auto clk_period = ctx->getDelayFromNS(1.0e9 / ctx->setting<float>("target_freq")).maxDelay();

        // First, compute the topographical order of nets to walk through the circuit, assuming it is a _acyclic_ graph
        // TODO(eddieh): Handle the case where it is cyclic, e.g. combinatorial loops
        std::vector<NetInfo *> topographical_order;
        std::unordered_map<const NetInfo *, std::unordered_map<ClockEvent, TimingData>> net_data;
        // In lieu of deleting edges from the graph, simply count the number of fanins to each output port
        std::unordered_map<const PortInfo *, unsigned> port_fanin;

        std::vector<IdString> input_ports;
        std::vector<const PortInfo *> output_ports;
        for (auto &cell : ctx->cells) {
            input_ports.clear();
            output_ports.clear();
            for (auto &port : cell.second->ports) {
                if (!port.second.net)
                    continue;
                if (port.second.type == PORT_OUT)
                    output_ports.push_back(&port.second);
                else
                    input_ports.push_back(port.first);
            }

            for (auto o : output_ports) {
                int clocks = 0;
                TimingPortClass portClass = ctx->getPortTimingClass(cell.second.get(), o->name, clocks);
                // If output port is influenced by a clock (e.g. FF output) then add it to the ordering as a timing
                // start-point
                if (portClass == TMG_REGISTER_OUTPUT) {
                    topographical_order.emplace_back(o->net);
                    for (int i = 0; i < clocks; i++) {
                        TimingClockingInfo clkInfo = ctx->getPortClockingInfo(cell.second.get(), o->name, i);
                        const NetInfo *clknet = get_net_or_empty(cell.second.get(), clkInfo.clock_port);
                        IdString clksig = clknet ? clknet->name : async_clock;
                        net_data[o->net][ClockEvent{clksig, clknet ? clkInfo.edge : RISING_EDGE}] =
                                TimingData{clkInfo.clockToQ.maxDelay()};
                    }

                } else {
                    if (portClass == TMG_STARTPOINT || portClass == TMG_GEN_CLOCK || portClass == TMG_IGNORE) {
                        topographical_order.emplace_back(o->net);
                        TimingData td;
                        td.false_startpoint = (portClass == TMG_GEN_CLOCK || portClass == TMG_IGNORE);
                        td.max_arrival = 0;
                        net_data[o->net][ClockEvent{async_clock, RISING_EDGE}] = td;
                    }

                    // Don't analyse paths from a clock input to other pins - they will be considered by the
                    // special-case handling register input/output class ports
                    if (portClass == TMG_CLOCK_INPUT)
                        continue;

                    // Otherwise, for all driven input ports on this cell, if a timing arc exists between the input and
                    // the current output port, increment fanin counter
                    for (auto i : input_ports) {
                        DelayInfo comb_delay;
                        bool is_path = ctx->getCellDelay(cell.second.get(), i, o->name, comb_delay);
                        if (is_path)
                            port_fanin[o]++;
                    }
                    // If there is no fanin, add the port as a false startpoint
                    if (!port_fanin.count(o) && !net_data.count(o->net)) {
                        topographical_order.emplace_back(o->net);
                        TimingData td;
                        td.false_startpoint = true;
                        td.max_arrival = 0;
                        net_data[o->net][ClockEvent{async_clock, RISING_EDGE}] = td;
                    }
                }
            }
        }

        // In out-of-context mode, handle top-level ports correctly
        if (bool_or_default(ctx->settings, ctx->id("arch.ooc"))) {
            for (auto &p : ctx->ports) {
                if (p.second.type != PORT_IN || p.second.net == nullptr)
                    continue;
                topographical_order.emplace_back(p.second.net);
            }
        }

        std::deque<NetInfo *> queue(topographical_order.begin(), topographical_order.end());
        // Now walk the design, from the start points identified previously, building up a topographical order
        while (!queue.empty()) {
            const auto net = queue.front();
            queue.pop_front();

            for (auto &usr : net->users) {
                int user_clocks;
                TimingPortClass usrClass = ctx->getPortTimingClass(usr.cell, usr.port, user_clocks);
                if (usrClass == TMG_IGNORE || usrClass == TMG_CLOCK_INPUT)
                    continue;
                for (auto &port : usr.cell->ports) {
                    if (port.second.type != PORT_OUT || !port.second.net)
                        continue;
                    int port_clocks;
                    TimingPortClass portClass = ctx->getPortTimingClass(usr.cell, port.first, port_clocks);

                    // Skip if this is a clocked output (but allow non-clocked ones)
                    if (portClass == TMG_REGISTER_OUTPUT || portClass == TMG_STARTPOINT || portClass == TMG_IGNORE ||
                        portClass == TMG_GEN_CLOCK)
                        continue;
                    DelayInfo comb_delay;
                    bool is_path = ctx->getCellDelay(usr.cell, usr.port, port.first, comb_delay);
                    if (!is_path)
                        continue;
                    // Decrement the fanin count, and only add to topographical order if all its fanins have already
                    // been visited
                    auto it = port_fanin.find(&port.second);
                    if (it == port_fanin.end()) {
                        log_error("Internal timing error (negative fanin count) for %s.%s\n", ctx->nameOf(usr.cell),
                                  ctx->nameOf(port.first));
                    }
                    if (--it->second == 0) {
                        topographical_order.emplace_back(port.second.net);
                        queue.emplace_back(port.second.net);
                        port_fanin.erase(it);
                    }
                }
            }
        }

        // Sanity check to ensure that all ports where fanins were recorded were indeed visited
        if (!port_fanin.empty() && !bool_or_default(ctx->settings, ctx->id("timing/ignoreLoops"), false)) {
            for (auto fanin : port_fanin) {
                NetInfo *net = fanin.first->net;
                if (net != nullptr) {
                    log_info("   remaining fanin includes %s (net %s)\n", fanin.first->name.c_str(ctx),
                             net->name.c_str(ctx));
                    if (net->driver.cell != nullptr)
                        log_info("        driver = %s.%s\n", net->driver.cell->name.c_str(ctx),
                                 net->driver.port.c_str(ctx));
                    for (auto net_user : net->users)
                        log_info("        user: %s.%s\n", net_user.cell->name.c_str(ctx), net_user.port.c_str(ctx));
                } else {
                    log_info("   remaining fanin includes %s (no net)\n", fanin.first->name.c_str(ctx));
                }
            }
            if (ctx->force)
                log_warning("timing analysis failed due to presence of combinatorial loops, incomplete specification "
                            "of timing ports, etc.\n");
            else
                log_error("timing analysis failed due to presence of combinatorial loops, incomplete specification of "
                          "timing ports, etc.\n");
        }

        // Go forwards topographically to find the maximum arrival time and max path length for each net
        for (auto net : topographical_order) {
            if (!net_data.count(net))
                continue;
            auto &nd_map = net_data.at(net);
            for (auto &startdomain : nd_map) {
                ClockEvent start_clk = startdomain.first;
                auto &nd = startdomain.second;
                if (nd.false_startpoint)
                    continue;
                const auto net_arrival = nd.max_arrival;
                const auto net_length_plus_one = nd.max_path_length + 1;
                nd.min_remaining_budget = clk_period;
                for (auto &usr : net->users) {
                    int port_clocks;
                    TimingPortClass portClass = ctx->getPortTimingClass(usr.cell, usr.port, port_clocks);
                    auto net_delay = net_delays ? ctx->getNetinfoRouteDelay(net, usr) : delay_t();
                    auto usr_arrival = net_arrival + net_delay;

                    if (portClass == TMG_ENDPOINT || portClass == TMG_IGNORE || portClass == TMG_CLOCK_INPUT) {
                        // Skip
                    } else {
                        auto budget_override = ctx->getBudgetOverride(net, usr, net_delay);
                        // Iterate over all output ports on the same cell as the sink
                        for (auto port : usr.cell->ports) {
                            if (port.second.type != PORT_OUT || !port.second.net)
                                continue;
                            DelayInfo comb_delay;
                            // Look up delay through this path
                            bool is_path = ctx->getCellDelay(usr.cell, usr.port, port.first, comb_delay);
                            if (!is_path)
                                continue;
                            auto &data = net_data[port.second.net][start_clk];
                            auto &arrival = data.max_arrival;
                            arrival = std::max(arrival, usr_arrival + comb_delay.maxDelay());
                            if (!budget_override) { // Do not increment path length if budget overriden since it doesn't
                                // require a share of the slack
                                auto &path_length = data.max_path_length;
                                path_length = std::max(path_length, net_length_plus_one);
                            }
                        }
                    }
                }
            }
        }

        std::unordered_map<ClockPair, std::pair<delay_t, NetInfo *>> crit_nets;

        // Now go backwards topographically to determine the minimum path slack, and to distribute all path slack evenly
        // between all nets on the path
        for (auto net : boost::adaptors::reverse(topographical_order)) {
            if (!net_data.count(net))
                continue;
            auto &nd_map = net_data.at(net);
            for (auto &startdomain : nd_map) {
                auto &nd = startdomain.second;
                // Ignore false startpoints
                if (nd.false_startpoint)
                    continue;
                const delay_t net_length_plus_one = nd.max_path_length + 1;
                auto &net_min_remaining_budget = nd.min_remaining_budget;
                for (auto &usr : net->users) {
                    auto net_delay = net_delays ? ctx->getNetinfoRouteDelay(net, usr) : delay_t();
                    auto budget_override = ctx->getBudgetOverride(net, usr, net_delay);
                    int port_clocks;
                    TimingPortClass portClass = ctx->getPortTimingClass(usr.cell, usr.port, port_clocks);
                    if (portClass == TMG_REGISTER_INPUT || portClass == TMG_ENDPOINT) {
                        auto process_endpoint = [&](IdString clksig, ClockEdge edge, delay_t setup) {
                            const auto net_arrival = nd.max_arrival;
                            const auto endpoint_arrival = net_arrival + net_delay + setup;
                            delay_t period;
                            // Set default period
                            if (edge == startdomain.first.edge) {
                                period = clk_period;
                            } else {
                                period = clk_period / 2;
                            }
                            if (clksig != async_clock) {
                                if (ctx->nets.at(clksig)->clkconstr) {
                                    if (edge == startdomain.first.edge) {
                                        // same edge
                                        period = ctx->nets.at(clksig)->clkconstr->period.minDelay();
                                    } else if (edge == RISING_EDGE) {
                                        // falling -> rising
                                        period = ctx->nets.at(clksig)->clkconstr->low.minDelay();
                                    } else if (edge == FALLING_EDGE) {
                                        // rising -> falling
                                        period = ctx->nets.at(clksig)->clkconstr->high.minDelay();
                                    }
                                }
                            }
                            auto path_budget = period - endpoint_arrival;

                            if (update) {
                                auto budget_share = budget_override ? 0 : path_budget / net_length_plus_one;
                                usr.budget = std::min(usr.budget, net_delay + budget_share);
                                net_min_remaining_budget =
                                        std::min(net_min_remaining_budget, path_budget - budget_share);
                            }

                            if (path_budget < min_slack)
                                min_slack = path_budget;

                            if (slack_histogram) {
                                int slack_ps = ctx->getDelayNS(path_budget) * 1000;
                                (*slack_histogram)[slack_ps]++;
                            }
                            ClockEvent dest_ev{clksig, edge};
                            ClockPair clockPair{startdomain.first, dest_ev};
                            nd.arrival_time[dest_ev] = std::max(nd.arrival_time[dest_ev], endpoint_arrival);

                            if (crit_path) {
                                if (!crit_nets.count(clockPair) || crit_nets.at(clockPair).first < endpoint_arrival) {
                                    crit_nets[clockPair] = std::make_pair(endpoint_arrival, net);
                                    (*crit_path)[clockPair].path_delay = endpoint_arrival;
                                    (*crit_path)[clockPair].path_period = period;
                                    (*crit_path)[clockPair].ports.clear();
                                    (*crit_path)[clockPair].ports.push_back(&usr);
                                }
                            }
                        };
                        if (portClass == TMG_REGISTER_INPUT) {
                            for (int i = 0; i < port_clocks; i++) {
                                TimingClockingInfo clkInfo = ctx->getPortClockingInfo(usr.cell, usr.port, i);
                                const NetInfo *clknet = get_net_or_empty(usr.cell, clkInfo.clock_port);
                                IdString clksig = clknet ? clknet->name : async_clock;
                                process_endpoint(clksig, clknet ? clkInfo.edge : RISING_EDGE, clkInfo.setup.maxDelay());
                            }
                        } else {
                            process_endpoint(async_clock, RISING_EDGE, 0);
                        }

                    } else if (update) {

                        // Iterate over all output ports on the same cell as the sink
                        for (const auto &port : usr.cell->ports) {
                            if (port.second.type != PORT_OUT || !port.second.net)
                                continue;
                            DelayInfo comb_delay;
                            bool is_path = ctx->getCellDelay(usr.cell, usr.port, port.first, comb_delay);
                            if (!is_path)
                                continue;
                            if (net_data.count(port.second.net) &&
                                net_data.at(port.second.net).count(startdomain.first)) {
                                auto path_budget =
                                        net_data.at(port.second.net).at(startdomain.first).min_remaining_budget;
                                auto budget_share = budget_override ? 0 : path_budget / net_length_plus_one;
                                usr.budget = std::min(usr.budget, net_delay + budget_share);
                                net_min_remaining_budget =
                                        std::min(net_min_remaining_budget, path_budget - budget_share);
                            }
                        }
                    }
                }
            }
        }

        if (crit_path) {
            // Walk backwards from the most critical net
            for (auto crit_pair : crit_nets) {
                NetInfo *crit_net = crit_pair.second.second;
                auto &cp_ports = (*crit_path)[crit_pair.first].ports;
                while (crit_net) {
                    const PortInfo *crit_ipin = nullptr;
                    delay_t max_arrival = std::numeric_limits<delay_t>::min();
                    // Look at all input ports on its driving cell
                    for (const auto &port : crit_net->driver.cell->ports) {
                        if (port.second.type != PORT_IN || !port.second.net)
                            continue;
                        DelayInfo comb_delay;
                        bool is_path =
                                ctx->getCellDelay(crit_net->driver.cell, port.first, crit_net->driver.port, comb_delay);
                        if (!is_path)
                            continue;
                        // If input port is influenced by a clock, skip
                        int port_clocks;
                        TimingPortClass portClass =
                                ctx->getPortTimingClass(crit_net->driver.cell, port.first, port_clocks);
                        if (portClass == TMG_CLOCK_INPUT || portClass == TMG_ENDPOINT || portClass == TMG_IGNORE)
                            continue;
                        // And find the fanin net with the latest arrival time
                        if (net_data.count(port.second.net) &&
                            net_data.at(port.second.net).count(crit_pair.first.start)) {
                            auto net_arrival = net_data.at(port.second.net).at(crit_pair.first.start).max_arrival;
                            if (net_delays) {
                                for (auto &user : port.second.net->users)
                                    if (user.port == port.first && user.cell == crit_net->driver.cell) {
                                        net_arrival += ctx->getNetinfoRouteDelay(port.second.net, user);
                                        break;
                                    }
                            }
                            net_arrival += comb_delay.maxDelay();
                            if (net_arrival > max_arrival) {
                                max_arrival = net_arrival;
                                crit_ipin = &port.second;
                            }
                        }
                    }

                    if (!crit_ipin)
                        break;
                    // Now convert PortInfo* into a PortRef*
                    for (auto &usr : crit_ipin->net->users) {
                        if (usr.cell->name == crit_net->driver.cell->name && usr.port == crit_ipin->name) {
                            cp_ports.push_back(&usr);
                            break;
                        }
                    }
                    crit_net = crit_ipin->net;
                }
                std::reverse(cp_ports.begin(), cp_ports.end());
            }
        }

        if (net_crit) {
            NPNR_ASSERT(crit_path);
            // Go through in reverse topographical order to set required times
            for (auto net : boost::adaptors::reverse(topographical_order)) {
                if (!net_data.count(net))
                    continue;
                auto &nd_map = net_data.at(net);
                for (auto &startdomain : nd_map) {
                    auto &nd = startdomain.second;
                    if (nd.false_startpoint)
                        continue;
                    if (startdomain.first.clock == async_clock)
                        continue;
                    if (nd.min_required.empty())
                        nd.min_required.resize(net->users.size(), std::numeric_limits<delay_t>::max());
                    delay_t net_min_required = std::numeric_limits<delay_t>::max();
                    for (size_t i = 0; i < net->users.size(); i++) {
                        auto &usr = net->users.at(i);
                        auto net_delay = ctx->getNetinfoRouteDelay(net, usr);
                        int port_clocks;
                        TimingPortClass portClass = ctx->getPortTimingClass(usr.cell, usr.port, port_clocks);
                        if (portClass == TMG_REGISTER_INPUT || portClass == TMG_ENDPOINT) {
                            auto process_endpoint = [&](IdString clksig, ClockEdge edge, delay_t setup) {
                                delay_t period;
                                // Set default period
                                if (edge == startdomain.first.edge) {
                                    period = clk_period;
                                } else {
                                    period = clk_period / 2;
                                }
                                if (clksig != async_clock) {
                                    if (ctx->nets.at(clksig)->clkconstr) {
                                        if (edge == startdomain.first.edge) {
                                            // same edge
                                            period = ctx->nets.at(clksig)->clkconstr->period.minDelay();
                                        } else if (edge == RISING_EDGE) {
                                            // falling -> rising
                                            period = ctx->nets.at(clksig)->clkconstr->low.minDelay();
                                        } else if (edge == FALLING_EDGE) {
                                            // rising -> falling
                                            period = ctx->nets.at(clksig)->clkconstr->high.minDelay();
                                        }
                                    }
                                }
                                nd.min_required.at(i) = std::min(period - setup, nd.min_required.at(i));
                            };
                            if (portClass == TMG_REGISTER_INPUT) {
                                for (int j = 0; j < port_clocks; j++) {
                                    TimingClockingInfo clkInfo = ctx->getPortClockingInfo(usr.cell, usr.port, j);
                                    const NetInfo *clknet = get_net_or_empty(usr.cell, clkInfo.clock_port);
                                    IdString clksig = clknet ? clknet->name : async_clock;
                                    process_endpoint(clksig, clknet ? clkInfo.edge : RISING_EDGE,
                                                     clkInfo.setup.maxDelay());
                                }
                            } else {
                                process_endpoint(async_clock, RISING_EDGE, 0);
                            }
                        }
                        net_min_required = std::min(net_min_required, nd.min_required.at(i) - net_delay);
                    }
                    PortRef &drv = net->driver;
                    if (drv.cell == nullptr)
                        continue;
                    for (const auto &port : drv.cell->ports) {
                        if (port.second.type != PORT_IN || !port.second.net)
                            continue;
                        DelayInfo comb_delay;
                        bool is_path = ctx->getCellDelay(drv.cell, port.first, drv.port, comb_delay);
                        if (!is_path)
                            continue;
                        int cc;
                        auto pclass = ctx->getPortTimingClass(drv.cell, port.first, cc);
                        if (pclass != TMG_COMB_INPUT)
                            continue;
                        NetInfo *sink_net = port.second.net;
                        if (net_data.count(sink_net) && net_data.at(sink_net).count(startdomain.first)) {
                            auto &sink_nd = net_data.at(sink_net).at(startdomain.first);
                            if (sink_nd.min_required.empty())
                                sink_nd.min_required.resize(sink_net->users.size(),
                                                            std::numeric_limits<delay_t>::max());
                            for (size_t i = 0; i < sink_net->users.size(); i++) {
                                auto &user = sink_net->users.at(i);
                                if (user.cell == drv.cell && user.port == port.first) {
                                    sink_nd.min_required.at(i) = std::min(sink_nd.min_required.at(i),
                                                                          net_min_required - comb_delay.maxDelay());
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            std::unordered_map<ClockEvent, delay_t> worst_slack;

            // Assign slack values
            for (auto &net_entry : net_data) {
                const NetInfo *net = net_entry.first;
                for (auto &startdomain : net_entry.second) {
                    auto &nd = startdomain.second;
                    if (startdomain.first.clock == async_clock)
                        continue;
                    if (nd.min_required.empty())
                        continue;
                    auto &nc = (*net_crit)[net->name];
                    if (nc.slack.empty())
                        nc.slack.resize(net->users.size(), std::numeric_limits<delay_t>::max());
#if 0
                    if (ctx->debug)
                        log_info("Net %s cd %s\n", net->name.c_str(ctx), startdomain.first.clock.c_str(ctx));
#endif
                    for (size_t i = 0; i < net->users.size(); i++) {
                        delay_t slack = nd.min_required.at(i) -
                                        (nd.max_arrival + ctx->getNetinfoRouteDelay(net, net->users.at(i)));
#if 0
                        if (ctx->debug)
                            log_info("    user %s.%s required %.02fns arrival %.02f route %.02f slack %.02f\n",
                                    net->users.at(i).cell->name.c_str(ctx), net->users.at(i).port.c_str(ctx),
                                    ctx->getDelayNS(nd.min_required.at(i)), ctx->getDelayNS(nd.max_arrival),
                                    ctx->getDelayNS(ctx->getNetinfoRouteDelay(net, net->users.at(i))), ctx->getDelayNS(slack));
#endif
                        if (worst_slack.count(startdomain.first))
                            worst_slack.at(startdomain.first) = std::min(worst_slack.at(startdomain.first), slack);
                        else
                            worst_slack[startdomain.first] = slack;
                        nc.slack.at(i) = slack;
                    }
                    if (ctx->debug)
                        log_break();
                }
            }
            // Assign criticality values
            for (auto &net_entry : net_data) {
                const NetInfo *net = net_entry.first;
                for (auto &startdomain : net_entry.second) {
                    if (startdomain.first.clock == async_clock)
                        continue;
                    auto &nd = startdomain.second;
                    if (nd.min_required.empty())
                        continue;
                    auto &nc = (*net_crit)[net->name];
                    if (nc.slack.empty())
                        continue;
                    if (nc.criticality.empty())
                        nc.criticality.resize(net->users.size(), 0);
                    // Only consider intra-clock paths for criticality
                    if (!crit_path->count(ClockPair{startdomain.first, startdomain.first}))
                        continue;
                    delay_t dmax = crit_path->at(ClockPair{startdomain.first, startdomain.first}).path_delay;
                    for (size_t i = 0; i < net->users.size(); i++) {
                        float criticality =
                                1.0f - ((float(nc.slack.at(i)) - float(worst_slack.at(startdomain.first))) / dmax);
                        nc.criticality.at(i) = std::min<double>(1.0, std::max<double>(0.0, criticality));
                    }
                    nc.max_path_length = nd.max_path_length;
                    nc.cd_worst_slack = worst_slack.at(startdomain.first);
                }
            }
#if 0
            if (ctx->debug) {
                for (auto &nc : *net_crit) {
                    NetInfo *net = ctx->nets.at(nc.first).get();
                    log_info("Net %s maxlen %d worst_slack %.02fns: \n", nc.first.c_str(ctx), nc.second.max_path_length,
                             ctx->getDelayNS(nc.second.cd_worst_slack));
                    if (!nc.second.criticality.empty() && !nc.second.slack.empty()) {
                        for (size_t i = 0; i < net->users.size(); i++) {
                            log_info("   user %s.%s slack %.02fns crit %.03f\n", net->users.at(i).cell->name.c_str(ctx),
                                     net->users.at(i).port.c_str(ctx), ctx->getDelayNS(nc.second.slack.at(i)),
                                     nc.second.criticality.at(i));
                        }
                    }
                    log_break();
                }
            }
#endif
        }
        return min_slack;
    }

    void assign_budget()
    {
        // Clear delays to a very high value first
        for (auto &net : ctx->nets) {
            for (auto &usr : net.second->users) {
                usr.budget = std::numeric_limits<delay_t>::max();
            }
        }

        walk_paths();
    }
};
*/

/**
void assign_budget(Context *ctx, bool quiet)
{
    if (!quiet) {
        log_break();
        if(ctx->verbose){
            LogData logEntry = LogData::CreateLogStruct(
                LevelCode::ALWAYS_LOG,
                LogCategory::OPT,
                PhaseType::PACK,
                "Annotating ports with timing budgets for target frequency"
            );
            log_always("Annotating ports with timing budgets for target frequency %.2f MHz\n",logEntry,
                    ctx->setting<float>("target_freq") / 1e6);
        }
    }

    Timing timing(ctx, ctx->setting<int>("slack_redist_iter") > 0 , true);
    timing.assign_budget();

    if (!quiet || ctx->verbose) {
        for (auto &net : ctx->nets) {
            for (auto &user : net.second->users) {
                // Post-update check
                if (!ctx->setting<bool>("auto_freq") && user.budget < 0){
                    LogData logEntry1 = LogData::CreateLogStruct(
                        LevelCode::ALWAYS_LOG,
                        LogCategory::OPT,
                        PhaseType::PLACE,
                        "port connected to net has negative timing budget"
                    );
                    log_always("port %s.%s, connected to net '%s', has negative "
                             "timing budget of %fns\n",logEntry1,
                             user.cell->name.c_str(ctx), user.port.c_str(ctx), net.first.c_str(ctx),
                             ctx->getDelayNS(user.budget));
                }
                else if (ctx->debug){
                    LogData logEntry2 = LogData::CreateLogStruct(
                        LevelCode::ALWAYS_LOG,
                        LogCategory::OPT,
                        PhaseType::PLACE,
                        "port connected to net has timing budget"
                    );
                    log_always("port %s.%s, connected to net '%s', has "
                             "timing budget of %fns\n",
                             user.cell->name.c_str(ctx), user.port.c_str(ctx), net.first.c_str(ctx),
                             ctx->getDelayNS(user.budget));
                }
            }
        }
    }

    // For slack redistribution, if user has not specified a frequency dynamically adjust the target frequency to be the
    // currently achieved maximum
    if (ctx->setting<bool>("auto_freq") && ctx->setting<int>("slack_redist_iter") > 0) {
        delay_t default_slack = delay_t((1.0e9 / ctx->getDelayNS(1)) / ctx->setting<float>("target_freq"));
        ctx->settings[ctx->id("target_freq")] =
                std::to_string(1.0e9 / ctx->getDelayNS(default_slack - timing.min_slack));
        if (ctx->verbose)
            log_info("minimum slack for this assign = %.2f ns, target Fmax for next "
                     "update = %.2f MHz\n",
                     ctx->getDelayNS(timing.min_slack), ctx->setting<float>("target_freq") / 1e6);
    }

    if (!quiet){
        if(ctx->verbose){
            LogData logEntry3 = LogData::CreateLogStruct(
                LevelCode::ALWAYS_LOG,
                LogCategory::OPT,
                PhaseType::PACK,
                "Checksum"
            );
            log_always("Checksum: 0x%08x\n", logEntry3,ctx->checksum());
        }
    }
}
 */

/** 
void timing_analysis(Context *ctx, bool print_histogram, bool print_fmax, bool print_path, bool warn_on_failure)
{
    auto format_event = [ctx](const ClockEvent &e, int field_width = 0) {
        std::string value;
        if (e.clock == ctx->id("$async$"))
            value = std::string("<async>");
        else
            value = (e.edge == FALLING_EDGE ? std::string("negedge ") : std::string("posedge ")) + e.clock.str(ctx);
        if (int(value.length()) < field_width)
            value.insert(value.length(), field_width - int(value.length()), ' ');
        return value;
    };

    CriticalPathMap crit_paths;
    DelayFrequency slack_histogram;

    Timing timing(ctx, true, false, (print_path || print_fmax) ? &crit_paths : nullptr,
                  print_histogram ? &slack_histogram : nullptr);
    timing.walk_paths();
    std::map<IdString, std::pair<ClockPair, CriticalPath>> clock_reports;
    std::map<IdString, double> clock_fmax;
    std::vector<ClockPair> xclock_paths;
    std::set<IdString> empty_clocks; // set of clocks with no interior paths
    if (print_path || print_fmax) {
        for (auto path : crit_paths) {
            const ClockEvent &a = path.first.start;
            const ClockEvent &b = path.first.end;
            empty_clocks.insert(a.clock);
            empty_clocks.insert(b.clock);
        }
        for (auto path : crit_paths) {
            const ClockEvent &a = path.first.start;
            const ClockEvent &b = path.first.end;
            if (a.clock != b.clock || a.clock == ctx->id("$async$"))
                continue;
            double Fmax;
            empty_clocks.erase(a.clock);
            if (a.edge == b.edge)
                Fmax = 1000 / ctx->getDelayNS(path.second.path_delay);
            else
                Fmax = 500 / ctx->getDelayNS(path.second.path_delay);
            if (!clock_fmax.count(a.clock) || Fmax < clock_fmax.at(a.clock)) {
                clock_reports[a.clock] = path;
                clock_fmax[a.clock] = Fmax;
            }
        }

        for (auto &path : crit_paths) {
            const ClockEvent &a = path.first.start;
            const ClockEvent &b = path.first.end;
            if (a.clock == b.clock && a.clock != ctx->id("$async$"))
                continue;
            xclock_paths.push_back(path.first);
        }

        if (clock_reports.empty()) {
            log_warning("No clocks found in design\n");
        }

        std::sort(xclock_paths.begin(), xclock_paths.end(), [ctx](const ClockPair &a, const ClockPair &b) {
            if (a.start.clock.str(ctx) < b.start.clock.str(ctx))
                return true;
            if (a.start.clock.str(ctx) > b.start.clock.str(ctx))
                return false;
            if (a.start.edge < b.start.edge)
                return true;
            if (a.start.edge > b.start.edge)
                return false;
            if (a.end.clock.str(ctx) < b.end.clock.str(ctx))
                return true;
            if (a.end.clock.str(ctx) > b.end.clock.str(ctx))
                return false;
            if (a.end.edge < b.end.edge)
                return true;
            return false;
        });
    }

    if (print_path) {
        auto print_path_report = [ctx](ClockPair &clocks, PortRefVector &crit_path) {
            delay_t total = 0, logic_total = 0, route_total = 0;
            auto &front = crit_path.front();
            auto &front_port = front->cell->ports.at(front->port);
            auto &front_driver = front_port.net->driver;

            int port_clocks;
            auto portClass = ctx->getPortTimingClass(front_driver.cell, front_driver.port, port_clocks);
            IdString last_port = front_driver.port;
            int clock_start = -1;
            if (portClass == TMG_REGISTER_OUTPUT) {
                for (int i = 0; i < port_clocks; i++) {
                    TimingClockingInfo clockInfo = ctx->getPortClockingInfo(front_driver.cell, front_driver.port, i);
                    const NetInfo *clknet = get_net_or_empty(front_driver.cell, clockInfo.clock_port);
                    if (clknet != nullptr && clknet->name == clocks.start.clock &&
                        clockInfo.edge == clocks.start.edge) {
                        last_port = clockInfo.clock_port;
                        clock_start = i;
                        break;
                    }
                }
            }
            LogData logEntry5 = LogData::CreateLogStruct(
                LevelCode::INFO_LOG,
                LogCategory::ROUTE,
                PhaseType::ROUTE,
                "curr total"
            );
            log_info("curr total\n",logEntry5);
            for (auto sink : crit_path) {
                auto sink_cell = sink->cell;
                auto &port = sink_cell->ports.at(sink->port);
                auto net = port.net;
                auto &driver = net->driver;
                auto driver_cell = driver.cell;
                DelayInfo comb_delay;
                if (clock_start != -1) {
                    auto clockInfo = ctx->getPortClockingInfo(driver_cell, driver.port, clock_start);
                    comb_delay = clockInfo.clockToQ;
                    clock_start = -1;
                } else if (last_port == driver.port) {
                    // Case where we start with a STARTPOINT etc
                    comb_delay = ctx->getDelayFromNS(0);
                } else {
                    ctx->getCellDelay(driver_cell, last_port, driver.port, comb_delay);
                }
                total += comb_delay.maxDelay();
                logic_total += comb_delay.maxDelay();
                // log_info("%4.1f %4.1f  Source %s.%s\n", ctx->getDelayNS(comb_delay.maxDelay()), ctx->getDelayNS(total),
                //          driver_cell->name.c_str(ctx), driver.port.c_str(ctx));
                auto net_delay = ctx->getNetinfoRouteDelay(net, *sink);
                total += net_delay;
                route_total += net_delay;
                auto driver_loc = ctx->getBelLocation(driver_cell->bel);
                auto sink_loc = ctx->getBelLocation(sink_cell->bel);
                // log_info("%4.1f %4.1f    Net %s budget %f ns (%d,%d) -> (%d,%d)\n", ctx->getDelayNS(net_delay),
                //          ctx->getDelayNS(total), net->name.c_str(ctx), ctx->getDelayNS(sink->budget), driver_loc.x,
                //          driver_loc.y, sink_loc.x, sink_loc.y);
                // log_info("               Sink %s.%s\n", sink_cell->name.c_str(ctx), sink->port.c_str(ctx));
                if (ctx->verbose) {
                    auto driver_wire = ctx->getNetinfoSourceWire(net);
                    auto sink_wire = ctx->getNetinfoSinkWire(net, *sink);
                    // log_info("                 prediction: %f ns estimate: %f ns\n",
                    //          ctx->getDelayNS(ctx->predictDelay(net, *sink)),
                    //          ctx->getDelayNS(ctx->estimateDelay(driver_wire, sink_wire)));
                    auto cursor = sink_wire;
                    delay_t delay;
                    while (driver_wire != cursor) {
#ifdef ARCH_ECP5
                        if (net->is_global)
                            break;
#endif
                        auto it = net->wires.find(cursor);
                        assert(it != net->wires.end());
                        auto pip = it->second.pip;
                        NPNR_ASSERT(pip != PipId());
                        delay = ctx->getPipDelay(pip).maxDelay();
                        // log_info("                 %1.3f %s\n", ctx->getDelayNS(delay),
                        //          ctx->getPipName(pip).c_str(ctx));
                        cursor = ctx->getPipSrcWire(pip);
                    }
                }
                last_port = sink->port;
            }
            int clockCount = 0;
            auto sinkClass = ctx->getPortTimingClass(crit_path.back()->cell, crit_path.back()->port, clockCount);
            if (sinkClass == TMG_REGISTER_INPUT && clockCount > 0) {
                auto sinkClockInfo = ctx->getPortClockingInfo(crit_path.back()->cell, crit_path.back()->port, 0);
                delay_t setup = sinkClockInfo.setup.maxDelay();
                total += setup;
                logic_total += setup;
                // log_info("%4.1f %4.1f  Setup %s.%s\n", ctx->getDelayNS(setup), ctx->getDelayNS(total),
                //          crit_path.back()->cell->name.c_str(ctx), crit_path.back()->port.c_str(ctx));
            }
            // log_info("%.1f ns logic, %.1f ns routing\n", ctx->getDelayNS(logic_total), ctx->getDelayNS(route_total));
        };

        for (auto &clock : clock_reports) {
            log_break();
            std::string start =
                    clock.second.first.start.edge == FALLING_EDGE ? std::string("negedge") : std::string("posedge");
            std::string end =
                    clock.second.first.end.edge == FALLING_EDGE ? std::string("negedge") : std::string("posedge");
            LogData logEntry4 = LogData::CreateLogStruct(
                LevelCode::INFO_LOG,
                LogCategory::ROUTE,
                PhaseType::ROUTE,
                "Critical path report for clock"
            );
            log_info("Critical path report for clock '%s' (%s -> %s):\n", logEntry4,clock.first.c_str(ctx), start.c_str(),
                     end.c_str());
            auto &crit_path = clock.second.second.ports;
            print_path_report(clock.second.first, crit_path);
        }

        for (auto &xclock : xclock_paths) {
            log_break();
            std::string start = format_event(xclock.start);
            std::string end = format_event(xclock.end);
            log_info("Critical path report for cross-domain path '%s' -> '%s':\n", start.c_str(), end.c_str());
            auto &crit_path = crit_paths.at(xclock).ports;
            print_path_report(xclock, crit_path);
        }
    }
    if (print_fmax) {
        log_break();
        unsigned max_width = 0;
        for (auto &clock : clock_reports)
            max_width = std::max<unsigned>(max_width, clock.first.str(ctx).size());
        for (auto &clock : clock_reports) {
            const auto &clock_name = clock.first.str(ctx);
            const int width = max_width - clock_name.size();
            float target = ctx->setting<float>("target_freq") / 1e6;
            if (ctx->nets.at(clock.first)->clkconstr)
                target = 1000 / ctx->getDelayNS(ctx->nets.at(clock.first)->clkconstr->period.minDelay());

            bool passed = target < clock_fmax[clock.first];
            // if (!warn_on_failure || passed)
                // log_info("Max frequency for clock %*s'%s': %.02f MHz (%s at %.02f MHz)\n", width, "",
                //          clock_name.c_str(), clock_fmax[clock.first], passed ? "PASS" : "FAIL", target);
            // else if (bool_or_default(ctx->settings, ctx->id("timing/allowFail"), false))
                // log_warning("Max frequency for clock %*s'%s': %.02f MHz (%s at %.02f MHz)\n", width, "",
                //             clock_name.c_str(), clock_fmax[clock.first], passed ? "PASS" : "FAIL", target);
            // else
                // log_nonfatal_error("Max frequency for clock %*s'%s': %.02f MHz (%s at %.02f MHz)\n", width, "",
                //                    clock_name.c_str(), clock_fmax[clock.first], passed ? "PASS" : "FAIL", target);
        }
        for (auto &eclock : empty_clocks) {
            if (eclock != ctx->id("$async$"))
                log_info("Clock '%s' has no interior paths\n", eclock.c_str(ctx));
        }
        log_break();

        int start_field_width = 0, end_field_width = 0;
        for (auto &xclock : xclock_paths) {
            start_field_width = std::max((int)format_event(xclock.start).length(), start_field_width);
            end_field_width = std::max((int)format_event(xclock.end).length(), end_field_width);
        }

        for (auto &xclock : xclock_paths) {
            const ClockEvent &a = xclock.start;
            const ClockEvent &b = xclock.end;
            auto &path = crit_paths.at(xclock);
            auto ev_a = format_event(a, start_field_width), ev_b = format_event(b, end_field_width);
            log_info("Max delay %s -> %s: %0.02f ns\n", ev_a.c_str(), ev_b.c_str(), ctx->getDelayNS(path.path_delay));
        }
        log_break();
    }

    if (print_histogram && slack_histogram.size() > 0) {
        unsigned num_bins = 20;
        unsigned bar_width = 60;
        auto min_slack = slack_histogram.begin()->first;
        auto max_slack = slack_histogram.rbegin()->first;
        auto bin_size = std::max<unsigned>(1, ceil((max_slack - min_slack + 1) / float(num_bins)));
        std::vector<unsigned> bins(num_bins);
        unsigned max_freq = 0;
        for (const auto &i : slack_histogram) {
            auto &bin = bins[(i.first - min_slack) / bin_size];
            bin += i.second;
            max_freq = std::max(max_freq, bin);
        }
        bar_width = std::min(bar_width, max_freq);

        log_break();
        LogData logEntry6 = LogData::CreateLogStruct(
            LevelCode::INFO_LOG,
            LogCategory::ROUTE,
            PhaseType::ROUTE,
            "Slack histogram"
        );
        log_info("Slack histogram:\n",logEntry6);
        // log_info(" legend: * represents %d endpoint(s)\n", max_freq / bar_width);
        // log_info("         + represents [1,%d) endpoint(s)\n", max_freq / bar_width);
        for (unsigned i = 0; i < num_bins; ++i)
            if(ctx->verbose){
                LogData logEntry7 = LogData::CreateLogStruct(
                    LevelCode::ALWAYS_LOG,
                    LogCategory::PACK,
                    PhaseType::PACK,
                    ""
                );
                log_always("[%6d, %6d) |%s%c\n", logEntry7,min_slack + bin_size * i, min_slack + bin_size * (i + 1),
                     std::string(bins[i] * bar_width / max_freq, '*').c_str(),
                     (bins[i] * bar_width) % max_freq > 0 ? '+' : ' ');
            }
    }
}
*/

CellInfo *TimingAnalyser::cell_info(const CellPortKey &key) { return ctx->cells.at(key.cell).get(); }

PortInfo &TimingAnalyser::port_info(const CellPortKey &key) { return ctx->cells.at(key.cell)->ports.at(key.port); }

domain_id_t TimingAnalyser::domain_id(IdString cell, IdString clock_port, ClockEdge edge)
{
    return domain_id(ctx->cells.at(cell)->ports.at(clock_port).net, edge);
}

domain_id_t TimingAnalyser::domain_id(const NetInfo *net, ClockEdge edge)
{
    NPNR_ASSERT(net != nullptr);
    ClockDomainKey key{net->name, edge};
    auto inserted = domain_to_id.emplace(key, domains.size());
    if (inserted.second) {
        domains.emplace_back(key);
    }
    return inserted.first->second;
}

void TimingAnalyser::copy_domains(const CellPortKey &from, const CellPortKey &to, bool backward)
{
    auto &f = ports.at(from), &t = ports.at(to);
    for (auto &dom : (backward ? f.required : f.arrival)) {
        updated_domains |= (backward ? t.required : t.arrival).emplace(dom.first, ArrivReqTime{}).second;
    }
}

domain_id_t TimingAnalyser::domain_pair_id(domain_id_t launch, domain_id_t capture)
{
    ClockDomainPairKey key{launch, capture};
    auto inserted = pair_to_id.emplace(key, domain_pairs.size());
    if (inserted.second) {
        domain_pairs.emplace_back(key);
    }
    return inserted.first->second;
}

void TimingAnalyser::set_arrival_time(CellPortKey target, domain_id_t domain, DelayPair arrival, int path_length,
                                      CellPortKey prev)
{
    auto &arr = ports.at(target).arrival.at(domain);
    if (arrival.max_delay > arr.value.max_delay) {
        arr.value.max_delay = arrival.max_delay;
        arr.bwd_max = prev;
    }
    if (!setup_only && (arrival.min_delay < arr.value.min_delay)) {
        arr.value.min_delay = arrival.min_delay;
        arr.bwd_min = prev;
    }
    arr.path_length = std::max(arr.path_length, path_length);
}

void TimingAnalyser::set_required_time(CellPortKey target, domain_id_t domain, DelayPair required, int path_length,
                                       CellPortKey prev)
{
    auto &req = ports.at(target).required.at(domain);
    if (required.min_delay < req.value.min_delay) {
        req.value.min_delay = required.min_delay;
        req.bwd_min = prev;
    }
    if (!setup_only && (required.max_delay > req.value.max_delay)) {
        req.value.max_delay = required.max_delay;
        req.bwd_max = prev;
    }
    req.path_length = std::max(req.path_length, path_length);
}

std::vector<CriticalPath> TimingAnalyser::get_min_delay_violations()
{
    std::vector<CriticalPath> violations;

    for (domain_id_t capture_id = 0; capture_id < domain_id_t(domains.size()); ++capture_id) {
        const auto &capture = domains.at(capture_id);
        const auto &capture_clock = capture.key.clock;

        for (const auto &ep : capture.endpoints) {
            const CellInfo *ci = cell_info(ep.first);
            int clkInfoCount = 0;
            const TimingPortClass cls = ctx->getPortTimingClass(ci, ep.first.port, clkInfoCount);
            if (cls != TMG_REGISTER_INPUT)
                continue;

            const auto &port = ports.at(ep.first);

            const auto &req = port.required.at(capture_id);

            for (auto &[launch_id, arr] : port.arrival) {
                const auto &launch = domains.at(launch_id);
                const auto &launch_clock = launch.key.clock;
                const auto dom_pair_id = domain_pair_id(launch_id, capture_id);

                auto clocks = std::make_pair(launch_clock, capture_clock);
                auto related_clocks = clock_delays.count(clocks) > 0;

                if (launch_id == async_clock_id || (launch_id != capture_id && !related_clocks)) {
                    continue;
                }

                delay_t clock_to_clock = 0;
                if (related_clocks) {
                    clock_to_clock = clock_delays.at(clocks);
                }

                auto hold_slack = arr.value.minDelay() - req.value.maxDelay() + clock_to_clock;

                if (hold_slack <= 0) {
                    auto report = build_critical_path_report(dom_pair_id, ep.first, false);
                    violations.emplace_back(report);
                }
            }
        }
    }

    std::vector<std::pair<size_t, delay_t>> sum_indices;
    sum_indices.reserve(violations.size());

    for (size_t i = 0; i < violations.size(); ++i) {
        delay_t delay = 0;
        for (const auto &seg : violations[i].segments) {
            delay += seg.delay;
        }

        sum_indices.emplace_back(i, delay);
    }

    std::sort(sum_indices.begin(), sum_indices.end(),
              [](const std::pair<size_t, delay_t> &left, const std::pair<size_t, delay_t> &right) {
                  return left.second < right.second;
              });

    std::vector<CriticalPath> sorted_violations;
    sorted_violations.reserve(violations.size());
    for (const auto &pair : sum_indices) {
        sorted_violations.push_back(std::move(violations[pair.first]));
    }

    return sorted_violations;
}

CriticalPath TimingAnalyser::build_critical_path_report(domain_id_t domain_pair, CellPortKey endpoint,
                                                        bool longest_path)
{
    CriticalPath report;

    const auto &dp = domain_pairs.at(domain_pair);
    const auto &launch = domains.at(dp.key.launch).key;
    const auto &capture = domains.at(dp.key.capture).key;

    report.clock_pair.start.clock = launch.clock;
    report.clock_pair.start.edge = launch.edge;
    report.clock_pair.end.clock = capture.clock;
    report.clock_pair.end.edge = capture.edge;

    report.max_delay = ctx->getDelayFromNS(1.0e9 / ctx->setting<float>("target_freq"));
    if (launch.edge != capture.edge) {
        report.max_delay = report.max_delay / 2;
    }

    if (!launch.is_async() && ctx->nets.at(launch.clock)->clkconstr) {
        if (launch.edge == capture.edge) {
            report.max_delay = ctx->nets.at(launch.clock)->clkconstr->period.minDelay();
        } else if (capture.edge == RISING_EDGE) {
            report.max_delay = ctx->nets.at(launch.clock)->clkconstr->low.minDelay();
        } else if (capture.edge == FALLING_EDGE) {
            report.max_delay = ctx->nets.at(launch.clock)->clkconstr->high.minDelay();
        }
    }

    auto crit_path_rev = walk_crit_path(domain_pair, endpoint, longest_path);
    auto crit_path = boost::adaptors::reverse(crit_path_rev);

    // Get timing and clocking info on the startpoint
    auto first_inp = crit_path.front();
    const auto &sp = first_inp.cell->ports.at(first_inp.port).net->driver;
    const auto &sp_cell = sp.cell;
    const auto &sp_port = sp_cell->ports.at(sp.port);
    int sp_clocks;
    const auto sp_portClass = ctx->getPortTimingClass(sp_cell, sp_port.name, sp_clocks);
    TimingClockingInfo sp_clk_info;
    const NetInfo *sp_clk_net = nullptr;
    bool register_start = sp_portClass == TMG_REGISTER_OUTPUT;

    if (register_start) {
        // If we don't find a clock we don't consider this startpoint to be registered.
        register_start = sp_clocks > 0;
        for (int i = 0; i < sp_clocks; i++) {
            sp_clk_info = ctx->getPortClockingInfo(sp_cell, sp_port.name, i);
            const auto clk_net = sp_cell->getPort(sp_clk_info.clock_port); 
            register_start = clk_net != nullptr && clk_net->name == launch.clock && sp_clk_info.edge == launch.edge;
            if (register_start) {
                sp_clk_net = clk_net;
                break;
            }
        }
    }

    // Get timing and clocking info on the endpoint
    const auto &ep = crit_path.back();
    const auto &ep_cell = ep.cell;
    const auto &ep_port = ep_cell->ports.at(ep.port);
    int ep_clocks;
    const auto ep_portClass = ctx->getPortTimingClass(ep_cell, ep_port.name, ep_clocks);
    TimingClockingInfo ep_clk_info;
    const NetInfo *ep_clk_net = nullptr;

    bool register_end = ep_portClass == TMG_REGISTER_INPUT;

    if (register_end) {
        // If we don't find a clock we don't consider this startpoint to be registered.
        register_end = ep_clocks > 0;
        for (int i = 0; i < ep_clocks; i++) {
            ep_clk_info = ctx->getPortClockingInfo(ep_cell, ep_port.name, i);
            const auto clk_net = ep_cell->getPort(ep_clk_info.clock_port);

            register_end = clk_net != nullptr && clk_net->name == capture.clock && ep_clk_info.edge == capture.edge;
            if (register_end) {
                ep_clk_net = clk_net;
                break;
            }
        }
    }

    auto clock_pair = std::make_pair(launch.clock, capture.clock);
    auto related_clock = clock_delays.count(clock_pair) > 0;
    auto same_clock = launch.clock == capture.clock;

    if (related_clock) {
        delay_t clock_delay = clock_delays.at(clock_pair);
        if (!is_zero_delay(clock_delay)) {
            CriticalPath::Segment seg_c2c;
            seg_c2c.type = CriticalPath::Segment::Type::CLK_TO_CLK;
            seg_c2c.delay = clock_delay;
            seg_c2c.from = std::make_pair(sp_cell->name, sp_clk_info.clock_port);
            seg_c2c.to = std::make_pair(ep_cell->name, ep_clk_info.clock_port);
            seg_c2c.net = IdString();
            report.segments.push_back(seg_c2c);
        }
    }

    if (with_clock_skew && register_start && register_end && (same_clock || related_clock)) {

        PortRef sp_clk_ref;
        sp_clk_ref.cell = sp_cell;
        sp_clk_ref.port = sp_clk_info.clock_port;
        auto clock_delay_launch = ctx->getNetinfoRouteDelay(sp_clk_net, sp_clk_ref);

        PortRef ep_clk_ref;
        ep_clk_ref.cell = ep_cell;
        ep_clk_ref.port = ep_clk_info.clock_port;
        auto clock_delay_capture = ctx->getNetinfoRouteDelay(ep_clk_net, ep_clk_ref);

        delay_t clock_skew = clock_delay_launch - clock_delay_capture;

        if (!is_zero_delay(clock_skew)) {
            CriticalPath::Segment seg_skew;
            seg_skew.type = CriticalPath::Segment::Type::CLK_SKEW;
            seg_skew.delay = clock_skew;
            seg_skew.from = std::make_pair(sp_cell->name, sp_clk_info.clock_port);
            seg_skew.to = std::make_pair(ep_cell->name, ep_clk_info.clock_port);
            if (same_clock) {
                seg_skew.net = launch.clock;
            } else {
                seg_skew.net = IdString();
            }
            report.segments.push_back(seg_skew);
        }
    }

    const CellInfo *prev_cell = sp_cell;
    IdString prev_port = sp_port.name;

    bool is_startpoint = true;
    for (auto sink : crit_path) {
        auto sink_cell = sink.cell;
        auto &port = sink_cell->ports.at(sink.port);
        auto net = port.net;
        auto &driver = net->driver;
        auto driver_cell = driver.cell;

        CriticalPath::Segment seg_logic;

        DelayQuad comb_delay;
        if (is_startpoint && register_start) {
            comb_delay = sp_clk_info.clockToQ;
            seg_logic.type = CriticalPath::Segment::Type::CLK_TO_Q;
        } else if (is_startpoint) {
            comb_delay = DelayQuad(0);
            seg_logic.type = CriticalPath::Segment::Type::SOURCE;
        } else {
            ctx->getCellDelay(driver_cell, prev_port, driver.port, comb_delay);
            seg_logic.type = CriticalPath::Segment::Type::LOGIC;
        }

        seg_logic.delay = longest_path ? comb_delay.maxDelay() : comb_delay.minDelay();
        seg_logic.from = std::make_pair(prev_cell->name, prev_port);
        seg_logic.to = std::make_pair(driver_cell->name, driver.port);
        seg_logic.net = IdString();
        report.segments.push_back(seg_logic);

        auto net_delay = DelayPair(ctx->getNetinfoRouteDelay(net, sink));

        CriticalPath::Segment seg_route;
        seg_route.type = CriticalPath::Segment::Type::ROUTING;
        seg_route.delay = longest_path ? net_delay.maxDelay() : net_delay.minDelay();
        seg_route.from = std::make_pair(driver_cell->name, driver.port);
        seg_route.to = std::make_pair(sink_cell->name, sink.port);
        seg_route.net = net->name;
        report.segments.push_back(seg_route);

        prev_cell = sink_cell;
        prev_port = sink.port;
        is_startpoint = false;
    }

    if (register_end) {
        CriticalPath::Segment seg_logic;
        seg_logic.delay = 0;
        if (longest_path) {
            seg_logic.type = CriticalPath::Segment::Type::SETUP;
            seg_logic.delay += ep_clk_info.setup.maxDelay();
        } else {
            seg_logic.type = CriticalPath::Segment::Type::HOLD;
            seg_logic.delay -= ep_clk_info.hold.maxDelay();
        }
        seg_logic.from = std::make_pair(prev_cell->name, prev_port);
        seg_logic.to = seg_logic.from;
        seg_logic.net = IdString();
        report.segments.push_back(seg_logic);
    }
    auto &pd = ports.at(endpoint);
    auto &php = pd.domain_pairs.at(domain_pair);
    report.is_setup = longest_path;
    report.slack = report.is_setup? php.setup_slack : php.hold_slack;
    return report;
}

std::vector<PortRef> TimingAnalyser::walk_crit_path(domain_id_t domain_pair, CellPortKey endpoint, bool longest_path)
{
    const auto &dp = domain_pairs.at(domain_pair);

    // Walk the min or max path backwards to find a single crit path
    pool<std::pair<IdString, IdString>> visited;
    std::vector<PortRef> crit_path_rev;
    auto cursor = endpoint;

    bool is_startpoint = false;
    do {
        auto cell = cell_info(cursor);
        auto &port = port_info(cursor);
        int port_clocks;
        auto portClass = ctx->getPortTimingClass(cell, port.name, port_clocks);

        // combinational loop
        if (!visited.insert(std::make_pair(cell->name, port.name)).second)
            break;

        // We store the reversed critical path as all input ports that lead to
        // the timing startpoint.
        auto is_input = portClass != TMG_CLOCK_INPUT && portClass != TMG_IGNORE && port.type == PortType::PORT_IN;

        if (is_input) {
            PortRef pf;
            pf.cell = cell;
            pf.port = port.name;
            crit_path_rev.emplace_back(pf);
        }

        if (!ports.at(cursor).arrival.count(dp.key.launch))
            break;

        if (longest_path) {
            cursor = ports.at(cursor).arrival.at(dp.key.launch).bwd_max;
        } else {
            cursor = ports.at(cursor).arrival.at(dp.key.launch).bwd_min;
        }
        is_startpoint = portClass == TMG_REGISTER_OUTPUT || portClass == TMG_STARTPOINT;
    } while (!is_startpoint);

    return crit_path_rev;
}

TimingAnalyser::TimingAnalyser(Context *ctx) : ctx(ctx)
{
    ClockDomainKey key{IdString(), ClockEdge::RISING_EDGE};
    domain_to_id.emplace(key, 0);
    domains.emplace_back(key);
    async_clock_id = 0;
};

void TimingAnalyser::setup(bool update_net_timings, bool update_histogram, bool update_crit_paths)
{
    init_ports();
    get_cell_delays();
    topo_sort();
    setup_port_domains();
    identify_related_domains();
    run(true, update_net_timings, update_histogram, update_crit_paths);
}

void TimingAnalyser::init_ports()
{
    // Per cell port structures
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        for (auto &port : ci->ports) {
            CellPortKey ck = CellPortKey(ci->name, port.first);
            auto &data = ports[ck];
            data.type = port.second.type;
            data.cell_port = ck;
        }
    }
}

void TimingAnalyser::get_cell_delays()
{
    auto async_clk_key = domains.at(async_clock_id);

    for (auto &port : ports) {
        CellInfo *ci = cell_info(port.first);
        auto &pi = port_info(port.first);
        auto &pd = port.second;

        IdString name = port.first.port;
        // Ignore dangling ports altogether for timing purposes
        if (!pi.net)
            continue;
        pd.cell_arcs.clear();
        int clkInfoCount = 0; // 这里表示pin相关的时钟数量，当前只会是1
        TimingPortClass cls = ctx->getPortTimingClass(ci, name, clkInfoCount);
        if (cls == TMG_CLOCK_INPUT || cls == TMG_GEN_CLOCK || cls == TMG_IGNORE)
            continue;
        if (pi.type == PORT_IN) {
            // Input ports might have setup/hold relationships
            if (cls == TMG_REGISTER_INPUT) {
                for (int i = 0; i < clkInfoCount; i++) {
                    auto info = ctx->getPortClockingInfo(ci, name, i);
                    if (!ci->ports.count(info.clock_port) || ci->ports.at(info.clock_port).net == nullptr)
                        continue;
                    pd.cell_arcs.emplace_back(CellArc::SETUP, info.clock_port, DelayQuad(info.setup, info.setup),
                                              info.edge);
                    pd.cell_arcs.emplace_back(CellArc::HOLD, info.clock_port, DelayQuad(info.hold, info.hold),
                                              info.edge);
                }
            }
            // asynchronous endpoint , OBUF之类的net末端
            else if (cls == TMG_ENDPOINT) {
                pd.cell_arcs.emplace_back(CellArc::ENDPOINT, async_clk_key.key.clock, DelayQuad{});
            }
            // Combinational delays through cell
            for (auto &other_port : ci->ports) {
                auto &op = other_port.second;
                // ignore dangling ports and non-outputs
                if (op.net == nullptr || op.type != PORT_OUT)
                    continue;
                DelayQuad delay;
                bool is_path = ctx->getCellDelay(ci, name, other_port.first, delay);
                if (is_path)
                    pd.cell_arcs.emplace_back(CellArc::COMBINATIONAL, other_port.first, delay);
            }
        } else if (pi.type == PORT_OUT) {
            // Output ports might have clk-to-q relationships
            if (cls == TMG_REGISTER_OUTPUT) {
                for (int i = 0; i < clkInfoCount; i++) {
                    auto info = ctx->getPortClockingInfo(ci, name, i);
                    if (!ci->ports.count(info.clock_port) || ci->ports.at(info.clock_port).net == nullptr)
                        continue;
                    pd.cell_arcs.emplace_back(CellArc::CLK_TO_Q, info.clock_port, info.clockToQ, info.edge);
                }
            }
            // Asynchronous startpoint
            else if (cls == TMG_STARTPOINT) {
                pd.cell_arcs.emplace_back(CellArc::STARTPOINT, async_clk_key.key.clock, DelayQuad{});
            }
            // Combinational delays through cell
            for (auto &other_port : ci->ports) {
                auto &op = other_port.second;
                // ignore dangling ports and non-inputs
                if (op.net == nullptr || op.type != PORT_IN)
                    continue;
                DelayQuad delay;
                bool is_path = ctx->getCellDelay(ci, other_port.first, name, delay);
                if (is_path)
                    pd.cell_arcs.emplace_back(CellArc::COMBINATIONAL, other_port.first, delay);
            }
        }
    }
}

void TimingAnalyser::topo_sort()
{
    TopoSort<CellPortKey> topo;
    for (auto &port : ports) {
        auto &pd = port.second;
        // All ports are nodes
        topo.node(port.first);
        if (pd.type == PORT_IN) {
            // inputs: combinational arcs through the cell are edges
            for (auto &arc : pd.cell_arcs) {
                if (arc.type != CellArc::COMBINATIONAL)
                    continue;
                topo.edge(port.first, CellPortKey(port.first.cell, arc.other_port));
            }
        } else if (pd.type == PORT_OUT) {
            // output: routing arcs are edges
            const NetInfo *pn = port_info(port.first).net;
            if (pn != nullptr) {
                for (auto &usr : pn->users)
                    topo.edge(port.first, CellPortKey(usr));
            }
        }
    }

    bool ignore_loops = bool_or_default(ctx->settings, ctx->id("timing/ignoreLoops"), false);
    bool no_loops = topo.sort();
    if (!no_loops && !ignore_loops) {
        log_info("Found %d combinational loops:\n", int(topo.loops.size()));
        int i = 0;
        for (auto &loop : topo.loops) {
            log_info("    loop %d:\n", ++i);
            for (auto &port : loop) {
                log_info("        %s.%s (%s)\n", ctx->nameOf(port.cell), ctx->nameOf(port.port),
                         ctx->nameOf(port_info(port).net));
            }
        }

        if (ctx->force)
            log_warning("Timing analysis failed due to combinational loops.\n");
        else
            log_error("Timing analysis failed due to combinational loops.\n");
    }
    have_loops = !no_loops;
    std::swap(topological_order, topo.sorted);
}

void TimingAnalyser::setup_port_domains()
{
    for (auto &d : domains) {
        d.startpoints.clear();
        d.endpoints.clear();
    }
    bool first_iter = true;
    do {
        // Go forward through the topological order (domains from the PoV of arrival time)
        updated_domains = false;
        for (auto port : topological_order) {
            auto &pd = ports.at(port);
            auto &pi = port_info(port);
            if (pi.type == PORT_OUT) {
                if (first_iter) {
                    for (auto &fanin : pd.cell_arcs) {
                        domain_id_t dom;
                        // registered outputs are startpoints
                        if (fanin.type == CellArc::CLK_TO_Q)
                            dom = domain_id(port.cell, fanin.other_port, fanin.edge);
                        else if (fanin.type == CellArc::STARTPOINT)
                            dom = async_clock_id;
                        else
                            continue;
                        // create per-domain data
                        pd.arrival[dom];
                        domains.at(dom).startpoints.emplace_back(port, fanin.other_port);
                    }
                }
                // copy domains across routing
                if (pi.net != nullptr)
                    for (auto &usr : pi.net->users)
                        copy_domains(port, CellPortKey(usr), false);
            } else {
                // copy domains from input to output
                for (auto &fanout : pd.cell_arcs) {
                    if (fanout.type != CellArc::COMBINATIONAL)
                        continue;
                    copy_domains(port, CellPortKey(port.cell, fanout.other_port), false);
                }
            }
        }
        // Go backward through the topological order (domains from the PoV of required time)
        for (auto port : reversed_range(topological_order)) {
            auto &pd = ports.at(port);
            auto &pi = port_info(port);
            
            if (pi.type == PORT_OUT) {
                // copy domains from output to input
                for (auto &fanin : pd.cell_arcs) {
                    if (fanin.type != CellArc::COMBINATIONAL)
                        continue;
                    copy_domains(port, CellPortKey(port.cell, fanin.other_port), true);
                }
            } else {
                if (first_iter) {
                    for (auto &fanout : pd.cell_arcs) {
                        domain_id_t dom;
                        // registered inputs are endpoints
                        // TODO: 这里是否要判断HOLD？
                        if (fanout.type == CellArc::SETUP)
                            dom = domain_id(port.cell, fanout.other_port, fanout.edge);
                        else if (fanout.type == CellArc::ENDPOINT)
                            dom = async_clock_id;
                        else
                            continue;
                        // create per-domain data
                        pd.required[dom];
                        domains.at(dom).endpoints.emplace_back(port, fanout.other_port);
                    }
                }
                // copy port to driver
                if (pi.net != nullptr && pi.net->driver.cell != nullptr)
                    copy_domains(port, CellPortKey(pi.net->driver), true);
            }
        }
        // Iterate over ports and find domain pairs
        for (auto port : topological_order) {
            auto &pd = ports.at(port);
            for (auto &arr : pd.arrival)
                for (auto &req : pd.required) {
                    pd.domain_pairs[domain_pair_id(arr.first, req.first)];
                }
        }
        first_iter = false;
        // If there are loops, repeat the process until a fixed point is reached, as there might be unusual ways to
        // visit points, which would result in a missing domain key and therefore crash later on
    } while (have_loops && updated_domains);
    for (auto &dp : domain_pairs) {
        auto &launch_data = domains.at(dp.key.launch);
        auto &capture_data = domains.at(dp.key.capture);
        if (launch_data.key.clock != capture_data.key.clock)
            continue;
        IdString clk = launch_data.key.clock;
        delay_t period = ctx->getDelayFromNS(1.0e9 / ctx->setting<float>("target_freq"));
        if (ctx->nets.count(clk)) {
            NetInfo *clk_net = ctx->nets.at(clk).get();
            if (clk_net->clkconstr) {
                period = clk_net->clkconstr->period.minDelay();
            }
        }
        if (launch_data.key.edge != capture_data.key.edge)
            period /= 2;
        dp.period = DelayPair(period);
    }
}

void TimingAnalyser::identify_related_domains()
{

    // Identify clock nets
    pool<IdString> clock_nets;
    for (const auto &domain : domains) {
        clock_nets.insert(domain.key.clock);
    }

    // For each clock net identify all nets that can possibly drive it. Compute
    // cumulative delays to each of them.
    std::function<void(const NetInfo *, pool<IdString> &, dict<IdString, delay_t> &, delay_t)> find_net_drivers =
            [&](const NetInfo *ni, pool<IdString> &net_trace, dict<IdString, delay_t> &drivers, delay_t delay_acc) {
                // Get driving cell and port
                if (ni == nullptr)
                    return;
                const CellInfo *cell = ni->driver.cell;
                if (cell == nullptr)
                    return;

                const IdString port = ni->driver.port;

                bool didGoUpstream = false;

                // Ring oscillator driving the net
                if (net_trace.find(ni->name) != net_trace.end()) {
                    drivers[ni->name] = delay_acc;
                    return;
                }
                net_trace.insert(ni->name);

                // The cell has only one port
                if (cell->ports.size() == 1) {
                    drivers[ni->name] = delay_acc;
                    return;
                }

                // Get the driver timing class
                int info_count = 0;
                auto timing_class = ctx->getPortTimingClass(cell, port, info_count);

                // The driver must be a combinational output
                if (timing_class != TMG_COMB_OUTPUT) {
                    drivers[ni->name] = delay_acc;
                    return;
                }

                // Recurse upstream through all input ports that have combinational
                // paths to this driver
                for (const auto &it : cell->ports) {
                    const auto &pi = it.second;

                    // Only connected inputs
                    if (pi.type != PORT_IN) {
                        continue;
                    }
                    if (pi.net == nullptr) {
                        continue;
                    }

                    // The input must be a combinational input
                    timing_class = ctx->getPortTimingClass(cell, pi.name, info_count);
                    if (timing_class != TMG_COMB_INPUT) {
                        continue;
                    }
                    // There must be a combinational arc
                    DelayQuad delay;
                    if (!ctx->getCellDelay(cell, pi.name, port, delay)) {
                        continue;
                    }

                    // Recurse
                    find_net_drivers(pi.net, net_trace, drivers, delay_acc + delay.maxDelay());
                    didGoUpstream = true;
                }

                // Did not propagate upstream through the cell, mark the net as driver
                if (!didGoUpstream) {
                    drivers[ni->name] = delay_acc;
                }
            };

    // Identify possible drivers for each clock domain
    dict<IdString, dict<IdString, delay_t>> clock_drivers;
    for (const auto &domain : domains) {
        if (domain.key.is_async())
            continue;

        const NetInfo *ni = ctx->nets.at(domain.key.clock).get();
        if (ni == nullptr)
            continue;
        if (ni->driver.cell == nullptr)
            continue;

        dict<IdString, delay_t> drivers;
        pool<IdString> net_trace;
        find_net_drivers(ni, net_trace, drivers, 0);

        clock_drivers[domain.key.clock] = drivers;

        if (ctx->debug) {
            log("Clock '%s' can be driven by:\n", domain.key.clock.str(ctx).c_str());
            for (const auto &it : drivers) {
                const NetInfo *net = ctx->nets.at(it.first).get();
                log(" %s.%s delay %.3fns\n", net->driver.cell->name.str(ctx).c_str(), net->driver.port.str(ctx).c_str(),
                    ctx->getDelayNS(it.second));
            }
        }
    }

    // Identify related clocks. For simplicity do it both for A->B and B->A
    // cases.
    for (const auto &c1 : clock_drivers) {
        for (const auto &c2 : clock_drivers) {

            if (c1 == c2) {
                continue;
            }

            // Make an intersection of the two drivers sets
            pool<IdString> common_drivers;
            for (const auto &it : c1.second) {
                common_drivers.insert(it.first);
            }
            for (const auto &it : c2.second) {
                common_drivers.insert(it.first);
            }

            for (auto it = common_drivers.begin(); it != common_drivers.end();) {
                if (!c1.second.count(*it) || !c2.second.count(*it)) {
                    it = common_drivers.erase(it);
                } else {
                    ++it;
                }
            }

            if (ctx->debug) {

                log("Possible common driver(s) for clocks '%s' and '%s'\n", c1.first.str(ctx).c_str(),
                    c2.first.str(ctx).c_str());

                for (const auto &it : common_drivers) {

                    const NetInfo *ni = ctx->nets.at(it).get();
                    const CellInfo *cell = ni->driver.cell;
                    const IdString port = ni->driver.port;

                    log(" net '%s', cell %s (%s), port %s\n", it.str(ctx).c_str(), cell->name.str(ctx).c_str(),
                        cell->type.str(ctx).c_str(), port.str(ctx).c_str());
                }
            }

            // If there is no single driver then consider the two clocks
            // unrelated.
            if (common_drivers.size() != 1) {
                continue;
            }

            // Compute delay from c1 to c2 and store it
            auto driver = *common_drivers.begin();
            auto delay = c2.second.at(driver) - c1.second.at(driver);
            clock_delays[std::make_pair(c1.first, c2.first)] = delay;
        }
    }
}

void TimingAnalyser::run(bool update_route_delays, bool update_net_timings, bool update_histogram,
                         bool update_crit_paths)
{
    reset_times();
    if (update_route_delays)
        get_route_delays();
    walk_forward();
    walk_backward();
    compute_slack();
    compute_criticality();

    // Ensure we clear all timing results if any of them has been marked as
    // as to be updated. This is done so we ensure it's not possible to have
    // timing_result which contains mixed reports
    if (update_net_timings || update_histogram || update_crit_paths) {
        result = TimingResult();
    }

    if (update_net_timings) {
        build_detailed_net_timing_report();
    }

    if (update_histogram) {
        build_slack_histogram_report();
    }

    if (update_crit_paths) {
        build_crit_path_reports();
    }
}

void TimingAnalyser::reset_times()
{
    static const auto init_delay =
            DelayPair(std::numeric_limits<delay_t>::max(), std::numeric_limits<delay_t>::lowest());
    for (auto &port : ports) {
        auto do_reset = [&](dict<domain_id_t, ArrivReqTime> &times) {
            for (auto &t : times) {
                t.second.value = init_delay;
                t.second.path_length = 0;
                t.second.bwd_min = CellPortKey();
                t.second.bwd_max = CellPortKey();
            }
        };
        do_reset(port.second.arrival);
        do_reset(port.second.required);
        for (auto &dp : port.second.domain_pairs) {
            dp.second.setup_slack = std::numeric_limits<delay_t>::max();
            dp.second.hold_slack = std::numeric_limits<delay_t>::max();
            dp.second.max_path_length = 0;
            dp.second.criticality = 0;
        }
        port.second.worst_crit = 0;
        port.second.worst_setup_slack = std::numeric_limits<delay_t>::max();
        port.second.worst_hold_slack = std::numeric_limits<delay_t>::max();
    }
}

void TimingAnalyser::get_route_delays()
{
    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        if (ni->driver.cell == nullptr || ni->driver.cell->bel == BelId())
            continue;

        for (auto &usr : ni->users) {
            if (usr.cell->bel == BelId())
                continue;
            ports.at(CellPortKey(usr)).route_delay = DelayPair(ctx->getNetinfoRouteDelay(ni, usr));
        }
    }
}

void TimingAnalyser::walk_forward()
{
    // Assign initial arrival time to domain startpoints
    for (domain_id_t dom_id = 0; dom_id < domain_id_t(domains.size()); ++dom_id) {
        auto &dom = domains.at(dom_id);
        for (auto &sp : dom.startpoints) {
            auto &pd = ports.at(sp.first);
            DelayPair init_arrival(0);
            CellPortKey clock_key;
            if (sp.second != IdString()) {
                // clocked startpoints have a clock-to-out time
                for (auto &fanin : pd.cell_arcs) {
                    if (fanin.type == CellArc::CLK_TO_Q && fanin.other_port == sp.second) {
                        init_arrival += fanin.value.delayPair();
                        // Include the clock delay if clock_skew analysis is enabled
                        if (with_clock_skew) {
                            init_arrival += ports.at(CellPortKey(sp.first.cell, fanin.other_port)).route_delay;
                        }
                        break;
                    }
                }
                clock_key = CellPortKey(sp.first.cell, sp.second);
            }
            set_arrival_time(sp.first, dom_id, init_arrival, 1, clock_key);
        }
    }
    // Walk forward in topological order
    for (auto p : topological_order) {
        auto &pd = ports.at(p);
        for (auto &arr : pd.arrival) {
            if (pd.type == PORT_OUT) {
                // Output port: propagate delay through net, adding route delay
                NetInfo *net = port_info(p).net;
                if (net != nullptr)
                    for (auto &usr : net->users) {
                        CellPortKey usr_key(usr);
                        auto &usr_pd = ports.at(usr_key);
                        auto next_arr = arr.second.value + usr_pd.route_delay;
                        set_arrival_time(usr_key, arr.first, next_arr, arr.second.path_length, p);
                    }
            } else if (pd.type == PORT_IN) {
                // Input port; propagate delay through cell, adding combinational delay
                for (auto &fanout : pd.cell_arcs) {
                    if (fanout.type != CellArc::COMBINATIONAL)
                        continue;

                    auto next_arr = arr.second.value + fanout.value.delayPair();
                    set_arrival_time(CellPortKey(p.cell, fanout.other_port), arr.first, next_arr,
                                     arr.second.path_length + 1, p);
                }
            }
        }
    }
}

void TimingAnalyser::walk_backward()
{
    // Assign initial required time to domain endpoints
    // Note that clock frequency will be considered later in the analysis for, for now all required times are normalised
    // to 0ns
    for (domain_id_t dom_id = 0; dom_id < domain_id_t(domains.size()); ++dom_id) {
        auto &dom = domains.at(dom_id);
        for (auto &ep : dom.endpoints) {
            auto &pd = ports.at(ep.first);
            DelayPair init_required(0);
            CellPortKey clock_key;
            // TODO: clock routing delay, if analysis of that is enabled
            if (ep.second != IdString()) {
                // Add setup/hold time, if this endpoint is clocked
                for (auto &fanin : pd.cell_arcs) {

                    if (fanin.type == CellArc::SETUP && fanin.other_port == ep.second) {
                        if (with_clock_skew) {
                            init_required += ports.at(CellPortKey(ep.first.cell, fanin.other_port)).route_delay;
                        }
                        init_required.min_delay -= fanin.value.maxDelay();
                    }
                    if (fanin.type == CellArc::HOLD && fanin.other_port == ep.second)
                        init_required.max_delay += fanin.value.maxDelay();
                }
                clock_key = CellPortKey(ep.first.cell, ep.second);
            }
            set_required_time(ep.first, dom_id, init_required, 1, clock_key);
        }
    }
    // Walk backwards in topological order
    for (auto p : reversed_range(topological_order)) {
        auto &pd = ports.at(p);
        for (auto &req : pd.required) {
            if (pd.type == PORT_IN) {
                // Input port: propagate delay back through net, subtracting route delay
                NetInfo *net = port_info(p).net;
                if (net != nullptr && net->driver.cell != nullptr)
                    set_required_time(CellPortKey(net->driver), req.first,
                                      req.second.value - DelayPair(pd.route_delay.maxDelay()), req.second.path_length,
                                      p);
            } else if (pd.type == PORT_OUT) {
                // Output port : propagate delay back through cell, subtracting combinational delay
                for (auto &fanin : pd.cell_arcs) {
                    if (fanin.type != CellArc::COMBINATIONAL)
                        continue;
                    set_required_time(CellPortKey(p.cell, fanin.other_port), req.first,
                                      req.second.value - DelayPair(fanin.value.maxDelay()), req.second.path_length + 1,
                                      p);
                }
            }
        }
    }
}

void TimingAnalyser::compute_slack()
{
    for (auto &dp : domain_pairs) {
        dp.worst_setup_slack = std::numeric_limits<delay_t>::max();
        dp.worst_hold_slack = std::numeric_limits<delay_t>::max();
    }
    for (auto p : topological_order) {
        auto &pd = ports.at(p);
        for (auto &pdp : pd.domain_pairs) {
            auto &dp = domain_pairs.at(pdp.first);

            // Get clock names
            const auto &launch_clock = domains.at(dp.key.launch).key.clock;
            const auto &capture_clock = domains.at(dp.key.capture).key.clock;

            // Get clock-to-clock delay if any
            delay_t clock_to_clock = 0;
            auto clocks = std::make_pair(launch_clock, capture_clock);
            if (clock_delays.count(clocks)) {
                clock_to_clock = clock_delays.at(clocks);
            }

            auto &arr = pd.arrival.at(dp.key.launch);
            auto &req = pd.required.at(dp.key.capture);
            pdp.second.setup_slack = 0 - (arr.value.maxDelay() - req.value.minDelay() + clock_to_clock);
            if (!setup_only)
                pdp.second.hold_slack = arr.value.minDelay() - req.value.maxDelay() + clock_to_clock;
            pdp.second.max_path_length = arr.path_length + req.path_length;
            if (dp.key.launch == dp.key.capture)
                pd.worst_setup_slack = std::min(pd.worst_setup_slack, dp.period.minDelay() + pdp.second.setup_slack);
            dp.worst_setup_slack = std::min(dp.worst_setup_slack, pdp.second.setup_slack);
            if (!setup_only) {
                pd.worst_hold_slack = std::min(pd.worst_hold_slack, pdp.second.hold_slack);
                dp.worst_hold_slack = std::min(dp.worst_hold_slack, pdp.second.hold_slack);
            }
        }
    }
}

void TimingAnalyser::compute_criticality()
{
    for (auto p : topological_order) {
        auto &pd = ports.at(p);
        for (auto &pdp : pd.domain_pairs) {
            auto &dp = domain_pairs.at(pdp.first);
            // Do not set criticality for asynchronous paths
            if (domains.at(dp.key.launch).key.is_async() || domains.at(dp.key.capture).key.is_async())
                continue;

            float crit =
                    1.0f - (float(pdp.second.setup_slack) - float(dp.worst_setup_slack)) / float(-dp.worst_setup_slack);
            crit = std::min(crit, 1.0f);
            crit = std::max(crit, 0.0f);
            pdp.second.criticality = crit;
            pd.worst_crit = std::max(pd.worst_crit, crit);
        }
    }
}

void TimingAnalyser::build_detailed_net_timing_report()
{
    auto &net_timings = result.detailed_net_timings;

    for (domain_id_t dom_id = 0; dom_id < domain_id_t(domains.size()); ++dom_id) {
        auto &dom = domains.at(dom_id);
        for (auto &ep : dom.endpoints) {
            auto &pd = ports.at(ep.first);
            const NetInfo *net = port_info(ep.first).net;

            for (auto &arr : pd.arrival) {
                auto &launch = domains.at(arr.first).key;
                for (auto &req : pd.required) {
                    auto &capture = domains.at(req.first).key;

                    NetSinkTiming sink_timing;
                    sink_timing.clock_pair.start.clock = launch.clock;
                    sink_timing.clock_pair.start.edge = launch.edge;
                    sink_timing.clock_pair.end.clock = capture.clock;
                    sink_timing.clock_pair.end.edge = capture.edge;
                    sink_timing.cell_port = std::make_pair(pd.cell_port.cell, pd.cell_port.port);
                    sink_timing.delay = arr.second.value;

                    net_timings[net->name].push_back(sink_timing);
                }
            }
        }
    }
}

void TimingAnalyser::build_slack_histogram_report()
{
    auto &slack_histogram = result.slack_histogram;

    for (domain_id_t dom_id = 0; dom_id < domain_id_t(domains.size()); ++dom_id) {
        for (auto &ep : domains.at(dom_id).endpoints) {
            auto &pd = ports.at(ep.first);

            for (auto &req : pd.required) {
                auto &capture = domains.at(req.first).key;
                for (auto &arr : pd.arrival) {
                    auto &launch = domains.at(arr.first).key;

                    if (launch.clock != capture.clock || launch.is_async())
                        continue;

                    float clk_period = ctx->getDelayFromNS(1.0e9 / ctx->setting<float>("target_freq"));
                    if (ctx->nets.at(launch.clock)->clkconstr)
                        clk_period = ctx->nets.at(launch.clock)->clkconstr->period.minDelay();

                    if (launch.edge != capture.edge)
                        clk_period = clk_period / 2;

                    delay_t delay = arr.second.value.maxDelay() - req.second.value.minDelay();
                    delay_t slack = clk_period - delay;

                    int slack_ps = ctx->getDelayNS(slack) * 1000;
                    slack_histogram[slack_ps]++;
                }
            }
        }
    }
}

std::vector<CellPortKey> TimingAnalyser::get_eps(domain_id_t domain_pair)
{
    std::vector<CellPortKey> eps;
    // delay_t last_slack = std::numeric_limits<delay_t>::lowest();
    auto &dp = domain_pairs.at(domain_pair);
    auto &cap_d = domains.at(dp.key.capture);
    for (auto ep : cap_d.endpoints) {
        auto &pd = ports.at(ep.first);
        if (!pd.domain_pairs.count(domain_pair))
            continue;
        eps.push_back(ep.first);
    }
    return eps;
}

void TimingAnalyser::build_crit_path_reports()
{
    auto &clock_reports = result.clock_paths;
    auto &xclock_reports = result.xclock_paths;
    auto &clock_fmax = result.clock_fmax;
    auto &empty_clocks = result.empty_paths;

    auto &all_signle_clock_setup_analysis = result.clock_paths_setup;
    auto &all_signle_clock_hold_analysis = result.clock_paths_hold;

    if (!setup_only) {
        result.min_delay_violations = get_min_delay_violations();
    }

    auto delay_by_domain = max_delay_by_domain_pairs();

    for (int i = 0; i < int(domains.size()); i++) {
        empty_clocks.insert(domains.at(i).key.clock);
    }

    for (int i = 0; i < int(domain_pairs.size()); i++) {
        auto &dp = domain_pairs.at(i);
        auto &launch = domains.at(dp.key.launch).key;
        auto &capture = domains.at(dp.key.capture).key;

        if (launch.clock != capture.clock || launch.is_async())
            continue;

        auto path_delay = delay_by_domain.at(i);

        double Fmax;

        if (launch.edge == capture.edge)
            Fmax = 1000 / ctx->getDelayNS(path_delay);
        else
            Fmax = 500 / ctx->getDelayNS(path_delay);

        if (!clock_fmax.count(launch.clock) || Fmax < clock_fmax.at(launch.clock).achieved) {
            float target = ctx->setting<float>("target_freq") / 1e6;
            if (ctx->nets.at(launch.clock)->clkconstr)
                target = 1000 / ctx->getDelayNS(ctx->nets.at(launch.clock)->clkconstr->period.minDelay());

             // 获取所有path，计算setup和hold
            auto endpoints = get_eps(i);
            if (endpoints.empty())
                continue;
            for (int j = 0; j < endpoints.size(); j++)  // for setup analysis
                all_signle_clock_setup_analysis[launch.clock].push_back(build_critical_path_report(i, endpoints.at(j), true));
            for (int j = 0; j < endpoints.size(); j++)  // for hold analysis
                all_signle_clock_hold_analysis[launch.clock].push_back(build_critical_path_report(i, endpoints.at(j), false));


            auto worst_endpoint = get_worst_eps(i, 1);
            if (worst_endpoint.empty())
                continue;

            clock_fmax[launch.clock].achieved = Fmax;
            clock_fmax[launch.clock].constraint = target;

            clock_reports[launch.clock] = build_critical_path_report(i, worst_endpoint.at(0), true);

            empty_clocks.erase(launch.clock);
        }
    }

    for (int i = 0; i < int(domain_pairs.size()); i++) {
        auto &dp = domain_pairs.at(i);
        auto &launch = domains.at(dp.key.launch).key;
        auto &capture = domains.at(dp.key.capture).key;

        if (launch.clock == capture.clock && !launch.is_async())
            continue;
        
        auto worst_endpoint = get_worst_eps(i, 1);
        if (worst_endpoint.empty())
            continue;

        xclock_reports.emplace_back(build_critical_path_report(i, worst_endpoint.at(0), true));
    }

    auto cmp_crit_path = [&](const CriticalPath &ra, const CriticalPath &rb) {
        const auto &a = ra.clock_pair;
        const auto &b = rb.clock_pair;

        if (a.start.clock.str(ctx) < b.start.clock.str(ctx))
            return true;
        if (a.start.clock.str(ctx) > b.start.clock.str(ctx))
            return false;
        if (a.start.edge < b.start.edge)
            return true;
        if (a.start.edge > b.start.edge)
            return false;
        if (a.end.clock.str(ctx) < b.end.clock.str(ctx))
            return true;
        if (a.end.clock.str(ctx) > b.end.clock.str(ctx))
            return false;
        if (a.end.edge < b.end.edge)
            return true;
        return false;
    };

    std::sort(xclock_reports.begin(), xclock_reports.end(), cmp_crit_path);
}

dict<domain_id_t, delay_t> TimingAnalyser::max_delay_by_domain_pairs()
{
    dict<domain_id_t, delay_t> domain_delay;

    for (domain_id_t capture_id = 0; capture_id < domain_id_t(domains.size()); ++capture_id) {
        const auto &capture = domains.at(capture_id);

        for (auto &ep : capture.endpoints) {
            auto &ep_port = ports.at(ep.first);

            auto &req = ep_port.required.at(capture_id);

            for (auto &[launch_id, arr] : ep_port.arrival) {
                const auto &launch = domains.at(capture_id);

                auto dp = domain_pair_id(launch_id, capture_id);

                auto clocks = std::make_pair(launch.key.clock, capture.key.clock);
                auto same_clock = capture_id == launch_id;
                auto related_clocks = clock_delays.count(clocks) > 0;
                delay_t clock_to_clock = 0;
                if (related_clocks) {
                    clock_to_clock = clock_delays.at(clocks);
                }

                auto delay = arr.value.maxDelay() - req.value.minDelay() + clock_to_clock;

                // If domains are unrelated or not the same clock we need to make sure
                // to remove the clock delays from the arrival and required times
                // because the delays have no common reference.
                if (with_clock_skew && !same_clock && !related_clocks) {
                    for (auto &fanin : ep_port.cell_arcs) {
                        if (fanin.type == CellArc::SETUP) {
                            auto clock_delay = ports.at(CellPortKey(ep.first.cell, fanin.other_port)).route_delay;
                            delay += clock_delay.minDelay();
                        }
                    }

                    // walk back to startpoint
                    auto crit_path = walk_crit_path(domain_pair_id(launch_id, capture_id), ep.first, true);
                    auto first_inp = crit_path.back();
                    const auto &sp = first_inp.cell->ports.at(first_inp.port).net->driver;
                    auto &sp_port = ports.at(CellPortKey{sp.cell->name, sp.port});

                    for (auto &fanin : sp_port.cell_arcs) {
                        if (fanin.type == CellArc::CLK_TO_Q) {
                            auto clock_delay = ports.at(CellPortKey(sp.cell->name, fanin.other_port)).route_delay;
                            delay -= clock_delay.maxDelay();
                        }
                    }
                }

                if (!domain_delay.count(dp) || domain_delay.at(dp) < delay) {
                    domain_delay[dp] = delay;
                }
            }
        }
    }

    return domain_delay;
}

std::vector<CellPortKey> TimingAnalyser::get_worst_eps(domain_id_t domain_pair, int count)
{
    std::vector<CellPortKey> worst_eps;
    delay_t last_slack = std::numeric_limits<delay_t>::lowest();
    auto &dp = domain_pairs.at(domain_pair);
    auto &cap_d = domains.at(dp.key.capture);
    while (int(worst_eps.size()) < count) {
        CellPortKey next;
        delay_t next_slack = std::numeric_limits<delay_t>::max();
        for (auto ep : cap_d.endpoints) {
            auto &pd = ports.at(ep.first);
            if (!pd.domain_pairs.count(domain_pair))
                continue;
            delay_t ep_slack = pd.domain_pairs.at(domain_pair).setup_slack;
            if (ep_slack < next_slack && ep_slack > last_slack) {
                next = ep.first;
                next_slack = ep_slack;
            }
        }
        if (next == CellPortKey())
            break;
        worst_eps.push_back(next);
        last_slack = next_slack;
    }
    return worst_eps;
}

// 返回cell type的timing index
int get_cell_timing_idx(IdString type_variant)
{
    // return db_binary_search(
    //         speed_grade->cell_types, [](const CellTimingPOD &ct) { return ct.type_variant; }, type_variant.index);
    return 0;
}

static std::string clock_event_name(const Context *ctx, const ClockEvent &e, int field_width = 0)
{
    std::string value;
    if (e.clock == IdString() || e.clock == ctx->id("$async$"))
        value = std::string("<async>");
    else
        value = (e.edge == FALLING_EDGE ? std::string("negedge ") : std::string("posedge ")) + e.clock.str(ctx);
    if (int(value.length()) < field_width)
        value.insert(value.length(), field_width - int(value.length()), ' ');
    return value;
};

static json11::Json::array json_report_critical_paths(const Context *ctx)
{

    auto report_critical_path = [ctx](const CriticalPath &report) {
        json11::Json::array pathJson;

        for (const auto &segment : report.segments) {

            const auto &driver = ctx->cells.at(segment.from.first);
            const auto &sink = ctx->cells.at(segment.to.first);

            auto fromLoc = ctx->getBelLocation(driver->bel);
            auto toLoc = ctx->getBelLocation(sink->bel);

            auto fromJson = json11::Json::object({{"cell", segment.from.first.c_str(ctx)},
                                          {"port", segment.from.second.c_str(ctx)},
                                          {"loc", json11::Json::array({fromLoc.x, fromLoc.y})}});

            auto toJson = json11::Json::object({{"cell", segment.to.first.c_str(ctx)},
                                        {"port", segment.to.second.c_str(ctx)},
                                        {"loc", json11::Json::array({toLoc.x, toLoc.y})}});

            auto segmentJson = json11::Json::object({
                    {"delay", ctx->getDelayNS(segment.delay)},
                    {"from", fromJson},
                    {"to", toJson},
            });

            segmentJson["type"] = CriticalPath::Segment::type_to_str(segment.type);
            if (segment.type == CriticalPath::Segment::Type::ROUTING) {
                segmentJson["net"] = segment.net.c_str(ctx);
            }

            pathJson.push_back(segmentJson);
        }

        return pathJson;
    };

    auto critPathsJson = json11::Json::array();

    // Critical paths
    for (auto &report : ctx->timing_result.clock_paths) {

        critPathsJson.push_back(json11::Json::object({{"from", clock_event_name(ctx, report.second.clock_pair.start)},
                                              {"to", clock_event_name(ctx, report.second.clock_pair.end)},
                                              {"path", report_critical_path(report.second)}}));
    }

    // Cross-domain paths
    for (auto &report : ctx->timing_result.xclock_paths) {
        critPathsJson.push_back(json11::Json::object({{"from", clock_event_name(ctx, report.clock_pair.start)},
                                              {"to", clock_event_name(ctx, report.clock_pair.end)},
                                              {"path", report_critical_path(report)}}));
    }

    return critPathsJson;
}

static json11::Json::array json_report_timing_paths(const Context *ctx, bool is_setup)
{

    auto report_timing_path = [ctx](const CriticalPath &report, const bool is_single_domain_, const bool is_setup_) {
        json11::Json::array pathJson;

        for (const auto &segment : report.segments) {

            const auto &driver = ctx->cells.at(segment.from.first);
            const auto &sink = ctx->cells.at(segment.to.first);

            auto fromLoc = ctx->getBelLocation(driver->bel);
            auto toLoc = ctx->getBelLocation(sink->bel);

            auto fromJson = json11::Json::object({{"cell", segment.from.first.c_str(ctx)},
                                          {"port", segment.from.second.c_str(ctx)},
                                          {"loc", json11::Json::array({fromLoc.x, fromLoc.y})}});

            auto toJson = json11::Json::object({{"cell", segment.to.first.c_str(ctx)},
                                        {"port", segment.to.second.c_str(ctx)},
                                        {"loc", json11::Json::array({toLoc.x, toLoc.y})}});

            auto segmentJson = json11::Json::object({
                    {"delay", ctx->getDelayNS(segment.delay)},
                    {"from", fromJson},
                    {"to", toJson},
            });

            segmentJson["type"] = CriticalPath::Segment::type_to_str(segment.type);
            if (segment.type == CriticalPath::Segment::Type::ROUTING) {
                segmentJson["net"] = segment.net.c_str(ctx);
            }

            pathJson.push_back(segmentJson);
        }

        return pathJson;
    };

    auto timingPathsJson = json11::Json::array();

    // Timing paths
    if(is_setup)
    {
        for (auto &report : ctx->timing_result.clock_paths_setup)
            for (auto &timing_path : report.second)
                timingPathsJson.push_back(json11::Json::object({{"from", clock_event_name(ctx, timing_path.clock_pair.start)},
                                                      {"to", clock_event_name(ctx, timing_path.clock_pair.end)},
                                                      {"path", report_timing_path(timing_path, true, true)},
                                                      {"slack", timing_path.slack}}));

        // // Cross-domain timing paths
        // for (auto &report : ctx->timing_result.xclock_paths_recovery)
        //     timingPathsJson.push_back(json11::Json::object({{"from", clock_event_name(ctx, report.clock_pair.start)},
        //                                           {"to", clock_event_name(ctx, report.clock_pair.end)},
        //                                           {"path", report_timing_path(report, false, false)}}));
    }
    else
    {
        for (auto &report : ctx->timing_result.clock_paths_hold)
            for (auto &timing_path : report.second)
                timingPathsJson.push_back(json11::Json::object({{"from", clock_event_name(ctx, timing_path.clock_pair.start)},
                                                      {"to", clock_event_name(ctx, timing_path.clock_pair.end)},
                                                      {"path", report_timing_path(timing_path, true, false)},
                                                      {"slack", timing_path.slack}}));

        // // Cross-domain timing paths
        // for (auto &report : ctx->timing_result.xclock_paths_removal)
        //     timingPathsJson.push_back(json11::Json::object({{"from", clock_event_name(ctx, report.clock_pair.start)},
        //                                           {"to", clock_event_name(ctx, report.clock_pair.end)},
        //                                           {"path", report_timing_path(report, false, false)}}));
    }


    return timingPathsJson;
}

void timing_analysis(Context *ctx, bool print_slack_histogram, bool print_fmax, bool print_path, bool warn_on_failure, bool update_results)
{
    TimingAnalyser tmg(ctx);
    tmg.setup_only = false;
    tmg.with_clock_skew = false;//TODO: 暂时设置为false
    tmg.setup(ctx->detailed_timing_report, print_slack_histogram, print_path || print_fmax);

    auto &result = tmg.get_timing_result();
    ctx->log_timing_results(result, print_slack_histogram, print_fmax, print_path, warn_on_failure);

    if (update_results)
        ctx->timing_result = result;

    
    // 导出timing result
    auto timing_result = str_or_default(ctx->settings, ctx->id("timing_result"), "NONE");
    if (timing_result != "NONE") {
        std::string filename = timing_result;
        std::ofstream f(filename);
        auto result = json11::Json::object();
        result["setup"] = json_report_timing_paths(ctx, true);
        result["hold"] = json_report_timing_paths(ctx, false);
        f<< json11::Json(result).dump();
        f.close();
    }
}



static void log_crit_paths(const Context *ctx, TimingResult &result)
{
    static auto print_net_source = [ctx](const NetInfo *net) {
        // Check if this net is annotated with a source list
        auto sources = net->attrs.find(ctx->id("src"));
        if (sources == net->attrs.end()) {
            // No sources for this net, can't print anything
            return;
        }

        // Sources are separated by pipe characters.
        // There is no guaranteed ordering on sources, so we just print all
        auto sourcelist = sources->second.as_string();
        std::vector<std::string> source_entries;
        size_t current = 0, prev = 0;
        while ((current = sourcelist.find("|", prev)) != std::string::npos) {
            source_entries.emplace_back(sourcelist.substr(prev, current - prev));
            prev = current + 1;
        }
        // Ensure we emplace the final entry
        source_entries.emplace_back(sourcelist.substr(prev, current - prev));

        // Iterate and print our source list at the correct indentation level
        log_info("                         Defined in:\n");
        for (auto entry : source_entries) {
            log_info("                              %s\n", entry.c_str());
        }
    };

    // A helper function for reporting one critical path
    auto print_path_report = [ctx](const CriticalPath &path) {
        delay_t total(0), logic_total(0), route_total(0);

        log_info("      type curr  total name\n");
        for (const auto &segment : path.segments) {

            delay_t delay = segment.delay;

            total += delay;

            if (segment.type == CriticalPath::Segment::Type::CLK_TO_Q ||
                segment.type == CriticalPath::Segment::Type::SOURCE ||
                segment.type == CriticalPath::Segment::Type::LOGIC ||
                segment.type == CriticalPath::Segment::Type::SETUP ||
                segment.type == CriticalPath::Segment::Type::HOLD) {
                logic_total += delay;

                log_info("%10s % 5.2f % 5.2f Source %s.%s\n", CriticalPath::Segment::type_to_str(segment.type).c_str(),
                         ctx->getDelayNS(delay), ctx->getDelayNS(total), segment.to.first.c_str(ctx),
                         segment.to.second.c_str(ctx));
            } else if (segment.type == CriticalPath::Segment::Type::ROUTING ||
                       segment.type == CriticalPath::Segment::Type::CLK_TO_CLK ||
                       segment.type == CriticalPath::Segment::Type::CLK_SKEW) {
                route_total = route_total + delay;

                const auto &driver = ctx->cells.at(segment.from.first);
                const auto &sink = ctx->cells.at(segment.to.first);

                auto driver_loc = ctx->getBelLocation(driver->bel);
                auto sink_loc = ctx->getBelLocation(sink->bel);

                log_info("%10s % 5.2f % 5.2f Net %s (%d,%d) -> (%d,%d)\n",
                         CriticalPath::Segment::type_to_str(segment.type).c_str(), ctx->getDelayNS(delay),
                         ctx->getDelayNS(total), segment.net.c_str(ctx), driver_loc.x, driver_loc.y, sink_loc.x,
                         sink_loc.y);
                log_info("                         Sink %s.%s\n", segment.to.first.c_str(ctx),
                         segment.to.second.c_str(ctx));

                // CLK_TO_CLK has no net and CLK_SKEW might have a net
                if (ctx->nets.count(segment.net) == 0) {
                    continue;
                }
                const NetInfo *net = ctx->nets.at(segment.net).get();

                if (ctx->verbose) {

                    PortRef sink_ref;
                    sink_ref.cell = sink.get();
                    sink_ref.port = segment.to.second;

                    auto driver_wire = ctx->getNetinfoSourceWire(net);
                    auto sink_wire = ctx->getNetinfoSinkWire(net, sink_ref); // TODO: 原方法第三个参数 , 0
                    log_info("                          prediction: %f ns estimate: %f ns\n",
                             ctx->getDelayNS(ctx->predictArcDelay(net, sink_ref)),
                             ctx->getDelayNS(ctx->estimateDelay(driver_wire, sink_wire)));
                    auto cursor = sink_wire;
                    delay_t delay;
                    while (driver_wire != cursor) {
                        auto it = net->wires.find(cursor);
                        assert(it != net->wires.end());
                        auto pip = it->second.pip;
                        NPNR_ASSERT(pip != PipId());
                        delay = ctx->getPipDelay(pip).maxDelay();
                        log_info("                 %1.3f %s\n", ctx->getDelayNS(delay), ctx->nameOfPip(pip));
                        cursor = ctx->getPipSrcWire(pip);
                    }
                }

                if (!ctx->disable_critical_path_source_print) {
                    print_net_source(net);
                }
            }
        }
        log_info("%.2f ns logic, %.2f ns routing\n", ctx->getDelayNS(logic_total), ctx->getDelayNS(route_total));
    };

    // Single domain paths
    for (auto &clock : result.clock_paths) {
        log_break();
        std::string start =
                clock.second.clock_pair.start.edge == FALLING_EDGE ? std::string("negedge") : std::string("posedge");
        std::string end =
                clock.second.clock_pair.end.edge == FALLING_EDGE ? std::string("negedge") : std::string("posedge");
        log_info("Critical path report for clock '%s' (%s -> %s):\n", clock.first.c_str(ctx), start.c_str(),
                 end.c_str());
        auto &report = clock.second;
        print_path_report(report);
    }

    // Cross-domain paths
    for (auto &report : result.xclock_paths) {
        log_break();
        std::string start = clock_event_name(ctx, report.clock_pair.start);
        std::string end = clock_event_name(ctx, report.clock_pair.end);
        log_info("Critical path report for cross-domain path '%s' -> '%s':\n", start.c_str(), end.c_str());
        print_path_report(report);
    }

    // Min delay violated paths
    // Show maximum of 10
    auto num_min_violations = result.min_delay_violations.size();
    bool allow_fail = bool_or_default(ctx->settings, ctx->id("timing/allowFail"), false);
    if (num_min_violations > 0) {
        log_break();
        log_info("%zu Hold/min time violations (showing 10 worst paths):\n", num_min_violations);
        for (size_t i = 0; i < std::min((size_t)10, num_min_violations); ++i) {
            auto &report = result.min_delay_violations.at(i);
            log_break();
            std::string start = clock_event_name(ctx, report.clock_pair.start);
            std::string end = clock_event_name(ctx, report.clock_pair.end);

            std::string message;
            if (report.clock_pair.start == report.clock_pair.end) {
                message = "Hold/min time violation for clock '" + start + "':\n";
            } else {
                message = "Hold/min time violation for path '" + start + "' -> '" + end + "':\n";
            }

            if (allow_fail) {
                log_warning("%s", message.c_str());
            } else {
                log_nonfatal_error("%s", message.c_str());
            }

            print_path_report(report);
        }
    }
}

static void log_fmax(Context *ctx, TimingResult &result, bool warn_on_failure)
{
    log_break();

    bool allow_fail = bool_or_default(ctx->settings, ctx->id("timing/allowFail"), false);

    if (result.clock_paths.empty() && result.clock_paths.empty()) {
        log_info("No Fmax available; no interior timing paths found in design.\n");
        return;
    }

    unsigned max_width = 0;
    for (auto &clock : result.clock_paths)
        max_width = std::max<unsigned>(max_width, clock.first.str(ctx).size());

    for (auto &clock : result.clock_paths) {
        const auto &clock_name = clock.first.str(ctx);
        const int width = max_width - clock_name.size();

        float fmax = result.clock_fmax[clock.first].achieved;
        float target = result.clock_fmax[clock.first].constraint;
        bool passed = target < fmax;

        if (!warn_on_failure || passed)
            log_info("Max frequency for clock %*s'%s': %.02f MHz (%s at %.02f MHz)\n", width, "", clock_name.c_str(),
                     fmax, passed ? "PASS" : "FAIL", target);
        else if (allow_fail)
            log_warning("Max frequency for clock %*s'%s': %.02f MHz (%s at %.02f MHz)\n", width, "", clock_name.c_str(),
                        fmax, passed ? "PASS" : "FAIL", target);
        else
            log_nonfatal_error("Max frequency for clock %*s'%s': %.02f MHz (%s at %.02f MHz)\n", width, "",
                               clock_name.c_str(), fmax, passed ? "PASS" : "FAIL", target);
    }
    log_break();

    // Clock to clock delays for xpaths
    dict<ClockPair, delay_t> xclock_delays;
    for (auto &report : result.xclock_paths) {
        // Check if this path has a clock-2-clock delay
        // clock-2-clock delays are always the first segment in the path
        // But we walk the entire path anyway.
        bool has_clock_to_clock = false;
        delay_t clock_delay = 0;
        for (const auto &seg : report.segments) {
            if (seg.type == CriticalPath::Segment::Type::CLK_TO_CLK) {
                has_clock_to_clock = true;
                clock_delay += seg.delay;
            }
        }

        if (has_clock_to_clock) {
            xclock_delays[report.clock_pair] = clock_delay;
        }
    }

    unsigned max_width_xca = 0;
    unsigned max_width_xcb = 0;
    for (auto &report : result.xclock_paths) {
        max_width_xca = std::max((unsigned)clock_event_name(ctx, report.clock_pair.start).length(), max_width_xca);
        max_width_xcb = std::max((unsigned)clock_event_name(ctx, report.clock_pair.end).length(), max_width_xcb);
    }

    // Check and report xpath delays for related clocks
    if (!result.xclock_paths.empty()) {
        for (auto &report : result.xclock_paths) {
            const auto &clock_a = report.clock_pair.start.clock;
            const auto &clock_b = report.clock_pair.end.clock;

            if (!xclock_delays.count(report.clock_pair)) {
                continue;
            }

            delay_t path_delay = 0;
            for (const auto &segment : report.segments) {
                path_delay += segment.delay;
            }

            // Compensate path delay for clock-to-clock delay. If the
            // result is negative then only the latter matters. Otherwise
            // the compensated path delay is taken.
            auto clock_delay = xclock_delays.at(report.clock_pair);

            float fmax = std::numeric_limits<float>::infinity();
            if (path_delay < 0) {
                fmax = 1e3f / ctx->getDelayNS(clock_delay);
            } else if (path_delay > 0) {
                fmax = 1e3f / ctx->getDelayNS(path_delay);
            }

            // Both clocks are related so they should have the same
            // frequency. However, they may get different constraints from
            // user input. In case of only one constraint preset take it,
            // otherwise get the worst case (min.)
            float target;
            auto &clock_fmax = result.clock_fmax;
            if (clock_fmax.count(clock_a) && !clock_fmax.count(clock_b)) {
                target = clock_fmax.at(clock_a).constraint;
            } else if (!clock_fmax.count(clock_a) && clock_fmax.count(clock_b)) {
                target = clock_fmax.at(clock_b).constraint;
            } else {
                target = std::min(clock_fmax.at(clock_a).constraint, clock_fmax.at(clock_b).constraint);
            }

            bool passed = target < fmax;

            auto ev_a = clock_event_name(ctx, report.clock_pair.start, max_width_xca);
            auto ev_b = clock_event_name(ctx, report.clock_pair.end, max_width_xcb);

            if (!warn_on_failure || passed)
                log_info("Max frequency for %s -> %s: %.02f MHz (%s at %.02f MHz)\n", ev_a.c_str(), ev_b.c_str(), fmax,
                         passed ? "PASS" : "FAIL", target);
            else if (allow_fail || bool_or_default(ctx->settings, ctx->id("timing/ignoreRelClk"), false))
                log_warning("Max frequency for  %s -> %s: %.02f MHz (%s at %.02f MHz)\n", ev_a.c_str(), ev_b.c_str(),
                            fmax, passed ? "PASS" : "FAIL", target);
            else
                log_nonfatal_error("Max frequency for %s -> %s: %.02f MHz (%s at %.02f MHz)\n", ev_a.c_str(),
                                   ev_b.c_str(), fmax, passed ? "PASS" : "FAIL", target);
        }
        log_break();
    }

    // Report clock delays for xpaths
    if (!xclock_delays.empty()) {
        for (auto &pair : xclock_delays) {
            auto ev_a = clock_event_name(ctx, pair.first.start, max_width_xca);
            auto ev_b = clock_event_name(ctx, pair.first.end, max_width_xcb);

            delay_t delay = pair.second;
            if (pair.first.start.edge != pair.first.end.edge) {
                delay /= 2;
            }

            log_info("Clock to clock delay %s -> %s: %0.02f ns\n", ev_a.c_str(), ev_b.c_str(), ctx->getDelayNS(delay));
        }

        log_break();
    }

    for (auto &eclock : result.empty_paths) {
        if (eclock != IdString())
            log_info("Clock '%s' has no interior paths\n", eclock.c_str(ctx));
    }
    log_break();

    int start_field_width = 0, end_field_width = 0;
    for (auto &report : result.xclock_paths) {
        start_field_width = std::max((int)clock_event_name(ctx, report.clock_pair.start).length(), start_field_width);
        end_field_width = std::max((int)clock_event_name(ctx, report.clock_pair.end).length(), end_field_width);
    }

    for (auto &report : result.xclock_paths) {
        const ClockEvent &a = report.clock_pair.start;
        const ClockEvent &b = report.clock_pair.end;
        delay_t path_delay = 0;
        for (const auto &segment : report.segments) {
            path_delay += segment.delay;
        }
        auto ev_a = clock_event_name(ctx, a, start_field_width), ev_b = clock_event_name(ctx, b, end_field_width);
        log_info("Max delay %s -> %s: %0.02f ns\n", ev_a.c_str(), ev_b.c_str(), ctx->getDelayNS(path_delay));
    }
    log_break();
}

static void log_timing_paths(const Context *ctx, TimingResult &result)
{
    static auto print_net_source = [ctx](const NetInfo *net) {
        // Check if this net is annotated with a source list
        auto sources = net->attrs.find(ctx->id("src"));
        if (sources == net->attrs.end()) {
            // No sources for this net, can't print anything
            return;
        }

        // Sources are separated by pipe characters.
        // There is no guaranteed ordering on sources, so we just print all
        auto sourcelist = sources->second.as_string();
        std::vector<std::string> source_entries;
        size_t current = 0, prev = 0;
        while ((current = sourcelist.find("|", prev)) != std::string::npos) {
            source_entries.emplace_back(sourcelist.substr(prev, current - prev));
            prev = current + 1;
        }
        // Ensure we emplace the final entry
        source_entries.emplace_back(sourcelist.substr(prev, current - prev));

        // Iterate and print our source list at the correct indentation level
        log_info("                         Defined in:\n");
        for (auto entry : source_entries) {
            log_info("                              %s\n", entry.c_str());
        }
    };

    // A helper function for reporting one critical path
    auto print_path_report = [ctx](const CriticalPath &path, const bool is_single_domain, const bool is_setup = true) {
        delay_t total(0), logic_total(0), route_total(0);

        log_info("      type curr  total name\n");
        for (const auto &segment : path.segments) {

            delay_t delay = segment.delay;

            total += delay;

            if (segment.type == CriticalPath::Segment::Type::CLK_TO_Q ||
                segment.type == CriticalPath::Segment::Type::SOURCE ||
                segment.type == CriticalPath::Segment::Type::LOGIC ||
                segment.type == CriticalPath::Segment::Type::SETUP ||
                segment.type == CriticalPath::Segment::Type::HOLD) {
                logic_total += delay;

                log_info("%10s % 5.2f % 5.2f Source %s.%s\n", CriticalPath::Segment::type_to_str(segment.type).c_str(),
                         ctx->getDelayNS(delay), ctx->getDelayNS(total), segment.to.first.c_str(ctx),
                         segment.to.second.c_str(ctx));
            } else if (segment.type == CriticalPath::Segment::Type::ROUTING ||
                       segment.type == CriticalPath::Segment::Type::CLK_TO_CLK ||
                       segment.type == CriticalPath::Segment::Type::CLK_SKEW) {
                route_total = route_total + delay;

                const auto &driver = ctx->cells.at(segment.from.first);
                const auto &sink = ctx->cells.at(segment.to.first);

                auto driver_loc = ctx->getBelLocation(driver->bel);
                auto sink_loc = ctx->getBelLocation(sink->bel);

                log_info("%10s % 5.2f % 5.2f Net %s (%d,%d) -> (%d,%d)\n",
                         CriticalPath::Segment::type_to_str(segment.type).c_str(), ctx->getDelayNS(delay),
                         ctx->getDelayNS(total), segment.net.c_str(ctx), driver_loc.x, driver_loc.y, sink_loc.x,
                         sink_loc.y);
                log_info("                         Sink %s.%s\n", segment.to.first.c_str(ctx),
                         segment.to.second.c_str(ctx));

                // CLK_TO_CLK has no net and CLK_SKEW might have a net
                if (ctx->nets.count(segment.net) == 0) {
                    continue;
                }
                const NetInfo *net = ctx->nets.at(segment.net).get();

                if (ctx->verbose) {

                    PortRef sink_ref;
                    sink_ref.cell = sink.get();
                    sink_ref.port = segment.to.second;

                    auto driver_wire = ctx->getNetinfoSourceWire(net);
                    auto sink_wire = ctx->getNetinfoSinkWire(net, sink_ref);// TODO: 原方法第三个参数 , 0
                    log_info("                          prediction: %f ns estimate: %f ns\n",
                             ctx->getDelayNS(ctx->predictArcDelay(net, sink_ref)),
                             ctx->getDelayNS(ctx->estimateDelay(driver_wire, sink_wire)));
                    auto cursor = sink_wire;
                    delay_t delay;
                    while (driver_wire != cursor) {
#ifdef ARCH_ECP5
                        if (net->is_global)
                            break;
#endif
                        auto it = net->wires.find(cursor);
                        assert(it != net->wires.end());
                        auto pip = it->second.pip;
                        NPNR_ASSERT(pip != PipId());
                        delay = ctx->getPipDelay(pip).maxDelay();
                        log_info("                 %1.3f %s\n", ctx->getDelayNS(delay), ctx->nameOfPip(pip));
                        cursor = ctx->getPipSrcWire(pip);
                    }
                }

                if (!ctx->disable_critical_path_source_print) {
                    print_net_source(net);
                }
            }
        }
        // log_info("%.2f ns logic, %.2f ns routing\n", ctx->getDelayNS(logic_total), ctx->getDelayNS(route_total));
        log_info("%10s % 5.2f ns logic, % 5.2f ns routing\n", "delays:", ctx->getDelayNS(logic_total), ctx->getDelayNS(route_total));
        if (is_single_domain && is_setup)
            log_info("%10s % 5.2f ns\n", "Slack:", ctx->getDelayNS(path.slack));
        else if (is_single_domain && !is_setup)
            log_info("%10s % 5.2f ns\n", "Slack:", ctx->getDelayNS(path.slack));

    };

    // Single domain paths
    for (auto &clock : result.clock_paths_setup) {  //<IdString, CriticalPath> ==> <IdString, std::vector<CriticalPath>>
        for (auto &path : clock.second) {
            log_break();
            std::string start =
                    path.clock_pair.start.edge == FALLING_EDGE ? std::string("negedge") : std::string("posedge");
            std::string end =
                    path.clock_pair.end.edge == FALLING_EDGE ? std::string("negedge") : std::string("posedge");
            log_info("Setup analysis report for clock '%s' (%s -> %s):\n", clock.first.c_str(ctx), start.c_str(),
                    end.c_str());
            // auto &report = path;
            print_path_report(path, true, true);
        }
    }

    for (auto &clock : result.clock_paths_hold) {  //<IdString, CriticalPath> ==> <IdString, std::vector<CriticalPath>>
        for (auto &path : clock.second) {
            log_break();
            std::string start =
                    path.clock_pair.start.edge == FALLING_EDGE ? std::string("negedge") : std::string("posedge");
            std::string end =
                    path.clock_pair.end.edge == FALLING_EDGE ? std::string("negedge") : std::string("posedge");
            log_info("Hold analysis report for clock '%s' (%s -> %s):\n", clock.first.c_str(ctx), start.c_str(),
                    end.c_str());
            // auto &report = path;
            print_path_report(path, true, false);
        }
    }

    // // Cross-domain paths
    // for (auto &report : result.xclock_paths_recovery) {
    //     log_break();
    //     std::string start = clock_event_name(ctx, report.clock_pair.start);
    //     std::string end = clock_event_name(ctx, report.clock_pair.end);
    //     log_info("Recovery analysis report for cross-domain path '%s' -> '%s':\n", start.c_str(), end.c_str());
    //     print_path_report(report, false, true);
    // }
    // for (auto &report : result.xclock_paths_removal) {
    //     log_break();
    //     std::string start = clock_event_name(ctx, report.clock_pair.start);
    //     std::string end = clock_event_name(ctx, report.clock_pair.end);
    //     log_info("Removal analysis report for cross-domain path '%s' -> '%s':\n", start.c_str(), end.c_str());
    //     print_path_report(report, false, false);
    // }
}


static void log_histogram(Context *ctx, TimingResult &result)
{
    unsigned num_bins = 20;
    unsigned bar_width = 60;

    int min_slack = std::numeric_limits<int>::max();
    int max_slack = std::numeric_limits<int>::min();

    for (const auto &i : result.slack_histogram) {
        if (i.first < min_slack)
            min_slack = i.first;
        if (i.first > max_slack)
            max_slack = i.first;
    }

    auto bin_size = std::max<unsigned>(1, ceil((max_slack - min_slack + 1) / float(num_bins)));
    std::vector<unsigned> bins(num_bins);
    unsigned max_freq = 0;
    for (const auto &i : result.slack_histogram) {
        int bin_idx = int((i.first - min_slack) / bin_size);
        if (bin_idx < 0)
            bin_idx = 0;
        else if (bin_idx >= int(num_bins))
            bin_idx = num_bins - 1;
        auto &bin = bins.at(bin_idx);
        bin += i.second;
        max_freq = std::max(max_freq, bin);
    }
    bar_width = std::min(bar_width, max_freq);

    log_break();
    log_info("Slack histogram:\n");
    log_info(" legend: * represents %d endpoint(s)\n", max_freq / bar_width);
    log_info("         + represents [1,%d) endpoint(s)\n", max_freq / bar_width);
    for (unsigned i = 0; i < num_bins; ++i)
        log_info("[%6d, %6d) |%s%c\n", min_slack + bin_size * i, min_slack + bin_size * (i + 1),
                 std::string(bins[i] * bar_width / max_freq, '*').c_str(),
                 (bins[i] * bar_width) % max_freq > 0 ? '+' : ' ');
}

void Context::log_timing_results(TimingResult &result, bool print_histogram, bool print_fmax, bool print_path, bool warn_on_failure)
{
    if (print_path) {
        log_crit_paths(this, result);
        log_timing_paths(this, result);
    }
        
    if (print_fmax)
        log_fmax(this, result, warn_on_failure);

    if (print_histogram && !result.slack_histogram.empty())
        log_histogram(this, result);
}


/**
void get_criticalities(Context *ctx, NetCriticalityMap *net_crit)
{
    CriticalPathMap crit_paths;
    net_crit->clear();
    Timing timing(ctx, true, true, &crit_paths, nullptr, net_crit);
    timing.walk_paths();
}
*/
NEXTPNR_NAMESPACE_END
