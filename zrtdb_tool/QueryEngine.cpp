// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#include "QueryEngine.hpp"

#include "ToolUtil.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace mmdb::dbio {

static inline char lower_ch(char c)
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

namespace tu = mmdb::dbio::toolutil;

bool QueryEngine::hasGlob(std::string_view pattern)
{
    return pattern.find('*') != std::string_view::npos || pattern.find('?') != std::string_view::npos;
}

bool QueryEngine::containsCI(std::string_view text, std::string_view needle)
{
    if (needle.empty())
        return true;
    const std::string t = tu::lower(text);
    const std::string n = tu::lower(needle);
    return t.find(n) != std::string::npos;
}

// 简单 glob：* 匹配任意串，? 匹配单字符
bool QueryEngine::globMatchCI(std::string_view pattern, std::string_view text)
{
    // DP (m+1)*(n+1) 在长字符串下可能大；这里用双指针回溯法
    size_t p = 0, t = 0;
    size_t star = std::string::npos;
    size_t match = 0;

    auto eqci = [](char a, char b) {
        return lower_ch(a) == lower_ch(b);
    };

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || eqci(pattern[p], text[t]))) {
            ++p;
            ++t;
            continue;
        }
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = t;
            continue;
        }
        if (star != std::string::npos) {
            p = star + 1;
            t = ++match;
            continue;
        }
        return false;
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

std::vector<std::string> QueryEngine::splitCommandTokens(const std::string& line)
{
    std::vector<std::string> out;
    std::string cur;
    char quote = 0;
    bool esc = false;

    auto flush = [&]() {
        if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    };

    for (char ch : line) {
        if (esc) {
            cur.push_back(ch);
            esc = false;
            continue;
        }
        if (ch == '\\') {
            // 允许 \" 或 \' 等
            esc = true;
            cur.push_back(ch);
            continue;
        }
        if (quote) {
            cur.push_back(ch);
            if (ch == quote)
                quote = 0;
            continue;
        }
        if (ch == '"' || ch == '\'') {
            quote = ch;
            cur.push_back(ch);
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(ch))) {
            flush();
            continue;
        }
        cur.push_back(ch);
    }
    flush();
    return out;
}

namespace {

// ------------------------
// Lexer
// ------------------------
enum class TokKind {
    END,
    IDENT,
    NUMBER,
    STRING,
    OP,
    LPAREN,
    RPAREN
};

struct Tok {
    TokKind kind = TokKind::END;
    std::string text;
};

class Lexer {
public:
    explicit Lexer(std::string_view s)
        : s_(s)
    {
    }

    Tok next()
    {
        skipWs();
        if (i_ >= s_.size())
            return { TokKind::END, "" };

        char c = s_[i_];

        // parens
        if (c == '(') {
            ++i_;
            return { TokKind::LPAREN, "(" };
        }
        if (c == ')') {
            ++i_;
            return { TokKind::RPAREN, ")" };
        }

        // string literal
        if (c == '"' || c == '\'') {
            char q = c;
            size_t j = i_ + 1;
            bool esc = false;
            std::string val;
            for (; j < s_.size(); ++j) {
                char x = s_[j];
                if (esc) {
                    // minimal escapes
                    switch (x) {
                    case 'n':
                        val.push_back('\n');
                        break;
                    case 'r':
                        val.push_back('\r');
                        break;
                    case 't':
                        val.push_back('\t');
                        break;
                    case '\\':
                    case '\'':
                    case '"':
                        val.push_back(x);
                        break;
                    default:
                        val.push_back(x);
                        break;
                    }
                    esc = false;
                    continue;
                }
                if (x == '\\') {
                    esc = true;
                    continue;
                }
                if (x == q) {
                    i_ = j + 1;
                    return { TokKind::STRING, val };
                }
                val.push_back(x);
            }
            // unterminated
            i_ = s_.size();
            return { TokKind::STRING, val };
        }

        // multi-char operators
        auto peek = [&](size_t k) -> char {
            return (i_ + k < s_.size()) ? s_[i_ + k] : '\0';
        };

        // && ||
        if (c == '&' && peek(1) == '&') {
            i_ += 2;
            return { TokKind::OP, "&&" };
        }
        if (c == '|' && peek(1) == '|') {
            i_ += 2;
            return { TokKind::OP, "||" };
        }

        // <= >= == != !~
        if ((c == '<' || c == '>' || c == '!' || c == '=') && peek(1) == '=') {
            std::string op;
            op.push_back(c);
            op.push_back('=');
            i_ += 2;
            return { TokKind::OP, op };
        }
        if (c == '!' && peek(1) == '~') {
            i_ += 2;
            return { TokKind::OP, "!~" };
        }

        // single-char operators
        if (c == '<' || c == '>' || c == '=' || c == '!' || c == '~') {
            ++i_;
            return { TokKind::OP, std::string(1, c) };
        }

        // number (support leading +-/., exponent)
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))
            || ((c == '+' || c == '-') && (std::isdigit(static_cast<unsigned char>(peek(1))) || peek(1) == '.'))) {
            size_t j = i_;
            bool seenDot = false;
            bool seenExp = false;

            if (s_[j] == '+' || s_[j] == '-')
                ++j;
            for (; j < s_.size(); ++j) {
                char x = s_[j];
                if (std::isdigit(static_cast<unsigned char>(x)))
                    continue;
                if (x == '.' && !seenDot && !seenExp) {
                    seenDot = true;
                    continue;
                }
                if ((x == 'e' || x == 'E') && !seenExp) {
                    seenExp = true;
                    // allow exp sign
                    if (j + 1 < s_.size() && (s_[j + 1] == '+' || s_[j + 1] == '-'))
                        ++j;
                    continue;
                }
                break;
            }
            std::string num(s_.substr(i_, j - i_));
            i_ = j;
            return { TokKind::NUMBER, num };
        }

        // identifier / keyword (allow A-Z a-z 0-9 _ $)
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
            size_t j = i_;
            for (; j < s_.size(); ++j) {
                char x = s_[j];
                if (std::isalnum(static_cast<unsigned char>(x)) || x == '_' || x == '$')
                    continue;
                break;
            }
            std::string id(s_.substr(i_, j - i_));
            i_ = j;
            return { TokKind::IDENT, id };
        }

        // unknown char -> treat as operator token for clearer error
        ++i_;
        return { TokKind::OP, std::string(1, c) };
    }

private:
    void skipWs()
    {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_])))
            ++i_;
    }

    std::string_view s_;
    size_t i_ = 0;
};

// ------------------------
// AST & Evaluation
// ------------------------
enum class ValKind { NULLV, NUM, STR, BOOL };

struct Val {
    ValKind kind = ValKind::NULLV;
    double num = 0.0;
    std::string str;
    bool b = false;

    static Val Null() { return {}; }
    static Val Num(double x)
    {
        Val v;
        v.kind = ValKind::NUM;
        v.num = x;
        return v;
    }
    static Val Str(std::string s)
    {
        Val v;
        v.kind = ValKind::STR;
        v.str = std::move(s);
        return v;
    }
    static Val Bool(bool x)
    {
        Val v;
        v.kind = ValKind::BOOL;
        v.b = x;
        return v;
    }
};

static bool isTruthy(const Val& v)
{
    switch (v.kind) {
    case ValKind::BOOL:
        return v.b;
    case ValKind::NUM:
        return v.num != 0.0;
    case ValKind::STR:
        return !v.str.empty();
    default:
        return false;
    }
}

static std::optional<double> toNumber(const Val& v)
{
    if (v.kind == ValKind::NUM)
        return v.num;
    if (v.kind == ValKind::BOOL)
        return v.b ? 1.0 : 0.0;
    if (v.kind == ValKind::STR) {
        try {
            size_t idx = 0;
            double x = std::stod(v.str, &idx);
            if (idx == v.str.size())
                return x;
        } catch (...) {
        }
    }
    return std::nullopt;
}

static std::string toString(const Val& v)
{
    switch (v.kind) {
    case ValKind::STR:
        return v.str;
    case ValKind::NUM: {
        std::ostringstream oss;
        oss << v.num;
        return oss.str();
    }
    case ValKind::BOOL:
        return v.b ? "true" : "false";
    default:
        return "";
    }
}

struct Node {
    virtual ~Node() = default;
    virtual Val eval(const QueryEngine::EvalLookup& lk, int row) const = 0;
    virtual void collectIdents(std::vector<std::string>& out) const = 0;
};

struct LitNum : Node {
    double x;
    explicit LitNum(double v)
        : x(v)
    {
    }
    Val eval(const QueryEngine::EvalLookup&, int) const override { return Val::Num(x); }
    void collectIdents(std::vector<std::string>&) const override { }
};

struct LitStr : Node {
    std::string s;
    explicit LitStr(std::string v)
        : s(std::move(v))
    {
    }
    Val eval(const QueryEngine::EvalLookup&, int) const override { return Val::Str(s); }
    void collectIdents(std::vector<std::string>&) const override { }
};

struct Ident : Node {
    std::string nameUpper;
    explicit Ident(std::string u)
        : nameUpper(std::move(u))
    {
    }
    Val eval(const QueryEngine::EvalLookup& lk, int row) const override
    {
        if (nameUpper == "_IDX") {
            return Val::Num((double)row);
        }
        DbValue dv;
        if (!lk.resolve || !lk.resolve(nameUpper, row, dv))
            return Val::Null();

        return std::visit([](auto&& arg) -> Val {
            using T = std::decay_t<decltype(arg)>;
            if constexpr (std::is_same_v<T, std::string>)
                return Val::Str(arg);
            else
                return Val::Num((double)arg);
        },
            dv);
    }
    void collectIdents(std::vector<std::string>& out) const override { out.push_back(nameUpper); }
};

struct Unary : Node {
    std::string opUpper; // "NOT" or "!"
    std::unique_ptr<Node> rhs;
    Unary(std::string op, std::unique_ptr<Node> r)
        : opUpper(std::move(op))
        , rhs(std::move(r))
    {
    }
    Val eval(const QueryEngine::EvalLookup& lk, int row) const override
    {
        Val v = rhs->eval(lk, row);
        if (opUpper == "NOT" || opUpper == "!") {
            return Val::Bool(!isTruthy(v));
        }
        return Val::Null();
    }
    void collectIdents(std::vector<std::string>& out) const override { rhs->collectIdents(out); }
};

struct Binary : Node {
    std::string opUpper; // AND OR etc, comparisons
    std::unique_ptr<Node> a;
    std::unique_ptr<Node> b;
    Binary(std::string op, std::unique_ptr<Node> x, std::unique_ptr<Node> y)
        : opUpper(std::move(op))
        , a(std::move(x))
        , b(std::move(y))
    {
    }

    Val eval(const QueryEngine::EvalLookup& lk, int row) const override
    {
        const std::string& op = opUpper;

        if (op == "AND" || op == "&&") {
            Val va = a->eval(lk, row);
            if (!isTruthy(va))
                return Val::Bool(false);
            Val vb = b->eval(lk, row);
            return Val::Bool(isTruthy(vb));
        }
        if (op == "OR" || op == "||") {
            Val va = a->eval(lk, row);
            if (isTruthy(va))
                return Val::Bool(true);
            Val vb = b->eval(lk, row);
            return Val::Bool(isTruthy(vb));
        }

        // comparisons
        Val va = a->eval(lk, row);
        Val vb = b->eval(lk, row);

        if (op == "~" || op == "!~") {
            const std::string left = toString(va);
            const std::string pat = toString(vb);
            bool ok = false;
            if (QueryEngine::hasGlob(pat))
                ok = QueryEngine::globMatchCI(pat, left);
            else
                ok = QueryEngine::containsCI(left, pat);
            if (op == "!~")
                ok = !ok;
            return Val::Bool(ok);
        }

        // numeric first if both convertible
        auto na = toNumber(va);
        auto nb = toNumber(vb);
        if (na && nb) {
            const double x = *na;
            const double y = *nb;
            bool ok = false;
            if (op == "=" || op == "==")
                ok = (x == y);
            else if (op == "!=")
                ok = (x != y);
            else if (op == "<")
                ok = (x < y);
            else if (op == "<=")
                ok = (x <= y);
            else if (op == ">")
                ok = (x > y);
            else if (op == ">=")
                ok = (x >= y);
            else
                ok = false;
            return Val::Bool(ok);
        }

        // fallback string compare
        const std::string xs = toString(va);
        const std::string ys = toString(vb);
        bool ok = false;
        if (op == "=" || op == "==")
            ok = (xs == ys);
        else if (op == "!=")
            ok = (xs != ys);
        else if (op == "<")
            ok = (xs < ys);
        else if (op == "<=")
            ok = (xs <= ys);
        else if (op == ">")
            ok = (xs > ys);
        else if (op == ">=")
            ok = (xs >= ys);
        else
            ok = false;
        return Val::Bool(ok);
    }

    void collectIdents(std::vector<std::string>& out) const override
    {
        a->collectIdents(out);
        b->collectIdents(out);
    }
};

// ------------------------
// Parser
// ------------------------
class Parser {
public:
    Parser(std::string_view expr, std::string& err)
        : lex_(expr)
        , err_(err)
    {
        cur_ = lex_.next();
    }

    std::unique_ptr<Node> parseExpr()
    {
        auto n = parseOr();
        if (!n)
            return nullptr;
        if (cur_.kind != TokKind::END) {
            err_ = "Unexpected token: '" + cur_.text + "'";
            return nullptr;
        }
        return n;
    }

private:
    std::unique_ptr<Node> parseOr()
    {
        auto n = parseAnd();
        if (!n)
            return nullptr;
        for (;;) {
            if (isKw("OR") || isOp("||")) {
                std::string op = cur_.text;
                consume();
                auto r = parseAnd();
                if (!r)
                    return nullptr;
                n = std::make_unique<Binary>(tu::upper(op), std::move(n), std::move(r));
                continue;
            }
            return n;
        }
    }

    std::unique_ptr<Node> parseAnd()
    {
        auto n = parseNot();
        if (!n)
            return nullptr;
        for (;;) {
            if (isKw("AND") || isOp("&&")) {
                std::string op = cur_.text;
                consume();
                auto r = parseNot();
                if (!r)
                    return nullptr;
                n = std::make_unique<Binary>(tu::upper(op), std::move(n), std::move(r));
                continue;
            }
            return n;
        }
    }

    std::unique_ptr<Node> parseNot()
    {
        if (isKw("NOT") || isOp("!")) {
            std::string op = cur_.text;
            consume();
            auto r = parseNot();
            if (!r)
                return nullptr;
            return std::make_unique<Unary>(tu::upper(op), std::move(r));
        }
        return parseCmp();
    }

    std::unique_ptr<Node> parseCmp()
    {
        auto lhs = parsePrimary();
        if (!lhs)
            return nullptr;

        if (cur_.kind == TokKind::OP) {
            std::string op = cur_.text;
            std::string opUp = tu::upper(op);
            // recognized comparison ops
            if (op == "=" || op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">="
                || op == "~" || op == "!~") {
                consume();
                auto rhs = parsePrimary();
                if (!rhs) {
                    err_ = "Missing RHS for operator '" + op + "'";
                    return nullptr;
                }
                return std::make_unique<Binary>(opUp, std::move(lhs), std::move(rhs));
            }
        }
        // if no comparison, interpret primary as boolean (non-zero / non-empty)
        return lhs;
    }

    std::unique_ptr<Node> parsePrimary()
    {
        if (cur_.kind == TokKind::LPAREN) {
            consume();
            auto n = parseOr();
            if (!n)
                return nullptr;
            if (cur_.kind != TokKind::RPAREN) {
                err_ = "Missing ')'";
                return nullptr;
            }
            consume();
            return n;
        }

        if (cur_.kind == TokKind::NUMBER) {
            double x = 0.0;
            try {
                x = std::stod(cur_.text);
            } catch (...) {
                err_ = "Bad number: '" + cur_.text + "'";
                return nullptr;
            }
            consume();
            return std::make_unique<LitNum>(x);
        }

        if (cur_.kind == TokKind::STRING) {
            std::string s = cur_.text;
            consume();
            return std::make_unique<LitStr>(std::move(s));
        }

        if (cur_.kind == TokKind::IDENT) {
            std::string id = tu::upper(cur_.text);
            consume();
            return std::make_unique<Ident>(std::move(id));
        }

        err_ = "Unexpected token: '" + cur_.text + "'";
        return nullptr;
    }

    bool isKw(const char* kw) const
    {
        return cur_.kind == TokKind::IDENT && tu::upper(cur_.text) == kw;
    }

    bool isOp(const char* op) const
    {
        return cur_.kind == TokKind::OP && cur_.text == op;
    }

    void consume()
    {
        cur_ = lex_.next();
    }

    Lexer lex_;
    Tok cur_;
    std::string& err_;
};

static void uniqKeepOrder(std::vector<std::string>& xs)
{
    std::unordered_set<std::string> seen;
    std::vector<std::string> out;
    out.reserve(xs.size());
    for (auto& s : xs) {
        if (seen.insert(s).second)
            out.push_back(s);
    }
    xs.swap(out);
}

} // namespace

bool QueryEngine::compileBoolExpr(const std::string& expr,
    const EvalLookup& lookup,
    CompileResult& out,
    std::string& errMsg)
{
    errMsg.clear();
    out = CompileResult {};

    std::string err;
    Parser ps(expr, err);
    auto root = ps.parseExpr();
    if (!root) {
        errMsg = err.empty() ? "Parse error" : err;
        return false;
    }

    std::vector<std::string> ids;
    root->collectIdents(ids);
    uniqKeepOrder(ids);

    // Build eval lambda
    auto spRoot = std::shared_ptr<const Node>(root.release());
    out.eval = [spRoot, lookup](int row) -> bool {
        Val v = spRoot->eval(lookup, row);
        return isTruthy(v);
    };
    out.identifiers_upper = std::move(ids);
    return true;
}

} // namespace mmdb::dbio
