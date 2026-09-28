#include "ankurafathom/des/multi_server.hpp"
#include "ankurafathom/des/process.hpp"
#include "ankurafathom/des/routing.hpp"
#include <iostream>

namespace {
using namespace ankurafathom;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void fractional_path(bool single) {
    // Minimized from mixed M/G/1 scenario 22, replication 33. Adding elapsed
    // time after the second completion used to round the third arrival down.
    const std::vector<des::Entity> arrivals{{1,1.8319702089718573,.25},
        {2,2.0199542415267091,.25},{3,6.4067953695328468,.25}};
    devs::Simulator<des::Entity> sim;
    const auto source=sim.add(std::make_unique<des::ScheduledSource<>>(arrivals));
    const auto router=sim.add(std::make_unique<des::PriorityRouter<>>());
    std::unique_ptr<devs::Atomic<des::Entity>> station;
    if (single) station=std::make_unique<des::SingleServer<>>();
    else station=std::make_unique<des::MultiServer<>>(1);
    const auto server=sim.add(std::move(station));
    auto sink=std::make_unique<des::CompletionSink<>>();
    const auto* observed=sink.get();
    const auto sink_id=sim.add(std::move(sink));
    sim.connect(source,0,router,0); sim.connect(router,1,server,0); sim.connect(server,1,sink_id,0);
    const auto trace=sim.run_until_transactional(7);
    require(observed->completed_count()==3,"fractional arrival was lost");
    const std::vector<double> deadlines{arrivals[0].arrived_at+.25,arrivals[0].arrived_at+.5,
                                        arrivals[2].arrived_at+.25};
    for (std::size_t i=0;i<deadlines.size();++i)
        require(observed->completed()[i].completed_at==deadlines[i],"service deadline drifted");
    for (const auto& step:trace) for (const auto& emission:step.emissions) {
        if (emission.source==source || emission.source==router)
            require(step.time==emission.value.arrived_at,"arrival publication moved its absolute time");
        if (emission.source==server)
            require(step.time==emission.value.completed_at,"completion timestamp differs from kernel clock");
    }
}
void immutable_deadline() {
    des::MultiServer<> station(1);
    station.external_transition_at(.1,.1,{{0,0,{1,.1,.7}}});
    const double deadline=.1+.7;
    double previous=.1;
    for (double time:{.13,.19,.27,.41,.58,.69}) {
        station.external_transition_at(time,time-previous,{});
        require(*station.next_event_time()==deadline && station.output()[0].value.completed_at==deadline,
                "unrelated external events changed a service deadline");
        previous=time;
    }
    bool caught=false;
    try { station.external_transition_at(.7,.5,{}); } catch (const std::invalid_argument&) { caught=true; }
    require(caught && *station.next_event_time()==deadline,"inconsistent timestamp committed state");
    station.internal_transition();
    require(station.completed_count()==1,"absolute deadline failed to complete");
    des::MultiServer<> unrepresentable(1);
    caught=false;
    try { unrepresentable.external_transition_at(1e16,1e16,{{0,0,{1,1e16,.25}}}); }
    catch (const std::overflow_error&) { caught=true; }
    require(caught && unrepresentable.accepted_count()==0 && !unrepresentable.busy_servers(),
            "unrepresentable positive service committed a zero-time completion");
}
void source_roundtrip() {
    // Find an IEEE-double timestamp pair whose subtraction/addition is lossy.
    const double first=2.3319702089718573, second=6.4067953695328468;
    require(first+(second-first)!=second,"regression pair no longer exercises lossy clock addition");
    des::ScheduledSource<> source({{1,first,1},{2,second,1}});
    require(*source.next_event_time()==first,"source did not expose absolute arrival");
    source.internal_transition();
    require(*source.next_event_time()==second,"source did not preserve second arrival");
}
}
int main() {
    try { fractional_path(false); fractional_path(true); immutable_deadline(); source_roundtrip();
        std::cout<<"DES absolute clocks: fractional arrivals, routing, deadlines and rollback passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
