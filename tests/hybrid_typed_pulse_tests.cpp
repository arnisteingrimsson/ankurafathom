#include "ankurafathom/hybrid/event_to_stock_pulse.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <array>
#include <iostream>

struct Event { std::uint64_t key; double quantity; double price; };
using Pulse=ankurafathom::hybrid::StockPulse;
using Scalar=ankurafathom::hybrid::ScalarPublication;
using Message=std::variant<Event,Pulse,Scalar>;
using Bridge=ankurafathom::hybrid::EventToStockPulse<Event,Message>;
using SD=ankurafathom::hybrid::SignalSD<Message>;
using Input=ankurafathom::devs::Input<Message>;
using Sim=ankurafathom::devs::Simulator<Message>;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) { bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }require(caught,"invalid pulse operation accepted"); }
Bridge bridge(std::string channel="sale") {
    return Bridge(std::move(channel),2,[](const Event& e) { return e.key; },
                  [](const Event& e,double) { return std::vector<double>{-e.quantity,e.quantity*e.price}; });
}
Input event(std::uint64_t key,double quantity=1,double price=2,std::size_t source=7) {
    return {source,0,Message{Event{key,quantity,price}}};
}
Input pulse(double t,std::string channel,std::uint64_t revision,std::vector<double> amounts,std::size_t source=7) {
    return {source,SD::pulse_port,Message{Pulse{t,std::move(channel),revision,std::move(amounts)}}};
}
Input scalar(double t,std::uint64_t revision,double value) { return {9,0,Message{Scalar{t,revision,{value}}}}; }
void bridge_contract() {
    auto b=bridge();
    require(b.event_count()==0 && std::isinf(b.time_advance()) && b.output().empty(),"bridge initial state");
    rejects([&] { b.internal_transition(); });
    rejects([&] { b.confluent_transition({event(1)}); });
    b.external_transition_at(.125,.125,{event(3,2,4),event(1,3,2)});
    const auto first=std::get<Pulse>(b.output().at(0).value);
    require(first==Pulse{.125,"sale",0,{-5,14}} && b.event_count()==2,"typed field projection");
    auto clone=b.clone();
    rejects([&] { b.external_transition(0,{event(4)}); });
    rejects([&] { b.confluent_transition({event(3)}); });
    require(std::get<Pulse>(b.output()[0].value)==first && b.event_count()==2,"failed confluence corrupted pulse");
    b.confluent_transition({event(4,0)});
    require(b.revision()==1 && b.event_count()==3 && std::get<Pulse>(b.output()[0].value).amounts==std::vector<double>({0,0}),"zero event not revisioned");
    require(std::get<Pulse>(clone->output()[0].value)==first,"clone aliases publication");
    b.internal_transition();
    rejects([&] { b.external_transition_at(.25,.125,{event(9),event(9)}); });
    rejects([&] { b.external_transition_at(.25,.125,{event(9),event(3)}); });
    rejects([&] { b.external_transition_at(.25,.125,{event(9),event(10,INFINITY)}); });
    rejects([&] { b.external_transition_at(.25,.125,{event(9),event(10,1,NAN)}); });
    require(b.event_count()==3 && b.time()==.125 && b.revision()==1 && b.output().empty(),"failed bag changed bridge");
    b.external_transition_at(.25,.125,{event(9),event(std::numeric_limits<std::uint64_t>::max())});
    require(b.event_count()==5 && b.revision()==2,"keys committed on failed projection");
    b.internal_transition();
    auto bad=event(11);bad.port=2;
    rejects([&] { b.external_transition_at(.5,.25,{bad}); });
    rejects([&] { b.external_transition_at(.5,.125,{event(11)}); });
    rejects([&] { b.external_transition_at(INFINITY,INFINITY,{event(11)}); });
    rejects([&] { b.external_transition_at(.5,.25,{}); });
    rejects([&] { b.external_transition_at(.5,.25,{scalar(.5,0,1)}); });
    rejects([] { Bridge("",1,[](const Event& e) { return e.key; },[](const Event&,double) { return std::vector<double>{1}; }); });
    rejects([] { Bridge("x",0,{},{}); });
    Bridge width("x",2,[](const Event& e) { return e.key; },[](const Event&,double) { return std::vector<double>{1}; });
    rejects([&] { width.external_transition(0,{event(1)}); });
    require(!width.event_count(),"width failure committed key");
    // Numerically observable order: canonical keys, independent of bag/source order.
    std::vector<Input> ordered{event(1,1e16),event(2,-1e16),event(3,1)};
    do {
        auto stable=bridge();stable.external_transition(0,ordered);
        require(std::get<Pulse>(stable.output()[0].value).amounts==std::vector<double>({-1,2}),"bag order affected reduction");
    }while(std::next_permutation(ordered.begin(),ordered.end(),[](const Input& a,const Input& b) { return std::get<Event>(a.value).key<std::get<Event>(b.value).key; }));
}
void consumer_contract() {
    SD sd({{"inventory",2},{"cash",0}},{{SD::boundary,0,[](const auto&,const auto& v,double) { return v[0]; }}},{2},.5,{"sale","supply"});
    sd.external_transition_at(.25,.25,{pulse(.25,"sale",0,{-3,6}),scalar(.25,0,4),pulse(.25,"supply",0,{2,0},8)});
    require(sd.state()==std::vector<double>({1.5,6}) && sd.signals()[0]==4,"integrate/net/latch order");
    require(sd.pulse_revision("sale")==0 && sd.pulse_revision("supply")==0,"missing channel revisions");
    sd.confluent_transition({pulse(.5,"sale",1,{-1,2}),scalar(.5,1,0)});
    require(sd.state()==std::vector<double>({1.5,8}) && sd.time()==.5 && sd.next_event_time()==1,"tick confluence");
    auto before=sd.state();auto copy=sd.clone();
    std::vector<std::vector<Input>> bad{
        {pulse(.75,"sale",1,{0,0})},{pulse(.75,"sale",2,{0,0},10)},
        {pulse(.75,"unknown",0,{0,0})},{pulse(.75,"sale",2,{0})},
        {pulse(.75,"sale",2,{NAN,0})},{pulse(.75,"sale",2,{-2,0})},
        {pulse(.75,"sale",2,{0,0}),pulse(.75,"sale",3,{0,0})},
        {pulse(.8,"sale",2,{0,0})},{event(1)},
        {scalar(.75,2,0),scalar(.75,3,0)},
        {pulse(.75,"sale",2,{0,0}),pulse(.75,"supply",1,{0,INFINITY},8)}
    };
    auto port=pulse(.75,"sale",2,{0,0});port.port=0;bad.push_back({port});
    for(const auto& bag:bad) {
        rejects([&] { sd.external_transition_at(.75,.25,bag); });
        require(sd.state()==before && sd.time()==.5 && sd.pulse_revision("sale")==1 && sd.revision()==1,"bad batch partially committed");
    }
    sd.external_transition_at(.75,.25,{pulse(.75,"sale",2,{-1,2})});
    require(dynamic_cast<const SD&>(*copy).state()==before,"consumer clone aliases state");
    // All bag permutations agree, including offsetting withdrawals/deposits.
    std::array<int,3> order{0,1,2};
    do {
        SD batch({{"x",0}},{},{1},1,{"a","b"});
        std::array<Input,3> inputs{pulse(.5,"a",0,{-4}),pulse(.5,"b",0,{4},8),scalar(.5,0,2)};
        batch.external_transition_at(.5,.5,{inputs[order[0]],inputs[order[1]],inputs[order[2]]});
        require(batch.state()[0]==0 && batch.signals()[0]==2,"simultaneous net depends on bag order");
    }while(std::next_permutation(order.begin(),order.end()));
    SD only({{"x",0,false}},{},{},1,{"a"});
    only.external_transition(.25,{pulse(.25,"a",0,{-2})});require(only.state()[0]==-2,"pulse-only signed stock");
    rejects([&] { only.external_transition(.25,{scalar(.5,0,1)}); });
    SD strict({{"x",1}},{{0,SD::boundary,[](const auto&,const auto&,double) { return 4.; }}},{0},1,{"a"});
    rejects([&] { strict.external_transition(.5,{pulse(.5,"a",0,{5})}); });
    require(strict.time()==0 && !strict.pulse_revision("a"),"pulse rescued invalid integration");
    const double max=std::numeric_limits<double>::max();
    SD sums({{"x",0,false}},{},{},1,{"a","b"});
    rejects([&] { sums.external_transition(0,{pulse(0,"a",0,{max}),pulse(0,"b",0,{max})}); });
    require(!sums.pulse_revision("a") && sums.state()[0]==0,"sum overflow committed");
    SD limit({{"x",max}},{},{},1,{"a"});
    rejects([&] { limit.external_transition(0,{pulse(0,"a",0,{max})}); });
    SD rollover({{"x",0}},{},{},max,{"a"});
    rejects([&] { rollover.confluent_transition({pulse(max,"a",0,{1})}); });
    require(rollover.time()==0 && rollover.state()[0]==0 && !rollover.pulse_revision("a"),"deadline overflow committed pulse");
    rejects([] { SD({{"x",0}},{},{},1,{"a","a"}); });
    rejects([] { SD({{"x",0}},{},{},1,{""}); });
    rejects([] { SD({}, {}, {},1,{"a"}); });
}
class Failing final:public ankurafathom::devs::Atomic<Message> {
public:
    std::unique_ptr<ankurafathom::devs::Atomic<Message>> clone()const override { return std::make_unique<Failing>(*this); }
    double time_advance()const override { return INFINITY; }
    std::vector<ankurafathom::devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<Input>&)override { throw std::runtime_error("downstream"); }
    void confluent_transition(const std::vector<Input>& bag)override { external_transition(0,bag); }
};
void kernel_contract() {
    std::array<int,3> order{0,1,2};
    do {
        Sim sim;std::array<std::size_t,3> ids{};
        for(int role:order) {
            if(role<2) ids[role]=sim.add(std::make_unique<Bridge>(bridge(role==0?"a":"b")));
            else ids[role]=sim.add(std::make_unique<SD>(std::vector<SD::Stock>{{"stock",10},{"cash",0}},std::vector<SD::Flow>{},std::vector<double>{},.5,std::vector<std::string>{"a","b"}));
        }
        for(int role:{0,1}) sim.connect(ids[role],1,ids[2],SD::pulse_port);
        sim.inject(.25,ids[0],0,Message{Event{1,2,3}});sim.inject(.25,ids[1],0,Message{Event{1,-1,2}});
        sim.inject(.5,ids[0],0,Message{Event{2,1,4}});
        sim.run_until_transactional(1,100);
        const auto& sd=dynamic_cast<const SD&>(sim.model(ids[2]));
        require(sd.state()==std::vector<double>({8,8}) && sd.pulse_revision("a")==1,"graph declaration order");
    }while(std::next_permutation(order.begin(),order.end()));
    Sim sim;
    auto b=sim.add(std::make_unique<Bridge>(bridge()));
    auto s=sim.add(std::make_unique<SD>(std::vector<SD::Stock>{{"inventory",5},{"cash",0}},std::vector<SD::Flow>{},std::vector<double>{},.5,std::vector<std::string>{"sale"}));
    auto f=sim.add(std::make_unique<Failing>());
    sim.connect(b,1,s,2);sim.connect(b,1,f,0);
    sim.inject(.25,b,0,Message{Event{10,2,3}});
    (void)sim.step_transactional();
    rejects([&] { (void)sim.step_transactional(); });
    require(dynamic_cast<const Bridge&>(sim.model(b)).event_count()==1 && sim.model(b).output().size()==1,"failed publication consumed event");
    require(dynamic_cast<const SD&>(sim.model(s)).state()==std::vector<double>({5,0}),"failed publication changed stock");
    sim.disconnect(b,1,f,0);sim.run_until_transactional(.25,100);
    require(dynamic_cast<const SD&>(sim.model(s)).state()==std::vector<double>({3,6}),"publication retry duplicated amount");
    sim.inject(.5,b,0,Message{Event{10,1,1}});
    rejects([&] { sim.run_until_transactional(.5,100); });
    require(dynamic_cast<const SD&>(sim.model(s)).time()==.25 && sim.next_time()==.5,"duplicate event failed to restore shared tick");
}
void typed_des_completion() {
    struct Job {};
    using Token=ankurafathom::des::EntityToken<Job>;
    using M=std::variant<Token,ankurafathom::des::QueuePull,Pulse,Scalar>;
    using D=ankurafathom::des::ReferenceDelay<Job,M>;
    using B=ankurafathom::hybrid::EventToStockPulse<Token,M>;
    using S=ankurafathom::hybrid::SignalSD<M>;
    ankurafathom::des::EntityStore<Job,double,double> jobs(7);
    auto a=jobs.spawn({3,100}),b=jobs.spawn({2,150}),c=jobs.spawn({4,125});
    std::array<int,3> order{0,1,2};
    do {
        ankurafathom::devs::Simulator<M> sim;std::array<std::size_t,3> ids{};
        for(int role:order) {
            if(role==0) ids[role]=sim.add(std::make_unique<D>(7,.375));
            if(role==1) ids[role]=sim.add(std::make_unique<B>("completed",2,[](const Token& t) { return t.entity.id; },
                [jobs](const Token& t,double) { return std::vector<double>{jobs.field<0>(t.entity),jobs.field<0>(t.entity)*jobs.field<1>(t.entity)}; }));
            if(role==2) ids[role]=sim.add(std::make_unique<S>(std::vector<S::Stock>{{"hours",0},{"revenue",0}},
                std::vector<S::Flow>{},std::vector<double>{},.5,std::vector<std::string>{"completed"}));
        }
        sim.connect(ids[0],1,ids[1],0);sim.connect(ids[1],1,ids[2],S::pulse_port);
        sim.inject(0,ids[0],0,M{Token{a}});sim.inject(.125,ids[0],0,M{Token{b}});sim.inject(.625,ids[0],0,M{Token{c}});
        const std::array<double,4> times{0,.375,.5,1},hours{0,3,5,9},revenue{0,300,600,1100};
        for(std::size_t i=0;i<times.size();++i) {
            sim.run_until_transactional(times[i]);
            const auto& sd=dynamic_cast<const S&>(sim.model(ids[2]));
            require(sd.state()==std::vector<double>({hours[i],revenue[i]}),"typed DES payload/reference projection mismatch");
            require(dynamic_cast<const D&>(sim.model(ids[0])).completed_count()==i,"DES/pulse completion mismatch");
        }
    }while(std::next_permutation(order.begin(),order.end()));
}
int main() { try { bridge_contract();consumer_contract();kernel_contract();typed_des_completion();std::cout<<"typed pulse contracts passed\n"; }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
