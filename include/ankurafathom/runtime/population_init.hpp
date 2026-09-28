#pragma once
#include "ankurafathom/runtime/data.hpp"
#include "ankurafathom/abm/population_store.hpp"
#include <limits>
#include <map>
#include <set>
#include <span>
#include <type_traits>

namespace ankurafathom::runtime::data {
struct PopulationField {
    std::string field;
    std::string column;
    // Expected target unit, checked exactly against the source declaration.
    // Native population schemas do not otherwise carry dimensional information.
    std::string unit;
};
template<class Tag> struct PopulationReceipt {
    struct Entry { Value source_key;typename abm::PopulationStore<Tag>::Reference agent; };
    std::string file_hash,canonical_hash;
    std::vector<Entry> entries;
};

// Initialize a never-populated store. External keys select row order and are
// retained in the receipt; they are never cast into simulation entity IDs.
// Records, assigned IDs, membership and the receipt are staged before commit.
template<class Tag>
PopulationReceipt<Tag> initialize_population(abm::PopulationStore<Tag>& population,
                                             const Table& table,std::span<const PopulationField> fields) {
    using Store=abm::PopulationStore<Tag>;
    static_assert(std::is_nothrow_move_assignable_v<Store>);
    static_assert(std::is_nothrow_move_constructible_v<PopulationReceipt<Tag>>);
    if(population.size()!=0 || population.next_id()!=population.first_id())
        throw Error("DATA_POPULATION","/population","population initialization requires a never-populated store");
    if(fields.size()!=population.schema().size())
        throw Error("DATA_MAPPING","/fields","every population field requires exactly one mapping");
    struct Binding { std::size_t column;std::string pointer; };
    std::vector<Binding> bindings(population.schema().size());
    std::map<std::string,std::size_t> targets;
    for(std::size_t i=0;i<population.schema().size();++i) targets.emplace(population.schema()[i].name,i);
    std::set<std::string> seen;
    for(std::size_t i=0;i<fields.size();++i) {
        const auto& field=fields[i];const auto pointer="/fields/"+std::to_string(i);
        if(!targets.contains(field.field) || !seen.insert(field.field).second)
            throw Error("DATA_MAPPING",pointer+"/field","unknown or duplicate target field");
        std::size_t source=0;
        try { source=table.column_index(field.column); }
        catch(const Error&) { throw Error("DATA_MAPPING",pointer+"/column","unknown source column"); }
        const auto& column=table.schema().columns[source];
        if(column.unit!=field.unit) throw Error("DATA_UNIT",pointer+"/unit","source and target unit declarations differ");
        const auto target=targets.at(field.field);
        const auto kind=population.schema()[target].kind;
        const bool compatible=(kind==des::FieldKind::real && column.type==Type::f64) ||
            (kind==des::FieldKind::integer && (column.type==Type::i32 || column.type==Type::i64 || column.type==Type::u64)) ||
            (kind==des::FieldKind::boolean && column.type==Type::boolean) ||
            (kind==des::FieldKind::string && column.type==Type::string);
        if(!compatible) throw Error("DATA_MAPPING",pointer+"/column","source type cannot initialize target field");
        bindings[target]={source,pointer};
    }
    if(table.rows().size()>Store::max_entity_id-population.next_id()+1)
        throw Error("DATA_POPULATION","/population","population initialization exceeds the 48-bit entity ID space");
    std::vector<typename Store::Record> records;records.reserve(table.rows().size());
    for(std::size_t row=0;row<table.rows().size();++row) {
        typename Store::Record record;record.reserve(bindings.size());
        for(const auto& binding:bindings) {
            record.push_back(std::visit([&](const auto& value)->typename Store::Value {
                using T=std::decay_t<decltype(value)>;
                if constexpr(std::is_same_v<T,std::int32_t>) return static_cast<std::int64_t>(value);
                else if constexpr(std::is_same_v<T,std::uint64_t>) {
                    if(value>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
                        throw Error("DATA_RANGE","/rows/"+std::to_string(row)+"/"+table.schema().columns[binding.column].name,
                                    "unsigned value cannot fit a signed population integer");
                    return static_cast<std::int64_t>(value);
                } else return value;
            },table.rows()[row][binding.column]));
        }
        records.push_back(std::move(record));
    }
    PopulationReceipt<Tag> receipt{table.file_hash(),table.canonical_hash(),{}};
    receipt.entries.reserve(table.rows().size());
    const auto key=table.column_index(table.schema().key_column);
    // Allocate source-key copies before the population can change.
    for(const auto& row:table.rows()) receipt.entries.push_back({row[key],{population.store_id(),0}});
    auto candidate=population;
    const auto refs=candidate.spawn_many(records);
    for(std::size_t i=0;i<refs.size();++i) receipt.entries[i].agent=refs[i];
    population=std::move(candidate);
    return receipt;
}
} // namespace ankurafathom::runtime::data
