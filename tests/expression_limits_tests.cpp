#include "ankurafathom/ir/model.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

using ankurafathom::ir::Expression;
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::string nested(const std::string& prefix, std::size_t count) {
    std::string value;
    for (std::size_t i = 0; i < count; ++i) value += prefix;
    return value + "1" + std::string(count, ')');
}
std::string chain(std::size_t count) {
    std::string value = "1";
    for (std::size_t i = 1; i < count; ++i) value += "+1";
    return value;
}
void accept(const std::string& source, double expected) {
    const Expression expression(source);
    require(expression.evaluate({}) == expected, "boundary expression value changed");
    require(expression.infer_unit({}) == ankurafathom::ir::Dimension{}, "boundary units changed");
}
void reject(const std::string& source) {
    try { (void)Expression(source); }
    catch (const std::invalid_argument& error) {
        require(std::string(error.what()).find("limit") != std::string::npos, "wrong limit diagnostic");
        return;
    }
    throw std::runtime_error("oversized expression accepted");
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected decay model path");
        const auto depth = Expression::max_depth;
        accept(nested("(", depth - 1), 1);
        accept(nested("NONNEGATIVE(", depth - 1), 1);
        accept(std::string(depth - 1, '+') + "1", 1);
        accept(std::string(depth - 1, '-') + "1", -1);
        accept(chain(depth), static_cast<double>(depth));
        accept(std::string(Expression::max_source_bytes - 1, ' ') + "1", 1);
        for (const auto count : {depth, std::size_t{4096}, std::size_t{8192}}) {
            reject(nested("(", count));
            reject(std::string(count, '+') + "1");
            reject(std::string(count, '-') + "1");
            reject(chain(count + 1));
        }
        reject(nested("NONNEGATIVE(", depth));
        reject(std::string(Expression::max_source_bytes, ' ') + "1");
        // Syntax nesting and AST depth are independent: a flat chain inside a
        // function must not evade the AST bound.
        reject("NONNEGATIVE(" + chain(depth) + ")");
        accept("10000000000000000 + -10000000000000000 + 1", 1);
        accept("10 - 3 - 2", 5);
        std::ifstream input(argv[1]);
        std::string document((std::istreambuf_iterator<char>(input)), {});
        const auto begin = document.find("decay_rate * material");
        require(begin != std::string::npos, "fixture expression missing");
        document.replace(begin, std::string("decay_rate * material").size(), std::string(8192, '-') + "1");
        bool caught = false;
        try { (void)ankurafathom::ir::load_json(document); }
        catch (const ankurafathom::ir::Error& error) {
            caught = error.code == "IR_EXPR" && error.pointer == "/components/1/expr";
        }
        require(caught, "loader did not preserve structured expression diagnostic");
        std::cout << "Expression limits: accepted boundaries, rejected nesting/AST/source limits, arithmetic order and IR diagnostics pass\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
