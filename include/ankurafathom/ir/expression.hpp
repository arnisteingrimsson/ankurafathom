#pragma once

#include "ankurafathom/ir/unit.hpp"
#include "ankurafathom/sd/test_inputs.hpp"

#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ankurafathom::ir {

class Expression {
public:
    static constexpr std::size_t max_source_bytes = 65536;
    static constexpr std::size_t max_depth = 256;
    static bool tick_builtin(const std::string& name) {
        return name == "STEP" || name == "PULSE" || name == "RAMP" ||
            name == "XMILE_STEP" || name == "XMILE_PULSE" || name == "XMILE_RAMP" ||
            name == "XMILE_NEXT_STEP" || name == "XMILE_NEXT_PULSE" || name == "XMILE_NEXT_RAMP";
    }
    static bool builtin(const std::string& name) {
        return tick_builtin(name) || name == "NONNEGATIVE" || name == "MIN" ||
            name == "MAX" || name == "IF_POSITIVE";
    }
    explicit Expression(const std::string& source) {
        if (source.size() > max_source_bytes)
            throw std::invalid_argument("expression exceeds 65536-byte source limit");
        Parser parser(source);
        root_ = parser.parse();
        symbols_ = std::move(parser.symbols);
        functions_ = std::move(parser.functions);
    }

    const std::set<std::string>& symbols() const noexcept { return symbols_; }
    const std::set<std::string>& functions() const noexcept { return functions_; }

    double evaluate(const std::map<std::string, double>& values,
                    const std::map<std::string, std::function<double(double)>>& functions = {}) const {
        const double result = evaluate_node(*root_, values, functions);
        if (!std::isfinite(result)) throw std::domain_error("non-finite expression result");
        return result;
    }

    Dimension infer_unit(const std::map<std::string, Dimension>& units,
                         const std::map<std::string, std::pair<Dimension, Dimension>>& function_units = {}) const {
        return infer_node(*root_, units, function_units);
    }

private:
    struct Node {
        std::size_t depth = 1;
        char operation = 'n';
        double number = 0;
        std::string symbol;
        std::shared_ptr<Node> left;
        std::shared_ptr<Node> right;
        std::vector<std::shared_ptr<Node>> arguments;
    };

    class Parser {
    public:
        explicit Parser(const std::string& source) : source_(source) {}
        std::set<std::string> symbols;
        std::set<std::string> functions;

        std::shared_ptr<Node> parse() {
            auto result = expression();
            skip_spaces();
            if (position_ != source_.size()) throw std::invalid_argument("unexpected expression token");
            return result;
        }

    private:
        struct NestingGuard {
            explicit NestingGuard(std::size_t& depth) : depth_(depth) {
                if (depth_ >= max_depth)
                    throw std::invalid_argument("expression exceeds 256-level nesting limit");
                ++depth_;
            }
            ~NestingGuard() { --depth_; }
            std::size_t& depth_;
        };
        static std::size_t parent_depth(std::size_t child_depth) {
            if (child_depth >= max_depth)
                throw std::invalid_argument("expression exceeds 256-level AST depth limit");
            return child_depth + 1;
        }
        void skip_spaces() {
            while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_])))
                ++position_;
        }
        bool consume(char token) {
            skip_spaces();
            if (position_ < source_.size() && source_[position_] == token) {
                ++position_;
                return true;
            }
            return false;
        }
        std::shared_ptr<Node> binary(char operation, std::shared_ptr<Node> left,
                                     std::shared_ptr<Node> right) {
            auto node = std::make_shared<Node>();
            node->depth = parent_depth(std::max(left->depth, right->depth));
            node->operation = operation;
            node->left = std::move(left);
            node->right = std::move(right);
            return node;
        }
        std::shared_ptr<Node> expression() {
            auto left = term();
            while (true) {
                if (consume('+')) left = binary('+', left, term());
                else if (consume('-')) left = binary('-', left, term());
                else break;
            }
            return left;
        }
        std::shared_ptr<Node> term() {
            auto left = unary();
            while (true) {
                if (consume('*')) left = binary('*', left, unary());
                else if (consume('/')) left = binary('/', left, unary());
                else break;
            }
            return left;
        }
        std::shared_ptr<Node> unary() {
            NestingGuard guard(nesting_);
            if (consume('+')) return unary();
            if (consume('-')) {
                auto node = std::make_shared<Node>();
                node->operation = 'u';
                node->left = unary();
                node->depth = parent_depth(node->left->depth);
                return node;
            }
            return primary();
        }
        std::shared_ptr<Node> binary_number(double value) {
            auto node = std::make_shared<Node>();
            node->number = value;
            return node;
        }
        std::shared_ptr<Node> primary() {
            skip_spaces();
            if (consume('(')) {
                auto result = expression();
                if (!consume(')')) throw std::invalid_argument("unclosed expression parenthesis");
                return result;
            }
            if (position_ >= source_.size()) throw std::invalid_argument("missing expression operand");
            const unsigned char current = static_cast<unsigned char>(source_[position_]);
            if (std::isalpha(current) || current == '_') {
                const auto begin = position_++;
                while (position_ < source_.size()) {
                    const unsigned char c = static_cast<unsigned char>(source_[position_]);
                    if (!std::isalnum(c) && c != '_') break;
                    ++position_;
                }
                auto node = std::make_shared<Node>();
                node->operation = 's';
                node->symbol = source_.substr(begin, position_ - begin);
                if (consume('(')) {
                    node->operation = 'f';
                    node->arguments.push_back(expression());
                    while (consume(',')) node->arguments.push_back(expression());
                    for (const auto& argument : node->arguments)
                        node->depth = std::max(node->depth, parent_depth(argument->depth));
                    if (!consume(')')) throw std::invalid_argument("unclosed function argument");
                    const std::size_t arity = node->symbol == "IF_POSITIVE" ? 3
                        : node->symbol == "MIN" || node->symbol == "MAX" ? 2
                        : node->symbol == "XMILE_NEXT_PULSE" ? 4
                        : node->symbol == "XMILE_NEXT_STEP" || node->symbol == "XMILE_NEXT_RAMP" ? 3
                        : node->symbol == "RAMP" || node->symbol == "XMILE_PULSE"
                        ? 3 : tick_builtin(node->symbol) ? 2 : 1;
                    if (node->arguments.size() != arity) throw std::invalid_argument("wrong function argument count");
                    if (tick_builtin(node->symbol)) { symbols.insert("t"); symbols.insert("dt"); }
                    functions.insert(node->symbol);
                } else {
                    symbols.insert(node->symbol);
                }
                return node;
            }
            if (std::isdigit(current) || current == '.') {
                const char* begin = source_.c_str() + position_;
                char* end = nullptr;
                const double number = std::strtod(begin, &end);
                if (end == begin || !std::isfinite(number)) throw std::invalid_argument("invalid numeric literal");
                position_ += static_cast<std::size_t>(end - begin);
                return binary_number(number);
            }
            throw std::invalid_argument("invalid expression operand");
        }

        const std::string& source_;
        std::size_t position_ = 0;
        std::size_t nesting_ = 0;
    };

    static double evaluate_node(const Node& node, const std::map<std::string, double>& values,
                                const std::map<std::string, std::function<double(double)>>& functions) {
        if (node.operation == 'n') return node.number;
        if (node.operation == 's') return values.at(node.symbol);
        if (node.operation == 'u') return -evaluate_node(*node.left, values, functions);
        if (node.operation == 'f') {
            if (node.symbol == "IF_POSITIVE") {
                const double condition = evaluate_node(*node.arguments[0], values, functions);
                if (!std::isfinite(condition)) throw std::domain_error("nonfinite conditional input");
                return evaluate_node(*node.arguments[condition > 0 ? 1 : 2], values, functions);
            }
            std::vector<double> arguments;
            for (const auto& argument : node.arguments) {
                const double value = evaluate_node(*argument, values, functions);
                if (!std::isfinite(value)) throw std::domain_error("nonfinite function argument");
                arguments.push_back(value);
            }
            if (node.symbol == "NONNEGATIVE") return arguments[0] > 0 ? arguments[0] : 0.;
            if (node.symbol == "MIN") return std::min(arguments[0], arguments[1]);
            if (node.symbol == "MAX") return std::max(arguments[0], arguments[1]);
            if (node.symbol == "XMILE_NEXT_STEP") return sd::xmile_next_step(arguments[0], arguments[1], arguments[2], values.at("t"), values.at("dt"));
            if (node.symbol == "XMILE_NEXT_RAMP") return sd::xmile_next_ramp(arguments[0], arguments[1], arguments[2], values.at("t"), values.at("dt"));
            if (node.symbol == "XMILE_NEXT_PULSE") return sd::xmile_next_pulse(arguments[0], arguments[1], arguments[2], arguments[3], values.at("t"), values.at("dt"));
            if (node.symbol == "XMILE_STEP") return sd::xmile_step(arguments[0], arguments[1], values.at("t"), values.at("dt"));
            if (node.symbol == "XMILE_RAMP") return sd::xmile_ramp(arguments[0], arguments[1], values.at("t"), values.at("dt"));
            if (node.symbol == "XMILE_PULSE") return sd::xmile_pulse(arguments[0], arguments[1], arguments[2], values.at("t"), values.at("dt"));
            if (node.symbol == "STEP") return sd::step_input(arguments[0], arguments[1], values.at("t"), values.at("dt"));
            if (node.symbol == "PULSE") return sd::pulse_input(arguments[0], arguments[1], values.at("t"), values.at("dt"));
            if (node.symbol == "RAMP") return sd::ramp_input(arguments[0], arguments[1], arguments[2], values.at("t"), values.at("dt"));
            return functions.at(node.symbol)(arguments[0]);
        }
        const double left = evaluate_node(*node.left, values, functions);
        const double right = evaluate_node(*node.right, values, functions);
        switch (node.operation) {
            case '+': return left + right;
            case '-': return left - right;
            case '*': return left * right;
            case '/':
                if (right == 0) throw std::domain_error("division by zero");
                return left / right;
            default: throw std::logic_error("unknown expression operation");
        }
    }

    static bool numeric_constant(const Node& node) {
        if (node.operation == 'n') return true;
        if (node.operation == 's' || node.operation == 'f') return false;
        if (node.operation == 'u') return numeric_constant(*node.left);
        return numeric_constant(*node.left) && numeric_constant(*node.right);
    }

    static Dimension infer_node(const Node& node, const std::map<std::string, Dimension>& units,
                                const std::map<std::string, std::pair<Dimension, Dimension>>& functions) {
        if (node.operation == 'n') return {};
        if (node.operation == 's') return units.at(node.symbol);
        if (node.operation == 'u') return infer_node(*node.left, units, functions);
        if (node.operation == 'f') {
            if (node.symbol == "MIN" || node.symbol == "MAX" || node.symbol == "IF_POSITIVE") {
                const bool conditional = node.symbol == "IF_POSITIVE";
                if (conditional && !(infer_node(*node.arguments[0], units, functions) == Dimension{}))
                    throw std::invalid_argument("conditional input must be dimensionless");
                const auto first = infer_node(*node.arguments[conditional ? 1 : 0], units, functions);
                const auto second = infer_node(*node.arguments[conditional ? 2 : 1], units, functions);
                if (!(first == second)) throw std::invalid_argument("function arguments have incompatible units");
                return first;
            }
            if (node.symbol == "NONNEGATIVE") return infer_node(*node.arguments[0], units, functions);
            if (tick_builtin(node.symbol)) {
                const auto& time = units.at("t");
                const std::size_t first_time = node.symbol == "PULSE" ? 0 : 1;
                for (std::size_t i = first_time; i < node.arguments.size(); ++i) {
                    const auto dimension = infer_node(*node.arguments[i], units, functions);
                    if (!(dimension == time) && !(dimension == Dimension{} && numeric_constant(*node.arguments[i])))
                        throw std::invalid_argument("test input time argument has incompatible units");
                }
                if (node.symbol == "PULSE") return {};
                const auto value = infer_node(*node.arguments[0], units, functions);
                if (node.symbol == "XMILE_PULSE" || node.symbol == "XMILE_NEXT_PULSE") return value.divided(time);
                return node.symbol == "STEP" || node.symbol == "XMILE_STEP" || node.symbol == "XMILE_NEXT_STEP" ? value : value.multiplied(time);
            }
            const Dimension argument = infer_node(*node.arguments[0], units, functions);
            const auto& [input, output] = functions.at(node.symbol);
            if (!(argument == input)) throw std::invalid_argument("lookup argument has incompatible units");
            return output;
        }
        const Dimension left = infer_node(*node.left, units, functions);
        const Dimension right = infer_node(*node.right, units, functions);
        switch (node.operation) {
            case '+':
            case '-':
                if (!(left == right)) throw std::invalid_argument("addition or subtraction mixes incompatible units");
                return left;
            case '*': return left.multiplied(right);
            case '/': return left.divided(right);
            default: throw std::logic_error("unknown expression operation");
        }
    }

    std::shared_ptr<Node> root_;
    std::set<std::string> symbols_;
    std::set<std::string> functions_;
};

} // namespace ankurafathom::ir
