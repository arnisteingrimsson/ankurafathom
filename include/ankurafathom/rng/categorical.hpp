#pragma once

#include "ankurafathom/rng/philox.hpp"
#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace ankurafathom::rng {

class Categorical {
public:
    explicit Categorical(const std::vector<double>& probabilities):size_(probabilities.size()) {
        if(probabilities.empty()) throw std::invalid_argument("categorical distribution needs probabilities");
        double total=0,correction=0;
        for(double value:probabilities) {
            if(!std::isfinite(value) || value<0 || value>1) throw std::invalid_argument("invalid categorical probability");
            add(total,correction,value);
        }
        if(std::abs(total-1)>1e-12) throw std::invalid_argument("categorical probabilities must sum to one");
        double cumulative=0,previous=0;
        correction=0;
        for(std::size_t i=0;i<probabilities.size();++i) {
            add(cumulative,correction,probabilities[i]);
            if(probabilities[i]>0) {
                previous=std::clamp(cumulative/total,previous,1.0);
                cutoffs_.push_back({previous,i});
            }
        }
    }
    std::size_t size()const noexcept { return size_; }
    std::size_t sample(std::uint32_t word)const noexcept {
        const double value=uniform_open(word);
        for(const auto& [cutoff,index]:cutoffs_) if(value<cutoff) return index;
        return cutoffs_.back().second;
    }
private:
    static void add(double& sum,double& correction,double value)noexcept {
        const double adjusted=value-correction,next=sum+adjusted;
        correction=(next-sum)-adjusted;
        sum=next;
    }
    std::size_t size_;
    std::vector<std::pair<double,std::size_t>> cutoffs_;
};

} // namespace ankurafathom::rng
