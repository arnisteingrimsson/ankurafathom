#pragma once
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace ankurafathom::runtime::data {
// These codes are part of the versioned canonical encoding.
enum class Type:std::uint8_t { boolean=1,i32=2,i64=3,u64=4,f64=5,string=6 };
using Value=std::variant<bool,std::int32_t,std::int64_t,std::uint64_t,double,std::string>;
using Row=std::vector<Value>;
struct Column {
    std::string name;
    Type type;
    std::string unit;
    // A string domain is an unordered set, normalized during load.
    std::vector<std::string> categories;
};
struct Schema { std::vector<Column> columns;std::string key_column; };
enum class Format { infer,csv,parquet,arrow_ipc };
struct ReadOptions {
    Format format=Format::infer;
    std::uint64_t max_file_bytes=256*1024*1024;
    std::uint64_t max_rows=1000000;
};
class Error:public std::runtime_error {
public:
    Error(std::string code,std::string pointer,std::string message)
        :std::runtime_error(std::move(message)),code(std::move(code)),pointer(std::move(pointer)) {}
    std::string code,pointer;
};
class Table {
public:
    const Schema& schema() const noexcept { return schema_; }
    const std::vector<Row>& rows() const noexcept { return rows_; }
    const std::string& file_hash() const noexcept { return file_hash_; }
    const std::string& canonical_hash() const noexcept { return canonical_hash_; }
    std::size_t column_index(const std::string& name) const;
    // Exact typed keys; no numeric or string coercion. A missing key is not row 0.
    std::optional<std::size_t> find_row(const Value& key) const;
private:
    friend Table load_file(const std::filesystem::path&,Schema,const ReadOptions&);
    Table()=default;
    Schema schema_;
    std::vector<Row> rows_;
    std::string file_hash_,canonical_hash_;
    std::size_t key_index_=0;
};
bool available() noexcept;
Table load_file(const std::filesystem::path& path,Schema schema,const ReadOptions& options={});
} // namespace ankurafathom::runtime::data
