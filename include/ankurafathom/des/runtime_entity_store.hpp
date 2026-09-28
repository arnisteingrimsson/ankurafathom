#pragma once

#include "ankurafathom/des/entity_store.hpp"
#include <map>
#include <variant>

namespace ankurafathom::des {

enum class FieldKind { real, integer, boolean, string };
struct EntityField {
    std::string name;
    FieldKind kind;
    bool operator==(const EntityField&)const=default;
};

template<class Tag>
class RuntimeEntityStore {
public:
    using Reference=EntityRef<Tag>;
    using Value=std::variant<double,std::int64_t,bool,std::string>;
    using Record=std::vector<Value>;
    using Column=std::variant<std::vector<double>,std::vector<std::int64_t>,std::vector<bool>,std::vector<std::string>>;
    static constexpr std::uint64_t max_entity_id=(std::uint64_t{1}<<48)-1;
    RuntimeEntityStore(std::uint32_t store,std::vector<EntityField> schema,std::uint64_t first=0)
        :store_(store),schema_(std::move(schema)),first_(first),next_(first) {
        if(first>max_entity_id) throw std::invalid_argument("first entity ID exceeds 48 bits");
        for(const auto& field:schema_) {
            if(field.name.empty() || !indices_.emplace(field.name,columns_.size()).second)
                throw std::invalid_argument("entity field names must be nonempty and unique");
            switch(field.kind) {
                case FieldKind::real: columns_.emplace_back(std::vector<double>{}); break;
                case FieldKind::integer: columns_.emplace_back(std::vector<std::int64_t>{}); break;
                case FieldKind::boolean: columns_.emplace_back(std::vector<bool>{}); break;
                case FieldKind::string: columns_.emplace_back(std::vector<std::string>{}); break;
                default: throw std::invalid_argument("unknown entity field kind");
            }
        }
    }
    std::uint32_t store_id()const noexcept { return store_; }
    std::uint64_t first_id()const noexcept { return first_; }
    std::uint64_t next_id()const noexcept { return next_; }
    std::size_t size()const noexcept { return live_.size(); }
    std::size_t active_count()const noexcept { return active_; }
    const std::vector<EntityField>& schema()const noexcept { return schema_; }
    const std::vector<bool>& live_rows()const noexcept { return live_; }
    const Column& column(std::size_t index)const { return columns_.at(index); }
    std::size_t field_index(const std::string& name)const { return indices_.at(name); }
    bool alive(Reference reference)const {
        if(reference.store!=store_) throw std::invalid_argument("entity reference belongs to a different store");
        return reference.id>=first_ && reference.id<next_ && live_[reference.id-first_];
    }
    Value field(Reference reference,std::size_t index)const { return value_at(offset(reference),index); }
    Value field(Reference reference,const std::string& name)const { return field(reference,field_index(name)); }
    Record record(Reference reference)const {
        const auto row=offset(reference);
        Record result; result.reserve(columns_.size());
        for(std::size_t i=0;i<columns_.size();++i) result.push_back(value_at(row,i));
        return result;
    }
    Reference spawn(const Record& value) { return spawn_many({value}).front(); }
    std::vector<Reference> spawn_many(const std::vector<Record>& values) {
        if(values.empty()) return {};
        if(next_>max_entity_id || values.size()>max_entity_id-next_+1) throw std::overflow_error("entity ID space exhausted");
        for(const auto& value:values) validate(value);
        RuntimeEntityStore candidate(*this);
        std::vector<Reference> references; references.reserve(values.size());
        for(const auto& value:values) {
            for(std::size_t i=0;i<columns_.size();++i)
                std::visit([&](auto& column) {
                    using T=typename std::decay_t<decltype(column)>::value_type;
                    column.push_back(std::get<T>(value[i]));
                },candidate.columns_[i]);
            candidate.live_.push_back(true);
            references.push_back({store_,candidate.next_++});
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
            (void)offset(reference); validate(value);
            if(!seen.insert(reference.id).second) throw std::invalid_argument("duplicate entity update");
        }
        RuntimeEntityStore candidate(*this);
        for(const auto& [reference,value]:changes) {
            const auto row=offset(reference);
            for(std::size_t i=0;i<columns_.size();++i)
                std::visit([&](auto& column) {
                    using T=typename std::decay_t<decltype(column)>::value_type;
                    column[row]=std::get<T>(value[i]);
                },candidate.columns_[i]);
        }
        *this=std::move(candidate);
    }
    void retire(Reference reference) { const auto row=offset(reference); live_[row]=false; --active_; }
private:
    std::size_t offset(Reference reference)const {
        if(!alive(reference)) throw std::out_of_range("entity reference is unknown or retired");
        return static_cast<std::size_t>(reference.id-first_);
    }
    Value value_at(std::size_t row,std::size_t index)const {
        return std::visit([&](const auto& column)->Value {
            using T=typename std::decay_t<decltype(column)>::value_type;
            return T(column[row]);
        },columns_.at(index));
    }
    void validate(const Record& value)const {
        if(value.size()!=schema_.size()) throw std::invalid_argument("entity record does not match schema width");
        for(std::size_t i=0;i<value.size();++i) {
            if(value[i].index()!=columns_[i].index()) throw std::invalid_argument("entity value has wrong field type");
            if(const auto* real=std::get_if<double>(&value[i]); real && !std::isfinite(*real))
                throw std::invalid_argument("entity real field must be finite");
        }
    }
    std::uint32_t store_;
    std::vector<EntityField> schema_;
    std::map<std::string,std::size_t> indices_;
    std::vector<Column> columns_;
    std::uint64_t first_,next_;
    std::vector<bool> live_;
    std::size_t active_=0;
};

} // namespace ankurafathom::des
