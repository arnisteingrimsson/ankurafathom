#include "ankurafathom/des/reference_delay.hpp"
#include "ankurafathom/hybrid/event_to_stock_pulse.hpp"
#include "ankurafathom/hybrid/signal_sd.hpp"
#include <iostream>

struct Engagement {};
using Store=ankurafathom::des::EntityStore<Engagement,double,double>;
using Token=ankurafathom::des::EntityToken<Engagement>;
using Pulse=ankurafathom::hybrid::StockPulse;
using Message=std::variant<Token,ankurafathom::des::QueuePull,Pulse,ankurafathom::hybrid::ScalarPublication>;
using Bridge=ankurafathom::hybrid::EventToStockPulse<Token,Message>;
using SD=ankurafathom::hybrid::SignalSD<Message>;
int main() {
    // The projection owns an immutable copy of the typed hours/rate records.
    Store jobs(7);
    auto a=jobs.spawn({3,100}),b=jobs.spawn({2,150}),c=jobs.spawn({4,125});
    auto pulse=std::make_unique<Bridge>("completed",2,[](const Token& t) { return t.entity.id; },
        [jobs](const Token& t,double) {
            const double hours=jobs.field<0>(t.entity),rate=jobs.field<1>(t.entity);
            return std::vector<double>{hours,hours*rate};
        });
    ankurafathom::devs::Simulator<Message> sim;
    auto delay=sim.add(std::make_unique<ankurafathom::des::ReferenceDelay<Engagement,Message>>(7,.375));
    auto bridge=sim.add(std::move(pulse));
    auto stock=sim.add(std::make_unique<SD>(std::vector<SD::Stock>{{"completed_hours",0},{"revenue",0}},
        std::vector<SD::Flow>{},std::vector<double>{},.5,std::vector<std::string>{"completed"}));
    sim.connect(delay,1,bridge,0);sim.connect(bridge,1,stock,SD::pulse_port);
    sim.inject(0,delay,0,Message{Token{a}});sim.inject(.125,delay,0,Message{Token{b}});
    sim.inject(.625,delay,0,Message{Token{c}});
    std::cout<<"time,completed_hours,revenue\n";
    for(double time:{0.,.375,.5,1.}) {
        sim.run_until_transactional(time);
        const auto& sd=dynamic_cast<const SD&>(sim.model(stock));
        std::cout<<time<<','<<sd.state()[0]<<','<<sd.state()[1]<<'\n';
    }
}
