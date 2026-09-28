#pragma once

#include "ankurafathom/des/process.hpp"
#include "ankurafathom/hybrid/clocked_sd.hpp"
#include "ankurafathom/hybrid/entity_to_pulse.hpp"
#include <array>
#include <numeric>

namespace ankurafathom::hybrid::models {

// Native source -> FIFO server, with arrival/completion pulses into one SD stock.
// See docs/SEMANTICS.md and docs/HYBRID_DES_FLUID_LIMIT.md.
class QueueBacklog {
public:
    using Message=std::variant<des::Entity,double>;
    struct Observation {
        double time,stock;
        std::size_t arrivals,completed,waiting;
        bool busy;
        bool operator==(const Observation&)const=default;
    };
    // order maps declaration positions to source/server/arrival-pulse/completion-pulse/SD.
    explicit QueueBacklog(std::vector<des::Entity> schedule,double sd_dt=.25,
                          std::array<std::size_t,5> order={0,1,2,3,4}) {
        auto sorted=order;std::sort(sorted.begin(),sorted.end());
        if(sorted!=std::array<std::size_t,5>{0,1,2,3,4}) throw std::invalid_argument("invalid backlog declaration order");
        if(schedule.size()>1000000) throw std::invalid_argument("backlog schedule exceeds allocation limit");
        std::array<std::unique_ptr<devs::Atomic<Message>>,5> components;
        components[0]=std::make_unique<des::ScheduledSource<Message>>(std::move(schedule));
        components[1]=std::make_unique<des::SingleServer<Message>>();
        components[2]=std::make_unique<EntityToPulse<Message>>([](const des::Entity&) { return 1.; });
        components[3]=std::make_unique<EntityToPulse<Message>>([](const des::Entity&) { return -1.; });
        sd::Model stock;stock.add_stock("backlog_jobs",0);
        components[4]=std::make_unique<ClockedSD<Message>>(std::move(stock),sd_dt);
        for(auto logical:order) ids_[logical]=simulator_.add(std::move(components[logical]));
        simulator_.connect(ids_[0],0,ids_[1],0);
        simulator_.connect(ids_[0],0,ids_[2],0);
        simulator_.connect(ids_[1],1,ids_[3],0);
        simulator_.connect(ids_[2],1,ids_[4],0);
        simulator_.connect(ids_[3],1,ids_[4],0);
    }
    Observation observe(double horizon,std::size_t max_steps=1000000) {
        if(!std::isfinite(horizon) || horizon<observed_until_ || horizon<simulator_.now() || max_steps==0)
            throw std::invalid_argument("invalid backlog observation horizon or step budget");
        std::size_t steps=0;
        // Each kernel step is checked; earlier successful steps remain committed on failure.
        // Drain all same-time publications before checking the observable stock identity.
        while(simulator_.next_time()<=horizon) {
            if(steps++>=max_steps) throw std::runtime_error("backlog transition budget exhausted");
            (void)simulator_.step_transactional();
        }
        const auto& source=dynamic_cast<const des::ScheduledSource<Message>&>(simulator_.model(ids_[0]));
        const auto& server=dynamic_cast<const des::SingleServer<Message>&>(simulator_.model(ids_[1]));
        const auto& sd=dynamic_cast<const ClockedSD<Message>&>(simulator_.model(ids_[4]));
        const auto arrivals=source.emitted_count();
        const auto completed=server.completed_count();
        const auto stock=sd.model().state()[0];
        if(arrivals!=server.accepted_count() || completed>arrivals
           || arrivals-completed!=server.waiting()+static_cast<std::size_t>(server.busy())
           || stock!=static_cast<double>(arrivals-completed))
            throw std::logic_error("DES/SD backlog accounting mismatch");
        observed_until_=horizon;
        return {horizon,stock,arrivals,static_cast<std::size_t>(completed),server.waiting(),server.busy()};
    }
private:
    devs::Simulator<Message> simulator_;
    std::array<std::size_t,5> ids_{};
    double observed_until_=0;
};
} // namespace ankurafathom::hybrid::models
