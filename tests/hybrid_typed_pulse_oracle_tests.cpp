#include "ankurafathom/hybrid/event_to_stock_pulse.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <iostream>

struct Event { std::uint64_t key; double quantity,price; };
using Pulse=ankurafathom::hybrid::StockPulse;
using Scalar=ankurafathom::hybrid::ScalarPublication;
using Message=std::variant<Event,Pulse,Scalar>;
using Bridge=ankurafathom::hybrid::EventToStockPulse<Event,Message>;
using SD=ankurafathom::hybrid::SignalSD<Message>;
using json=nlohmann::json;
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan/output paths");
        std::ifstream input(argv[1]);const auto plan=json::parse(input);
        std::ofstream output(argv[2]);output.exceptions(std::ios::badbit|std::ios::failbit);
        std::size_t rows=0;
        for(const auto& c:plan.at("cases")) for(const auto& order:plan.at("orders")) for(int input_order:plan.at("input_orders")) {
            ankurafathom::devs::Simulator<Message> sim;std::array<std::size_t,3> ids{};
            for(int role:order) {
                if(role<2) ids[role]=sim.add(std::make_unique<Bridge>(role==0?"sale":"supply",3,
                    [](const Event& e) { return e.key; },[role](const Event& e,double) {
                        const auto sign=role==0?-1.:1.;
                        return std::vector<double>{sign*e.quantity,-sign*e.quantity*e.price,0};
                    }));
                else {
                    const auto init=c.at("initial").get<std::vector<double>>();
                    std::vector<SD::Stock> stocks{{"inventory",init[0]},{"cash",init[1]},{"fees",init[2]}};
                    std::vector<SD::Flow> flows{{0,1,[](const auto& s,const auto& v,double) { return s[0]*v[0]; }},
                                              {1,2,[](const auto& s,const auto&,double) { return s[1]/32.; }}};
                    ids[2]=sim.add(std::make_unique<SD>(stocks,flows,std::vector<double>{.125},c.at("dt").get<double>(),
                                                       std::vector<std::string>{"sale","supply"}));
                }
            }
            for(int role:{0,1}) sim.connect(ids[role],Bridge::output_port,ids[2],SD::pulse_port);
            auto events=c.at("events").get<std::vector<json>>();
            if(input_order) std::reverse(events.begin(),events.end());
            for(const auto& event:events) sim.inject(event.at("time"),ids[event.at("channel").get<std::size_t>()],0,
                Message{Event{event.at("key").get<std::uint64_t>(),event.at("quantity"),event.at("price")}});
            for(const auto& s:c.at("signals")) sim.inject(s.at("time"),ids[2],0,
                Message{Scalar{s.at("time"),s.at("revision"),{s.at("value")}}});
            for(double horizon:plan.at("horizons")) {
                (void)sim.run_until_transactional(horizon);
                const auto& sd=dynamic_cast<const SD&>(sim.model(ids[2]));
                json revisions=json::array(),counts=json::array();
                for(int role:{0,1}) {
                    const auto revision=sd.pulse_revision(role==0?"sale":"supply");
                    revisions.push_back(revision?json(*revision):json(nullptr));
                    counts.push_back(dynamic_cast<const Bridge&>(sim.model(ids[role])).event_count());
                }
                output<<json{{"case",c.at("id")},{"order",order},{"input_order",input_order},{"horizon",horizon},
                             {"time",sd.time()},{"stocks",sd.state()},{"signal",sd.signals()[0]},
                             {"scalar_revision",sd.revision().value()},{"revisions",revisions},{"counts",counts}}.dump()<<'\n';
                ++rows;
            }
        }
        std::cout<<rows<<" typed pulse/SD snapshots generated\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
