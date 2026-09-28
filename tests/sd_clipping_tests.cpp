#include "ankurafathom/sd/model.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

using ankurafathom::sd::Model;
using ankurafathom::sd::Integrator;

void check(bool yes,const char* message) { if (!yes) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    bool caught=false;
    try { f(); } catch (const std::exception&) { caught=true; }
    check(caught,"invalid clipping operation accepted");
}
auto constant(double value) { return [value](const auto&,double){return value;}; }

void priorities_and_conservation() {
    for (bool reverse : {false,true}) {
        Model m;
        auto a=m.add_stock("a",2,true,true), b=m.add_stock("b",0), c=m.add_stock("c",0);
        m.add_flow(Model::boundary,a,constant(3),true,true);
        auto high=m.add_flow(a,b,constant(6),true,true);
        auto low=m.add_flow(a,c,constant(8),true,true);
        m.set_outflow_order(a,reverse ? std::vector<std::size_t>{low,high} : std::vector<std::size_t>{high,low});
        m.step(0,.5);
        check(m.state()[a]==0,"source depleted exactly");
        check(m.state()[b]==(reverse ? 0 : 3) && m.state()[c]==(reverse ? 3.5 : .5),"explicit priority allocation");
        check(m.state()[a]+m.state()[b]+m.state()[c]==3.5,"shared flows must conserve material");
    }
    Model roundoff;
    auto a=roundoff.add_stock("a",.3,true,true), b=roundoff.add_stock("b",0);
    auto f=roundoff.add_flow(a,b,constant(.2));
    auto g=roundoff.add_flow(a,b,constant(.2));
    roundoff.set_outflow_order(a,{f,g});
    roundoff.step(0,1);
    check(roundoff.state()[a]==0 && std::abs(roundoff.state()[b]-.3)<1e-16,"roundoff must not create negative residue");
}

void same_tick_cascade() {
    Model m;
    // Reverse declaration order relative to material dependency order.
    auto c=m.add_stock("c",0), b=m.add_stock("b",0,true,true), a=m.add_stock("a",2,true,true);
    int calls=0;
    auto bc=m.add_flow(b,c,[&](const auto& s,double){++calls;check(s[b]==0,"requested rates must read old state");return 10.;});
    auto ab=m.add_flow(a,b,[&](const auto& s,double){++calls;check(s[a]==2,"requested rates must read old state");return 8.;});
    m.add_flow(Model::boundary,a,constant(3));
    m.set_outflow_order(a,{ab});m.set_outflow_order(b,{bc});
    m.step(0,1);
    check(calls==2 && m.state()[a]==0 && m.state()[b]==0 && m.state()[c]==5,"upstream constrained amount funds downstream in same tick");
}

void signs_domains_and_rollback() {
    Model m;
    auto a=m.add_stock("a",2,true,true), b=m.add_stock("b",0);
    bool fail=true;
    auto out=m.add_flow(a,b,constant(20),true,true);
    m.add_flow(Model::boundary,a,[&](const auto&,double){if(fail) throw std::runtime_error("late failure");return -10.;},true,true);
    m.set_outflow_order(a,{out});
    const auto before=m.state();
    rejects([&]{m.step(0,1);});check(m.state()==before,"failed evaluation changed stocks");
    rejects([&]{m.step(0,1,Integrator::rk4);});check(m.state()==before,"unsupported solver changed stocks");
    rejects([&]{m.derivative(m.state(),0);});
    fail=false;m.step(0,1);
    check(m.state()[a]==0 && m.state()[b]==2,"negative clipped inflow is zero and retry conserves material");
    Model strict;
    auto x=strict.add_stock("x",2), y=strict.add_stock("y",0);
    strict.add_flow(x,y,constant(20));
    rejects([&]{strict.step(0,1);});check(strict.state()[x]==2 && strict.state()[y]==0,"native rejection semantics changed");
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),std::numeric_limits<double>::infinity(),std::numeric_limits<double>::max()}) {
        Model invalid;auto s=invalid.add_stock("s",0);
        invalid.add_flow(Model::boundary,s,constant(bad),true,true);
        rejects([&]{invalid.step(0,2);});check(invalid.state()[s]==0,"nonfinite clipping input changed state");
    }
    Model signed_flow;
    auto s=signed_flow.add_stock("s",3),t=signed_flow.add_stock("t",2);
    signed_flow.add_flow(Model::boundary,s,constant(-9),true,true);
    signed_flow.add_flow(s,t,constant(-1),false);
    signed_flow.step(0,1);
    check(signed_flow.state()[s]==4 && signed_flow.state()[t]==1,"unrelated signed flow changed meaning");
}

void graph_rejections() {
    Model m;auto a=m.add_stock("a",1,true,true),b=m.add_stock("b",0,true,true);
    auto ab=m.add_flow(a,b,constant(0)), ba=m.add_flow(b,a,constant(0));
    m.set_outflow_order(a,{ab});m.set_outflow_order(b,{ba});
    rejects([&]{m.validate_clipping();});rejects([&]{m.step(0,1);});
    Model order;auto s=order.add_stock("s",1,true,true);
    auto f=order.add_flow(s,Model::boundary,constant(1));
    rejects([&]{order.validate_clipping();});
    rejects([&]{order.set_outflow_order(s,{f,f});});
    order.set_outflow_order(s,{f});order.validate_clipping();
    order.add_flow(s,Model::boundary,constant(1));
    rejects([&]{order.step(0,1);});check(order.state()[s]==1,"stale order changed stock");
    Model negative;auto n=negative.add_stock("n",0,true,true);
    negative.add_flow(Model::boundary,n,constant(-1),false);
    rejects([&]{negative.validate_clipping();});
    rejects([]{Model bad;bad.add_stock("bad",1,false,true);});
}

int main() {
    try {priorities_and_conservation();same_tick_cascade();signs_domains_and_rollback();graph_rejections();
         std::cout<<"Euler clipping semantics passed\n";}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
