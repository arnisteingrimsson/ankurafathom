#include "ankurafathom/des/entity_store.hpp"
#include "ankurafathom/devs/simulator.hpp"
#include <iostream>
#include <limits>
#include <memory>
#include <variant>

namespace {
using namespace ankurafathom;
struct Work {};
struct Other {};
using Store=des::EntityStore<Work,double,std::int64_t,bool,std::string>;
using Ref=des::EntityRef<Work>;
static_assert(!std::is_convertible_v<Ref,des::EntityRef<Other>>);
static_assert(std::is_same_v<Store::Record,std::tuple<double,std::int64_t,bool,std::string>>);
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class Function> void rejects(Function function) {
    bool caught=false;
    try { function(); } catch(const std::exception&) { caught=true; }
    require(caught,"invalid entity operation succeeded");
}

void identity_columns_and_retirement() {
    Store store(17,100);
    auto refs=store.spawn_many({{1.5,4,true,"draft"},{2.5,-7,false,"review"}});
    require(refs==std::vector<Ref>{{17,100},{17,101}},"entity identities differ");
    require(store.size()==2 && store.active_count()==2 && store.next_id()==102,"store size/count differs");
    require(store.column<0>()==std::vector<double>{1.5,2.5} && store.column<1>()==std::vector<std::int64_t>{4,-7} &&
            store.column<2>()==std::vector<bool>{true,false} && store.column<3>()==std::vector<std::string>{"draft","review"},
            "typed columns differ");
    require(store.field<2>(refs[0]) && !store.field<2>(refs[1]),"boolean column access differs");
    store.retire(refs[0]);
    require(!store.alive(refs[0]) && store.alive(refs[1]) && store.active_count()==1,"retirement count differs");
    require(store.column<0>()[0]==1.5 && store.live_rows()==std::vector<bool>{false,true},"retirement removed audit row");
    rejects([&] { (void)store.record(refs[0]); });
    rejects([&] { store.retire(refs[0]); });
    rejects([&] { (void)store.field<0>(Ref{18,101}); });
    rejects([&] { (void)store.alive(Ref{18,101}); });
    require(!store.alive({17,99}) && !store.alive({17,999}),"unknown identity is live");
    const auto next=store.spawn({3,0,true,"new"});
    require(next==Ref{17,102} && store.size()==3 && store.active_count()==2,"retired ID reused");
    Store copy=store;
    copy.update(refs[1],{9,8,true,"changed"});
    require(store.field<0>(refs[1])==2.5 && copy.field<0>(refs[1])==9,"store copy shares columns");
}

void batches_and_boundaries() {
    Store store(1);
    const auto a=store.spawn({1,1,false,"a"});
    const auto b=store.spawn({2,2,true,"b"});
    const auto before=store;
    for(double bad:{std::numeric_limits<double>::infinity(),-std::numeric_limits<double>::infinity(),
                    std::numeric_limits<double>::quiet_NaN()}) {
        rejects([&] { (void)store.spawn_many({{3,3,true,"c"},{bad,4,false,"bad"}}); });
        rejects([&] { store.update_many({{a,{8,8,true,"changed"}},{b,{bad,4,false,"bad"}}}); });
        require(store.next_id()==before.next_id() && store.live_rows()==before.live_rows() &&
                store.record(a)==before.record(a) && store.record(b)==before.record(b),"invalid batch partially committed");
    }
    rejects([&] { store.update_many({{a,{8,8,true,"x"}},{a,{9,9,false,"y"}}}); });
    rejects([&] { store.update_many({{a,{8,8,true,"x"}},{{2,b.id},{9,9,false,"y"}}}); });
    require(store.record(a)==before.record(a),"duplicate/foreign update changed prior row");
    store.update_many({{b,{8,7,false,"B"}},{a,{9,6,true,"A"}}});
    require(store.column<0>()==std::vector<double>{9,8} && store.next_id()==2,"updates changed row identity/order");
    require(store.spawn_many({}).empty(),"empty spawn produced references");
    store.update_many({});
    constexpr std::uint64_t maximum=(std::uint64_t{1}<<48)-1;
    Store edge(9,maximum);
    rejects([&] { (void)edge.spawn_many({{0,0,false,"a"},{0,0,false,"b"}}); });
    require(edge.size()==0 && edge.next_id()==maximum,"exhausted batch consumed final ID");
    auto last=edge.spawn({0,std::numeric_limits<std::int64_t>::min(),false,""});
    require(last.id==maximum && edge.next_id()==maximum+1,"maximum ID not allocated");
    edge.retire(last);
    rejects([&] { (void)edge.spawn({0,0,false,"again"}); });
    rejects([&] { Store invalid(9,maximum+1); });
    // Growth retains alignment and immutable copies despite column reallocation.
    Store growth(3);
    for(int i=0;i<128;++i) {
        auto ref=growth.spawn({i*.5,i,i%2==0,std::to_string(i)});
        require(ref.id==static_cast<std::uint64_t>(i),"growth ID skipped");
    }
    for(int i=0;i<128;++i) require(growth.record({3,static_cast<std::uint64_t>(i)})==Store::Record{i*.5,i,i%2==0,std::to_string(i)},
                                  "column growth lost alignment");
}

using Message=std::variant<Store::Record,Ref>;
class Owner final:public devs::Atomic<Message> {
public:
    Store entities{7};
    std::vector<Ref> pending;
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<Owner>(*this); }
    double time_advance()const override { return pending.empty() ? std::numeric_limits<double>::infinity() : 0; }
    std::vector<devs::PortValue<Message>> output()const override {
        std::vector<devs::PortValue<Message>> result;
        for(auto ref:pending) result.push_back({1,ref});
        return result;
    }
    void internal_transition()override { pending.clear(); }
    void external_transition(double,const std::vector<devs::Input<Message>>& bag)override {
        for(const auto& input:bag) pending.push_back(entities.spawn(std::get<Store::Record>(input.value)));
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override { internal_transition(); external_transition(0,bag); }
};
class Receiver final:public devs::Atomic<Message> {
public:
    explicit Receiver(std::shared_ptr<bool> fail):fail_(std::move(fail)) {}
    std::vector<Ref> received;
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<Receiver>(*this); }
    double time_advance()const override { return std::numeric_limits<double>::infinity(); }
    std::vector<devs::PortValue<Message>> output()const override { return {}; }
    void internal_transition()override { throw std::logic_error("passive"); }
    void external_transition(double,const std::vector<devs::Input<Message>>& bag)override {
        for(const auto& input:bag) received.push_back(std::get<Ref>(input.value));
        if(*fail_) throw std::runtime_error("injected receiver failure");
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override { external_transition(0,bag); }
private:std::shared_ptr<bool> fail_;
};

void owned_store_checked_rollback() {
    devs::Simulator<Message> sim;
    const auto owner=sim.add(std::make_unique<Owner>());
    auto fail=std::make_shared<bool>(true);
    const auto receiver=sim.add(std::make_unique<Receiver>(fail));
    sim.connect(owner,1,receiver,0);
    sim.inject(0,owner,0,Store::Record{1,1,true,"first"});
    (void)sim.step_transactional();
    // Owner spawns during confluence while downstream throws on its prior output.
    sim.inject(0,owner,0,Store::Record{2,2,false,"second"});
    rejects([&] { (void)sim.step_transactional(); });
    const auto& restored=dynamic_cast<const Owner&>(sim.model(owner));
    require(restored.entities.size()==1 && restored.entities.next_id()==1 && restored.pending==std::vector<Ref>{{7,0}},
            "checked rollback lost store/identity state");
    require(dynamic_cast<const Receiver&>(sim.model(receiver)).received.empty(),"receiver state survived failed step");
    *fail=false;
    (void)sim.step_transactional();
    (void)sim.step_transactional();
    require(dynamic_cast<const Receiver&>(sim.model(receiver)).received==std::vector<Ref>{{7,0},{7,1}},
            "retry changed entity reference sequence");
    const auto& retried=dynamic_cast<const Owner&>(sim.model(owner));
    require(retried.entities.size()==2 && retried.entities.field<3>({7,1})=="second","retry lost new entity fields");
}
}

int main() {
    try { identity_columns_and_retirement(); batches_and_boundaries(); owned_store_checked_rollback();
        std::cout<<"Typed entity columns, identities, batch rollback, boundaries and checked DEVS retry passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
