#include "ankurafathom/runtime/data.hpp"
#include "ankurafathom/runtime/sha256.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#if FATHOM_HAS_ARROW
#include <arrow/api.h>
#include <arrow/csv/api.h>
#include <arrow/io/api.h>
#include <arrow/ipc/api.h>
#include <parquet/arrow/reader.h>
#endif

namespace ankurafathom::runtime::data {
namespace {
bool byte_less(const std::string& a,const std::string& b) {
    return std::lexicographical_compare(a.begin(),a.end(),b.begin(),b.end(),
        [](unsigned char x,unsigned char y) { return x<y; });
}
bool less(const Value& a,const Value& b) {
    if(a.index()!=b.index()) throw Error("DATA_KEY","/key_column","key types differ");
    return std::visit([&](const auto& x) {
        using T=std::decay_t<decltype(x)>;
        if constexpr(std::is_same_v<T,std::string>) return byte_less(x,std::get<T>(b));
        else return x<std::get<T>(b);
    },a);
}
bool identifier(const std::string& value) {
    const auto alpha=[](unsigned char c) { return (c>='a' && c<='z') || (c>='A' && c<='Z') || c=='_'; };
    if(value.empty() || !alpha(value[0])) return false;
    return std::all_of(value.begin(),value.end(),[&](unsigned char c) { return alpha(c) || (c>='0' && c<='9'); });
}
void validate_schema(Schema& schema) {
    if(schema.columns.empty() || schema.columns.size()>1024) throw Error("DATA_SCHEMA","/schema","schema needs 1–1024 columns");
    std::set<std::string> names;
    for(auto& column:schema.columns) {
        const auto pointer="/schema/"+column.name;
        if(!identifier(column.name) || !names.insert(column.name).second)
            throw Error("DATA_SCHEMA","/schema","column names must be unique identifiers");
        if(column.type<Type::boolean || column.type>Type::string) throw Error("DATA_SCHEMA",pointer,"unsupported column type");
        if(!column.categories.empty() && column.type!=Type::string)
            throw Error("DATA_SCHEMA",pointer,"categories require a string column");
        std::sort(column.categories.begin(),column.categories.end(),byte_less);
        if(std::adjacent_find(column.categories.begin(),column.categories.end())!=column.categories.end())
            throw Error("DATA_SCHEMA",pointer,"duplicate category");
    }
    if(!names.contains(schema.key_column)) throw Error("DATA_KEY","/key_column","key must name a declared column");
    std::sort(schema.columns.begin(),schema.columns.end(),[](const auto& a,const auto& b) { return a.name<b.name; });
}
#if FATHOM_HAS_ARROW
void integer(Sha256& hash,std::uint64_t value,std::size_t width=8) {
    std::array<char,8> bytes{};
    for(std::size_t i=0;i<width;++i) bytes[i]=static_cast<char>(value>>(i*8));
    hash.update(std::string_view(bytes.data(),width));
}
void text(Sha256& hash,const std::string& value) { integer(hash,value.size());hash.update(value); }
std::string canonical(const Schema& schema,const std::vector<Row>& rows) {
    Sha256 hash;hash.update("AnkuraFathom.table.v1");integer(hash,0,1);
    integer(hash,schema.columns.size());integer(hash,rows.size());text(hash,schema.key_column);
    for(const auto& column:schema.columns) {
        text(hash,column.name);integer(hash,static_cast<std::uint8_t>(column.type),1);text(hash,column.unit);
        integer(hash,column.categories.size());for(const auto& category:column.categories) text(hash,category);
    }
    for(const auto& row:rows) for(const auto& value:row) std::visit([&](const auto& v) {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,std::string>) text(hash,v);
        else if constexpr(std::is_same_v<T,double>) integer(hash,std::bit_cast<std::uint64_t>(v));
        else if constexpr(std::is_same_v<T,bool>) integer(hash,v?1:0,1);
        else integer(hash,static_cast<std::uint64_t>(v),sizeof(T));
    },value);
    return hash.digest();
}
template<class T> T unwrap(arrow::Result<T> result,const std::string& pointer="/source") {
    if(!result.ok()) throw Error("DATA_READ",pointer,result.status().ToString());
    return std::move(result).ValueUnsafe();
}
std::shared_ptr<arrow::DataType> arrow_type(Type type) {
    switch(type) {
        case Type::boolean:return arrow::boolean();case Type::i32:return arrow::int32();
        case Type::i64:return arrow::int64();case Type::u64:return arrow::uint64();
        case Type::f64:return arrow::float64();case Type::string:return arrow::utf8();
    }
    throw Error("DATA_SCHEMA","/schema","unsupported type");
}
std::string snapshot(const std::filesystem::path& path,std::uint64_t limit) {
    std::error_code error;
    if(!std::filesystem::is_regular_file(path,error)) throw Error("DATA_IO","/source","source must be a regular local file");
    std::ifstream file(path,std::ios::binary);
    if(!file) throw Error("DATA_IO","/source","cannot open source");
    std::string bytes;std::array<char,65536> block{};
    while(file) {
        file.read(block.data(),block.size());const auto count=static_cast<std::size_t>(file.gcount());
        if(count>limit-bytes.size()) throw Error("DATA_LIMIT","/source","file byte limit exceeded");
        bytes.append(block.data(),count);
    }
    if(file.bad() || !file.eof()) throw Error("DATA_IO","/source","cannot read source");
    return bytes;
}
std::shared_ptr<arrow::Table> decode(std::string bytes,Format format,const Schema& schema) {
    auto input=std::make_shared<arrow::io::BufferReader>(arrow::Buffer::FromString(std::move(bytes)));
    if(format==Format::parquet) {
        auto reader=unwrap(parquet::arrow::OpenFile(input,arrow::default_memory_pool()));
        reader->set_use_threads(false);return unwrap(reader->ReadTable());
    }
    if(format==Format::arrow_ipc) return unwrap(unwrap(arrow::ipc::RecordBatchFileReader::Open(input))->ToTable());
    auto read=arrow::csv::ReadOptions::Defaults();read.use_threads=false;
    auto parse=arrow::csv::ParseOptions::Defaults();parse.newlines_in_values=true;parse.ignore_empty_lines=false;
    auto convert=arrow::csv::ConvertOptions::Defaults();
    convert.null_values={""};convert.strings_can_be_null=false;convert.quoted_strings_can_be_null=false;
    convert.true_values={"true"};convert.false_values={"false"};
    for(const auto& column:schema.columns) convert.column_types[column.name]=arrow_type(column.type);
    auto reader=unwrap(arrow::csv::TableReader::Make(arrow::io::default_io_context(),input,read,parse,convert));
    return unwrap(reader->Read());
}
Value cell(std::shared_ptr<arrow::Scalar> scalar,Type type,const std::string& pointer) {
    if(!scalar->is_valid) throw Error("DATA_NULL",pointer,"null values are not supported by this binding");
    if(scalar->type->id()==arrow::Type::DICTIONARY)
        scalar=unwrap(std::static_pointer_cast<arrow::DictionaryScalar>(scalar)->GetEncodedValue(),pointer);
    if(!scalar->is_valid) throw Error("DATA_NULL",pointer,"null values are not supported by this binding");
    switch(type) {
        case Type::boolean:return std::static_pointer_cast<arrow::BooleanScalar>(scalar)->value;
        case Type::i32:return std::static_pointer_cast<arrow::Int32Scalar>(scalar)->value;
        case Type::i64:return std::static_pointer_cast<arrow::Int64Scalar>(scalar)->value;
        case Type::u64:return std::static_pointer_cast<arrow::UInt64Scalar>(scalar)->value;
        case Type::f64: {
            const auto value=std::static_pointer_cast<arrow::DoubleScalar>(scalar)->value;
            if(!std::isfinite(value)) throw Error("DATA_VALUE",pointer,"numeric binding values must be finite");
            return value;
        }
        case Type::string:return std::static_pointer_cast<arrow::StringScalar>(scalar)->value->ToString();
    }
    throw Error("DATA_SCHEMA",pointer,"unsupported type");
}
#endif
} // namespace

bool available() noexcept { return FATHOM_HAS_ARROW; }
std::size_t Table::column_index(const std::string& name) const {
    const auto found=std::lower_bound(schema_.columns.begin(),schema_.columns.end(),name,
        [](const auto& column,const std::string& key) { return column.name<key; });
    if(found==schema_.columns.end() || found->name!=name) throw Error("DATA_SCHEMA","/schema","unknown column: "+name);
    return static_cast<std::size_t>(found-schema_.columns.begin());
}
std::optional<std::size_t> Table::find_row(const Value& key) const {
    if(key.index()!=static_cast<std::size_t>(schema_.columns[key_index_].type)-1)
        throw Error("DATA_KEY","/key_column","lookup key has wrong type");
    if(const auto* value=std::get_if<double>(&key);value && !std::isfinite(*value))
        throw Error("DATA_KEY","/key_column","lookup key must be finite");
    const auto found=std::lower_bound(rows_.begin(),rows_.end(),key,[&](const auto& row,const auto& value) { return less(row[key_index_],value); });
    if(found==rows_.end() || less(key,(*found)[key_index_])) return {};
    return static_cast<std::size_t>(found-rows_.begin());
}
Table load_file(const std::filesystem::path& path,Schema schema,const ReadOptions& options) {
    validate_schema(schema);
    if(options.max_file_bytes==0 || options.max_rows==0 || options.max_file_bytes>std::numeric_limits<std::size_t>::max())
        throw Error("DATA_LIMIT","/options","file and row limits must be positive and representable");
    auto format=options.format;
    if(format==Format::infer) {
        const auto suffix=path.extension().string();
        if(suffix==".parquet") format=Format::parquet;
        else if(suffix==".arrow" || suffix==".ipc") format=Format::arrow_ipc;
        else if(suffix==".csv") format=Format::csv;
        else throw Error("DATA_FORMAT","/source","source format must be explicit or use .csv, .parquet, .arrow or .ipc");
    }
    if(format!=Format::csv && format!=Format::parquet && format!=Format::arrow_ipc)
        throw Error("DATA_FORMAT","/options/format","unsupported source format");
#if FATHOM_HAS_ARROW
    if(path.string().find("://")!=std::string::npos) throw Error("DATA_IO","/source","this loader accepts local paths only");
    auto bytes=snapshot(path,options.max_file_bytes);
    Table result;result.file_hash_=sha256(bytes);
    auto table=decode(std::move(bytes),format,schema);
    if(static_cast<std::uint64_t>(table->num_rows())>options.max_rows)
        throw Error("DATA_LIMIT","/source","decoded row limit exceeded");
    const auto valid=table->ValidateFull();
    if(!valid.ok()) throw Error("DATA_READ","/source",valid.ToString());
    if(table->num_columns()!=static_cast<int>(schema.columns.size()))
        throw Error("DATA_SCHEMA","/schema","file columns must exactly match binding schema");
    std::map<std::string,int> indices;
    for(int i=0;i<table->num_columns();++i)
        if(!indices.emplace(table->field(i)->name(),i).second) throw Error("DATA_SCHEMA","/schema","duplicate source column name");
    for(const auto& column:schema.columns) {
        const auto pointer="/schema/"+column.name;
        if(!indices.contains(column.name)) throw Error("DATA_SCHEMA",pointer,"missing source column");
        auto type=table->field(indices.at(column.name))->type();
        if(type->id()==arrow::Type::DICTIONARY) type=std::static_pointer_cast<arrow::DictionaryType>(type)->value_type();
        if(!type->Equals(arrow_type(column.type))) throw Error("DATA_SCHEMA",pointer,"source type differs from declared type");
    }
    result.schema_=std::move(schema);result.key_index_=result.column_index(result.schema_.key_column);
    result.rows_.reserve(static_cast<std::size_t>(table->num_rows()));
    for(std::int64_t i=0;i<table->num_rows();++i) {
        Row row;row.reserve(result.schema_.columns.size());
        for(const auto& column:result.schema_.columns) {
            const auto pointer="/rows/"+std::to_string(i)+"/"+column.name;
            auto value=cell(unwrap(table->column(indices.at(column.name))->GetScalar(i),pointer),column.type,pointer);
            if(!column.categories.empty() && !std::binary_search(column.categories.begin(),column.categories.end(),std::get<std::string>(value),byte_less))
                throw Error("DATA_VALUE",pointer,"value is outside the declared category domain");
            row.push_back(std::move(value));
        }
        result.rows_.push_back(std::move(row));
    }
    std::sort(result.rows_.begin(),result.rows_.end(),[&](const auto& a,const auto& b) { return less(a[result.key_index_],b[result.key_index_]); });
    for(std::size_t i=1;i<result.rows_.size();++i)
        if(!less(result.rows_[i-1][result.key_index_],result.rows_[i][result.key_index_]))
            throw Error("DATA_KEY","/key_column","duplicate source key");
    result.canonical_hash_=canonical(result.schema_,result.rows_);
    return result;
#else
    throw Error("DATA_UNAVAILABLE","/source","data readers require an Arrow-enabled build");
#endif
}
} // namespace ankurafathom::runtime::data
