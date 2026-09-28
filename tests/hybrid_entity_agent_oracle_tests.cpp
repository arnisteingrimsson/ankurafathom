#include "ankurafathom/hybrid/entity_agent.hpp"
#include "ankurafathom/abm/statechart.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <iostream>

struct Person {};
using Owner=ankurafathom::hybrid::EntityAgentAtomic<Person>;
using M=ankurafathom::hybrid::EntityAgentMessage<Person>;
using Pop=Owner::Population;using Store=Pop::Store;using Token=Owner::Token;
using Chart=ankurafathom::abm::Statechart<Person>;
using json=nlohmann::json;
Store::Record newborn(double work,double duration) { return {std::int64_t{-1},0.,std::int64_t{-1},work,duration}; }
Owner make_owner(const json& c) {
    Store s(8,{{"state",ankurafathom::des::FieldKind::integer},{"entered",ankurafathom::des::FieldKind::real},
        {"generation",ankurafathom::des::FieldKind::integer},{"work",ankurafathom::des::FieldKind::real},
        {"duration",ankurafathom::des::FieldKind::real}});
    for(const auto& r:c.at("initial")) s.spawn(newborn(r[0],r[1]));
    Chart::Transition ready;ready.name="ready";ready.source=0;ready.target=1;ready.trigger=Chart::Trigger::timeout;ready.value=c.at("ready_delay");
    ready.action=[](auto ref,const Store& store) { auto r=store.record(ref);std::get<double>(r[3])+=1;return r; };
    Chart::Transition progress;progress.name="progress";progress.source=1;progress.target=2;progress.trigger=Chart::Trigger::timeout;progress.value=.25;
    progress.action=[](auto ref,const Store& store) { auto r=store.record(ref);std::get<double>(r[3])+=10;return r; };
    std::vector<Chart::Transition> transitions{ready,progress};
    for(std::int64_t state:{1,2}) {
        Chart::Transition finish;finish.name="finish"+std::to_string(state);finish.source=state;finish.target=3;
        finish.trigger=Chart::Trigger::message;finish.message="complete";
        finish.action=[](auto ref,const Store& store) { auto r=store.record(ref);std::get<double>(r[3])+=100;return r; };
        if(c.at("retire").get<bool>()) finish.lifecycle=[](auto ref,const Store&) { return Chart::Lifecycle{ref.id%3==1,{}}; };
        transitions.push_back(finish);
    }
    Chart chart(s,{"state","entered","generation"},{0,1,2,3},transitions);
    Pop::Async runtime(s,[chart](const auto& timer,const Store& store) { return chart.on_timer(timer,store); });
    for(auto id=s.first_id();id<s.next_id();++id) runtime.apply(chart.start({8,id},runtime.store(),0,0));
    Pop pop(std::move(runtime),[chart](const Pop::Command& c,const Store& store) { return chart.message(c.agent,store,c.kind,c.time); });
    pop.configure_births([chart](auto ref,const Store& store,double t) {
        const auto e=chart.start(ref,store,0,t);Pop::Async::Birth birth{e.updates.at(0).second,{}};
        for(const auto& timer:e.schedules) birth.timers.push_back({timer.time,timer.kind,timer.generation});return birth;
    });
    return Owner(std::move(pop),[](auto ref,const Store& store,double) { return std::get<std::int64_t>(store.field(ref,0))==1; },"complete");
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan/output paths");
        std::ifstream in(argv[1]);const auto plan=json::parse(in);
        std::ofstream out(argv[2]);out.exceptions(std::ios::badbit|std::ios::failbit);std::size_t snapshots=0;
        using Seize=ankurafathom::des::ReferenceSeize<Person,M>;
        using Pool=ankurafathom::des::TypedResourcePool<M>;
        using Delay=ankurafathom::des::ReferenceDelay<Person,M>;
        using Release=ankurafathom::des::ReferenceRelease<Person,M>;
        for(const auto& c:plan.at("cases")) for(const auto& order:plan.at("orders")) {
            ankurafathom::devs::Simulator<M> sim;std::array<std::size_t,5> ids{};
            for(int role:order) {
                if(role==0) ids[role]=sim.add(std::make_unique<Owner>(make_owner(c)));
                if(role==1) ids[role]=sim.add(std::make_unique<Seize>(8,11,1,1));
                if(role==2) ids[role]=sim.add(std::make_unique<Pool>(c.at("capacity").get<std::size_t>(),1));
                if(role==3) ids[role]=sim.add(std::make_unique<Delay>(8,[](const Token& t) { return std::get<double>(t.snapshot->record[4]); }));
                if(role==4) ids[role]=sim.add(std::make_unique<Release>(8,11));
            }
            sim.connect(ids[0],5,ids[1],0);sim.connect(ids[1],3,ids[2],0);sim.connect(ids[2],2,ids[1],2);
            sim.connect(ids[1],1,ids[3],0);sim.connect(ids[3],1,ids[4],0);sim.connect(ids[4],3,ids[2],0);sim.connect(ids[4],1,ids[0],6);
            std::uint64_t seq=0;
            for(const auto& b:c.at("births")) sim.inject(b.at("time"),ids[0],3,M{Pop::LifecycleInput{b.at("time"),seq++,{}, {newborn(b.at("work"),b.at("duration"))}}});
            for(double h:plan.at("horizons")) {
                sim.run_until_transactional(h);
                const auto& owner=dynamic_cast<const Owner&>(sim.model(ids[0]));
                const auto& store=owner.store();const auto& pool=dynamic_cast<const Pool&>(sim.model(ids[2])).pool();
                if(owner.in_flight_count()!=pool.waiting()+pool.allocated_units() || pool.available()+pool.allocated_units()!=c.at("capacity").get<std::size_t>())
                    throw std::logic_error("entity-agent/resource conservation failure");
                json records=json::array();
                for(std::uint64_t id=0;id<store.next_id();++id) {
                    const auto status=owner.status({8,id});json snapshot=nullptr;
                    if(status!=Owner::Status::waiting) {
                        const auto& issued=owner.issued({8,id});
                        snapshot=json::array({issued.snapshot->time,std::get<double>(issued.snapshot->record[3]),std::get<double>(issued.snapshot->record[4])});
                    }
                    records.push_back(json::array({id,std::get<std::vector<std::int64_t>>(store.column(0))[id],
                        std::get<std::vector<double>>(store.column(1))[id],std::get<std::vector<std::int64_t>>(store.column(2))[id],
                        std::get<std::vector<double>>(store.column(3))[id],std::get<std::vector<double>>(store.column(4))[id],
                        bool(store.live_rows()[id]),static_cast<int>(status),snapshot}));
                }
                out<<json{{"case",c.at("id")},{"order",order},{"horizon",h},{"time",owner.time()},{"rows",records},
                    {"allocated",pool.allocated_units()},{"waiting",pool.waiting()},{"launched",owner.launched_count()},
                    {"returned",owner.returned_count()}}.dump()<<'\n';++snapshots;
            }
        }
        std::cout<<snapshots<<" shared entity-agent/process snapshots generated\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
