#include "ankurafathom/hybrid/typed_agent_pool_atomic.hpp"
#include "ankurafathom/hybrid/population_result_publisher.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <fstream>
#include <iostream>

namespace af=ankurafathom;
struct Worker {};
using Core=af::hybrid::TypedAgentPool<Worker>;
using Control=af::hybrid::TypedPoolControl<Worker>;
using M=std::variant<Control,Core::Result,Core::Notification,af::des::Seize,af::des::Release,af::des::Grant,
    af::abm::PopulationResult<Worker>,af::hybrid::PopulationSnapshot<Worker>,af::hybrid::ScalarPublication>;
using Pool=af::hybrid::TypedAgentPoolAtomic<Worker,M>;
using Publisher=af::hybrid::PopulationResultPublisher<Worker,M>;
using Aggregate=af::hybrid::PopulationAggregate<Worker,M>;
using Reduction=Aggregate::Reduction;
using SD=af::hybrid::SignalSD<M>;
using json=nlohmann::json;
Core::Record row(std::int64_t n,bool enabled=true) { return {n,std::int64_t{0},enabled,std::int64_t{0},std::int64_t{0}}; }
std::size_t capacity(Core::Reference ref,const Core::Store& s) {
    return std::get<bool>(s.field(ref,2))?static_cast<std::size_t>(std::get<std::int64_t>(s.field(ref,0))):0;
}
Core initial(const json& c) {
    using K=af::des::FieldKind;
    Core::Store store(7,{{"capacity",K::integer},{"allocated",K::integer},{"enabled",K::boolean},{"assigned",K::integer},{"released",K::integer}},c.at("first"));
    for(const std::int64_t n:c.at("initial")) store.spawn(row(n));
    const auto discipline=c.at("discipline")=="fifo"?af::des::QueueDiscipline::fifo:
        c.at("discipline")=="lifo"?af::des::QueueDiscipline::lifo:af::des::QueueDiscipline::priority;
    Core core(store,1,capacity,8,discipline,[](const auto& note,const auto& s) {
        auto r=s.record(note.agent);const auto index=note.assigned?3:4;
        r[index]=std::get<std::int64_t>(r[index])+static_cast<std::int64_t>(note.units);return r;
    });
    core.add_phase([](auto ref,const auto& s) {
        std::int64_t enabled=0;
        for(auto id=s.first_id();id<s.next_id();++id) if(id!=ref.id && s.alive({7,id}) && std::get<bool>(s.field({7,id},2))) ++enabled;
        auto r=s.record(ref);r[0]=std::get<std::int64_t>(r[0])+enabled;return r;
    });return core;
}
int main(int argc,char** argv) {
    try {
        if(argc!=3) throw std::invalid_argument("expected plan/output paths");
        std::ifstream input(argv[1]);const auto plan=json::parse(input);
        std::ofstream out(argv[2]);out.exceptions(std::ios::badbit|std::ios::failbit);std::size_t snapshots=0;
        for(const auto& c:plan.at("cases")) for(const auto& order:plan.at("orders")) for(const std::string direction:plan.at("bag_orders")) {
            const auto start=initial(c);af::devs::Simulator<M> sim;std::array<std::size_t,4> ids{};
            for(const int role:order) {
                if(role==0) ids[role]=sim.add(std::make_unique<Pool>(start));
                if(role==1) ids[role]=sim.add(std::make_unique<Publisher>(start.store()));
                if(role==2) ids[role]=sim.add(std::make_unique<Aggregate>(start.store(),std::vector<Reduction>{
                    {"capacity",Reduction::Kind::sum,[](auto ref,const auto& s) { return static_cast<double>(capacity(ref,s)); }},
                    {"allocated",Reduction::Kind::sum,[](auto ref,const auto& s) { return static_cast<double>(std::get<std::int64_t>(s.field(ref,1))); }},
                    {"headcount",Reduction::Kind::count}}));
                if(role==3) ids[role]=sim.add(std::make_unique<SD>(std::vector<SD::Stock>{{"capacity_hours",0},{"allocated_hours",0},{"agent_hours",0}},
                    std::vector<SD::Flow>{{SD::boundary,0,[](const auto&,const auto& v,double) { return v[0]; }},
                        {SD::boundary,1,[](const auto&,const auto& v,double) { return v[1]; }},
                        {SD::boundary,2,[](const auto&,const auto& v,double) { return v[2]; }}},std::vector<double>{0,0,0},.25));
            }
            sim.connect(ids[0],Pool::population_port,ids[1],0);sim.connect(ids[1],1,ids[2],0);sim.connect(ids[2],1,ids[3],0);
            std::optional<Core::Result> last;std::size_t event_index=0;
            const auto run=[&](double horizon) {
                for(const auto& step:sim.run_until_transactional(horizon)) for(const auto& emission:step.emissions)
                    if(emission.source==ids[0] && emission.port==Pool::result_port) last=std::get<Core::Result>(emission.value);
            };
            for(const double horizon:plan.at("horizons")) {
                while(event_index<c.at("events").size() && c.at("events")[event_index].at("time").get<double>()<=horizon) {
                    const auto& e=c.at("events")[event_index];const auto& current=dynamic_cast<const Pool&>(sim.model(ids[0])).core();
                    Core::Change change;change.time=e.at("time");change.run_phases=e.value("phase",false);
                    if(e.contains("updates")) for(const auto& u:e.at("updates")) {
                        const Core::Reference ref{7,current.store().first_id()+u[0].get<std::uint64_t>()};auto record=current.store().record(ref);
                        // Explicit updates are full records after the release-handler batch.
                        if(e.value("release_all",false)) record[4]=std::get<std::int64_t>(record[4])+std::get<std::int64_t>(record[1]);
                        record[0]=u[1].get<std::int64_t>();record[1]=std::int64_t{0};record[2]=u[2].get<bool>();change.updates.emplace_back(ref,record);
                    }
                    if(e.value("retire_all",false)) {
                        for(auto id=current.store().first_id();id<current.store().next_id();++id) if(current.store().alive({7,id})) change.retirements.push_back({7,id});
                    } else if(e.contains("retire")) for(const std::uint64_t offset:e.at("retire")) change.retirements.push_back({7,current.store().first_id()+offset});
                    if(e.contains("births")) for(const auto& b:e.at("births")) change.births.push_back(row(b[0],b[1]));
                    std::vector<std::pair<std::uint32_t,M>> bag{{2,M{Control{event_index,change}}}};
                    if(e.value("release_all",false)) for(const auto& [key,shares]:current.assignments()) { (void)shares;bag.emplace_back(0,M{af::des::Release{key}}); }
                    if(e.contains("requests")) for(const auto& q:e.at("requests")) bag.emplace_back(0,M{af::des::Seize{q[0],q[1],q[2]}});
                    if(direction=="reverse") std::reverse(bag.begin(),bag.end());
                    for(const auto& [port,message]:bag) sim.inject(change.time,ids[0],port,message);
                    run(change.time);++event_index;
                }
                run(horizon);
                const auto& current=dynamic_cast<const Pool&>(sim.model(ids[0])).core();const auto& store=current.store();
                const auto& snapshot=dynamic_cast<const Publisher&>(sim.model(ids[1])).snapshot();
                const auto& sd=dynamic_cast<const SD&>(sim.model(ids[3]));
                const auto& cap=std::get<std::vector<std::int64_t>>(store.column(0));const auto& allocated=std::get<std::vector<std::int64_t>>(store.column(1));
                const auto& enabled=std::get<std::vector<bool>>(store.column(2));const auto& assigned=std::get<std::vector<std::int64_t>>(store.column(3));
                const auto& released=std::get<std::vector<std::int64_t>>(store.column(4));
                json records=json::array(),ledger=json::array(),notifications=json::array(),grants=json::array();
                for(std::size_t i=0;i<store.size();++i) records.push_back(json::array({store.first_id()+i,cap[i],allocated[i],bool(enabled[i]),assigned[i],released[i],bool(store.live_rows()[i])}));
                for(const auto& [key,shares]:current.assignments()) {
                    json entries=json::array();for(const auto& share:shares) entries.push_back(json::array({share.agent.id,share.units}));ledger.push_back(json::array({key,entries}));
                }
                if(last) {
                    for(const auto& n:last->notifications) notifications.push_back(json::array({n.request_id,n.agent.id,n.units,n.assigned}));
                    for(const auto& g:last->grants) grants.push_back(json::array({g.request_id,g.units}));
                }
                const auto stats=current.statistics(horizon);
                if(snapshot.population.next_id()!=store.next_id() || snapshot.population.live_rows()!=store.live_rows()) throw std::logic_error("pool snapshot mismatch");
                for(std::size_t field=0;field<store.schema().size();++field) if(snapshot.population.column(field)!=store.column(field)) throw std::logic_error("pool snapshot field mismatch");
                const json state{{"time",current.time()},{"rows",records},{"assignments",ledger},{"capacity",current.pool().capacity()},
                    {"allocated",current.pool().allocated_units()},{"waiting",current.pool().waiting()},{"live",store.active_count()},
                    {"statistics",json::array({stats.capacity_time,stats.allocated_time,stats.waiting_request_time,stats.live_agent_time})},
                    {"sd_time",sd.time()},{"sd",sd.state()},{"signals",sd.signals()},{"revision",snapshot.revision},
                    {"notifications",notifications},{"grants",grants},{"next_id",store.next_id()}};
                out<<json{{"case",c.at("id")},{"order",order},{"bag_order",direction},{"horizon",horizon},{"state",state}}.dump()<<'\n';++snapshots;
            }
        }
        std::cout<<snapshots<<" typed workforce/aggregate/SD snapshots generated\n";
    }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
