#include "ankurafathom/des/reference_resource.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <iostream>

namespace {
using namespace ankurafathom;
struct Work {};
using Token=des::EntityToken<Work>;
using Message=des::ResourceProcessMessage<Work>;
using Seize=des::ReferenceSeize<Work>;
using Release=des::ReferenceRelease<Work>;
using Pool=des::TypedResourcePool<Message>;
using Delay=des::ReferenceDelay<Work,Message>;
using Input=devs::Input<Message>;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class Function> void rejects(Function function) {
    bool caught=false; try { function(); } catch(const std::exception&) { caught=true; }
    require(caught,"invalid reference resource operation succeeded");
}
Token token(std::uint64_t id,std::int32_t priority=0) { return {{7,id},priority}; }
std::uint64_t request(std::uint16_t block,std::uint64_t id) { return (static_cast<std::uint64_t>(block)<<48)|id; }

void hand_protocol() {
    Seize seize(7,11,5,2);
    seize.external_transition(0,{{0,0,token(3,-2)}});
    const auto pending=seize.output();
    require(pending.size()==1 && pending[0].port==3,"seize request publication differs");
    const auto value=std::get<des::Seize>(pending[0].value);
    require(value.request_id==request(5,3) && value.units==2 && value.priority==-2 && !value.preempt,"seize request metadata differs");
    seize.internal_transition();
    rejects([&] { seize.external_transition(1,{{0,2,des::Grant{request(5,3),1}}}); });
    require(seize.waiting()==1 && seize.granted_count()==0,"mismatched grant changed waiting state");
    seize.external_transition(1,{{0,2,des::Grant{request(5,3),2}}});
    const auto granted=std::get<Token>(seize.output()[0].value);
    require(granted.entity==token(3).entity && granted.priority==-2 &&
            granted.leases==std::vector<des::ResourceLease>{{11,request(5,3),2}},"grant lease or token metadata differs");
    Release release(7,11);
    release.external_transition(1,{{0,0,granted}});
    bool saw_request=false,saw_token=false;
    for(const auto& output:release.output()) {
        if(output.port==3) saw_request=std::get<des::Release>(output.value).request_id==request(5,3);
        else if(output.port==1) saw_token=std::get<Token>(output.value).leases.empty();
    }
    require(saw_request && saw_token,"release did not publish request and unleased token");
    rejects([&] { release.confluent_transition({{0,0,granted}}); });
    require(release.released_count()==1 && release.output().size()==2,"invalid release confluence erased publication");
    rejects([&] { Seize invalid(7,11,5,1,true); });
    rejects([&] { Seize invalid(7,11,65536,1); });
    rejects([&] { Seize invalid(7,11,5,0); });
    Seize other(7,11,6,1);
    rejects([&] { other.external_transition(0,{{0,0,granted}}); });
    rejects([&] { other.external_transition(0,{{0,0,token(1)},{0,2,des::Grant{request(6,1),1}}}); });
    require(other.waiting()==0 && other.received_count()==0,"same-bag invented grant partially admitted");
    Token multiple=token(9);
    multiple.leases={{12,request(8,9),1},{11,request(5,9),2}};
    Release multiple_release(7,11);
    multiple_release.external_transition(0,{{0,0,multiple}});
    for(const auto& output:multiple_release.output()) if(output.port==1)
        require(std::get<Token>(output.value).leases==std::vector<des::ResourceLease>{{12,request(8,9),1}},"release removed unrelated lease");
    Token malformed=token(10); malformed.leases={{11,request(5,9),2}};
    rejects([&] { multiple_release.confluent_transition({{0,0,malformed}}); });
}

void shared_pool_trace() {
    devs::Simulator<Message> sim;
    const auto aid=sim.add(std::make_unique<Seize>(7,11,1,1));
    const auto bid=sim.add(std::make_unique<Seize>(7,11,2,1));
    auto pool=std::make_unique<Pool>(2,2,des::QueueDiscipline::priority);
    auto* observed=pool.get();
    const auto pid=sim.add(std::move(pool));
    const auto did=sim.add(std::make_unique<Delay>(7,[](const Token& item) { return item.entity.id==0 ? 2. : 1.; }));
    const auto rid=sim.add(std::make_unique<Release>(7,11));
    sim.connect(aid,3,pid,0); sim.connect(bid,3,pid,0);
    sim.connect(pid,2,aid,2); sim.connect(pid,3,bid,2); // Grant port = block ID + 1.
    sim.connect(aid,1,did,0); sim.connect(bid,1,did,0);
    sim.connect(did,1,rid,0); sim.connect(rid,3,pid,0);
    for(std::uint64_t id=0;id<4;++id) sim.inject(0,id%2 ? bid : aid,0,token(id,static_cast<std::int32_t>(3-id)));
    std::vector<std::pair<double,std::uint64_t>> completed;
    for(const auto& step:sim.run_until(3)) for(const auto& output:step.emissions) if(output.source==rid && output.port==1) {
        const auto& item=std::get<Token>(output.value);
        require(item.leases.empty(),"completion retains a released resource");
        completed.push_back({step.time,item.entity.id});
    }
    // Equal-time starts merge in seize component-ID order; both urgent jobs
    // receive the initial allocations and complete before the lower priorities.
    require(completed==std::vector<std::pair<double,std::uint64_t>>{{1,2},{1,3},{2,1},{3,0}},"seize/delay/release priority trace differs");
    require(observed->pool().available()==2 && observed->pool().allocated_units()==0 && observed->pool().waiting()==0,
            "seize/delay/release failed resource conservation");
    require(dynamic_cast<const Seize&>(sim.model(aid)).granted_count()==2 &&
            dynamic_cast<const Seize&>(sim.model(bid)).granted_count()==2 &&
            dynamic_cast<const Release&>(sim.model(rid)).released_count()==4,"resource process counters differ");
    require(std::abs(observed->utilization(3)-5./6)<1e-12 && std::abs(observed->mean_waiting(3)-2./3)<1e-12,
            "resource time-weighted accounting differs");
}

void pool_resize_statistics_and_rollback() {
    Pool pool(2,3);
    pool.external_transition(0,{{0,0,des::Seize{1,1}}}); pool.internal_transition();
    pool.external_transition_at(1,1,{{0,2,des::SetCapacity{4}},{0,0,des::Seize{2,3}}}); pool.internal_transition();
    pool.external_transition_at(1.5,.5,{{0,0,des::Seize{3,3}}});
    pool.external_transition_at(2,.5,{{0,0,des::Release{1}},{0,2,des::SetCapacity{3}}});
    pool.external_transition_at(3,1,{{0,0,des::Release{2}},{0,2,des::SetCapacity{0}}});
    pool.external_transition_at(4,1,{{0,2,des::SetCapacity{3}}}); pool.internal_transition();
    require(std::abs(pool.utilization(4)-8./9)<1e-12 && pool.mean_waiting(4)==2.5/4 && pool.mean_capacity(4)==9./4,
            "resized resource areas differ");
    rejects([&] { pool.external_transition_at(5,1,{{0,2,des::SetCapacity{0}}}); });
    require(pool.pool().allocated_units()==3 && pool.mean_allocated(5)==11./5 && pool.mean_capacity(5)==12./5 &&
            pool.mean_waiting(5)==.5,"invalid resize changed projected statistics");
    rejects([&] { pool.external_transition_at(5,1,{{0,0,token(0)}}); });
    rejects([&] { (void)pool.utilization(0); });
    Pool empty(0,1);
    require(empty.utilization(1)==0 && empty.mean_capacity(1)==0,"zero capacity produced invalid utilization");
    Pool overflow(2,1);
    overflow.external_transition(0,{{0,0,des::Seize{1,1}}}); overflow.internal_transition();
    rejects([&] { overflow.external_transition_at(1e308,1e308,{{0,0,des::Release{1}}}); });
    require(overflow.pool().allocated_units()==1 && overflow.utilization(1)==.5,"resource area overflow partially released allocation");
    Seize units(7,11,5,[](const Token& item) { return item.entity.id==1 ? 0U : 1U; });
    rejects([&] { units.external_transition(0,{{0,0,token(0)},{0,0,token(1)}}); });
    require(units.received_count()==0 && units.waiting()==0 && units.output().empty(),"invalid units partially admitted bag");
}

class Receiver final:public devs::Atomic<Message> {
public:
    explicit Receiver(std::shared_ptr<bool> fail):fail_(std::move(fail)) {}
    std::vector<std::uint64_t> completed;
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<Receiver>(*this); }
    double time_advance()const override { return std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override { throw std::logic_error("passive"); }
    void external_transition(double,const std::vector<devs::Input<Message>>& bag)override {
        for(const auto& input:bag) {
            const auto& item=std::get<Token>(input.value);
            require(item.leases.empty(),"sink received held resources");
            completed.push_back(item.entity.id);
        }
        if(*fail_) throw std::runtime_error("injected downstream failure");
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override { external_transition(0,bag); }
private:std::shared_ptr<bool> fail_;
};

void checked_release_rollback() {
    devs::Simulator<Message> sim;
    const auto seize=sim.add(std::make_unique<Seize>(7,11,1,1));
    const auto pool=sim.add(std::make_unique<Pool>(1,1));
    const auto delay=sim.add(std::make_unique<Delay>(7,1.));
    const auto release=sim.add(std::make_unique<Release>(7,11));
    auto fail=std::make_shared<bool>(true);
    const auto sink=sim.add(std::make_unique<Receiver>(fail));
    sim.connect(seize,3,pool,0); sim.connect(pool,2,seize,2); sim.connect(seize,1,delay,0);
    sim.connect(delay,1,release,0); sim.connect(release,3,pool,0); sim.connect(release,1,sink,0);
    sim.inject(0,seize,0,token(0)); sim.inject(0,seize,0,token(1));
    while(sim.next_time()==0) (void)sim.step_transactional();
    require(sim.next_time()==1,"shared resource did not start service");
    (void)sim.step_transactional(); // Completion enters release block.
    rejects([&] { (void)sim.step_transactional(); });
    const auto& restored=dynamic_cast<const Pool&>(sim.model(pool));
    require(restored.pool().allocated_units()==1 && restored.pool().waiting()==1 &&
            dynamic_cast<const Release&>(sim.model(release)).output().size()==2 &&
            dynamic_cast<const Receiver&>(sim.model(sink)).completed.empty(),"failed release publication lost ownership");
    *fail=false;
    while(std::isfinite(sim.next_time())) (void)sim.step_transactional();
    const auto& final=dynamic_cast<const Pool&>(sim.model(pool));
    require(dynamic_cast<const Receiver&>(sim.model(sink)).completed==std::vector<std::uint64_t>{0,1} &&
            final.pool().available()==1 && final.pool().allocated_units()==0 && final.utilization(2)==1,
            "release retry changed completion, ownership or resource area");
}
}

int main() {
    try { hand_protocol(); shared_pool_trace(); pool_resize_statistics_and_rollback(); checked_release_rollback();
        std::cout<<"Reference seize/release protocols, leases, validation and shared pool traces passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
