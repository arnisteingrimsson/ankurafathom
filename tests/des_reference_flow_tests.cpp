#include "ankurafathom/des/reference_flow.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include "ankurafathom/des/reference_queue.hpp"
#include "ankurafathom/des/reference_resource.hpp"
#include <iostream>
#include <numeric>

namespace {
using namespace ankurafathom;
struct Work {};
using Token=des::EntityToken<Work>;
using Message=des::ProcessMessage<Work>;
using Source=des::ReferenceSource<Work>;
using Sink=des::ReferenceSink<Work>;
using Select=des::ReferenceSelect<Work>;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class Function> void rejects(Function function) {
    bool caught=false; try { function(); } catch(const std::exception&) { caught=true; }
    require(caught,"invalid reference flow operation succeeded");
}
Token token(std::uint64_t id,std::int32_t priority=0) { return {{7,id},priority}; }

void categorical_boundaries() {
    const std::vector<unsigned> weights{4,8,16,32,64,64,40,28};
    std::vector<double> probabilities;
    for(auto weight:weights) probabilities.push_back(weight/256.);
    rng::Categorical distribution(probabilities);
    std::vector<std::uint32_t> words{0,0xFFFFFFFFU};
    unsigned sum=0;
    for(std::size_t i=0;i+1<weights.size();++i) {
        sum+=weights[i]; words.push_back((sum<<24)-1); words.push_back(sum<<24);
    }
    for(std::uint64_t id=0;id<8192;++id) words.push_back(rng::draw(314159,{1,2,id,0,100,0})[0]);
    for(auto word:words) {
        const auto bucket=word>>24;
        unsigned cumulative=0; std::size_t expected=0;
        while(bucket>=cumulative+weights[expected]) cumulative+=weights[expected++];
        require(distribution.sample(word)==expected,"categorical selection differs from exact integer partition");
    }
    for(auto word:words) {
        require(rng::Categorical({0,0,1,0}).sample(word)==2,"categorical selected zero mass");
        require(rng::Categorical({1}).sample(word)==0,"single category differs");
    }
    for(auto probabilities:std::vector<std::vector<double>>{{},{0,0},{.2,.3},{1,1},{-.1,1.1},
        {std::numeric_limits<double>::quiet_NaN()},{std::numeric_limits<double>::infinity()}})
        rejects([&] { rng::Categorical invalid(probabilities); });
    rng::Categorical rounded({.25,.75+5e-13});
    require(rounded.sample(0)==0 && rounded.sample(0xFFFFFFFFU)==1,"rounding normalization differs");
}

void source_select_sink() {
    devs::Simulator<Message> sim;
    const auto source=sim.add(std::make_unique<Source>(7,std::vector<Source::Entry>{{0,token(0,-1)},{0,token(1,0)},
        {.5,token(2,1)},{1,token(3,-2)}}));
    int calls=0;
    auto select=std::make_unique<Select>(7,std::vector<Select::Condition>{
        [&](const Token& item) { ++calls; return item.priority<0; },
        [](const Token& item) {
            if(item.priority<0) throw std::runtime_error("condition did not short circuit");
            return item.priority==0;
        }});
    auto* selection=select.get();
    const auto selected=sim.add(std::move(select));
    std::array<std::size_t,3> delays{};
    for(std::size_t i=0;i<3;++i) delays[i]=sim.add(std::make_unique<des::ReferenceDelay<Work>>(7,static_cast<double>(i+1)));
    const auto origin=[](const Token& item) { return item.entity.id<2 ? 0. : item.entity.id==2 ? .5 : 1.; };
    auto sink=std::make_unique<Sink>(7,origin); auto* observed=sink.get();
    const auto sink_id=sim.add(std::move(sink));
    sim.connect(source,0,selected,0);
    for(std::size_t i=0;i<3;++i) { sim.connect(selected,static_cast<std::uint32_t>(i+1),delays[i],0); sim.connect(delays[i],1,sink_id,0); }
    (void)sim.run_until(4);
    require(calls==4 && selection->received_count()==4 && selection->routed_count(1)==2 &&
            selection->routed_count(2)==1 && selection->routed_count(3)==1,"condition short-circuit/count differs");
    require(observed->completed_count()==4 && observed->total_cycle_time()==7 && observed->mean_cycle_time()==1.75,
            "typed source/selection/sink cycle accounting differs");
    require(observed->completed()[0].token.entity.id==0 && observed->completed().back().time==3.5,
            "typed source/sink absolute schedule differs");
    rejects([&] { (void)selection->routed_count(0); });
    rejects([&] { Source invalid(7,{{1,token(1)},{0,token(2)}}); });
    rejects([&] { Source invalid(7,{{0,token(1)},{1,token(1)}}); });
    rejects([&] { Source invalid(7,{{0,Token{{8,1},0}}}); });
    for(double time:std::vector<double>{-1,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { Source invalid(7,{{time,token(1)}}); });
    Source empty(7,{});
    require(empty.output().empty() && std::isinf(empty.time_advance()),"empty source is not passive");
    Token held=token(9); held.leases={{1,9,1}};
    rejects([&] { Source invalid(7,{{0,held}}); });
    rejects([&] { observed->external_transition(1,{{0,0,held}}); });
    require(observed->completed_count()==4 && observed->total_cycle_time()==7,"invalid sink token changed history");
}

void selection_and_sink_rollback() {
    des::CategoricalRouting routing{{.25,0,.5,.25},1234,12,13,456};
    Select select(7,routing);
    std::vector<devs::Input<Message>> bag;
    for(std::uint64_t id=0;id<64;++id) bag.push_back({0,0,token(id)});
    select.external_transition(0,bag);
    const auto pending=select.output();
    for(const auto& output:pending) {
        const auto id=std::get<Token>(output.value).entity.id;
        const double u=rng::uniform_open(rng::draw(1234,{12,13,id,0,456,0})[0]);
        require(output.port==(u<.25 ? 1U : u<.75 ? 3U : 4U),"addressed categorical route differs");
    }
    rejects([&] { select.confluent_transition({{0,0,token(64)},{0,0,token(0)}}); });
    require(select.received_count()==64 && select.output().size()==64,"failed selection changed prior decisions");
    Select bad(7,std::vector<Select::Condition>{[](const Token& item) {
        if(item.entity.id==1) throw std::runtime_error("injected predicate failure"); return true;
    }});
    rejects([&] { bad.external_transition(0,{{0,0,token(0)},{0,0,token(1)}}); });
    require(bad.received_count()==0 && bad.output().empty(),"failed predicate partially routed bag");
    rejects([&] { Select invalid(7,des::CategoricalRouting{{1},0,0,0,65536}); });
    Select deterministic(7,des::CategoricalRouting{{1},0,0,0,0});
    rejects([&] { deterministic.external_transition(0,{{0,0,token(std::uint64_t{1}<<48)}}); });
    Sink sink(7,[](const Token&) { return 0.; });
    rejects([&] { sink.external_transition_at(1e308,1e308,{{0,0,token(0)},{0,0,token(1)}}); });
    require(sink.completed_count()==0,"sink cycle overflow partially committed");
    sink.external_transition(.25,{{0,0,token(0)}});
    rejects([&] { sink.external_transition(1,{{0,0,token(1)},{0,0,token(0)}}); });
    require(sink.completed_count()==1 && sink.total_cycle_time()==.25,"sink duplicate rollback changed cycle history");
    Sink future(7,[](const Token&) { return 2.; });
    rejects([&] { future.external_transition(1,{{0,0,token(0)}}); });
    require(future.completed_count()==0,"future origin was accepted");
    future.external_transition(3,{{0,0,token(0)}});
    require(future.total_cycle_time()==1,"origin rejection changed sink clock");
    Sink provider(7,[](const Token& item) {
        if(item.entity.id==1) throw std::runtime_error("injected origin failure");
        return 0.;
    });
    rejects([&] { provider.external_transition(1,{{0,0,token(0)},{0,0,token(1)}}); });
    require(provider.completed_count()==0,"failed origin partially committed");
    Token leased=token(128,-2); leased.leases={{9,128,3}};
    Select passthrough(7,std::vector<Select::Condition>{});
    passthrough.external_transition(0,{{0,0,leased}});
    require(std::get<Token>(passthrough.output().front().value)==leased,"selector changed lease metadata");
    auto copy=passthrough.clone();
    passthrough.internal_transition();
    require(copy->output().size()==1 && passthrough.output().empty(),"selector clone shares buffered decisions");
}

void priority_providers() {
    using Queue=des::ReferenceQueue<Work>;
    int calls=0;
    Queue queue(7,1,des::QueueDiscipline::priority,[&](const Token& item) {
        ++calls;
        if(item.entity.id==4) throw std::runtime_error("priority failure");
        return -static_cast<std::int32_t>(item.entity.id);
    });
    rejects([&] { queue.external_transition(0,{{0,0,token(3)},{0,0,token(4)}}); });
    require(queue.accepted_count()==0 && queue.rejected_count()==0,"priority failure partly admitted queue");
    queue.external_transition(0,{{0,2,des::QueuePull{1}},{0,0,token(1)},{0,0,token(2)},{0,0,token(3)}});
    const auto output=queue.output();
    require(output.size()==2 && output[0].port==3 && std::get<Token>(output[0].value).entity.id==1 &&
        output[1].port==1 && std::get<Token>(output[1].value).entity.id==3 && std::get<Token>(output[1].value).priority==-3,
        "priority provider did not precede admission/dispatch");
    (void)queue.output(); (void)queue.clone();
    require(calls==5,"queue priority provider was reevaluated");
    rejects([&] { Queue invalid(7,1,des::QueueDiscipline::fifo,[](const Token&) { return 0; }); });
    using Seize=des::ReferenceSeize<Work>;
    using RMessage=des::ResourceProcessMessage<Work>;
    int units_calls=0,priority_calls=0;
    Seize seize(7,9,2,Seize::Units{[&](const Token& item) {
        ++units_calls; require(item.priority==10,"priority changed before units evaluation"); return 1;
    }},false,[&](const Token& item) {
        ++priority_calls; require(item.priority==10,"priority provider did not read original token");
        if(item.entity.id==2) throw std::runtime_error("seize priority failure"); return -1;
    });
    rejects([&] { seize.external_transition(0,{{0,0,RMessage{token(1,10)}},{0,0,RMessage{token(2,10)}}}); });
    require(seize.received_count()==0 && seize.waiting()==0,"priority failure partly seized");
    seize.external_transition(0,{{0,0,RMessage{token(1,10)}}});
    const auto request=std::get<des::Seize>(seize.output().front().value);
    require(request.priority==-1,"seize request priority differs");
    seize.internal_transition();
    seize.external_transition(0,{{0,2,des::Grant{request.request_id,1}}});
    require(std::get<Token>(seize.output().front().value).priority==-1,"grant lost priority snapshot");
    require(units_calls==3 && priority_calls==3,"grant reevaluated providers");
}
}

int main() {
    try { categorical_boundaries(); source_select_sink(); selection_and_sink_rollback(); priority_providers();
        std::cout<<"Typed source/sink/selection schedules, categorical integer boundaries and rollback passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
