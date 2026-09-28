#pragma once
#include "ankurafathom/des/process_reference.hpp"
#include "ankurafathom/devs/simulator.hpp"
#include "ankurafathom/hybrid/scalar_publication.hpp"
#include "ankurafathom/rng/categorical.hpp"
#include <functional>

namespace ankurafathom::hybrid {

template<class Tag,class Message=std::variant<des::EntityToken<Tag>,ScalarPublication>>
class SignalSelect final:public devs::Atomic<Message> {
public:
    using Token=des::EntityToken<Tag>;
    using Probabilities=std::function<std::vector<double>(const std::vector<double>&,double)>;
    struct Draws { std::uint64_t seed=0;std::uint32_t scenario=0,replication=0,stream=0; };
    static constexpr std::uint32_t input_port=0,signal_port=2;
    SignalSelect(std::uint32_t store,std::vector<double> initial,Probabilities probabilities,Draws draws={})
        :store_(store),signals_(std::move(initial)),probabilities_(std::move(probabilities)),draws_(draws) {
        if(signals_.empty() || !probabilities_) throw std::invalid_argument("selector requires scalars and projection");
        for(double x:signals_) if(!std::isfinite(x)) throw std::invalid_argument("nonfinite selector input");
        (void)rng::pack_counter({draws_.scenario,draws_.replication,0,0,draws_.stream,0});
        distribution_=rng::Categorical(probabilities_(signals_,0));
        if(distribution_->size()>65535) throw std::invalid_argument("too many selector branches");
        counts_.resize(distribution_->size());
    }
    std::unique_ptr<devs::Atomic<Message>> clone()const override { return std::make_unique<SignalSelect>(*this); }
    double time_advance()const override { return pending_.empty()?std::numeric_limits<double>::infinity():0; }
    std::optional<double> next_event_time()const override { return pending_.empty()?std::numeric_limits<double>::infinity():clock_; }
    std::vector<devs::PortValue<Message>> output()const override { return pending_; }
    void internal_transition()override {
        if(pending_.empty()) throw std::logic_error("selector has no pending output");
        pending_.clear();
    }
    void external_transition(double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!std::isfinite(elapsed) || elapsed<0 || (elapsed>0 && clock_+elapsed<=clock_))
            throw std::invalid_argument("invalid selector elapsed time");
        external_transition_at(clock_+elapsed,(clock_+elapsed)-clock_,bag);
    }
    void external_transition_at(double time,double elapsed,const std::vector<devs::Input<Message>>& bag)override {
        if(!pending_.empty() || !std::isfinite(time) || !std::isfinite(elapsed) || elapsed<0 || time<clock_ || time-clock_!=elapsed)
            throw std::invalid_argument("invalid selector timestamp or pending output");
        auto candidate=*this;candidate.accept(time,bag);*this=std::move(candidate);
    }
    void confluent_transition(const std::vector<devs::Input<Message>>& bag)override {
        if(pending_.empty()) throw std::logic_error("selector has no confluent output");
        auto candidate=*this;candidate.pending_.clear();candidate.accept(clock_,bag);*this=std::move(candidate);
    }
    double time()const noexcept { return clock_; }
    const std::vector<double>& signals()const noexcept { return signals_; }
    const std::vector<std::size_t>& counts()const noexcept { return counts_; }
    std::size_t received_count()const noexcept { return seen_.size(); }
    std::optional<std::uint64_t> revision()const noexcept { return revision_; }
private:
    void accept(double time,const std::vector<devs::Input<Message>>& bag) {
        if(bag.empty()) throw std::invalid_argument("empty selector input bag");
        const ScalarPublication* publication=nullptr;std::size_t owner=0;
        std::vector<const Token*> tokens;
        for(const auto& input:bag) {
            if(const auto* token=std::get_if<Token>(&input.value)) {
                if(input.port!=input_port || token->entity.store!=store_ || token->entity.id>0xFFFFFFFFFFFFULL ||
                   !seen_.insert(token->entity.id).second) throw std::invalid_argument("invalid or duplicate selector token");
                tokens.push_back(token);
            } else if(const auto* p=std::get_if<ScalarPublication>(&input.value)) {
                if(publication || input.port!=signal_port || p->time!=time || p->values.size()!=signals_.size() ||
                   (owner_ && *owner_!=input.source) || (revision_ && p->revision<=*revision_))
                    throw std::invalid_argument("invalid selector scalar width, timestamp, owner or revision");
                for(double x:p->values) if(!std::isfinite(x)) throw std::invalid_argument("nonfinite selector scalar");
                publication=p;owner=input.source;
            } else throw std::invalid_argument("invalid selector message type");
        }
        std::sort(tokens.begin(),tokens.end(),[](const Token* a,const Token* b) { return a->entity.id<b->entity.id; });
        for(const auto* token:tokens) {
            const auto branch=distribution_->sample(rng::draw(draws_.seed,{draws_.scenario,draws_.replication,token->entity.id,0,draws_.stream,0})[0]);
            ++counts_[branch];pending_.push_back({static_cast<std::uint32_t>(branch+1),Message{*token}});
        }
        if(publication) {
            auto probabilities=probabilities_(publication->values,time);
            if(probabilities.size()!=counts_.size()) throw std::invalid_argument("selector branch count changed");
            distribution_=rng::Categorical(probabilities);
            signals_=publication->values;revision_=publication->revision;owner_=owner;
        }
        clock_=time;
    }
    std::uint32_t store_;
    std::vector<double> signals_;
    Probabilities probabilities_;
    Draws draws_;
    std::optional<rng::Categorical> distribution_;
    std::vector<std::size_t> counts_;
    std::set<std::uint64_t> seen_;
    std::vector<devs::PortValue<Message>> pending_;
    std::optional<std::size_t> owner_;
    std::optional<std::uint64_t> revision_;
    double clock_=0;
};
} // namespace ankurafathom::hybrid
