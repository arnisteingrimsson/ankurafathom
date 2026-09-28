#include "ankurafathom/abm/population_atomic.hpp"
#include "ankurafathom/abm/statechart.hpp"
#include <iostream>
using namespace ankurafathom;
struct Agent {};
using A=abm::PopulationAtomic<Agent>;using Store=A::Store;using Chart=abm::Statechart<Agent>;
void require(bool b,const char* m) { if(!b) throw std::runtime_error(m); }
template<class F> void rejects(F f) { bool b=false;try { f(); } catch(const std::exception&) { b=true; } require(b,"invalid operation accepted"); }
Store fresh() { Store s(7,{{"n",des::FieldKind::integer}});s.spawn_many({{std::int64_t(1)},{std::int64_t(2)}});return s; }
std::int64_t n(const Store& s,std::uint64_t id=0) {return std::get<std::int64_t>(s.field({7,id},0));}
A::TopicConfig config(std::size_t capacity=20) {
 A::TopicConfig c;c.capacities={{"count",capacity}};
 c.handler=[](const A::Delivery& d,const Store& s) { A::TopicEffects e;e.effects.updates.push_back({d.receiver,{n(s,d.receiver.id)+std::int64_t(d.value)}});return e; };return c;
}
A::Sync phases() {
 A::Sync p(fresh());
 p.add_phase([](auto r,const Store& s) {return Store::Record{n(s,r.id)+10};},[](auto r,const Store& s) {return std::vector<A::Emission>{{"count",r,double(n(s,r.id))}};});
 p.add_phase([](auto r,const Store& s) {return Store::Record{n(s,r.id)*2};},[](auto r,const Store& s) {return std::vector<A::Emission>{{"count",r,double(n(s,r.id))}};});return p;
}
int main() {try {
 auto direct=phases();direct.step();require(n(direct.store())==22 && direct.pending_publications().size()==4,"phase emission changed Jacobi updates");
 require(direct.pending_publications()[0].value==1 && direct.pending_publications()[2].value==11,"phase publisher did not see pre-phase snapshot");
 auto copy=direct;auto out=direct.take_publications();require(out.size()==4 && direct.pending_publications().empty() && copy.pending_publications().size()==4,"outbox copies share state");
 rejects([&] { A invalid(copy,1); });
 auto failed=phases();failed.add_phase([](auto,const Store&)->Store::Record {throw std::runtime_error("last phase");});
 rejects([&] {failed.step();});require(failed.pending_publications().empty() && n(failed.store())==1,"phase failure leaked outbox");
 A a(phases(),1);a.configure_topics(config());a.internal_transition();require(n(a.store())==34 && n(a.store(),1)==38,"tick publications delivered before final phase");
 auto result=std::get<A::Result>(a.output()[0].value);require(result.deliveries.size()==4 && result.deliveries[0].sequence==0 && result.deliveries[1].sequence==1,"generated ordering differs");
 A limited(phases(),1);limited.configure_topics(config(3));rejects([&] {limited.internal_transition();});
 require(n(limited.store())==1 && limited.next_message_sequence(0)==0 && limited.sync().pending_publications().empty(),"broker failure committed phase outbox");
 limited.configure_topics(config());limited.internal_transition();require(n(limited.store())==34,"retry phase publications differ");
 // Due timer emits, external explicit sequence zero remains independent, then immediate timer/publication feedback.
 A::Async p(fresh(),[](const auto& t,const Store& s) {A::Effects e;e.updates.push_back({t.agent,{n(s,t.agent.id)+1}});e.publications.push_back({"count",t.agent,t.agent,2});return e;},3);
 p.schedule({1,{7,0},"fire"});A async(p);auto c=config();
 c.handler=[](const A::Delivery& d,const Store& s) {A::TopicEffects e;e.effects.updates.push_back({d.receiver,{n(s,d.receiver.id)+std::int64_t(d.value)}});if(n(s)<5)e.effects.schedules.push_back({d.time,d.receiver,"fire"});return e;};async.configure_topics(c);
 async.confluent_transition({{0,2,A::TopicInput{1,"count",{7,0},Store::Reference{7,0},0,1}}});
 require(n(async.store())==11 && async.delivered_count()==4,"timer/publication closure differs");
 auto trace=std::get<A::Result>(async.output()[0].value);require(trace.timers.size()==3 && trace.deliveries.front().sequence==0 && trace.deliveries[1].sequence==0,"external/generated batches collided");
 // A timer-message cycle must share the cumulative timer budget across every drain.
 A cycle(p);c.handler=[](const A::Delivery& d,const Store&) {A::TopicEffects e;e.effects.schedules.push_back({d.time,d.receiver,"fire"});return e;};cycle.configure_topics(c);
 rejects([&] {cycle.internal_transition();});require(cycle.now()==0 && cycle.async().pending_count()==1 && cycle.delivered_count()==0 && cycle.next_message_sequence(0)==0,"closure budget did not roll back timestamp");
 // Publication validation is part of native apply, including existing queued endpoints.
 A::Async owner(fresh(),[](const auto&,const Store&) { return A::Effects{}; });
 A::Effects valid;valid.publications.push_back({"count",{7,0},Store::Reference{7,1},1});owner.apply(valid);
 auto saved=owner.pending_publications();auto owner_copy=owner;
 A::Effects invalid;invalid.updates.push_back({{7,0},{std::int64_t(99)}});invalid.publications.push_back({"count",{7,0},std::nullopt,std::numeric_limits<double>::infinity()});
 rejects([&] {owner.apply(invalid);});require(n(owner.store())==1 && owner.pending_publications()==saved,"invalid publication partly committed");
 rejects([&] {owner.retire({7,1});});require(owner.store().alive({7,1}),"retirement invalidated queued receiver");
 require(owner.take_publications()==saved && owner_copy.pending_publications()==saved,"async outbox copies share buffers");
 owner.retire({7,1});require(!owner.store().alive({7,1}),"drained receiver could not retire");
 // A native topic handler's effect publications join the next round before its reply list.
 A effects(A::Sync(fresh()),1);auto ec=config();
 ec.handler=[](const A::Delivery& d,const Store& s) {A::TopicEffects e;e.effects.updates.push_back({d.receiver,{n(s,d.receiver.id)+std::int64_t(d.value)}});
   if(d.value==1) { e.effects.publications.push_back({"count",d.receiver,d.receiver,2});e.publications.push_back({"count",d.receiver,3}); } return e;};
 effects.configure_topics(ec);effects.external_transition_at(.5,.5,{{0,2,A::TopicInput{.5,"count",{7,0},Store::Reference{7,0},0,1}}});
 auto et=std::get<A::Result>(effects.output()[0].value).deliveries;
 require(n(effects.store())==7 && et.size()==3 && et[1].round==1 && et[1].value==2 && et[2].value==3,"effect publication/reply order differs");
 // Chart publishes only selected transition, and its value precedes the action update.
 Store s(7,{{"n",des::FieldKind::integer},{"state",des::FieldKind::integer},{"entered",des::FieldKind::real},{"generation",des::FieldKind::integer}});
 s.spawn({std::int64_t(5),std::int64_t(0),0.,std::int64_t(-1)});
 Chart::Transition t;t.name="work";t.source=0;t.target=0;t.trigger=Chart::Trigger::timeout;t.value=1;
 t.action=[](auto r,const Store& v) {auto record=v.record(r);record[0]=std::int64_t(9);return record;};
 t.publish=[](auto r,const Store& v) {return std::vector<A::Emission>{{"count",r,double(n(v))}};};
 Chart chart(s,{"state","entered","generation"},{0},{t});auto init=chart.start({7,0},s,0,0);require(init.publications.empty(),"initialization published");
 A::Async cp(s,[chart](const auto& timer,const Store& v) {return chart.on_timer(timer,v);});cp.apply(init);
 cp.step();require(n(cp.store())==9 && cp.pending_publications().size()==1 && cp.pending_publications()[0].value==5,"chart publication snapshot differs");
 const auto stale=chart.on_timer({1,{7,0},99,"statechart/work",0},cp.store());require(stale.publications.empty(),"stale timer published");
 t.guard=[](auto,const Store&) {return false;};Chart guarded(s,{"state","entered","generation"},{0},{t});
 A::Async gp(s,[guarded](const auto& timer,const Store& v) {return guarded.on_timer(timer,v);});gp.apply(guarded.start({7,0},s,0,0));gp.step();require(gp.pending_publications().empty(),"false guard published");
 std::cout<<"Generated phase/chart publications, staged outboxes and closure rollback passed\n";
 }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}}
