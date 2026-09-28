#include "ankurafathom/hybrid/population_result_publisher.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <array>
#include <iostream>

struct Person {};
using Message=ankurafathom::hybrid::PopulationAggregateMessage<Person>;
using Pop=ankurafathom::abm::PopulationAtomic<Person,Message>;
using Publisher=ankurafathom::hybrid::PopulationResultPublisher<Person>;
using Aggregate=ankurafathom::hybrid::PopulationAggregate<Person,Message>;
using Reduction=Aggregate::Reduction;
using SD=ankurafathom::hybrid::SignalSD<Message>;
using Store=Pop::Store;
using Sim=ankurafathom::devs::Simulator<Message>;
using Input=ankurafathom::devs::Input<Message>;
using Snapshot=Publisher::Snapshot;
void require(bool ok,const char* text) { if(!ok) throw std::runtime_error(text); }
template<class F> void rejects(F f) {
    bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }
    require(caught,"invalid population result accepted");
}
Store initial() {
    Store s(8,{{"work",ankurafathom::des::FieldKind::real}});
    s.spawn({1.});s.spawn({3.});return s;
}
double work(const Store& s,Store::Reference ref) { return std::get<double>(s.field(ref,0)); }
std::unique_ptr<Pop> population(bool async) {
    const Pop::InputRule input=[](const Pop::Command& command,const Store& s) {
        if(command.kind!="bump") throw std::invalid_argument("unknown input");
        Pop::Effects e;e.updates.push_back({command.agent,{work(s,command.agent)+7}});return e;
    };
    if(!async) {
        Pop::Sync p(initial());
        p.add_phase([](auto ref,const Store& s) { return Store::Record{work(s,ref)+1}; });
        return std::make_unique<Pop>(std::move(p),.5,input);
    }
    Pop::Async p(initial(),[](const auto& timer,const Store& s) {
        const double delta=timer.kind=="large" ? 10 : timer.kind=="two" ? 2 : 1;
        Pop::Effects e;e.updates.push_back({timer.agent,{work(s,timer.agent)+delta}});return e;
    });
    p.schedule({.25,{8,0},"large",0});p.schedule({.5,{8,1},"one",0});
    auto result=std::make_unique<Pop>(std::move(p),input);
    result->configure_births([](auto ref,const Store& s,double) {
        Pop::Async::Birth birth{s.record(ref),{}};
        if(ref.id==2) birth.timers.push_back({.75,"two",0});
        if(ref.id==3) { birth.timers.push_back({1,"one",0});birth.timers.push_back({1.5,"one",0}); }
        return birth;
    });
    return result;
}
struct Graph {
    Sim sim;
    std::array<std::size_t,4> ids;
    Graph(std::array<std::size_t,4> order,bool async) {
        std::array<std::unique_ptr<ankurafathom::devs::Atomic<Message>>,4> models;
        models[0]=population(async);
        models[1]=std::make_unique<Publisher>(initial());
        models[2]=std::make_unique<Aggregate>(initial(),std::vector<Reduction>{
            {"sum",Reduction::Kind::sum,[](auto r,const Store& s) { return work(s,r); }},
            {"count",Reduction::Kind::count}});
        models[3]=std::make_unique<SD>(std::vector<SD::Stock>{{"work_area",0},{"person_time",0}},
            std::vector<SD::Flow>{{SD::boundary,0,[](const auto&,const auto& v,double) { return v[0]; }},
                                  {SD::boundary,1,[](const auto&,const auto& v,double) { return v[1]; }}},std::vector<double>{0,0},.25);
        for(auto index:order) ids[index]=sim.add(std::move(models[index]));
        for(int i=0;i<3;++i) sim.connect(ids[i],1,ids[i+1],0);
        sim.inject(.25,ids[0],3,Message{Pop::LifecycleInput{.25,0,{{8,0}},{{5.}}}});
        sim.inject(.5,ids[0],3,Message{Pop::LifecycleInput{.5,1,{},{{2.}}}});
        sim.inject(.75,ids[0],0,Message{Pop::Command{.75,0,{8,1},"bump"}});
        sim.inject(1.25,ids[0],3,Message{Pop::LifecycleInput{1.25,2,{{8,1},{8,2},{8,3}},{}}});
    }
    const Publisher& publisher()const { return dynamic_cast<const Publisher&>(sim.model(ids[1])); }
    const SD& consumer()const { return dynamic_cast<const SD&>(sim.model(ids[3])); }
};
void committed_histories() {
    const std::array<double,7> count{2,2,3,3,3,0,0},person_time{0,.5,1,1.75,2.5,3.25,3.25};
    std::array<std::size_t,4> order{0,1,2,3};
    do {
        for(bool async:{false,true}) {
            Graph graph(order,async);
            const std::array<double,7> sum=async ? std::array<double,7>{4,8,11,20,21,0,0} : std::array<double,7>{4,8,13,20,23,0,0};
            const std::array<double,7> area=async ? std::array<double,7>{0,1,3,5.75,10.75,16,16} : std::array<double,7>{0,1,3,6.25,11.25,17,17};
            for(std::size_t k=0;k<7;++k) {
                (void)graph.sim.run_until_transactional(k*.25);
                const auto revision=static_cast<std::uint64_t>(async && k==6 ? 5 : k);
                require(graph.consumer().signals()==std::vector<double>({sum[k],count[k]}),"ABM committed reducer history mismatch");
                require(graph.consumer().state()==std::vector<double>({area[k],person_time[k]}),"ABM committed exposure mismatch");
                require(graph.publisher().snapshot().revision==revision && graph.consumer().revision()==revision,"ABM publication revision mismatch");
            }
        }
    } while(std::next_permutation(order.begin(),order.end()));
}
std::vector<Input> input(double time,Store s,std::size_t source=6) {
    return {{source,0,Message{Pop::Result{time,std::move(s),{},{},{}}}}};
}
void adapter_contract() {
    auto store=initial();Publisher p(store);
    store.update({8,0},{9.});
    require(work(p.snapshot().population,{8,0})==1,"initial snapshot borrows store");
    auto old=p.output();
    rejects([&] { p.external_transition(0,input(0,store)); });
    p.confluent_transition(input(0,store));
    require(p.snapshot().revision==1 && work(p.snapshot().population,{8,0})==9,"same-time result not staged");
    require(work(std::get<Snapshot>(old[0].value).population,{8,0})==1,"old publication was mutated");
    auto clone=p.clone();p.internal_transition();
    require(p.output().empty() && clone->output().size()==1,"clone shares pending state");
    const auto before=p.snapshot();
    rejects([&] { p.external_transition(.5,input(.5,store,7)); });
    auto bad=input(.5,store);bad.push_back(bad[0]);
    rejects([&] { p.external_transition(.5,bad); });
    bad=input(.5,store);bad[0].port=1;
    rejects([&] { p.external_transition(.5,bad); });
    rejects([&] { p.external_transition_at(.25,.25,input(.5,store)); });
    rejects([&] { p.external_transition_at(.5,.25,input(.5,store)); });
    rejects([&] { p.external_transition(.5,input(.5,Store(9,store.schema()))); });
    rejects([&] { p.external_transition(.5,input(.5,Store(8,store.schema(),1))); });
    rejects([&] { p.external_transition(.5,input(.5,Store(8,{{"other",ankurafathom::des::FieldKind::real}}))); });
    require(p.snapshot().revision==before.revision && p.snapshot().time==0 && work(p.snapshot().population,{8,0})==9,
            "invalid result changed adapter state");
    p.external_transition(.5,input(.5,store));
    require(p.snapshot().revision==2 && p.output().size()==1,"unchanged result must publish new revision");
    p.internal_transition();
    rejects([&] { p.external_transition_at(.25,-.25,input(.25,store)); });
    rejects([&] { p.internal_transition(); });
}
class Reject final:public ankurafathom::devs::Atomic<Message> {
public:
    std::unique_ptr<ankurafathom::devs::Atomic<Message>> clone()const override { return std::make_unique<Reject>(*this); }
    double time_advance()const override { return std::numeric_limits<double>::infinity(); }
    std::vector<ankurafathom::devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<Input>&)override { throw std::runtime_error("downstream failure"); }
    void confluent_transition(const std::vector<Input>& bag)override { external_transition(0,bag); }
};
void retry() {
    Graph g({0,1,2,3},false);
    (void)g.sim.run_until_transactional(0);
    auto bad=g.sim.add(std::make_unique<Reject>());
    g.sim.connect(g.ids[1],1,bad,0);
    (void)g.sim.step_transactional(); // Lifecycle and SD tick.
    (void)g.sim.step_transactional(); // Population result -> adapter.
    rejects([&] { (void)g.sim.step_transactional(); });
    require(g.publisher().snapshot().revision==1 && g.publisher().output().size()==1 && g.consumer().signals()[0]==4,
            "failed adapter publication did not restore pending state");
    g.sim.disconnect(g.ids[1],1,bad,0);
    (void)g.sim.run_until_transactional(.25);
    require(g.consumer().signals()[0]==8 && g.consumer().state()[0]==1 && g.publisher().snapshot().revision==1,
            "adapter retry duplicated commit or area");
}
int main() {
    try {
        committed_histories();adapter_contract();retry();
        std::cout<<"Typed ABM aggregate adapter: 24 graph orders, sync/async lifecycle histories, 336 exact snapshots and rollback passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
