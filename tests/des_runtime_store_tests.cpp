#include "ankurafathom/des/runtime_entity_store.hpp"
#include <iostream>
#include <limits>

namespace {
using namespace ankurafathom::des;
struct Work {};
using Store=RuntimeEntityStore<Work>;
void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
template<class Function> void rejects(Function function) {
    bool caught=false; try { function(); } catch(const std::exception&) { caught=true; }
    require(caught,"invalid runtime store operation succeeded");
}
Store::Record record(double hours,std::int64_t priority,bool urgent,std::string name) {
    return {hours,priority,urgent,std::move(name)};
}
void schema_and_columns() {
    Store store(7,{{"hours",FieldKind::real},{"priority",FieldKind::integer},
        {"urgent",FieldKind::boolean},{"name",FieldKind::string}},100);
    const auto refs=store.spawn_many({record(1.5,-2,true,"first"),record(3.5,4,false,"second")});
    require(refs[0]==Store::Reference{7,100} && refs[1].id==101 && store.next_id()==102,"runtime identities differ");
    require(std::get<double>(store.field(refs[0],"hours"))==1.5 &&
        std::get<std::int64_t>(store.field(refs[1],1))==4 &&
        std::get<bool>(store.field(refs[0],"urgent")) &&
        std::get<std::string>(store.field(refs[1],3))=="second","runtime typed values differ");
    require(std::get<std::vector<double>>(store.column(0))==std::vector<double>({1.5,3.5}),"real column not aligned");
    require(std::get<std::vector<bool>>(store.column(2))==std::vector<bool>({true,false}),"boolean column not aligned");
    auto copy=store;
    copy.update(refs[0],record(8,9,false,"changed"));
    copy.retire(refs[1]);
    require(store.active_count()==2 && std::get<double>(store.field(refs[0],0))==1.5,"copy mutated original");
    require(copy.size()==2 && copy.active_count()==1 && !copy.alive(refs[1]),"retirement changed allocation size");
    require(std::get<std::vector<std::string>>(copy.column(3))[1]=="second","retirement erased audit row");
    rejects([&] { (void)copy.record(refs[1]); });
    rejects([&] { copy.retire(refs[1]); });
    require(copy.spawn(record(2,0,false,"third")).id==102,"retired identity reused");
    require(!store.alive({7,99}) && !store.alive({7,102}),"unknown identity is live");
    rejects([&] { (void)store.alive({8,100}); });
    rejects([&] { (void)store.field(refs[0],"missing"); });
    rejects([&] { (void)store.field(refs[0],4); });
    rejects([&] { Store bad(1,{{"x",FieldKind::real},{"x",FieldKind::integer}}); });
    rejects([&] { Store bad(1,{{"",FieldKind::real}}); });
    rejects([&] { Store bad(1,{{"x",static_cast<FieldKind>(99)}}); });
    Store identity_only(3,{});
    auto id=identity_only.spawn({});
    require(identity_only.record(id).empty() && identity_only.alive(id),"empty schema lost identity");
    rejects([&] { identity_only.spawn({1.}); });
}
void transactions() {
    Store store(1,{{"x",FieldKind::real},{"n",FieldKind::integer}});
    auto first=store.spawn({1.,std::int64_t{2}});
    const auto baseline=store.record(first);
    for(const auto& invalid:std::vector<Store::Record>{{1.},{1.,2.},{true,std::int64_t{2}},
        {std::numeric_limits<double>::infinity(),std::int64_t{0}},
        {std::numeric_limits<double>::quiet_NaN(),std::int64_t{0}}}) {
        rejects([&] { store.spawn_many({{3.,std::int64_t{4}},invalid}); });
        rejects([&] { store.update(first,invalid); });
        require(store.size()==1 && store.next_id()==1 && store.record(first)==baseline,"bad record partially committed");
    }
    rejects([&] { store.update_many({{first,{3.,std::int64_t{4}}},{first,{5.,std::int64_t{6}}}}); });
    rejects([&] { store.update_many({{first,{3.,std::int64_t{4}}},{{1,20},{5.,std::int64_t{6}}}}); });
    require(store.record(first)==baseline,"bad update batch changed row");
    std::vector<Store::Record> rows;
    for(std::int64_t i=0;i<256;++i) rows.push_back({static_cast<double>(i)*.5,i});
    auto refs=store.spawn_many(rows);
    for(std::size_t i=0;i<rows.size();++i) require(store.record(refs[i])==rows[i],"column growth corrupted rows");
    store.update_many({{first,{9.,std::int64_t{10}}},{refs.back(),{11.,std::int64_t{12}}}});
    require(std::get<double>(store.field(first,0))==9 && std::get<std::int64_t>(store.field(refs.back(),1))==12,
        "valid update batch did not commit");
    const auto next=store.next_id(); store.spawn_many({}); store.update_many({});
    require(store.next_id()==next,"empty batch changed identity");
    Store last(4,{},Store::max_entity_id);
    rejects([&] { last.spawn_many({{}, {}}); });
    require(last.size()==0 && last.next_id()==Store::max_entity_id,"exhausted batch advanced identity");
    auto final=last.spawn({});
    rejects([&] { last.spawn({}); });
    last.retire(final);
    rejects([&] { last.spawn({}); });
    require(last.next_id()==Store::max_entity_id+1,"exhaustion wrapped identity");
}
}
int main() {
    try { schema_and_columns(); transactions(); std::cout<<"Runtime schemas, typed columns, identities and atomic batches passed\n"; }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
