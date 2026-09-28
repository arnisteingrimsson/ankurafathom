#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ankurafathom::abm {
// Contract: docs/SEMANTICS.md, "ABM networks and bounded message topics".
// Publications preserve visible references; a successful flush invalidates them.
template<class Payload>
class TopicBroker {
public:
    struct Message {
        std::uint64_t sender;
        std::optional<std::uint64_t> receiver;
        std::uint64_t sequence;
        Payload payload;
        bool operator==(const Message&)const=default;
    };
    struct Publication { std::string topic; Message message; };
    void declare_topic(std::string name,std::size_t capacity) {
        if(name.empty() || topics_.contains(name)) throw std::invalid_argument("empty or duplicate broker topic");
        topics_.emplace(std::move(name),Topic{capacity,{},{}});
    }
    void publish_many(const std::vector<Publication>& publications) {
        if(publications.empty()) return;
        TopicBroker candidate(*this);
        for(const auto& publication:publications) {
            auto found=candidate.topics_.find(publication.topic);
            if(found==candidate.topics_.end()) throw std::invalid_argument("unknown broker topic");
            auto& topic=found->second;
            if(topic.pending.size()>=topic.capacity) throw std::overflow_error("broker topic capacity exceeded");
            const auto& message=publication.message;
            if(!topic.pending.emplace(std::make_pair(message.sender,message.sequence),message).second)
                throw std::invalid_argument("duplicate sender sequence in pending topic");
        }
        // Only pending buffers change. Consumers may publish replies while
        // iterating visible messages, so their vectors must stay in place.
        // Every topic exists in both copies; map swaps do not allocate or copy.
        for(auto& [name,topic]:topics_) topic.pending.swap(candidate.topics_.at(name).pending);
    }
    void flush() {
        TopicBroker candidate(*this);
        for(auto& [name,topic]:candidate.topics_) {
            (void)name;
            topic.visible.clear(); topic.visible.reserve(topic.pending.size());
            for(const auto& [key,message]:topic.pending) { (void)key; topic.visible.push_back(message); }
            topic.pending.clear();
        }
        *this=std::move(candidate);
    }
    std::size_t pending_count(const std::string& topic)const { return topics_.at(topic).pending.size(); }
    const std::vector<Message>& visible(const std::string& topic)const { return topics_.at(topic).visible; }
    std::vector<Message> for_receiver(const std::string& topic,std::uint64_t receiver)const {
        std::vector<Message> messages;
        for(const auto& message:visible(topic))
            if(!message.receiver || *message.receiver==receiver) messages.push_back(message);
        return messages;
    }
private:
    struct Topic {
        std::size_t capacity;
        std::map<std::pair<std::uint64_t,std::uint64_t>,Message> pending;
        std::vector<Message> visible;
    };
    std::map<std::string,Topic> topics_;
};
} // namespace ankurafathom::abm
