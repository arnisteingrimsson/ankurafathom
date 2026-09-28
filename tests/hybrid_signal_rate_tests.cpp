#include "ankurafathom/hybrid/signal_rate_source.hpp"
#include "ankurafathom/hybrid/signal_select.hpp"
#include "ankurafathom/hybrid/publishing_signal_sd.hpp"
#include <array>
#include <iostream>

struct Job {};
using Token=ankurafathom::des::EntityToken<Job>;
using Scalar=ankurafathom::hybrid::ScalarPublication;
using Pulse=ankurafathom::hybrid::StockPulse;
using M=std::variant<Token,Scalar,Pulse>;
using Source=ankurafathom::hybrid::SignalRateSource<Job,M>;
using Select=ankurafathom::hybrid::SignalSelect<Job,M>;
using SD=ankurafathom::hybrid::SignalSD<M>;
using Publisher=ankurafathom::hybrid::PublishingSignalSD<M>;
using Input=ankurafathom::devs::Input<M>;
using Sim=ankurafathom::devs::Simulator<M>;
void require(bool ok,const char* why) { if(!ok) throw std::runtime_error(why); }
template<class F> void rejects(F f) { bool caught=false;try { f(); }catch(const std::exception&) { caught=true; }require(caught,"invalid signal/rate operation accepted"); }
Source::Store records(std::uint64_t first=0) {
    return Source::Store(7,{{"arrival",ankurafathom::des::FieldKind::real},{"mark",ankurafathom::des::FieldKind::real}},first);
}
Source source(double initial=0,std::size_t limit=4,std::uint64_t first=0) {
    return Source(records(first),{initial,2},[](const auto& v,double) { return v[0]; },
        [](auto,const auto& v,double t) { return Source::Record{t,v[1]}; },{123,0,0,7},limit);
}
Input signals(double time,std::uint64_t revision,std::vector<double> values,std::uint32_t port=0,std::size_t owner=7) {
    return {owner,port,M{Scalar{time,revision,std::move(values)}}};
}
void rate_contract() {
    auto s=source();
    const auto h=s.remaining_hazard();
    require(h>0 && std::isinf(*s.next_event_time()) && s.generated_count()==0,"initial paused source");
    s.external_transition_at(.25,.25,{signals(.25,0,{2,3})});
    const auto first=.25+h/2;
    require(s.next_event_time()==first,"initial hazard to deadline");
    const double split=.25+(first-.25)/4;
    s.external_transition_at(split,split-.25,{signals(split,1,{0,4})});
    const auto residual=h-2*(split-.25);
    require(s.remaining_hazard()==residual && std::isinf(*s.next_event_time()),"pause redrew hazard");
    s.external_transition_at(split+.5,.5,{signals(split+.5,2,{4,5})});
    const double next=split+.5+residual/4;
    require(s.next_event_time()==next,"resume lost residual hazard");
    auto token=std::get<Token>(s.output()[0].value);
    require(token.entity.id==0 && token.snapshot->time==next && std::get<double>(token.snapshot->record[1])==5,"arrival factory inputs");
    require(s.generated_count()==0,"output allocated authoritative record");
    auto copy=s.clone();s.confluent_transition({signals(next,3,{0,9})});
    require(s.generated_count()==1 && s.time()==next && std::get<double>(s.store().field({7,0},1))==5 && s.signals()[1]==9,"arrival/update confluence used new factory fields");
    require(dynamic_cast<const Source&>(*copy).generated_count()==0,"source clone aliases allocation");
    const auto before=s.remaining_hazard();
    std::vector<Input> invalid{
        signals(next+.25,3,{1,1}),signals(next+.25,4,{1,1},0,8),signals(next+.25,4,{1}),
        signals(next+.25,4,{-1,1}),signals(next+.25,4,{NAN,1}),signals(next+.25,4,{1,1},2),
        signals(next+.5,4,{1,1})
    };
    for(const auto& input:invalid) {
        rejects([&] { s.external_transition_at(next+.25,(next+.25)-next,{input}); });
        require(s.generated_count()==1 && s.time()==next && s.remaining_hazard()==before && s.revision()==3,"invalid source input changed state");
    }
    rejects([&] { s.external_transition_at(next+.25,.125,{signals(next+.25,4,{1,1})}); });
    rejects([&] { s.external_transition_at(next+.25,(next+.25)-next,{}); });
    s.external_transition_at(next+.25,(next+.25)-next,{signals(next+.25,4,{2,10})});
    rejects([&] { s.external_transition_at(*s.next_event_time(),*s.next_event_time()-s.time(),{signals(*s.next_event_time(),5,{0,0})}); });
    while(s.generated_count()<4) s.internal_transition();
    require(std::isinf(*s.next_event_time()),"source limit ignored");
    const auto end=s.time();s.external_transition_at(end+.5,(end+.5)-end,{signals(end+.5,5,{1,0})});
    require(s.generated_count()==4 && std::isinf(*s.next_event_time()),"exhausted source restarted");
    auto empty=source(1,0);require(empty.output().empty(),"zero source limit");
    auto high=source(1,1,Source::Store::max_entity_id);high.internal_transition();
    require(high.store().next_id()==Source::Store::max_entity_id+1,"last source identity allocation");
    rejects([] { (void)source(1,2,Source::Store::max_entity_id); });
    rejects([] { (void)source(1,1000001); });
    auto unchanged=source(1);const auto deadline=*unchanged.next_event_time();
    unchanged.external_transition_at(deadline/4,deadline/4,{signals(deadline/4,0,{1,99})});
    require(unchanged.next_event_time()==deadline,"unchanged rate moved arrival deadline");
    Source broken(records(),{1,0},[](const auto& v,double) { return v[0]; },[](auto,const auto&,double) { return Source::Record{NAN,1.}; },{123,0,0,7},1);
    rejects([&] { (void)broken.output(); });rejects([&] { broken.internal_transition(); });
    require(broken.generated_count()==0 && broken.time()==0,"invalid factory allocated record");
    // Invalid following rate must restore the confluent arrival too.
    auto confluent=source(1);const auto due=*confluent.next_event_time();
    rejects([&] { confluent.confluent_transition({signals(due,0,{-1,0})}); });
    require(confluent.generated_count()==0 && confluent.time()==0 && confluent.next_event_time()==due,"bad confluence consumed arrival");
    auto unreachable=source();
    rejects([&] { unreachable.external_transition_at(std::numeric_limits<double>::max(),std::numeric_limits<double>::max(),
        {signals(std::numeric_limits<double>::max(),0,{1,0})}); });
    require(unreachable.time()==0 && !unreachable.revision(),"unrepresentable arrival committed rate update");
    for(auto draws:{Source::Draws{1,65536,0,0},Source::Draws{1,0,65536,0},Source::Draws{1,0,0,65536}})
        rejects([&] { Source(records(),{1},[](const auto& v,double) { return v[0]; },[](auto,const auto&,double t) { return Source::Record{t,0.}; },draws,1); });
    auto fractional=source();fractional.external_transition_at(.1,.1,{signals(.1,0,{0,1})});
    fractional.external_transition_at(.3,.3-.1,{signals(.3,1,{0,1})});require(fractional.time()==.3,"source absolute timestamp drift");
}
void selector_contract() {
    auto make=[] { return Select(7,{0},[](const auto& v,double) { return std::vector<double>{v[0],1-v[0]}; },{123,0,0,8}); };
    Token token{{7,9},-2,{{11,9,1}},ankurafathom::des::EntitySnapshot<Job>{0,records().schema(),{2.,3.}}};
    const std::array<Input,2> bag{{{9,0,M{token}},signals(.25,0,{1},2)}};
    for(bool reverse:{false,true}) {
        auto select=make();select.external_transition_at(.25,.25,reverse?std::vector<Input>{bag[1],bag[0]}:std::vector<Input>{bag[0],bag[1]});
        require(select.output().size()==1 && select.output()[0].port==2 && std::get<Token>(select.output()[0].value)==token,"simultaneous route retroactively used new probabilities");
        auto next=token;next.entity.id=10;
        select.confluent_transition({{9,0,M{next}}});
        require(select.output()[0].port==1 && select.counts()==std::vector<std::size_t>({1,1}),"next microstep did not use new probability");
        select.internal_transition();
        const auto before=select.counts();
        for(auto input:{signals(.5,0,{.5},2),signals(.5,1,{.5},2,8),signals(.5,1,{2},2),signals(.5,1,{.5,.5},2)}) {
            rejects([&] { select.external_transition(.25,{input}); });
            require(select.counts()==before && select.time()==.25 && select.revision()==0,"bad selector update committed");
        }
        rejects([&] { select.external_transition(.25,{{9,0,M{token}}}); });
        auto copy=select.clone();next.entity.id=11;
        select.external_transition(.25,{{9,0,M{next}},signals(.5,1,{0},2)});
        require(std::get<Token>(select.output()[0].value).snapshot==token.snapshot && dynamic_cast<const Select&>(*copy).received_count()==2,"selector mutated snapshot or clone");
    }
    auto ordered=make();ordered.external_transition(0,{{9,0,M{Token{{7,3}}}},{9,0,M{Token{{7,1}}}},{9,0,M{Token{{7,2}}}}});
    for(std::size_t i=0;i<3;++i) require(std::get<Token>(ordered.output()[i].value).entity.id==i+1,"selector input ordering");
    auto fail=make();Token one{{7,1}},two{{7,2}};
    rejects([&] { fail.external_transition(0,{{9,0,M{one}},{9,0,M{two}},signals(0,0,{2},2)}); });
    require(fail.received_count()==0 && fail.output().empty(),"failed probability projection consumed tokens");
    fail.external_transition(0,{{9,0,M{one}},{9,0,M{two}}});require(fail.received_count()==2,"selector retry poisoned IDs");
    Select width(7,{0},[](const auto& v,double) { return v[0]==0?std::vector<double>{1,0}:std::vector<double>{1}; });
    rejects([&] { width.external_transition(.5,{signals(.5,0,{1},2)}); });
    require(width.counts().size()==2 && width.time()==0,"branch count changed");
}
Publisher publisher() {
    SD core({{"x",1}},{{SD::boundary,0,[](const auto&,const auto& v,double) { return v[0]; }}},{2},.5,{"p"});
    return Publisher(std::move(core),[](const auto& s,const auto& v,double t) { return std::vector<double>{s[0]+v[0],t}; });
}
void publisher_contract() {
    rejects([] { Publisher(SD({{"x",0}},{},{0},1),{}); });
    rejects([] { Publisher(SD({{"x",0}},{},{0},1),[](const auto&,const auto&,double) { return std::vector<double>{}; }); });
    auto p=publisher();
    require(std::get<Scalar>(p.output()[0].value)==Scalar{0,0,{3,0}},"SD initial projection");
    auto copy=p.clone();p.internal_transition();p.internal_transition();
    require(std::get<Scalar>(p.output()[0].value)==Scalar{.5,1,{4,.5}},"SD committed tick projection");
    require(std::get<Scalar>(copy->output()[0].value).revision==0,"SD clone aliases projection");
    p.confluent_transition({signals(.5,0,{4})});
    require(std::get<Scalar>(p.output()[0].value)==Scalar{.5,2,{6,.5}} && p.core().state()[0]==2,"SD pending-publication confluence integrated twice");
    p.internal_transition();p.external_transition_at(.75,.25,{{7,SD::pulse_port,M{Pulse{.75,"p",0,{3}}}}});
    require(p.core().state()[0]==6 && std::get<Scalar>(p.output()[0].value).values[0]==10,"pulse not reflected in SD projection");
    p.internal_transition();p.confluent_transition({signals(1,1,{1})});
    require(p.core().state()[0]==7 && p.core().next_event_time()==1.5,"SD tick/input confluence");
    Publisher fail(SD({{"x",0}},{{SD::boundary,0,[](const auto&,const auto&,double) { return 1.; }}},{0},.5),
        [](const auto& state,const auto&,double) { return state[0]==0?std::vector<double>{0}:std::vector<double>{NAN}; });
    fail.internal_transition();rejects([&] { fail.internal_transition(); });
    Publisher width(SD({{"x",0}},{{SD::boundary,0,[](const auto&,const auto&,double) { return 1.; }}},{0},.5),
        [](const auto& state,const auto&,double) { return state[0]==0?std::vector<double>{0}:std::vector<double>{0,0}; });
    width.internal_transition();rejects([&] { width.internal_transition(); });
    require(width.core().state()[0]==0 && width.revision()==0,"projection width change committed integration");
    require(fail.core().time()==0 && fail.core().state()[0]==0 && fail.revision()==0,"failed projection committed SD tick");
}
class Failing final:public ankurafathom::devs::Atomic<M> {
public:
    std::unique_ptr<ankurafathom::devs::Atomic<M>> clone()const override { return std::make_unique<Failing>(*this); }
    double time_advance()const override { return INFINITY; }
    std::vector<ankurafathom::devs::PortValue<M>> output()const override { return {}; }
    void internal_transition()override {}
    void external_transition(double,const std::vector<Input>&)override { throw std::runtime_error("downstream failure"); }
    void confluent_transition(const std::vector<Input>& bag)override { external_transition(0,bag); }
};
void checked_retry() {
    Sim sim;const auto sid=sim.add(std::make_unique<Source>(source(1,1)));
    const auto router=sim.add(std::make_unique<Select>(7,std::vector<double>{1},[](const auto& v,double) { return std::vector<double>{v[0],1-v[0]}; }));
    const auto fail=sim.add(std::make_unique<Failing>());
    sim.connect(sid,1,router,0);sim.connect(sid,1,fail,0);
    const auto due=sim.next_time();
    rejects([&] { sim.step_transactional(); });
    require(sim.next_time()==due && dynamic_cast<const Source&>(sim.model(sid)).generated_count()==0 && dynamic_cast<const Select&>(sim.model(router)).received_count()==0,"arrival delivery failed without rollback");
    sim.disconnect(sid,1,fail,0);sim.connect(router,1,fail,0);sim.step_transactional();
    rejects([&] { sim.step_transactional(); });
    require(dynamic_cast<const Source&>(sim.model(sid)).generated_count()==1 && dynamic_cast<const Select&>(sim.model(router)).received_count()==1 && sim.model(router).output().size()==1,"routing failure lost committed arrival/pending output");
    sim.disconnect(router,1,fail,0);sim.run_until_transactional(due);
    require(dynamic_cast<const Select&>(sim.model(router)).counts()==std::vector<std::size_t>({1,0}),"routing retry duplicated draw");
    Sim sd;const auto pub=sd.add(std::make_unique<Publisher>(publisher()));
    const auto source_id=sd.add(std::make_unique<Source>(source()));const auto bad=sd.add(std::make_unique<Failing>());
    sd.connect(pub,1,source_id,0);sd.connect(pub,1,bad,0);
    rejects([&] { sd.step_transactional(); });
    require(sd.model(pub).output().size()==1 && !dynamic_cast<const Source&>(sd.model(source_id)).revision(),"failed scalar delivery consumed revision");
    sd.disconnect(pub,1,bad,0);sd.run_until_transactional(0);
    require(dynamic_cast<const Source&>(sd.model(source_id)).rate()==3 && dynamic_cast<const Source&>(sd.model(source_id)).revision()==0,"scalar publication retry changed rate");
    Sim loop;
    auto node=loop.add(std::make_unique<Publisher>(SD({{"x",0}},{},{1},1),[](const auto&,const auto& v,double) { return v; }));
    loop.connect(node,1,node,0);rejects([&] { loop.run_until_transactional(0,5); });
    require(dynamic_cast<const Publisher&>(loop.model(node)).revision()==5 && loop.model(node).output().size()==1,"zero-time feedback did not preserve budget checkpoint");
}
int main() { try { rate_contract();selector_contract();publisher_contract();checked_retry();std::cout<<"Scalar SD publication, rate source and routing contracts passed\n"; }catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; } }
