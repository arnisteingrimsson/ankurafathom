#pragma once

#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace ankurafathom::des {

template<class Tag>
struct EntityRef {
    std::uint32_t store;
    std::uint64_t id;
    auto operator<=>(const EntityRef&) const = default;
};

// A value-owned column store. Read-only column views include retired rows and
// are invalidated by a successful mutation. Keep entity references, not pointers.
template<class Tag,class... Fields>
class EntityStore {
    template<class T> static constexpr bool supported = std::is_same_v<T,double> ||
        std::is_same_v<T,std::int64_t> || std::is_same_v<T,bool> || std::is_same_v<T,std::string>;
    static_assert(sizeof...(Fields)>0 && (supported<Fields> && ...),"unsupported entity field type");
    using Columns=std::tuple<std::vector<Fields>...>;
    static_assert(std::is_nothrow_move_assignable_v<Columns>);
public:
    using Reference=EntityRef<Tag>;
    using Record=std::tuple<Fields...>;
    static constexpr std::uint64_t max_entity_id=(std::uint64_t{1}<<48)-1;

    explicit EntityStore(std::uint32_t store_id,std::uint64_t first_id=0)
        : store_id_(store_id),first_id_(first_id),next_id_(first_id) {
        if(first_id>max_entity_id) throw std::invalid_argument("first entity ID exceeds 48 bits");
    }
    std::uint32_t store_id()const noexcept { return store_id_; }
    std::uint64_t first_id()const noexcept { return first_id_; }
    std::uint64_t next_id()const noexcept { return next_id_; }
    std::size_t size()const noexcept { return live_.size(); }
    std::size_t active_count()const noexcept { return active_; }
    const std::vector<bool>& live_rows()const noexcept { return live_; }
    template<std::size_t Field> const auto& column()const noexcept { return std::get<Field>(columns_); }

    bool alive(Reference reference)const {
        check_store(reference);
        return reference.id>=first_id_ && reference.id<next_id_ && live_[reference.id-first_id_];
    }
    Record record(Reference reference)const {
        return record_at(offset(reference),std::index_sequence_for<Fields...>{});
    }
    template<std::size_t Field> decltype(auto) field(Reference reference)const {
        return std::get<Field>(columns_)[offset(reference)];
    }
    Reference spawn(const Record& value) { return spawn_many({value}).front(); }
    std::vector<Reference> spawn_many(const std::vector<Record>& values) {
        if(values.empty()) return {};
        if(next_id_>max_entity_id || values.size()>max_entity_id-next_id_+1)
            throw std::overflow_error("entity ID space exhausted");
        for(const auto& value:values) validate(value);
        EntityStore candidate(*this);
        std::vector<Reference> references;
        references.reserve(values.size());
        for(const auto& value:values) {
            candidate.append(value,std::index_sequence_for<Fields...>{});
            candidate.live_.push_back(true);
            references.push_back({store_id_,candidate.next_id_++});
            ++candidate.active_;
        }
        *this=std::move(candidate);
        return references;
    }
    void update(Reference reference,const Record& value) { update_many({{reference,value}}); }
    void update_many(const std::vector<std::pair<Reference,Record>>& changes) {
        if(changes.empty()) return;
        std::set<std::uint64_t> seen;
        for(const auto& [reference,value]:changes) {
            (void)offset(reference);
            validate(value);
            if(!seen.insert(reference.id).second) throw std::invalid_argument("duplicate entity update");
        }
        EntityStore candidate(*this);
        for(const auto& [reference,value]:changes)
            candidate.assign(offset(reference),value,std::index_sequence_for<Fields...>{});
        *this=std::move(candidate);
    }
    void retire(Reference reference) {
        const auto index=offset(reference);
        live_[index]=false;
        --active_;
    }
private:
    void check_store(Reference reference)const {
        if(reference.store!=store_id_) throw std::invalid_argument("entity reference belongs to a different store");
    }
    std::size_t offset(Reference reference)const {
        if(!alive(reference)) throw std::out_of_range("entity reference is unknown or retired");
        return static_cast<std::size_t>(reference.id-first_id_);
    }
    template<class T> static void validate_field(const T& value) {
        if constexpr(std::is_same_v<T,double>)
            if(!std::isfinite(value)) throw std::invalid_argument("entity real field must be finite");
    }
    static void validate(const Record& value) {
        std::apply([](const auto&... fields) { (validate_field(fields),...); },value);
    }
    template<std::size_t... I> Record record_at(std::size_t index,std::index_sequence<I...>)const {
        return Record{std::get<I>(columns_)[index]...};
    }
    template<std::size_t... I> void append(const Record& value,std::index_sequence<I...>) {
        (std::get<I>(columns_).push_back(std::get<I>(value)),...);
    }
    template<std::size_t... I> void assign(std::size_t index,const Record& value,std::index_sequence<I...>) {
        ((std::get<I>(columns_)[index]=std::get<I>(value)),...);
    }
    std::uint32_t store_id_;
    std::uint64_t first_id_;
    std::uint64_t next_id_;
    Columns columns_;
    std::vector<bool> live_;
    std::size_t active_=0;
};

} // namespace ankurafathom::des
