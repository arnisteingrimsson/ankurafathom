#pragma once

#include "ankurafathom/des/process_reference.hpp"
#include "ankurafathom/devs/simulator.hpp"
#include <functional>
#include <map>
#include <optional>

namespace ankurafathom::des {

template<class Tag,class Message=ProcessMessage<Tag>>
class ReferenceDelay final:public devs::Atomic<Message> {
    using Token=EntityToken<Tag>;
public:
    using Duration=std::function<double(const Token&)>;
    static constexpr std::uint32_t input_port=0, output_port=1, rejection_port=2, credit_port=3;
    ReferenceDelay(std::uint32_t store,double duration,std::optional<std::size_t> capacity=std::nullopt)
        : ReferenceDelay(store,Duration{[duration](const Token&) { return duration; }},capacity) {
        if(!std::isfinite(duration) || duration<=0) throw std::invalid_argument("delay duration must be finite and positive");
    }
    ReferenceDelay(std::uint32_t store,Duration duration,std::optional<std::size_t> capacity=std::nullopt)
        : store_(store),duration_(std::move(duration)),capacity_(capacity),initial_credit_(capacity.value_or(0)) {
        if(!duration_ || (capacity && *capacity==0)) throw std::invalid_argument("invalid delay provider or capacity");
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<ReferenceDelay>(*this); }
    double time_advance()const override { return *next_event_time()-clock_; }
    std::optional<double> next_event_time()const override {
        if(initial_credit_ || !pending_rejections_.empty()) return clock_;
        return active_.empty() ? std::numeric_limits<double>::infinity() : active_.begin()->first.first;
    }
    std::vector<devs::PortValue<Message>> output()const override {
        const double next=*next_event_time();
        std::vector<devs::PortValue<Message>> result;
        std::size_t credits=initial_credit_;
        for(const auto& [key,job]:active_) {
            if(key.first!=next) break;
            result.push_back({output_port,job.token});
            ++credits;
        }
        for(const auto& token:pending_rejections_) result.push_back({rejection_port,token});
        if(capacity_ && credits) result.push_back({credit_port,QueuePull{credits}});
        return result;
    }
    void internal_transition()override {
        ReferenceDelay candidate(*this);
        candidate.finish_event();
        *this=std::move(candidate);
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || (elapsed>0 && clock_+elapsed<=clock_))
            throw std::invalid_argument("invalid delay elapsed time");
        external_transition_at(clock_+elapsed,(clock_+elapsed)-clock_,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || time-clock_!=elapsed)
            throw std::invalid_argument("inconsistent delay timestamp");
        ReferenceDelay candidate(*this);
        candidate.advance(time);
        candidate.accept(bag);
        *this=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        ReferenceDelay candidate(*this);
        candidate.finish_event();
        candidate.accept(bag);
        *this=std::move(candidate);
    }
    std::size_t active_count()const noexcept { return active_.size(); }
    std::size_t accepted_count()const noexcept { return accepted_; }
    std::size_t completed_count()const noexcept { return completed_; }
    std::size_t rejected_count()const noexcept { return rejected_; }
    double completed_residence_time()const noexcept { return residence_total_; }
    double mean_in_process(double horizon)const { return projected_area(horizon)/horizon; }
    double utilization(double horizon)const {
        if(!capacity_) throw std::logic_error("unbounded delay has no capacity utilization");
        return mean_in_process(horizon)/static_cast<double>(*capacity_);
    }
private:
    struct Active { Token token; double entered; };
    double projected_area(double horizon)const {
        if(!std::isfinite(horizon) || horizon<=0 || horizon<clock_)
            throw std::invalid_argument("invalid delay statistics horizon");
        const auto area=busy_area_+static_cast<double>(active_count())*(horizon-clock_);
        if(!std::isfinite(area)) throw std::overflow_error("delay statistics overflow");
        return area;
    }
    void advance(double time) {
        if(!std::isfinite(time) || time<clock_ || time>*next_event_time())
            throw std::invalid_argument("invalid delay event time");
        busy_area_+=static_cast<double>(active_count())*(time-clock_);
        if(!std::isfinite(busy_area_)) throw std::overflow_error("delay busy area overflow");
        clock_=time;
    }
    void finish_event() {
        const double next=*next_event_time();
        if(!std::isfinite(next)) throw std::logic_error("delay has no internal event");
        advance(next);
        while(!active_.empty() && active_.begin()->first.first==clock_) {
            residence_total_+=clock_-active_.begin()->second.entered;
            if(!std::isfinite(residence_total_)) throw std::overflow_error("delay residence total overflow");
            active_.erase(active_.begin()); ++completed_;
        }
        initial_credit_=0;
        pending_rejections_.clear();
    }
    void accept(const std::vector<devs::Input<Message>>& bag) {
        for(const auto& input:bag) {
            if(input.port!=input_port || !std::holds_alternative<Token>(input.value))
                throw std::invalid_argument("invalid delay input message or port");
            const auto& token=std::get<Token>(input.value);
            if(token.entity.store!=store_ || token.entity.id>0xFFFFFFFFFFFFULL || !seen_.insert(token.entity.id).second)
                throw std::invalid_argument("invalid or duplicate delay reference");
            const double duration=duration_(token);
            if(!std::isfinite(duration) || duration<=0) throw std::invalid_argument("invalid delay duration");
            if(capacity_ && active_.size()==*capacity_) {
                pending_rejections_.push_back(token); ++rejected_;
            } else {
                const double deadline=clock_+duration;
                if(!std::isfinite(deadline) || deadline<=clock_) throw std::overflow_error("delay deadline is not representable");
                active_.emplace(std::pair{deadline,accepted_++},Active{token,clock_});
            }
        }
    }
    std::uint32_t store_;
    Duration duration_;
    std::optional<std::size_t> capacity_;
    std::size_t initial_credit_,accepted_=0,completed_=0,rejected_=0;
    double clock_=0,busy_area_=0,residence_total_=0;
    std::map<std::pair<double,std::size_t>,Active> active_;
    std::set<std::uint64_t> seen_;
    std::vector<Token> pending_rejections_;
};

} // namespace ankurafathom::des
