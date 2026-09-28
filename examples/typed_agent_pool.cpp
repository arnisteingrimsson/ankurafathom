#include "ankurafathom/hybrid/typed_agent_pool_atomic.hpp"
#include "ankurafathom/des/reference_resource.hpp"
#include "ankurafathom/des/reference_delay.hpp"
#include <iostream>

struct Consultant {};struct Engagement {};
namespace af=ankurafathom;
using Workforce=af::hybrid::TypedAgentPool<Consultant>;
using Control=af::hybrid::TypedPoolControl<Consultant>;
using Token=af::des::EntityToken<Engagement>;
using Message=std::variant<Control,Workforce::Result,Workforce::Notification,af::abm::PopulationResult<Consultant>,
    af::des::Seize,af::des::Release,af::des::Grant,Token,af::des::QueuePull>;
using Pool=af::hybrid::TypedAgentPoolAtomic<Consultant,Message>;
int main() {
    using K=af::des::FieldKind;
    Workforce::Store staff(1,{{"capacity",K::integer},{"allocated",K::integer},{"completed",K::integer}});
    staff.spawn({std::int64_t{1},std::int64_t{0},std::int64_t{0}});
    Workforce workforce(staff,1,[](auto ref,const auto& store) {
        return static_cast<std::size_t>(std::get<std::int64_t>(store.field(ref,0)));
    },1,af::des::QueueDiscipline::fifo,[](const auto& notification,const auto& store) {
        auto record=store.record(notification.agent);
        if(!notification.assigned) record[2]=std::get<std::int64_t>(record[2])+static_cast<std::int64_t>(notification.units);
        return record;
    });
    af::devs::Simulator<Message> sim;
    const auto pool=sim.add(std::make_unique<Pool>(std::move(workforce)));
    const auto seize=sim.add(std::make_unique<af::des::ReferenceSeize<Engagement,Message>>(2,1,1,1));
    const auto delivery=sim.add(std::make_unique<af::des::ReferenceDelay<Engagement,Message>>(2,.5));
    const auto release=sim.add(std::make_unique<af::des::ReferenceRelease<Engagement,Message>>(2,1));
    sim.connect(seize,3,pool,0);sim.connect(pool,2,seize,2);sim.connect(seize,1,delivery,0);
    sim.connect(delivery,1,release,0);sim.connect(release,3,pool,0);
    for(std::uint64_t id=0;id<5;++id) sim.inject(0,seize,0,Message{Token{{2,id}}});
    Workforce::Change hiring;hiring.time=.75;hiring.births={{std::int64_t{1},std::int64_t{0},std::int64_t{0}}};
    sim.inject(.75,pool,2,Message{Control{0,hiring}});
    std::cout<<"time,headcount,allocated,waiting,completed,allocated_hours\n";
    for(const double time:{0.,.5,.75,1.,1.25,1.5,2.}) {
        sim.run_until_transactional(time);
        const auto& owner=dynamic_cast<const Pool&>(sim.model(pool)).core();
        std::int64_t completed=0;for(const auto n:std::get<std::vector<std::int64_t>>(owner.store().column(2))) completed+=n;
        std::cout<<time<<','<<owner.store().active_count()<<','<<owner.pool().allocated_units()<<','<<owner.pool().waiting()<<','
                 <<completed<<','<<owner.statistics(time).allocated_time<<'\n';
    }
}
