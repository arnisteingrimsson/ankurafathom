#pragma once

#include "ankurafathom/des/process_reference.hpp"
#include "ankurafathom/des/queue_discipline.hpp"
#include "ankurafathom/devs/simulator.hpp"
#include <algorithm>
#include <deque>
#include <functional>
#include <optional>

namespace ankurafathom::des {

template<class Tag,class Message=ProcessMessage<Tag>>
class ReferenceQueue final:public devs::Atomic<Message> {
    using Token=EntityToken<Tag>;
public:
    using Priority=std::function<std::int32_t(const Token&)>;
    static constexpr std::uint32_t input_port=0, output_port=1, pull_port=2, rejection_port=3;
    explicit ReferenceQueue(std::uint32_t store,std::optional<std::size_t> capacity=std::nullopt,
                            QueueDiscipline discipline=QueueDiscipline::fifo,Priority priority={})
        : store_(store),capacity_(capacity),discipline_(discipline),priority_(std::move(priority)) {
        if(!valid_queue_discipline(discipline)) throw std::invalid_argument("unknown reference queue discipline");
        if(priority_ && discipline!=QueueDiscipline::priority)
            throw std::invalid_argument("priority provider requires priority queue discipline");
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<ReferenceQueue>(*this); }
    double time_advance()const override { return pending_.empty() ? std::numeric_limits<double>::infinity() : 0; }
    std::optional<double> next_event_time()const override {
        return pending_.empty() ? std::numeric_limits<double>::infinity() : clock_;
    }
    std::vector<devs::PortValue<Message>> output()const override { return pending_; }
    void internal_transition()override {
        if(pending_.empty()) throw std::logic_error("queue has no publication");
        pending_.clear();
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || (elapsed>0 && clock_+elapsed<=clock_))
            throw std::invalid_argument("invalid queue elapsed time");
        external_transition_at(clock_+elapsed,(clock_+elapsed)-clock_,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || time-clock_!=elapsed)
            throw std::invalid_argument("inconsistent queue timestamp");
        ReferenceQueue candidate(*this);
        candidate.advance(time);
        candidate.accept(bag);
        *this=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        if(pending_.empty()) throw std::logic_error("queue has no confluent publication");
        ReferenceQueue candidate(*this);
        candidate.pending_.clear();
        candidate.accept(bag);
        *this=std::move(candidate);
    }
    std::size_t waiting()const noexcept { return waiting_.size(); }
    std::size_t demand()const noexcept { return demand_; }
    std::size_t accepted_count()const noexcept { return accepted_; }
    std::size_t released_count()const noexcept { return released_; }
    std::size_t rejected_count()const noexcept { return rejected_; }
    double total_waiting_time()const noexcept { return wait_total_; }
    double mean_queue_length(double horizon)const {
        if(!std::isfinite(horizon) || horizon<=0 || horizon<clock_)
            throw std::invalid_argument("invalid queue statistics horizon");
        const auto area=queue_area_+static_cast<double>(waiting())*(horizon-clock_);
        if(!std::isfinite(area)) throw std::overflow_error("queue statistics overflow");
        return area/horizon;
    }
private:
    struct Waiting { Token token; double entered; };
    void advance(double time) {
        if(!std::isfinite(time) || time<clock_ || time>*next_event_time())
            throw std::invalid_argument("invalid reference queue event time");
        queue_area_+=static_cast<double>(waiting())*(time-clock_);
        if(!std::isfinite(queue_area_)) throw std::overflow_error("queue area overflow");
        clock_=time;
    }
    void accept(const std::vector<devs::Input<Message>>& bag) {
        std::vector<Token> incoming;
        for(const auto& input:bag) {
            if(input.port==pull_port && std::holds_alternative<QueuePull>(input.value)) {
                const auto count=std::get<QueuePull>(input.value).count;
                if(count==0 || count>std::numeric_limits<std::size_t>::max()-demand_)
                    throw std::invalid_argument("invalid or overflowing queue demand");
                demand_+=count;
            } else if(input.port==input_port && std::holds_alternative<Token>(input.value)) {
                auto token=std::get<Token>(input.value);
                if(token.entity.store!=store_ || token.entity.id>0xFFFFFFFFFFFFULL || !seen_.insert(token.entity.id).second)
                    throw std::invalid_argument("invalid or duplicate queue reference");
                if(priority_) token.priority=priority_(token);
                incoming.push_back(token);
            } else throw std::invalid_argument("invalid queue message or port");
        }
        if(discipline_==QueueDiscipline::priority)
            std::sort(incoming.begin(),incoming.end(),[](const Token& a,const Token& b) {
                return a.priority!=b.priority ? a.priority<b.priority : a.entity.id<b.entity.id;
            });
        for(const auto& token:incoming) {
            if(capacity_ && waiting_.size()>=demand_ && waiting_.size()-demand_>=*capacity_) {
                pending_.push_back({rejection_port,token}); ++rejected_;
            } else { waiting_.push_back({token,clock_}); ++accepted_; }
        }
        if(discipline_==QueueDiscipline::priority)
            std::stable_sort(waiting_.begin(),waiting_.end(),[](const Waiting& a,const Waiting& b) {
                return a.token.priority<b.token.priority;
            });
        while(demand_ && !waiting_.empty()) {
            const auto selected=discipline_==QueueDiscipline::lifo ? waiting_.back() : waiting_.front();
            if(discipline_==QueueDiscipline::lifo) waiting_.pop_back(); else waiting_.pop_front();
            wait_total_+=clock_-selected.entered;
            if(!std::isfinite(wait_total_)) throw std::overflow_error("queue waiting-time overflow");
            pending_.push_back({output_port,selected.token});
            --demand_; ++released_;
        }
    }
    std::uint32_t store_;
    std::optional<std::size_t> capacity_;
    QueueDiscipline discipline_;
    Priority priority_;
    double clock_=0,queue_area_=0,wait_total_=0;
    std::size_t demand_=0,accepted_=0,released_=0,rejected_=0;
    std::deque<Waiting> waiting_;
    std::set<std::uint64_t> seen_;
    std::vector<devs::PortValue<Message>> pending_;
};

} // namespace ankurafathom::des
