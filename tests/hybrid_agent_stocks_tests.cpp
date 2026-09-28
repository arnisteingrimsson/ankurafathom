#include "ankurafathom/hybrid/agent_stocks.hpp"
#include <iostream>

struct Person {};
using Model=ankurafathom::hybrid::AgentStocks<Person>;
using E=Model::Endpoint;
using Store=Model::Store;
using Ref=Model::Reference;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    bool caught=false;
    try { f(); } catch(const std::exception&) { caught=true; }
    require(caught,"invalid agent stock operation accepted");
}
void close(double actual,double expected) {
    require(std::abs(actual-expected)<=2e-12*std::max(1.,std::abs(expected)),"agent stock numeric mismatch");
}
Store people(std::vector<double> values,std::uint64_t first=0) {
    Store s(17,{{"work",ankurafathom::des::FieldKind::real},
                {"done",ankurafathom::des::FieldKind::real},
                {"label",ankurafathom::des::FieldKind::string},
                {"level",ankurafathom::des::FieldKind::integer}},first);
    for(double value:values) s.spawn({value,0.,std::string("consultant"),std::int64_t{3}});
    return s;
}
double work(Ref ref,const Store& s) { return std::get<double>(s.field(ref,0)); }
void snapshot_and_conservation() {
    auto store=people({2,4,100},Store::max_entity_id-2);
    const auto first=store.first_id();
    store.retire({17,first+2});
    store.configure_network(ankurafathom::abm::CsrNetwork({first,first+1},{{first,first+1}},false));
    Model m(store,{{1},{0}},{{"output",0}}); // Reordered binding list is intentional.
    m.add_agent_flow(E::agent(0),E::agent(1),[](Ref r,const Store& s,const auto&,double) { return .5*work(r,s); });
    m.add_global_flow(E::boundary(),E::global(0),[](const Store& s,const auto&,double) { return Model::sum(s,1); });
    m.step(.5);
    close(m.sum(0),4.5); close(m.sum(1),1.5); close(m.global_state()[0],0); // No retroactive publication.
    m.step(.5);
    close(m.sum(0),3.375); close(m.sum(1),2.625); close(m.global_state()[0],.75);
    close(m.sum(0)+m.sum(1),6);
    require(m.store().network()->edges()==store.network()->edges(),"continuous integration changed topology");
    require(m.store().live_rows()==store.live_rows() && m.store().next_id()==store.next_id(),"membership changed");
    require(m.store().field({17,first},2)==store.field({17,first},2) &&
            m.store().field({17,first},3)==store.field({17,first},3),"nonstock fields changed");
    auto independent=m;
    independent.step(.5);
    close(m.time(),1);close(m.sum(0),3.375);
    m.step(.5);
    require(m.global_state()==independent.global_state() && m.sum(0)==independent.sum(0),"copy replay differs");
    rejects([&] { m.add_global_flow(E::boundary(),E::global(0),[](const auto&,const auto&,double) { return 1.; }); });
}
void homogeneous_equivalence() {
    // Global reservoir -> agent work -> global completed, with feedback from
    // the shared reservoir. No same-step consumption of freshly supplied work.
    for(const std::size_t n:{1,3,16}) for(const double dt:{.25,.125,.0625}) {
        Model m(people(std::vector<double>(n,2)),{{0}},{{"reservoir",4.*n},{"completed",0}});
        m.add_agent_flow(E::global(0),E::agent(0),[n](Ref,const Store&,const auto& s,double) { return .5*s[0]/n; });
        m.add_agent_flow(E::agent(0),E::global(1),[](Ref r,const Store& s,const auto&,double) { return .25*work(r,s); });
        ankurafathom::sd::Model pure;
        pure.add_stock("reservoir",4.*n);pure.add_stock("work",2.*n);pure.add_stock("completed",0);
        pure.add_flow(0,1,[](const auto& s,double) { return .5*s[0]; });
        pure.add_flow(1,2,[](const auto& s,double) { return .25*s[1]; });
        for(int i=0;i<64;++i) {
            pure.step(m.time(),dt);m.step(dt);
            close(m.global_state()[0],pure.state()[0]);close(m.sum(0),pure.state()[1]);
            close(m.global_state()[1],pure.state()[2]);
            close(m.global_state()[0]+m.sum(0)+m.global_state()[1],6.*n);
            for(std::size_t j=0;j<n;++j) close(work({17,j},m.store()),m.sum(0)/n);
        }
    }
}
void rollback_and_validation() {
    Model overshoot(people({1,2}),{{0}},{{"done",0}});
    overshoot.add_agent_flow(E::agent(0),E::global(0),[](Ref r,const Store& s,const auto&,double) { return work(r,s); });
    rejects([&] { overshoot.step(2); });
    require(overshoot.time()==0 && overshoot.sum(0)==3 && overshoot.global_state()[0]==0,"overshoot was partially committed");
    // Failure does not freeze configuration; successful retry does.
    overshoot.add_global_flow(E::boundary(),E::global(0),[](const auto&,const auto&,double) { return 0.; });
    overshoot.step(.5);close(overshoot.sum(0),1.5);close(overshoot.global_state()[0],1.5);
    for(double dt:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { overshoot.step(dt); });
    close(overshoot.time(),.5);

    Model throwing(people({1,2}),{{0}},{{"done",0}});
    throwing.add_agent_flow(E::agent(0),E::global(0),[](Ref r,const Store&,const auto&,double) {
        if(r.id==1) throw std::runtime_error("later agent failed");
        return 1.;
    });
    rejects([&] { throwing.step(.25); });
    require(throwing.time()==0 && throwing.sum(0)==3 && throwing.global_state()[0]==0,"callback failure changed state");
    rejects([&] { throwing.step(.25); }); // Busy guard must have unwound.

    Model recursive(people({1}),{{0}});
    recursive.add_agent_flow(E::agent(0),E::boundary(),[&recursive](Ref,const Store&,const auto&,double) {
        recursive.step(.1);return 0.;
    });
    rejects([&] { recursive.step(.25); });
    require(recursive.time()==0 && recursive.sum(0)==1,"reentrant step changed state");

    const auto maximum=std::numeric_limits<double>::max();
    Model overflow(people({maximum,maximum}),{{0}},{{"done",0}});
    rejects([&] { (void)overflow.sum(0); });
    overflow.add_agent_flow(E::agent(0),E::global(0),[maximum](Ref,const Store&,const auto&,double) { return maximum; });
    rejects([&] { overflow.step(.25); }); // Summed global derivative overflows.
    require(overflow.time()==0 && overflow.global_state()[0]==0,"overflow changed state");
    Model clock(people({1}),{{0}});
    clock.step(maximum);
    rejects([&] { clock.step(1); });rejects([&] { clock.step(maximum); });
    require(clock.time()==maximum,"failed clock advance changed time");

    rejects([] { Model(people({1}),{}); });
    rejects([] { Model(people({1}),{{0},{0}}); });
    rejects([] { Model(people({1}),{{2}}); });
    rejects([] { Model(people({1}),{{9}}); });
    rejects([] { Model(people({-1}),{{0}}); });
    rejects([] { Model(people({1}),{{0}},{{"x",0},{"x",1}}); });
    Model endpoints(people({1}),{{0}},{{"x",0}});
    const auto ar=[](Ref,const Store&,const auto&,double) { return 0.; };
    const auto gr=[](const Store&,const auto&,double) { return 0.; };
    rejects([&] { endpoints.add_agent_flow(E::agent(1),E::boundary(),ar); });
    rejects([&] { endpoints.add_agent_flow(E::agent(0),E::global(1),ar); });
    rejects([&] { endpoints.add_agent_flow(E::global(0),E::boundary(),ar); });
    rejects([&] { endpoints.add_agent_flow(E::agent(0),E::boundary(),{}); });
    rejects([&] { endpoints.add_global_flow(E::agent(0),E::global(0),gr); });
    rejects([&] { endpoints.add_global_flow(E::boundary(),E::boundary(),gr); });
    rejects([&] { endpoints.add_global_flow(E::boundary(),E::global(0),{}); });
    rejects([&] { endpoints.add_agent_flow({E::Kind::boundary,1},E::agent(0),ar); });
    rejects([&] { endpoints.add_agent_flow({static_cast<E::Kind>(99),0},E::agent(0),ar); });
    rejects([&] { (void)endpoints.sum(2); });
    for(double bad:{-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}) {
        Model m(people({1}),{{0}});
        m.add_agent_flow(E::boundary(),E::agent(0),[bad](Ref,const Store&,const auto&,double) { return bad; });
        rejects([&] { m.step(.25); });require(m.sum(0)==1 && m.time()==0,"invalid rate changed state");
    }
}
void jacobi_signed_empty() {
    Model m(people({1,3}),{{0,false}});
    m.add_agent_flow(E::boundary(),E::agent(0),[](Ref r,const Store& s,const auto&,double t) {
        return Model::sum(s,0)-3*work(r,s)+t;
    },false);
    m.step(1);
    close(work({17,0},m.store()),2);close(work({17,1},m.store()),-2);
    m.step(.5);
    close(work({17,0},m.store()),-.5);close(work({17,1},m.store()),1.5);
    Model empty(people({}),{{0}},{{"time_integral",0}});
    empty.add_agent_flow(E::boundary(),E::agent(0),[](Ref,const Store&,const auto&,double)->double {
        throw std::runtime_error("empty population evaluated an agent rate");
    });
    empty.add_global_flow(E::boundary(),E::global(0),[](const Store& s,const auto&,double) { return 1+Model::sum(s,0); });
    empty.step(.25);close(empty.sum(0),0);close(empty.global_state()[0],.25);
}
int main() {
    try {
        snapshot_and_conservation();homogeneous_equivalence();rollback_and_validation();jacobi_signed_empty();
        std::cout<<"Agent stocks: simultaneous snapshots, SD equivalence, conservation, copy, topology, rollback and domains passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
