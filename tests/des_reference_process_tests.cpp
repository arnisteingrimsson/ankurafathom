#include "ankurafathom/des/reference_queue.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <iostream>
#include <memory>

namespace {
using namespace ankurafathom;
struct Work {};
using Token=des::EntityToken<Work>;
using Message=des::ProcessMessage<Work>;
using Queue=des::ReferenceQueue<Work>;
using Delay=des::ReferenceDelay<Work>;
using Input=devs::Input<Message>;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
void close(double a,double b,const char* message) { require(std::abs(a-b)<1e-12,message); }
template<class Function> void rejects(Function function) {
    bool caught=false; try { function(); } catch(const std::exception&) { caught=true; }
    require(caught,"invalid reference-process operation succeeded");
}
Token token(std::uint64_t id,std::int32_t priority=0) { return {{7,id},priority}; }
Input put(std::uint64_t id,std::int32_t priority=0) { return {0,0,token(id,priority)}; }
Input pull(std::size_t count) { return {0,2,des::QueuePull{count}}; }
std::vector<std::uint64_t> output_ids(const devs::Atomic<Message>& model,std::uint32_t port) {
    std::vector<std::uint64_t> result;
    for(const auto& output:model.output()) if(output.port==port) result.push_back(std::get<Token>(output.value).entity.id);
    return result;
}

void queue_contract() {
    Queue queue(7,2,des::QueueDiscipline::lifo);
    queue.external_transition(0,{put(0),put(1),put(2),pull(1)});
    require(output_ids(queue,1)==std::vector<std::uint64_t>{2} && queue.waiting()==2 && queue.rejected_count()==0,
            "same-bag demand failed to reserve queue admission");
    rejects([&] { queue.external_transition(1,{pull(1)}); });
    queue.internal_transition();
    queue.external_transition(1,{put(3)});
    require(output_ids(queue,3)==std::vector<std::uint64_t>{3} && queue.waiting()==2,"queue overflow selection differs");
    queue.internal_transition();
    rejects([&] { queue.external_transition(1,{pull(2),put(4),put(3)}); });
    require(queue.waiting()==2 && queue.demand()==0 && queue.released_count()==1,"invalid queue bag changed demand");
    close(queue.mean_queue_length(2),2,"failed queue bag changed area");
    queue.external_transition(1,{pull(2)});
    require(output_ids(queue,1)==std::vector<std::uint64_t>{1,0} && queue.waiting()==0,"LIFO pull order differs");
    close(queue.total_waiting_time(),4,"queue wait total differs");
    auto clone=queue.clone();
    queue.confluent_transition({pull(1),put(4)});
    require(output_ids(queue,1)==std::vector<std::uint64_t>{4} && output_ids(*clone,1)==std::vector<std::uint64_t>{1,0},
            "queue confluence or clone state differs");
    Queue zero(7,0);
    zero.external_transition(0,{put(0)});
    require(zero.accepted_count()==0 && zero.rejected_count()==1,"zero queue admitted without demand");
    zero.internal_transition();
    zero.external_transition(1,{pull(2),put(1)});
    require(zero.demand()==1 && zero.accepted_count()==1 && zero.waiting()==0,"zero queue lost residual demand");
    zero.internal_transition();
    rejects([&] { zero.external_transition(0,{pull(std::numeric_limits<std::size_t>::max())}); });
    require(zero.demand()==1,"demand overflow partially committed");
    rejects([&] { zero.external_transition(0,{pull(0)}); });
    rejects([&] { zero.external_transition(0,{{0,0,Token{{8,2},0}}}); });
    rejects([&] { zero.external_transition(0,{put(std::uint64_t{1}<<48)}); });
    Queue priority(7,std::nullopt,des::QueueDiscipline::priority);
    priority.external_transition(0,{put(4,0),put(2,0),put(3,-1),pull(2)});
    require(output_ids(priority,1)==std::vector<std::uint64_t>{3,2},"reference priority arbitration differs");
}

void queue_delay_composition() {
    for(auto discipline:{des::QueueDiscipline::fifo,des::QueueDiscipline::lifo,des::QueueDiscipline::priority}) {
        devs::Simulator<Message> sim;
        auto queue=std::make_unique<Queue>(7,1,discipline);
        auto* observed_queue=queue.get();
        const auto qid=sim.add(std::move(queue));
        auto delay=std::make_unique<Delay>(7,[](const Token& item) { return item.entity.id==0 ? 2.0 : item.entity.id==1 ? 1.0 : 3.0; },2);
        auto* observed_delay=delay.get();
        const auto did=sim.add(std::move(delay));
        sim.connect(qid,1,did,0); sim.connect(did,3,qid,2);
        for(std::uint64_t id=0;id<5;++id) sim.inject(0,qid,0,token(id));
        std::vector<std::pair<double,std::uint64_t>> completed;
        std::vector<std::uint64_t> lost;
        for(const auto& step:sim.run_until(4)) for(const auto& output:step.emissions) {
            if(output.source==did && output.port==1) completed.push_back({step.time,std::get<Token>(output.value).entity.id});
            if(output.source==qid && output.port==3) lost.push_back(std::get<Token>(output.value).entity.id);
        }
        const auto expected=discipline==des::QueueDiscipline::lifo ?
            std::vector<std::pair<double,std::uint64_t>>{{1,1},{3,2},{3,0}} :
            std::vector<std::pair<double,std::uint64_t>>{{1,1},{2,0},{4,2}};
        require(completed==expected && lost==std::vector<std::uint64_t>{3,4},"queue/delay hand trace differs");
        require(observed_queue->accepted_count()==3 && observed_queue->released_count()==3 && observed_queue->waiting()==0 &&
                observed_queue->demand()==2 && observed_delay->active_count()==0 && observed_delay->completed_count()==3 &&
                observed_delay->rejected_count()==0,"credit or entity conservation failed");
        close(observed_queue->mean_queue_length(4),.25,"composed queue area differs");
        close(observed_delay->mean_in_process(4),1.5,"composed delay area differs");
        close(observed_delay->utilization(4),.75,"composed delay utilization differs");
        close(observed_delay->completed_residence_time(),6,"delay residence sum differs");
    }
}

void delay_contract() {
    for(double invalid:{0.,-1.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { Delay bad(7,invalid); });
    rejects([&] { Delay bad(7,1.,0); });
    Delay parallel(7,[](const Token& item) { return 3.-item.entity.id; });
    parallel.external_transition(0,{put(0),put(1),put(2)});
    require(parallel.active_count()==3 && output_ids(parallel,1)==std::vector<std::uint64_t>{2},"parallel delay order differs");
    rejects([&] { (void)parallel.utilization(3); });
    parallel.internal_transition(); parallel.internal_transition(); parallel.internal_transition();
    require(parallel.completed_count()==3 && parallel.active_count()==0,"parallel delay failed to drain");
    close(parallel.mean_in_process(3),2,"parallel delay busy area differs");
    Delay fractional(7,.2);
    fractional.external_transition_at(.1,.1,{put(0)});
    const auto deadline=fractional.next_event_time();
    fractional.external_transition_at(.15,.15-.1,{put(1)});
    require(fractional.next_event_time()==deadline,"intervening arrival changed absolute delay deadline");
    auto snapshot=fractional.clone();
    rejects([&] { fractional.confluent_transition({put(2),put(1)}); });
    require(fractional.completed_count()==0 && fractional.active_count()==2 &&
            output_ids(fractional,1)==output_ids(*snapshot,1) && fractional.next_event_time()==snapshot->next_event_time(),
            "delay confluence failure changed calendar");
    Delay bad_duration(7,[](const Token& item) { return item.entity.id==1 ? -1. : 1.; });
    rejects([&] { bad_duration.external_transition(0,{put(0),put(1)}); });
    require(bad_duration.accepted_count()==0,"invalid duration partially admitted bag");
    Delay collapsed(7,1.);
    rejects([&] { collapsed.external_transition_at(0x1p53,0x1p53,{put(0)}); });
    require(collapsed.accepted_count()==0,"unrepresentable deadline consumed identity");
    Delay bounded(7,1.,1);
    bounded.internal_transition(); // Publish initial credit.
    bounded.external_transition(0,{put(0),put(1)});
    require(output_ids(bounded,2)==std::vector<std::uint64_t>{1} && bounded.active_count()==1,"delay overflow not published");
    bounded.internal_transition(); // Rejection publication preserves service deadline.
    require(bounded.next_event_time()==1.,"rejection publication shifted delay completion");
    bounded.confluent_transition({put(2)});
    require(bounded.completed_count()==1 && bounded.active_count()==1 && bounded.rejected_count()==1,
            "confluent delay arrival did not see freed capacity");
    rejects([&] { bounded.external_transition(0,{{0,0,Token{{8,3},0}}}); });
    rejects([&] { bounded.external_transition(0,{pull(1)}); });
    int evaluations=0;
    Delay cached(7,[&](const Token&) { ++evaluations; return 1.; });
    cached.external_transition(0,{put(0),put(1)});
    (void)cached.output(); (void)cached.output(); (void)cached.clone()->output();
    require(evaluations==2,"delay provider evaluated during output or clone");
    Queue huge_queue(7);
    huge_queue.external_transition(0,{put(0),put(1)});
    rejects([&] { huge_queue.external_transition_at(1e308,1e308,{}); });
    require(huge_queue.waiting()==2 && huge_queue.mean_queue_length(1)==2,"queue statistics failure advanced state");
    Delay huge_delay(7,1e308);
    huge_delay.external_transition(0,{put(0),put(1)});
    rejects([&] { huge_delay.internal_transition(); });
    require(huge_delay.active_count()==2 && huge_delay.completed_count()==0 && huge_delay.next_event_time()==1e308,
            "delay statistics failure advanced calendar");
}

class Receiver final:public devs::Atomic<Message> {
public:
    explicit Receiver(std::shared_ptr<bool> fail):fail_(std::move(fail)) {}
    std::vector<std::uint64_t> received;
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<Receiver>(*this); }
    double time_advance()const override { return std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override { throw std::logic_error("passive receiver"); }
    void external_transition(double,const std::vector<devs::Input<Message>>& bag)override {
        for(const auto& input:bag) received.push_back(std::get<Token>(input.value).entity.id);
        if(*fail_) throw std::runtime_error("injected failure after receipt");
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override { external_transition(0,bag); }
private: std::shared_ptr<bool> fail_;
};

void checked_graph_rollback() {
    devs::Simulator<Message> sim;
    const auto qid=sim.add(std::make_unique<Queue>(7));
    const auto did=sim.add(std::make_unique<Delay>(7,1.,1));
    auto fail=std::make_shared<bool>(true);
    const auto sink=sim.add(std::make_unique<Receiver>(fail));
    sim.connect(qid,1,did,0); sim.connect(did,3,qid,2); sim.connect(did,1,sink,0);
    for(std::uint64_t id=0;id<3;++id) sim.inject(0,qid,0,token(id));
    (void)sim.step_transactional(); (void)sim.step_transactional();
    require(sim.next_time()==1,"unexpected first delayed completion");
    rejects([&] { (void)sim.step_transactional(); });
    require(dynamic_cast<const Queue&>(sim.model(qid)).waiting()==2 &&
            dynamic_cast<const Queue&>(sim.model(qid)).released_count()==1 &&
            dynamic_cast<const Delay&>(sim.model(did)).active_count()==1 &&
            dynamic_cast<const Delay&>(sim.model(did)).completed_count()==0 &&
            dynamic_cast<const Receiver&>(sim.model(sink)).received.empty(),"checked failure duplicated/lost tokens or credits");
    *fail=false;
    while(std::isfinite(sim.next_time())) (void)sim.step_transactional();
    require(dynamic_cast<const Receiver&>(sim.model(sink)).received==std::vector<std::uint64_t>{0,1,2},
            "retry changed reference completion sequence");
    const auto& queue=dynamic_cast<const Queue&>(sim.model(qid));
    const auto& delay=dynamic_cast<const Delay&>(sim.model(did));
    require(queue.demand()==1 && queue.waiting()==0 && delay.active_count()==0,"retry lost free capacity credit");
    close(queue.total_waiting_time(),3,"retry double-counted waiting time");
    close(delay.completed_residence_time(),3,"retry double-counted residence time");
}
}

int main() {
    try { queue_contract(); queue_delay_composition(); delay_contract(); checked_graph_rollback();
        std::cout<<"Reference queue/delay ordering, credits, conservation, clocks and rollback passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
