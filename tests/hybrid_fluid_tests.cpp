#include "ankurafathom/hybrid/models/queue_backlog.hpp"
#include <iostream>
using Model=ankurafathom::hybrid::models::QueueBacklog;
void require(bool v,const char* message) { if(!v) throw std::runtime_error(message); }
template<class F> void rejects(F f) { bool caught=false;try { f(); } catch(const std::exception&) { caught=true; }require(caught,"invalid backlog accepted"); }
int main() {
    try {
        const std::vector<ankurafathom::des::Entity> schedule{{9,0,1},{2,0,.5},{4,1,.5},{5,1.5,.25},{6,3,.5}};
        // This server preserves source bag order for simultaneous arrivals: ID 9 precedes ID 2.
        const std::vector<Model::Observation> expected{{0,2,2,0,1,true},{.5,2,2,0,1,true},
            {1,2,3,1,1,true},{1.5,2,4,2,1,true},{2,1,4,3,0,true},{2.25,0,4,4,0,false},
            {3,1,5,4,0,true},{3.5,0,5,5,0,false},{4,0,5,5,0,false}};
        std::array<std::size_t,5> order{0,1,2,3,4};
        do {
            Model model(schedule,.25,order);
            for(const auto& x:expected) require(model.observe(x.time)==x,"backlog hand schedule/declaration order");
        } while(std::next_permutation(order.begin(),order.end()));
        Model sparse(schedule,.1),dense(schedule,.1);
        for(int k=0;k<=40;++k) {
            const auto a=dense.observe(k*.1);
            if(k%10==0) require(a==sparse.observe(k*.1),"observation density changed backlog");
        }
        Model retry(schedule);
        rejects([&] { retry.observe(0,1); }); // Arrival event committed, bridge publication still pending.
        require(retry.observe(0)==expected[0],"step-budget retry lost a pulse");
        rejects([&] { retry.observe(-1); });
        rejects([&] { retry.observe(std::numeric_limits<double>::infinity()); });
        rejects([&] { retry.observe(1,0); });
        Model empty({});require(empty.observe(2)==Model::Observation{2,0,0,0,0,false},"empty backlog");
        rejects([] { Model({{0,0,0}}); });
        rejects([] { Model({{0,1,1},{1,0,1}}); });
        rejects([] { Model({},0); });
        rejects([] { Model({},.25,{0,1,1,3,4}); });
        const auto maximum=std::numeric_limits<double>::max();
        Model overflow({{0,0,maximum},{1,0,maximum}},maximum);
        const auto before=overflow.observe(0);
        rejects([&] { overflow.observe(maximum); });
        require(overflow.observe(0)==before,"failed completion step changed server or pulse ledger");
        // Off-grid fluid emptying: clipping limits service to initial + incoming material.
        ankurafathom::sd::Model fluid;
        const auto b=fluid.add_stock("backlog",.3,true,true);
        fluid.add_flow(fluid.boundary,b,[](const auto&,double) { return .2; });
        const auto service=fluid.add_flow(b,fluid.boundary,[](const auto&,double) { return 1.; });
        fluid.set_outflow_order(b,{service});
        fluid.step(0,.25);require(std::abs(fluid.state()[b]-.1)<1e-14,"fluid draining");
        fluid.step(.25,.25);require(fluid.state()[b]==0,"fluid reflection at empty boundary");
        fluid.step(.5,.25);require(fluid.state()[b]==0,"empty fluid backlog grew under load below capacity");
        std::cout<<"Backlog: 120 declaration permutations, exact schedules, confluence, density, retry and fluid reflection passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
