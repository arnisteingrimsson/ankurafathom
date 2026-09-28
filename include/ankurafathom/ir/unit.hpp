#pragma once

#include <cctype>
#include <map>
#include <stdexcept>
#include <string>

namespace ankurafathom::ir {

struct Dimension {
    std::map<std::string, int> powers;

    static Dimension parse(const std::string& text) {
        if (text.empty()) throw std::invalid_argument("empty unit");
        Dimension result;
        int sign = 1;
        std::size_t position = 0;
        while (position < text.size()) {
            const auto begin = position;
            while (position < text.size() && text[position] != '*' && text[position] != '/') ++position;
            const auto token = text.substr(begin, position - begin);
            if (token.empty()) throw std::invalid_argument("empty unit factor");
            if (token != "1") {
                if (!std::isalpha(static_cast<unsigned char>(token[0])) && token[0] != '_')
                    throw std::invalid_argument("invalid unit factor");
                for (char c : token) {
                    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                        throw std::invalid_argument("invalid unit factor");
                }
                result.powers[token] += sign;
            }
            if (position == text.size()) break;
            sign = text[position] == '/' ? -1 : 1;
            ++position;
            if (position == text.size()) throw std::invalid_argument("unit ends with operator");
        }
        result.normalize();
        return result;
    }

    Dimension multiplied(const Dimension& other) const {
        Dimension result = *this;
        for (const auto& [name, exponent] : other.powers) result.powers[name] += exponent;
        result.normalize();
        return result;
    }

    Dimension divided(const Dimension& other) const {
        Dimension result = *this;
        for (const auto& [name, exponent] : other.powers) result.powers[name] -= exponent;
        result.normalize();
        return result;
    }

    bool operator==(const Dimension& other) const noexcept { return powers == other.powers; }

private:
    void normalize() {
        for (auto it = powers.begin(); it != powers.end();) {
            if (it->second == 0) it = powers.erase(it);
            else ++it;
        }
    }
};

} // namespace ankurafathom::ir
