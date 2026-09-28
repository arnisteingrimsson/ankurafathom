#include "ankurafathom/abm/population_atomic.hpp"
#include <iostream>
using namespace ankurafathom;
struct Agent {};
using A=abm::PopulationAtomic<Agent>;
using Store=A::Store;
using Sim=devs::Simulator<A::Message>;
void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
template<class F> void rejects(F f) { bool b=false;try { f(); } catch(const std::exception&) { b=true; } require(b,"invalid topic operation accepted"); }
Store initial() { Store s(7,{{"n",des::FieldKind::integer}});s.spawn_many({{std::int64_t(0)},{std::int64_t(0)}});return s; }
std::int64_t value(const Store& s,std::uint64_t id) { return std::get<std::int64_t>(s.field({7,id},0)); }
A::TopicConfig config(std::size_t capacity=8,std::size_t budget=100) {
    A::TopicConfig c; c.capacities={{"ping",capacity},{"pong",capacity}};c.budget=budget;
    c.handler=[](const A::Delivery& d,const Store& s) {
        A::TopicEffects e;e.effects.updates.push_back({d.receiver,{value(s,d.receiver.id)*10+std::int64_t(d.value)}});
        if(d.topic=="ping") e.publications.push_back({"pong",d.sender,2});
        return e;
    }; return c;
}
A make(std::size_t capacity=8,std::size_t budget=100) { A a(A::Sync(initial()),1);a.configure_topics(config(capacity,budget));return a; }
A::TopicInput ping(double t,std::uint64_t seq=0) { return {t,"ping",{7,0},std::nullopt,seq,1}; }
class Receiver final:public devs::Atomic<A::Message> {
public:
    explicit Receiver(bool* fail):fail_(fail) {}
    std::uint64_t deliveries=0;
    double time_advance()const override { return std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<A::Message>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<devs::Input<A::Message>>& bag)override {
        for(const auto& v:bag) deliveries+=std::get<A::Result>(v.value).deliveries.size();
        if(*fail_) throw std::runtime_error("receiver failure");
    }
    void confluent_transition(const std::vector<devs::Input<A::Message>>& bag)override { external_transition(0,bag); }
    std::unique_ptr<devs::Atomic<A::Message>> clone()const override { return std::make_unique<Receiver>(*this); }
private: bool* fail_;
};
int main() {
 try {
    auto a=make();a.external_transition_at(.5,.5,{{0,2,ping(.5)}});
    require(value(a.store(),0)==122 && value(a.store(),1)==1,"broadcast/reply rounds differ");
    auto result=std::get<A::Result>(a.output()[0].value);
    require(result.deliveries.size()==4 && result.deliveries[2].round==1 && result.deliveries[2].sender.id==0 && result.deliveries[3].sender.id==1,"delivery trace differs");
    require(a.delivered_count()==4 && a.next_message_sequence(0)==1 && a.next_message_sequence(1)==1,"reply counter differs");
    require(a.output().size()==1 && a.delivered_count()==4,"output redelivered");
    a.internal_transition();auto clone=a;
    a.external_transition_at(.75,.25,{{0,2,ping(.75)}});
    require(clone.delivered_count()==4 && a.delivered_count()==8,"clone shares broker/counters");
    auto overflow=make(1);rejects([&] { overflow.external_transition_at(.5,.5,{{0,2,ping(.5)}}); });
    require(overflow.now()==0 && overflow.delivered_count()==0 && overflow.next_message_sequence(0)==0 && value(overflow.store(),0)==0,"overflow failed rollback");
    auto bad=make();auto foreign=ping(.5);foreign.sender.store=9;
    rejects([&] { bad.external_transition_at(.5,.5,{{0,2,foreign}}); });
    rejects([&] { bad.external_transition_at(.5,.5,{{0,2,ping(.5)},{0,2,ping(.5)}}); });
    require(bad.now()==0 && bad.delivered_count()==0,"invalid input partly committed");
    auto loop=make(8,3);auto c=config(8,3);c.handler=[](const A::Delivery& d,const Store&) { A::TopicEffects e;e.publications.push_back({"ping",d.receiver,1});return e; };loop.configure_topics(c);
    rejects([&] { loop.external_transition_at(.5,.5,{{0,2,ping(.5)}}); });require(loop.next_message_sequence(0)==0 && loop.now()==0,"cycle consumed message IDs");
    bool fail=true;auto retry=make();auto rc=config();auto handler=rc.handler;
    rc.handler=[&](const A::Delivery& d,const Store& s) { if(fail && d.topic=="pong" && d.sender.id==1) throw std::runtime_error("injected failure");return handler(d,s); };retry.configure_topics(rc);
    rejects([&] { retry.external_transition_at(.5,.5,{{0,2,ping(.5)}}); });fail=false;
    retry.external_transition_at(.5,.5,{{0,2,ping(.5)}});
    require(retry.next_message_sequence(0)==1 && value(retry.store(),0)==122,"retry differs");
    // Same-time due timer, direct input, topics, immediate timer, then publication.
    A::Async async(initial(),[](const auto& t,const Store& s) { A::Effects e;e.updates.push_back({t.agent,{value(s,t.agent.id)*10+1}});return e; });
    async.schedule({1,{7,0},"due"});
    A ordered(async,[](const auto& cmd,const Store& s) { A::Effects e;e.updates.push_back({cmd.agent,{value(s,cmd.agent.id)*10+2}});return e; });
    auto oc=config();oc.handler=[](const A::Delivery& d,const Store& s) { A::TopicEffects e;e.effects.updates.push_back({d.receiver,{value(s,d.receiver.id)*10+3}});e.effects.schedules.push_back({d.time,d.receiver,"immediate"});return e; };ordered.configure_topics(oc);
    auto direct=ping(1);direct.receiver=Store::Reference{7,0};
    ordered.confluent_transition({{0,2,direct},{0,0,A::Command{1,0,{7,0},"input"}}});
    require(value(ordered.store(),0)==1231,"topic/timer confluence differs");
    // A phase failure after successful delivery must restore broker identities too.
    A::Sync sync(initial());sync.add_phase([](auto,const auto&)->Store::Record { throw std::runtime_error("phase"); });
    A phase(sync,1);phase.configure_topics(config());rejects([&] { phase.confluent_transition({{0,2,ping(1)}}); });
    require(phase.now()==0 && phase.delivered_count()==0 && phase.next_message_sequence(0)==0,"later phase failure committed broker");
    bool receiver_fail=true;Sim simulator;
    const auto source=simulator.add(std::make_unique<A>(make()));
    const auto receiver=simulator.add(std::make_unique<Receiver>(&receiver_fail));
    simulator.connect(source,1,receiver,0);
    simulator.inject(.5,source,2,ping(.5));
    simulator.step_transactional();
    rejects([&] { simulator.step_transactional(); });
    const auto& committed=dynamic_cast<const A&>(simulator.model(source));
    require(committed.delivered_count()==4 && committed.next_message_sequence(0)==1 && committed.output().size()==1,"downstream failure lost pending trace");
    require(dynamic_cast<const Receiver&>(simulator.model(receiver)).deliveries==0,"receiver rollback failed");
    receiver_fail=false;simulator.step_transactional();
    require(dynamic_cast<const Receiver&>(simulator.model(receiver)).deliveries==4 && committed.delivered_count()==4,"publication retry redelivered topics");
    auto zero=make();zero.external_transition_at(0,0,{{0,2,ping(0)}});zero.internal_transition();
    rejects([&] { zero.configure_topics(config()); });
    auto lifecycle=make();auto lc=config();lc.handler=[](const A::Delivery& d,const Store&) { A::TopicEffects e;e.effects.retirements.push_back(d.receiver);return e; };lifecycle.configure_topics(lc);
    rejects([&] { lifecycle.external_transition_at(.5,.5,{{0,2,ping(.5)}}); });require(lifecycle.store().active_count()==2,"topic retirement committed");
    // External declaration order cannot change the complete delivery trace.
    auto left=make(),right=make();auto second=ping(.5,1);second.sender={7,1};
    left.external_transition_at(.5,.5,{{0,2,ping(.5)},{0,2,second}});
    right.external_transition_at(.5,.5,{{0,2,second},{0,2,ping(.5)}});
    require(std::get<A::Result>(left.output()[0].value).deliveries==std::get<A::Result>(right.output()[0].value).deliveries,"input permutation changed trace");
    using Wider=std::variant<A::Command,A::Result,int>;
    abm::PopulationAtomic<Agent,Wider> compatible(A::Sync(initial()),1);
    compatible.internal_transition();require(!compatible.output().empty(),"legacy wider variant compatibility failed");
    std::cout<<"Population topic delivery, confluence, cycles, counters and rollback passed\n";
 } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
